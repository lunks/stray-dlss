// The guide-extent latch that forces a reset of NR's own temporal accumulation when the guide
// grid moves under it. (The post-tonemap hook sites this file also covered were removed on
// 2026-09-02; NR runs at the TAA site only.)
#include <doctest/doctest.h>

#include "core/nr_hook_plan.hpp"

using namespace stray_dlss::nrplan;

TEST_CASE("the guide-extent latch does NOT reset on the first frame")
{
	// The reference's own rule, `resetGuideHistory = (latched != 0)`: there is no accumulated
	// history to invalidate before the first evaluate, and resetting anyway would put a spurious
	// reset into every session.
	GuideExtentLatch latch;
	CHECK(latch_guide_extent(latch, 1920, 1080) == false);
	CHECK(latch.width == 1920);
	CHECK(latch.height == 1080);
}

TEST_CASE("the guide-extent latch forces one reset when the guide grid moves, then settles")
{
	// The concrete case: a mid-session screen-percentage change from 50% to 70% at a fixed 4K
	// output. The FEATURE is keyed on the colour grid, which does not change, so nothing else in
	// the pipeline notices that MVecScale went from 2.0 to 1.42857 underneath a history that was
	// accumulated against the old grid.
	GuideExtentLatch latch;
	CHECK(latch_guide_extent(latch, 1920, 1080) == false); // first observation
	CHECK(latch_guide_extent(latch, 1920, 1080) == false); // steady state
	CHECK(latch_guide_extent(latch, 2688, 1512));          // 50% -> 70%: reset exactly once
	CHECK(latch_guide_extent(latch, 2688, 1512) == false); // and only once
	CHECK(latch_guide_extent(latch, 1920, 1080));          // back again: reset again
	CHECK(latch_guide_extent(latch, 1920, 1080) == false);
}

TEST_CASE("the guide-extent latch reacts to either axis alone")
{
	GuideExtentLatch latch;
	CHECK(latch_guide_extent(latch, 1920, 1080) == false);
	CHECK(latch_guide_extent(latch, 1920, 1081));
	CHECK(latch_guide_extent(latch, 1921, 1081));
}
