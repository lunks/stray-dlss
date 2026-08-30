// Decoding the UE 4.27 View uniform buffer. Pure logic: no Windows, no D3D, no ReShade, so
// this builds and is tested on Linux too.
//
// Offsets come from CLAUDE.md §2.6, which reproduces them three independent ways.
#pragma once

#include "ue4_view.hpp"

#include <cstddef>

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
	Float4 temporal_aa_jitter{};  // (CurX, CurY, PrevX, PrevY) in clip/NDC units
	Float4 view_rect_min{};
	Float4 view_size_and_inv_size{};
	Float4 buffer_size_and_inv_size{};
	Float4 temporal_aa_params{};  // (Index, Count, JitterPixelsX, JitterPixelsY)
	float pre_exposure = 1.0f;
	float one_over_pre_exposure = 1.0f;
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

// TemporalAAParams.zw IS TemporalJitterPixels, already in render-resolution pixels. NGX takes
// it with NO sign flip. (docs/RESEARCH.md §3.3)
Float2 ngx_jitter_offset(const ViewParams &p);

// UE4's bCameraCut is `!InputHistory.IsValid() || View.bCameraCut`, and the history-invalid
// half never reaches the View buffer — hence the third argument. (CLAUDE.md §2.8)
bool is_camera_cut(const ViewParams &p, bool history_or_velocity_is_1x1);

} // namespace stray_dlss::ue4
