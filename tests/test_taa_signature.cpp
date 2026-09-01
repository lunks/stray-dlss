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
	// upsamples 1080p to 4K. A dispatch smaller than the pass's own inputs is downsampling,
	// which DLSS cannot do.
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

// ---------------------------------------------------------------------------------------
// DLAA (1:1) — the shapes UE 4.27 actually produces at 100% screen percentage.
//
// The dispatch and the output TEXTURE are two different quantities in UE 4.27
// (Release-4.27 branch, TemporalAA.cpp):
//
//   :950  the dispatch is GetGroupCount(PracticableDestRect.Size(), 8) — the output VIEW RECT
//   :596  the texture is allocated at FTAAPassParameters::GetOutputExtent():
//             Main:           SceneColorInput->Desc.Extent
//             MainUpsampling: Max(InputExtent, Quantize(OutputViewRect.Size()))
//
// Stray ships r.TemporalAA.Upsampling=True and nothing downgrades TemporalUpscale at a
// resolution fraction of 1.0 (SceneRendering.cpp:2546 gates only on the AA method), so the
// game runs MainUpsampling at EVERY screen percentage — 100% included, where InputViewRect
// and OutputViewRect are equal.
//
// That is the asymmetry. While the pass really upscales, InputExtent is the smaller buffer,
// so the Max() pins the output texture to exactly Quantize(OutputViewRect) — the dispatched
// rect — and an equality test is guaranteed to hold. At 1:1 the quantized output view size
// can no longer exceed InputExtent, the Max() degenerates to InputExtent, and NOTHING relates
// the texture's extent to the dispatch: any scene-colour buffer left larger than the current
// view rect (UE4 shrinks the scene targets lazily, and this session has been seen at both
// 3840x2160 and 2560x1440) flows straight into the output extent while the dispatch stays at
// the view rect. Below 100% that same inflation is absorbed by the Max(); at 100% it rejects
// the pass outright. These cases pin that down.
// ---------------------------------------------------------------------------------------

TEST_CASE("DLAA: the output texture may be larger than the dispatched rect")
{
	// 1:1 with the scene buffer still sized for a larger earlier view rect — everything the
	// pass binds is at the buffer extent, the dispatch is over the view rect. This is real
	// FTAAStandaloneCS and must match. Before the fix it returned no_match ("dispatch size
	// does not cover the output UAV at 8x8"), which is DLAA silently never engaging while
	// 50% and 70% keep working.
	auto sig = make_stray_taa(4096, 2304, 4096, 2304);
	sig.group_count_x = (3840 + kTaaTileSize - 1) / kTaaTileSize; // OutputViewRect 3840x2160
	sig.group_count_y = (2160 + kTaaTileSize - 1) / kTaaTileSize;

	const auto r = match_taa_dispatch(sig, 3840, 2160);

	REQUIRE(r.verdict == MatchVerdict::hash_and_structural);
	CHECK(r.output_uav == 0);
	// DLSS must be created for the rect the dispatch WRITES, never for the allocation: sizing
	// the feature to 4096x2304 would ask it to produce pixels the engine never writes or reads.
	CHECK(r.output_width == 3840);
	CHECK(r.output_height == 2160);
	CHECK_FALSE(r.is_upsampling);
}

TEST_CASE("DLAA: a clean 1:1 frame still reports the dispatched rect")
{
	const auto r = match_taa_dispatch(make_stray_taa(3840, 2160, 3840, 2160), 3840, 2160);

	CHECK(r.verdict == MatchVerdict::hash_and_structural);
	CHECK(r.output_width == 3840);
	CHECK(r.output_height == 2160);
	CHECK(r.render_width == 3840);
	CHECK(r.render_height == 2160);
}

TEST_CASE("DLAA: the half-res TAA_DOWNSAMPLE UAV never steals the output slot")
{
	// TAA_DOWNSAMPLE puts OutComputeTexDownsampled at u1, half the output extent
	// (TAAStandalone.usf; bDownsample = bAllowDownsampleSceneColor && bUseFast, and Stray
	// runs r.PostProcessAAQuality=3 so bUseFast is true). u0 stays the output.
	auto sig = make_stray_taa(3840, 2160, 3840, 2160);
	sig.uavs.push_back({ 1, 0x8000, TexFormat::r11g11b10_float, 1920, 1080 });

	const auto r = match_taa_dispatch(sig, 3840, 2160);

	CHECK(r.verdict == MatchVerdict::hash_and_structural);
	CHECK(r.output_uav == 0);
	CHECK(r.output_width == 3840);
	CHECK(r.has_downsample_uav);
}

