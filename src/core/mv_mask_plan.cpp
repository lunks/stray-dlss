#include "core/mv_mask_plan.hpp"

namespace stray_dlss::mvmaskplan {

const char *refusal_name(Refusal r) noexcept
{
	switch (r)
	{
	case Refusal::none:      return "none";
	case Refusal::disabled:  return "disabled";
	case Refusal::not_ready: return "not-ready";
	case Refusal::unfilled:  return "unfilled";
	case Refusal::extent:    return "extent";
	case Refusal::count:     break;
	}
	return "?";
}

bool alternate_phase_on(std::uint64_t frame, std::uint32_t period) noexcept
{
	if (period == 0)
		return true; // no alternation: hold the configured value
	return ((frame / period) & 1ull) != 0ull;
}

Plan plan(bool enabled, bool ready, bool filled, std::uint32_t mask_w, std::uint32_t mask_h,
	std::uint32_t render_w, std::uint32_t render_h, float configured_value,
	std::uint32_t alternate_period, std::uint64_t frame) noexcept
{
	Plan p;
	p.phase_on = alternate_phase_on(frame, alternate_period);
	p.value = p.phase_on ? configured_value : kNeutral;

	if (!enabled)
	{
		p.refusal = Refusal::disabled;
		p.value = kNeutral;
		p.phase_on = false;
		return p;
	}
	if (!ready)
	{
		p.refusal = Refusal::not_ready;
		return p;
	}
	if (!filled)
	{
		// The texture's contents are whatever the allocator left there. Handing that to DLSS is
		// worse than handing it nothing, because it is per-pixel noise on the one channel whose
		// job is to say which pixels to distrust.
		p.refusal = Refusal::unfilled;
		return p;
	}
	if (render_w == 0 || render_h == 0 || mask_w < render_w || mask_h < render_h)
	{
		p.refusal = Refusal::extent;
		return p;
	}

	p.bind = true;
	p.refusal = Refusal::none;
	return p;
}

} // namespace stray_dlss::mvmaskplan
