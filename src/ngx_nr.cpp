#include "ngx_nr.hpp"

#include "ext_unhook.hpp"
#include "log.hpp"
#include "ngx_backend.hpp"
#include "ngx_snippet.hpp"

#include <d3d12.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

#if !defined(STRAY_DLSS_ENABLE_NGX)
#define STRAY_DLSS_ENABLE_NGX 1
#endif

namespace stray_dlss::nr {

const char *const kNrRefusalNames[kNrRefusalCount] = {
	"dll-missing", "create-failed", "evaluate-failed", "degenerate-output",
	"bad-inputs", "alloc-failed", "validating",
};

} // namespace stray_dlss::nr

#if !STRAY_DLSS_ENABLE_NGX

// NGX-less diagnostic build: the whole surface exists and does nothing.
namespace stray_dlss::nr {
namespace { char g_err[64] = "<ngx disabled>"; }
void set_enabled(bool) {}
bool enabled() { return false; }
void set_dll_path(const char *) {}
void set_topology(Topology) {}
void set_tuning(float, float, float) {}
void set_mvec_scale_override(float) {}
bool load_runtime(ID3D12Device *) { return false; }
bool apply(ID3D12Device *, ID3D12GraphicsCommandList *, const ApplyInputs &) { return false; }
void on_present() {}
void shutdown() {}
const char *last_error() { return g_err; }
void counters(std::uint64_t &applied, std::uint64_t &refused, std::uint32_t out[kNrRefusalCount])
{
	applied = 0;
	refused = 0;
	for (int i = 0; i < kNrRefusalCount; ++i)
		out[i] = 0;
}
bool validated() { return false; }
} // namespace stray_dlss::nr

#else

#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers.h>

