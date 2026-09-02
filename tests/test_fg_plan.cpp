// The frame-generation arithmetic (src/core/fg_plan.hpp). Every rule here is one the game
// cannot report a violation of: a wrong mirror is a stale frame, a wrong pacer is judder, a
// wrong gate is a black frame on screen. So each is pinned.
#include "core/fg_plan.hpp"

#include <doctest/doctest.h>

using namespace stray_dlss::core::fg;

TEST_CASE("GameIndexMirror: UE 4.27's counter - 0 after reset, +1 mod count per Present, reset on resize (facts §30.4)")
{
	GameIndexMirror m;
	CHECK_FALSE(m.valid());
	m.reset(3);
	CHECK(m.valid());
	CHECK(m.current() == 0);
	// The game renders frame k into buffer k % 3 and presents it; the hook consumes
	// current() BEFORE advancing.
	CHECK(m.on_present() == 1);
	CHECK(m.current() == 1);
	CHECK(m.on_present() == 2);
	CHECK(m.on_present() == 0); // wrapped
	CHECK(m.presents == 3);
	// Two presents of OURS per game frame never touch the mirror - only the game's Present
	// advances it. A ResizeBuffers (any count) restarts at 0 with the new count.
	m.reset(4);
	CHECK(m.current() == 0);
	CHECK(m.presents == 0);
	CHECK(m.count == 4);
	for (int i = 0; i < 4; ++i)
		m.on_present();
	CHECK(m.current() == 0);
	// A zero count (no swapchain) never divides by zero.
	GameIndexMirror z;
	CHECK(z.on_present() == 0);
}

TEST_CASE("Pacer: no estimate on the first sample; EMA converges; delay is half the interval, clamped; a hitch is counted, not folded in")
{
	Pacer p;
	p.cfg.alpha = 0.5;
	const std::uint64_t ms = 1'000'000;
	CHECK(p.on_game_present(100 * ms) == 0); // first sample: nothing to estimate from
	CHECK(p.samples == 0);
	CHECK(p.on_game_present(110 * ms) == 5 * ms); // one 10 ms interval -> 5 ms
	CHECK(p.samples == 1);
	CHECK(p.on_game_present(130 * ms) == 7'500'000); // EMA(10, 20) at alpha .5 = 15 -> 7.5
	// A 300 ms stall must not become the estimate: the next delay is still from the EMA.
	const std::uint64_t before = static_cast<std::uint64_t>(p.interval_ns);
	CHECK(p.on_game_present(430 * ms) == before / 2);
	CHECK(p.hitches == 1);
	CHECK(p.samples == 2);
	// The clamp: a 90 ms cadence (below the hitch threshold) would ask for 45 ms; the cap
	// holds it at 25 ms so a slow scene never doubles its own latency.
	Pacer slow;
	slow.on_game_present(0);
	slow.on_game_present(90 * ms);
	CHECK(slow.on_game_present(180 * ms) == slow.cfg.max_delay_ns);
	// A non-monotonic clock (same or earlier timestamp) is ignored, not negative.
	Pacer back;
	back.on_game_present(50 * ms);
	CHECK(back.on_game_present(40 * ms) == 0);
	CHECK(back.samples == 0);
	// reset() forgets everything.
	p.reset();
	CHECK(p.interval_ns == 0.0);
	CHECK(p.on_game_present(1000 * ms) == 0);
}

