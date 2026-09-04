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

	// ClipToWorld (rows 8-11 — the block slot the first shipped guess misread as
	// TranslatedWorldToView): inverse-projection-shaped, never orthonormal. Distinct
	// values so the block parse is checkable and the plausibility gate provably rejects it.
	put4(ue4::ViewRow::kViewMatrixBlock + 2 * 4 + 0, 0.83f, 0.0f, 0.0f, 0.0f);
	put4(ue4::ViewRow::kViewMatrixBlock + 2 * 4 + 1, 0.0f, 0.48f, 0.0f, 0.0f);
	put4(ue4::ViewRow::kViewMatrixBlock + 2 * 4 + 2, 0.0f, 0.0f, 0.0f, 0.1f);
	put4(ue4::ViewRow::kViewMatrixBlock + 2 * 4 + 3, 0.0f, 0.0f, 1.0f, 0.0f);
	// TranslatedWorldToView (rows 12-15, mirror-verified): a real rotation (90 degrees
	// about Z) with a translation row, so both the parse and the plausibility gate are
	// exercised.
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

TEST_CASE("the seven-matrix block layout is pinned to the mirror-verified order")
{
	// SceneView.h VIEW_UNIFORM_BUFFER_MEMBER_TABLE (~:1078, 4.27.2 mirror, fetched
	// 2026-08-31): TranslatedWorldToClip, WorldToClip, ClipToWorld, TranslatedWorldToView,
	// ViewToTranslatedWorld, TranslatedWorldToCameraView, CameraViewToTranslatedWorld,
	// then ViewToClip — whose MEASURED row-28 anchor is what pins the count at seven.
	// The first shipped guess put TranslatedWorldToView at row 8 (forgetting ClipToWorld)
	// and the live run refused every frame; this case makes that regression impossible.
	CHECK(ue4::ViewRow::kTranslatedWorldToView == 12);
	CHECK(ue4::ViewRow::kViewMatrixBlock + 7 * 4 == ue4::ViewRow::kViewToClip);
	CHECK(std::strcmp(ue4::view_matrix_block_name(2), "ClipToWorld") == 0);
	CHECK(std::strcmp(ue4::view_matrix_block_name(3), "TranslatedWorldToView") == 0);

	const auto buf = make_view_buffer();
	ue4::ViewParams p;
	REQUIRE(ue4::parse_view_params(buf.data(), buf.size(), p));

	// Block slot 2 reads rows 8-11 (the inverse-projection shape) and slot 3 IS the named
	// TranslatedWorldToView.
	CHECK(p.view_matrix_block[2].m[0] == doctest::Approx(0.83f));
	CHECK(p.view_matrix_block[2].m[11] == doctest::Approx(0.1f));
	CHECK(p.view_matrix_block[3].m[1] == doctest::Approx(p.translated_world_to_view.m[1]));

	// And the gate's verdicts across the block: ClipToWorld rejected, the rigid transform
	// at slot 3 accepted — the "no,no,no,YES..." pattern the diagnostics line prints.
	CHECK_FALSE(ue4::world_to_view_rotation_plausible(p.view_matrix_block[2]));
	CHECK(ue4::world_to_view_rotation_plausible(p.view_matrix_block[3]));
}

TEST_CASE("the plausibility check accepts a rotation under BOTH matrix conventions")
{
	// A 3x3 is orthogonal iff its transpose is, so the check cannot distinguish row-vector
	// from column-vector storage — and must not try. This case PROVES both pass, so the
	// convention question is settled by layout knowledge (UE stores row-vector, RESEARCH.md
	// §4.7), never by this gate.
	ue4::Matrix4 rot{}; // 90 degrees about Z, row-vector convention, translation in row 3
	rot.m[0 * 4 + 1] = 1.0f;
	rot.m[1 * 4 + 0] = -1.0f;
	rot.m[2 * 4 + 2] = 1.0f;
	rot.m[3 * 4 + 0] = 5.0f;
	rot.m[3 * 4 + 3] = 1.0f;
	CHECK(ue4::world_to_view_rotation_plausible(rot));

	ue4::Matrix4 transposed{}; // the same rotation, column-vector convention (translation column)
	for (int r = 0; r < 4; ++r)
		for (int c = 0; c < 4; ++c)
			transposed.m[r * 4 + c] = rot.m[c * 4 + r];
	CHECK(ue4::world_to_view_rotation_plausible(transposed));
}

