// The frame-generation arithmetic (src/core/fg_plan.hpp). Every rule here is one the game
// cannot report a violation of: a wrong mirror is a stale frame, a wrong pacer is judder, a
// wrong gate is a black frame on screen. So each is pinned.
#include "core/fg_plan.hpp"

#include <doctest/doctest.h>

using namespace stray_dlss::core::fg;

TEST_CASE("GameIndexMirror: UE 4.27's counter - 0 after reset, +1 mod count per Present, reset on resize (facts §31.4)")
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
	// The first non-black look has nothing to compare against: neutral, never "good" (a stale
	// output validated on CI on exactly such looks before this class existed).
	CHECK(j.judge(gen, real) == CropVerdict::first);
	CHECK(crop_weight(CropVerdict::first) == CropWeight::neutral);
	// The real frame changes every look, the generated hash never does: suspect, suspect, STALE.
	for (int i = 0; i < 2; ++i)
	{
		real.hash += 1;
		CHECK(j.judge(gen, real) == CropVerdict::suspect);
	}
	CHECK(crop_weight(CropVerdict::suspect) == CropWeight::neutral);
	real.hash += 1;
	CHECK(j.judge(gen, real) == CropVerdict::stale);
	CHECK(crop_weight(CropVerdict::stale) == CropWeight::bad);
	CHECK(j.stale_run == 3);
	// One frame where the generated hash moves breaks the run and is the first GOOD look.
	real.hash += 1;
	gen.hash += 1;
	CHECK(j.judge(gen, real) == CropVerdict::ok);
	CHECK(crop_weight(CropVerdict::ok) == CropWeight::good);
	CHECK(j.stale_run == 0);
	// A static camera: neither moves - that is not stale (nothing to interpolate).
	CHECK(j.judge(gen, real) == CropVerdict::ok);
	CHECK(j.judge(gen, real) == CropVerdict::ok);
	// Generated == real byte for byte is reported, not refused.
	gen.hash = real.hash;
	CHECK(j.judge(gen, real) == CropVerdict::identical);
	CHECK(crop_weight(CropVerdict::identical) == CropWeight::good);
	CHECK(crop_weight(CropVerdict::black) == CropWeight::bad);
	// Two output textures alternating stale content (A, B, A, B) while the real frame moves:
	// each look differs from the previous one, but not from the one before - not motion.
	CropJudge alt;
	CropStats a{ 0xAAAA, 3000, 4096 }, b{ 0xBBBB, 3000, 4096 }, r{ 0x1, 4000, 4096 };
	CHECK(alt.judge(a, r) == CropVerdict::first);
	r.hash = 0x2;
	CHECK(alt.judge(b, r) == CropVerdict::ok); // only one look behind: cannot know yet
	r.hash = 0x3;
	CHECK(alt.judge(a, r) == CropVerdict::suspect); // equals the look before last
	r.hash = 0x4;
	CHECK(alt.judge(b, r) == CropVerdict::suspect);
	r.hash = 0x5;
	CHECK(alt.judge(a, r) == CropVerdict::stale);
	// An empty crop is black, never a division by zero.
	CropJudge z;
	CHECK(z.judge(CropStats{}, CropStats{}) == CropVerdict::black);
	for (int i = 0; i < static_cast<int>(CropVerdict::count); ++i)
		CHECK(crop_verdict_name(static_cast<CropVerdict>(i))[0] != '?');
}

TEST_CASE("camera constants: invert4x4 round-trips a projection-like matrix and refuses a singular one; FOV and basis from UE4's matrices")
{
	// UE4-shaped reversed-Z infinite projection (row-major, row-vector): [0][0]=a, [1][1]=b,
	// [2][3]=1, [3][2]=near. Its inverse must satisfy M * inv = I.
	const float near_plane = 10.0f;
	const float a = 0.75f, b = 1.3333333f;
	float m[16] = { a, 0, 0, 0, 0, b, 0, 0, 0, 0, 0, 1, 0, 0, near_plane, 0 };
	float inv[16];
	REQUIRE(invert4x4(m, inv));
	for (int r = 0; r < 4; ++r)
		for (int c = 0; c < 4; ++c)
		{
			double s = 0;
			for (int k = 0; k < 4; ++k)
				s += static_cast<double>(m[r * 4 + k]) * inv[k * 4 + c];
			CHECK(s == doctest::Approx(r == c ? 1.0 : 0.0).epsilon(1e-5));
		}
	float singular[16] = {};
	float out[16] = { 42 };
	CHECK_FALSE(invert4x4(singular, out));
	CHECK(out[0] == 42.0f); // untouched
	// Vertical FOV: b = 1/tan(fov/2) -> fov = 2*atan(1/b); b = 1 is exactly 90 degrees.
	float ninety[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0, 0, 10, 0 };
	CHECK(vertical_fov_radians(ninety) == doctest::Approx(1.5707963).epsilon(1e-5));
	CHECK(vertical_fov_radians(m) == doctest::Approx(2.0 * 0.6435011).epsilon(1e-4)); // atan(0.75)=0.6435
	float bad[16] = {};
	CHECK(vertical_fov_radians(bad) == 0.0f);
	// Basis: an identity view (world axes = view axes) yields the unit vectors, and a
	// rotation about Y (camera yawed) moves right/forward together as the columns say.
	float ident[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
	CameraBasis cb = camera_basis(ident);
	CHECK((cb.right[0] == 1.0f && cb.up[1] == 1.0f && cb.fwd[2] == 1.0f));
	// A matrix whose columns are known vectors: column 0 = (0,0,1), column 2 = (-1,0,0).
	float yawed[16] = { 0, 0, -1, 0, 0, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0, 1 };
	cb = camera_basis(yawed);
	CHECK((cb.right[0] == 0.0f && cb.right[2] == 1.0f));
	CHECK((cb.fwd[0] == -1.0f && cb.fwd[2] == 0.0f));
}
