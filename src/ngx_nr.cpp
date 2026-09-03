#include "ngx_nr.hpp"

#include "ext_unhook.hpp"
#include "log.hpp"
#include "ngx_backend.hpp"
#include "ngx_snippet.hpp"

#include "core/nr_hook_plan.hpp"
#include "core/nr_lifetime.hpp"
#include "core/nr_params.hpp"

#include <d3d12.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

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
	// A teardown or a resolution change is waiting on the GPU. Frames in this state are normal
	// and brief; a rate that never falls means a fence that never advances.
	"recreating",
	// NO CODEC, NO EVALUATE (src/core/nr_hook_plan.hpp). The proxy IS the input contract of a
	// display-referred network, so a frame that cannot produce a correct one is declined rather
	// than handed raw HDR.
	"no-codec", "exposure-unknown", "degenerate-scale",
};

} // namespace stray_dlss::nr

#if !STRAY_DLSS_ENABLE_NGX

// NGX-less diagnostic build: the whole surface exists and does nothing.
namespace stray_dlss::nr {
namespace { char g_err[64] = "<ngx disabled>"; }
void set_enabled(bool) {}
bool enabled() { return false; }
void set_dll_path(const char *) {}
void set_tuning(float, float, float) {}
void set_renodx_tuning(float, unsigned int, unsigned int, unsigned int) {}
void set_style(unsigned int) {}
void set_mvec_scale_override(float) {}
bool preload() { return false; }
void set_warmup_frames(unsigned int) {}
bool apply(ID3D12Device *, ID3D12GraphicsCommandList *, const ApplyInputs &) { return false; }
void on_present(ID3D12CommandQueue *) {}
void shutdown() {}
const char *last_error() { return g_err; }
void counters(std::uint64_t &applied, std::uint64_t &refused, std::uint32_t out[kNrRefusalCount])
{
	applied = 0;
	refused = 0;
	for (int i = 0; i < kNrRefusalCount; ++i)
		out[i] = 0;
}
ResetCounts reset_counters() { return ResetCounts{}; }
bool validated() { return false; }
} // namespace stray_dlss::nr

#else

#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers.h>