TEST_CASE("nov_rotation_rows applies the row-vector convention, provably")
{
	// The stored matrix rotates 90 degrees about Z in the row-vector convention:
	// n_view = n_world * M, so world +X must land on view +Y.
	ue4::Matrix4 rot{};
	rot.m[0 * 4 + 1] = 1.0f;
	rot.m[1 * 4 + 0] = -1.0f;
	rot.m[2 * 4 + 2] = 1.0f;
	rot.m[3 * 4 + 3] = 1.0f;

	float rows[3][3] = {};
	ue4::nov_rotation_rows(rot, rows);

	const float n_world[3] = { 1.0f, 0.0f, 0.0f };
	float n_view[3] = {};
	for (int i = 0; i < 3; ++i)
		n_view[i] = rows[i][0] * n_world[0] + rows[i][1] * n_world[1] +
			rows[i][2] * n_world[2];

	// The reference: direct row-vector multiplication (n * M)[j] = sum_i n[i] * M[i][j].
	float reference[3] = {};
	for (int j = 0; j < 3; ++j)
		reference[j] = n_world[0] * rot.m[0 * 4 + j] + n_world[1] * rot.m[1 * 4 + j] +
			n_world[2] * rot.m[2 * 4 + j];

	for (int i = 0; i < 3; ++i)
		CHECK(n_view[i] == doctest::Approx(reference[i]));
	CHECK(n_view[1] == doctest::Approx(1.0f)); // +X -> +Y

	// The bug this helper exists to prevent: the UNtransposed rows apply the inverse
	// rotation (+X -> -Y here). If someone inlines the extraction again, this shows what
	// goes wrong.
	float wrong = rot.m[1 * 4 + 0] * n_world[0] + rot.m[1 * 4 + 1] * n_world[1] +
		rot.m[1 * 4 + 2] * n_world[2];
	CHECK(wrong == doctest::Approx(-1.0f));
}

// Pre-exposure is now load-bearing: the NR codec derives its whole proxy scale from
// OneOverPreExposure, and nvsdk_ngx_helpers.h:507 silently rewrites a zero InPreExposure to 1.0.
// One stale frame is a large history mis-scale with no log line, so the gate has to catch it.
// Rows 135.y and 135.z are reciprocals by construction, which makes the pair self-checking.
TEST_CASE("a zero or inconsistent pre-exposure pair is rejected")
{
	const auto buf = make_view_buffer();
	ue4::ViewParams good;
	REQUIRE(ue4::parse_view_params(buf.data(), buf.size(), good));
	// A real measured pair from the live game must pass.
	good.pre_exposure = 0.056f;
	good.one_over_pre_exposure = 1.0f / 0.056f;
	REQUIRE(ue4::pre_exposure_plausible(good));
	// The whole-view gate must stay INDIFFERENT to pre-exposure: it governs the entire DLSS
	// path, so folding this check into it would disable upscaling outright if the [derived]
	// rows were ever wrong.
	ue4::ViewParams no_exposure = good;
	no_exposure.pre_exposure = 0.0f;
	no_exposure.one_over_pre_exposure = 0.0f;
	CHECK(ue4::view_params_plausible(no_exposure));

	ue4::ViewParams zero = good;
	zero.pre_exposure = 0.0f;
	CHECK_FALSE(ue4::pre_exposure_plausible(zero));

	ue4::ViewParams zero_inv = good;
	zero_inv.one_over_pre_exposure = 0.0f;
	CHECK_FALSE(ue4::pre_exposure_plausible(zero_inv));

	// Both non-zero but not reciprocals: not the pre-exposure pair at all.
	ue4::ViewParams mismatched = good;
	mismatched.one_over_pre_exposure = 3.0f;
	CHECK_FALSE(ue4::pre_exposure_plausible(mismatched));
}

