// DLSSNR.ControlMask: the bind decision, and what binding one does to the structure pair.
//
// The second half is the interesting one. "Binding a mask sets UseAutoMask to 0" and "the
// sentinel test is a sign test, so any negative inherits local structure" are two separately true
// sentences that, read apart, invite the false conclusion that binding a mask slaves skin to local
// structure. It does not: the inherit is on the other branch, and a bound mask retires the
// resolved pair entirely. These cases are the runtime's own code transcribed (the addresses are in
// src/core/nr_mask_plan.hpp), so the two halves can never again be read as one claim.
#include <doctest/doctest.h>

#include "core/nr_mask_plan.hpp"

#include <cmath>
#include <string>

using namespace stray_dlss::nrmaskplan;

namespace {

// DXGI_FORMAT_R8G8B8A8_UNORM: normalised, non-integer, and what the WARP lane round-trips.
constexpr int kOkFormat = 28;

FormatSupport good_format()
{
	FormatSupport s;
	s.queried = true;
	s.view = true;
	s.store = true;
	return s;
}

Config enabled_all(float r, float g, float b)
{
	Config c;
	c.enabled = true;
	c.channel_r = c.channel_g = c.channel_b = true;
	c.value_r = r;
	c.value_g = g;
	c.value_b = b;
	return c;
}

} // namespace

TEST_CASE("ControlMask: the sentinel constant is exactly -1.0f")
{
	// 0x1800afc40 reads 00 00 80 bf. If this ever needs changing, the binary changed.
	CHECK(kStructureSentinel == -1.0f);
	CHECK(kNeutral == 1.0f);
}

TEST_CASE("ControlMask: binding a mask forces UseAutoMask to 0 whatever the caller asked for")
{
	const ResolvedStructure r = resolve_structure(/*mask_bound=*/true, /*use_auto_mask=*/1,
		/*skin_raw=*/1.33f, /*local_raw=*/1.74f);
	CHECK(r.effective_auto_mask == 0u);
}

TEST_CASE("ControlMask: with a mask bound BOTH resolved strengths become the sentinel")
{
	// The heart of it. Not "skin inherits local" — local does not survive either.
	const ResolvedStructure r = resolve_structure(/*mask_bound=*/true, /*use_auto_mask=*/1,
		/*skin_raw=*/1.33f, /*local_raw=*/1.74f);
	CHECK(r.skin == kStructureSentinel);
	CHECK(r.local == kStructureSentinel);

	// And a caller who had already turned the auto mask off gets the identical answer, which is
	// why "bind a mask" and "set UseAutoMask=0" are the same state to the network.
	const ResolvedStructure off = resolve_structure(/*mask_bound=*/false, /*use_auto_mask=*/0,
		1.33f, 1.74f);
	CHECK(off.skin == kStructureSentinel);
	CHECK(off.local == kStructureSentinel);
}

TEST_CASE("ControlMask: the INHERIT lives on the auto path only")
{
	// UseAutoMask on, no mask: local passes through, and a negative skin takes local's value.
	const ResolvedStructure inherit = resolve_structure(/*mask_bound=*/false, /*use_auto_mask=*/1,
		/*skin_raw=*/-1.0f, /*local_raw=*/1.74f);
	CHECK(inherit.skin == doctest::Approx(1.74f));
	CHECK(inherit.local == doctest::Approx(1.74f));

	// A non-negative skin keeps its own value.
	const ResolvedStructure own = resolve_structure(false, 1, 1.33f, 1.74f);
	CHECK(own.skin == doctest::Approx(1.33f));
	CHECK(own.local == doctest::Approx(1.74f));

	// The runtime's test is `comiss` + `jae`, so it is a SIGN test and not a compare against -1:
	// any negative inherits.
	CHECK(resolve_structure(false, 1, -0.001f, 0.5f).skin == doctest::Approx(0.5f));
	// Exactly zero is NOT negative and keeps its own value.
	CHECK(resolve_structure(false, 1, 0.0f, 0.5f).skin == doctest::Approx(0.0f));
	// NaN fails `jae` and therefore inherits too.
	CHECK(resolve_structure(false, 1, std::nanf(""), 0.5f).skin == doctest::Approx(0.5f));
}

