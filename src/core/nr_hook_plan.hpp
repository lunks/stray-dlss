// WHERE DLSS Neural Rendering is injected, whether a given frame may be injected into, and the
// guide grid its own temporal history was accumulated against. Pure; tested.
//
// WHY THIS IS A PURE MODULE. Every precondition of the present stage — the back buffer's shape,
// the freshness of the guides, the format's typed-UAV support — is a decision that can be made
// from plain numbers. Keeping them here means CI proves the refusal rules instead of the user's
// machine discovering them one round trip at a time (CLAUDE.md §0.4). src/nr_hook.cpp does
// nothing but gather those numbers from D3D12 and act on the verdict.
//
// HISTORY, because this file has been through three shapes. It once carried a THREE-site choice
// (`taa` / `present` / `preui`); `preui` was deleted on 2026-09-02, and `taa` followed once the
// present STAGE was confirmed correct in the game on 2026-09-03. There is one site now, so there
// is no mode to choose: what remains is the per-frame admission decision for that site.
#pragma once

#include "core/feature_recreate.hpp"

#include <cstdint>

namespace stray_dlss::nrplan {

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
	// the ratio back when NR ran at the TAA site, and ngx_nr.cpp sends 1.0 for that reason — we
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


// WHAT ACTUALLY IDENTIFIES A LIVE FEATURE 18, and what merely moved underneath it.
//
// THE DEFECT THIS EXISTS TO CLOSE (2026-09-04). ngx_nr.cpp's ensure_feature keyed the live
// feature on FOUR numbers - the guide rect AND the colour rect - and tore the feature down when
// any of them moved. Only two of them are real:
//
//   * nrparam::build_create writes DLSSNR.Width / DLSSNR.Height / DLSSNR.ScalingRatio and
//     nothing else, and ngx_nr.cpp passes `in_w = out_w, in_h = out_h` into it. The GUIDE rect
//     is never an argument to CreateFeature. (HARD: src/core/nr_params.cpp build_create, and
//     ngx_nr.cpp's `const std::uint32_t in_w = out_w;`.)
//   * The guide rect IS sent, but PER EVALUATE, through nrparam::build_rects ->
//     DLSSNR.DepthSubrectWidth/Height and DLSSNR.MVecSubrectWidth/Height. A moved guide grid is
//     therefore expressed by writing different numbers into the same live feature.
//
// So a guide-grid move needed no recreate at all - it needed exactly the single DLSSNR.Reset
// that latch_guide_extent above already forces. Recreating instead cost, every time:
//
//   * feature 18's ENTIRE temporal accumulation, where one reset frame was the documented and
//     already-implemented remedy;
//   * a run of frames refused as `recreating`, because the release is deferred to the present
//     boundary and the GPU fence has to pass the last evaluate first; and
//   * a CreateFeature, which on the SR path has been measured at 57-79 ms against a 16-18 ms
//     median (CLAUDE.md §2.1).
//
// MEASURED SHAPE, from the box's own log (stray-dlss-plugin.log.pre-dlaaclean): the session ran
// `1920x1080 -> 3840x2160` and then DLAA at `3840x2160 -> 3840x2160`. The COLOUR rect never
// moved - the back buffer stayed 3840x2160 the whole time - while the guide grid doubled. Under
// the old key that one event paid the whole list above; under this one it costs a reset frame.
// The same shape recurs wherever the render rect moves at a fixed output: a screen-percentage
// change (1920x1080 guides at 50%, 2688x1512 at 70%, both of which this project runs), and every
// letterbox slide the hold declines (CLAUDE.md §2.1).
// The rects are core::FeatureRect, the SAME four numbers the SR path is keyed on — deliberately
// the same type, because they are the same four numbers and a second struct shaped like it would
// invite the two to drift. What differs is the RULE over them, and that is the whole content of
// this function: core::operator== treats all four as identity, which is right for SR (its render
// rect is `InRenderSubrectDimensions`, a real create-time-bounded input), and wrong for feature
// 18 (whose `render_*` here is the GUIDE grid, a per-evaluate subrect parameter).
//
// True only when the live feature must be DESTROYED and rebuilt. `created` is what the live
// feature was created with; `want` is what this frame asks for.
bool feature_needs_recreate(const core::FeatureRect &created, const core::FeatureRect &want);

// ---- the validation verdict ----
//
// Until validation passes, NR evaluates but its result never reaches the screen: a leaked
// pre-release runtime that answers black must not be able to show a black frame (CLAUDE.md
// §0.2). The verdict is taken ONCE per session from a 128x128 centre crop of the neural output.
//
// THE DEFECT THIS EXISTS TO CLOSE (2026-09-04): that verdict read ONLY the neural crop, and a
// neural crop at or below the luminance floor latched NR off PERMANENTLY for the session, with
// one ERROR line. A frame that is BLACK ON THE INPUT - a loading screen, a fade, a cinematic
// letterbox, the menu before the first render - produces a black neural output because that is
// the CORRECT answer, and it convicted the runtime on it.
//
// This project has already met and fixed exactly this, on the frame-generation path: "the crop
// gate must treat a black REAL crop as a neutral look, or every loading screen revokes FG and
// re-validates three looks later (3 325 refused presents in one session)" (CLAUDE.md §5), which
// is core::fg::CropVerdict::dark. NR reads BOTH crops already - the colour input crop is
// captured beside the neural one and printed in the NR LUMINANCE line - and simply did not use
// the input half in the verdict. It does now.
enum class ValidationVerdict
{
	// The network answered with light. NR may reach the screen.
	pass,
	// The network answered black over an input that was NOT black. That is a real defect in the
	// runtime and it latches NR off for the session, loudly, exactly as before.
	degenerate,
	// BOTH crops are black, so the black answer is the correct one and proves nothing about the
	// runtime. Try again on a later frame rather than convicting on no evidence.
	inconclusive,
	// The neural crop could not be read back or decoded at all. Fails safe: NR stays off.
	undecodable,
};

const char *validation_verdict_name(ValidationVerdict v);

// One crop's measured maximum luminance, and whether it was decodable at all.
struct ValidationCrop
{
	double luma = 0.0;
	bool decoded = false;
};

// `floor` is the luminance below which a crop counts as black.
//
// NOTE the one deliberately conservative leg: if the NEURAL crop decoded but the INPUT crop did
// not (its capture is best-effort and must never block validation), we know nothing about what
// the network was shown, and the verdict stays `degenerate` - i.e. the behaviour before this
// function existed. Latching off on thin evidence is wrong, but so is never latching off at all,
// and this leg is an allocation failure rather than a scene the game is showing.
ValidationVerdict judge_validation(const ValidationCrop &input, const ValidationCrop &neural,
                                   double floor);




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
// those are not rare: the stage declines any present whose guides are absent or stale (a loading
// screen, a frame in which the TAA pass was never matched), and NgxNR=0 -> 1 deliberately KEEPS
// the existing feature and its accumulated history across a gap of arbitrary length
// (nr::set_enabled). On every one of those frames feature 18's history stands still while the
// world moves, and the next evaluate reprojects across the whole gap with motion vectors that
// describe one frame.
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
