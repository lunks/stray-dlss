#include <doctest/doctest.h>

#include "core/feature_recreate.hpp"

#include <vector>

using stray_dlss::core::FeatureRect;
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
