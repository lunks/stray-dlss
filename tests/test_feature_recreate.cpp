#include <doctest/doctest.h>

#include "core/feature_recreate.hpp"

#include <algorithm>
#include <string>
#include <vector>

using stray_dlss::core::FeatureRect;
using stray_dlss::core::HoldRefusal;
using stray_dlss::core::plan_letterbox_hold;
using stray_dlss::core::plan_recreate;
using stray_dlss::core::RecreateAction;
using stray_dlss::core::RecreateState;

namespace {

constexpr FeatureRect kPrimary{ 1920, 1080, 3840, 2160 };

// One frame of the debounce, keeping a caller's feature state the way ngx_backend does.
struct Sim
{
	RecreateState state{};
	bool have_feature = false;
	FeatureRect live{};
	unsigned creations = 0;
	unsigned declines = 0;

	RecreateAction frame(const FeatureRect &want, std::uint32_t stable = 8)
	{
		const RecreateAction a = plan_recreate(state, have_feature, live, want, stable);
		if (a == RecreateAction::create)
		{
			have_feature = true;
			live = want;
			++creations;
		}
		else if (a == RecreateAction::wait)
		{
			++declines;
		}
		return a;
	}
};

} // namespace

TEST_CASE("the first feature is never debounced")
{
	Sim s;
	CHECK(s.frame(kPrimary) == RecreateAction::create);
	CHECK(s.creations == 1);
	// And a matching rect is kept, forever, with no further creations.
	for (int i = 0; i < 100; ++i)
		CHECK(s.frame(kPrimary) == RecreateAction::keep);
	CHECK(s.creations == 1);
	CHECK(s.declines == 0);
}

TEST_CASE("A SCRIPTED SCENE TRANSITION CREATES NOTHING - the measured sequence, replayed")
{
	// The rects are the live log's own, 2026-09-03: a cinematic-bar animation walking the view
	// rect from 3840x2160 down and back, which produced SIX TO TEN consecutive
	// `DLSS feature created:` lines per transition - each one a lost temporal history (the pop)
	// and a full CreateFeature (the hitch).
	const std::vector<FeatureRect> transition = {
		{ 1920, 1074, 3840, 2148 }, { 1920, 1070, 3840, 2140 }, { 1920, 1068, 3840, 2135 },
		{ 1920, 1061, 3840, 2122 }, { 1920, 1039, 3840, 2078 }, { 1920, 1044, 3840, 2087 },
		{ 1920, 1049, 3840, 2097 }, { 1920, 1064, 3840, 2127 },
	};

	Sim s;
	REQUIRE(s.frame(kPrimary) == RecreateAction::create);
	const unsigned before = s.creations;

	// Each step is held for three frames, which is the cadence the log implies.
	for (const FeatureRect &r : transition)
		for (int hold = 0; hold < 3; ++hold)
			CHECK(s.frame(r) == RecreateAction::wait);

	CHECK(s.creations == before);           // nothing built while the rect was moving
	CHECK(s.declines == 3 * transition.size());

	SUBCASE("and the ORIGINAL feature is still alive when the bars retract")
	{
		// This is the half that removes the pop at the END of the transition too: the primary
		// feature was never released, so returning to it is a `keep`, not a create, and DLSS's
		// accumulated history survives the whole cinematic.
		CHECK(s.frame(kPrimary) == RecreateAction::keep);
		CHECK(s.creations == before);
		CHECK(s.state.restarts == transition.size());
	}
}

TEST_CASE("a GENUINE resolution change still creates - once, after it settles")
{
	// The failure mode to avoid is a debounce that never lets a real change through. A
	// resolution or screen-percentage change asks for the SAME new rect every frame, so it
	// settles immediately and costs `stable_frames` frames of the engine's own TAA.
	Sim s;
	REQUIRE(s.frame(kPrimary) == RecreateAction::create);
	const FeatureRect seventy{ 2688, 1512, 3840, 2160 };

	for (int i = 0; i < 7; ++i)
		CHECK(s.frame(seventy) == RecreateAction::wait);
	CHECK(s.frame(seventy) == RecreateAction::create);
	CHECK(s.creations == 2);
	CHECK(s.frame(seventy) == RecreateAction::keep);
	CHECK(s.creations == 2);
}

