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

Float2 ngx_jitter_offset(const ViewParams &p)
{
	return Float2{ p.temporal_aa_params.z, p.temporal_aa_params.w };
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
