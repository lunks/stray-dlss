// The gate that decides whether a present-stage frame may be handed to DLSS Neural Rendering, the
// site selection in front of it, and the guide-extent latch that forces a reset of NR's own
// temporal accumulation when the guide grid moves under it.
//
// Every one of these refusals is a thing that would otherwise be discovered on the user's machine,
// one expensive round trip at a time (CLAUDE.md §0.1). Pinning them here is the whole reason the
// logic lives in src/core/.
#include <doctest/doctest.h>

#include "core/nr_hook_plan.hpp"

#include <cstring>
// The range-for over a braced list in the mode round-trip test needs this explicitly on
// libstdc++; libc++ happens to pull it in transitively, which is exactly the kind of difference
// the Linux lane exists to catch.
#include <initializer_list>

using namespace stray_dlss::nrplan;

namespace {

constexpr float kTol = 1e-5f;

bool near_enough(float a, float b, float tol = kTol)
{
	return (a > b ? a - b : b - a) <= tol;
}

// A back buffer that passes everything, so each test can spoil exactly one thing. The extent and
// the shape are Stray's own (CLAUDE.md §2.1: 3840x2160, R10G10B10A2_UNORM, 3 buffers).
ColourDesc good_colour()
{
	ColourDesc c;
	c.width = 3840;
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




TEST_CASE("every plan result has a distinct name")
{
	// The names reach the user through the periodic NR STAGE line; two reasons sharing a name
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

TEST_CASE("a healthy 4K back buffer over 1080p guides is accepted, and reports ratio 2.0")
{
	const Plan p = plan_post_tonemap(good_colour(), good_guides(7), 6);
	CHECK(p.result == PlanResult::ok);
	CHECK(p.width == 3840);
	CHECK(p.height == 2160);
	// The colour/guide ratio is REPORTED, not sent — the stage passes 0 ("derive") to nr::apply,
	// which reaches 1.0, the value the user's own live A/B settled on. This pins the number the
	// periodic line prints so the two can be compared on the box.
	CHECK(near_enough(p.mvec_scale_x, 2.0f));
	CHECK(near_enough(p.mvec_scale_y, 2.0f));
}

TEST_CASE("the reported ratio is computed, never hardcoded: 70% screen percentage gives 1.42857")
{
	// This project runs both 50% and 70% (CLAUDE.md §5, "70% is the highest working setting").
	GuideState g = good_guides(1);
	g.render_width = 2688;
	g.render_height = 1512;
	const Plan p = plan_post_tonemap(good_colour(), g, 0);
	CHECK(p.result == PlanResult::ok);
	CHECK(near_enough(p.mvec_scale_x, 3840.0f / 2688.0f));
	CHECK(near_enough(p.mvec_scale_y, 2160.0f / 1512.0f));
	CHECK(near_enough(p.mvec_scale_x, 1.4285714f, 1e-4f));
}

TEST_CASE("a colour target the host could not supply is refused before anything else is looked at")
{
	// `live` carries "the host gave us a back buffer AND a command list", so it is answered first
	// and the rest of the desc is not to be trusted until it passes.
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
	// A mipped input to feature 18 is a documented DXGI_ERROR_DEVICE_HUNG arriving seconds later,
	// not an error return. A log line costs a frame; a hung GPU costs a power cycle.
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
	// Freshness is "has this capture been used yet?", NOT "does its present index match?" — which
	// keeps the gate independent of the order in which a host fires its present callback relative
	// to the frame's TAA dispatch. A one-frame-old depth buffer produces a plausible-looking wrong
	// image rather than an error, so there is no age to tolerate.
	CHECK(plan_post_tonemap(good_colour(), good_guides(42), 41).result == PlanResult::ok);
	CHECK(plan_post_tonemap(good_colour(), good_guides(42), 42).result == PlanResult::guides_stale);
	CHECK(plan_post_tonemap(good_colour(), good_guides(41), 42).result == PlanResult::guides_stale);
	// A frame that ran no TAA dispatch does not advance the counter, so a loading screen is
	// refused with no separate test for "the TAA pass did not run this frame".
	CHECK(plan_post_tonemap(good_colour(), good_guides(7), 7).result == PlanResult::guides_stale);
	// Nothing consumed yet: the very first capture is fresh.
	CHECK(plan_post_tonemap(good_colour(), good_guides(1), 0).result == PlanResult::ok);
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

// ---------------------------------------------------------------------------------------
// NO CODEC, NO EVALUATE.
//
// Feature 18 is a DISPLAY-REFERRED network and our hook point carries raw, unbounded,
// pre-exposed linear HDR. The soft-clip + exact-sRGB proxy IS what puts the input into the
// domain the network was trained on, so a frame that cannot produce a correct proxy has nothing
// valid to hand it — and handing it the raw image anyway is the exact configuration that
// measured a 0.0026 neural output with red noise on screen.
//
// A sibling port reached the same rule from the opposite direction: it moved its pass after the
// sRGB encode because feeding the network a linear image made the runtime apply a second gamma
// curve — "lifted blacks and washed out greys in dark scenes" — and it now declines the frames
// whose encode did not run "rather than evaluating on an input domain the model was not trained
// on" (RemixProjGroup/dxvk-remix @ 2df9c812).
// ---------------------------------------------------------------------------------------






TEST_CASE("a declined frame forces DLSSNR.Reset on the next evaluate, exactly once")
{
	// Feature 18 keeps its OWN temporal accumulation and reprojects it with the motion vectors we
	// supply, which describe motion since the LAST frame. A frame it did not see is a hole in
	// that continuity, so the next evaluate must not reproject across it.
	EvaluateGapLatch latch;
	CHECK(take_evaluate_reset(latch) == false); // nothing declined yet

	note_evaluate_gap(latch);
	CHECK(take_evaluate_reset(latch));          // the evaluate after the gap resets
	CHECK(take_evaluate_reset(latch) == false); // and only that one

	// A run of declines still costs exactly one reset.
	note_evaluate_gap(latch);
	note_evaluate_gap(latch);
	note_evaluate_gap(latch);
	CHECK(take_evaluate_reset(latch));
	CHECK(take_evaluate_reset(latch) == false);
}

TEST_CASE("a frame NR was never ASKED about is a gap too")
{
	// The refusal path only fires from inside apply(), which is reached only when the TAA pass
	// was intercepted and the SR/RR evaluate succeeded. Every other frame is invisible to it —
	// and CLAUDE.md measures evaluates tracking dispatches at 99.7%, so those frames exist.
	EvaluateGapLatch latch;

	// A frame that evaluated arms nothing.
	note_frame_boundary(latch, /*evaluated_this_frame=*/true);
	CHECK(take_evaluate_reset(latch) == false);

	// A frame that did not — with no refusal recorded anywhere, because apply() never ran —
	// still arms the next evaluate.
	note_frame_boundary(latch, /*evaluated_this_frame=*/false);
	CHECK(take_evaluate_reset(latch));
	CHECK(take_evaluate_reset(latch) == false);
}

TEST_CASE("a run of un-evaluated frames still costs exactly one reset")
{
	// NgxNR toggled off and back on keeps the feature and its history (nr::set_enabled), so the
	// gap can be thousands of frames. It is still one hole and one reset.
	EvaluateGapLatch latch;
	for (int i = 0; i < 500; ++i)
		note_frame_boundary(latch, /*evaluated_this_frame=*/false);
	CHECK(take_evaluate_reset(latch));
	CHECK(take_evaluate_reset(latch) == false);
}

TEST_CASE("the frame boundary only ever ARMS - it never clears a pending reset")
{
	// take_evaluate_reset() runs BEFORE the NGX call, so by the time the frame boundary is
	// reached the reset has already been consumed. If the evaluate then failed, refuse_pre_evaluate
	// re-armed the latch on purpose; a boundary that "saw an evaluate" must not undo that.
	EvaluateGapLatch latch;
	CHECK(take_evaluate_reset(latch) == false);

	note_evaluate_gap(latch); // the evaluate failed and re-armed
	note_frame_boundary(latch, /*evaluated_this_frame=*/true);
	CHECK(take_evaluate_reset(latch)); // still armed
}