TEST_CASE("stable_frames = 0 is the OLD behaviour exactly, so the box can A/B it")
{
	Sim s;
	REQUIRE(s.frame(kPrimary, 0) == RecreateAction::create);
	CHECK(s.frame({ 1920, 1074, 3840, 2148 }, 0) == RecreateAction::create);
	CHECK(s.frame({ 1920, 1070, 3840, 2140 }, 0) == RecreateAction::create);
	CHECK(s.creations == 3);
	CHECK(s.declines == 0);
}

TEST_CASE("a rect that flickers away and back accumulates no credit")
{
	// Alternating A, B, A, B must never reach the threshold: `pending_count` counts CONSECUTIVE
	// requests, and a return to the live rect clears the debounce outright.
	Sim s;
	REQUIRE(s.frame(kPrimary) == RecreateAction::create);
	const FeatureRect other{ 1920, 1064, 3840, 2127 };
	for (int i = 0; i < 20; ++i)
	{
		CHECK(s.frame(other) == RecreateAction::wait);
		CHECK(s.frame(kPrimary) == RecreateAction::keep);
	}
	CHECK(s.creations == 1);
	CHECK(s.state.pending_count == 0);
}

TEST_CASE("two differing rects alternating never settle either")
{
	Sim s;
	REQUIRE(s.frame(kPrimary) == RecreateAction::create);
	const FeatureRect a{ 1920, 1064, 3840, 2127 };
	const FeatureRect b{ 1920, 1066, 3840, 2132 };
	for (int i = 0; i < 30; ++i)
	{
		CHECK(s.frame(a) == RecreateAction::wait);
		CHECK(s.frame(b) == RecreateAction::wait);
	}
	CHECK(s.creations == 1);
	CHECK(s.state.restarts == 60);
}

TEST_CASE("a degenerate rect invents nothing")
{
	// The caller has its own refusal for a rect it could not read; this function must not turn
	// that into a creation, and must not let it disturb a debounce in progress.
	RecreateState st{};
	CHECK(plan_recreate(st, false, FeatureRect{}, FeatureRect{ 0, 0, 3840, 2160 }, 8) ==
		RecreateAction::keep);
	CHECK(plan_recreate(st, true, kPrimary, FeatureRect{ 1920, 1080, 0, 0 }, 8) ==
		RecreateAction::keep);
	CHECK(st.pending_count == 0);
	CHECK(st.waits == 0);
}


// ---------------------------------------------------------------------------------------
// Holding the feature across the letterbox slide, so SR - and therefore NR - keeps running.
// ---------------------------------------------------------------------------------------

namespace {

// Every distinct `1920x10xx -> 3840x20xx` pair the live log recorded during the scripted
// transitions, verbatim. Twenty-two of them, and the whole point of the ratio test is that ALL
// of them must be recognised as the same 2.0x feature seen through a shrinking rect.
constexpr FeatureRect kSlide[] = {
	{ 1920, 1037, 3840, 2073 }, { 1920, 1037, 3840, 2074 }, { 1920, 1038, 3840, 2075 },
	{ 1920, 1038, 3840, 2076 }, { 1920, 1039, 3840, 2077 }, { 1920, 1039, 3840, 2078 },
	{ 1920, 1040, 3840, 2079 }, { 1920, 1043, 3840, 2085 }, { 1920, 1044, 3840, 2087 },
	{ 1920, 1049, 3840, 2097 }, { 1920, 1052, 3840, 2104 }, { 1920, 1054, 3840, 2108 },
	{ 1920, 1061, 3840, 2122 }, { 1920, 1064, 3840, 2127 }, { 1920, 1064, 3840, 2128 },
	{ 1920, 1065, 3840, 2129 }, { 1920, 1066, 3840, 2131 }, { 1920, 1066, 3840, 2132 },
	{ 1920, 1068, 3840, 2135 }, { 1920, 1070, 3840, 2140 }, { 1920, 1073, 3840, 2146 },
	{ 1920, 1074, 3840, 2148 },
};

} // namespace

