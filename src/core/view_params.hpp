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

// THE OTHER HALF OF THE SAME TEST, and the search GATES on it (facts §36.20, report §16.5).
//
// `view_fits_dispatch` catches an impostor whose rect is LARGER than the dispatch — the subset
// that failed loudly and became the 1.2% `unclaimed` (facts §36.18). An impostor whose rect is
// SMALLER passes it silently, and hands DLSS another view's jitter, ClipToPrevClip and
// CameraCut — and, because `fd.render_*` is read from the same buffer, an
// `InRenderSubrectDimensions` of 64x41 against the real 1920x1080 scene colour, which magnifies
// the top-left corner of the frame over the whole screen (the user's "carpet"). MEASURED: 37 of
// 62 DLSS feature creations in one session, `suspectSmall=162` in the same session's `[view]`
// line while nothing was gated on it.
//
// A candidate failing this is SKIPPED and the search continues, so the real view — usually on a
// HIGHER root parameter — is found and DLSS runs correctly rather than the frame being declined.
// `taa_signature::primary_view_shape_ok` is the create-site backstop for when it is not.
//
// It comes from the engine's own declared range, not from a guess:
// `FSceneViewScreenPercentageConfig::kMinTAAUpsampleResolutionFraction` is **0.5**
// (SceneView.h:1438-1439) — the same constant `seam::discover` already validates the
// ITemporalUpscaler vtable against. So for any `Main*` config the input view rect is at least
// half the output rect, and the dispatch covers the output rect.
//
// The slack is quantisation: the dispatch covers `group count * 8`, which rounds the real
// output rect UP by at most 7 px per axis, so the floor is loosened by 8 rather than being
// exact. A shadow or capture view is nowhere near this line — 512x512 against 3840x2160 is
// 0.13, not 0.5 — so the test does not need to be tight to be decisive.
bool view_fraction_plausible(const ViewParams &p, std::uint32_t covered_w,
                             std::uint32_t covered_h);

// Do these two decoded Views differ in the fields that actually REACH A TEMPORAL CONSUMER?
//
// This is what makes an ambiguity count mean something. Two root parameters can point at one
// suballocation, or hold byte-identical copies of the same view's uniform buffer — neither is a
// choice the search can get wrong, and counting them inflates the number with events where no
// view was ever at stake. Only a candidate that would hand DLSS DIFFERENT motion is ambiguity.
//
// The fields are exactly those a temporal consumer integrates: ClipToPrevClip element by
// element (the camera-motion reconstruction), TemporalAAParams (the jitter, CLAUDE.md §2.7) and
// CameraCut (§2.8). Exact comparison, not a tolerance: two reads of the same buffer are
// bit-identical, and anything else is a different view.
bool views_differ_temporally(const ViewParams &a, const ViewParams &b);

// Is the PreExposure / OneOverPreExposure pair (rows 135.y and 135.z) self-consistent? Separate
// from view_params_plausible ON PURPOSE — that gate governs the whole DLSS path, and those rows
// are [derived], so a wrong offset there would disable upscaling entirely. Callers that consume
// the value check this and fall back to a static scale.
bool pre_exposure_plausible(const ViewParams &p);

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

// WE PASS NO SUBRECT BASE TO NGX. Every evaluate hands colour, depth, motion vectors and output
// as whole textures with an implicit origin of (0,0), which is correct only while UE4's view rect
// starts at the buffer top-left. §5's rule for this field is "read it, do not assume it", so it
// was read: (0,0) in all 41 observations across 8 sessions on the box (facts §40).
//
// The plumbing is therefore deliberately NOT built. It would be untested code for a case that has
// never occurred here, and the obvious version of it is wrong anyway: the motion-vector texture
// NGX receives is OURS, written from 0, so it must not carry the engine's view-rect offset even
// when colour and depth would. Getting that split wrong offsets the guides against each other,
// which is worse than the uniform offset it was meant to fix.
//
// What is built instead is the loud failure the prime directives ask for.
inline bool view_rect_min_is_origin(const ViewParams &p)
{
	return p.view_rect_min.x == 0.0f && p.view_rect_min.y == 0.0f;
}

} // namespace stray_dlss::ue4