TEST_CASE("view_fits_dispatch separates THIS view from A view")
{
	// The measured case (facts §36.18): the CB search runs in slot order and used to keep the
	// first plausible hit, so b3 carrying a 4088x4088 view beat the real one on b4 on ~1.2% of
	// frames. Both are real View uniform buffers - plausibility and row 135 pass on both - so
	// the only thing that tells them apart is the dispatch itself.
	ue4::ViewParams p{};

	SUBCASE("the measured wrong view is rejected against the measured dispatch")
	{
		p.view_size_and_inv_size = { 4088.0f, 4088.0f, 0.0f, 0.0f };
		CHECK_FALSE(ue4::view_fits_dispatch(p, 3840, 2160));  // 480x270 groups x 8
	}
	SUBCASE("the real 50%-screen-percentage view is accepted")
	{
		p.view_size_and_inv_size = { 1920.0f, 1080.0f, 0.0f, 0.0f };
		CHECK(ue4::view_fits_dispatch(p, 3840, 2160));
	}
	SUBCASE("DLAA passes - the bound is inclusive")
	{
		// At 1:1 OutputViewRect == InputViewRect, so view == dispatch coverage exactly. An
		// exclusive bound here would reject every DLAA frame.
		p.view_size_and_inv_size = { 3840.0f, 2160.0f, 0.0f, 0.0f };
		CHECK(ue4::view_fits_dispatch(p, 3840, 2160));
	}
	SUBCASE("200% downsampling is still rejected, which is what the old gate was for")
	{
		p.view_size_and_inv_size = { 7680.0f, 4320.0f, 0.0f, 0.0f };
		CHECK_FALSE(ue4::view_fits_dispatch(p, 3840, 2160));
	}
	SUBCASE("only ONE axis needs to disagree")
	{
		p.view_size_and_inv_size = { 1920.0f, 4320.0f, 0.0f, 0.0f };
		CHECK_FALSE(ue4::view_fits_dispatch(p, 3840, 2160));
		p.view_size_and_inv_size = { 7680.0f, 1080.0f, 0.0f, 0.0f };
		CHECK_FALSE(ue4::view_fits_dispatch(p, 3840, 2160));
	}
	SUBCASE("a view with no extent is not accepted on this test's word")
	{
		p.view_size_and_inv_size = { 0.0f, 0.0f, 0.0f, 0.0f };
		CHECK_FALSE(ue4::view_fits_dispatch(p, 3840, 2160));
	}
	SUBCASE("with no dispatch extent to compare against, invent no refusal")
	{
		// The caller has nothing to judge with; the ordinary plausibility gate still applies.
		p.view_size_and_inv_size = { 1920.0f, 1080.0f, 0.0f, 0.0f };
		CHECK(ue4::view_fits_dispatch(p, 0, 0));
	}
}

