#include "view_params.hpp"

#include <cmath>
#include <cstring>

namespace stray_dlss::ue4 {
namespace {

const unsigned char *row_ptr(const void *data, std::uint32_t row)
{
	return static_cast<const unsigned char *>(data) + row_to_byte(row);
}

Float4 read_float4(const void *data, std::uint32_t row)
{
	float v[4] = {};
	std::memcpy(v, row_ptr(data, row), sizeof(v));
	return Float4{ v[0], v[1], v[2], v[3] };
}

float read_scalar(const void *data, std::uint32_t row, std::uint32_t component)
{
	float v = 0.0f;
	std::memcpy(&v, row_ptr(data, row) + std::size_t{ component } * 4, sizeof(v));
	return v;
}

} // namespace

bool parse_view_params(const void *data, std::size_t size, ViewParams &out)
{
	if (data == nullptr || size < kViewPrefixBytes)
		return false;

	for (std::uint32_t r = 0; r < 4; ++r)
	{
		const Float4 row = read_float4(data, ViewRow::kClipToPrevClip + r);
		out.clip_to_prev_clip.m[r * 4 + 0] = row.x;
		out.clip_to_prev_clip.m[r * 4 + 1] = row.y;
		out.clip_to_prev_clip.m[r * 4 + 2] = row.z;
		out.clip_to_prev_clip.m[r * 4 + 3] = row.w;
	}

	const auto read_matrix = [&](std::uint32_t base_row, Matrix4 &m) {
		for (std::uint32_t r = 0; r < 4; ++r)
		{
			const Float4 row = read_float4(data, base_row + r);
			m.m[r * 4 + 0] = row.x;
			m.m[r * 4 + 1] = row.y;
			m.m[r * 4 + 2] = row.z;
			m.m[r * 4 + 3] = row.w;
		}
	};
	for (std::uint32_t i = 0; i < 7; ++i)
		read_matrix(ViewRow::kViewMatrixBlock + i * 4, out.view_matrix_block[i]);
	// TranslatedWorldToView is block index 3 (rows 12-15, mirror-verified — ue4_view.hpp).
	out.translated_world_to_view = out.view_matrix_block[3];
	read_matrix(ViewRow::kViewToClip, out.view_to_clip);
	read_matrix(ViewRow::kViewToClipNoAA, out.view_to_clip_no_aa);

	out.temporal_aa_jitter = read_float4(data, ViewRow::kTemporalAAJitter);
	out.view_rect_min = read_float4(data, ViewRow::kViewRectMin);
	out.view_size_and_inv_size = read_float4(data, ViewRow::kViewSizeAndInvSize);
	out.buffer_size_and_inv_size = read_float4(data, ViewRow::kBufferSizeAndInvSize);
	out.temporal_aa_params = read_float4(data, ViewRow::kTemporalAAParams);

	out.pre_exposure = read_scalar(data, ViewRow::kPreExposureRow, kPreExposureComponent);
	out.one_over_pre_exposure = read_scalar(data, ViewRow::kPreExposureRow, kOneOverPreExposureComponent);
	out.near_plane = read_scalar(data, ViewRow::kNearPlaneRow, kNearPlaneComponent);
	out.delta_time = read_scalar(data, ViewRow::kDeltaTimeRow, kDeltaTimeComponent);
	out.camera_cut = read_scalar(data, ViewRow::kCameraCutRow, kCameraCutComponent);

	return true;
}

bool view_params_plausible(const ViewParams &p)
{
	const float w = p.view_size_and_inv_size.x;
	const float h = p.view_size_and_inv_size.y;

	// A real render target, not the (1,1,1,1) decoy at row 131 and not zeros.
	if (!(w >= 32.0f && w <= 16384.0f) || !(h >= 32.0f && h <= 16384.0f))
		return false;

	// .zw must be the reciprocals of .xy, which the decoy row does not satisfy.
	if (std::fabs(p.view_size_and_inv_size.z * w - 1.0f) > 0.01f)
		return false;
	if (std::fabs(p.view_size_and_inv_size.w * h - 1.0f) > 0.01f)
		return false;

	// Jitter is sub-pixel by construction.
	if (std::fabs(p.temporal_aa_params.z) > 0.5f || std::fabs(p.temporal_aa_params.w) > 0.5f)
		return false;

	// The jitter sequence length is small and positive.
	if (!(p.temporal_aa_params.y >= 1.0f && p.temporal_aa_params.y <= 512.0f))
		return false;

	return true;
}

bool pre_exposure_plausible(const ViewParams &p)
{
	// DELIBERATELY NOT part of view_params_plausible, which gates the ENTIRE DLSS path: rows
	// 135.y/135.z are [derived], and if they are ever not where we believe, folding this into
	// that gate would reject every frame and silently disable DLSS altogether — a far worse
	// failure than the one it prevents. Checked at the point of USE instead, where the fallback
	// is a static scale rather than no upscaling at all.
	//
	// Worth checking because the value is load-bearing: the NR codec derives its whole proxy
	// scale from OneOverPreExposure, and nvsdk_ngx_helpers.h:507 rewrites a zero InPreExposure
	// to 1.0 without a word. The two rows are reciprocals by construction, which makes the pair
	// self-checking — a pair that does not multiply to 1 is not the pre-exposure pair.
	if (!(p.pre_exposure > 0.0f) || !(p.one_over_pre_exposure > 0.0f))
		return false;
	return std::fabs(p.pre_exposure * p.one_over_pre_exposure - 1.0f) <= 1e-3f;
}

Float2 ngx_jitter_offset(const ViewParams &p)
{
	return Float2{ p.temporal_aa_params.z, p.temporal_aa_params.w };
}

bool world_to_view_rotation_plausible(const Matrix4 &m)
{
	// Upper 3x3, row-major (m[r*4+c]): each row unit length, rows mutually orthogonal.
	constexpr float kTol = 1e-2f;
	for (int r = 0; r < 3; ++r)
	{
		const float len2 = m.m[r * 4 + 0] * m.m[r * 4 + 0] +
			m.m[r * 4 + 1] * m.m[r * 4 + 1] + m.m[r * 4 + 2] * m.m[r * 4 + 2];
		if (std::fabs(len2 - 1.0f) > kTol)
			return false;
	}
	for (int a = 0; a < 3; ++a)
		for (int b = a + 1; b < 3; ++b)
		{
			const float dot = m.m[a * 4 + 0] * m.m[b * 4 + 0] +
				m.m[a * 4 + 1] * m.m[b * 4 + 1] + m.m[a * 4 + 2] * m.m[b * 4 + 2];
			if (std::fabs(dot) > kTol)
				return false;
		}
	return true;
}

void nov_rotation_rows(const Matrix4 &m, float out[3][3])
{
	// Transposed upper 3x3: out[i][j] = M[j][i]. See the header for why (row-vector
	// storage vs the shader's dot(row, n) form).
	for (int i = 0; i < 3; ++i)
		for (int j = 0; j < 3; ++j)
			out[i][j] = m.m[j * 4 + i];
}

const char *view_matrix_block_name(int index)
{
	switch (index)
	{
	case 0: return "TranslatedWorldToClip";
	case 1: return "WorldToClip";
	case 2: return "ClipToWorld";
	case 3: return "TranslatedWorldToView";
	case 4: return "ViewToTranslatedWorld";
	case 5: return "TranslatedWorldToCameraView";
	case 6: return "CameraViewToTranslatedWorld";
	}
	return "?";
}

bool is_camera_cut(const ViewParams &p, bool history_or_velocity_is_1x1)
{
	if (p.camera_cut != 0.0f)
		return true;

	if (history_or_velocity_is_1x1)
		return true;

	return jitter_indicates_camera_cut(
		Float2{ p.temporal_aa_jitter.x, p.temporal_aa_jitter.y },
		Float2{ p.temporal_aa_jitter.z, p.temporal_aa_jitter.w });
}

} // namespace stray_dlss::ue4