TEST_CASE("ControlMask: default is OFF and emits no rect")
{
	Config cfg; // default-constructed
	const Plan p = plan_mask(cfg, 2560, 1440, kOkFormat, good_format());
	CHECK(p.result == MaskResult::disabled);
	CHECK(subrect_for(p).present == false);
}

TEST_CASE("ControlMask: an unwritable format refuses rather than binding an uninitialised texture")
{
	const Config cfg = enabled_all(0.5f, 0.5f, 0.5f);

	FormatSupport no_store = good_format();
	no_store.store = false;
	CHECK(plan_mask(cfg, 2560, 1440, kOkFormat, no_store).result == MaskResult::no_typed_uav_store);

	FormatSupport no_view = good_format();
	no_view.view = false;
	CHECK(plan_mask(cfg, 2560, 1440, kOkFormat, no_view).result == MaskResult::no_typed_uav_store);

	// A FAILED query is not evidence of support.
	FormatSupport unqueried;
	CHECK(plan_mask(cfg, 2560, 1440, kOkFormat, unqueried).result == MaskResult::no_typed_uav_store);

	CHECK(subrect_for(plan_mask(cfg, 2560, 1440, kOkFormat, no_store)).present == false);
}

TEST_CASE("ControlMask: a zero extent refuses")
{
	const Config cfg = enabled_all(0.5f, 0.5f, 0.5f);
	CHECK(plan_mask(cfg, 0, 1440, kOkFormat, good_format()).result == MaskResult::zero_extent);
	CHECK(plan_mask(cfg, 2560, 0, kOkFormat, good_format()).result == MaskResult::zero_extent);
}

TEST_CASE("ControlMask: an all-neutral mask binds, and says so")
{
	// The FIRST experiment: a mask that is bound and provably doing nothing. If the image changes
	// when this is turned on, something about the binding is wrong — that is the whole point of
	// running it before any heuristic.
	Config cfg;
	cfg.enabled = true;
	cfg.channel_r = cfg.channel_g = cfg.channel_b = true;
	cfg.value_r = cfg.value_g = cfg.value_b = 1.0f;

	const Plan p = plan_mask(cfg, 2560, 1440, kOkFormat, good_format());
	CHECK(p.result == MaskResult::ok);
	CHECK(p.is_identity);
	CHECK(p.value_r == 1.0f);
	CHECK(p.value_g == 1.0f);
	CHECK(p.value_b == 1.0f);
	CHECK(p.value_a == 1.0f);
}

TEST_CASE("ControlMask: a disabled channel writes neutral, not its configured value")
{
	// Turning a channel off must not leave that component uninitialised or carrying the old
	// number: R, G and B have to be testable APART, and that only works if the two off channels
	// are provably the identity.
	Config cfg = enabled_all(0.25f, 0.25f, 0.25f);
	cfg.channel_g = false;
	cfg.channel_b = false;

	const Plan p = plan_mask(cfg, 2560, 1440, kOkFormat, good_format());
	REQUIRE(p.result == MaskResult::ok);
	CHECK(p.value_r == doctest::Approx(0.25f));
	CHECK(p.value_g == kNeutral);
	CHECK(p.value_b == kNeutral);
	CHECK(p.is_identity == false);
}

TEST_CASE("ControlMask: the subrect is the COLOUR rect, based at the origin")
{
	const Plan p = plan_mask(enabled_all(0.5f, 1.0f, 1.0f), 2560, 1440, kOkFormat, good_format());
	REQUIRE(p.result == MaskResult::ok);
	const Subrect r = subrect_for(p);
	CHECK(r.present);
	CHECK(r.base_x == 0u);
	CHECK(r.base_y == 0u);
	CHECK(r.width == 2560u);
	CHECK(r.height == 1440u);
	// The extent the fill pass covers and the extent we declare must be the same number. The
	// runtime fills a zero subrect in from the resource's own extent (0x18001c596), so a
	// disagreement here would be invisible rather than an error.
	CHECK(r.width == p.width);
	CHECK(r.height == p.height);
}

