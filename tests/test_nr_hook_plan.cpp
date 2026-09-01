// The gate that decides whether a post-tonemap frame may be handed to DLSS Neural Rendering,
// and the two rules that go with it: the `preui` render-target boundary, and the guide-extent
// latch that forces a reset of NR's own temporal accumulation.
//
// Every one of these refusals is a thing that would otherwise be discovered on the user's
// machine, one expensive round trip at a time (CLAUDE.md §0.1). Pinning them here is the whole
// reason the logic lives in src/core/.
#include <doctest/doctest.h>

#include "core/nr_hook_plan.hpp"

#include <cstring>
// The range-for over a braced list in the mode round-trip test needs this explicitly on
// libstdc++; libc++ happens to pull it in transitively, which is exactly the kind of
// difference the Linux lane exists to catch.
#include <initializer_list>

using namespace stray_dlss::nrplan;

namespace {

constexpr float kTol = 1e-5f;

bool near(float a, float b, float tol = kTol)
{
	return (a > b ? a - b : b - a) <= tol;
}

// A colour target that passes everything, so each test can spoil exactly one thing.
ColourDesc good_colour()
{
	ColourDesc c;
	c.width = 3840;   // CLAUDE.md §2.1: the observed swapchain extent
	c.height = 2160;
	c.mip_levels = 1;
	c.array_size = 1;
	c.sample_count = 1;
	c.typed_uav_view = true;
	c.typed_uav_load = true;
	c.typed_uav_store = true;
	c.live = true;
	return c;
}

// Guides as the TAA path publishes them at 50% screen percentage (CLAUDE.md §2.3.1).
// `sequence` is a publication counter, so 0 means "never published".
GuideState good_guides(std::uint64_t sequence = 100)
{
	GuideState g;
	g.published = true;
	g.have_depth = true;
	g.have_motion = true;
	g.sequence = sequence;
	g.render_width = 1920;
	g.render_height = 1080;
	return g;
}

} // namespace

TEST_CASE("the hook mode defaults to taa for every unrecognised value")
{
	// The default MUST be the shipped behaviour: a typo in ReShade.ini must not silently move
	// the hook to an unproven site.
	CHECK(hook_mode_from_string(nullptr) == HookMode::taa);
	CHECK(hook_mode_from_string("") == HookMode::taa);
	CHECK(hook_mode_from_string("taa") == HookMode::taa);
	CHECK(hook_mode_from_string("Present") == HookMode::taa); // case-sensitive, like NgxNRTopology
	CHECK(hook_mode_from_string("pre-ui") == HookMode::taa);
	CHECK(hook_mode_from_string("nonsense") == HookMode::taa);

	CHECK(hook_mode_from_string("present") == HookMode::present);
	CHECK(hook_mode_from_string("preui") == HookMode::preui);
}

TEST_CASE("mode names round-trip, so a log line is unambiguous about which path drew the image")
{
	for (const HookMode m : { HookMode::taa, HookMode::present, HookMode::preui })
		CHECK(hook_mode_from_string(hook_mode_name(m)) == m);
}

TEST_CASE("the HDR codec applies to the TAA site only")
{
	// The codec turns raw unbounded pre-exposed linear HDR into a display-referred proxy. Both
	// post-tonemap sites are ALREADY display-referred, so running it there would apply the
	// soft-clip and the sRGB encode a second time.
	CHECK(is_post_tonemap(HookMode::taa) == false);
	CHECK(is_post_tonemap(HookMode::present));
	CHECK(is_post_tonemap(HookMode::preui));
}

TEST_CASE("every plan result has a distinct name")
{
	// The names reach the user through the periodic counter line; two reasons sharing a name
	// would make a refusal undiagnosable from the log alone.
	for (int i = 0; i < kPlanResultCount; ++i)
	{
		const char *a = plan_result_name(static_cast<PlanResult>(i));
		REQUIRE(a != nullptr);
		CHECK(std::strcmp(a, "?") != 0);
		for (int j = i + 1; j < kPlanResultCount; ++j)
			CHECK(std::strcmp(a, plan_result_name(static_cast<PlanResult>(j))) != 0);
	}
	CHECK(std::strcmp(plan_result_name(static_cast<PlanResult>(kPlanResultCount)), "?") == 0);
}

TEST_CASE("a healthy 4K back buffer over 1080p guides is accepted, with MVecScale 2.0")
{
	const Plan p = plan_post_tonemap(good_colour(), good_guides(7), 6);
	CHECK(p.result == PlanResult::ok);
	CHECK(p.width == 3840);
	CHECK(p.height == 2160);
	// THE REGRESSION THIS PINS: MVecScale is the colour/guide ratio, not 1.0. The snippet works
	// on the colour grid while our vectors are on the guide grid, and it takes the colour rect,
	// the mvec rect and this scale as three independent values.
	CHECK(near(p.mvec_scale_x, 2.0f));
	CHECK(near(p.mvec_scale_y, 2.0f));
}

TEST_CASE("MVecScale is computed, never hardcoded: 70% screen percentage gives 1.42857")
{
	// This project runs both 50% and 70% (CLAUDE.md §5, "70% is the highest working setting").
	GuideState g = good_guides(1);
	g.render_width = 2688;
	g.render_height = 1512;
	const Plan p = plan_post_tonemap(good_colour(), g, 0);
	CHECK(p.result == PlanResult::ok);
	CHECK(near(p.mvec_scale_x, 3840.0f / 2688.0f));
	CHECK(near(p.mvec_scale_y, 2160.0f / 1512.0f));
	CHECK(near(p.mvec_scale_x, 1.4285714f, 1e-4f));
}

