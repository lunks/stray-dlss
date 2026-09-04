#include "core/nr_hook_plan.hpp"


#include <cmath>
#include <cstring>

namespace stray_dlss::nrplan {
namespace {

// Parallel to PlanResult. NOTE: kPlanResultCount in the header must match.
const char *const kPlanResultNames[kPlanResultCount] = {
	"ok", "no-colour", "mipped-colour", "zero-extent", "no-typed-uav-store",
	"guides-absent", "guides-stale", "staging-failed",
};

} // namespace




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
	// and store bits are mandatory; it reads DLSSNR.Color through its own path, so a missing typed
	// LOAD is not a reason to refuse (the caller warns about it instead).
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

bool latch_guide_extent(GuideExtentLatch &latch, std::uint32_t width, std::uint32_t height)
{
	const bool had_previous = latch.width != 0 || latch.height != 0;
	const bool moved = latch.width != width || latch.height != height;
	latch.width = width;
	latch.height = height;
	return had_previous && moved;
}


bool feature_needs_recreate(const core::FeatureRect &created, const core::FeatureRect &want)
{
	// THE COLOUR RECT AND NOTHING ELSE. See the header for why the guide rect is not identity:
	// it is not an argument to CreateFeature, it is a per-evaluate subrect parameter, and a move
	// in it is answered by latch_guide_extent's single DLSSNR.Reset.
	return created.output_w != want.output_w || created.output_h != want.output_h;
}

const char *validation_verdict_name(ValidationVerdict v)
{
	switch (v)
	{
	case ValidationVerdict::pass:         return "pass";
	case ValidationVerdict::degenerate:   return "degenerate";
	case ValidationVerdict::inconclusive: return "inconclusive";
	case ValidationVerdict::undecodable:  return "undecodable";
	}
	return "?";
}

ValidationVerdict judge_validation(const ValidationCrop &input, const ValidationCrop &neural,
                                   double floor)
{
	// Nothing to judge: the neural crop is the one the verdict rests on.
	if (!neural.decoded)
		return ValidationVerdict::undecodable;
	// Light in the answer is the whole test, and it is checked FIRST so a working runtime is
	// never held up by anything the input crop did or did not do.
	if (neural.luma > floor)
		return ValidationVerdict::pass;
	// Black answer. Was it a black question? core::fg::CropVerdict::dark, applied here.
	if (input.decoded && input.luma <= floor)
		return ValidationVerdict::inconclusive;
	return ValidationVerdict::degenerate;
}

void note_evaluate_gap(EvaluateGapLatch &latch) { latch.reset_pending = true; }

bool take_evaluate_reset(EvaluateGapLatch &latch)
{
	const bool pending = latch.reset_pending;
	latch.reset_pending = false;
	return pending;
}

void note_frame_boundary(EvaluateGapLatch &latch, bool evaluated_this_frame)
{
	// Only ever ARMS. A frame that did evaluate must not clear a pending reset, because the
	// evaluate itself already consumed it (take_evaluate_reset runs before the NGX call) and a
	// later failure in the same frame re-arms it deliberately.
	if (!evaluated_this_frame)
		latch.reset_pending = true;
}

} // namespace stray_dlss::nrplan
