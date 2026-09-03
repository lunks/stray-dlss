// The flip-queue throttle's pure decisions: whether to wait on the swapchain's frame-latency
// waitable object before a present, what to do with the answer, and when to give up. No DXGI
// here — src/backend_native/fg_throttle.cpp is the shell.
//
// WHY. Our present path paces the two presents of a pair against a phase-locked clock
// (core::fg::Schedule) but never bounds how far the PRESENT QUEUE runs ahead of the display.
// The measured symptom is our own [fg] line reporting "issued-interval ... BIMODAL
// (back-to-back presents)": the pair goes out together because nothing made the second one
// wait for a flip slot. NVIDIA's Streamline bounds it in sl.dlss_g with a routine its own
// strings call `throttleFlipQueue` — SetMaximumFrameLatency, GetFrameLatencyWaitableObject,
// then a bounded wait on that object before presenting, with named failures. HARD (binary),
// docs/RESEARCH-STREAMLINE-INTERNALS.md §7.2c: "GetFrameLatencyWaitableObject flip-queue
// throttling".
//
// THE MECHANISM WE ARE DRIVING, read from the two implementations that will actually run:
//
//   * DXGI (MSDN, DXGI_SWAP_CHAIN_FLAG): the waitable object exists only when the swapchain
//     was CREATED with DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT, and then "the
//     swapchain's latency must be set with IDXGISwapChain2::SetMaximumFrameLatency INSTEAD OF
//     IDXGIDevice1::SetMaximumFrameLatency". D3D12 has no IDXGIDevice at all, so on this title
//     the swapchain route is the ONLY route — the device route is attempted once and reported,
//     never assumed. The flag "isn't supported in full-screen mode, unless the render API is
//     Direct3D 12", which we are. ResizeBuffers may "reset or change all DXGI_SWAP_CHAIN_FLAG
//     flags", so the flag must be re-asserted on every ResizeBuffers or the object dies with
//     no error. All HARD, MSDN 2026-06-17 revision.
//   * vkd3d-proton (libs/vkd3d/swapchain.c, master): with the flag it creates a COUNTING
//     SEMAPHORE with initial count 1 and hands out a SYNCHRONIZE-only duplicate;
//     SetFrameLatency(N) releases N-1 more counts; the present-wait thread releases exactly 1
//     per completed present. So a wait ACQUIRES a slot and blocks only when `max_latency`
//     presents are already outstanding. Not waiting is harmless — the count simply grows and
//     vkd3d logs a TRACE. HARD, read from source.
//
// THE PACER INTERACTION, which is the thing most likely to be got wrong. The schedule plans
// ABSOLUTE target times and the presenter sleeps until them, so a throttle wait placed BEFORE
// that sleep is absorbed into it: the two are max(), never a sum, and the throttle can only
// push a present late when the flip queue is genuinely full — precisely when presenting sooner
// would have achieved nothing. Placed AFTER the sleep it would add its block straight onto the
// issued time and double-delay every present. Hence `wait_for_slot` is always called before
// `wait_until`, and the observable of a throttle that is BITING is holds falling while
// catchups rise, not the interval moving.
#pragma once

#include <cstdint>

