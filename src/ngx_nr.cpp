#include "ngx_nr.hpp"

#include "ext_unhook.hpp"
#include "log.hpp"
#include "ngx_backend.hpp"
#include "ngx_snippet.hpp"
#include "nr_codec_pass.hpp"

#include "core/nr_codec.hpp"
#include "core/nr_hook_plan.hpp"

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
	"bad-inputs", "alloc-failed", "validating", "warmup", "mipped-input",
	// The HDR colour codec. It is mandatory (ngx_nr.hpp): refusing loudly is strictly better
	// than silently reverting to the raw-HDR path that produced the near-black neural output.
	"codec-failed", "codec-topology",
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
void set_renodx_tuning(float, unsigned int, unsigned int, unsigned int) {}
void set_mvec_scale_override(float) {}
bool preload() { return false; }
void set_warmup_frames(unsigned int) {}
void set_codec_tuning(float, float, float) {}
void set_track_exposure(bool) {}
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
// "DLSSNR.ScalingRatio", NOT "DLSSNR.Scale". Both names appear in RenoDX's binary — it sets
// each defensively across snippet builds — but only ScalingRatio is a real string in the
// 310.8.0 runtime we load, so everything we ever wrote to "Scale" was silently discarded.
// Verified by exact null-terminated string search over nvngx_dlssnr.dll.
constexpr const char *kScalingRatio = "DLSSNR.ScalingRatio";
// The remaining parameters RenoDX sets that we did not. Each one below is confirmed present
// in the runtime; the seven RenoDX names that are ABSENT from this build (InputWidth,
// InputHeight, OutputWidth, OutputHeight, Output.Width, Output.Height, Upscaling) are
// deliberately not set here, because writing dead names is how "Scale" hid for so long.
constexpr const char *kEnabled      = "DLSSNR.Enabled";
constexpr const char *kPreset       = "DLSSNR.Hint.Render.Preset";
constexpr const char *kSkinStruct   = "DLSSNR.SkinStructureStrength";
constexpr const char *kUseAutoMask  = "DLSSNR.UseAutoMask";
constexpr const char *kUICorrection = "DLSSNR.UICorrection";

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
	kRefWarmup,
	kRefMippedInput,
	kRefCodecFailed,
	kRefCodecTopology,
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
// The proxy is always FP16 RGBA, whatever the engine image is (src/nr_codec_pass.cpp explains
// why), so its readback decodes with a fixed format rather than g_nr_format.
constexpr DXGI_FORMAT kProxyReadbackFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
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
// Defaults are RenoDX's own shipped [RenoDX.DLSS5] values, not invented ones: NRIntensity=1.05,
// NRLocalTone=1.74, NRSkinStructure=1.33, NRPreset=1, NRAutoMask=1, NRUICorrection=1.
float g_intensity = 1.05f;
float g_local_tone = 1.74f;
float g_local_structure = 1.0f;
float g_skin_structure = 1.33f;
unsigned int g_preset = 1;
unsigned int g_auto_mask = 1;
unsigned int g_ui_correction = 1;
float g_mvec_scale_override = 0.0f;
// --- HDR colour codec tuning (ngx_nr.hpp documents each; math in core/nr_codec.hpp) ---
// 1.0 is deliberately NOT copied from the reference tree's default: that default exists only
// because the reference multiplies it by its tonemapper's auto-exposure texture, which we have
// no access to at a TAA-dispatch hook. It is 1.0 here because it is neutral and because the
// direction this title needs is unknown until the codec's own luminance line is read once.
float g_paper_white = 1.0f;
float g_color_strength = 1.0f;
float g_transfer_strength = 1.0f;
// Default ON, matching the reference's own `trackAutoExposure` default. See ngx_nr.hpp.
bool g_track_exposure = true;
// NR's OWN temporal accumulation is keyed on the COLOUR grid, so nothing in the feature notices
// when the GUIDE grid moves underneath it — and ours moves whenever the screen percentage does
// (1920x1080 guides at 50%, 2688x1512 at 70%, both of which this project runs). The reference
// deployment latches the guide extent and forces a single reset frame when it changes; without
// that, MVecScale silently goes from 2.0 to 1.42857 under a history accumulated against the old
// grid. Rule and provenance: src/core/nr_hook_plan.hpp.
nrplan::GuideExtentLatch g_guide_latch;
// The rect the codec actually processed this frame — the OUTPUT subrect, which can be smaller
// than the colour texture's allocation (the GetOutputExtent Max() lesson, CLAUDE.md §5). Every
// validation crop is centred on this rect so the three luminances describe the same pixels.
std::uint32_t g_codec_rect_w = 0, g_codec_rect_h = 0;
char g_dll_path[512] = "";
char g_last_error[256] = "";

// The snippet module is owned by src/ngx_snippet.cpp; this only records that the load was
// attempted and succeeded, so apply() can tell "never loaded" from "loaded but not initialised".
bool g_runtime_loaded = false;   // snippet::load succeeded (exports resolved, IAT patched)
bool g_load_tried = false;       // snippet::load has been attempted (never retried)
bool g_init_tried = false;       // Init_Ext has been attempted (never retried)
// Successful SR/RR evaluates observed. apply() is ONLY reached after a successful evaluate, so
// counting calls here counts exactly the frames in which the device, queue and swapchain
// demonstrably worked — which is the health signal the warmup gate wants.
std::uint64_t g_sr_evaluates_seen = 0;
// 60: deliberately more conservative than RenoDX, which initialises on the FIRST evaluate.
// Two measured GPU_IS_LOST events justify the margin; it costs one second of gameplay.
unsigned int g_warmup_frames = 60;

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

