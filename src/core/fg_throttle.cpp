#include "core/fg_throttle.hpp"

namespace stray_dlss::core::fg {

namespace {
// DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT = 64 (dxgi.h, MSDN's own enum listing).
// Named here rather than including dxgi.h, so the core still builds on the Linux test lane.
constexpr unsigned kWaitableFlag = 64u;
// DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL = 3, DXGI_SWAP_EFFECT_FLIP_DISCARD = 4 (dxgi.h). The
// bitblt effects (0 DISCARD, 1 SEQUENTIAL) cannot carry the flag.
constexpr unsigned kFlipSequential = 3u;
constexpr unsigned kFlipDiscard = 4u;
} // namespace

const char *throttle_refusal_name(ThrottleRefusal r)
{
	switch (r)
	{
	case ThrottleRefusal::none: return "none";
	case ThrottleRefusal::disabled: return "disabled";
	case ThrottleRefusal::no_swapchain2: return "no-swapchain2";
	case ThrottleRefusal::not_waitable: return "not-waitable";
	case ThrottleRefusal::no_handle: return "no-handle";
	case ThrottleRefusal::gave_up: return "gave-up";
	case ThrottleRefusal::count: break;
	}
	return "?";
}

const char *flag_verdict_name(FlagVerdict v)
{
	switch (v)
	{
	case FlagVerdict::disabled: return "disabled";
	case FlagVerdict::already_present: return "already-present";
	case FlagVerdict::added: return "added";
	case FlagVerdict::not_flip_model: return "not-flip-model";
	case FlagVerdict::count: break;
	}
	return "?";
}

bool should_wait(const ThrottleConfig &cfg, const ThrottleState &s)
{
	return cfg.enabled && s.armed && s.refusal == ThrottleRefusal::none;
}

ThrottleRefusal arm_verdict(const ArmInputs &in)
{
	if (!in.enabled)
		return ThrottleRefusal::disabled;
	if (!in.has_swapchain2)
		return ThrottleRefusal::no_swapchain2;
	if (!in.waitable_flag)
		return ThrottleRefusal::not_waitable;
	if (!in.have_handle)
		return ThrottleRefusal::no_handle;
	return ThrottleRefusal::none;
}

bool note_wait(const ThrottleConfig &cfg, ThrottleState &s, WaitOutcome outcome, std::uint64_t blocked_ns)
{
	++s.waits;
	s.blocked_ns += blocked_ns;
	if (blocked_ns > s.blocked_max_ns)
		s.blocked_max_ns = blocked_ns;
	switch (outcome)
	{
	case WaitOutcome::slot_freed:
		++s.slots;
		s.consecutive_timeouts = 0;
		return false;
	case WaitOutcome::failed:
	{
		// A broken handle is not a full queue: disarm at once rather than burning the timeout
		// on every present for the rest of the session.
		++s.failures;
		const bool transition = s.armed;
		s.armed = false;
		s.refusal = ThrottleRefusal::gave_up;
		return transition; // the loud line is logged on the TRANSITION, never once per present
	}
	case WaitOutcome::timed_out:
		break;
	}
	++s.timeouts;
	++s.consecutive_timeouts;
	if (cfg.give_up_after != 0 && s.consecutive_timeouts >= cfg.give_up_after)
	{
		const bool transition = s.armed;
		s.armed = false;
		s.refusal = ThrottleRefusal::gave_up;
		return transition;
	}
	return false;
}

void note_bypassed(ThrottleState &s) { ++s.bypassed; }

double throttle_blocked_mean_ms(const ThrottleState &s)
{
	if (s.waits == 0)
		return 0.0;
	return static_cast<double>(s.blocked_ns) / static_cast<double>(s.waits) / 1e6;
}

FlagVerdict plan_creation_flags(bool enabled, unsigned swap_effect, unsigned in_flags, unsigned *out_flags)
{
	if (out_flags != nullptr)
		*out_flags = in_flags;
	if (!enabled)
		return FlagVerdict::disabled;
	if ((in_flags & kWaitableFlag) != 0)
		return FlagVerdict::already_present;
	if (swap_effect != kFlipSequential && swap_effect != kFlipDiscard)
		return FlagVerdict::not_flip_model;
	if (out_flags != nullptr)
		*out_flags = in_flags | kWaitableFlag;
	return FlagVerdict::added;
}

unsigned resize_flags(bool we_added_it, unsigned game_flags)
{
	return we_added_it ? (game_flags | kWaitableFlag) : game_flags;
}

} // namespace stray_dlss::core::fg