TEST_CASE("view_fraction_plausible is the OTHER half - it catches an impostor that is too SMALL")
{
	// view_fits_dispatch catches a rect LARGER than the dispatch (the loud 1.2%, facts §36.18).
	// A rect that is SMALLER passes it silently and would hand DLSS another view's jitter,
	// ClipToPrevClip and CameraCut. The floor is the engine's OWN declared minimum,
	// FSceneViewScreenPercentageConfig::kMinTAAUpsampleResolutionFraction == 0.5 - the same
	// constant seam::discover already validates the ITemporalUpscaler vtable against.
	ue4::ViewParams p{};

	SUBCASE("the shipped 50% screen percentage sits exactly ON the floor and passes")
	{
		p.view_size_and_inv_size = { 1920.0f, 1080.0f, 0.0f, 0.0f };
		CHECK(ue4::view_fraction_plausible(p, 3840, 2160));
	}
	SUBCASE("70%, the other configuration this project runs, passes")
	{
		p.view_size_and_inv_size = { 2688.0f, 1512.0f, 0.0f, 0.0f };
		CHECK(ue4::view_fraction_plausible(p, 3840, 2160));
	}
	SUBCASE("DLAA passes")
	{
		p.view_size_and_inv_size = { 3840.0f, 2160.0f, 0.0f, 0.0f };
		CHECK(ue4::view_fraction_plausible(p, 3840, 2160));
	}
	SUBCASE("a shadow-sized view is nowhere near the line")
	{
		p.view_size_and_inv_size = { 512.0f, 512.0f, 0.0f, 0.0f };
		CHECK_FALSE(ue4::view_fraction_plausible(p, 3840, 2160));
		p.view_size_and_inv_size = { 1024.0f, 1024.0f, 0.0f, 0.0f };
		CHECK_FALSE(ue4::view_fraction_plausible(p, 3840, 2160));
	}
	SUBCASE("the quantisation slack is 8px, not a licence to drift")
	{
		// group count * 8 rounds the real output rect up by at most 7 per axis, so the floor is
		// loosened by 8 - enough for DivideAndRoundUp and nothing like enough for a wrong view.
		p.view_size_and_inv_size = { 1912.0f, 1072.0f, 0.0f, 0.0f };
		CHECK(ue4::view_fraction_plausible(p, 3840, 2160));
		p.view_size_and_inv_size = { 1700.0f, 1080.0f, 0.0f, 0.0f };
		CHECK_FALSE(ue4::view_fraction_plausible(p, 3840, 2160));
	}
	SUBCASE("no extent, or nothing to compare against")
	{
		p.view_size_and_inv_size = { 0.0f, 0.0f, 0.0f, 0.0f };
		CHECK_FALSE(ue4::view_fraction_plausible(p, 3840, 2160));
		p.view_size_and_inv_size = { 1920.0f, 1080.0f, 0.0f, 0.0f };
		CHECK(ue4::view_fraction_plausible(p, 0, 0));
	}
}

TEST_CASE("views_differ_temporally: duplicates are not ambiguity, different motion is")
{
	// An ambiguity counter that counts SURVIVING candidates is inflated by events where no view
	// was ever at stake - two root parameters pointing at one suballocation, or two copies of
	// the same view's uniform buffer. Only a candidate that would hand DLSS DIFFERENT motion is
	// a choice the search can get wrong.
	ue4::ViewParams a{};
	a.temporal_aa_params = { 8.0f, 8.0f, 0.25f, -0.125f };
	a.camera_cut = 0.0f;
	for (int i = 0; i < 16; ++i)
		a.clip_to_prev_clip.m[i] = static_cast<float>(i) * 0.5f;

	SUBCASE("a byte-identical copy is NOT ambiguity")
	{
		ue4::ViewParams b = a;
		CHECK_FALSE(ue4::views_differ_temporally(a, b));
	}
	SUBCASE("a different view rect alone is NOT ambiguity - it reaches no temporal consumer")
	{
		// The rect matters for the matcher, but DLSS integrates motion, not extents. Two
		// buffers agreeing on ClipToPrevClip/jitter/CameraCut give the same reprojection.
		ue4::ViewParams b = a;
		b.view_size_and_inv_size = { 1920.0f, 1080.0f, 0.0f, 0.0f };
		CHECK_FALSE(ue4::views_differ_temporally(a, b));
	}
	SUBCASE("any ClipToPrevClip element differing IS ambiguity")
	{
		for (int i = 0; i < 16; ++i)
		{
			ue4::ViewParams b = a;
			b.clip_to_prev_clip.m[i] += 1.0f;
			CHECK(ue4::views_differ_temporally(a, b));
		}
	}
	SUBCASE("jitter differing IS ambiguity")
	{
		ue4::ViewParams b = a;
		b.temporal_aa_params.z = 0.30f;
		CHECK(ue4::views_differ_temporally(a, b));
		ue4::ViewParams c = a;
		c.temporal_aa_params.w = 0.0f;
		CHECK(ue4::views_differ_temporally(a, c));
	}
	SUBCASE("CameraCut differing IS ambiguity")
	{
		ue4::ViewParams b = a;
		b.camera_cut = 1.0f;
		CHECK(ue4::views_differ_temporally(a, b));
	}
	SUBCASE("it is symmetric")
	{
		ue4::ViewParams b = a;
		b.camera_cut = 1.0f;
		CHECK(ue4::views_differ_temporally(a, b) == ue4::views_differ_temporally(b, a));
	}
}

