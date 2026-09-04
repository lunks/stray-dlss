// WHEN MAY A DLSS FEATURE BE RE-CREATED? The pure decision, with no NGX in it.
//
// THE MEASUREMENT THAT MOTIVATES IT (live plugin log, one session, 2026-09-03, read read-only
// while the user played). 88 `DLSS feature created:` lines, and only ONE of them is the feature
// this session actually needed:
//
//   31  1920x1080 -> 3840x2160        the primary view
//   26  64x34 .. 1024x1024 -> 3840x2160   the wrong-View-CB bug (report §16), fixed at source
//   31  1920x10xx -> 3840x20xx        THIS: a scripted scene transition animating the rect
//
// The third family is the user's second defect — *"during some script scene transitions, the NR
// kinda pops up/slows down as if there was something off"*. The rects walk 3840x2160 down to
// 3840x2073 and back over a fraction of a second, in runs of SIX to TEN consecutive creations,
// and the aspect goes 1.778 -> 1.85: cinematic bars, animated. Every step destroys DLSS's
// temporal accumulation (the pop) and pays a full `CreateFeature` (the hitch; the same session
// carries 164 stalls, the recent ones 57-79 ms against a 16-18 ms median).
//
// WHY THE OBVIOUS FIX DOES NOT APPLY, and it is worth stating because it is the first thing
// anyone reaches for. DLSS handles a varying RENDER rect without recreating: create at the
// maximum and pass `InRenderSubrectDimensions` per evaluate — which `ngx_backend::evaluate`
// already does, from `fd.render_*`, every frame. But NGX has no such parameter for the OUTPUT:
// `NVSDK_NGX_D3D12_DLSS_Eval_Params` carries `InRenderSubrectDimensions` (dimensions) and
// `InOutputSubrectBase` (a base COORDINATE only) — the target extent is fixed at
// `CreateFeature` by `InTargetWidth/Height` (nvsdk_ngx_helpers.h:377-398, HARD). In this family
// the output rect moves with the render rect on every step, so a subrect cannot express it and
// a recreate is genuinely required, not habitual. CLAUDE.md §2.1's rule is intact.
//
// SO THE FIX IS NOT TO RECREATE FASTER, IT IS NOT TO CHASE. A rect that is still animating is
// not a rect worth building a feature for: by the time the tensors are allocated it has moved
// again. Wait until a differing rect has been ASKED FOR the same way `stable_frames` times in a
// row, and decline DLSS meanwhile — the engine's own TAA renders those frames correctly, which
// is the cheap and correct failure (prime directive 2). Applied to the measured sequence this
// turns each 6-10 creation burst into ZERO: the animation never settles, so nothing is built,
// and when it returns to 1920x1080 the ORIGINAL feature is still alive and its history intact,
// so there is no pop at the end of the transition either.
//
// A genuine change — a resolution or screen-percentage change — settles immediately, so it
// costs `stable_frames` frames of engine TAA and then creates exactly once.
//
// THE FIRST creation is never debounced: with no feature there is no history to protect and
// nothing to weigh against, so waiting would only delay DLSS coming up.
#pragma once

#include <cstdint>