TEST_CASE("ControlMask: every result has a name")
{
	for (int i = 0; i < kMaskResultCount; ++i)
	{
		const char *n = mask_result_name(static_cast<MaskResult>(i));
		REQUIRE(n != nullptr);
		CHECK(n[0] != '\0');
		CHECK(n[0] != '?');
	}
	CHECK(std::string(mask_result_name(static_cast<MaskResult>(kMaskResultCount))) == "?");
}

TEST_CASE("ControlMask: an integer format is refused, and BEFORE the typed-UAV gate")
{
	// A *_UINT texture is perfectly storable through a typed UAV, so it would sail past the other
	// gate — and the runtime creates its SRV without complaint, after which the kernel does a
	// `tex.2d.v4.f32.f32` float fetch against an integer texture. Undefined values, no error.
	const Config cfg = enabled_all(0.5f, 0.5f, 0.5f);
	CHECK(plan_mask(cfg, 2560, 1440, 28, good_format()).result == MaskResult::ok);      // RGBA8_UNORM
	CHECK(plan_mask(cfg, 2560, 1440, 10, good_format()).result == MaskResult::ok);      // RGBA16_FLOAT
	CHECK(plan_mask(cfg, 2560, 1440, 30, good_format()).result == MaskResult::integer_format); // RGBA8_UINT
	CHECK(plan_mask(cfg, 2560, 1440, 42, good_format()).result == MaskResult::integer_format); // R32_UINT

	// The ordering matters for the log, not the outcome: with BOTH problems present the reported
	// reason must be the integer format, because that is the one the operator can act on.
	FormatSupport no_store = good_format();
	no_store.store = false;
	CHECK(plan_mask(cfg, 2560, 1440, 30, no_store).result == MaskResult::integer_format);
}

TEST_CASE("ControlMask: format_is_integer covers the families, and nothing else")
{
	for (int f : { 3, 4, 7, 8, 13, 14, 17, 18, 22, 23, 30, 32, 36, 38, 42, 43, 47, 50, 52, 57,
	               59, 62, 64 })
		CHECK_MESSAGE(format_is_integer(f), "DXGI_FORMAT ", f, " is an integer format");
	// The formats a mask would plausibly be built in, and the ones the runtime's own canonicalizer
	// produces, must all pass.
	for (int f : { 2, 10, 11, 24, 26, 28, 29, 34, 41, 54, 61, 87 })
		CHECK_MESSAGE(!format_is_integer(f), "DXGI_FORMAT ", f, " is not an integer format");
}

TEST_CASE("ControlMask: an identity mask leaves the BLEND WEIGHT untouched")
{
	// The kernel computes `saturate(DLSSNR.Intensity * mask.x)`, and the no-mask path is
	// `saturate(DLSSNR.Intensity)` — so mask.R == 1.0 is exactly the unmasked weight, whatever
	// Intensity is. That is what makes the identity mask a clean control for the PLUMBING.
	//
	// It is NOT a clean control for the whole feature: binding a mask also drives the resolved
	// structure pair to the sentinel, which changes the network's own input channels. The
	// comparison that isolates the mask alone is identity-mask against UseAutoMask=0-and-no-mask,
	// because those two agree on both the weight and the resolved pair.
	Config identity;
	identity.enabled = true;
	identity.channel_r = identity.channel_g = identity.channel_b = true;
	const Plan p = plan_mask(identity, 2560, 1440, kOkFormat, good_format());
	REQUIRE(p.result == MaskResult::ok);
	CHECK(p.value_r == kNeutral);
	CHECK(p.is_identity);

	const ResolvedStructure with_mask = resolve_structure(true, 1, 1.33f, 1.74f);
	const ResolvedStructure auto_off = resolve_structure(false, 0, 1.33f, 1.74f);
	CHECK(with_mask.skin == auto_off.skin);
	CHECK(with_mask.local == auto_off.local);
	CHECK(with_mask.effective_auto_mask == auto_off.effective_auto_mask);
}