TEST_CASE("the two bounds are COMPLEMENTARY - which is why the small impostor reached the screen")
{
	// The search now rejects a candidate that fails EITHER bound and keeps looking. This pins
	// why both are needed: the shadow/capture views that created DLSS features at 64x41 ->
	// 3840x2160 (report §16) all PASS view_fits_dispatch, because it only bounds from above.
	// Every rect here is one the live log actually named on a dispatch covering 3840x2160.
	const struct { float w, h; } impostors[] = {
		{ 64.0f, 34.0f }, { 64.0f, 41.0f }, { 64.0f, 52.0f }, { 128.0f, 109.0f },
		{ 128.0f, 126.0f }, { 256.0f, 240.0f }, { 1024.0f, 1024.0f },
	};
	for (const auto &r : impostors)
	{
		ue4::ViewParams p{};
		p.view_size_and_inv_size = { r.w, r.h, 0.0f, 0.0f };
		CHECK(ue4::view_fits_dispatch(p, 3840, 2160));          // the old filter lets it through
		CHECK_FALSE(ue4::view_fraction_plausible(p, 3840, 2160)); // the new one does not
	}

	SUBCASE("and the real views pass BOTH, so gating costs no legitimate frame")
	{
		// Each render rect against the dispatch that ACTUALLY covered it - group count * 8 over
		// that frame's own output rect. The dynamic-resolution rows are the point: under it the
		// output rect shrinks WITH the render rect, so the pair stays at ~0.5 and the 8px
		// quantisation slack is what absorbs the rounding. (Pairing 1920x1064 with a full
		// 3840x2160 dispatch would fail, and correctly so - that combination never occurs.)
		const struct { float w, h; std::uint32_t cw, ch; } real[] = {
			{ 1920.0f, 1080.0f, 3840, 2160 },  // 50% screen percentage, what the game ships
			{ 2688.0f, 1512.0f, 3840, 2160 },  // 70%
			{ 3840.0f, 2160.0f, 3840, 2160 },  // DLAA
			{ 1920.0f, 1070.0f, 3840, 2144 },  // dynamic res, 3840x2140 output, from the log
			{ 1920.0f, 1064.0f, 3840, 2128 },  // dynamic res, 3840x2127 output
			{ 1920.0f, 1066.0f, 3840, 2136 },  // dynamic res, 3840x2132 output
		};
		for (const auto &r : real)
		{
			ue4::ViewParams p{};
			p.view_size_and_inv_size = { r.w, r.h, 0.0f, 0.0f };
			CHECK(ue4::view_fits_dispatch(p, r.cw, r.ch));
			CHECK(ue4::view_fraction_plausible(p, r.cw, r.ch));
		}
	}

	SUBCASE("a portrait impostor fails the fraction test on its NARROW axis")
	{
		// 1064x2128 against 3840x2160: the height alone would pass, the width cannot. Both
		// axes are tested for exactly this shape.
		ue4::ViewParams p{};
		p.view_size_and_inv_size = { 1064.0f, 2128.0f, 0.0f, 0.0f };
		CHECK_FALSE(ue4::view_fraction_plausible(p, 3840, 2160));
	}
}