TEST_CASE("a dead colour resource is refused before anything else is looked at")
{
	// ReShade's view->resource map outlives the resource on D3D12 and GetDesc() happily returns
	// plausible values off freed memory (CLAUDE.md §5), so liveness is checked first and the
	// rest of the desc is not to be trusted until it passes.
	ColourDesc c = good_colour();
	c.live = false;
	c.width = 0; // would otherwise report zero-extent
	CHECK(plan_post_tonemap(c, good_guides(), 0).result == PlanResult::no_colour);
}

TEST_CASE("a zero-sized target is refused (a minimised or mid-resize swapchain)")
{
	ColourDesc c = good_colour();
	c.width = 0;
	CHECK(plan_post_tonemap(c, good_guides(), 0).result == PlanResult::zero_extent);
	c = good_colour();
	c.height = 0;
	CHECK(plan_post_tonemap(c, good_guides(), 0).result == PlanResult::zero_extent);
}

TEST_CASE("a mipped, arrayed or multisampled colour target is refused, never passed through")
{
	// A mipped input to feature 18 is a documented DXGI_ERROR_DEVICE_HUNG arriving seconds
	// later, not an error return. A log line costs a frame; a hung GPU costs a power cycle.
	ColourDesc c = good_colour();
	c.mip_levels = 4;
	CHECK(plan_post_tonemap(c, good_guides(), 0).result == PlanResult::mipped_colour);
	c = good_colour();
	c.array_size = 2;
	CHECK(plan_post_tonemap(c, good_guides(), 0).result == PlanResult::mipped_colour);
	c = good_colour();
	c.sample_count = 4;
	CHECK(plan_post_tonemap(c, good_guides(), 0).result == PlanResult::mipped_colour);
}

TEST_CASE("typed UAV STORE is mandatory and typed UAV LOAD is not")
{
	// NGX writes DLSSNR.Output through a typed UAV, so store and the view bit decide. It reads
	// DLSSNR.Color through its own CUDA-texture path, so refusing for a missing typed LOAD would
	// be a needless refusal — and a needless refusal costs a round trip to discover.
	ColourDesc c = good_colour();
	c.typed_uav_store = false;
	CHECK(plan_post_tonemap(c, good_guides(), 0).result == PlanResult::no_typed_uav_store);

	c = good_colour();
	c.typed_uav_view = false;
	CHECK(plan_post_tonemap(c, good_guides(), 0).result == PlanResult::no_typed_uav_store);

	c = good_colour();
	c.typed_uav_load = false;
	CHECK(plan_post_tonemap(c, good_guides(), 0).result == PlanResult::ok);
}

TEST_CASE("guides that were never published, or are half-published, are refused as absent")
{
	GuideState g = good_guides();
	g.published = false;
	CHECK(plan_post_tonemap(good_colour(), g, 0).result == PlanResult::guides_absent);

	g = good_guides();
	g.have_depth = false;
	CHECK(plan_post_tonemap(good_colour(), g, 0).result == PlanResult::guides_absent);

	g = good_guides();
	g.have_motion = false;
	CHECK(plan_post_tonemap(good_colour(), g, 0).result == PlanResult::guides_absent);

	g = good_guides();
	g.render_width = 0;
	CHECK(plan_post_tonemap(good_colour(), g, 0).result == PlanResult::guides_absent);

	// Sequence 0 is "never published", whatever the other flags say.
	g = good_guides(0);
	CHECK(plan_post_tonemap(good_colour(), g, 0).result == PlanResult::guides_absent);
}

TEST_CASE("each guide capture is consumable exactly once")
{
	// Freshness is "has this capture been used yet?", NOT "does its present index match?" —
	// which keeps the gate independent of the order in which ReShade fires addon_event::present
	// and addon_event::reshade_begin_effects. A one-frame-old depth buffer produces a
	// plausible-looking wrong image rather than an error, so there is no age to tolerate.
	CHECK(plan_post_tonemap(good_colour(), good_guides(42), 41).result == PlanResult::ok);
	CHECK(plan_post_tonemap(good_colour(), good_guides(42), 42).result == PlanResult::guides_stale);
	CHECK(plan_post_tonemap(good_colour(), good_guides(41), 42).result == PlanResult::guides_stale);
	// A frame that ran no TAA dispatch does not advance the counter, so it is refused with no
	// separate test for "the TAA pass did not run this frame".
	CHECK(plan_post_tonemap(good_colour(), good_guides(7), 7).result == PlanResult::guides_stale);
	// Nothing consumed yet: the very first capture is fresh.
	CHECK(plan_post_tonemap(good_colour(), good_guides(1), 0).result == PlanResult::ok);
}

TEST_CASE("the preui boundary fires exactly once, on the configured back-buffer bind")
{
	// Default 2: the first back-buffer render-target bind is UE4's scene composite, the second is
	// Slate's HUD pass, and we want to be recorded between them.
	CHECK(preui_boundary(1, 2, false) == false);
	CHECK(preui_boundary(2, 2, false));
	CHECK(preui_boundary(3, 2, false) == false);

	// Once per frame, whatever else the frame does.
	CHECK(preui_boundary(2, 2, true) == false);

	// FAIL SAFE. A frame that never reaches the ordinal is skipped, not injected at a guessed
	// point: a missed frame is invisible, injecting into the middle of the HUD is not.
	CHECK(preui_boundary(1, 3, false) == false);

	// A target of 0 is a configuration mistake, and the fail-safe reading of it is "never".
	CHECK(preui_boundary(0, 0, false) == false);
	CHECK(preui_boundary(1, 0, false) == false);
}

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
