// The guide-extent latch that forces a reset of NR's own temporal accumulation when the guide
// grid moves under it. (The post-tonemap hook sites this file also covered were removed on
// 2026-09-02; NR runs at the TAA site only.)
#include <doctest/doctest.h>

#include "core/nr_codec.hpp"
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

TEST_CASE("the codec gate lets a well-formed frame through")
{
	CodecGateInputs in;
	in.codec_site = true;
	in.encode_recorded = true;
	in.track_exposure = true;
	in.exposure_known = true;
	in.scale = 8.0f;
	CHECK(codec_gate(in) == CodecGate::evaluate);
}

TEST_CASE("a site that cannot run the codec never evaluates")
{
	// The post-tonemap sites were removed on 2026-09-02, but the enum value that reaches this
	// code still exists, so the branch that would evaluate on an un-encoded image is still
	// reachable. It must refuse rather than quietly run the raw-HDR path.
	CodecGateInputs in;
	in.codec_site = false;
	in.encode_recorded = true;
	in.exposure_known = true;
	in.scale = 8.0f;
	CHECK(codec_gate(in) == CodecGate::no_codec);
}

TEST_CASE("an encode that did not record never evaluates")
{
	CodecGateInputs in;
	in.encode_recorded = false;
	in.exposure_known = true;
	in.scale = 8.0f;
	CHECK(codec_gate(in) == CodecGate::encode_failed);
}

TEST_CASE("tracking exposure with no exposure ever read never evaluates")
{
	// With NgxNRTrackExposure on, the codec's operating point IS the engine's exposure. A frame
	// whose View constant buffer has never decoded leaves that term unknown, and the old
	// behaviour — silently substituting the static scale — puts the network in a DIFFERENT input
	// domain from the one its own temporal history was accumulated in, with no diagnostic.
	CodecGateInputs in;
	in.encode_recorded = true;
	in.track_exposure = true;
	in.exposure_known = false;
	in.scale = 8.0f;
	CHECK(codec_gate(in) == CodecGate::exposure_unknown);

	// With tracking OFF the static scale is the whole answer by design, so the same frame is
	// fine — the exposure term is not unknown, it is not part of the definition.
	in.track_exposure = false;
	CHECK(codec_gate(in) == CodecGate::evaluate);
}

TEST_CASE("a scale pinned at either clamp never evaluates")
{
	// nrc::proxy_scale clamps to [1e-6, 1e6], so a degenerate operating point arrives as a value
	// sitting exactly on a bound rather than as a zero or a NaN. At 1e-6 the proxy is flat black
	// and at 1e6 it is flat white; in both cases the network is shown no image at all.
	CodecGateInputs in;
	in.encode_recorded = true;
	in.track_exposure = true;
	in.exposure_known = true;

	in.scale = stray_dlss::nrc::kScaleMin;
	CHECK(codec_gate(in) == CodecGate::degenerate_scale);
	in.scale = stray_dlss::nrc::kScaleMax;
	CHECK(codec_gate(in) == CodecGate::degenerate_scale);
	in.scale = 0.0f;
	CHECK(codec_gate(in) == CodecGate::degenerate_scale);
	in.scale = -1.0f;
	CHECK(codec_gate(in) == CodecGate::degenerate_scale);

	in.scale = 1.0f;
	CHECK(codec_gate(in) == CodecGate::evaluate);
}

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