namespace stray_dlss::core::fg {

// Why the throttle is not waiting. Every one of these is counted and logged once: "armed and
// did nothing" and "refused to arm" must never look alike from the log (the lesson the NR
// history-restore counters bought, CLAUDE.md §5).
enum class ThrottleRefusal
{
	none = 0,        // armed and waiting
	disabled,        // [STRAYDLSS] NgxFGThrottle=0
	no_swapchain2,   // QueryInterface(IDXGISwapChain2) failed: this DXGI is older than 1.3
	not_waitable,    // the swapchain was not CREATED with FRAME_LATENCY_WAITABLE_OBJECT
	no_handle,       // GetFrameLatencyWaitableObject returned null despite the flag
	gave_up,         // too many consecutive timeouts; self-disarmed, loudly
	count
};
const char *throttle_refusal_name(ThrottleRefusal r);

struct ThrottleConfig
{
	bool enabled = false;        // [STRAYDLSS] NgxFGThrottle
	// [STRAYDLSS] NgxFGMaxLatency: slots to allow, 0 = leave the swapchain's own value alone.
	// Our pair is two presents per game frame, so 2 is the value that bounds the queue at one
	// pair; 1 would serialise the pair against the display and is almost certainly too tight.
	unsigned max_latency = 0;
	unsigned timeout_ms = 50;    // [STRAYDLSS] NgxFGThrottleTimeoutMs: bound on ONE wait
	// [STRAYDLSS] NgxFGThrottleGiveUp: consecutive timeouts after which the throttle disarms
	// itself for the session rather than blocking every present forever. 0 = never give up.
	unsigned give_up_after = 8;
};

enum class WaitOutcome
{
	slot_freed = 0, // WAIT_OBJECT_0: a flip slot was acquired
	timed_out,      // WAIT_TIMEOUT: nothing freed within timeout_ms
	failed,         // WAIT_FAILED / WAIT_ABANDONED: the handle is broken
};

struct ThrottleState
{
	bool armed = false;                 // a waitable handle is held and waits are being made
	ThrottleRefusal refusal = ThrottleRefusal::disabled;
	std::uint64_t waits = 0;            // waits performed
	std::uint64_t slots = 0;            // waits that got a slot
	std::uint64_t timeouts = 0;
	std::uint64_t failures = 0;
	std::uint64_t skipped = 0;          // presents that deliberately did not wait (drain, teardown)
	std::uint64_t blocked_ns = 0;       // total time blocked in waits
	std::uint64_t blocked_max_ns = 0;
	unsigned consecutive_timeouts = 0;
	unsigned max_latency_requested = 0; // what we asked SetMaximumFrameLatency for (0 = untouched)
	unsigned max_latency_before = 0;    // GetMaximumFrameLatency before, ~0u = unreadable
	unsigned max_latency_after = 0;     // and after
};

// Whether this present should block on the waitable object at all.
bool should_wait(const ThrottleConfig &cfg, const ThrottleState &s);

// What DXGI reported at arming time, in the order the shell learns it.
struct ArmInputs
{
	bool enabled = false;
	bool has_swapchain2 = false;
	bool waitable_flag = false; // the CREATED desc carried FRAME_LATENCY_WAITABLE_OBJECT
	bool have_handle = false;   // GetFrameLatencyWaitableObject returned non-null
};
// The refusal (none = arm). Ordered so the FIRST thing that is wrong is the one reported.
ThrottleRefusal arm_verdict(const ArmInputs &in);

// Folds one wait's result in. Returns true exactly once: on the call that makes the throttle
// GIVE UP, so the shell logs the loud line and releases the handle.
bool note_wait(const ThrottleConfig &cfg, ThrottleState &s, WaitOutcome outcome, std::uint64_t blocked_ns);

double throttle_blocked_mean_ms(const ThrottleState &s);

// ---- the creation flag ([STRAYDLSS] NgxFGWaitableSwapChain) ----
//
// The waitable object cannot be added after the fact, so if the game's own swapchain is to
// have one we must OR the flag into the desc the game hands DXGI. Stray creates the swapchain,
// not us, which is why this is a separate opt-in knob from the throttle itself.
enum class FlagVerdict
{
	disabled = 0,     // the knob is off: the desc is untouched
	already_present,  // the game asked for it itself; nothing to do, and nothing to undo later
	added,            // we added it
	not_flip_model,   // the swap effect is bitblt: the flag is illegal, refuse rather than fail creation
	count
};
const char *flag_verdict_name(FlagVerdict v);

// `swap_effect` is DXGI_SWAP_EFFECT_*; only the two flip-model values (3 FLIP_SEQUENTIAL,
// 4 FLIP_DISCARD) may carry the flag. `*out_flags` is always written.
FlagVerdict plan_creation_flags(bool enabled, unsigned swap_effect, unsigned in_flags, unsigned *out_flags);

// ResizeBuffers may "reset or change all DXGI_SWAP_CHAIN_FLAG flags" (MSDN), and UE 4.27 passes
// the flags IT asked for — which do not include ours. Re-assert the flag on every resize, but
// only when WE added it: a game that asked for it itself needs no help, and a game that did not
// get it must not have it appear at a resize.
unsigned resize_flags(bool we_added_it, unsigned game_flags);

} // namespace stray_dlss::core::fg
