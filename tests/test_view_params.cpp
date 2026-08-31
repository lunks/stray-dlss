#include <doctest/doctest.h>

#include "core/ue4_view.hpp"
#include "core/view_params.hpp"

#include <cstring>
#include <vector>

using namespace stray_dlss;

namespace {

// Builds a synthetic View constant buffer with known values at the measured offsets.
std::vector<unsigned char> make_view_buffer()
{
	std::vector<unsigned char> buf(ue4::kViewPrefixBytes, 0);

	auto put4 = [&](std::uint32_t row, float x, float y, float z, float w) {
		const float v[4] = { x, y, z, w };
		std::memcpy(buf.data() + ue4::row_to_byte(row), v, sizeof(v));
	};

	// ClipToPrevClip: rows 122..125, filled with 0..15 so row-major order is checkable.
	for (std::uint32_t r = 0; r < 4; ++r)
		put4(ue4::ViewRow::kClipToPrevClip + r,
			static_cast<float>(r * 4 + 0), static_cast<float>(r * 4 + 1),
			static_cast<float>(r * 4 + 2), static_cast<float>(r * 4 + 3));

	// TranslatedWorldToView (rows 8-11, derived): a real rotation (90 degrees about Z)
	// with a translation row, so both the parse and the plausibility gate are exercised.
	put4(ue4::ViewRow::kTranslatedWorldToView + 0, 0.0f, 1.0f, 0.0f, 0.0f);
	put4(ue4::ViewRow::kTranslatedWorldToView + 1, -1.0f, 0.0f, 0.0f, 0.0f);
	put4(ue4::ViewRow::kTranslatedWorldToView + 2, 0.0f, 0.0f, 1.0f, 0.0f);
	put4(ue4::ViewRow::kTranslatedWorldToView + 3, 5.0f, 6.0f, 7.0f, 1.0f);
	// ViewToClip (rows 28-31) and ViewToClipNoAA (rows 32-35): projection-shaped, with
	// DIFFERENT diagonals so reading the wrong matrix is caught.
	put4(ue4::ViewRow::kViewToClip + 0, 1.21f, 0.0f, 0.0f, 0.0f);
	put4(ue4::ViewRow::kViewToClip + 1, 0.0f, 2.15f, 0.0f, 0.0f);
	put4(ue4::ViewRow::kViewToClip + 2, 0.01f, -0.02f, 0.0f, 1.0f); // jittered off-centre terms
	put4(ue4::ViewRow::kViewToClip + 3, 0.0f, 0.0f, 10.0f, 0.0f);
	put4(ue4::ViewRow::kViewToClipNoAA + 0, 1.2f, 0.0f, 0.0f, 0.0f);
	put4(ue4::ViewRow::kViewToClipNoAA + 1, 0.0f, 2.1f, 0.0f, 0.0f);
	put4(ue4::ViewRow::kViewToClipNoAA + 2, 0.0f, 0.0f, 0.0f, 1.0f);
	put4(ue4::ViewRow::kViewToClipNoAA + 3, 0.0f, 0.0f, 10.0f, 0.0f);
	put4(ue4::ViewRow::kTemporalAAJitter, 0.001f, -0.002f, 0.003f, -0.004f);
	put4(ue4::ViewRow::kViewRectMin, 0.0f, 0.0f, 0.0f, 0.0f);
	put4(ue4::ViewRow::kViewSizeAndInvSize, 2560.0f, 1440.0f, 1.0f / 2560.0f, 1.0f / 1440.0f);
	put4(ue4::ViewRow::kBufferSizeAndInvSize, 2560.0f, 1440.0f, 1.0f / 2560.0f, 1.0f / 1440.0f);
	put4(ue4::ViewRow::kTemporalAAParams, 3.0f, 8.0f, 0.25f, -0.125f);
	put4(ue4::ViewRow::kPreExposureRow, 0.0f, 2.0f, 0.5f, 0.0f); // .y PreExposure, .z 1/PreExposure
	put4(ue4::ViewRow::kNearPlaneRow, 10.0f, 0.0f, 0.0f, 0.0f);
	put4(ue4::ViewRow::kDeltaTimeRow, 0.016f, 0.0f, 0.0f, 0.0f);
	put4(ue4::ViewRow::kCameraCutRow, 0.0f, 0.0f, 0.0f, 0.0f);

	return buf;
}

} // namespace

