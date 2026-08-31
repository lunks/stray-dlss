#include <doctest/doctest.h>

#include "core/taa_signature.hpp"

using namespace stray_dlss;

namespace {

// The REAL captured bindings of Stray's TAA pass, copied from a live dispatch report:
//
//   0x901e041a7cadc9db  dispatch 480x270 -> 3840x2160
//     t0  1x1                                 EyeAdaptationTexture
//     t1  1920x1080                           InputSceneColor
//     t2  1920x1080  R32_FLOAT_X8X24 (depth)  SceneDepthTexture   ┐ same resource
//     t3  1920x1080  RGBA16_UNORM (velocity)  GBufferVelocity     │
//     t4  1920x1080  X32_G8X24_UINT (stencil) StencilTexture      ┘
//     t5  1920x1080  RGBA16_FLOAT             HistoryBuffer_0
//     u0  3840x2160  RGBA16_FLOAT             OutComputeTex_0
//
// The game runs temporal upsampling, so the output is 4K while every input is 1080p.
constexpr std::uint64_t kDepthRes = 0x23378480;

DispatchSignature make_stray_taa(std::uint32_t rw = 1920, std::uint32_t rh = 1080,
                                 std::uint32_t ow = 3840, std::uint32_t oh = 2160)
{
	DispatchSignature s;
	s.shader_hash = kTaaMainHash;
	s.has_view_cb = true;
	s.group_count_x = (ow + kTaaTileSize - 1) / kTaaTileSize;
	s.group_count_y = (oh + kTaaTileSize - 1) / kTaaTileSize;
	s.group_count_z = 1;

	s.srvs = {
		{ 0, 0x1000, TexFormat::r32g32b32a32_float, 1, 1 },              // eye adaptation
		{ 1, 0x2000, TexFormat::r16g16b16a16_float, rw, rh },            // scene colour
		{ 2, kDepthRes, TexFormat::r32_float_x8x24_typeless, rw, rh },   // depth
		{ 3, 0x3000, TexFormat::r16g16b16a16_unorm, rw, rh },            // velocity
		{ 4, kDepthRes, TexFormat::x32_typeless_g8x24_uint, rw, rh },    // stencil, SAME resource
		{ 5, 0x4000, TexFormat::r16g16b16a16_float, rw, rh },            // history
	};
	s.uavs = {
		{ 0, 0x5000, TexFormat::r16g16b16a16_float, ow, oh },
	};
	return s;
}

} // namespace

TEST_CASE("the real captured TAA dispatch matches on hash and structure")
{
	const auto sig = make_stray_taa();
	const auto r = match_taa_dispatch(sig, 1920, 1080);

	CHECK(r.verdict == MatchVerdict::hash_and_structural);
	CHECK(r.depth_srv == 2);
	CHECK(r.stencil_srv == 4);
	CHECK(r.velocity_srv == 3);
	CHECK(r.output_uav == 0);
	CHECK(r.output_width == 3840);
	CHECK(r.output_height == 2160);
	CHECK(r.is_upsampling);
	CHECK_FALSE(r.camera_cut_dummies);
}

TEST_CASE("depth and stencil are identified by sharing one resource, not by register")
{
	// This pairing is the strongest runtime discriminator there is, and it is precisely what
	// the original heuristic scored as "depth=2" and treated as a disqualifier.
	auto sig = make_stray_taa();
	sig.srvs[2].slot = 4;
	sig.srvs[4].slot = 2;

	const auto r = match_taa_dispatch(sig, 1920, 1080);
	REQUIRE(r.verdict == MatchVerdict::hash_and_structural);
	CHECK(r.depth_srv == 4);
	CHECK(r.stencil_srv == 2);
}

TEST_CASE("a camera-cut frame still matches, and is flagged")
{
	// On a cut UE4 swaps velocity and history for the 1x1 BlackDummy. That is the TAA pass
	// resetting, not a different pass — rejecting it here is the exact error that mislabelled
	// this shader a false positive for the whole project.
	auto sig = make_stray_taa();
	sig.srvs[3] = { 3, 0x9000, TexFormat::unknown, 1, 1 };            // velocity -> dummy
	sig.srvs[5] = { 5, 0x9000, TexFormat::r16g16b16a16_float, 1, 1 }; // history  -> dummy

	const auto r = match_taa_dispatch(sig, 1920, 1080);
	CHECK(r.verdict == MatchVerdict::hash_and_structural);
	CHECK(r.camera_cut_dummies);
}

TEST_CASE("structure alone suffices when the hash is unfamiliar")
{
	auto sig = make_stray_taa();
	sig.shader_hash = 0xdeadbeefcafef00dull;

	const auto r = match_taa_dispatch(sig, 1920, 1080);
	CHECK(r.verdict == MatchVerdict::structural_only);
	CHECK(r.depth_srv == 2);
}

TEST_CASE("the denoiser look-alike is excluded")
{
	// 0x1708ec956099e259 reads depth and velocity and reprojects with ClipToPrevClip, so its
	// bindings look convincing. It declares cb1[126] and never indexes View row 144, so it
	// cannot be FTAAStandaloneCS.
	auto sig = make_stray_taa();
	sig.shader_hash = kDenoiserLookalikeHash;

	CHECK(match_taa_dispatch(sig, 1920, 1080).verdict == MatchVerdict::excluded);
}

TEST_CASE("the eleven-SRV candidate is excluded")
{
	auto sig = make_stray_taa();
	sig.shader_hash = kSecondCandidateHash;

	CHECK(match_taa_dispatch(sig, 1920, 1080).verdict == MatchVerdict::excluded);
}

TEST_CASE("a pass without the depth+stencil pair is rejected")
{
	auto sig = make_stray_taa();
	sig.srvs[4].resource = 0xBEEF; // stencil over a DIFFERENT resource

	CHECK(match_taa_dispatch(sig, 1920, 1080).verdict == MatchVerdict::no_match);
}

TEST_CASE("the dispatch must cover the OUTPUT rect, not the render rect")
{
	// Matching against the render rect would reject the real pass outright, since the game
	// upsamples 1080p to 4K.
	auto sig = make_stray_taa();
	sig.group_count_x = (1920 + 7) / 8;
	sig.group_count_y = (1080 + 7) / 8;

	CHECK(match_taa_dispatch(sig, 1920, 1080).verdict == MatchVerdict::no_match);
}

TEST_CASE("a 1:1 render-to-output pass matches and reports no upsampling")
{
	// What DLAA, or the game at 100% screen percentage, would look like.
	const auto sig = make_stray_taa(3840, 2160, 3840, 2160);
	const auto r = match_taa_dispatch(sig, 3840, 2160);

	CHECK(r.verdict == MatchVerdict::hash_and_structural);
	CHECK_FALSE(r.is_upsampling);
	CHECK(r.output_width == 3840);
}

TEST_CASE("a dispatch with no colour UAV is rejected")
{
	auto sig = make_stray_taa();
	sig.uavs.clear();

	CHECK(match_taa_dispatch(sig, 1920, 1080).verdict == MatchVerdict::no_match);
}
