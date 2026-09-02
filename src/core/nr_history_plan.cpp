#include "core/nr_history_plan.hpp"

namespace stray_dlss::histplan {
namespace {

// Parallel to Step. NOTE: kStepCount in the header must match.
const char *const kStepNames[kStepCount] = {
	"ok", "disabled", "nr-disabled", "nr-cannot-apply",
	"no-image", "zero-extent", "unsupported-image", "alloc-failed", "no-snapshot",
	"nr-not-applied", "no-command-list",
};

} // namespace

const char *step_name(Step step)
{
	const auto i = static_cast<int>(step);
	return i >= 0 && i < kStepCount ? kStepNames[i] : "?";
}

Step plan_snapshot(const Config &config, const SnapshotInputs &in)
{
	// Order: the CHEAPEST and most fundamental refusals first, so the counted reason names the
	// real problem rather than a downstream symptom.
	if (!config.enabled)
		return Step::disabled;
	if (!in.nr_enabled)
		return Step::nr_disabled;
	if (!in.nr_can_apply)
		return Step::nr_cannot_apply;
	if (!in.have_image)
		return Step::no_image;
	if (in.width == 0 || in.height == 0)
		return Step::zero_extent;
	// The same shape the neural runtime demands of its colour input. If this fires, nr::apply
	// refused on the identical resource and there is nothing to restore — but say so rather than
	// leave a silent hole where the snapshot should be.
	if (in.mip_levels != 1 || in.array_size != 1 || in.sample_count != 1)
		return Step::unsupported_image;
	return Step::ok;
}

Step plan_restore(const Config &config, const RestoreInputs &in)
{
	if (!config.enabled)
		return Step::disabled;
	// NR REFUSED, SO `u0` IS UNTOUCHED. Checked BEFORE the snapshot test on purpose: a frame
	// where NR did not apply is the common, benign case, and reporting it as `no-snapshot` would
	// bury the one instance of that reason that actually matters.
	if (!in.nr_applied)
		return Step::nr_not_applied;
	if (!in.have_snapshot)
		return Step::no_snapshot;
	return Step::ok;
}

bool restore_miss_is_harmful(const Config &config, const RestoreInputs &in)
{
	return config.enabled && in.nr_applied && !in.have_snapshot;
}

} // namespace stray_dlss::histplan