TEST_CASE("parse_view_params reads every field from the measured offsets")
{
	const auto buf = make_view_buffer();
	ue4::ViewParams p;
	REQUIRE(ue4::parse_view_params(buf.data(), buf.size(), p));

	CHECK(p.temporal_aa_params.z == doctest::Approx(0.25f));
	CHECK(p.temporal_aa_params.w == doctest::Approx(-0.125f));
	CHECK(p.view_size_and_inv_size.x == doctest::Approx(2560.0f));
	CHECK(p.view_size_and_inv_size.y == doctest::Approx(1440.0f));
	CHECK(p.buffer_size_and_inv_size.x == doctest::Approx(2560.0f));
	CHECK(p.pre_exposure == doctest::Approx(2.0f));
	CHECK(p.one_over_pre_exposure == doctest::Approx(0.5f));
	CHECK(p.near_plane == doctest::Approx(10.0f));
	CHECK(p.delta_time == doctest::Approx(0.016f));
	CHECK(p.camera_cut == doctest::Approx(0.0f));

	// Row-major: element [r][c] lives at m[r * 4 + c].
	for (int i = 0; i < 16; ++i)
		CHECK(p.clip_to_prev_clip.m[i] == doctest::Approx(static_cast<float>(i)));
}

TEST_CASE("parse_view_params rejects a short buffer")
{
	const auto buf = make_view_buffer();
	ue4::ViewParams p;
	CHECK_FALSE(ue4::parse_view_params(buf.data(), ue4::kViewPrefixBytes - 1, p));
	CHECK_FALSE(ue4::parse_view_params(nullptr, ue4::kViewPrefixBytes, p));
}

TEST_CASE("ngx_jitter_offset passes TemporalAAParams.zw through unchanged")
{
	// docs/RESEARCH.md §3.3: NVIDIA's own UE plugin assigns View.TemporalJitterPixels straight
	// to InJitterOffsetX/Y. No negation, no scaling. This test exists to stop anyone "fixing"
	// it by reapplying the negative Y from the NDC derivation.
	const auto buf = make_view_buffer();
	ue4::ViewParams p;
	REQUIRE(ue4::parse_view_params(buf.data(), buf.size(), p));

	const ue4::Float2 jitter = ue4::ngx_jitter_offset(p);
	CHECK(jitter.x == doctest::Approx(0.25f));
	CHECK(jitter.y == doctest::Approx(-0.125f));
}

TEST_CASE("view_params_plausible catches a buffer read at the wrong offset")
{
	const auto buf = make_view_buffer();
	ue4::ViewParams good;
	REQUIRE(ue4::parse_view_params(buf.data(), buf.size(), good));
	CHECK(ue4::view_params_plausible(good));

	ue4::ViewParams bad = good;
	bad.view_size_and_inv_size = { 0.0f, 0.0f, 0.0f, 0.0f };
	CHECK_FALSE(ue4::view_params_plausible(bad));

	// The (1,1,1,1) decoy at row 131 would land here if offsets slipped by one row.
	ue4::ViewParams decoy = good;
	decoy.view_size_and_inv_size = { 1.0f, 1.0f, 1.0f, 1.0f };
	CHECK_FALSE(ue4::view_params_plausible(decoy));

	// Jitter outside [-0.5, 0.5] means we are not reading TemporalAAParams.
	ue4::ViewParams wild = good;
	wild.temporal_aa_params.z = 7.0f;
	CHECK_FALSE(ue4::view_params_plausible(wild));
}