TEST_CASE("EVERY measured letterbox rect is held, so SR and NR run through the slide")
{
	for (const FeatureRect &want : kSlide)
	{
		CAPTURE(want.render_h);
		CAPTURE(want.output_h);
		CHECK(plan_letterbox_hold(kPrimary, want, 0.0f, 0.0f, 3840, 2160) == HoldRefusal::none);
	}
}

TEST_CASE("the origin is the clause the whole thing rests on, and it is READ not assumed")
{
	// Holding is correct only because a shrinking rect is a PREFIX of the created one on both
	// axes. If Stray ever letterboxed by CENTRING the rect, View.ViewRectMin would move off
	// (0,0), the created scale would no longer carry input onto output, and holding would put a
	// silently displaced image on screen. That must refuse, not guess.
	const FeatureRect want{ 1920, 1037, 3840, 2073 };
	CHECK(plan_letterbox_hold(kPrimary, want, 0.0f, 0.0f, 3840, 2160) == HoldRefusal::none);
	CHECK(plan_letterbox_hold(kPrimary, want, 0.0f, 21.0f, 3840, 2160) == HoldRefusal::origin_moved);
	CHECK(plan_letterbox_hold(kPrimary, want, 1.0f, 0.0f, 3840, 2160) == HoldRefusal::origin_moved);
	CHECK(plan_letterbox_hold(kPrimary, want, -1.0f, 0.0f, 3840, 2160) == HoldRefusal::origin_moved);
}

TEST_CASE("a GENUINE resolution change is not a slide, and must rebuild")
{
	// 70% screen percentage against a feature built for 50%: the render rect grows and the
	// output rect does not, so the created scale no longer maps one onto the other. This is the
	// case that MUST fall through to the debounce - holding it would upscale 2688x1512 through
	// a 2.0x feature and stretch the frame.
	CHECK(plan_letterbox_hold(kPrimary, FeatureRect{ 2688, 1512, 3840, 2160 }, 0.0f, 0.0f, 3840, 2160) ==
		HoldRefusal::larger_than_feature);

	// The same shape the other way: a smaller render rect into the FULL output. That is exactly
	// option 2 - the non-uniform stretch - and it is what this refusal prevents.
	CHECK(plan_letterbox_hold(kPrimary, FeatureRect{ 1920, 1037, 3840, 2160 }, 0.0f, 0.0f, 3840, 2160) ==
		HoldRefusal::ratio_moved);

	// A REAL DROP TO A LOWER OUTPUT RESOLUTION, and this one is the sharp edge: 1280x720 ->
	// 2560x1440 has the SAME 2.0 scale as the live feature, so the ratio test does not separate
	// it from a slide. The mapping argument even survives it on paper - and it would still be
	// wrong, because the engine reallocates its buffers and DLSS would write a 3840x2160 target
	// into a 2560x1440 UAV. The fraction bound refuses it here; the call site additionally
	// requires the output texture to still hold the created target, which is the physical half
	// of the same fence.
	CHECK(plan_letterbox_hold(kPrimary, FeatureRect{ 1280, 720, 2560, 1440 }, 0.0f, 0.0f, 3840, 2160) ==
		HoldRefusal::too_small);
}

TEST_CASE("a wrong-View rect cannot sneak through the hold path")
{
	// Defect 1's impossible rects are gated at the search now, but this path must not become a
	// second way in: they are far below the created extent and their ratio is nothing like 2.0.
	// WHICH refusal fires is not the point and must not be over-specified - that they are all
	// refused is. (64x41 and 1024x1024 fail the fraction; the portrait exceeds the extent.)
	CHECK(plan_letterbox_hold(kPrimary, FeatureRect{ 64, 41, 3840, 2160 }, 0, 0, 3840, 2160) != HoldRefusal::none);
	CHECK(plan_letterbox_hold(kPrimary, FeatureRect{ 1024, 1024, 3840, 2160 }, 0, 0, 3840, 2160) != HoldRefusal::none);
	CHECK(plan_letterbox_hold(kPrimary, FeatureRect{ 512, 512, 3840, 2160 }, 0, 0, 3840, 2160) != HoldRefusal::none);
	CHECK(plan_letterbox_hold(kPrimary, FeatureRect{ 256, 240, 3840, 2160 }, 0, 0, 3840, 2160) != HoldRefusal::none);
	CHECK(plan_letterbox_hold(kPrimary, FeatureRect{ 1064, 2128, 3840, 2160 }, 0.0f, 0.0f, 3840, 2160) ==
		HoldRefusal::larger_than_feature);
}