TEST_CASE("200% screen percentage is still refused: the dispatch is smaller than the inputs")
{
	// Measured live: 7680x4320 rendered, downsampled to 3840x2160. DLSS upscales by
	// definition and cannot take an input larger than its output, so the matcher must keep
	// rejecting this — the relaxed coverage rule must not let it through.
	auto sig = make_stray_taa(7680, 4320, 7680, 4320);
	sig.group_count_x = (3840 + 7) / 8; // GetOutputExtent() = Max(7680x4320, 3840x2160)
	sig.group_count_y = (2160 + 7) / 8; // while PracticableDestRect is 3840x2160

	CHECK(match_taa_dispatch(sig, 7680, 4320).verdict == MatchVerdict::no_match);
}

TEST_CASE("a dispatch that runs past its output UAV is refused")
{
	// Writing outside the UAV is not something FTAAStandaloneCS ever does; a pass whose
	// dispatch exceeds its own output is something else entirely.
	auto sig = make_stray_taa(1920, 1080, 3840, 2160); // dispatch sized for 3840x2160
	sig.uavs[0].width = 1920;                          // ...into a 1920x1080 UAV
	sig.uavs[0].height = 1080;

	CHECK(match_taa_dispatch(sig, 1920, 1080).verdict == MatchVerdict::no_match);
}

TEST_CASE("the view rect is a lower bound on the dispatch, not just decoration")
{
	// A 1:1-shaped pass whose dispatch covers only part of the view rect is not the TAA
	// resolve. This is the bound that keeps the relaxed 1:1 rule honest.
	auto sig = make_stray_taa(3840, 2160, 3840, 2160);
	sig.group_count_x = (1920 + kTaaTileSize - 1) / kTaaTileSize;
	sig.group_count_y = (1080 + kTaaTileSize - 1) / kTaaTileSize;

	CHECK(match_taa_dispatch(sig, 3840, 2160).verdict == MatchVerdict::no_match);
}

TEST_CASE("the working SR configurations are unchanged")
{
	// 50% and 70% must behave EXACTLY as before the DLAA fix: these are the two settings
	// confirmed working on the target, and the output rect they hand DLSS is load-bearing.
	{
		const auto r = match_taa_dispatch(make_stray_taa(1920, 1080, 3840, 2160), 1920, 1080);
		CHECK(r.verdict == MatchVerdict::hash_and_structural);
		CHECK(r.render_width == 1920);
		CHECK(r.output_width == 3840);
		CHECK(r.output_height == 2160);
		CHECK(r.is_upsampling);
	}
	{
		const auto r = match_taa_dispatch(make_stray_taa(2688, 1512, 3840, 2160), 2688, 1512);
		CHECK(r.verdict == MatchVerdict::hash_and_structural);
		CHECK(r.render_width == 2688);
		CHECK(r.output_width == 3840);
		CHECK(r.output_height == 2160);
		CHECK(r.is_upsampling);
	}
	{
		// The same 50% frame with depth/velocity/colour at the full scene-buffer extent,
		// which is how they were captured at 1440p (taa_hook.cpp reads the render rect from
		// View.ViewSizeAndInvSize for exactly this reason).
		auto sig = make_stray_taa(2560, 1440, 2560, 1440);
		const auto r = match_taa_dispatch(sig, 1280, 720);
		CHECK(r.verdict == MatchVerdict::hash_and_structural);
		CHECK(r.output_width == 2560);
		CHECK(r.output_height == 1440);
	}
}

TEST_CASE("a dispatch with no colour UAV is rejected")
{
	auto sig = make_stray_taa();
	sig.uavs.clear();

	CHECK(match_taa_dispatch(sig, 1920, 1080).verdict == MatchVerdict::no_match);
}

TEST_CASE("known-hash table: live-measured permutations in, look-alikes out")
{
	// The three live-measured TAA hashes are all cooked permutations...
	CHECK(stray_dlss::is_known_taa_hash(0x901e041a7cadc9dbull)); // 4K/50%
	CHECK(stray_dlss::is_known_taa_hash(0xd2e4d8c23c362ed1ull)); // 1440p/50%
	CHECK(stray_dlss::is_known_taa_hash(0xe14e7fc8d0db9b0full));
	// ...and the two convincing look-alikes are not.
	CHECK(!stray_dlss::is_known_taa_hash(stray_dlss::kDenoiserLookalikeHash));
	CHECK(!stray_dlss::is_known_taa_hash(stray_dlss::kSecondCandidateHash));
	CHECK(!stray_dlss::is_known_taa_hash(0ull));
}