TEST_CASE("is_camera_cut ORs all three signals")
{
	const auto buf = make_view_buffer();
	ue4::ViewParams p;
	REQUIRE(ue4::parse_view_params(buf.data(), buf.size(), p));

	// Baseline: jitter .zw differs from .xy, CameraCut is 0, nothing is 1x1.
	CHECK_FALSE(ue4::is_camera_cut(p, false));

	// Signal 1: View.CameraCut.
	ue4::ViewParams cut = p;
	cut.camera_cut = 1.0f;
	CHECK(ue4::is_camera_cut(cut, false));

	// Signal 2: previous jitter equals current.
	ue4::ViewParams same = p;
	same.temporal_aa_jitter = { 0.001f, -0.002f, 0.001f, -0.002f };
	CHECK(ue4::is_camera_cut(same, false));

	// Signal 3: a 1x1 history or velocity texture (BlackDummy).
	CHECK(ue4::is_camera_cut(p, true));
}

TEST_CASE("the RR matrices parse from their rows, row-major")
{
	const auto buf = make_view_buffer();
	ue4::ViewParams p;
	REQUIRE(ue4::parse_view_params(buf.data(), buf.size(), p));

	// TranslatedWorldToView: rotation rows as written, translation in row 3.
	CHECK(p.translated_world_to_view.m[0 * 4 + 1] == doctest::Approx(1.0f));
	CHECK(p.translated_world_to_view.m[1 * 4 + 0] == doctest::Approx(-1.0f));
	CHECK(p.translated_world_to_view.m[2 * 4 + 2] == doctest::Approx(1.0f));
	CHECK(p.translated_world_to_view.m[3 * 4 + 0] == doctest::Approx(5.0f));

	// The two projections carry DIFFERENT diagonals — reading row 28 for row 32 (or vice
	// versa) fails here.
	CHECK(p.view_to_clip.m[0] == doctest::Approx(1.21f));
	CHECK(p.view_to_clip.m[5] == doctest::Approx(2.15f));
	CHECK(p.view_to_clip_no_aa.m[0] == doctest::Approx(1.2f));
	CHECK(p.view_to_clip_no_aa.m[5] == doctest::Approx(2.1f));
	// NoAA has no off-centre terms; the jittered one does.
	CHECK(p.view_to_clip.m[2 * 4 + 0] == doctest::Approx(0.01f));
	CHECK(p.view_to_clip_no_aa.m[2 * 4 + 0] == doctest::Approx(0.0f));
}

TEST_CASE("world_to_view_rotation_plausible gates the derived rows loudly")
{
	// The synthetic buffer's row 8 holds a genuine rotation: plausible.
	const auto buf = make_view_buffer();
	ue4::ViewParams p;
	REQUIRE(ue4::parse_view_params(buf.data(), buf.size(), p));
	CHECK(ue4::world_to_view_rotation_plausible(p.translated_world_to_view));

	// Identity: plausible.
	ue4::Matrix4 identity{};
	identity.m[0] = identity.m[5] = identity.m[10] = identity.m[15] = 1.0f;
	CHECK(ue4::world_to_view_rotation_plausible(identity));

	// A scaled matrix is NOT a rotation: rows are not unit length.
	ue4::Matrix4 scaled = identity;
	scaled.m[0] = 2.0f;
	CHECK_FALSE(ue4::world_to_view_rotation_plausible(scaled));

	// Non-orthogonal rows (a shear) fail on the dot-product check.
	ue4::Matrix4 shear = identity;
	shear.m[1] = 0.2f; // row 0 leans into Y while row 1 is still Y
	CHECK_FALSE(ue4::world_to_view_rotation_plausible(shear));

	// All-zero (the wrong-row / uninitialised case) fails.
	ue4::Matrix4 zero{};
	CHECK_FALSE(ue4::world_to_view_rotation_plausible(zero));

	// A projection matrix at the wrong row fails: its rows are nothing like unit length.
	CHECK_FALSE(ue4::world_to_view_rotation_plausible(p.view_to_clip));
}