namespace stray_dlss::nr {
namespace {

// --- the DLSSNR.* parameter namespace (docs/RESEARCH-RENODX-DLSS5.md §2.2, HARD) ---
// The public SDK has no DLSSNR helper, so every key is set by name. Names are verbatim from
// the study's exhaustive string dump of both addon builds.
constexpr const char *kColor        = "DLSSNR.Color";
constexpr const char *kDepth        = "DLSSNR.Depth";
constexpr const char *kMVec         = "DLSSNR.MVec";
constexpr const char *kOutput       = "DLSSNR.Output";
constexpr const char *kColorBaseX   = "DLSSNR.ColorSubrectBaseX";
constexpr const char *kColorBaseY   = "DLSSNR.ColorSubrectBaseY";
constexpr const char *kColorW       = "DLSSNR.ColorSubrectWidth";
constexpr const char *kColorH       = "DLSSNR.ColorSubrectHeight";
constexpr const char *kDepthBaseX   = "DLSSNR.DepthSubrectBaseX";
constexpr const char *kDepthBaseY   = "DLSSNR.DepthSubrectBaseY";
constexpr const char *kDepthW       = "DLSSNR.DepthSubrectWidth";
constexpr const char *kDepthH       = "DLSSNR.DepthSubrectHeight";
constexpr const char *kMVecBaseX    = "DLSSNR.MVecSubrectBaseX";
constexpr const char *kMVecBaseY    = "DLSSNR.MVecSubrectBaseY";
constexpr const char *kMVecW        = "DLSSNR.MVecSubrectWidth";
constexpr const char *kMVecH        = "DLSSNR.MVecSubrectHeight";
constexpr const char *kOutBaseX     = "DLSSNR.OutputSubrectBaseX";
constexpr const char *kOutBaseY     = "DLSSNR.OutputSubrectBaseY";
constexpr const char *kOutW         = "DLSSNR.OutputSubrectWidth";
constexpr const char *kOutH         = "DLSSNR.OutputSubrectHeight";
constexpr const char *kMVecScaleX   = "DLSSNR.MVecScaleX";
constexpr const char *kMVecScaleY   = "DLSSNR.MVecScaleY";
constexpr const char *kDepthInverted= "DLSSNR.DepthInverted";
constexpr const char *kReset        = "DLSSNR.Reset";
constexpr const char *kIntensity    = "DLSSNR.Intensity";
constexpr const char *kLocalTone    = "DLSSNR.LocalToneStrength";
constexpr const char *kLocalStruct  = "DLSSNR.LocalStructureStrength";
constexpr const char *kWidth        = "DLSSNR.Width";
constexpr const char *kHeight       = "DLSSNR.Height";
constexpr const char *kScale        = "DLSSNR.Scale";

// Refusal indices, parallel to kNrRefusalNames.
enum
{
	kRefDllMissing = 0,
	kRefCreateFailed,
	kRefEvaluateFailed,
	kRefDegenerate,
	kRefBadInputs,
	kRefAllocFailed,
	kRefValidating,
};

// Validation crop: a centred region of the neural output, read back once. Small enough that
// the copy and the map are trivial, large enough that "every texel is black" is a real verdict
// rather than one unlucky dark pixel.
constexpr UINT kValidateSize = 128;
// Presents to wait before mapping the readback — the same latency the input_dump path uses,
// which is proven to work under vkd3d.
constexpr int kValidateLatency = 5;
// Luminance floor. The addon's own shader uses `neural_y <= 1e-5` (§4.1); we accept any texel
// above it anywhere in the crop.
constexpr double kLumaFloor = 1e-5;
// Application id for the snippet's own Init_Ext. NGX's ProjectID form is not exported by the
// snippet, so the numeric-id form is used; the value is ours and needs no NVIDIA registration
// (same reasoning as the SR path's self-generated project GUID).
constexpr unsigned long long kNrApplicationId = 0x5354524159444C53ull; // "STRAYDLS"

// True once the snippet's OWN exports are resolved AND its Init_Ext succeeded. Everything
// feature-18 then goes through the snippet rather than the NGX core, because the core has no
// knowledge of a pre-release snippet and answers FAIL_OutOfDate (measured: 0xbad0000c).
bool g_use_direct = false;
// Which runtime allocated g_params. An NVSDK_NGX_Parameter is just an interface object, so the
// snippet happily consumes one the CORE allocated — but whatever allocated it must also destroy
// it, so this flag pairs the two and we never cross-call.
bool g_params_from_snippet = false;
bool g_enabled = false;
Topology g_topology = Topology::post_process;
float g_intensity = 1.0f;
float g_local_tone = 1.0f;
float g_local_structure = 1.0f;
float g_mvec_scale_override = 0.0f;
char g_dll_path[512] = "";
char g_last_error[256] = "";

// The snippet module is owned by src/ngx_snippet.cpp; this only records that the load was
// attempted and succeeded, so apply() can tell "never loaded" from "loaded but not initialised".
bool g_runtime_loaded = false;
bool g_runtime_tried = false;

NVSDK_NGX_Handle *g_feature = nullptr;
NVSDK_NGX_Parameter *g_params = nullptr;
std::uint32_t g_feature_render_w = 0, g_feature_render_h = 0;
std::uint32_t g_feature_out_w = 0, g_feature_out_h = 0;
bool g_create_latched = false;

// Our own neural-output texture, created to MATCH the engine output's desc so the copy-back is
// a plain CopyResource (identical format and extent — D3D12 requires it).
ID3D12Resource *g_nr_output = nullptr;
DXGI_FORMAT g_nr_format = DXGI_FORMAT_UNKNOWN;
std::uint32_t g_nr_width = 0, g_nr_height = 0;

// Validation state machine: run NR, read back a crop, and only then start copying back.
enum class Validation { pending, in_flight, ok, failed };
// ATOMIC on purpose: begin_validation() writes the readback fields on the RECORDING thread and
// publishes them by storing `in_flight` last (release); on_present() consumes them on the
// PRESENT thread after loading `in_flight` (acquire). A plain enum here would be a real data
// race — the fields would not be guaranteed visible with the state that advertises them.
std::atomic<Validation> g_validation{ Validation::pending };
ID3D12Resource *g_validate_readback = nullptr;
D3D12_PLACED_SUBRESOURCE_FOOTPRINT g_validate_footprint = {};
std::uint64_t g_validate_bytes = 0;
int g_validate_presents_left = 0;

std::atomic<std::uint64_t> g_applied{ 0 };
std::atomic<std::uint64_t> g_refused{ 0 };
std::atomic<std::uint32_t> g_refusals[kNrRefusalCount] = {};
bool g_refusal_logged[kNrRefusalCount] = {};

// Keep NR's inputs and output alive past GPU execution, exactly as the SR/RR paths do: NGX
// holds no references and EvaluateFeature only RECORDS work. (CLAUDE.md §5)
constexpr std::size_t kKeepAliveFrames = 6;
struct KeepAlive
{
	ID3D12Resource *resources[4] = {};
	std::uint64_t frame = 0;
};
KeepAlive g_keep_alive[kKeepAliveFrames * 2];
std::size_t g_keep_alive_count = 0;
std::uint64_t g_eval_frame = 0;

template <typename T>
void release(T *&p)
{
	if (p != nullptr)
	{
		p->Release();
		p = nullptr;
	}
}

bool refuse(int reason, const char *fmt_msg)
{
	g_refused.fetch_add(1, std::memory_order_relaxed);
	if (reason >= 0 && reason < kNrRefusalCount)
	{
		g_refusals[reason].fetch_add(1, std::memory_order_relaxed);
		if (!g_refusal_logged[reason])
		{
			g_refusal_logged[reason] = true;
			STRAY_LOG_WARN("NR refused (%s): %s The SR/RR image is kept unchanged. First "
				"occurrence only; the periodic report carries the rate.",
				kNrRefusalNames[reason], fmt_msg);
		}
	}
	return false;
}

void set_error(const char *what, NVSDK_NGX_Result result)
{
	std::snprintf(g_last_error, sizeof(g_last_error), "%s: 0x%08x (%s)", what,
		static_cast<unsigned int>(result), ngx::result_name(static_cast<unsigned int>(result)));
	STRAY_LOG_ERROR("NR %s", g_last_error);
}

void retire_keep_alive(bool all)
{
	std::size_t kept = 0;
	for (std::size_t i = 0; i < g_keep_alive_count; ++i)
	{
		KeepAlive &ka = g_keep_alive[i];
		if (all || ka.frame + kKeepAliveFrames <= g_eval_frame)
		{
			for (ID3D12Resource *r : ka.resources)
				if (r != nullptr)
					r->Release();
			continue;
		}
		g_keep_alive[kept++] = ka;
	}
	g_keep_alive_count = kept;
}

// --- route every feature-18 NGX call to the snippet when the direct path is live ---
NVSDK_NGX_Result nr_alloc_params(NVSDK_NGX_Parameter **out)
{
	// Prefer the snippet's own allocator when it has one; otherwise use the CORE's, which we
	// already allocate from successfully for SR/RR. Measured: this snippet exports none of the
	// parameter helpers, and that is fine — only the block's interface matters to it.
	const bool from_snippet = g_use_direct && snippet::has_allocate_parameters();
	const NVSDK_NGX_Result r = from_snippet
		? static_cast<NVSDK_NGX_Result>(
			snippet::allocate_parameters(reinterpret_cast<void **>(out)))
		: NVSDK_NGX_D3D12_AllocateParameters(out);
	if (NVSDK_NGX_SUCCEED(r))
		g_params_from_snippet = from_snippet;
	static bool s_logged = false;
	if (!s_logged)
	{
		s_logged = true;
		STRAY_LOG_INFO("NR: parameter block allocated by the %s (0x%08x). The snippet consumes "
			"a block whichever runtime made it; only the destroy must match.",
			from_snippet ? "SNIPPET's own allocator" : "NGX CORE",
			static_cast<unsigned int>(r));
	}
	return r;
}

NVSDK_NGX_Result nr_destroy_params(NVSDK_NGX_Parameter *p)
{
	// Destroy with whichever API allocated it — never cross-call.
	if (g_params_from_snippet)
		return static_cast<NVSDK_NGX_Result>(snippet::destroy_parameters(p));
	return NVSDK_NGX_D3D12_DestroyParameters(p);
}

NVSDK_NGX_Result nr_create_feature(ID3D12GraphicsCommandList *cmd, NVSDK_NGX_Parameter *p,
                                   NVSDK_NGX_Handle **out)
{
	if (g_use_direct)
		return static_cast<NVSDK_NGX_Result>(snippet::create_feature(cmd,
			static_cast<unsigned int>(NVSDK_NGX_Feature_Reserved18), p,
			reinterpret_cast<void **>(out)));
	return NVSDK_NGX_D3D12_CreateFeature(cmd, NVSDK_NGX_Feature_Reserved18, p, out);
}

NVSDK_NGX_Result nr_evaluate_feature(ID3D12GraphicsCommandList *cmd, NVSDK_NGX_Handle *h,
                                     NVSDK_NGX_Parameter *p)
{
	if (g_use_direct)
		return static_cast<NVSDK_NGX_Result>(snippet::evaluate_feature(cmd, h, p));
	return NVSDK_NGX_D3D12_EvaluateFeature(cmd, h, p, nullptr);
}

NVSDK_NGX_Result nr_release_feature(NVSDK_NGX_Handle *h)
{
	if (g_use_direct)
		return static_cast<NVSDK_NGX_Result>(snippet::release_feature(h));
	return NVSDK_NGX_D3D12_ReleaseFeature(h);
}

void release_feature()
{
	retire_keep_alive(/*all=*/true);
	if (g_feature != nullptr)
	{
		nr_release_feature(g_feature);
		g_feature = nullptr;
	}
	if (g_params != nullptr)
	{
		nr_destroy_params(g_params);
		g_params = nullptr;
	}
	g_feature_render_w = g_feature_render_h = g_feature_out_w = g_feature_out_h = 0;
}

// Creates (or recreates on a size change) feature 18. Availability is deliberately NOT gated on
// any capability key: the reserved slot has none, so a create attempt is the only real answer —
// once, then the latch, so a failing create never re-runs per frame.
bool ensure_feature(ID3D12GraphicsCommandList *cmd, std::uint32_t render_w,
                    std::uint32_t render_h, std::uint32_t out_w, std::uint32_t out_h)
{
	ext_unhook::repair();

	const bool same = g_feature != nullptr && g_feature_render_w == render_w &&
		g_feature_render_h == render_h && g_feature_out_w == out_w &&
		g_feature_out_h == out_h;
	if (same)
		return true;
	if (g_create_latched)
		return false;

	release_feature();

	NVSDK_NGX_Result result = nr_alloc_params(&g_params);
	if (NVSDK_NGX_FAILED(result) || g_params == nullptr)
	{
		set_error("AllocateParameters", result);
		g_create_latched = true;
		return false;
	}

	// Under post_process the network works at OUTPUT resolution (Color is already upscaled),
	// so Width/Height are the output rect and Scale is 1. Under sr_shaped it upscales, so
	// Width/Height are the render rect.
	const bool post = g_topology == Topology::post_process;
	const std::uint32_t in_w = post ? out_w : render_w;
	const std::uint32_t in_h = post ? out_h : render_h;

	g_params->Set(kWidth, in_w);
	g_params->Set(kHeight, in_h);
	g_params->Set(kScale, out_w > 0 && in_w > 0
		? static_cast<float>(out_w) / static_cast<float>(in_w) : 1.0f);
	// Reversed-Z: UE 4.27 throughout, same flag SR carries. (CLAUDE.md §2.4)
	g_params->Set(kDepthInverted, 1u);
	g_params->Set(kIntensity, g_intensity);
	g_params->Set(kLocalTone, g_local_tone);
	g_params->Set(kLocalStruct, g_local_structure);

	STRAY_LOG_WARN("NR: creating NGX feature 18 (Reserved18 / DLSSNR) %ux%u -> %ux%u, "
		"topology=%s, intensity=%.2f localTone=%.2f localStructure=%.2f...",
		in_w, in_h, out_w, out_h, post ? "post-process" : "sr-shaped",
		g_intensity, g_local_tone, g_local_structure);

	result = nr_create_feature(cmd, g_params, &g_feature);
	if (NVSDK_NGX_FAILED(result) || g_feature == nullptr)
	{
		set_error("CreateFeature(18)", result);
		STRAY_LOG_ERROR("  NR feature-18 create failed and is LATCHED off for this session. "
			"FAIL_FeatureNotFound/NotSupported here means the loaded nvngx_dlssnr.dll does not "
			"provide the reserved slot (wrong build, or the Ada patch this 4090 needs is "
			"missing); the SR/RR image is unaffected.");
		release_feature();
		g_create_latched = true;
		return false;
	}

	g_feature_render_w = render_w;
	g_feature_render_h = render_h;
	g_feature_out_w = out_w;
	g_feature_out_h = out_h;
	g_last_error[0] = 0;
	STRAY_LOG_WARN("NR: NGX feature 18 CREATED via the %s.%s",
		g_use_direct ? "SNIPPET's own exports (direct path)" : "NGX core",
		g_use_direct ? "" : " The core does not know this pre-release snippet, so this is "
			"unexpected — if it works, note it.");
	STRAY_LOG_WARN("NR: feature 18 live. DLSS Neural Rendering is live; the result is "
		"validated against a black/degenerate output before it is allowed on screen.");
	return true;
}

// Our neural-output texture must match the engine output exactly so the copy-back is legal.
bool ensure_output_texture(ID3D12Device *device, ID3D12Resource *image)
{
	const D3D12_RESOURCE_DESC src = image->GetDesc();
	const auto w = static_cast<std::uint32_t>(src.Width);
	const auto h = src.Height;
	if (g_nr_output != nullptr && g_nr_format == src.Format && g_nr_width == w &&
		g_nr_height == h)
		return true;

	release(g_nr_output);
	release(g_validate_readback);
	g_validation.store(Validation::pending, std::memory_order_release);

	D3D12_HEAP_PROPERTIES heap = {};
	heap.Type = D3D12_HEAP_TYPE_DEFAULT;

	D3D12_RESOURCE_DESC tex = src;
	tex.Alignment = 0;
	// NGX writes its output as a UAV; missing the flag can yield a black result with no error
	// at all. (docs/RESEARCH.md §3.5)
	tex.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

	const HRESULT hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &tex,
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&g_nr_output));
	if (FAILED(hr))
	{
		std::snprintf(g_last_error, sizeof(g_last_error),
			"CreateCommittedResource(nr_output) hr=0x%08x", static_cast<unsigned int>(hr));
		return false;
	}
	g_nr_format = src.Format;
	g_nr_width = w;
	g_nr_height = h;
	STRAY_LOG_INFO("NR: neural output texture %ux%u fmt=%d created (matches the engine output, "
		"so the copy-back is a plain CopyResource).", w, h, static_cast<int>(src.Format));
	return true;
}