TEST_CASE("any cooked permutation earns hash_and_structural, not only 0x901e")
{
	stray_dlss::DispatchSignature s = make_stray_taa(1280, 720, 2560, 1440);
	s.shader_hash = 0xd2e4d8c23c362ed1ull;
	const auto r = stray_dlss::match_taa_dispatch(s, 1280, 720);
	CHECK(r.verdict == stray_dlss::MatchVerdict::hash_and_structural);
}

TEST_CASE("the SSD temporal-accumulation family: baked members plus runtime extension")
{
	// The RR guide-resolve trigger fires on any family member (content-alive point). Two
	// permutations are baked (the ones named offline; #5 is the measured live dispatcher);
	// the rest arrive via stray-dlss-ssd-hashes.txt through set_extra_ssd_hashes.
	CHECK(stray_dlss::is_ssd_temporal_hash(stray_dlss::kDenoiserLookalikeHash));
	CHECK(stray_dlss::is_ssd_temporal_hash(stray_dlss::kSecondCandidateHash));
	// TAA permutations are NOT SSD members - the families must never blur together.
	CHECK(!stray_dlss::is_ssd_temporal_hash(stray_dlss::kTaaMainHash));
	CHECK(!stray_dlss::is_ssd_temporal_hash(0));

	const std::uint64_t extras[2] = { 0x1111222233334444ull, 0x5555666677778888ull };
	stray_dlss::set_extra_ssd_hashes(extras, 2);
	CHECK(stray_dlss::is_ssd_temporal_hash(0x1111222233334444ull));
	CHECK(stray_dlss::is_ssd_temporal_hash(0x5555666677778888ull));
	CHECK(stray_dlss::is_ssd_temporal_hash(stray_dlss::kDenoiserLookalikeHash)); // baked survive

	// A reload replaces the extra set entirely.
	stray_dlss::set_extra_ssd_hashes(nullptr, 0);
	CHECK(!stray_dlss::is_ssd_temporal_hash(0x1111222233334444ull));
}

TEST_CASE("the eye-adaptation finder: t0 by register, structural fallback, dummies rejected")
{
	using stray_dlss::BoundTexture;
	using stray_dlss::TexFormat;

	// The measured §2.3 binding set: t0 is the 1x1 RGBA32F eye adaptation.
	std::vector<BoundTexture> srvs = {
		{ 0, 0x1000, TexFormat::r32g32b32a32_float, 1, 1 },
		{ 1, 0x2000, TexFormat::r16g16b16a16_float, 1920, 1080 },
		{ 2, 0x3000, TexFormat::r32_float_x8x24_typeless, 1920, 1080 },
		{ 3, 0x4000, TexFormat::r16g16b16a16_unorm, 1920, 1080 },
	};
	CHECK(stray_dlss::find_eye_adaptation_srv(srvs) == 0x1000);

	// Register t0 wins over a stray structural 1x1 RGBA32F elsewhere.
	srvs.push_back({ 9, 0x9000, TexFormat::r32g32b32a32_float, 1, 1 });
	CHECK(stray_dlss::find_eye_adaptation_srv(srvs) == 0x1000);

	// A permutation with shifted registers still yields the texture structurally.
	std::vector<BoundTexture> shifted = {
		{ 0, 0x2000, TexFormat::r16g16b16a16_float, 1920, 1080 },
		{ 2, 0x1000, TexFormat::r32g32b32a32_float, 1, 1 },
	};
	CHECK(stray_dlss::find_eye_adaptation_srv(shifted) == 0x1000);

	// Camera-cut 1x1 dummies are DIFFERENT formats and must never be picked — even at t0.
	std::vector<BoundTexture> cut = {
		{ 0, 0x5000, TexFormat::r16g16b16a16_float, 1, 1 }, // a dummy squatting on t0
		{ 3, 0x6000, TexFormat::r16g16b16a16_unorm, 1, 1 }, // velocity BlackDummy
	};
	CHECK(stray_dlss::find_eye_adaptation_srv(cut) == 0);

	// A full-size RGBA32F buffer is not the eye adaptation; absence returns 0.
	std::vector<BoundTexture> none = {
		{ 0, 0x7000, TexFormat::r32g32b32a32_float, 256, 256 },
	};
	CHECK(stray_dlss::find_eye_adaptation_srv(none) == 0);
	CHECK(stray_dlss::find_eye_adaptation_srv({}) == 0);
}