// One readback per stage of the codec. Reading only the NEURAL output told us it was near black
// (0.0026) but not WHY, and picking NgxNRPaperWhiteScale by guesswork costs a round trip per
// guess. Input -> proxy -> output in one line says immediately whether the input is already
// tiny, whether the proxy landed near the 0.75 soft-clip knee, and whether the network answered
// at all. (CLAUDE.md §0.1: each round trip must carry the maximum diagnostic payload.)
struct CropReadback
{
	ID3D12Resource *buffer = nullptr;
	D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
	std::uint64_t bytes = 0;
	DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
};
CropReadback g_crop_input;  // the engine's linear HDR image, before the encode
CropReadback g_crop_proxy;  // what we actually handed the network
CropReadback g_crop_neural; // what it answered
// The crop's own extent, for the diagnostic line. Written on the recording thread before the
// release-store that publishes `in_flight`, read on the present thread after the acquire-load —
// same handoff as the readback fields themselves.
std::uint32_t g_validate_crop_w = 0, g_validate_crop_h = 0;
// Whether the frame that armed the validation ran the HDR codec. Post-tonemap there is no proxy
// and no paper white, so the codec half of the luminance line would be a lie — and a diagnostic
// that lies is worse than one that is absent. Same publication discipline as the fields above.
bool g_validate_codec = false;
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
	g_params->Set(kScalingRatio, out_w > 0 && in_w > 0
		? static_cast<float>(out_w) / static_cast<float>(in_w) : 1.0f);
	// Reversed-Z: UE 4.27 throughout, same flag SR carries. (CLAUDE.md §2.4)
	g_params->Set(kDepthInverted, 1u);
	g_params->Set(kEnabled, 1u);
	g_params->Set(kIntensity, g_intensity);
	g_params->Set(kLocalTone, g_local_tone);
	g_params->Set(kLocalStruct, g_local_structure);
	g_params->Set(kSkinStruct, g_skin_structure);
	g_params->Set(kPreset, g_preset);
	g_params->Set(kUseAutoMask, g_auto_mask);
	g_params->Set(kUICorrection, g_ui_correction);

	STRAY_LOG_WARN("NR: creating NGX feature 18 (Reserved18 / DLSSNR) %ux%u -> %ux%u, "
		"topology=%s, intensity=%.2f localTone=%.2f localStructure=%.2f skinStructure=%.2f "
		"preset=%u autoMask=%u uiCorrection=%u scalingRatio=%.3f...",
		in_w, in_h, out_w, out_h, post ? "post-process" : "sr-shaped",
		g_intensity, g_local_tone, g_local_structure, g_skin_structure,
		g_preset, g_auto_mask, g_ui_correction,
		out_w > 0 && in_w > 0 ? static_cast<double>(out_w) / static_cast<double>(in_w) : 1.0);

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
	release(g_crop_input.buffer);
	release(g_crop_proxy.buffer);
	release(g_crop_neural.buffer);
	g_validation.store(Validation::pending, std::memory_order_release);

	D3D12_HEAP_PROPERTIES heap = {};
	heap.Type = D3D12_HEAP_TYPE_DEFAULT;

	D3D12_RESOURCE_DESC tex = src;
	tex.Alignment = 0;
	// NGX writes its output as a UAV; missing the flag can yield a black result with no error
	// at all. (docs/RESEARCH.md §3.5)
	tex.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
	// NEVER inherit the engine image's mip chain, array size or sample count. `tex = src` used
	// to copy all three, and a MIPPED neural output is a documented GPU hang: the community
	// D3D12 port reports DXGI_ERROR_DEVICE_HUNG a few seconds after the neural pass starts, on
	// two separate attempts to make a mipped texture acceptable, and concludes the code behind
	// the check genuinely cannot handle a mip chain. That is our exact symptom — NR ran ~48s
	// and then the GPU left the bus. The runtime wants a plain, typed, single-subresource 2D
	// texture, so build one rather than mirroring whatever UE4 happened to allocate.
	tex.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	tex.MipLevels = 1;
	tex.DepthOrArraySize = 1;
	tex.SampleDesc.Count = 1;
	tex.SampleDesc.Quality = 0;
	tex.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

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
	STRAY_LOG_INFO("NR: neural output texture %ux%u fmt=%d created as single-mip/single-slice "
		"(engine image had mips=%u arraySize=%u samples=%u — a mipped neural output is a "
		"documented DEVICE_HUNG, so those are never inherited).",
		w, h, static_cast<int>(src.Format), static_cast<unsigned int>(src.MipLevels),
		static_cast<unsigned int>(src.DepthOrArraySize),
		static_cast<unsigned int>(src.SampleDesc.Count));
	return true;
}

// Records a copy of a centred crop of one resource into its own readback buffer. Mapped a few
// presents later by on_present(). `res` is expected in `base_state` and is left in it.
//
// `base_state` is a parameter rather than a constant because the colour image's state depends on
// the call site: the TAA site hands us `u0` in UNORDERED_ACCESS (SR/RR just wrote it), the
// post-tonemap site hands us a staging copy in NON_PIXEL_SHADER_RESOURCE. Assuming the wrong one
// is a barrier whose StateBefore does not match reality — a validation error on WARP and, under
// vkd3d-proton where no debug layer exists, silent undefined behaviour.
bool copy_crop(ID3D12Device *device, ID3D12GraphicsCommandList *cmd, ID3D12Resource *res,
               DXGI_FORMAT format, const D3D12_BOX &box, D3D12_RESOURCE_STATES base_state,
               CropReadback &out)
{
	const UINT crop_w = box.right - box.left;
	const UINT crop_h = box.bottom - box.top;

	D3D12_RESOURCE_DESC crop = {};
	crop.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	crop.Width = crop_w;
	crop.Height = crop_h;
	crop.DepthOrArraySize = 1;
	crop.MipLevels = 1;
	crop.Format = format;
	crop.SampleDesc.Count = 1;
	UINT64 total = 0;
	D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
	device->GetCopyableFootprints(&crop, 0, 1, 0, &footprint, nullptr, nullptr, &total);
	if (total == 0)
		return false;

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
			D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&out.buffer))))
		return false;

	D3D12_RESOURCE_BARRIER barrier = {};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Transition.pResource = res;
	barrier.Transition.Subresource = 0;
	barrier.Transition.StateBefore = base_state;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
	cmd->ResourceBarrier(1, &barrier);

	D3D12_TEXTURE_COPY_LOCATION src_loc = {};
	src_loc.pResource = res;
	src_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	src_loc.SubresourceIndex = 0;
	D3D12_TEXTURE_COPY_LOCATION dst_loc = {};
	dst_loc.pResource = out.buffer;
	dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	dst_loc.PlacedFootprint = footprint;
	cmd->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, &box);

	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
	barrier.Transition.StateAfter = base_state;
	cmd->ResourceBarrier(1, &barrier);

	out.footprint = footprint;
	out.bytes = total;
	out.format = format;
	return true;
}

