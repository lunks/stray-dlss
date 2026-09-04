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

} // namespace stray_dlss::core