namespace stray_dlss::nr {
namespace {

// Exact piecewise sRGB decode, the surviving fragment of the deleted HDR colour codec. The
// luminance diagnostic reports the neural output both as a display code value and decoded to
// linear, and the network answers in true sRGB rather than an x^2.2 approximation.
float srgb_decode_channel(float c)
{
	c = c < 0.0f ? 0.0f : (c > 1.0f ? 1.0f : c);
	if (c <= 0.04045f)
		return c / 12.92f;
	const float shoulder = (c + 0.055f) / 1.055f;
	return std::pow(shoulder > 0.0f ? shoulder : 0.0f, 2.4f);
}


// --- the DLSSNR.* parameter namespace (docs/RESEARCH-RENODX-DLSS5.md §2.2, HARD) ---
// The public SDK has no DLSSNR helper, so every key is set by name. Names are verbatim from
// the study's exhaustive string dump of both addon builds.
constexpr const char *kColor        = "DLSSNR.Color";
constexpr const char *kDepth        = "DLSSNR.Depth";
constexpr const char *kMVec         = "DLSSNR.MVec";
constexpr const char *kOutput       = "DLSSNR.Output";
// The subrect and extent names live in src/core/nr_params.hpp, together with their TYPES: the
// snippet reads them as signed int, `Set(name, 0u)` and `Set(name, 0)` are different virtual
// overloads, and an NGX parameter block reports no error for a mismatch. Pinned in CI.
constexpr const char *kMVecScaleX   = "DLSSNR.MVecScaleX";
constexpr const char *kMVecScaleY   = "DLSSNR.MVecScaleY";
constexpr const char *kDepthInverted= "DLSSNR.DepthInverted";
constexpr const char *kReset        = "DLSSNR.Reset";
constexpr const char *kIntensity    = "DLSSNR.Intensity";
constexpr const char *kLocalTone    = "DLSSNR.LocalToneStrength";
constexpr const char *kLocalStruct  = "DLSSNR.LocalStructureStrength";
// The remaining parameters RenoDX sets that we did not. Each one below is confirmed present
// in the runtime; the seven RenoDX names that are ABSENT from this build (InputWidth,
// InputHeight, OutputWidth, OutputHeight, Output.Width, Output.Height, Upscaling) are
// deliberately not set here, because writing dead names is how "Scale" hid for so long.
constexpr const char *kEnabled      = "DLSSNR.Enabled";
constexpr const char *kPreset       = "DLSSNR.Hint.Render.Preset";
constexpr const char *kSkinStruct   = "DLSSNR.SkinStructureStrength";
constexpr const char *kUseAutoMask  = "DLSSNR.UseAutoMask";
constexpr const char *kUICorrection = "DLSSNR.UICorrection";
// Confirmed present by exact string search over the 310.8.0 runtime (appears once, as a bare
// parameter name — docs/RESEARCH-DLSSNR-STYLES.md). NOT the same axis as kPreset: preset selects
// a different embedded weight set, Style is a small integer with no weight switch behind it.
constexpr const char *kStyle        = "DLSSNR.Style";

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
	kRefRecreating,
	kRefNoCodec,
	kRefExposureUnknown,
	kRefDegenerateScale,
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
// Defaults are RenoDX's own shipped [RenoDX.DLSS5] values, not invented ones: NRIntensity=1.05,
// NRLocalTone=1.74, NRSkinStructure=1.33, NRPreset=1, NRAutoMask=1, NRUICorrection=1.
float g_intensity = 1.0f;
float g_local_tone = 1.74f;
float g_local_structure = 1.0f;
float g_skin_structure = 1.33f;
unsigned int g_preset = 1;
unsigned int g_auto_mask = 1;
unsigned int g_ui_correction = 1;
// 0 = "Default (standard)" in the community naming (docs/RESEARCH-DLSSNR-STYLES.md). We never
// wrote DLSSNR.Style before this knob existed, so 0 reproduces that behaviour exactly rather
// than picking a value that merely looks neutral.
unsigned int g_style = 0;
float g_mvec_scale_override = 0.0f;
// --- HDR colour codec tuning (ngx_nr.hpp documents each; math in core/nr_codec.hpp) ---
// 1.0 is deliberately NOT copied from the reference tree's default: that default exists only
// because the reference multiplies it by its tonemapper's auto-exposure texture, which we have
// no access to at a TAA-dispatch hook. It is 1.0 here because it is neutral and because the
// direction this title needs is unknown until the codec's own luminance line is read once.
// used raw: NR's own temporal history is accumulated at whatever scale was in force, so a scale
// that jitters frame to frame leaves that history in the wrong units.
// The codec scale NR's current temporal history was accumulated at, and how far it may drift
// Default ON, matching the reference's own `trackAutoExposure` default. See ngx_nr.hpp.
// NR's OWN temporal accumulation is keyed on the COLOUR grid, so nothing in the feature notices
// when the GUIDE grid moves underneath it — and ours moves whenever the screen percentage does
// (1920x1080 guides at 50%, 2688x1512 at 70%, both of which this project runs). The reference
// deployment latches the guide extent and forces a single reset frame when it changes; without
// that, MVecScale silently goes from 2.0 to 1.42857 under a history accumulated against the old
// grid. Rule and provenance: src/core/nr_hook_plan.hpp.
nrplan::GuideExtentLatch g_guide_latch;
// Any frame NR declines before the evaluate leaves a hole in feature 18's own temporal
// continuity, so the next evaluate carries DLSSNR.Reset. (src/core/nr_hook_plan.hpp)
nrplan::EvaluateGapLatch g_gap_latch;
// Whether an evaluate actually reached the network in the frame currently being recorded. The
// refusal path can only see frames NR was ASKED about; this is what lets the present boundary see
// the frames it was never asked about at all (a TAA dispatch that did not match, an SR/RR
// evaluate that failed, NgxNR toggled off and back on with the feature kept).
// nrplan::note_frame_boundary carries the rule.
bool g_evaluated_this_frame = false;
// A FRESH FEATURE HAS NO HISTORY, whatever the frame thinks about continuity. Covered accidentally
// today — the frames before a create are refused as warmup or recreating, and those arm the gap
// latch — but only accidentally: NgxNRWarmupFrames=0 creates the feature on the first apply() with
// nothing having been refused before it. The sibling port states this as its own first case
// ("a feature that was just created has no history"), so state it here rather than rely on the
// coincidence. (Kim2091/dxvk-remix @ gta4-atmos-dlss5, rtx_neural_uplift.cpp:145)
bool g_new_feature_reset = false;

// WHY EACH DLSSNR.Reset HAPPENED, counted per source. Not decoration: a reset discards feature
// 18's whole accumulation, so a reset source that fires often is itself a flicker source — and
// CLAUDE.md has already been caught once by exactly that ("a reset latch is right for a rare
// discrete change and wrong for a quantity that varies continuously — there it is a metronome",
// measured at 52 fires making the image worse). Whether our shipped NgxNRExposureSmoothing=0.05
// plus NgxNRScaleResetTolerance=0.15 is a metronome or a rarity is a question ONE log line
// answers and no amount of argument does. Build the counter before you need it.
std::atomic<std::uint32_t> g_reset_frame_gap{0};
std::atomic<std::uint32_t> g_reset_guide_grid{0};
std::atomic<std::uint32_t> g_reset_camera_cut{0};
std::atomic<std::uint32_t> g_reset_new_feature{0};
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
CropReadback g_crop_neural; // what it answered
// The crop's own extent, for the diagnostic line. Written on the recording thread before the
// release-store that publishes `in_flight`, read on the present thread after the acquire-load —
// same handoff as the readback fields themselves.
std::uint32_t g_validate_crop_w = 0, g_validate_crop_h = 0;
// Whether the frame that armed the validation ran the HDR codec. Post-tonemap there is no proxy
// and no paper white, so the codec half of the luminance line would be a lie — and a diagnostic
// that lies is worse than one that is absent. Same publication discipline as the fields above.
int g_validate_presents_left = 0;

std::atomic<std::uint64_t> g_applied{ 0 };
std::atomic<std::uint64_t> g_refused{ 0 };
std::atomic<std::uint32_t> g_refusals[kNrRefusalCount] = {};
bool g_refusal_logged[kNrRefusalCount] = {};

// --- THE GPU TIMELINE (src/core/nr_lifetime.hpp) ---
//
// Everything NR owns is bound into command lists the GPU executes later, so nothing may be
// destroyed on the thread that decided to stop using it. One fence on the swapchain's queue,
// signalled once per present, is what separates "the CPU is done with this" from "the GPU is done
// with this". Every deferred free and ReleaseFeature itself is decided against it.
nrlife::Timeline g_timeline;
ID3D12Fence *g_fence = nullptr;
// A fence that could not be created is a diagnosis, not a retry loop: fall back to the
// conservative present ring for the rest of the session and say so once.
bool g_fence_failed = false;
// The signal value covering the last evaluate that referenced the feature and its resources.
nrlife::Tag g_last_eval_tag;

// Objects retired but not yet freed. IUnknown is enough: everything in here is released, never
// used, and the type only matters for the log line.
struct Grave
{
	IUnknown *obj = nullptr;
	nrlife::Tag tag;
	const char *what = "";
};
std::vector<Grave> g_graves;

// A resolution change or an NgxNR 1->0. Both mean "stop using feature 18", and both are answered
// at the present boundary rather than where they were noticed.
bool g_release_feature_requested = false; // recreate: the feature goes, everything else stays
bool g_teardown_requested = false;        // NgxNR 1->0: give the whole working set back

// Keep NR's inputs and output alive past GPU execution, exactly as the SR/RR paths do: NGX
// holds no references and EvaluateFeature only RECORDS work. (CLAUDE.md §5)
//
// Tagged against the fence rather than counted in frames. The old six-evaluate rule was a guess
// at how far behind the GPU can be; the fence is the answer.
constexpr std::size_t kKeepAliveSlots = 16;
struct KeepAlive
{
	ID3D12Resource *resources[4] = {};
	nrlife::Tag tag;
};
KeepAlive g_keep_alive[kKeepAliveSlots];
std::size_t g_keep_alive_count = 0;

template <typename T>
void release(T *&p)
{
	if (p != nullptr)
	{
		p->Release();
		p = nullptr;
	}
}

// Writes a built parameter block, honouring each entry's declared TYPE. The `int` and
// `unsigned int` overloads of NVSDK_NGX_Parameter::Set are different virtual functions
// (nvsdk_ngx_params.h:58-59), so this switch is the whole point of the builder existing.
void apply_entries(NVSDK_NGX_Parameter *params, const nrparam::Entry *entries, int count)
{
	if (params == nullptr || entries == nullptr)
		return;
	for (int i = 0; i < count; ++i)
	{
		const nrparam::Entry &e = entries[i];
		switch (e.type)
		{
		case nrparam::Type::i32: params->Set(e.name, e.i); break;
		case nrparam::Type::u32: params->Set(e.name, e.u); break;
		case nrparam::Type::f32: params->Set(e.name, e.f); break;
		}
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

// A refusal that happens BEFORE the evaluate. Feature 18 keeps its own temporal accumulation and
// reprojects it with motion vectors describing exactly one frame of motion, so a frame it never
// saw is a hole in that continuity — the next evaluate must not reproject across it. Refusals
// that happen AFTER a successful evaluate (validating, degenerate-output) go through plain
// refuse(): those frames DID reach the network, and forcing a reset for them would discard the
// accumulation on every frame of the validation window.
bool refuse_pre_evaluate(int reason, const char *fmt_msg)
{
	nrplan::note_evaluate_gap(g_gap_latch);
	return refuse(reason, fmt_msg);
}

void set_error(const char *what, NVSDK_NGX_Result result)
{
	std::snprintf(g_last_error, sizeof(g_last_error), "%s: 0x%08x (%s)", what,
		static_cast<unsigned int>(result), ngx::result_name(static_cast<unsigned int>(result)));
	STRAY_LOG_ERROR("NR %s", g_last_error);
}

// Hands `obj` to the graveyard, tagged against the current timeline. DESTROYS NOTHING — that is
// the entire point. Takes ownership of the caller's reference and nulls it.
template <typename T>
void bury(T *&obj, const char *what)
{
	if (obj == nullptr)
		return;
	Grave g;
	g.obj = static_cast<IUnknown *>(obj);
	g.tag = nrlife::tag_now(g_timeline);
	g.what = what;
	g_graves.push_back(g);
	obj = nullptr;
}

// Frees whatever the timeline says the GPU has passed. PRESENT BOUNDARY ONLY.
void collect_graves()
{
	std::size_t kept = 0;
	for (std::size_t i = 0; i < g_graves.size(); ++i)
	{
		if (nrlife::safe_to_free(g_timeline, g_graves[i].tag))
		{
			g_graves[i].obj->Release();
			continue;
		}
		g_graves[kept++] = g_graves[i];
	}
	g_graves.resize(kept);
}

void retire_keep_alive(bool all)
{
	std::size_t kept = 0;
	for (std::size_t i = 0; i < g_keep_alive_count; ++i)
	{
		KeepAlive &ka = g_keep_alive[i];
		if (all || nrlife::safe_to_free(g_timeline, ka.tag))
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

// RELEASES FEATURE 18 NOW. Only legal once the queue has completed the last evaluate that used
// it, and only at the present boundary — nrlife::feature_release_ready is the gate, and every
// caller below goes through it. The reason is the sibling port's second defect verbatim: "the
// feature owns GPU resources DXVK cannot see and therefore cannot keep alive". Neither can we:
// our AddRefs cover the textures we pass in, not whatever the snippet allocated inside itself.
//
// `why` is logged with the fence values, so a live run can prove the ordering held rather than
// asking anyone to take it on trust.
void release_feature_now(const char *why)
{
	if (g_feature != nullptr || g_params != nullptr)
		STRAY_LOG_WARN("NR: releasing feature 18 (%s) at the PRESENT boundary. Last evaluate was "
			"tagged fence=%llu/present=%llu; the queue has completed %llu and presented %llu, so "
			"nothing that referenced the feature can still be executing. (If the completed value "
			"is below the tag in this line, the ordering gate is broken — that is the whole thing "
			"it exists to guarantee.)",
			why, static_cast<unsigned long long>(g_last_eval_tag.fence),
			static_cast<unsigned long long>(g_last_eval_tag.present),
			static_cast<unsigned long long>(g_timeline.completed),
			static_cast<unsigned long long>(g_timeline.present));

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
	g_release_feature_requested = false;
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

	// A LIVE FEATURE FOR A DIFFERENT SIZE IS NOT OURS TO DESTROY HERE. This runs on the recording
	// thread, inside the intercepted TAA dispatch, with the previous frames' evaluates very
	// possibly still executing. Ask for the release, decline this frame, and create the new
	// feature once the present boundary has actually done it. A resolution change costs a handful
	// of NR-less frames; releasing a feature under the GPU costs the session.
	if (g_feature != nullptr || g_params != nullptr)
	{
		if (!g_release_feature_requested)
		{
			g_release_feature_requested = true;
			STRAY_LOG_WARN("NR: the feature's rects moved (%ux%u -> %ux%u in, %ux%u -> %ux%u "
				"out). Release is DEFERRED to the present boundary and NR declines until then.",
				g_feature_render_w, g_feature_render_h, render_w, render_h,
				g_feature_out_w, g_feature_out_h, out_w, out_h);
		}
		return false;
	}

	NVSDK_NGX_Result result = nr_alloc_params(&g_params);
	if (NVSDK_NGX_FAILED(result) || g_params == nullptr)
	{
		set_error("AllocateParameters", result);
		g_create_latched = true;
		return false;
	}

	// The network works at OUTPUT resolution: Color is already upscaled when it reaches us, so
	// Width/Height are the output rect and the scaling ratio is 1. (DLSSNR.ScalingRatio is inert
	// in this runtime anyway: read, then overwritten with 1.0f.)
	const std::uint32_t in_w = out_w;
	const std::uint32_t in_h = out_h;

	{
		nrparam::Entry entries[nrparam::kMaxCreateEntries];
		const int n = nrparam::build_create(in_w, in_h,
			out_w > 0 && in_w > 0 ? static_cast<float>(out_w) / static_cast<float>(in_w) : 1.0f,
			entries, nrparam::kMaxCreateEntries);
		apply_entries(g_params, entries, n);
	}
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
	// See ngx_nr.hpp's set_style comment: community-tested labels only, not names the runtime
	// itself ships. Changing this away from 0 forces a one-frame history reset in the snippet.
	g_params->Set(kStyle, g_style);

	STRAY_LOG_WARN("NR: creating NGX feature 18 (Reserved18 / DLSSNR) %ux%u -> %ux%u, "
		"intensity=%.2f localTone=%.2f localStructure=%.2f skinStructure=%.2f "
		"preset=%u style=%u autoMask=%u uiCorrection=%u scalingRatio=%.3f...",
		in_w, in_h, out_w, out_h,
		g_intensity, g_local_tone, g_local_structure, g_skin_structure,
		g_preset, g_style, g_auto_mask, g_ui_correction,
		out_w > 0 && in_w > 0 ? static_cast<double>(out_w) / static_cast<double>(in_w) : 1.0);

	result = nr_create_feature(cmd, g_params, &g_feature);
	if (NVSDK_NGX_FAILED(result) || g_feature == nullptr)
	{
		set_error("CreateFeature(18)", result);
		STRAY_LOG_ERROR("  NR feature-18 create failed and is LATCHED off for this session. "
			"FAIL_FeatureNotFound/NotSupported here means the loaded nvngx_dlssnr.dll does not "
			"provide the reserved slot (wrong build, or the Ada patch this 4090 needs is "
			"missing); the SR/RR image is unaffected.");
		// Immediate, and safe precisely because there is no feature: CreateFeature returned
		// nothing, so nothing was ever evaluated against this parameter block and no command
		// list holds a handle to it.
		release_feature_now("create failed");
		g_create_latched = true;
		return false;
	}

	g_feature_render_w = render_w;
	g_feature_render_h = render_h;
	g_feature_out_w = out_w;
	g_feature_out_h = out_h;
	g_last_error[0] = 0;
	// A feature this new has no history behind it, whatever the frame thinks about continuity.
	g_new_feature_reset = true;
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

	// NEVER a straight Release here. This runs on the recording thread, and the outgoing texture
	// is DLSSNR.Output for every evaluate still in flight while the readback buffers are the
	// destinations of CopyTextureRegions that may not have executed yet. Both go to the
	// graveyard and are freed at a present the fence has passed.
	bury(g_nr_output, "neural output texture (resolution change)");
	bury(g_crop_input.buffer, "validation crop: colour input");
	bury(g_crop_neural.buffer, "validation crop: neural output");
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
                      ID3D12Resource *image, D3D12_RESOURCE_STATES image_state)
{
	if (g_validation.load(std::memory_order_acquire) != Validation::pending ||
		g_nr_output == nullptr || image == nullptr)
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
	g_validate_crop_w = crop_w;
	g_validate_crop_h = crop_h;
	g_validate_presents_left = kValidateLatency;
	// Publish LAST: everything above must be visible to the present thread that sees this.
	g_validation.store(Validation::in_flight, std::memory_order_release);
	STRAY_LOG_INFO("NR: validating the neural output (%ux%u crop centred on the %ux%u processed "
		"rect, with a matching colour-input crop). Until it passes, NR runs but the "
		"write-back does NOT touch the screen - a degenerate runtime cannot show a black frame.",
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
	// THIS RUNS ON WHATEVER THREAD FLIPPED THE KEY — the overlay checkbox, an ini reload — and it
	// must therefore free nothing. 1 -> 0 QUEUES a teardown that on_present() performs once the
	// fence has passed the last evaluate; 0 -> 1 before that teardown has run simply cancels it,
	// so a toggle loop keeps the working set it already has. The sibling port crashed on exactly
	// this transition by releasing on the spot (dxvk-remix @ a69254ab).
	if (g_enabled && !value)
	{
		g_teardown_requested = true;
		STRAY_LOG_WARN("NR DISABLED ([STRAYDLSS] NgxNR=0): teardown QUEUED. Nothing is destroyed "
			"on this thread; feature 18, the neural output texture and the codec's proxy are "
			"released at a present whose fence has passed the last evaluate.");
	}
	else if (!g_enabled && value && g_teardown_requested)
	{
		g_teardown_requested = false;
		STRAY_LOG_WARN("NR RE-ENABLED before its queued teardown ran; the teardown is cancelled "
			"and the existing feature and textures are kept.");
	}
	g_enabled = value;
	if (value)
		STRAY_LOG_WARN("NR ENABLED ([STRAYDLSS] NgxNR=1): DLSS Neural Rendering (NGX feature "
			"18 / DLSSNR) will run after the SR/RR evaluate. EXPERIMENTAL — the runtime is a "
			"leaked pre-release DLL; a degenerate result is caught by validation and NR simply "
			"stays off.");
}

bool enabled() { return g_enabled; }
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

void set_style(unsigned int style) { g_style = style; }

void counters(std::uint64_t &applied, std::uint64_t &refused, std::uint32_t out[kNrRefusalCount])
{
	applied = g_applied.load(std::memory_order_relaxed);
	refused = g_refused.load(std::memory_order_relaxed);
	for (int i = 0; i < kNrRefusalCount; ++i)
		out[i] = g_refusals[i].load(std::memory_order_relaxed);
}

ResetCounts reset_counters()
{
	ResetCounts c;
	c.frame_gap = g_reset_frame_gap.load(std::memory_order_relaxed);
	c.guide_grid = g_reset_guide_grid.load(std::memory_order_relaxed);
	c.camera_cut = g_reset_camera_cut.load(std::memory_order_relaxed);
	c.new_feature = g_reset_new_feature.load(std::memory_order_relaxed);
	return c;
}

void set_warmup_frames(unsigned int frames) { g_warmup_frames = frames; }




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
		return refuse_pre_evaluate(kRefWarmup, "waiting out the NR warmup window before initialising the "
			"runtime.");

	// FIRST USE: load (if not pre-loaded) and initialise, exactly once. Both halves latch, so
	// a failure never retries per frame.
	if (!g_load_tried)
		load_snippet_once();
	if (!g_runtime_loaded)
		return refuse_pre_evaluate(kRefDllMissing, "nvngx_dlssnr.dll was never loaded.");
	if (!g_init_tried)
	{
		STRAY_LOG_WARN("NR: warmup complete (%llu SR evaluates) — initialising the NR runtime "
			"now (lazy, first use).",
			static_cast<unsigned long long>(g_sr_evaluates_seen));
		init_snippet_once(device);
	}

	if (device == nullptr || cmd == nullptr || in.image == nullptr || in.depth == nullptr ||
		in.motion_vectors == nullptr || in.render_width == 0 || in.output_width == 0)
		return refuse_pre_evaluate(kRefBadInputs, "a required resource or dimension was missing.");

	// A queued teardown or feature release owns the working set until the present boundary has
	// carried it out. Evaluating into resources that are on their way to the graveyard is exactly
	// the use-after-free this whole path exists to prevent, so decline and count it.
	if (g_teardown_requested || g_release_feature_requested)
		return refuse_pre_evaluate(kRefRecreating,
			"a teardown or a feature release is queued and is waiting on the GPU fence.");

	// The pass's deferred frees are decided against the same timeline this module owns.

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

	// WHY THERE IS NO BLANKET `if (!codec) refuse` HERE, and why there WAS one between 2026-09-02
	// and 2026-09-02.
	//
	// The rule the codec gate enforces is "a DISPLAY-REFERRED network must not be handed an
	// out-of-domain image". At the TAA site the image is raw linear HDR, so the codec IS the input
	// contract and skipping it is the failure this project spent a session diagnosing (a neural
	// output whose max luminance read 0.0026, red noise on screen). At the present stage the image
	// is Stray's back buffer — R10G10B10A2_UNORM with no SetColorSpace1 call anywhere, i.e. SDR
	// display-encoded (docs/STRAY-RENDERING-FACTS.md §32) — which is ALREADY the network's domain,
	// and encoding it again would apply the transfer twice. Same rule, opposite action.
	//
	// While the post-tonemap sites did not exist, refusing every non-codec call was the correct
	// way to keep a future caller from walking into the raw-HDR path. Now that exactly one such
	// caller exists and is display-referred by construction, the refusal would refuse the correct
	// configuration. nrplan::codec_gate still enumerates every way the CODEC site can reach the
	// evaluate without a proxy, and is consulted below.

	// The residual needs the proxy, the neural answer and the original to be the SAME pixels.
	// sr-shaped puts Color at render resolution and Output at display resolution, so there is no
	// per-pixel correspondence to subtract across and no residual exists to carry. It is equally
	// meaningless post-tonemap, where there is nothing left to upscale and the whole image is one
	// rect. Refuse loudly rather than fall back to the raw-HDR path, which is the exact
	// configuration that produced red noise and a 0.0026 neural output (src/core/nr_codec.hpp).
	ID3D12Resource *colour = in.image;

	// The state `image` arrives in and must be left in. Documented in ngx_nr.hpp's Site enum and
	// enforced by the caller on both sides; the barriers below and the validation crop both
	// depend on getting it right.
	const D3D12_RESOURCE_STATES image_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

	// A mipped / arrayed / multisampled colour input is the one hazard we cannot fix by
	// allocating our own texture, and feeding one is a documented DXGI_ERROR_DEVICE_HUNG rather
	// than an error return. Refuse it loudly: a log line costs a frame, a hung GPU costs the
	// user a power cycle. (CLAUDE.md §0.2 — prefer a loud failure.)
	{
		const D3D12_RESOURCE_DESC cd = colour->GetDesc();
		if (cd.MipLevels != 1 || cd.DepthOrArraySize != 1 || cd.SampleDesc.Count != 1)
			return refuse_pre_evaluate(kRefMippedInput,
				"the colour input is not a plain single-mip, single-slice, non-MSAA 2D texture; "
				"handing one to the neural runtime hangs the GPU instead of returning an error.");
	}

	// The result goes to OUR texture, never straight over the engine's output.
	if (!ensure_output_texture(device, in.image))
		return refuse_pre_evaluate(kRefAllocFailed, g_last_error);
	if (!ensure_feature(cmd, in.render_width, in.render_height, in.output_width,
			in.output_height))
	{
		if (g_release_feature_requested)
			return refuse_pre_evaluate(kRefRecreating,
				"the feature's rects moved; its release is deferred to the present boundary.");
		return refuse_pre_evaluate(kRefCreateFailed, "feature 18 could not be created.");
	}

	ext_unhook::repair();

	// Colour rect: the output rect, since post-process consumes the already-upscaled image. At a
	// post-tonemap site that IS the back buffer's rect, which the caller passes as output_*.
	const std::uint32_t cw = in.output_width;
	const std::uint32_t ch = in.output_height;
	g_codec_rect_w = cw;
	g_codec_rect_h = ch;


	// Motion vectors are ours: dense RG16_FLOAT in RENDER-resolution pixels, y-down.
	//
	// SCALE 1.0, NOT the colour/guide ratio — MEASURED on the user's machine 2026-09-01, which
	// overturns a code-reading argument I had recorded as settled the same day.
	//
	// The argument for the ratio was good: dxvk-remix's NGXNeuralRenderingContext sets
	// colourExtent/guideExtent and its comment names our exact 4K-colour-over-1080p-guides case,
	// and a comment in its NGX wrapper states the snippet receives the colour rect, the mvec rect
	// and this scale as three INDEPENDENT values. But that last claim is from reverse
	// engineering, not from a header or a measurement — and we already declare the guides' own
	// rect through DLSSNR.MVecSubrectWidth/Height, so if the runtime normalises by the subrect
	// then the ratio is applied twice. A/B'd live while moving: 1.0 is visibly more stable.
	//
	// Keep this in mind before "fixing" it back: an argument from someone else's source, however
	// well cited, loses to a measurement on the machine that actually runs the code.
	float scale_x = 1.0f, scale_y = 1.0f;
	if (g_mvec_scale_override > 0.0f)
		scale_x = scale_y = g_mvec_scale_override;

	// FEATURE 18 KEEPS ITS OWN TEMPORAL ACCUMULATION, and it is keyed on the colour grid alone.
	// Moving the guide grid underneath it — which a screen-percentage change does, at a fixed
	// output resolution, taking MVecScale from 2.0 to 1.42857 with it — invalidates a history
	// nothing else in the pipeline notices. Latch the extent and force ONE reset frame when it
	// moves, exactly as the reference deployment does. (src/core/nr_hook_plan.hpp)
	bool reset = in.reset;
	if (reset)
		g_reset_camera_cut.fetch_add(1, std::memory_order_relaxed);

	// A FRESH FEATURE HAS NO HISTORY. Set where the feature is created, taken here, once.
	if (g_new_feature_reset)
	{
		g_new_feature_reset = false;
		reset = true;
		g_reset_new_feature.fetch_add(1, std::memory_order_relaxed);
	}

	// A FRAME NR DECLINED IS A HOLE IN FEATURE 18'S OWN TEMPORAL CONTINUITY. It reprojects its
	// accumulation with motion vectors that describe exactly one frame of motion, so reprojecting
	// across a gap fetches history from the wrong place for every pixel that moved — the same
	// class of error as a wrong MVecScale, and it compounds through the accumulation rather than
	// costing one frame (CLAUDE.md, "bad motion vectors do not produce one bad frame"). Taken
	// exactly once, so a run of declined frames costs one reset and not one per frame.
	if (nrplan::take_evaluate_reset(g_gap_latch))
	{
		reset = true;
		g_reset_frame_gap.fetch_add(1, std::memory_order_relaxed);
		static bool s_gap_logged = false;
		if (!s_gap_logged)
		{
			s_gap_logged = true;
			STRAY_LOG_INFO("NR: the previous frame(s) were declined, so this evaluate carries "
				"DLSSNR.Reset — feature 18's accumulation must not be reprojected across a gap. "
				"First occurrence only; the periodic NR line carries the refusal rate by reason, "
				"which is what says whether this is happening constantly.");
		}
	}

	// THE CODEC SCALE DEFINES THE UNITS NR'S HISTORY IS ACCUMULATED IN, so a scale change
	// invalidates that history exactly as a guide-grid change does — and just as silently.
	// Smoothing damps the jitter but cannot remove this: it makes the mismatch gradual rather
	// than absent, which is why a static camera recovers quickly while MOVEMENT — where the
	// engine's exposure genuinely swings — keeps re-triggering it. Measured live: the exposure
	// factor moved 6.68 -> 7.37 between two readings in one session.
	//
	// So latch the scale and force ONE reset when it drifts past the tolerance, the same shape
	// as the guide-extent latch below. With smoothing on, this should fire rarely.

	if (nrplan::latch_guide_extent(g_guide_latch, in.render_width, in.render_height))
	{
		reset = true;
		g_reset_guide_grid.fetch_add(1, std::memory_order_relaxed);
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

	// The four rects, through the builder that carries their TYPES. Every one of these used to be
	// written through the `unsigned int` overload while the snippet reads them as `int` — a
	// different vtable slot and a different stored type, with no error either way.
	// (src/core/nr_params.hpp, tests/test_nr_params.cpp)
	{
		nrparam::Rects rects;
		rects.color_width = cw;
		rects.color_height = ch;
		rects.guide_width = in.render_width;
		rects.guide_height = in.render_height;
		rects.output_width = in.output_width;
		rects.output_height = in.output_height;
		nrparam::Entry entries[nrparam::kMaxRectEntries];
		const int n = nrparam::build_rects(rects, entries, nrparam::kMaxRectEntries);
		apply_entries(g_params, entries, n);
	}

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
	g_params->Set(kStyle, g_style);

	// One line per SITE, not one per session: `taa` and a post-tonemap mode describe genuinely
	// different pixels, so a session that switched between them must not report only the first.
	static bool s_params_logged = false;
	if (!s_params_logged)
	{
		s_params_logged = true;
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

	// Tagged BEFORE the call, not after: EvaluateFeature records work whether or not it returns
	// success, so a failed evaluate can still have left references on the command list. The
	// feature-release gate reads this tag, and it must never name a moment earlier than the last
	// thing that touched the feature.
	g_last_eval_tag = nrlife::tag_now(g_timeline);

	const NVSDK_NGX_Result result =
		nr_evaluate_feature(cmd, g_feature, g_params);

	// UNCONDITIONALLY, and before the result is even looked at: barriers are recorded, so an
	// early return here would leave the command list describing a proxy in
	// NON_PIXEL_SHADER_RESOURCE while the next frame's encode transitions it out of
	// UNORDERED_ACCESS. That mismatch is a validation error on WARP and undefined behaviour on
	// the target, where there is no debug layer to say so.

	if (NVSDK_NGX_FAILED(result))
	{
		set_error("EvaluateFeature(18)", result);
		return refuse_pre_evaluate(kRefEvaluateFailed, g_last_error);
	}

	// THE NETWORK SAW THIS FRAME, so its accumulation advanced by exactly one frame of motion and
	// the present boundary must not treat this frame as a hole. Set here rather than at the end of
	// apply(): the refusals BELOW this point (validating, degenerate output) happen AFTER a
	// successful evaluate, and forcing a reset for those would discard the accumulation on every
	// frame of the validation window — which is the reasoning refuse() already documents.
	g_evaluated_this_frame = true;

	// Hold everything NGX touched alive past GPU execution, under the tag taken above.
	if (g_keep_alive_count >= sizeof(g_keep_alive) / sizeof(g_keep_alive[0]))
	{
		// Not a leak — a stall. The slots only fill when the fence stops advancing, which means
		// presents have stopped reaching us, and the frames that overflow hold NO reference at
		// all. Say so once rather than silently skipping the AddRef, which is what this did
		// before it was tagged against a timeline.
		static bool s_warned = false;
		if (!s_warned)
		{
			s_warned = true;
			STRAY_LOG_ERROR("NR: the keep-alive ring is full (%u slots) — the GPU fence is not "
				"advancing, so nothing can be retired and this frame's inputs are NOT held. "
				"Check that nr::on_present is being called with the swapchain's queue.",
				static_cast<unsigned int>(kKeepAliveSlots));
		}
	}
	else
	{
		KeepAlive &ka = g_keep_alive[g_keep_alive_count++];
		ka.tag = g_last_eval_tag;
		ka.resources[0] = colour;
		ka.resources[1] = in.depth;
		ka.resources[2] = in.motion_vectors;
		ka.resources[3] = g_nr_output;
		for (ID3D12Resource *r : ka.resources)
			if (r != nullptr)
				r->AddRef();
	}
	// The retirement itself happens at the present boundary, with everything else. Dropping the
	// last reference to a texture is a destruction like any other, and this is the recording
	// thread.

	// Until validation passes, NR must not reach the screen.
	if (g_validation.load(std::memory_order_acquire) != Validation::ok)
	{
		if (g_validation.load(std::memory_order_acquire) == Validation::pending)
			begin_validation(device, cmd, in.image, image_state);
		if (g_validation.load(std::memory_order_acquire) == Validation::failed)
			return refuse(kRefDegenerate,
				"the neural output validated as black/degenerate.");
		return refuse(kRefValidating, "the neural output is still being validated.");
	}

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
	static bool s_applied_logged = false;
	if (!s_applied_logged)
	{
		s_applied_logged = true;
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
							srgb_decode_channel(static_cast<float>(c[i])));
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

namespace {

// Advances the GPU timeline: signal our own fence on the swapchain's queue and read back how far
// the GPU has actually got. Everything freed this present is decided from the result.
//
// The fence is ours, monotonic and used for nothing else, so signalling a queue we do not own is
// both legal and side-effect free. Signalling HERE means the value covers every command list the
// game submitted this frame, which is exactly the set that can reference what we are about to
// free.
void advance_timeline(ID3D12CommandQueue *queue)
{
	if (queue == nullptr || g_fence_failed)
	{
		nrlife::on_present_unfenced(g_timeline);
		return;
	}

	if (g_fence == nullptr)
	{
		ID3D12Device *device = nullptr;
		if (FAILED(queue->GetDevice(IID_PPV_ARGS(&device))) || device == nullptr ||
			FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence))))
		{
			g_fence_failed = true;
			release(device);
			STRAY_LOG_ERROR("NR: could not create the lifetime fence, so deferred frees fall back "
				"to a %u-present ring. That is conservative and correct, but it cannot see a GPU "
				"that has fallen further behind than that.",
				static_cast<unsigned int>(nrlife::kNoFenceFreePresents));
			nrlife::on_present_unfenced(g_timeline);
			return;
		}
		release(device);
		STRAY_LOG_INFO("NR: lifetime fence created on the swapchain queue. Every deferred free "
			"and ReleaseFeature from here is gated on it.");
	}

	if (FAILED(queue->Signal(g_fence, nrlife::pending_signal(g_timeline))))
	{
		// A failed signal means the value will never complete, so DO NOT advance the timeline
		// past it — that would free work the GPU may still be running.
		nrlife::on_present_unfenced(g_timeline);
		return;
	}
	nrlife::on_signalled(g_timeline, g_fence->GetCompletedValue());
}

} // namespace

void on_present(ID3D12CommandQueue *queue)
{
	// NOT gated on g_enabled: disabling NR is what QUEUES the teardown, so a disabled NR is
	// precisely the state in which this has the most to do. It IS gated on there being anything
	// to manage, so a session with NgxNR=0 from the start never creates a fence and never
	// signals the game's queue — ngx_nr.hpp promises that configuration is byte-identical to
	// before, and an unconditional per-present Signal would quietly break that promise.
	// THE FRAME BOUNDARY, and it is ABOVE the early-out on purpose.
	//
	// Feature 18 reprojects its own accumulation with motion vectors describing exactly ONE frame
	// of motion, so every frame it did not see is a hole. refuse_pre_evaluate() covers the frames
	// NR was asked about and declined; it structurally cannot cover the frames it was never asked
	// about — a TAA dispatch that did not match, an SR/RR evaluate that failed, or NgxNR toggled
	// off and back on, which deliberately KEEPS the feature and its history (set_enabled). Those
	// are not hypothetical: CLAUDE.md measures evaluates tracking dispatches at 99.7%.
	//
	// Two CPU bools and no GPU work, so this does NOT break ngx_nr.hpp's promise that NgxNR=0 is
	// byte-identical: no fence is created and the game's queue is never signalled.
	// (src/core/nr_hook_plan.hpp, note_frame_boundary)
	nrplan::note_frame_boundary(g_gap_latch, g_evaluated_this_frame);
	g_evaluated_this_frame = false;

	const bool anything_to_do = g_enabled || g_teardown_requested ||
		g_release_feature_requested || !g_graves.empty() || g_keep_alive_count != 0 ||
		g_fence != nullptr;
	if (!anything_to_do)
		return;

	advance_timeline(queue);

	// Everything the GPU has passed, in one place and on one thread.
	retire_keep_alive(/*all=*/false);
	collect_graves();

	// FEATURE 18'S RELEASE. Gated on both halves: the present boundary (we are on it) and the
	// queue having completed the last evaluate. See nrlife::feature_release_ready.
	if (nrlife::feature_release_ready(g_release_feature_requested || g_teardown_requested,
			/*at_present=*/true, g_timeline, g_last_eval_tag))
	{
		const bool teardown = g_teardown_requested;
		release_feature_now(teardown ? "NgxNR=0 teardown" : "rect change");
		if (teardown)
		{
			// The whole working set goes back, not just the feature: the neural output texture,
			// the validation readbacks and the codec's proxy, heap and pipelines. All of it via
			// the graveyard, so a frame recorded moments ago is still safe.
			bury(g_nr_output, "neural output texture (teardown)");
			bury(g_crop_input.buffer, "validation crop: colour input (teardown)");
			bury(g_crop_neural.buffer, "validation crop: neural output (teardown)");
			g_nr_width = g_nr_height = 0;
			g_nr_format = DXGI_FORMAT_UNKNOWN;
			g_validation.store(Validation::pending, std::memory_order_release);
			g_teardown_requested = false;
			STRAY_LOG_WARN("NR: teardown complete. Re-enabling NgxNR rebuilds the feature and the "
				"textures from scratch; the old ones are freed as their fences pass.");
		}
	}

	if (!g_enabled || g_validation.load(std::memory_order_acquire) != Validation::in_flight)
		return;
	if (--g_validate_presents_left > 0)
		return;

	const CropLuma neural = drain_crop(g_crop_neural);
	const CropLuma input = drain_crop(g_crop_input);
	const double max_luma = neural.raw;

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
			"post-tonemap", max_luma, kLumaFloor, "as a copy over the back-buffer image");
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
	// IMMEDIATE, and the only call site where that is correct: this runs from
	// DlssApp::on_device(created=false), i.e. the device is being destroyed and the caller has
	// already established that nothing is executing on it. Anything still in the graveyard goes
	// with it — there is no queue left to fence against.
	if (!g_graves.empty())
		STRAY_LOG_INFO("NR: shutdown with %u object(s) still waiting on the GPU timeline; the "
			"device is going away, so they are released here.",
			static_cast<unsigned int>(g_graves.size()));
	release_feature_now("shutdown");
	release(g_crop_input.buffer);
	release(g_crop_neural.buffer);
	release(g_nr_output);
	for (Grave &g : g_graves)
		g.obj->Release();
	g_graves.clear();
	release(g_fence);
	g_fence_failed = false;
	g_timeline = nrlife::Timeline{};
	g_last_eval_tag = nrlife::Tag{};
	g_release_feature_requested = false;
	g_teardown_requested = false;
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