// Records the three validation crops — colour input, encoded proxy, neural output — over the
// SAME centred box, so their luminances are directly comparable. Until this passes, NR runs but
// the decode never touches the engine image.
void begin_validation(ID3D12Device *device, ID3D12GraphicsCommandList *cmd,
                      ID3D12Resource *image, D3D12_RESOURCE_STATES image_state, bool with_proxy)
{
	if (g_validation.load(std::memory_order_acquire) != Validation::pending ||
		g_nr_output == nullptr || image == nullptr)
		return;
	// The proxy only exists when the codec ran. At a post-tonemap site there is none, and its
	// absence must not block the verdict — the neural crop is what the verdict rests on.
	if (with_proxy && nrp::proxy() == nullptr)
		return;

	// Centre the crop on the rect the codec PROCESSED, not on the texture allocation: UE4's
	// scene targets can be left larger than the current view rect, and a crop centred on the
	// allocation would land outside the region NGX wrote.
	const UINT rect_w = g_codec_rect_w;
	const UINT rect_h = g_codec_rect_h;
	const UINT crop_w = rect_w < kValidateSize ? rect_w : kValidateSize;
	const UINT crop_h = rect_h < kValidateSize ? rect_h : kValidateSize;
	if (crop_w == 0 || crop_h == 0)
		return;

	D3D12_BOX box = {};
	box.left = (rect_w - crop_w) / 2;
	box.top = (rect_h - crop_h) / 2;
	box.front = 0;
	box.right = box.left + crop_w;
	box.bottom = box.top + crop_h;
	box.back = 1;

	// The neural crop is the one the verdict rests on; the other two are diagnostics, so a
	// failure to capture them must not block validation.
	if (!copy_crop(device, cmd, g_nr_output, g_nr_format, box,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS, g_crop_neural))
	{
		release(g_crop_neural.buffer);
		return;
	}
	if (!copy_crop(device, cmd, image, g_nr_format, box, image_state, g_crop_input))
		release(g_crop_input.buffer);
	if (with_proxy &&
		!copy_crop(device, cmd, nrp::proxy(), kProxyReadbackFormat, box,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS, g_crop_proxy))
		release(g_crop_proxy.buffer);

	g_validate_crop_w = crop_w;
	g_validate_crop_h = crop_h;
	g_validate_codec = with_proxy;
	g_validate_presents_left = kValidateLatency;
	// Publish LAST: everything above must be visible to the present thread that sees this.
	g_validation.store(Validation::in_flight, std::memory_order_release);
	STRAY_LOG_INFO("NR: validating the neural output (%ux%u crop centred on the %ux%u processed "
		"rect, with matching colour-input and proxy crops). Until it passes, NR runs but the "
		"decode does NOT touch the screen - a degenerate runtime cannot show a black frame.",
		crop_w, crop_h, rect_w, rect_h);
}

