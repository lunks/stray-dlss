#include <doctest/doctest.h>

#include "core/mv_census.hpp"

#include <cstring>
#include <string>

using namespace stray_dlss;

namespace {

// A window whose numbers are internally consistent, so the invariants pass and the report is
// allowed to print percentages.
mvcensus::Census make_good()
{
	std::uint32_t raw[mvcensus::kCounterCount] = {};
	raw[mvcensus::kTotal] = 1000;
	raw[mvcensus::kSparse] = 120;
	raw[mvcensus::kCamera] = 880;
	raw[mvcensus::kCameraReject] = 4;
	raw[mvcensus::kFar] = 50;
	raw[mvcensus::kFarReject] = 2;
	raw[mvcensus::kSparseSaturated] = 1;
	raw[mvcensus::kSparseStill] = 30;
	raw[mvcensus::kSparseMoving] = 90;
	raw[mvcensus::kMvLt025] = 400;
	raw[mvcensus::kMvLt1] = 300;
	raw[mvcensus::kMvLt4] = 200;
	raw[mvcensus::kMvLt32] = 90;
	raw[mvcensus::kMvGe32] = 10;
	raw[mvcensus::kNonFinite] = 0;

	mvcensus::Census c;
	c.add(raw, 10);
	return c;
}

} // namespace

TEST_CASE("mv census: the counter names line up with the enum, one for one")
{
	// The shader indexes the SAME buffer by these ordinals. If the table and the enum ever
	// diverge, every number in the report is silently relabelled — which is exactly the class
	// of failure this project keeps paying for.
	CHECK(std::string(mvcensus::kCounterNames[mvcensus::kTotal]) == "total");
	CHECK(std::string(mvcensus::kCounterNames[mvcensus::kSparse]) == "sparse");
	CHECK(std::string(mvcensus::kCounterNames[mvcensus::kCamera]) == "camera");
	CHECK(std::string(mvcensus::kCounterNames[mvcensus::kNonFinite]) == "non_finite");
	CHECK(mvcensus::kCounterCount == 16);
	for (int i = 0; i < mvcensus::kCounterCount; ++i)
		CHECK(mvcensus::kCounterNames[i] != nullptr);
}

TEST_CASE("mv census: accumulation widens the GPU's uint32 counters")
{
	std::uint32_t raw[mvcensus::kCounterCount] = {};
	raw[mvcensus::kTotal] = 4000000000u; // close to the uint32 rail
	raw[mvcensus::kSparse] = 1000000000u;
	raw[mvcensus::kCamera] = 3000000000u;
	raw[mvcensus::kMvLt025] = 4000000000u;

	mvcensus::Census c;
	c.add(raw, 100);
	c.add(raw, 100);

	// 8e9 does not fit in 32 bits. If this ever reads 3705032704 the accumulator narrowed.
	CHECK(c.c[mvcensus::kTotal] == 8000000000ull);
	CHECK(c.frames == 200);
	CHECK(c.windows == 2);

	c.clear();
	CHECK(c.c[mvcensus::kTotal] == 0);
	CHECK(c.frames == 0);
	CHECK(c.windows == 0);
}

TEST_CASE("mv census: the invariants hold on a consistent window")
{
	CHECK(mvcensus::check(make_good()) == mvcensus::Invariant::ok);
}

TEST_CASE("mv census: an empty window is refused rather than divided by")
{
	mvcensus::Census c;
	CHECK(mvcensus::check(c) == mvcensus::Invariant::empty);
	// And nothing in the report divides by it.
	CHECK(mvcensus::percent(5, 0) == 0.0);
}

TEST_CASE("mv census: every way the instrument can be broken is NAMED, not smoothed over")
{
	SUBCASE("the two branches must partition the frame")
	{
		mvcensus::Census c = make_good();
		c.c[mvcensus::kSparse] += 1;
		CHECK(mvcensus::check(c) == mvcensus::Invariant::branches_dont_sum);
	}
	SUBCASE("a rejection cannot exceed the branch it came from")
	{
		mvcensus::Census c = make_good();
		c.c[mvcensus::kCameraReject] = c.c[mvcensus::kCamera] + 1;
		CHECK(mvcensus::check(c) == mvcensus::Invariant::reject_exceeds);
	}
	SUBCASE("nor can a far rejection exceed the far pixels")
	{
		mvcensus::Census c = make_good();
		c.c[mvcensus::kFarReject] = c.c[mvcensus::kFar] + 1;
		CHECK(mvcensus::check(c) == mvcensus::Invariant::reject_exceeds);
	}
	SUBCASE("a subset of the sparse pixels cannot exceed them")
	{
		mvcensus::Census c = make_good();
		c.c[mvcensus::kSparseMoving] = c.c[mvcensus::kSparse] + 1;
		CHECK(mvcensus::check(c) == mvcensus::Invariant::subset_exceeds);
	}
	SUBCASE("the magnitude buckets must partition the frame too")
	{
		mvcensus::Census c = make_good();
		c.c[mvcensus::kMvGe32] += 1;
		CHECK(mvcensus::check(c) == mvcensus::Invariant::buckets_dont_sum);
	}
	SUBCASE("every invariant has a name")
	{
		for (int i = 0; i < static_cast<int>(mvcensus::Invariant::count); ++i)
			CHECK(std::strlen(
				mvcensus::invariant_name(static_cast<mvcensus::Invariant>(i))) > 1);
	}
}

