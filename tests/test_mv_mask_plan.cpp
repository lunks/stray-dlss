#include <doctest/doctest.h>

#include "core/mv_mask_plan.hpp"

#include <cstring>

using namespace stray_dlss;

namespace {

constexpr std::uint32_t kRenderW = 1920;
constexpr std::uint32_t kRenderH = 1080;

mvmaskplan::Plan on(float value = 1.0f, std::uint32_t period = 0, std::uint64_t frame = 0)
{
	return mvmaskplan::plan(/*enabled=*/true, /*ready=*/true, /*filled=*/true, kRenderW, kRenderH,
		kRenderW, kRenderH, value, period, frame);
}

} // namespace

TEST_CASE("mv mask: DISABLED is the default and it is exactly the old behaviour")
{
	const mvmaskplan::Plan p = mvmaskplan::plan(/*enabled=*/false, true, true, kRenderW, kRenderH,
		kRenderW, kRenderH, /*value=*/1.0f, /*period=*/0, /*frame=*/0);
	CHECK_FALSE(p.bind);
	CHECK(p.refusal == mvmaskplan::Refusal::disabled);
	// Not merely "not bound": the value it would have written is forced to neutral too, so a
	// disabled session cannot leave a non-neutral texture behind for anything to pick up.
	CHECK(p.value == mvmaskplan::kNeutral);
	CHECK_FALSE(p.phase_on);
}

TEST_CASE("mv mask: neutral is ZERO, and that is the same as binding nothing")
{
	// The one third-party integration known to drive this input writes its FAILING pixels into
	// the mask, so non-zero is the distrust direction and zero must be the no-op.
	CHECK(mvmaskplan::kNeutral == 0.0f);
}

TEST_CASE("mv mask: nothing undefined is ever handed to DLSS")
{
	SUBCASE("no texture yet")
	{
		const mvmaskplan::Plan p = mvmaskplan::plan(true, /*ready=*/false, true, kRenderW,
			kRenderH, kRenderW, kRenderH, 1.0f, 0, 0);
		CHECK_FALSE(p.bind);
		CHECK(p.refusal == mvmaskplan::Refusal::not_ready);
	}
	SUBCASE("allocated but never cleared")
	{
		// Undefined bytes on the one channel whose job is to say which pixels to distrust are
		// worse than no mask at all.
		const mvmaskplan::Plan p = mvmaskplan::plan(true, true, /*filled=*/false, kRenderW,
			kRenderH, kRenderW, kRenderH, 1.0f, 0, 0);
		CHECK_FALSE(p.bind);
		CHECK(p.refusal == mvmaskplan::Refusal::unfilled);
	}
}

TEST_CASE("mv mask: the mask must COVER the render subrect, and larger is fine")
{
	SUBCASE("exactly covering")
	{
		CHECK(on().bind);
	}
	SUBCASE("larger, which is what grow-only allocation produces after a resolution change")
	{
		const mvmaskplan::Plan p = mvmaskplan::plan(true, true, true, 3840, 2160, kRenderW,
			kRenderH, 1.0f, 0, 0);
		CHECK(p.bind);
	}
	SUBCASE("one pixel short in either axis is refused")
	{
		CHECK(mvmaskplan::plan(true, true, true, kRenderW - 1, kRenderH, kRenderW, kRenderH,
			1.0f, 0, 0).refusal == mvmaskplan::Refusal::extent);
		CHECK(mvmaskplan::plan(true, true, true, kRenderW, kRenderH - 1, kRenderW, kRenderH,
			1.0f, 0, 0).refusal == mvmaskplan::Refusal::extent);
	}
	SUBCASE("a zero render rect is refused rather than treated as trivially covered")
	{
		CHECK(mvmaskplan::plan(true, true, true, kRenderW, kRenderH, 0, kRenderH, 1.0f, 0, 0)
			.refusal == mvmaskplan::Refusal::extent);
		CHECK(mvmaskplan::plan(true, true, true, kRenderW, kRenderH, kRenderW, 0, 1.0f, 0, 0)
			.refusal == mvmaskplan::Refusal::extent);
	}
}

TEST_CASE("mv mask: with no alternation the configured value is simply held")
{
	const mvmaskplan::Plan p = on(0.75f, /*period=*/0, /*frame=*/12345);
	CHECK(p.bind);
	CHECK(p.phase_on);
	CHECK(p.value == doctest::Approx(0.75f));
}

TEST_CASE("mv mask: the A/B alternates INSIDE one session, on a known frame boundary")
{
	// CLAUDE.md §5, after three false starts: a moving camera makes two runs incomparable, so
	// the comparison has to happen within one session with the state flipping on a boundary
	// that is logged. This is DryRunAlternate's discipline applied to the mask.
	constexpr std::uint32_t kPeriod = 900;

	// The FIRST half-period is neutral, so a session that never reaches the second one is
	// unambiguously a control rather than an untested treatment.
	CHECK_FALSE(mvmaskplan::alternate_phase_on(0, kPeriod));
	CHECK_FALSE(mvmaskplan::alternate_phase_on(kPeriod - 1, kPeriod));
	CHECK(mvmaskplan::alternate_phase_on(kPeriod, kPeriod));
	CHECK(mvmaskplan::alternate_phase_on(2 * kPeriod - 1, kPeriod));
	CHECK_FALSE(mvmaskplan::alternate_phase_on(2 * kPeriod, kPeriod));

    CHECK(on(1.0f, kPeriod, 0).value == mvmaskplan::kNeutral);
	CHECK(on(1.0f, kPeriod, kPeriod).value == doctest::Approx(1.0f));

	// A neutral phase still BINDS. "Bound and deliberately neutral" and "refused" must never
	// look alike — an identity mask is a real experimental state, not an absence, and the two
	// arms of the A/B must differ in the VALUE and in nothing else.
	CHECK(on(1.0f, kPeriod, 0).bind);
	CHECK(on(1.0f, kPeriod, kPeriod).bind);
}

TEST_CASE("mv mask: period 1 alternates every frame, which is legal and useless")
{
	// Not forbidden — but every frame changing DLSS's history trust is not an A/B, it is noise.
	// Pinned so the behaviour is at least predictable if somebody sets it.
	CHECK_FALSE(mvmaskplan::alternate_phase_on(0, 1));
	CHECK(mvmaskplan::alternate_phase_on(1, 1));
	CHECK_FALSE(mvmaskplan::alternate_phase_on(2, 1));
}

TEST_CASE("mv mask: every refusal has a name, so no gate can refuse in silence")
{
	for (int i = 0; i < static_cast<int>(mvmaskplan::Refusal::count); ++i)
	{
		const char *n = mvmaskplan::refusal_name(static_cast<mvmaskplan::Refusal>(i));
		CHECK(n != nullptr);
		CHECK(std::strlen(n) > 1);
		CHECK(std::strcmp(n, "?") != 0);
	}
}