// Decodes one texel to three channel values, for the formats the engine output can be. Anything
// we cannot decode is treated as "cannot validate", which fails safe (NR stays off).
//
// Returns the CHANNELS rather than a luminance because the codec's diagnostic needs two
// luminances from the same texel: the raw code-value one (which is what the degeneracy verdict
// has always used, and its threshold is calibrated against) and the sRGB-DECODED one, which is
// the only one comparable to the 0.75 soft-clip knee. Both the proxy and the neural answer are
// sRGB-encoded display-referred images; the colour input is not.
bool decode_rgb(const unsigned char *p, DXGI_FORMAT fmt, double c[3], bool &known)
{
	known = true;
	switch (fmt)
	{
	case DXGI_FORMAT_R16G16B16A16_FLOAT:
	{
		// half -> float, three channels.
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
		c[0] = ufloat(v & 0x7FF, 6, 5);
		c[1] = ufloat((v >> 11) & 0x7FF, 6, 5);
		c[2] = ufloat((v >> 22) & 0x3FF, 5, 5);
		return true;
	}
	case DXGI_FORMAT_R10G10B10A2_UNORM:
	{
		std::uint32_t v = 0;
		std::memcpy(&v, p, sizeof(v));
		c[0] = (v & 0x3FF) / 1023.0;
		c[1] = ((v >> 10) & 0x3FF) / 1023.0;
		c[2] = ((v >> 20) & 0x3FF) / 1023.0;
		return true;
	}
	case DXGI_FORMAT_R8G8B8A8_UNORM:
	case DXGI_FORMAT_B8G8R8A8_UNORM:
		c[0] = p[0] / 255.0;
		c[1] = p[1] / 255.0;
		c[2] = p[2] / 255.0;
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

void set_renodx_tuning(float skin_structure_strength, unsigned int preset,
	unsigned int use_auto_mask, unsigned int ui_correction)
{
	g_skin_structure = skin_structure_strength;
	g_preset = preset;
	g_auto_mask = use_auto_mask;
	g_ui_correction = ui_correction;
}

void counters(std::uint64_t &applied, std::uint64_t &refused, std::uint32_t out[kNrRefusalCount])
{
	applied = g_applied.load(std::memory_order_relaxed);
	refused = g_refused.load(std::memory_order_relaxed);
	for (int i = 0; i < kNrRefusalCount; ++i)
		out[i] = g_refusals[i].load(std::memory_order_relaxed);
}

void set_warmup_frames(unsigned int frames) { g_warmup_frames = frames; }

void set_track_exposure(bool enabled) { g_track_exposure = enabled; }

void set_codec_tuning(float paper_white, float color_strength, float transfer_strength)
{
	g_paper_white = paper_white;
	// Both strengths are lerp weights and are meaningless outside [0,1]; the reference tree
	// clamps them the same way through its RTX_OPTION min/max.
	g_color_strength = color_strength < 0.0f ? 0.0f : (color_strength > 1.0f ? 1.0f : color_strength);
	g_transfer_strength = transfer_strength < 0.0f ? 0.0f
		: (transfer_strength > 1.0f ? 1.0f : transfer_strength);
	STRAY_LOG_INFO("NR codec: paperWhite=%.4f -> scale=%.4f, colorStrength=%.2f, "
		"transferStrength=%.2f. The codec is MANDATORY: feature 18 is a display-referred "
		"network and our hook point carries raw linear HDR, so without it the network answers "
		"near-black. transferStrength=0 is an exact bypass if you need one.",
		static_cast<double>(g_paper_white),
		static_cast<double>(nrc::proxy_scale(g_paper_white, 1.0f)),
		static_cast<double>(g_color_strength), static_cast<double>(g_transfer_strength));
}

// The CHEAP half: LoadLibrary + export resolution + the identity IAT patch. No GPU contact, no
// Init_Ext. Safe to call at device init, but off by default — see ngx_nr.hpp for why the init
// half is deferred instead.
static bool load_snippet_once()
{
	if (!g_enabled || g_load_tried)
		return g_runtime_loaded;
	g_load_tried = true;

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
	return true;
}

// The EXPENSIVE half, deliberately deferred to a healthy steady state (ngx_nr.hpp).
static bool init_snippet_once(ID3D12Device *device)
{
	if (g_init_tried)
		return g_use_direct;
	g_init_tried = true;

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
		return false;
	}

	g_use_direct = true;
	STRAY_LOG_WARN("NR: snippet Init_Ext SUCCEEDED — feature 18 will be created and evaluated "
		"through the SNIPPET's own exports, bypassing the NGX core entirely.");
	snippet::log_identity_calls();
	return true;
}

bool preload()
{
	if (!g_enabled)
		return false;
	STRAY_LOG_INFO("NR: snippet PRE-LOAD at device init ([STRAYDLSS] NgxNRPreload): "
		"LoadLibrary, export resolution and the identity IAT patch only — all pure memory "
		"work, no GPU contact. Init_Ext is deferred to first use.");
	return load_snippet_once();
}

bool apply(ID3D12Device *device, ID3D12GraphicsCommandList *cmd, const ApplyInputs &in)
{
	if (!g_enabled)
		return false;

	// apply() is only reached AFTER a successful SR/RR evaluate, so every call here is one
	// frame in which the device, queue and swapchain demonstrably worked. That makes this the
	// right place both to count warmup and to do first-use initialisation.
	++g_sr_evaluates_seen;

	// WARMUP GATE. RenoDX initialises on the first evaluate ("will retry lazily on first
	// evaluate"); we deliberately wait longer. Two GPU losses (GPU_IS_LOST, host power-cycle
	// each time) have been measured in sessions that touched this runtime early, so a margin
	// of demonstrably healthy frames before a leaked pre-release DLL contacts the GPU is us
	// being more conservative than RenoDX ON PURPOSE — not a guess about their behaviour.
	if (g_sr_evaluates_seen < g_warmup_frames)
		return refuse(kRefWarmup, "waiting out the NR warmup window before initialising the "
			"runtime.");

	// FIRST USE: load (if not pre-loaded) and initialise, exactly once. Both halves latch, so
	// a failure never retries per frame.
	if (!g_load_tried)
		load_snippet_once();
	if (!g_runtime_loaded)
		return refuse(kRefDllMissing, "nvngx_dlssnr.dll was never loaded.");
	if (!g_init_tried)
	{
		STRAY_LOG_WARN("NR: warmup complete (%llu SR evaluates) — initialising the NR runtime "
			"now (lazy, first use).",
			static_cast<unsigned long long>(g_sr_evaluates_seen));
		init_snippet_once(device);
	}

	if (device == nullptr || cmd == nullptr || in.image == nullptr || in.depth == nullptr ||
		in.motion_vectors == nullptr || in.render_width == 0 || in.output_width == 0)
		return refuse(kRefBadInputs, "a required resource or dimension was missing.");

	// WHICH COLOUR PIPELINE. One NR path, two call sites, and this is the only place they differ.
	//
	//  taa_dispatch  — `image` is raw unbounded pre-exposed linear HDR, which is out of a
	//                  display-referred network's domain, so the HDR codec is MANDATORY and the
	//                  network sees the PROXY. The result comes back as a residual, in place.
	//  post_tonemap  — `image` is the already-tonemapped back buffer. The codec is BYPASSED, on
	//                  purpose: running the soft clip and the sRGB encode over an image that has
	//                  already been through the game's tone curve and its display transfer would
	//                  apply that transfer twice. The network sees the image itself, and its
	//                  answer is copied back whole rather than differenced — which is only safe
	//                  because this site is TERMINAL (nothing carries it into the next frame).
	const bool codec = in.site == Site::taa_dispatch;

	// The residual needs the proxy, the neural answer and the original to be the SAME pixels.
	// sr-shaped puts Color at render resolution and Output at display resolution, so there is no
	// per-pixel correspondence to subtract across and no residual exists to carry. It is equally
	// meaningless post-tonemap, where there is nothing left to upscale. Refuse loudly rather than
	// fall back to the raw-HDR path, which is the exact configuration that produced red noise and
	// a 0.0026 neural output (src/core/nr_codec.hpp).
	const bool post = g_topology == Topology::post_process;
	if (!post)
		return refuse(kRefCodecTopology,
			"NgxNRTopology=sr cannot use the HDR colour codec: the residual transfer needs the "
			"proxy, the neural answer and the original to be the same pixels, and sr-shaped puts "
			"the colour input at render resolution and the output at display resolution. Use "
			"NgxNRTopology=post.");
	ID3D12Resource *colour = in.image;

	// The state `image` arrives in and must be left in. Documented in ngx_nr.hpp's Site enum and
	// enforced by the caller on both sides; the barriers below and the validation crop both
	// depend on getting it right.
	const D3D12_RESOURCE_STATES image_state = codec
		? D3D12_RESOURCE_STATE_UNORDERED_ACCESS
		: D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

	// A mipped / arrayed / multisampled colour input is the one hazard we cannot fix by
	// allocating our own texture, and feeding one is a documented DXGI_ERROR_DEVICE_HUNG rather
	// than an error return. Refuse it loudly: a log line costs a frame, a hung GPU costs the
	// user a power cycle. (CLAUDE.md §0.2 — prefer a loud failure.)
	{
		const D3D12_RESOURCE_DESC cd = colour->GetDesc();
		if (cd.MipLevels != 1 || cd.DepthOrArraySize != 1 || cd.SampleDesc.Count != 1)
			return refuse(kRefMippedInput,
				"the colour input is not a plain single-mip, single-slice, non-MSAA 2D texture; "
				"handing one to the neural runtime hangs the GPU instead of returning an error.");
	}

	// The result goes to OUR texture, never straight over the engine's output.
	if (!ensure_output_texture(device, in.image))
		return refuse(kRefAllocFailed, g_last_error);
	if (!ensure_feature(cmd, in.render_width, in.render_height, in.output_width,
			in.output_height))
		return refuse(kRefCreateFailed, "feature 18 could not be created.");

	ext_unhook::repair();

	// Colour rect: the output rect, since post-process consumes the already-upscaled image. At a
	// post-tonemap site that IS the back buffer's rect, which the caller passes as output_*.
	const std::uint32_t cw = in.output_width;
	const std::uint32_t ch = in.output_height;
	g_codec_rect_w = cw;
	g_codec_rect_h = ch;

	float codec_scale = 0.0f;
	if (codec)
	{
		// --- HDR colour codec, stage 1 of 2 ---
		//
		// Feature 18 is a DISPLAY-REFERRED image network and `in.image` is unbounded pre-exposed
		// linear HDR, so what the snippet gets is the PROXY, never the engine's raw image.
		// Nothing below reads `in.image` again until the decode. (src/core/nr_codec.hpp)
		if (!nrp::initialise(device, in.image, cw, ch))
			return refuse(kRefCodecFailed, nrp::last_error());

		// TRACKED EXPOSURE. `1.0f` is the fallback paper white, not an exposure — see
		// nrc::proxy_scale's signature.
		const float static_scale = nrc::proxy_scale(g_paper_white, 1.0f);
		codec_scale = g_track_exposure
			? nrc::proxy_scale_tracked(g_paper_white, 1.0f, in.one_over_pre_exposure)
			: static_scale;

		// The decomposition, once. When the user reports "paper white 0.1 looks best", this line
		// is what says whether tracking has made 1.0 the new correct value.
		static bool s_scale_logged = false;
		if (!s_scale_logged)
		{
			s_scale_logged = true;
			STRAY_LOG_WARN("NR codec scale: paperWhite=%.4f -> staticScale=%.4f x exposure=%.4f "
				"(View row 135.z OneOverPreExposure; tracking=%s%s) = EFFECTIVE %.4f. The 0.75 "
				"soft-clip knee is what this has to land the frame near. Stray's scene colour "
				"here carries UE4's pre-exposure (~0.056 measured), so an untracked scale near "
				"1.0 shows the network a nearly black image — which is why hand-dialling paper "
				"white to ~0.1 looked best before this existed.",
				static_cast<double>(g_paper_white), static_cast<double>(static_scale),
				static_cast<double>(in.one_over_pre_exposure),
				g_track_exposure ? "on" : "OFF",
				g_track_exposure && !(in.one_over_pre_exposure > 0.0f)
					? ", but the View CB was unreadable so the STATIC scale was used" : "",
				static_cast<double>(codec_scale));
		}
		if (!nrp::record_encode(cmd, in.image, cw, ch, codec_scale, g_color_strength,
				g_transfer_strength))
			return refuse(kRefCodecFailed, nrp::last_error());

		colour = nrp::proxy();
		if (colour == nullptr)
			return refuse(kRefCodecFailed, "the codec produced no proxy texture.");

		// NVIDIA's guide wants NGX inputs in NON_PIXEL_SHADER_RESOURCE. The proxy is ours, so
		// this costs nothing and removes one way to get a silently black result
		// (docs/RESEARCH.md §3.5). It MUST be undone on every path out of the evaluate below, or
		// the recorded state diverges from the resource's real state.
		nrp::transition_proxy(cmd, /*to_shader_resource=*/true);
	}
	// else: `colour` is `in.image` itself, already display-referred and already in
	// NON_PIXEL_SHADER_RESOURCE — the caller put it there, for exactly this reason.

	// Motion vectors are ours: dense RG16_FLOAT in RENDER-resolution pixels, y-down.
	//
	// The scale is the COLOUR/GUIDE ratio, not 1.0. The snippet works on the COLOUR grid while
	// our vectors are on the guide grid, and it receives the colour rect, the mvec rect and this
	// scale as three INDEPENDENT values — it never derives one from the others, so declaring
	// MVecSubrectWidth/Height does not make the ratio redundant.
	//
	// This was briefly 1.0, on a misreading of the reference. dxvk-remix's
	// NGXNeuralRenderingContext sets exactly this ratio and its comment names our case:
	// "2.0 for 4K colour over 1080p guides". The same codebase sets 1.0 for DLSS *SR* with the
	// SAME buffer, because SR's working grid IS the render grid — which is also why our SR path
	// correctly passes InMVScale = 1.
	//
	// Computed, never hardcoded: at 50% it is 2.0 but at 70% it is 1.42857, and this project
	// runs both.
	float scale_x = 1.0f, scale_y = 1.0f;
	if (in.render_width > 0 && in.render_height > 0)
	{
		scale_x = static_cast<float>(in.output_width) / static_cast<float>(in.render_width);
		scale_y = static_cast<float>(in.output_height) / static_cast<float>(in.render_height);
	}
	// A caller whose colour rect is NOT the TAA output rect — the post-tonemap sites, whose rect
	// is the back buffer's — computes the ratio against its own rect and passes it here.
	if (in.mvec_scale_x > 0.0f && in.mvec_scale_y > 0.0f)
	{
		scale_x = in.mvec_scale_x;
		scale_y = in.mvec_scale_y;
	}
	if (g_mvec_scale_override > 0.0f)
		scale_x = scale_y = g_mvec_scale_override;

	// FEATURE 18 KEEPS ITS OWN TEMPORAL ACCUMULATION, and it is keyed on the colour grid alone.
	// Moving the guide grid underneath it — which a screen-percentage change does, at a fixed
	// output resolution, taking MVecScale from 2.0 to 1.42857 with it — invalidates a history
	// nothing else in the pipeline notices. Latch the extent and force ONE reset frame when it
	// moves, exactly as the reference deployment does. (src/core/nr_hook_plan.hpp)
	bool reset = in.reset;
	if (nrplan::latch_guide_extent(g_guide_latch, in.render_width, in.render_height))
	{
		reset = true;
		STRAY_LOG_WARN("NR: the guide grid moved to %ux%u (MVecScale is now %.5f/%.5f). Feature "
			"18's temporal accumulation was built against the old grid and nothing else in the "
			"pipeline notices, so DLSSNR.Reset is forced for this one frame.",
			in.render_width, in.render_height, static_cast<double>(scale_x),
			static_cast<double>(scale_y));
	}

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
	g_params->Set(kReset, reset ? 1 : 0);
	g_params->Set(kEnabled, 1u);
	g_params->Set(kIntensity, g_intensity);
	g_params->Set(kLocalTone, g_local_tone);
	g_params->Set(kLocalStruct, g_local_structure);
	g_params->Set(kSkinStruct, g_skin_structure);
	g_params->Set(kPreset, g_preset);
	g_params->Set(kUseAutoMask, g_auto_mask);
	g_params->Set(kUICorrection, g_ui_correction);

	// One line per SITE, not one per session: `taa` and a post-tonemap mode describe genuinely
	// different pixels, so a session that switched between them must not report only the first.
	static bool s_params_logged[2] = {};
	const int site_index = codec ? 0 : 1;
	if (!s_params_logged[site_index])
	{
		s_params_logged[site_index] = true;
		if (codec)
			STRAY_LOG_INFO("NR params [site=taa-dispatch]: topology=post-process Color=%p = the "
				"CODEC PROXY (%ux%u, FP16, sRGB-encoded soft-clipped, paperWhite=%.4f "
				"scale=%.4f) NOT the raw HDR image %p; Depth=%p (%ux%u) MVec=%p (%ux%u, scale "
				"%.3f/%.3f) Output=%p (%ux%u) depthInverted=1 reset=%d intensity=%.2f",
				static_cast<void *>(colour), cw, ch, static_cast<double>(g_paper_white),
				static_cast<double>(codec_scale), static_cast<void *>(in.image),
				static_cast<void *>(in.depth), in.render_width, in.render_height,
				static_cast<void *>(in.motion_vectors), in.render_width, in.render_height,
				scale_x, scale_y, static_cast<void *>(g_nr_output), in.output_width,
				in.output_height, reset ? 1 : 0, g_intensity);
		else
			STRAY_LOG_INFO("NR params [site=post-tonemap]: HDR CODEC BYPASSED (the image is "
				"already display-referred; encoding it again would apply the tone transfer "
				"twice, and there is no pre-exposure left to undo, so NgxNRPaperWhiteScale and "
				"NgxNRTrackExposure are both inert here) — Color=%p is the back-buffer copy "
				"ITSELF (%ux%u); Depth=%p (%ux%u) "
				"MVec=%p (%ux%u, scale %.3f/%.3f = colour/guide) Output=%p (%ux%u) "
				"depthInverted=1 reset=%d intensity=%.2f uiCorrection=%u",
				static_cast<void *>(colour), cw, ch,
				static_cast<void *>(in.depth), in.render_width, in.render_height,
				static_cast<void *>(in.motion_vectors), in.render_width, in.render_height,
				scale_x, scale_y, static_cast<void *>(g_nr_output), in.output_width,
				in.output_height, reset ? 1 : 0, g_intensity, g_ui_correction);
	}

	const NVSDK_NGX_Result result =
		nr_evaluate_feature(cmd, g_feature, g_params);

	// UNCONDITIONALLY, and before the result is even looked at: barriers are recorded, so an
	// early return here would leave the command list describing a proxy in
	// NON_PIXEL_SHADER_RESOURCE while the next frame's encode transitions it out of
	// UNORDERED_ACCESS. That mismatch is a validation error on WARP and undefined behaviour on
	// the target, where there is no debug layer to say so.
	if (codec)
		nrp::transition_proxy(cmd, /*to_shader_resource=*/false);

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
			begin_validation(device, cmd, in.image, image_state, /*with_proxy=*/codec);
		if (g_validation.load(std::memory_order_acquire) == Validation::failed)
			return refuse(kRefDegenerate,
				"the neural output validated as black/degenerate.");
		return refuse(kRefValidating, "the neural output is still being validated.");
	}

	if (codec)
	{
		// --- HDR colour codec, stage 2 of 2 ---
		//
		// Validated, so carry the network's change back onto the engine image IN PLACE. This
		// replaces the full-RGBA CopyResource this used to end with, which was wrong twice over:
		// it discarded the HDR range (the network's answer is display-referred and bounded to
		// [0,1]) and it overwrote the alpha channel with the network's meaningless one — and on
		// this title that resource becomes the next frame's TAA history (CLAUDE.md §2.9), so the
		// engine would have read the damage straight back in.
		if (!nrp::record_decode(cmd, in.image, g_nr_output, cw, ch))
			return refuse(kRefCodecFailed, nrp::last_error());
	}
	else
	{
		// POST-TONEMAP: a whole copy, and the reasons the TAA site cannot do this do not apply.
		//
		//  * The HDR range is not discarded, because there is none left to discard — the image
		//    is display-referred already and the network answers in the same encoding.
		//  * The alpha is not damaged in a way anything reads: this resource is the frame's
		//    terminal image, presented and then reused as a fresh render target next frame.
		//  * Nothing carries it into the engine's temporal state, which is the entire point of
		//    moving the hook here (CLAUDE.md, "NR's output feeds the engine's temporal history").
		//
		// Both textures were built from the same desc by ensure_output_texture, so the extents,
		// the format and the subresource count match and CopyResource is legal.
		D3D12_RESOURCE_BARRIER to_copy[2] = {};
		to_copy[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		to_copy[0].Transition.pResource = in.image;
		to_copy[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		to_copy[0].Transition.StateBefore = image_state;
		to_copy[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
		to_copy[1] = to_copy[0];
		to_copy[1].Transition.pResource = g_nr_output;
		to_copy[1].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
		to_copy[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
		cmd->ResourceBarrier(2, to_copy);

		cmd->CopyResource(in.image, g_nr_output);

		// Put both back exactly where they were found. `in.image` is left in the state the
		// caller documented and will transition onward from; g_nr_output is left where the next
		// evaluate expects it.
		D3D12_RESOURCE_BARRIER back[2] = {};
		back[0] = to_copy[0];
		back[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
		back[0].Transition.StateAfter = image_state;
		back[1] = to_copy[1];
		back[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
		back[1].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
		cmd->ResourceBarrier(2, back);
	}

	g_applied.fetch_add(1, std::memory_order_relaxed);
	static bool s_applied_logged[2] = {};
	if (!s_applied_logged[site_index])
	{
		s_applied_logged[site_index] = true;
		if (codec)
			STRAY_LOG_WARN("NR APPLIED [site=taa-dispatch]: the DLSS Neural Rendering result is "
				"now carried onto the SR/RR image as a residual (validated non-degenerate), "
				"preserving its HDR range and its alpha. First occurrence only.");
		else
			STRAY_LOG_WARN("NR APPLIED [site=post-tonemap]: the DLSS Neural Rendering result is "
				"now copied over the back-buffer image (validated non-degenerate). No HDR codec "
				"and no residual: the image was already display-referred, and this site is "
				"terminal, so nothing carries the result into the engine's temporal state. "
				"First occurrence only.");
	}
	return true;
}

namespace {

// The two luminances one crop yields, plus whether it could be decoded at all.
struct CropLuma
{
	// Rec.709 over the stored code values. The degeneracy verdict has always used this and its
	// 1e-5 threshold is calibrated against it, so it stays the verdict's input, unchanged.
	double raw = -1.0;
	// Rec.709 after sRGB-DECODING each channel. For the proxy and the neural answer — both
	// sRGB-encoded display-referred images — this is the number that is directly comparable to
	// the 0.75 soft-clip knee, and to `colour input x scale`. Meaningless for the linear HDR
	// colour input, which is why the log prints `raw` for that one.
	double linear = -1.0;
	bool known = false;
};

// Maps one crop readback, reduces it, and releases it. A crop that was never captured comes back
// with the -1.0 sentinels.
CropLuma drain_crop(CropReadback &crop)
{
	CropLuma out;
	if (crop.buffer == nullptr)
		return out;

	void *data = nullptr;
	const D3D12_RANGE range = { 0, static_cast<SIZE_T>(crop.bytes) };
	if (SUCCEEDED(crop.buffer->Map(0, &range, &data)) && data != nullptr)
	{
		const std::uint32_t bpp = format_bytes(crop.format);
		if (bpp != 0)
		{
			out.raw = 0.0;
			out.linear = 0.0;
			const auto *base = static_cast<const unsigned char *>(data);
			for (UINT y = 0; y < crop.footprint.Footprint.Height; ++y)
			{
				const unsigned char *row =
					base + static_cast<std::size_t>(y) * crop.footprint.Footprint.RowPitch;
				for (UINT x = 0; x < crop.footprint.Footprint.Width; ++x)
				{
					double c[3] = {};
					bool texel_known = false;
					if (!decode_rgb(row + static_cast<std::size_t>(x) * bpp, crop.format, c,
							texel_known))
						continue;
					out.known = out.known || texel_known;

					const double raw = 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2];
					if (raw > out.raw)
						out.raw = raw;

					// The shader's own sRGB decode, through the twin CI pins
					// (src/core/nr_codec.cpp), so this number means the same thing the decode
					// pass means by it.
					double lin[3];
					for (int i = 0; i < 3; ++i)
						lin[i] = static_cast<double>(
							nrc::srgb_decode_channel(static_cast<float>(c[i])));
					const double linear = 0.2126 * lin[0] + 0.7152 * lin[1] + 0.0722 * lin[2];
					if (linear > out.linear)
						out.linear = linear;
				}
			}
		}
		const D3D12_RANGE no_write = { 0, 0 };
		crop.buffer->Unmap(0, &no_write);
	}

	release(crop.buffer);
	return out;
}

} // namespace

void on_present()
{
	if (!g_enabled || g_validation.load(std::memory_order_acquire) != Validation::in_flight)
		return;
	if (--g_validate_presents_left > 0)
		return;

	const CropLuma neural = drain_crop(g_crop_neural);
	const CropLuma input = drain_crop(g_crop_input);
	const CropLuma proxy = drain_crop(g_crop_proxy);
	const double max_luma = neural.raw;

	if (!g_validate_codec)
	{
		// POST-TONEMAP: no codec ran, so there is no proxy and paper white means nothing here.
		// Report only what was actually measured — the image the network was shown, and what it
		// answered — rather than dressing it in codec terms that do not apply. A diagnostic that
		// lies is worse than one that is absent.
		STRAY_LOG_WARN("NR LUMINANCE [site=post-tonemap, HDR codec BYPASSED] (max Rec.709 over "
			"one %ux%u centre crop): back-buffer INPUT raw %.6f%s -> neural OUTPUT raw %.6f%s "
			"(sRGB-decoded %.6f). Both are display-referred code values, so they are directly "
			"comparable to each other and NOT to the 0.75 soft-clip knee. NgxNRPaperWhiteScale, "
			"NgxNRColorStrength and NgxNRTransferStrength do NOTHING at this site.",
			g_validate_crop_w, g_validate_crop_h,
			input.raw, input.known ? "" : " (NOT DECODED)",
			neural.raw, neural.known ? "" : " (NOT DECODED)", neural.linear);
	}
	else
	{
		// THE line that chooses NgxNRPaperWhiteScale, and the reason all three crops are taken
		// over the same texels. Reading only the neural output told us it was near black (0.0026)
		// but not why, and every guess at the scale costs a round trip on a machine we do not
		// have.
		//
		// How to read it. `input` is scene-linear HDR; `proxy` and `output` are sRGB-DECODED, so
		// all three are linear and directly comparable. Below the soft-clip knee the codec is
		// just a multiply, so `proxy` should be about `input x scale`; if it is far below the
		// 0.75 knee the network is being shown a black image and paper white must come DOWN
		// (scale = 1/paperWhite). If `proxy` is pinned at 1.0 the frame is crushed into the
		// shoulder and paper white must go UP. A healthy proxy with a black output means the
		// fault is the runtime, not the codec.
		const float scale = nrc::proxy_scale(g_paper_white, 1.0f);
		const bool can_suggest = input.known && input.raw > 0.0;
		const double want_scale = can_suggest
			? static_cast<double>(nrc::kSoftClipKnee) / input.raw : -1.0;
		STRAY_LOG_WARN("NR CODEC LUMINANCE [site=taa-dispatch] (max Rec.709 over one %ux%u centre "
			"crop, all linear): colour INPUT %.6f (scene-linear HDR%s) -> encoded PROXY %.6f "
			"(sRGB-decoded%s, raw code value %.6f) -> neural OUTPUT %.6f (sRGB-decoded%s, raw "
			"%.6f). paperWhite=%.4f effectiveScale=%.4f colorStrength=%.2f transferStrength=%.2f. "
			"Below the %.2f knee the proxy is just input x scale, so to put this frame's peak AT "
			"the knee use scale ~%.4f, i.e. NgxNRPaperWhiteScale ~%.4f. A -1 means that crop was "
			"not captured or its format could not be decoded.",
			g_validate_crop_w, g_validate_crop_h,
			input.raw, input.known ? "" : ", NOT DECODED",
			proxy.linear, proxy.known ? "" : ", NOT DECODED", proxy.raw,
			neural.linear, neural.known ? "" : ", NOT DECODED", neural.raw,
			static_cast<double>(g_paper_white), static_cast<double>(scale),
			static_cast<double>(g_color_strength), static_cast<double>(g_transfer_strength),
			static_cast<double>(nrc::kSoftClipKnee),
			want_scale, can_suggest ? 1.0 / want_scale : -1.0);
	}

	if (!neural.known)
	{
		g_validation.store(Validation::failed, std::memory_order_release);
		STRAY_LOG_ERROR("NR: could not read back or decode the neural output (format %d), so NR "
			"stays OFF rather than risk a black frame. The SR/RR image is kept.",
			static_cast<int>(g_nr_format));
		return;
	}

	if (max_luma > kLumaFloor)
	{
		g_validation.store(Validation::ok, std::memory_order_release);
		STRAY_LOG_WARN("NR VALIDATED [site=%s]: neural output max luminance %.6f over the centre "
			"crop (> %.0e). The result will now reach the screen %s.",
			g_validate_codec ? "taa-dispatch" : "post-tonemap", max_luma, kLumaFloor,
			g_validate_codec ? "as a residual carried onto the SR/RR image"
				: "as a copy over the back-buffer image");
	}
	else
	{
		g_validation.store(Validation::failed, std::memory_order_release);
		STRAY_LOG_ERROR("NR DEGENERATE: the neural output is black (max luminance %.8f <= "
			"%.0e over the centre crop). Read the NR CODEC LUMINANCE line above before changing "
			"anything: it says whether the network was shown a usable image at all. NR stays OFF "
			"permanently this session; the SR/RR image is kept. Nothing black reached the "
			"screen.", max_luma, kLumaFloor);
	}
}

void shutdown()
{
	release_feature();
	release(g_crop_input.buffer);
	release(g_crop_proxy.buffer);
	release(g_crop_neural.buffer);
	nrp::shutdown();
	release(g_nr_output);
	g_guide_latch = nrplan::GuideExtentLatch{};
	// A snippet that was loaded but never initialised is a NORMAL, safe resting state — RenoDX
	// says as much in its own words ("nvngx_dlssnr.dll was loaded but never initialized;
	// leaving it mapped"). Say so calmly rather than treating it as a fault.
	if (g_runtime_loaded && !g_use_direct)
		STRAY_LOG_INFO("NR: the snippet was loaded but never initialised; leaving it mapped.");

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
