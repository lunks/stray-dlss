// The flip-queue throttle's decisions (src/core/fg_throttle.hpp). Nothing here touches DXGI,
// which is the point: whether to wait, what a wait's answer means, when to stop waiting, and
// which swapchain flags to ask for are all plain logic, and all of them are things the game
// cannot report a violation of. A throttle that silently refused to arm looks exactly like one
// that armed and changed nothing, so the states are pinned apart here.
#include "core/fg_throttle.hpp"

#include <doctest/doctest.h>

using namespace stray_dlss::core::fg;

namespace {
constexpr unsigned kWaitable = 64u;      // DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT
constexpr unsigned kAllowTearing = 2048u; // DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING
constexpr unsigned kFlipDiscard = 4u;    // DXGI_SWAP_EFFECT_FLIP_DISCARD
constexpr unsigned kBitbltDiscard = 0u;  // DXGI_SWAP_EFFECT_DISCARD
} // namespace

TEST_CASE("arm_verdict reports the FIRST thing that is wrong, and every reason has a name")
{
	ArmInputs in;
	in.enabled = false;
	CHECK(arm_verdict(in) == ThrottleRefusal::disabled);

	in.enabled = true;
	CHECK(arm_verdict(in) == ThrottleRefusal::no_swapchain2);
	in.has_swapchain2 = true;
	CHECK(arm_verdict(in) == ThrottleRefusal::not_waitable);
	in.waitable_flag = true;
	CHECK(arm_verdict(in) == ThrottleRefusal::no_handle);
	in.have_handle = true;
	CHECK(arm_verdict(in) == ThrottleRefusal::none);

	// The log has to be able to say which one, so no reason may be nameless.
	for (int i = 0; i < static_cast<int>(ThrottleRefusal::count); ++i)
	{
		const char *n = throttle_refusal_name(static_cast<ThrottleRefusal>(i));
		REQUIRE(n != nullptr);
		CHECK(n[0] != '?');
	}
}

TEST_CASE("should_wait: enabled AND armed AND no standing refusal - inert is not the same as refused")
{
	ThrottleConfig cfg;
	ThrottleState s;
	// Off by default: the shipped behaviour must be byte-identical until a key is set.
	CHECK_FALSE(cfg.enabled);
	CHECK_FALSE(should_wait(cfg, s));

	cfg.enabled = true;
	CHECK_FALSE(should_wait(cfg, s)); // enabled but never armed
	s.armed = true;
	s.refusal = ThrottleRefusal::none;
	CHECK(should_wait(cfg, s));

	// Armed, then gave up: the config is still on, and we must still not wait.
	s.armed = false;
	s.refusal = ThrottleRefusal::gave_up;
	CHECK_FALSE(should_wait(cfg, s));
}

TEST_CASE("note_wait: a slot resets the timeout run; give-up fires ONCE and disarms")
{
	ThrottleConfig cfg;
	cfg.enabled = true;
	cfg.give_up_after = 3;
	ThrottleState s;
	s.armed = true;
	s.refusal = ThrottleRefusal::none;

	CHECK_FALSE(note_wait(cfg, s, WaitOutcome::slot_freed, 2'000'000));
	CHECK(s.waits == 1);
	CHECK(s.slots == 1);
	CHECK(s.blocked_max_ns == 2'000'000);

	CHECK_FALSE(note_wait(cfg, s, WaitOutcome::timed_out, 50'000'000));
	CHECK_FALSE(note_wait(cfg, s, WaitOutcome::timed_out, 50'000'000));
	CHECK(s.consecutive_timeouts == 2);
	// A single success anywhere in the run clears it: a full flip queue is a transient.
	CHECK_FALSE(note_wait(cfg, s, WaitOutcome::slot_freed, 1'000'000));
	CHECK(s.consecutive_timeouts == 0);
	CHECK(s.armed);

	CHECK_FALSE(note_wait(cfg, s, WaitOutcome::timed_out, 50'000'000));
	CHECK_FALSE(note_wait(cfg, s, WaitOutcome::timed_out, 50'000'000));
	// The third consecutive timeout is the one that gives up, and it says so exactly once.
	CHECK(note_wait(cfg, s, WaitOutcome::timed_out, 50'000'000));
	CHECK_FALSE(s.armed);
	CHECK(s.refusal == ThrottleRefusal::gave_up);
	CHECK(s.timeouts == 5);
	CHECK(s.waits == 7); // 2 slots + 5 timeouts
	// And it does not report giving up a second time, so the loud line is logged once - the
	// return is a TRANSITION, not a predicate on the state.
	CHECK_FALSE(note_wait(cfg, s, WaitOutcome::timed_out, 50'000'000));
	CHECK_FALSE(note_wait(cfg, s, WaitOutcome::failed, 0));
}

