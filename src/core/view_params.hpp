// Decoding the UE 4.27 View uniform buffer. Pure logic: no Windows, no D3D, no ReShade, so
// this builds and is tested on Linux too.
//
// Offsets come from CLAUDE.md §2.6, which reproduces them three independent ways.
#pragma once

#include "ue4_view.hpp"

#include <cstddef>
#include <cstdint>

namespace stray_dlss::ue4 {

struct Float4
{
	float x = 0.0f;
	float y = 0.0f;
	float z = 0.0f;
	float w = 0.0f;
};

// UE4 compiles with D3D10_SHADER_PACK_MATRIX_ROW_MAJOR, so cb row r is M[r][0..3] and HLSL
// `mul(v, M)` is the row-vector convention. m[r * 4 + c] == M[r][c]. Transposing this into a
// column-vector library is the caller's job. (docs/RESEARCH.md §4.7)
struct Matrix4
{
	float m[16] = {};
};

struct ViewParams
{
	Matrix4 clip_to_prev_clip{};
	// The full seven-matrix block at rows 0-27 (order in ue4_view.hpp, HARD-via-mirror).
	// Kept whole so a plausibility failure can print every slice's orthonormality verdict
	// and turn a refusal into a member-order measurement in one log line.
	Matrix4 view_matrix_block[7]{};
	// TranslatedWorldToView (rows 12-15 — mirror-verified; equals view_matrix_block[3]).
	// The RR path derives the NoV rotation from it (nov_rotation_rows) and passes the
	// whole matrix as DLSSD's WorldToView; both consumers gate on
	// world_to_view_rotation_plausible first.
	Matrix4 translated_world_to_view{};
	// ViewToClip (rows 28-31) and ViewToClipNoAA (rows 32-35) — row 28/32 are measured
	// anchors (CLAUDE.md §2.6). NoAA is the jitter-free projection: jitter reaches NGX
	// separately as JitterOffset, so the unjittered matrix is the consistent one to pass.
	Matrix4 view_to_clip{};
	Matrix4 view_to_clip_no_aa{};
	Float4 temporal_aa_jitter{};  // (CurX, CurY, PrevX, PrevY) in clip/NDC units
	Float4 view_rect_min{};
	Float4 view_size_and_inv_size{};
	Float4 buffer_size_and_inv_size{};
	Float4 temporal_aa_params{};  // (Index, Count, JitterPixelsX, JitterPixelsY)
	float pre_exposure = 1.0f;
	float one_over_pre_exposure = 1.0f;
	// All four components of row 135 from ONE read, so the offset can be validated structurally
	// (.x a denormal, .w exactly 0.0) rather than by comparing numbers sampled at different
	// moments — which is the mistake that produced a wrong "fix" to this very row.
	Float4 pre_exposure_row{};
	float near_plane = 0.0f;
	float delta_time = 0.0f;
	float camera_cut = 0.0f;
};

// `size` must be at least kViewPrefixBytes. Returns false rather than reading past the end.
bool parse_view_params(const void *data, std::size_t size, ViewParams &out);

// A cheap sanity gate on a buffer we located by descriptor tracking rather than by name. It
// catches the two realistic failure modes: reading the wrong buffer entirely, and reading the
// right buffer at offsets that have slipped (row 131 is a (1,1,1,1) decoy).
bool view_params_plausible(const ViewParams &p);

// IS THIS VIEW THE ONE THIS DISPATCH BELONGS TO?
//
// `view_params_plausible` and the row-135 self-check both answer "is this A View uniform
// buffer" — and a shadow, cubemap-face or scene-capture view satisfies both, because it IS one.
// Neither says it is THIS view's. The CB is located by searching every bound constant buffer in
// slot order and keeping the first plausible hit, so a wrong-but-plausible candidate on a lower
// register wins and the search stops (MEASURED: b3 carrying a 4088x4088 view beat the real one
// on b4, facts §36.18).
//
// The test that separates them uses something the dispatch already tells us. UE 4.27's
// OutputViewRect is >= InputViewRect for every Main* config — equal at 1:1, larger when
// upscaling, never smaller (TemporalAA.cpp SetupViewRect) — and the dispatch covers the OUTPUT
// rect. So this view's InputViewRect (ViewSizeAndInvSize, row 130) cannot exceed what the
// dispatch covers. A 4088x4088 view against a 3840x2160 dispatch is not a close call; it is
// impossible.
//
// `covered_w`/`covered_h` are group_count * 8. Inclusive, so DLAA (view == dispatch) passes.
bool view_fits_dispatch(const ViewParams &p, std::uint32_t covered_w, std::uint32_t covered_h);

// Is the PreExposure / OneOverPreExposure pair (rows 135.y and 135.z) self-consistent? Separate
// from view_params_plausible ON PURPOSE — that gate governs the whole DLSS path, and those rows
// are [derived], so a wrong offset there would disable upscaling entirely. Callers that consume
// the value check this and fall back to a static scale.
bool pre_exposure_plausible(const ViewParams &p);

// DO TWO SURVIVING CANDIDATES DESCRIBE THE SAME VIEW?
//
// `view_fits_dispatch` closed the LOUD half of the wrong-view bug: an impostor whose rect was
// LARGER than the dispatch covers made the matcher refuse the real TAA pass, which showed up as
// `unclaimed` and as the visible flicker (facts §36.18-36.19). It cannot close the QUIET half.
// A second View uniform buffer whose rect is SMALLER than the dispatch covers passes every test
// we have — plausibility, row 135 and the fit bound — so if it sits on a lower root parameter it
// still wins the slot-order search, and DLSS is then fed another view's jitter, ClipToPrevClip
// and CameraCut with no counter firing at all. `wrongView` was ~1.8 per candidate dispatch, so
// the impostor is offered constantly; only the subset that failed loudly was ever measured.
//
// The instrument is ambiguity, not replay. Comparing the old "first plausible" rule against the
// new "first plausible that fits" rule can only ever reproduce the loud case, because the two
// rules differ EXACTLY when the first plausible candidate does not fit. What the quiet case
// looks like is instead: MORE THAN ONE surviving candidate on the same dispatch. If exactly one
// survives on every dispatch there is no quiet residue and the search's answer is forced; if two
// survive, the slot order decided which view DLSS was told about, and that count is the residue.
//
// Equivalent means "indistinguishable to everything downstream": two root parameters may point
// at the same suballocation, or at two byte-identical copies of one view's uniform buffer, and
// neither is ambiguity. Compared exactly (bitwise-equal floats), because that is what reading
// the same bytes twice produces; a tolerance would only hide a real second view.
bool view_params_equivalent(const ViewParams &a, const ViewParams &b);

// TemporalAAParams.zw IS TemporalJitterPixels, already in render-resolution pixels. NGX takes
// it with NO sign flip. (docs/RESEARCH.md §3.3)
Float2 ngx_jitter_offset(const ViewParams &p);

// UE4's bCameraCut is `!InputHistory.IsValid() || View.bCameraCut`, and the history-invalid
// half never reaches the View buffer — hence the third argument. (CLAUDE.md §2.8)
bool is_camera_cut(const ViewParams &p, bool history_or_velocity_is_1x1);

// Whether translated_world_to_view's upper 3x3 looks like the rigid rotation it must be:
// rows unit-length, mutually orthogonal. This is the loud-failure gate on the DERIVED row-8
// layout claim (ue4_view.hpp): if the rows hold anything else — the wrong rows, a scaled
// matrix, garbage — this returns false and the RR path must fall back rather than feed NGX
// a silently wrong NoV. Tolerance is loose (1e-2) because the buffer carries float32 of a
// double-precision engine transform.
// Convention note, provable in the tests: a 3x3 is orthogonal iff its transpose is, so
// this check passes a genuine rotation stored under EITHER matrix convention — it cannot
// distinguish them and does not try. The convention itself is settled by layout knowledge,
// not by this check: UE stores FMatrix row-major with the ROW-VECTOR convention
// (v' = v * M; docs/RESEARCH.md §4.7, proven live by the ClipToPrevClip path).
bool world_to_view_rotation_plausible(const Matrix4 &translated_world_to_view);

// The 3x3 the G-buffer resolve's NoV math needs, extracted with the convention made
// explicit. The shader computes n_view[i] = dot(row_i, n_world) — the COLUMN-vector form —
// while UE's stored matrix wants the row-vector form n_view = n_world * M, so this returns
// the TRANSPOSED upper 3x3 (the stored matrix's columns as rows). Handing the resolve the
// untransposed rows applies the inverse rotation: NoV goes wrong silently, which is why
// this lives here with a test instead of inline at the call site.
void nov_rotation_rows(const Matrix4 &translated_world_to_view, float out[3][3]);

// Member name for a view_matrix_block index (0-6), for the diagnostics line.
const char *view_matrix_block_name(int index);

} // namespace stray_dlss::ue4