namespace stray_dlss::core {

// The four numbers a DLSS feature is keyed on. Render is the subrect DLSS reads, output the
// target it writes; NGX can vary the first per evaluate and not the second.
struct FeatureRect
{
	std::uint32_t render_w = 0;
	std::uint32_t render_h = 0;
	std::uint32_t output_w = 0;
	std::uint32_t output_h = 0;
};

inline bool operator==(const FeatureRect &a, const FeatureRect &b)
{
	return a.render_w == b.render_w && a.render_h == b.render_h &&
		a.output_w == b.output_w && a.output_h == b.output_h;
}
inline bool operator!=(const FeatureRect &a, const FeatureRect &b) { return !(a == b); }

enum class RecreateAction : std::uint8_t
{
	keep,   // the live feature already matches: evaluate with it
	create, // build one now (no feature yet, or a differing rect that has settled)
	wait,   // a differing rect that has NOT settled: decline this frame, keep the feature
};

// The debounce's own memory. Owned by the caller so the decision function stays pure and the
// whole thing is testable without NGX.
struct RecreateState
{
	FeatureRect pending{};             // the differing rect most recently asked for
	std::uint32_t pending_count = 0;   // consecutive frames it has been asked for
	std::uint64_t waits = 0;           // frames declined by the debounce, for the log
	std::uint64_t restarts = 0;        // times the requested rect CHANGED AGAIN mid-debounce:
	                                   // a high count with no creations is an animating rect,
	                                   // which is the scripted-transition signature exactly
};

// `have_feature` / `live` describe the feature that exists now (`live` unread when false).
// `stable_frames` is [STRAYDLSS] NgxRecreateStableFrames; 0 restores the old behaviour of
// recreating on the first differing frame, so the box can A/B it without a rebuild.
//
// Call ONCE per candidate frame — it advances `state`.
RecreateAction plan_recreate(RecreateState &state, bool have_feature, const FeatureRect &live,
                             const FeatureRect &want, std::uint32_t stable_frames);

// Default for `stable_frames`. The measured transitions step the rect every ~3 frames and run
// 6-10 steps, so anything above ~4 refuses to chase them; 8 is comfortably clear of that while
// costing a genuine resolution change only ~8 frames (~0.13 s at 60 fps) of engine TAA.
constexpr std::uint32_t kDefaultRecreateStableFrames = 8;

// ---------------------------------------------------------------------------------------
// KEEPING DLSS ALIVE ACROSS THE LETTERBOX SLIDE
// ---------------------------------------------------------------------------------------
//
// The debounce above removed the 6-10 `CreateFeature` bursts per scripted transition, and it
// kept the feature and its history alive. But `wait` DECLINES the frame, so DLSS SR does not run
// during the slide — and NR consumes the guides the TAA hook publishes on a successful SR
// evaluate, so **NR is off for the whole animation**. USER-REPORTED: *"I think DLSS NR was
// deactivated on the scene transitions, so it should probably support it?"*, and on the
// mechanism, *"it's a letterbox animation, it slowly slides back to 100% frame."*
//
// THE THREE CANDIDATES, AND WHY ONLY ONE SURVIVES CONTACT WITH THE NGX HEADER:
//
//   1. DECLINE (the debounce alone). No distortion, no DLSS, no NR. What shipped.
//   2. KEEP EVALUATING AT THE ENGINE'S SHRINKING SUBRECT. **Refuted.** `NGX_D3D12_CREATE_DLSS_EXT`
//      writes `InTargetWidth/Height` into `NVSDK_NGX_Parameter_OutWidth/OutHeight`
//      (nvsdk_ngx_helpers.h:437-440) and there is NO per-evaluate output size, so DLSS always
//      upscales `InRenderSubrectDimensions` to the CREATE-TIME target. Render 1920x1037 into a
//      3840x2160 target is 2.000x across and 2.083x down — a non-uniform stretch that ANIMATES
//      as the bars slide. Worse than declining.
//   3. WRITE A SMALLER REGION OF THE TARGET. **Not expressible.** The eval params carry six
//      `NVSDK_NGX_Coordinates *SubrectBase` fields — `InOutputSubrectBase` among them — and they
//      are BASE COORDINATES only (nvsdk_ngx_helpers.h:377-398). There is no output-subrect
//      DIMENSIONS, so DLSS cannot be asked to write less than the created target.
//
// WHAT WORKS IS (3) TURNED AROUND, AND IT IS EXACT RATHER THAN APPROXIMATE. Hold
// `InRenderSubrectDimensions` at the extent the feature was CREATED with, and let DLSS write the
// whole created target. The scale then stays exactly what it was — 2.0 on both axes — and
// because UE anchors both rects at the origin (`FTAAPassParameters::SetupViewRect`, and
// `View.ViewRectMin` which we READ rather than assume), the engine's shrunken output rect is
// PRECISELY the image of its shrunken input rect:
//
//     engine renders rows 0..1036  ->  DLSS maps them to output rows 0..2073  ->  the engine
//     reads output rows 0..2072, which is exactly its own OutputViewRect.
//
// So nothing needs placing and `InOutputSubrectBase` stays (0,0), where it already is. DLSS
// additionally computes the rows BELOW the engine's rect, from input rows the engine did not
// render this frame — stale by a frame or two — and nothing displays them, because they are
// outside every downstream pass's view rect.
//
// THE COST, STATED HONESTLY, because it is not zero:
//   * DLSS's temporal history for those below-the-rect rows accumulates from stale input. When
//     the bars finish retracting and the rows become visible again, that history is wrong for a
//     few frames. That is a bounded, self-healing artefact at the very bottom of the frame,
//     against DLSS being off for the whole second.
//   * Reconstruction near the boundary draws on a few input rows either side of it, so the
//     bottom few pixels of the visible image can be touched by stale content during the slide.
//
// WHAT MAKES IT SAFE IS THE GATE, and every clause is a measurement rather than an assumption.
// In particular `origin_moved`: if Stray ever letterboxed by CENTRING the rect instead of
// shrinking it from the top-left, `View.ViewRectMin` would not be (0,0), the mapping above would
// be false, and this refuses and falls back to the debounce. That is the one fact that decides
// whether holding is correct, and it is read from the View CB every frame.

enum class HoldRefusal : std::uint8_t
{
	none = 0,            // hold: evaluate at the created extent, do not rebuild
	no_feature,          // nothing live to hold on to
	not_needed,          // the request already matches the live feature
	origin_moved,        // View.ViewRectMin != (0,0): the rect is NOT top-left anchored, so the
	                     // created scale does not map this input rect onto this output rect
	larger_than_feature, // the request exceeds the created extent on some axis - out of DLSS's
	                     // dynamic range, and it would read outside what we validated
	too_small,           // more than half the created extent would be unrendered input
	ratio_moved,         // the created scale does not carry this render rect onto this output
	                     // rect: a real resolution change, not a letterbox slide
	output_too_small,    // the OUTPUT UAV can no longer hold the created target. THE PHYSICAL
	                     // guard: every other clause is arithmetic over rects, and this is the
	                     // one that asks whether the memory DLSS would write into is still
	                     // there. A resolution change reallocates the engine's buffers, and
	                     // writing a 3840x2160 target into a 2560x1440 UAV is out of bounds with
	                     // nothing on this stack to object (vkd3d has no debug layer). Unknown
	                     // (0x0) is a refusal, never a guess.
	count
};
const char *hold_refusal_name(HoldRefusal r);

// How far the created scale's prediction of the output rect may miss, in output pixels. The
// engine's rects are not quantised here — the live log carries `3840x2073` and `3840x2074` for
// the SAME 1920x1037 render rect — so this absorbs the odd/even rounding and nothing more.
constexpr std::uint32_t kHoldRectTolerance = 2;

// The held region must be at least this fraction of the created extent on each axis.
//
// CAUGHT BY THE TESTS, and it is the sharp edge of this whole idea: the ratio test alone does
// NOT separate a letterbox slide from a genuine resolution change, because a drop to
// 2560x1440 output with a 1280x720 render rect has the SAME 2.0 scale and would satisfy every
// other clause. The mapping argument even survives it on paper - but the engine reallocates its
// buffers for the new resolution, so DLSS would be writing a 3840x2160 target into a 2560x1440
// UAV. **The physical guard is at the call site: the output texture must still be able to hold
// the created target**, and it is not expressible here. This bound is the cheap half of the
// same fence: a slide is a SMALL perturbation - the measured one bottoms out at 1037/1080 =
// 0.96 - while anything that halves the frame is a different configuration, and rebuilding that
// costs eight frames once.
constexpr double kHoldMinFraction = 0.75;

// May we hold `live` and evaluate at ITS extent instead of rebuilding for `want`?
// Returns `HoldRefusal::none` to hold; any other value names why not, for the counter and the
// log.
//
//   `view_rect_min_*`  `View.ViewRectMin` (row 129) as read THIS FRAME. Pass it, never a
//                      constant — it is the clause that separates a top-left slide from a
//                      centred one, and the whole mapping depends on it.
//   `out_tex_*`        the OUTPUT UAV's own extent this frame. Holding means DLSS writes the
//                      created target, not the engine's smaller rect, so the texture must still
//                      be big enough for it. 0x0 (not identified) refuses.
HoldRefusal plan_letterbox_hold(const FeatureRect &live, const FeatureRect &want,
                                float view_rect_min_x, float view_rect_min_y,
                                std::uint32_t out_tex_w, std::uint32_t out_tex_h);

} // namespace stray_dlss::core
