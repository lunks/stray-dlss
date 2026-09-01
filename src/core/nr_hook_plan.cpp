#include "core/nr_hook_plan.hpp"

#include <cstring>

namespace stray_dlss::nrplan {
namespace {

// Parallel to PlanResult. NOTE: kPlanResultCount in the header must match.
const char *const kPlanResultNames[kPlanResultCount] = {
	"ok", "no-colour", "mipped-colour", "zero-extent", "no-typed-uav-store",
	"guides-absent", "guides-stale", "boundary-not-reached", "already-ran", "staging-failed",
};

} // namespace

HookMode hook_mode_from_string(const char *value)
{
	if (value == nullptr)
		return HookMode::taa;
	if (std::strcmp(value, "present") == 0)
		return HookMode::present;
	if (std::strcmp(value, "preui") == 0)
		return HookMode::preui;
	// "taa", "" and every typo. Silence is not an option elsewhere, but the DEFAULT being the
	// shipped behaviour is: an unreadable key must never move the hook.
	return HookMode::taa;
}

const char *hook_mode_name(HookMode mode)
{
	switch (mode)
	{
	case HookMode::present: return "present";
	case HookMode::preui:   return "preui";
	case HookMode::taa:     break;
	}
	return "taa";
}

bool is_post_tonemap(HookMode mode)
{
	return mode == HookMode::present || mode == HookMode::preui;
}

const char *plan_result_name(PlanResult result)
{
	const auto i = static_cast<int>(result);
	return i >= 0 && i < kPlanResultCount ? kPlanResultNames[i] : "?";
}

Plan plan_post_tonemap(const ColourDesc &colour, const GuideState &guides,
                       std::uint64_t last_consumed_sequence)
{
	Plan plan;

	// Order matters only in that the CHEAPEST and most fundamental refusals come first, so the
	// counted reason names the real problem rather than a downstream symptom.
	if (!colour.live)
	{
		plan.result = PlanResult::no_colour;
		return plan;
	}
	if (colour.width == 0 || colour.height == 0)
	{
		plan.result = PlanResult::zero_extent;
		return plan;
	}
	if (colour.mip_levels != 1 || colour.array_size != 1 || colour.sample_count != 1)
	{
		plan.result = PlanResult::mipped_colour;
		return plan;
	}
	// Only the bits NGX genuinely needs. It writes DLSSNR.Output through a typed UAV, so the view
	// and store bits are mandatory; it reads DLSSNR.Color through its own CUDA-texture path, so a
	// missing typed LOAD is not a reason to refuse (the caller warns about it instead).
	if (!colour.typed_uav_view || !colour.typed_uav_store)
	{
		plan.result = PlanResult::no_typed_uav_store;
		return plan;
	}
	if (!guides.published || guides.sequence == 0 || !guides.have_depth || !guides.have_motion ||
		guides.render_width == 0 || guides.render_height == 0)
	{
		plan.result = PlanResult::guides_absent;
		return plan;
	}
	// STRICTLY NEWER, not an age window. The depth and the motion vectors must describe the frame
	// we are about to modify; a one-frame-old depth buffer is a plausible-looking wrong image,
	// which is the failure class this project exists to avoid (CLAUDE.md §0.2). Each capture is
	// consumable exactly once.
	if (guides.sequence <= last_consumed_sequence)
	{
		plan.result = PlanResult::guides_stale;
		return plan;
	}

	plan.result = PlanResult::ok;
	plan.width = colour.width;
	plan.height = colour.height;
	plan.mvec_scale_x = static_cast<float>(colour.width) / static_cast<float>(guides.render_width);
	plan.mvec_scale_y = static_cast<float>(colour.height) / static_cast<float>(guides.render_height);
	return plan;
}

bool preui_boundary(std::uint32_t ordinal, std::uint32_t target_ordinal, bool already_ran)
{
	if (already_ran)
		return false;
	// A target of 0 would fire on nothing, which is a configuration mistake rather than a
	// boundary; treat it as "never", which is the fail-safe direction.
	if (target_ordinal == 0)
		return false;
	return ordinal == target_ordinal;
}

bool latch_guide_extent(GuideExtentLatch &latch, std::uint32_t width, std::uint32_t height)
{
	const bool had_previous = latch.width != 0 || latch.height != 0;
	const bool moved = latch.width != width || latch.height != height;
	latch.width = width;
	latch.height = height;
	return had_previous && moved;
}

} // namespace stray_dlss::nrplan