// Records a copy of a centred crop of the neural output into a readback buffer. Mapped a few
// presents later by on_present(); until then NR does not touch the screen.
void begin_validation(ID3D12Device *device, ID3D12GraphicsCommandList *cmd)
{
	if (g_validation.load(std::memory_order_acquire) != Validation::pending ||
		g_nr_output == nullptr)
		return;

	const UINT crop_w = g_nr_width < kValidateSize ? g_nr_width : kValidateSize;
	const UINT crop_h = g_nr_height < kValidateSize ? g_nr_height : kValidateSize;
	if (crop_w == 0 || crop_h == 0)
		return;

	D3D12_RESOURCE_DESC crop = {};
	crop.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	crop.Width = crop_w;
	crop.Height = crop_h;
	crop.DepthOrArraySize = 1;
	crop.MipLevels = 1;
	crop.Format = g_nr_format;
	crop.SampleDesc.Count = 1;
	UINT64 total = 0;
	D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
	device->GetCopyableFootprints(&crop, 0, 1, 0, &footprint, nullptr, nullptr, &total);
	if (total == 0)
		return;

	D3D12_HEAP_PROPERTIES heap = {};
	heap.Type = D3D12_HEAP_TYPE_READBACK;
	D3D12_RESOURCE_DESC buf = {};
	buf.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	buf.Width = total;
	buf.Height = 1;
	buf.DepthOrArraySize = 1;
	buf.MipLevels = 1;
	buf.SampleDesc.Count = 1;
	buf.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buf,
			D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g_validate_readback))))
		return;

	D3D12_RESOURCE_BARRIER barrier = {};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Transition.pResource = g_nr_output;
	barrier.Transition.Subresource = 0;
	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
	cmd->ResourceBarrier(1, &barrier);

	// The centre of the frame: the most likely place for real content.
	D3D12_BOX box = {};
	box.left = (g_nr_width - crop_w) / 2;
	box.top = (g_nr_height - crop_h) / 2;
	box.front = 0;
	box.right = box.left + crop_w;
	box.bottom = box.top + crop_h;
	box.back = 1;

	D3D12_TEXTURE_COPY_LOCATION src_loc = {};
	src_loc.pResource = g_nr_output;
	src_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	src_loc.SubresourceIndex = 0;
	D3D12_TEXTURE_COPY_LOCATION dst_loc = {};
	dst_loc.pResource = g_validate_readback;
	dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	dst_loc.PlacedFootprint = footprint;
	cmd->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, &box);

	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
	cmd->ResourceBarrier(1, &barrier);

	g_validate_footprint = footprint;
	g_validate_bytes = total;
	g_validate_presents_left = kValidateLatency;
	// Publish LAST: everything above must be visible to the present thread that sees this.
	g_validation.store(Validation::in_flight, std::memory_order_release);
	STRAY_LOG_INFO("NR: validating the neural output (%ux%u centre crop). Until it passes, NR "
		"runs but does NOT touch the screen — a degenerate runtime cannot show a black frame.",
		crop_w, crop_h);
}

