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

} // namespace stray_dlss::core
