// Frame-time and OUR-CPU-share instrumentation.
//
// The question this exists to answer: the user sees ~30 fps at 4K/50% on a 4090, and the only
// honest way to know whether that cost is ours or the game's is to measure our own code
// directly and hand back a differential the operator can compare against an inert run
// (EnableNGX=0 MvDispatch=0).
//
// MEASUREMENT DISCIPLINE — correctness over granularity (a wrong number is worse than a coarse
// one), so:
//   * Frame times are sampled ONCE per present with a steady clock. Every frame, no sampling:
//     one clock read per present is free, and the worst-frame figure is only meaningful if no
//     frame is skipped.
//   * Our CPU buckets are timed on EVERY occurrence, not statistically sampled — because every
//     timed site is already low-frequency. intercept_dispatch is the only hot one, and its
//     timer starts AFTER the cheap size gate that rejects the thousands of small dispatches,
//     so the per-dispatch early-out pays nothing and only the few dozen full-screen-ish
//     dispatches per frame are instrumented. That keeps the perturbation far below the signal
//     while leaving the numbers exact rather than extrapolated.
//   * The buckets NEST: kDispatchPath spans the whole post-gate intercept_dispatch body, which
//     CONTAINS the resolve/evaluate/restore work. The report subtracts the nested buckets from
//     it to attribute a non-overlapping "intercept" figure, so the parts sum to the total
//     instead of double-counting.
//
// One caveat that keeps the percentages honest: bucket time is summed across every recording
// thread, while the frame time it is compared against is wall clock. If UE ever records our
// intercepted dispatches on several threads at once, the summed CPU can exceed the frame's
// wall time and a percentage can pass 100 — that means "this much CPU work, possibly in
// parallel", not "this fraction of the frame was stalled". In practice the dominant sites
// (the NGX evaluate, the resolves, the restore) all happen once per frame on the single
// thread that records the TAA dispatch, so the figures read as single-threaded.
//
// What this does NOT measure, stated so the numbers are not over-read: the ReShade event taps
// that run outside intercept_dispatch (the pass/G-buffer finder's per-draw and per-bind
// handlers, active only with their flags on). The operator's inert-run differential captures
// total add-on cost including those; these buckets explain where the DISPATCH-PATH share goes.
//
// [STRAYDLSS] PerfLog, default ON — it is cheap enough to leave on, and a perf report nobody
// enabled is a round trip wasted.
#pragma once

#include <cstdint>

namespace stray_dlss::perf {

enum Bucket
{
	kDispatchPath = 0, // whole post-gate intercept_dispatch body (CONTAINS the buckets below)
	kMvResolve,        // mv::record
	kGBufferResolve,   // gbr::record (the RR guide resolve)
	kNgxSr,            // ngx::evaluate      (DLSS SR)
	kNgxRr,            // ngx::evaluate_rr   (DLSS Ray Reconstruction)
	kNgxNr,            // nr::apply          (DLSS Neural Rendering, feature 18)
	kRestore,          // restore_game_compute_state
	kBucketCount,
};

void set_enabled(bool enabled);
bool enabled();

// Adds nanoseconds to a bucket. Lock-free; safe from any recording thread.
void add(Bucket bucket, std::uint64_t nanos);

// RAII timer. Reads the clock twice only when enabled, so a disabled build pays one atomic
// load per scope and nothing else.
class Scope
{
public:
	explicit Scope(Bucket bucket) noexcept;
	~Scope() noexcept;
	Scope(const Scope &) = delete;
	Scope &operator=(const Scope &) = delete;

private:
	Bucket bucket_;
	bool active_;
	std::uint64_t start_ns_;
};

// Called once per present. `dispatches_total` and `large_dispatches_total` are cumulative
// counters the caller already maintains (we take deltas, adding no hot-path cost). Emits the
// interval report every kReportInterval presents.
void on_present(std::uint64_t dispatches_total, std::uint64_t large_dispatches_total);

} // namespace stray_dlss::perf
