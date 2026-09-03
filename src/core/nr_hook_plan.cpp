#include "core/nr_hook_plan.hpp"

#include "core/nr_codec.hpp"

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

HookMode hook_mode_from_string(const char *value)
{
	if (value == nullptr)
		return HookMode::taa;
	if (std::strcmp(value, "present") == 0)
		return HookMode::present;
	// "taa", "" and every typo. Silence is not an option elsewhere, but the DEFAULT being the
	// shipped behaviour is: an unreadable key must never move the hook.
	return HookMode::taa;
}

const char *hook_mode_name(HookMode mode)
{
	switch (mode)
	{
	case HookMode::present: return "present";
	case HookMode::taa:     break;
	}
	return "taa";
}

bool is_post_tonemap(HookMode mode) { return mode == HookMode::present; }

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

CodecGate codec_gate(const CodecGateInputs &in)
{
	// Ordered most-fundamental first, so the reason a frame is refused names the thing that is
	// actually wrong rather than a consequence of it.
	if (!in.codec_site)
		return CodecGate::no_codec;
	if (in.track_exposure && !in.exposure_known)
		return CodecGate::exposure_unknown;
	// A scale sitting ON a clamp is degenerate, not merely extreme: nrc::proxy_scale only lands
	// there by saturating, and the resulting proxy carries no signal.
	if (!std::isfinite(in.scale) || in.scale <= nrc::kScaleMin || in.scale >= nrc::kScaleMax)
		return CodecGate::degenerate_scale;
	if (!in.encode_recorded)
		return CodecGate::encode_failed;
	return CodecGate::evaluate;
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
