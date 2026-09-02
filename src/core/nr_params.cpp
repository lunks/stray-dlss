#include "core/nr_params.hpp"

#include <limits>

namespace stray_dlss::nrparam {
namespace {

// D3D12 reports extents as uint32; the runtime reads them as int32. A value above INT32_MAX
// cannot be a real rect, and the one thing it must never become is a NEGATIVE number the runtime
// would take at face value — so it is clamped, not cast.
int to_signed_extent(std::uint32_t value)
{
	constexpr std::uint32_t kMax = static_cast<std::uint32_t>(std::numeric_limits<int>::max());
	return static_cast<int>(value > kMax ? kMax : value);
}

Entry signed_entry(const char *name, std::uint32_t value)
{
	Entry e;
	e.name = name;
	e.type = Type::i32;
	e.i = to_signed_extent(value);
	return e;
}

// One rect: base at the origin (UE4 shifts its view rects to the buffer top-left, and we never
// hand the runtime a sub-rectangle of a larger allocation) plus its extent.
int emit_rect(Entry *out, const char *base_x, const char *base_y, const char *width,
              const char *height, std::uint32_t w, std::uint32_t h)
{
	out[0] = signed_entry(base_x, 0);
	out[1] = signed_entry(base_y, 0);
	out[2] = signed_entry(width, w);
	out[3] = signed_entry(height, h);
	return 4;
}

} // namespace

int build_rects(const Rects &rects, Entry *out, int cap)
{
	if (out == nullptr || cap < kMaxRectEntries)
		return 0;

	int n = 0;
	n += emit_rect(out + n, "DLSSNR.ColorSubrectBaseX", "DLSSNR.ColorSubrectBaseY",
		"DLSSNR.ColorSubrectWidth", "DLSSNR.ColorSubrectHeight",
		rects.color_width, rects.color_height);
	n += emit_rect(out + n, "DLSSNR.DepthSubrectBaseX", "DLSSNR.DepthSubrectBaseY",
		"DLSSNR.DepthSubrectWidth", "DLSSNR.DepthSubrectHeight",
		rects.guide_width, rects.guide_height);
	n += emit_rect(out + n, "DLSSNR.MVecSubrectBaseX", "DLSSNR.MVecSubrectBaseY",
		"DLSSNR.MVecSubrectWidth", "DLSSNR.MVecSubrectHeight",
		rects.guide_width, rects.guide_height);
	n += emit_rect(out + n, "DLSSNR.OutputSubrectBaseX", "DLSSNR.OutputSubrectBaseY",
		"DLSSNR.OutputSubrectWidth", "DLSSNR.OutputSubrectHeight",
		rects.output_width, rects.output_height);
	return n;
}

int build_create(std::uint32_t in_width, std::uint32_t in_height, float scaling_ratio, Entry *out,
                 int cap)
{
	if (out == nullptr || cap < kMaxCreateEntries)
		return 0;

	out[0] = signed_entry("DLSSNR.Width", in_width);
	out[1] = signed_entry("DLSSNR.Height", in_height);
	// "DLSSNR.ScalingRatio", NOT "DLSSNR.Scale" — the latter does not exist in the 310.8.0
	// runtime, so every value we ever wrote to it was silently discarded. This one is a real key
	// and is a float; it is measured INERT (read, then unconditionally overwritten with 1.0f at
	// 0x18001a96a), but a real key written with its real type is not the same thing as a name the
	// runtime has never heard of.
	out[2].name = "DLSSNR.ScalingRatio";
	out[2].type = Type::f32;
	out[2].f = scaling_ratio;
	return 3;
}

} // namespace stray_dlss::nrparam
