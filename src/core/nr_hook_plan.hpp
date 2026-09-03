// WHERE DLSS Neural Rendering is injected, whether a given frame may be injected into, and the
// guide grid its own temporal history was accumulated against. Pure; tested.
//
// WHY THIS IS A PURE MODULE. Every precondition of the present stage — the back buffer's shape,
// the freshness of the guides, the format's typed-UAV support — is a decision that can be made
// from plain numbers. Keeping them here means CI proves the refusal rules instead of the user's
// machine discovering them one round trip at a time (CLAUDE.md §0.4). src/nr_hook.cpp does
// nothing but gather those numbers from D3D12 and act on the verdict.
//
// HISTORY, because this file has been emptied once already. It carried a THREE-site choice
// (`taa` / `present` / `preui`) that was removed on 2026-09-02 when neither post-tonemap site
// produced a correct frame. What comes back here is not that: `preui` stays deleted, and
// `present` is now a STAGE on our own present-time command list rather than a ReShade event.
// The argument for the move, and what is different this time, is on HookMode below.
#pragma once

#include <cstdint>

namespace stray_dlss::nrplan {

// [STRAYDLSS] NgxNRHook. WHERE DLSS Neural Rendering runs.
//
// THE FIRST PROBLEM, MEASURED (CLAUDE.md, "NR's output feeds the engine's temporal history"). At
// the TAA site we write the engine's `u0`, and UE 4.27 makes that ONE resource serve two roles:
// `TemporalAA.cpp:696` is literally `NewHistoryTexture[0] = Outputs.SceneColor =
// NewHistoryTexture[0];` and `:969` extracts the same texture as the next frame's history. So
// NR's residual re-enters the engine's temporal state every frame and compounds. Screen-space
// reflections read that history directly (`ScreenSpaceRayTracing.cpp:596-620`) and the
// eye-adaptation histogram downsamples it (`PostProcessing.cpp:626-648`). `NgxNRRestoreHistory`
// exists only to undo that.
//
// THE SECOND PROBLEM, which is the one that motivated the move. The TAA site carries raw,
// unbounded, PRE-EXPOSED LINEAR HDR, and feature 18 is a display-referred network — so the whole
// HDR codec, `NgxNRPaperWhiteScale` and `NgxNRTrackExposure` with its smoothing and its
// scale-reset latch exist ONLY to put a linear signal into the network's domain. Every one is a
// knob that can be wrong, and the scale-reset latch is a DLSSNR.Reset source driven by a
// continuously varying quantity — which CLAUDE.md has already measured making an image worse
// ("there it is a metronome"). A sibling implementation reached the same rule from the other
// direction (Kim2091/dxvk-remix @ gta4-atmos-dlss5, 10fa0368): a display-encoded anchor is a
// CORRECTNESS REQUIREMENT rather than a tuning choice, and its shipping runtime carries no NR
// shaders at all — deleting the codec is what shipped there.
//
// AT A POST-TONEMAP SITE BOTH PROBLEMS ARE GONE BY CONSTRUCTION. Nothing after the tonemapper is
// carried into the next frame — every `QueueTextureExtraction` into `PrevFrameViewInfo` sits at
// `PostProcessing.cpp` 576/599/643 while `AddTonemapPass` is at 777 — and Stray's back buffer is
// `R10G10B10A2_UNORM` with no `SetColorSpace1` call anywhere, i.e. SDR display-encoded already
// (docs/STRAY-RENDERING-FACTS.md §33). That IS the network's own domain: no codec, no paper
// white, no exposure term.
//
// WHAT WAS TRIED BEFORE AND FAILED, so this is not a re-run of it. Two post-tonemap sites were
// built and removed on 2026-09-02: `preui` (the frame's Nth back-buffer render-target bind) died
// clobbering state the GAME's command list needed, and the old `present` rode
// `addon_event::reshade_begin_effects`, which never fires with an empty preset. Neither failure
// generalises to the present STAGE restored here. It records on the present owner's OWN command
// list (src/backend_native/present_owner.hpp — the one frame generation already drives every
// frame, which survives ResizeBuffers and the fullscreen transition), where nothing of the game's
// is bound and there is therefore nothing of the game's to clobber; and it is triggered by
// `icept::Sink::on_present`, which both hosts deliver unconditionally with no dependency on a
// loaded effect preset.
enum class HookMode
{
	// Inside the intercepted TAA compute dispatch, writing the engine's `u0`. The shipped
	// behaviour and, in phase 1, still the DEFAULT: until a run on the box says the stage produces
	// a correct image, the shipped configuration must stay byte-identical.
	taa,
	// Our own command list at Present, over the back buffer. No feedback path, no HDR codec, no
	// pass identification.
	present,
};

// Parses the config string. Anything unrecognised — including an empty value — is `taa`, because
// the default must be the shipped behaviour and a typo must never silently move the hook.
HookMode hook_mode_from_string(const char *value);
const char *hook_mode_name(HookMode mode);
// True for the site that sees a tonemapped, display-referred image and therefore must NOT run the
// HDR colour codec (running the soft clip and the sRGB encode over an already-encoded image would
// apply a transfer that has already been applied).
bool is_post_tonemap(HookMode mode);

// Why a present-stage frame was, or was not, injected into. Every one is counted and named, for
// the same reason the TAA path's gate refusals are: a stage that never fires must never be
// indistinguishable from a stage that fired and did nothing.
enum class PlanResult
{
	ok = 0,
	// The host handed us no back buffer, no device, or no command list to record on.
	no_colour,
	// Mipped, arrayed or multisampled. A mipped input to feature 18 is a documented
	// DXGI_ERROR_DEVICE_HUNG a few seconds later, not an error return.
	mipped_colour,
	// A zero-sized target, which is what a minimised or mid-resize swapchain looks like.
	zero_extent,
	// The back buffer's format cannot be written through a typed UAV. Our staging pair is
	// allocated in the BACK BUFFER's own format so both transfers are plain same-format copies,
	// and NGX writes DLSSNR.Output through a typed UAV — so a format the device cannot store to is
	// the "black output with no further indication" class of failure (CLAUDE.md §0.2). Refuse
	// loudly rather than allocate a texture the driver will reject or the runtime will not fill.
	no_typed_uav_store,
	// No TAA dispatch has published guides yet this session (a loading screen, the first frames,
	// or a session in which the TAA pass is never matched).
	guides_absent,
	// Guides exist but this trigger has already consumed them. The depth and motion vectors must
	// describe the frame we are about to modify, and a capture used once is by definition older.
	guides_stale,
	// Our own staging colour texture could not be allocated. Its own name rather than a reused
	// one, because "the driver refused an allocation" and "the host handed us nothing" want
	// completely different responses.
	staging_failed,
};

// NOTE: the count is duplicated in src/core/nr_hook_plan.cpp's kPlanResultNames — change both
// together. Same discipline as nr::kNrRefusalNames.
constexpr int kPlanResultCount = 8;
const char *plan_result_name(PlanResult result);

// What D3D12 told us about the colour target we were handed.
struct ColourDesc
{
	std::uint32_t width = 0;
	std::uint32_t height = 0;
	std::uint32_t mip_levels = 1;
	std::uint32_t array_size = 1;
	std::uint32_t sample_count = 1;
	// The three bits CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT) reports, kept separate
	// rather than collapsed to one bool so the log can say WHICH half is missing. Only the view
	// and store bits are load-bearing — NGX reads DLSSNR.Color through its own CUDA-texture path,
	// so a missing typed LOAD is worth a warning and nothing more.
	bool typed_uav_view = false;
	bool typed_uav_load = false;
	bool typed_uav_store = false;
	bool live = false; // the host reported a back buffer AND a command list to record on
};

// The per-frame capture the TAA path publishes.
//
// `sequence` is a PUBLICATION COUNTER, deliberately not a present index. Freshness is then "has
// this capture been consumed yet?", which is independent of the order in which a host fires its
// present callback relative to the frame's TAA dispatch. A frame with no TAA dispatch at all (a
// loading screen) simply does not advance the counter, so it refuses as stale without needing a
// separate test.
struct GuideState
{
	bool published = false;
	bool have_depth = false;
	bool have_motion = false;
	std::uint64_t sequence = 0;
	std::uint32_t render_width = 0;
	std::uint32_t render_height = 0;
};

struct Plan
{
	PlanResult result = PlanResult::no_colour;
	std::uint32_t width = 0;
	std::uint32_t height = 0;
	// The COLOUR/GUIDE ratio, which is what DLSSNR.MVecScaleX/Y is documented to want: the
	// snippet works on the colour grid while our vectors are on the guide grid.
	//
	// CONTESTED, and that is why it is computed here and carried as data rather than baked in. A
	// live A/B on the user's machine (CLAUDE.md, 2026-09-01) found 1.0 visibly more stable than
	// the ratio at the TAA site, and ngx_nr.cpp sends 1.0 there for that reason — we already
	// declare the guides' own rect through DLSSNR.MVecSubrectWidth/Height, so a runtime that
	// normalises by the subrect would apply the ratio twice. The stage therefore leaves
	// nr::ApplyInputs::mvec_scale_* at 0 ("derive"), which reaches the same 1.0, and this field is
	// reported in the log so the two numbers can be compared on the box before anything changes.
	float mvec_scale_x = 1.0f;
	float mvec_scale_y = 1.0f;
};

// The whole gate, in one place. `last_consumed_sequence` is the highest guide publication this
// trigger has already used; anything at or below it is stale. Zero means "nothing consumed yet".
Plan plan_post_tonemap(const ColourDesc &colour, const GuideState &guides,
                       std::uint64_t last_consumed_sequence);

// Feature 18 is NOT a per-frame spatial filter. It keeps an internal accumulator: it consumes
// motion vectors and depth (which a spatial-only network would not need) and exposes
// `DLSSNR.Reset`, named `settings.resetAccumulation` in the reference deployment.
//
// The consequence, verbatim from that deployment
// (github.com/lunks/dxvk-remix-plus-dlssnr @ fc4de144,
//  src/dxvk/rtx_render/rtx_neural_rendering.cpp:220-230):
//
//   "The NGX feature is keyed on the colour grid alone, so switching DLSS quality at a fixed
//    output resolution moves the guide grid --- and DLSSNR.MVecScaleX/Y with it --- underneath a
//    temporal history that was accumulated against the old one. Nothing else notices, so latch
//    the guide extent here and force a single reset frame when it moves."
//
// This is a LIVE bug for us and not a hypothetical: the feature is created for the OUTPUT rect,
// which does not change when the screen percentage does, so nothing else in our code notices that
// 1920x1080 guides became 2688x1512 guides and that MVecScale went from 2.0 to 1.42857 under an
// accumulated history.
struct GuideExtentLatch
{
	std::uint32_t width = 0;
	std::uint32_t height = 0;
};

// Records this frame's guide extent and answers whether NR's accumulation must be reset.
//
// Returns FALSE on the first observation, exactly as the reference does (`resetGuideHistory =
// (latched != 0)`): there is no history to invalidate before the first evaluate, and treating the
// first frame as a change would put a spurious reset into every session.
bool latch_guide_extent(GuideExtentLatch &latch, std::uint32_t width, std::uint32_t height);

// ---------------------------------------------------------------------------------------
// NO CODEC, NO EVALUATE.
//
// Feature 18 is a DISPLAY-REFERRED image network. Our hook point — the intercepted TAA dispatch
// — carries raw, unbounded, PRE-EXPOSED LINEAR HDR, which is not in its domain, and the HDR
// codec's soft clip and exact sRGB encode are precisely what put it there. The proxy is not a
// tuning stage that can be skipped; it IS the input contract. Measured with it missing: a neural
// output whose max luminance read 0.0026 and red noise on the screen.
//
// A sibling port of this integration reached the same rule from the other direction. It moved
// its pass to after the runtime's sRGB encode because feeding the network a linear image made
// the runtime apply a second gamma curve on top — "lifted blacks and washed out greys in dark
// scenes, exactly as reported" — and it declines the frames where that encode is suppressed
// "rather than evaluating on an input domain the model was not trained on"
// (RemixProjGroup/dxvk-remix, branch dlss-nr @ 2df9c812). Two ports, two hook points, one rule.
//
// So every way of arriving at EvaluateFeature without a correct proxy is enumerated here and
// answered with a refusal rather than a fall-through.
//
// SCOPE, restated 2026-09-02 now that the present stage exists: this gate is about the CODEC
// SITE, not about every site. A post-tonemap image is already display-referred and already in the
// network's domain, so bypassing the codec there is correct and `codec_site` is simply false —
// ngx_nr never asks this gate on that path. What the gate still catches is the codec site
// arriving at the evaluate with no usable proxy.
// ---------------------------------------------------------------------------------------
enum class CodecGate
{
	evaluate,
	// This call site does not run the encode/decode pair at all. The present stage legitimately
	// bypasses the codec and never consults this gate, so reaching `no_codec` means a CODEC site
	// would have handed the network an un-encoded image.
	no_codec,
	// The encode dispatch did not record, so the proxy holds whatever the last frame left.
	encode_failed,
	// NgxNRTrackExposure is on and the engine's exposure has never decoded, so the scale — which
	// DEFINES the display-referred units — is unknown. Substituting the static scale silently
	// moves the network's input domain, and feature 18's own temporal history was accumulated in
	// the other one.
	exposure_unknown,
	// The scale is pinned at one of nrc's clamps (or is zero/negative/NaN). The proxy is then
	// flat black or flat white: an image in the right FORMAT carrying no signal.
	degenerate_scale,
};

struct CodecGateInputs
{
	bool codec_site = true;
	bool encode_recorded = false;
	bool track_exposure = true;
	// Whether a plausible engine exposure has EVER been read this session, not whether this
	// particular frame's View CB decoded: the smoothed factor legitimately carries across a bad
	// frame, and one unreadable constant buffer is not an unknown operating point.
	bool exposure_known = false;
	float scale = 0.0f;
};

CodecGate codec_gate(const CodecGateInputs &in);

// Any frame NR declines is a hole in feature 18's own temporal continuity: it reprojects its
// accumulation with motion vectors describing one frame of motion, and a skipped frame makes
// that reprojection wrong. The next evaluate must therefore carry DLSSNR.Reset — once.
struct EvaluateGapLatch
{
	bool reset_pending = false;
};

void note_evaluate_gap(EvaluateGapLatch &latch);
bool take_evaluate_reset(EvaluateGapLatch &latch);

// THE HOLE THE REFUSAL PATH CANNOT SEE.
//
// note_evaluate_gap() is reached only from inside nr::apply(), so it covers exactly the frames NR
// was ASKED about and declined. It cannot cover the frames NR was never asked about at all, and
// those are not rare: apply() is called only when the TAA pass was intercepted AND the SR/RR
// evaluate succeeded (src/taa_hook.cpp), and NgxNR=0 -> 1 deliberately KEEPS the existing feature
// and its accumulated history across a gap of arbitrary length (nr::set_enabled). On every one of
// those frames feature 18's history stands still while the world moves, and the next evaluate
// reprojects across the whole gap with motion vectors that describe one frame.
//
// The sibling port does not need this because its pass is a fixed pipeline stage that runs every
// frame, so its sticky flag sees every frame by construction:
//
//   "The snippet's temporal history is only meaningful if the evaluation before this one produced
//    it [...] Sticky rather than a one-shot, so a run of failures keeps it armed until one
//    succeeds."   (Kim2091/dxvk-remix @ gta4-atmos-dlss5, rtx_neural_uplift.h:214-226)
//
// Ours is a hook, not a stage, so the equivalent has to be asserted at the frame boundary: one
// call per present, saying whether an evaluate actually happened in it. Idempotent with
// note_evaluate_gap — a frame that declined has already armed the latch, and arming it twice is
// still one reset.
void note_frame_boundary(EvaluateGapLatch &latch, bool evaluated_this_frame);

} // namespace stray_dlss::nrplan