TEST_CASE("mv census: a BROKEN window prints no percentages at all")
{
	// The whole point. A plausible-looking wrong percentage gets quoted; this project's own
	// history is full of them. The report must say the instrument is unusable and stop.
	mvcensus::Census c = make_good();
	c.c[mvcensus::kSparse] += 7;

	char buf[768] = {};
	mvcensus::format_report(buf, sizeof(buf), c);
	const std::string s = buf;
	CHECK(s.find("UNUSABLE") != std::string::npos);
	CHECK(s.find("branches-dont-sum") != std::string::npos);
	CHECK(s.find("engine-written") == std::string::npos);
	CHECK(s.find('%') == std::string::npos);
}

TEST_CASE("mv census: a good window reports the density both ways round")
{
	char buf[768] = {};
	mvcensus::format_report(buf, sizeof(buf), make_good());
	const std::string s = buf;
	CHECK(s.find("engine-written 12.00%") != std::string::npos);
	CHECK(s.find("reconstructed 88.00%") != std::string::npos);
	CHECK(s.find("UNUSABLE") == std::string::npos);
}

TEST_CASE("mv census: the status block carries the derived percentages so no reader divides")
{
	char buf[2048] = {};
	const int n = mvcensus::format_status(buf, sizeof(buf), make_good());
	CHECK(n > 0);
	const std::string s = buf;
	CHECK(s.find("mv_census_windows=1\n") != std::string::npos);
	CHECK(s.find("mv_census_frames=10\n") != std::string::npos);
	CHECK(s.find("mv_census_invariant=ok\n") != std::string::npos);
	CHECK(s.find("mv_total=1000\n") != std::string::npos);
	CHECK(s.find("mv_sparse=120\n") != std::string::npos);
	CHECK(s.find("mv_engine_written_pct=12.000\n") != std::string::npos);
	CHECK(s.find("mv_reconstructed_pct=88.000\n") != std::string::npos);
	// The reserved slot is not published; a name nobody can interpret invites interpretation.
	CHECK(s.find("mv_reserved=") == std::string::npos);
}

TEST_CASE("mv census: neither formatter overruns a short buffer")
{
	char small[24];
	std::memset(small, 0x7f, sizeof(small));
	mvcensus::format_report(small, sizeof(small), make_good());
	CHECK(small[sizeof(small) - 1] == '\0');

	std::memset(small, 0x7f, sizeof(small));
	mvcensus::format_status(small, sizeof(small), make_good());
	CHECK(small[sizeof(small) - 1] == '\0');

	CHECK(mvcensus::format_report(nullptr, 0, make_good()) == 0);
	CHECK(mvcensus::format_status(nullptr, 0, make_good()) == 0);
}

TEST_CASE("mv census: the uint32 window ceiling is arithmetic, not a comment")
{
	// 1920x1080 render. The shipped MvStatsFrames of 300 must sit well inside it, and a future
	// edit raising the window past ~2071 frames must be caught by the live warning rather than
	// by someone noticing the percentages look odd.
	const std::uint64_t px = 1920ull * 1080ull;
	const std::uint64_t safe = mvcensus::safe_window_frames(px);
    CHECK(safe == 0xFFFFFFFFull / px);
	CHECK(safe > 2000);
	CHECK(safe < 2100);
	CHECK(300u < safe);
	CHECK(mvcensus::safe_window_frames(0) == 0);
}

TEST_CASE("mv census: the shader's classification thresholds are pinned")
{
	// Both are duplicated as literals in shaders/mv_resolve.hlsl, which fxc compiles and no
	// test can read. Pinning them here at least makes a silent divergence a deliberate edit.
	CHECK(mvcensus::kMovingThresholdPx == doctest::Approx(0.5f));
	CHECK(mvcensus::kStillThresholdPx == doctest::Approx(0.0625f));
}
