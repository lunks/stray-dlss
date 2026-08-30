#include <doctest/doctest.h>

#include "core/taa_signature.hpp"

using namespace stray_dlss;

namespace {

// The measured Stray TAA binding layout at 2560x1440. CLAUDE.md §2.3.
// t0 depth, t1 stencil (SAME resource as t0), t2 velocity, t3 eye adaptation (1x1),
// t5/t6 colour, u0 output, u1 downsampled.
DispatchSignature make_stray_taa(std::uint32_t w = 2560, std::uint32_t h = 1440)
{
	DispatchSignature s;
	s.shader_hash = kTaaMainHash;
	s.has_view_cb = true;
	s.group_count_x = (w + kTaaTileSize - 1) / kTaaTileSize;
	s.group_count_y = (h + kTaaTileSize - 1) / kTaaTileSize;
	s.group_count_z = 1;

	s.srvs = {
		{ 0, 0x1000, TexFormat::r32_float_x8x24_typeless, w, h },
		{ 1, 0x1000, TexFormat::x32_typeless_g8x24_uint, w, h }, // same resource as t0
		{ 2, 0x2000, TexFormat::r16g16b16a16_unorm, w, h },
		{ 3, 0x3000, TexFormat::r32g32b32a32_float, 1, 1 }, // eye adaptation
		{ 5, 0x4000, TexFormat::r16g16b16a16_float, w, h },
		{ 6, 0x5000, TexFormat::r16g16b16a16_float, w, h },
	};
	s.uavs = {
		{ 0, 0x6000, TexFormat::r16g16b16a16_float, w, h },
		{ 1, 0x7000, TexFormat::r16g16b16a16_float, w / 2, h / 2 },
	};
	return s;
}

} // namespace

TEST_CASE("the measured Stray TAA dispatch matches on hash and structure")
{
	const auto sig = make_stray_taa();
	const auto r = match_taa_dispatch(sig, 2560, 1440);

	CHECK(r.verdict == MatchVerdict::hash_and_structural);
	CHECK(r.depth_srv == 0);
	CHECK(r.stencil_srv == 1);
	CHECK(r.velocity_srv == 2);
	CHECK(r.output_uav == 0);
	CHECK(r.has_downsample_uav);
	// Both colour SRVs are reported; deciding which is history needs last frame's u0.
	CHECK(((r.colour_srv_a == 5 && r.colour_srv_b == 6) ||
		(r.colour_srv_a == 6 && r.colour_srv_b == 5)));
}

TEST_CASE("structure alone is enough when the hash moves")
{
	// Stray's TAA hash tracks r.PostProcessAAQuality, so a user changing the in-game AA
	// setting must not break the hook. CLAUDE.md §2.3.
	auto sig = make_stray_taa();
	sig.shader_hash = 0xdeadbeefcafef00dull;

	const auto r = match_taa_dispatch(sig, 2560, 1440);
	CHECK(r.verdict == MatchVerdict::structural_only);
	CHECK(r.depth_srv == 0);
	CHECK(r.velocity_srv == 2);
}

TEST_CASE("the known false positive is excluded no matter how it scores")
{
	auto sig = make_stray_taa();
	sig.shader_hash = kKnownFalsePositiveHash;

	const auto r = match_taa_dispatch(sig, 2560, 1440);
	CHECK(r.verdict == MatchVerdict::excluded);
}

TEST_CASE("the second candidate is excluded — it is almost certainly motion blur")
{
	auto sig = make_stray_taa();
	sig.shader_hash = kSecondCandidateHash;

	const auto r = match_taa_dispatch(sig, 2560, 1440);
	CHECK(r.verdict == MatchVerdict::excluded);
}

TEST_CASE("depth and stencil are told apart by view format over a shared resource")
{
	auto sig = make_stray_taa();
	// Swap the two views' slots. The matcher must follow the format, not the register.
	sig.srvs[0].format = TexFormat::x32_typeless_g8x24_uint;
	sig.srvs[1].format = TexFormat::r32_float_x8x24_typeless;

	const auto r = match_taa_dispatch(sig, 2560, 1440);
	REQUIRE(r.verdict != MatchVerdict::no_match);
	CHECK(r.depth_srv == 1);
	CHECK(r.stencil_srv == 0);
}

TEST_CASE("a dispatch of the wrong size is rejected")
{
	auto sig = make_stray_taa();
	sig.group_count_x += 3;

	const auto r = match_taa_dispatch(sig, 2560, 1440);
	CHECK(r.verdict == MatchVerdict::no_match);
}

TEST_CASE("a dispatch missing the View constant buffer is rejected")
{
	auto sig = make_stray_taa();
	sig.has_view_cb = false;

	CHECK(match_taa_dispatch(sig, 2560, 1440).verdict == MatchVerdict::no_match);
}

TEST_CASE("a dispatch without a velocity SRV is rejected")
{
	auto sig = make_stray_taa();
	sig.srvs.erase(sig.srvs.begin() + 2);

	CHECK(match_taa_dispatch(sig, 2560, 1440).verdict == MatchVerdict::no_match);
}

TEST_CASE("a dispatch with only one colour SRV is rejected")
{
	auto sig = make_stray_taa();
	sig.srvs.pop_back();

	CHECK(match_taa_dispatch(sig, 2560, 1440).verdict == MatchVerdict::no_match);
}

TEST_CASE("the downsample UAV is optional")
{
	// r.TemporalAA.AllowDownsampling=0 or a different AA quality removes u1.
	auto sig = make_stray_taa();
	sig.uavs.pop_back();

	const auto r = match_taa_dispatch(sig, 2560, 1440);
	CHECK(r.verdict == MatchVerdict::hash_and_structural);
	CHECK_FALSE(r.has_downsample_uav);
}