TEST_CASE("note_wait: give_up_after 0 never gives up, and a FAILED handle disarms immediately")
{
	ThrottleConfig cfg;
	cfg.enabled = true;
	cfg.give_up_after = 0;
	ThrottleState s;
	s.armed = true;
	s.refusal = ThrottleRefusal::none;
	for (int i = 0; i < 100; ++i)
		CHECK_FALSE(note_wait(cfg, s, WaitOutcome::timed_out, 1'000'000));
	CHECK(s.armed);
	CHECK(s.consecutive_timeouts == 100);

	// WAIT_FAILED is not back-pressure, it is a broken handle: one is enough.
	CHECK(note_wait(cfg, s, WaitOutcome::failed, 0));
	CHECK_FALSE(s.armed);
	CHECK(s.refusal == ThrottleRefusal::gave_up);
	CHECK(s.failures == 1);
}

TEST_CASE("blocked mean is per WAIT, not per present, and is 0 before the first wait")
{
	ThrottleState s;
	CHECK(throttle_blocked_mean_ms(s) == doctest::Approx(0.0));
	ThrottleConfig cfg;
	cfg.enabled = true;
	s.armed = true;
	s.refusal = ThrottleRefusal::none;
	note_wait(cfg, s, WaitOutcome::slot_freed, 4'000'000);
	note_wait(cfg, s, WaitOutcome::slot_freed, 2'000'000);
	CHECK(throttle_blocked_mean_ms(s) == doctest::Approx(3.0));
	CHECK(s.blocked_max_ns == 4'000'000);
}

TEST_CASE("plan_creation_flags: only flip-model, never twice, and the desc is untouched when off")
{
	unsigned out = 0xdeadbeef;
	CHECK(plan_creation_flags(false, kFlipDiscard, kAllowTearing, &out) == FlagVerdict::disabled);
	CHECK(out == kAllowTearing); // out is ALWAYS written, even when we change nothing

	CHECK(plan_creation_flags(true, kFlipDiscard, kAllowTearing, &out) == FlagVerdict::added);
	CHECK(out == (kAllowTearing | kWaitable)); // the game's own flags survive

	// A game that asked for it itself is left alone - and must be, since we then have nothing
	// to re-assert at a resize and nothing to be responsible for.
	CHECK(plan_creation_flags(true, kFlipDiscard, kWaitable, &out) == FlagVerdict::already_present);
	CHECK(out == kWaitable);

	// The flag is illegal on a bitblt swap effect; refusing beats failing the game's creation.
	CHECK(plan_creation_flags(true, kBitbltDiscard, 0, &out) == FlagVerdict::not_flip_model);
	CHECK(out == 0);

	for (int i = 0; i < static_cast<int>(FlagVerdict::count); ++i)
		CHECK(flag_verdict_name(static_cast<FlagVerdict>(i))[0] != '?');
}

TEST_CASE("resize_flags re-asserts the flag ONLY when we added it (MSDN: ResizeBuffers may reset any flag)")
{
	// UE 4.27 passes the flags IT asked for, so without this the waitable object silently
	// disappears at the first ResizeBuffers and the throttle blocks on a dead handle.
	CHECK(resize_flags(true, kAllowTearing) == (kAllowTearing | kWaitable));
	CHECK(resize_flags(true, kAllowTearing | kWaitable) == (kAllowTearing | kWaitable));
	// We did not add it: the game's flags go through exactly as given, in both directions.
	CHECK(resize_flags(false, kAllowTearing) == kAllowTearing);
	CHECK(resize_flags(false, kAllowTearing | kWaitable) == (kAllowTearing | kWaitable));
	CHECK(resize_flags(false, 0) == 0);
}

TEST_CASE("bypassed presents are counted separately from skipped ones")
{
	// The waitable object is a COUNTING semaphore released once per present, so a present that
	// never waited leaves a surplus behind and an armed throttle quietly stops binding. That
	// outcome must be readable off the counters, not deduced: `skipped` is a wait we chose not
	// to make (a drain), `bypassed` is a present that never reached the throttle at all.
	ThrottleState s;
	note_bypassed(s);
	note_bypassed(s);
	CHECK(s.bypassed == 2);
	CHECK(s.skipped == 0);
	CHECK(s.waits == 0);
}