// Decodes one texel's luminance for the formats the engine output can be. Anything we cannot
// decode is treated as "cannot validate", which fails safe (NR stays off).
bool decode_luma(const unsigned char *p, DXGI_FORMAT fmt, double &luma, bool &known)
{
	known = true;
	switch (fmt)
	{
	case DXGI_FORMAT_R16G16B16A16_FLOAT:
	{
		// half -> float, three channels.
		double c[3];
		for (int i = 0; i < 3; ++i)
		{
			std::uint16_t h = 0;
			std::memcpy(&h, p + i * 2, sizeof(h));
			const int exp = (h >> 10) & 0x1F;
			const int man = h & 0x3FF;
			const double sign = (h & 0x8000) ? -1.0 : 1.0;
			double v;
			if (exp == 0)
				v = man * 5.9604644775390625e-8;
			else if (exp == 31)
				v = 65504.0;
			else
				v = (1.0 + man / 1024.0) * std::pow(2.0, exp - 15);
			c[i] = sign * v;
		}
		luma = 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2];
		return true;
	}
	case DXGI_FORMAT_R11G11B10_FLOAT:
	{
		std::uint32_t v = 0;
		std::memcpy(&v, p, sizeof(v));
		const auto ufloat = [](std::uint32_t bits, int mant_bits, int exp_bits) {
			const int exp = static_cast<int>(bits >> mant_bits) & ((1 << exp_bits) - 1);
			const int man = static_cast<int>(bits) & ((1 << mant_bits) - 1);
			if (exp == 0)
				return man / static_cast<double>(1 << mant_bits) * std::pow(2.0, -14);
			return (1.0 + man / static_cast<double>(1 << mant_bits)) * std::pow(2.0, exp - 15);
		};
		const double r = ufloat(v & 0x7FF, 6, 5);
		const double g = ufloat((v >> 11) & 0x7FF, 6, 5);
		const double b = ufloat((v >> 22) & 0x3FF, 5, 5);
		luma = 0.2126 * r + 0.7152 * g + 0.0722 * b;
		return true;
	}
	case DXGI_FORMAT_R10G10B10A2_UNORM:
	{
		std::uint32_t v = 0;
		std::memcpy(&v, p, sizeof(v));
		const double r = (v & 0x3FF) / 1023.0;
		const double g = ((v >> 10) & 0x3FF) / 1023.0;
		const double b = ((v >> 20) & 0x3FF) / 1023.0;
		luma = 0.2126 * r + 0.7152 * g + 0.0722 * b;
		return true;
	}
	case DXGI_FORMAT_R8G8B8A8_UNORM:
	case DXGI_FORMAT_B8G8R8A8_UNORM:
		luma = (0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2]) / 255.0;
		return true;
	default:
		known = false;
		return false;
	}
}