TEST_CASE("the tolerance absorbs the engine's odd/even rounding and nothing more")
{
	// The live log carries BOTH 3840x2073 and 3840x2074 for the same 1920x1037 render rect, so
	// the predicted output may miss by a pixel. Two is the budget; a rect that misses by more
	// is a different view, not a rounding.
	CHECK(plan_letterbox_hold(kPrimary, FeatureRect{ 1920, 1037, 3840, 2074 }, 0, 0, 3840, 2160) ==
		HoldRefusal::none);
	CHECK(plan_letterbox_hold(kPrimary, FeatureRect{ 1920, 1037, 3840, 2072 }, 0, 0, 3840, 2160) ==
		HoldRefusal::none);
	CHECK(plan_letterbox_hold(kPrimary, FeatureRect{ 1920, 1037, 3840, 2070 }, 0, 0, 3840, 2160) ==
		HoldRefusal::ratio_moved);
}

TEST_CASE("degenerate and no-op inputs are named, never held by accident")
{
	CHECK(plan_letterbox_hold(FeatureRect{}, kPrimary, 0, 0, 3840, 2160) == HoldRefusal::no_feature);
	CHECK(plan_letterbox_hold(kPrimary, FeatureRect{}, 0, 0, 3840, 2160) == HoldRefusal::no_feature);
	CHECK(plan_letterbox_hold(kPrimary, kPrimary, 0, 0, 3840, 2160) == HoldRefusal::not_needed);
}

TEST_CASE("every refusal has a name, and they are distinct")
{
	// The counters are read off a log line; two reasons sharing a name would make a live session
	// undiagnosable, which is the failure this project keeps paying for.
	std::vector<std::string> names;
	for (int i = 0; i < static_cast<int>(HoldRefusal::count); ++i)
	{
		const char *n = stray_dlss::core::hold_refusal_name(static_cast<HoldRefusal>(i));
		REQUIRE(n != nullptr);
		CHECK(std::string(n) != "?");
		names.push_back(n);
	}
	std::sort(names.begin(), names.end());
	CHECK(std::unique(names.begin(), names.end()) == names.end());
}

TEST_CASE("THE PHYSICAL CLAUSE: the output UAV must still hold the created target")
{
	// Every other clause is arithmetic over rects. This one asks whether the memory DLSS would
	// write into is still there - and it is what stops a genuine resolution change, which
	// reallocates the engine's buffers underneath an UNCHANGED 2.0 ratio, from becoming an
	// out-of-bounds write that vkd3d-proton has no debug layer to object to.
	const FeatureRect want{ 1920, 1037, 3840, 2073 };
	CHECK(plan_letterbox_hold(kPrimary, want, 0, 0, 3840, 2160) == HoldRefusal::none);
	// The engine's scene targets are allowed to be BIGGER than the view rect (CLAUDE.md §2.5).
	CHECK(plan_letterbox_hold(kPrimary, want, 0, 0, 4096, 2304) == HoldRefusal::none);
	// Smaller on either axis, or unknown, refuses.
	CHECK(plan_letterbox_hold(kPrimary, want, 0, 0, 2560, 1440) == HoldRefusal::output_too_small);
	CHECK(plan_letterbox_hold(kPrimary, want, 0, 0, 3840, 2159) == HoldRefusal::output_too_small);
	CHECK(plan_letterbox_hold(kPrimary, want, 0, 0, 3839, 2160) == HoldRefusal::output_too_small);
	CHECK(plan_letterbox_hold(kPrimary, want, 0, 0, 0, 0) == HoldRefusal::output_too_small);
}
