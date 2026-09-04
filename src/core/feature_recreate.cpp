#include "feature_recreate.hpp"

namespace stray_dlss::core {

RecreateAction plan_recreate(RecreateState &state, bool have_feature, const FeatureRect &live,
                             const FeatureRect &want, std::uint32_t stable_frames)
{
	// A rect we cannot build anything from. The caller refuses it on its own account; say
	// `keep` so this function never invents a creation out of nothing.
	if (want.render_w == 0 || want.render_h == 0 || want.output_w == 0 || want.output_h == 0)
	{
		state.pending = FeatureRect{};
		state.pending_count = 0;
		return RecreateAction::keep;
	}

	// The live feature matches. Clear the debounce so a rect that flickers away and back does
	// not accumulate credit toward a recreation it no longer needs.
	if (have_feature && live == want)
	{
		state.pending = FeatureRect{};
		state.pending_count = 0;
		return RecreateAction::keep;
	}

	// Nothing to protect and nothing to weigh: build it now.
	if (!have_feature)
	{
		state.pending = FeatureRect{};
		state.pending_count = 0;
		return RecreateAction::create;
	}

	if (stable_frames == 0)
		return RecreateAction::create; // NgxRecreateStableFrames=0: the old behaviour, exactly

	if (state.pending == want)
	{
		++state.pending_count;
	}
	else
	{
		state.pending = want;
		state.pending_count = 1;
		++state.restarts;
	}

	if (state.pending_count >= stable_frames)
	{
		state.pending = FeatureRect{};
		state.pending_count = 0;
		return RecreateAction::create;
	}

	++state.waits;
	return RecreateAction::wait;
}

const char *hold_refusal_name(HoldRefusal r)
{
	switch (r)
	{
	case HoldRefusal::none:                return "hold";
	case HoldRefusal::no_feature:          return "noFeature";
	case HoldRefusal::not_needed:          return "notNeeded";
	case HoldRefusal::origin_moved:        return "originMoved";
	case HoldRefusal::larger_than_feature: return "largerThanFeature";
	case HoldRefusal::too_small:           return "tooSmall";
	case HoldRefusal::ratio_moved:         return "ratioMoved";
	case HoldRefusal::output_too_small:    return "outputTooSmall";
	case HoldRefusal::count:               break;
	}
	return "?";
}

HoldRefusal plan_letterbox_hold(const FeatureRect &live, const FeatureRect &want,
                                float view_rect_min_x, float view_rect_min_y,
                                std::uint32_t out_tex_w, std::uint32_t out_tex_h)
{
	if (live.render_w == 0 || live.render_h == 0 || live.output_w == 0 || live.output_h == 0)
		return HoldRefusal::no_feature;
	if (want.render_w == 0 || want.render_h == 0 || want.output_w == 0 || want.output_h == 0)
		return HoldRefusal::no_feature;
	if (live == want)
		return HoldRefusal::not_needed;

	// THE CLAUSE THE WHOLE THING RESTS ON. Holding is correct only because the engine's input
	// and output rects share the origin with the created ones, so a shrinking rect is a PREFIX
	// of the created one on both axes. A centred letterbox would move this off (0,0) and the
	// mapping would be silently wrong - which is exactly the class of failure that must refuse
	// loudly instead. Exact comparison: this row is written as an integer pair.
	if (view_rect_min_x != 0.0f || view_rect_min_y != 0.0f)
		return HoldRefusal::origin_moved;

	if (want.render_w > live.render_w || want.render_h > live.render_h ||
		want.output_w > live.output_w || want.output_h > live.output_h)
		return HoldRefusal::larger_than_feature;

	if (static_cast<double>(want.render_w) < static_cast<double>(live.render_w) * kHoldMinFraction ||
		static_cast<double>(want.render_h) < static_cast<double>(live.render_h) * kHoldMinFraction)
		return HoldRefusal::too_small;

	// Does the CREATED scale still carry this render rect onto this output rect? In exact
	// integer arithmetic, `want.render * live.output == want.output * live.render` up to
	// kHoldRectTolerance output pixels. A genuine resolution or screen-percentage change breaks
	// this and falls through to the debounce; a letterbox slide, which moves both rects
	// together, does not.
	const std::uint64_t predicted_w = static_cast<std::uint64_t>(want.render_w) * live.output_w;
	const std::uint64_t actual_w = static_cast<std::uint64_t>(want.output_w) * live.render_w;
	const std::uint64_t slack_w = static_cast<std::uint64_t>(kHoldRectTolerance) * live.render_w;
	if ((predicted_w > actual_w ? predicted_w - actual_w : actual_w - predicted_w) > slack_w)
		return HoldRefusal::ratio_moved;

	const std::uint64_t predicted_h = static_cast<std::uint64_t>(want.render_h) * live.output_h;
	const std::uint64_t actual_h = static_cast<std::uint64_t>(want.output_h) * live.render_h;
	const std::uint64_t slack_h = static_cast<std::uint64_t>(kHoldRectTolerance) * live.render_h;
	if ((predicted_h > actual_h ? predicted_h - actual_h : actual_h - predicted_h) > slack_h)
		return HoldRefusal::ratio_moved;

	// THE PHYSICAL CLAUSE, and it is last because it is the only one that is not arithmetic
	// over rects. Holding writes the CREATED target, so the UAV must still be able to hold it.
	// A genuine resolution change reallocates the engine's buffers underneath an unchanged
	// ratio - 1280x720 -> 2560x1440 has the same 2.0 scale as 1920x1080 -> 3840x2160 - and this
	// is what stops that becoming an out-of-bounds write. Unknown extent refuses.
	if (out_tex_w == 0 || out_tex_h == 0 ||
		out_tex_w < live.output_w || out_tex_h < live.output_h)
		return HoldRefusal::output_too_small;

	return HoldRefusal::none;
}

} // namespace stray_dlss::core