std::uint32_t format_bytes(DXGI_FORMAT fmt)
{
	switch (fmt)
	{
	case DXGI_FORMAT_R16G16B16A16_FLOAT: return 8;
	case DXGI_FORMAT_R11G11B10_FLOAT:
	case DXGI_FORMAT_R10G10B10A2_UNORM:
	case DXGI_FORMAT_R8G8B8A8_UNORM:
	case DXGI_FORMAT_B8G8R8A8_UNORM:     return 4;
	default:                             return 0;
	}
}

} // namespace

void set_enabled(bool value)
{
	g_enabled = value;
	if (value)
		STRAY_LOG_WARN("NR ENABLED ([STRAYDLSS] NgxNR=1): DLSS Neural Rendering (NGX feature "
			"18 / DLSSNR) will run after the SR/RR evaluate. EXPERIMENTAL — the runtime is a "
			"leaked pre-release DLL; a degenerate result is caught by validation and NR simply "
			"stays off.");
}

bool enabled() { return g_enabled; }
void set_topology(Topology topology) { g_topology = topology; }
void set_mvec_scale_override(float scale) { g_mvec_scale_override = scale; }
const char *last_error() { return g_last_error; }
bool validated() { return g_validation.load(std::memory_order_acquire) == Validation::ok; }