TEST_CASE("IntervalHistogram: 1 ms buckets, overflow, percentiles, and the bimodal (back-to-back) signature")
{
	IntervalHistogram h;
	CHECK(h.percentile_ms(0.5) == IntervalHistogram::kBuckets - 1); // empty
	CHECK(h.second_peak_ms() == -1);
	const std::uint64_t ms = 1'000'000;
	for (int i = 0; i < 100; ++i)
		h.add(8 * ms + 200'000); // 8.2 ms -> bucket 8
	CHECK(h.total == 100);
	CHECK(h.bucket[8] == 100);
	CHECK(h.percentile_ms(0.5) == 8);
	CHECK(h.percentile_ms(0.99) == 8);
	CHECK(h.mean_ms() == doctest::Approx(8.2));
	CHECK(h.second_peak_ms() == -1); // unimodal: paced correctly
	h.add(500 * ms);                 // overflow lands in the last bucket
	CHECK(h.bucket[IntervalHistogram::kBuckets - 1] == 1);
	// Back-to-back presents: half the intervals near 0, half near the game's interval.
	IntervalHistogram b;
	for (int i = 0; i < 50; ++i)
	{
		b.add(300'000); // 0.3 ms
		b.add(16 * ms);
	}
	CHECK(b.second_peak_ms() >= 0);
	CHECK((b.second_peak_ms() == 0 || b.second_peak_ms() == 16));
	// Two adjacent buckets (jitter around one mode) are NOT two peaks.
	IntervalHistogram j;
	for (int i = 0; i < 50; ++i)
	{
		j.add(8 * ms);
		j.add(9 * ms);
	}
	CHECK(j.second_peak_ms() == -1);
	j.clear();
	CHECK(j.total == 0);
}

TEST_CASE("Epoch: a reconfiguration suspends, and work armed before it is refused after it")
{
	Epoch e;
	const std::uint64_t armed = e.value;
	CHECK(e.live(armed));
	e.begin_reconfigure();
	CHECK_FALSE(e.live(armed)); // suspended
	e.end_reconfigure();
	CHECK_FALSE(e.live(armed)); // a different epoch now
	CHECK(e.live(e.value));
}

TEST_CASE("decide: every refusal in priority order, and the one path that presents a generated frame")
{
	PresentState s;
	CHECK(decide(s) == Refusal::off);
	s.enabled = true;
	CHECK(decide(s) == Refusal::not_armed);
	s.armed = true;
	s.suspended = true;
	CHECK(decide(s) == Refusal::suspended);
	s.suspended = false;
	CHECK(decide(s) == Refusal::no_previous_frame);
	s.have_previous_frame = true;
	CHECK(decide(s) == Refusal::source_missing);
	s.generated_valid = true;
	s.epoch = 3;
	s.generated_epoch = 2;
	CHECK(decide(s) == Refusal::stale_epoch);
	s.generated_epoch = 3;
	CHECK(decide(s) == Refusal::not_validated);
	s.validated = true;
	CHECK(decide(s) == Refusal::none);
	// Names exist for every refusal, for the log and the status file.
	for (int i = 0; i < static_cast<int>(Refusal::count); ++i)
		CHECK(refusal_name(static_cast<Refusal>(i))[0] != '?');
}

TEST_CASE("CropJudge: black is caught on the first look; stale needs the real frame to move while the generated one does not, repeatedly; identical is informational")
{
	CropJudge j;
	CropStats gen{ 0x1111, 0, 4096 };
	CropStats real{ 0x2222, 4000, 4096 };
	CHECK(j.judge(gen, real) == CropVerdict::black); // 0 of 4096 nonzero
	gen.nonzero = 20;                                // 0.5% - still black
	CHECK(j.judge(gen, real) == CropVerdict::black);
	gen.nonzero = 3000;
	CHECK(j.judge(gen, real) == CropVerdict::ok);
	// The real frame changes every look, the generated hash never does: stale after 3.
	for (int i = 0; i < 2; ++i)
	{
		real.hash += 1;
		CHECK(j.judge(gen, real) == CropVerdict::ok);
	}
	real.hash += 1;
	CHECK(j.judge(gen, real) == CropVerdict::stale);
	CHECK(j.stale_run == 3);
	// One frame where the generated hash moves breaks the run.
	real.hash += 1;
	gen.hash += 1;
	CHECK(j.judge(gen, real) == CropVerdict::ok);
	CHECK(j.stale_run == 0);
	// A static camera: neither moves - that is not stale (nothing to interpolate).
	CHECK(j.judge(gen, real) == CropVerdict::ok);
	CHECK(j.judge(gen, real) == CropVerdict::ok);
	// Generated == real byte for byte is reported, not refused.
	gen.hash = real.hash;
	CHECK(j.judge(gen, real) == CropVerdict::identical);
	// An empty crop is black, never a division by zero.
	CropJudge z;
	CHECK(z.judge(CropStats{}, CropStats{}) == CropVerdict::black);
	for (int i = 0; i < static_cast<int>(CropVerdict::count); ++i)
		CHECK(crop_verdict_name(static_cast<CropVerdict>(i))[0] != '?');
}