void set_dll_path(const char *utf8_path)
{
	if (utf8_path == nullptr)
		g_dll_path[0] = 0;
	else
		std::snprintf(g_dll_path, sizeof(g_dll_path), "%s", utf8_path);
}

void set_tuning(float intensity, float local_tone_strength, float local_structure_strength)
{
	g_intensity = intensity;
	g_local_tone = local_tone_strength;
	g_local_structure = local_structure_strength;
}

void counters(std::uint64_t &applied, std::uint64_t &refused, std::uint32_t out[kNrRefusalCount])
{
	applied = g_applied.load(std::memory_order_relaxed);
	refused = g_refused.load(std::memory_order_relaxed);
	for (int i = 0; i < kNrRefusalCount; ++i)
		out[i] = g_refusals[i].load(std::memory_order_relaxed);
}

bool load_runtime(ID3D12Device *device)
{
	if (!g_enabled || g_runtime_tried)
		return g_runtime_loaded;
	g_runtime_tried = true;

	// Load the snippet and resolve ITS OWN NGX exports (src/ngx_snippet.hpp). Also patches the
	// snippet's GetModuleFileNameW import so its identity queries are observable.
	const bool direct = snippet::load(g_dll_path);
	g_runtime_loaded = direct;
	if (!direct)
	{
		std::snprintf(g_last_error, sizeof(g_last_error), "%s", snippet::last_error());
		STRAY_LOG_ERROR("NR: the snippet's direct NGX path is unavailable (%s). NR stays OFF; "
			"SR/RR are unaffected.", g_last_error);
		return false;
	}

	// Initialise the snippet through its OWN Init_Ext, not the NGX core. This is the whole
	// point: the core resolves only driver-shipped snippets, so asking it for feature 18
	// returns FAIL_OutOfDate (measured 0xbad0000c) however the DLL is staged. RenoDX drives
	// this same runtime the same way ("direct Init_Ext failed with 0x..." is its own error
	// string for this call).
	const auto init = static_cast<NVSDK_NGX_Result>(snippet::init_ext(
		kNrApplicationId, L".", device, static_cast<unsigned int>(NVSDK_NGX_Version_API),
		nullptr));
	if (NVSDK_NGX_FAILED(init))
	{
		set_error("snippet Init_Ext", init);
		STRAY_LOG_ERROR("  The snippet loaded and exported the API but refused to initialise. "
			"If it is an identity check, the GetModuleFileNameW trace above/below shows what "
			"it asked for — try [STRAYDLSS] NgxNRIdentity=snippet|nvngx|exe. NR falls back to "
			"the NGX core path, which is expected to answer FAIL_OutOfDate.");
		snippet::log_identity_calls();
		g_use_direct = false;
		// Stay "loaded": the core path is still worth attempting, and its failure is itself
		// the evidence that the direct path is the only viable one.
		return true;
	}

	g_use_direct = true;
	STRAY_LOG_WARN("NR: snippet Init_Ext SUCCEEDED — feature 18 will be created and evaluated "
		"through the SNIPPET's own exports, bypassing the NGX core entirely.");
	snippet::log_identity_calls();
	return true;
}

bool apply(ID3D12Device *device, ID3D12GraphicsCommandList *cmd, const ApplyInputs &in)
{
	if (!g_enabled)
		return false;
	if (!g_runtime_loaded)
		return refuse(kRefDllMissing, "nvngx_dlssnr.dll was never loaded.");
	if (device == nullptr || cmd == nullptr || in.image == nullptr || in.depth == nullptr ||
		in.motion_vectors == nullptr || in.render_width == 0 || in.output_width == 0)
		return refuse(kRefBadInputs, "a required resource or dimension was missing.");

	const bool post = g_topology == Topology::post_process;
	ID3D12Resource *colour = post ? in.image : in.render_color;
	if (colour == nullptr)
		return refuse(kRefBadInputs, "no colour input for the selected topology.");

	// The result goes to OUR texture, never straight over the engine's output.
	if (!ensure_output_texture(device, in.image))
		return refuse(kRefAllocFailed, g_last_error);
	if (!ensure_feature(cmd, in.render_width, in.render_height, in.output_width,
			in.output_height))
		return refuse(kRefCreateFailed, "feature 18 could not be created.");

	ext_unhook::repair();

	// Colour rect: the output rect under post-process (already upscaled), the render rect
	// under sr-shaped.
	const std::uint32_t cw = post ? in.output_width : in.render_width;
	const std::uint32_t ch = post ? in.output_height : in.render_height;

	// Motion vectors are ours: dense RG16_FLOAT in RENDER-resolution pixels. Under post-process
	// the colour they must move is the OUTPUT rect, so they need the output/render ratio;
	// under sr-shaped the colour is already render-res, so the scale is 1. Neither reading is
	// documented for this leaked runtime — hence the override knob and this log line.
	float scale_x = 1.0f, scale_y = 1.0f;
	if (post && in.render_width > 0 && in.render_height > 0)
	{
		scale_x = static_cast<float>(in.output_width) / static_cast<float>(in.render_width);
		scale_y = static_cast<float>(in.output_height) / static_cast<float>(in.render_height);
	}
	if (g_mvec_scale_override > 0.0f)
		scale_x = scale_y = g_mvec_scale_override;

	g_params->Set(kColor, colour);
	g_params->Set(kDepth, in.depth);
	g_params->Set(kMVec, in.motion_vectors);
	g_params->Set(kOutput, g_nr_output);

	g_params->Set(kColorBaseX, 0u);
	g_params->Set(kColorBaseY, 0u);
	g_params->Set(kColorW, cw);
	g_params->Set(kColorH, ch);
	g_params->Set(kDepthBaseX, 0u);
	g_params->Set(kDepthBaseY, 0u);
	g_params->Set(kDepthW, in.render_width);
	g_params->Set(kDepthH, in.render_height);
	g_params->Set(kMVecBaseX, 0u);
	g_params->Set(kMVecBaseY, 0u);
	g_params->Set(kMVecW, in.render_width);
	g_params->Set(kMVecH, in.render_height);
	g_params->Set(kOutBaseX, 0u);
	g_params->Set(kOutBaseY, 0u);
	g_params->Set(kOutW, in.output_width);
	g_params->Set(kOutH, in.output_height);

	g_params->Set(kMVecScaleX, scale_x);
	g_params->Set(kMVecScaleY, scale_y);
	g_params->Set(kDepthInverted, 1u);
	g_params->Set(kReset, in.reset ? 1 : 0);
	g_params->Set(kIntensity, g_intensity);
	g_params->Set(kLocalTone, g_local_tone);
	g_params->Set(kLocalStruct, g_local_structure);

	static bool s_params_logged = false;
	if (!s_params_logged)
	{
		s_params_logged = true;
		STRAY_LOG_INFO("NR params: topology=%s Color=%p (%ux%u) Depth=%p (%ux%u) MVec=%p "
			"(%ux%u, scale %.3f/%.3f) Output=%p (%ux%u) depthInverted=1 intensity=%.2f",
			post ? "post-process" : "sr-shaped", static_cast<void *>(colour), cw, ch,
			static_cast<void *>(in.depth), in.render_width, in.render_height,
			static_cast<void *>(in.motion_vectors), in.render_width, in.render_height,
			scale_x, scale_y, static_cast<void *>(g_nr_output), in.output_width,
			in.output_height, g_intensity);
	}

	const NVSDK_NGX_Result result =
		nr_evaluate_feature(cmd, g_feature, g_params);
	if (NVSDK_NGX_FAILED(result))
	{
		set_error("EvaluateFeature(18)", result);
		return refuse(kRefEvaluateFailed, g_last_error);
	}

	// Hold everything NGX touched alive past GPU execution.
	if (g_keep_alive_count < sizeof(g_keep_alive) / sizeof(g_keep_alive[0]))
	{
		KeepAlive &ka = g_keep_alive[g_keep_alive_count++];
		ka.frame = g_eval_frame;
		ka.resources[0] = colour;
		ka.resources[1] = in.depth;
		ka.resources[2] = in.motion_vectors;
		ka.resources[3] = g_nr_output;
		for (ID3D12Resource *r : ka.resources)
			if (r != nullptr)
				r->AddRef();
	}
	++g_eval_frame;
	retire_keep_alive(/*all=*/false);

	// Until validation passes, NR must not reach the screen.
	if (g_validation.load(std::memory_order_acquire) != Validation::ok)
	{
		if (g_validation.load(std::memory_order_acquire) == Validation::pending)
			begin_validation(device, cmd);
		if (g_validation.load(std::memory_order_acquire) == Validation::failed)
			return refuse(kRefDegenerate,
				"the neural output validated as black/degenerate.");
		return refuse(kRefValidating, "the neural output is still being validated.");
	}

	// Validated: replace the engine's image with the neural result. Both are in states we
	// know — the engine output is the UAV our SR/RR just wrote, ours is a UAV we own.
	D3D12_RESOURCE_BARRIER to_copy[2] = {};
	to_copy[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	to_copy[0].Transition.pResource = in.image;
	to_copy[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	to_copy[0].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
	to_copy[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
	to_copy[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	to_copy[1].Transition.pResource = g_nr_output;
	to_copy[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	to_copy[1].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
	to_copy[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
	cmd->ResourceBarrier(2, to_copy);

	cmd->CopyResource(in.image, g_nr_output);

	for (int i = 0; i < 2; ++i)
	{
		const D3D12_RESOURCE_STATES before = to_copy[i].Transition.StateBefore;
		to_copy[i].Transition.StateBefore = to_copy[i].Transition.StateAfter;
		to_copy[i].Transition.StateAfter = before;
	}
	cmd->ResourceBarrier(2, to_copy);

	g_applied.fetch_add(1, std::memory_order_relaxed);
	static bool s_applied_logged = false;
	if (!s_applied_logged)
	{
		s_applied_logged = true;
		STRAY_LOG_WARN("NR APPLIED: the DLSS Neural Rendering result now replaces the SR/RR "
			"image on screen (validated non-degenerate). First occurrence only.");
	}
	return true;
}

void on_present()
{
	if (!g_enabled || g_validation.load(std::memory_order_acquire) != Validation::in_flight ||
		g_validate_readback == nullptr)
		return;
	if (--g_validate_presents_left > 0)
		return;

	void *data = nullptr;
	const D3D12_RANGE range = { 0, static_cast<SIZE_T>(g_validate_bytes) };
	if (FAILED(g_validate_readback->Map(0, &range, &data)) || data == nullptr)
	{
		g_validation.store(Validation::failed, std::memory_order_release);
		release(g_validate_readback);
		STRAY_LOG_ERROR("NR: could not map the validation readback; NR stays OFF and the SR/RR "
			"image is kept.");
		return;
	}

	const std::uint32_t bpp = format_bytes(g_nr_format);
	double max_luma = 0.0;
	bool any_known = false;
	if (bpp != 0)
	{
		const auto *base = static_cast<const unsigned char *>(data);
		for (UINT y = 0; y < g_validate_footprint.Footprint.Height; ++y)
		{
			const unsigned char *row = base + static_cast<std::size_t>(y) *
				g_validate_footprint.Footprint.RowPitch;
			for (UINT x = 0; x < g_validate_footprint.Footprint.Width; ++x)
			{
				double luma = 0.0;
				bool known = false;
				if (decode_luma(row + static_cast<std::size_t>(x) * bpp, g_nr_format, luma,
						known) && luma > max_luma)
					max_luma = luma;
				any_known = any_known || known;
			}
		}
	}

	const D3D12_RANGE no_write = { 0, 0 };
	g_validate_readback->Unmap(0, &no_write);
	release(g_validate_readback);

	if (!any_known)
	{
		g_validation.store(Validation::failed, std::memory_order_release);
		STRAY_LOG_ERROR("NR: cannot decode the output format (%d) to validate it, so NR stays "
			"OFF rather than risk a black frame. The SR/RR image is kept.",
			static_cast<int>(g_nr_format));
		return;
	}

	if (max_luma > kLumaFloor)
	{
		g_validation.store(Validation::ok, std::memory_order_release);
		STRAY_LOG_WARN("NR VALIDATED: neural output max luminance %.6f over the centre crop "
			"(> %.0e). NR will now replace the SR/RR image.", max_luma, kLumaFloor);
	}
	else
	{
		g_validation.store(Validation::failed, std::memory_order_release);
		STRAY_LOG_ERROR("NR DEGENERATE: the neural output is black (max luminance %.8f <= "
			"%.0e over the centre crop). This is the documented DLSSNR failure on some HDR "
			"inputs (docs/RESEARCH-RENODX-DLSS5.md §4.1). NR stays OFF permanently this "
			"session; the SR/RR image is kept. Nothing black ever reached the screen.",
			max_luma, kLumaFloor);
	}
}

void shutdown()
{
	release_feature();
	release(g_validate_readback);
	release(g_nr_output);
	// The snippet module is deliberately NOT FreeLibrary'd: it may still hold references, and
	// unloading a 165 MB neural DLL under a live device is not worth the teardown risk. Its
	// own Shutdown1 is likewise skipped — the caller tears the device down immediately after.
	g_runtime_loaded = false;
	g_use_direct = false;
	g_validation.store(Validation::pending, std::memory_order_release);
	g_nr_width = g_nr_height = 0;
	g_nr_format = DXGI_FORMAT_UNKNOWN;
}

} // namespace stray_dlss::nr

#endif // STRAY_DLSS_ENABLE_NGX
