#include "envbrdf.hpp"

#include <cmath>

namespace stray_dlss::gbr {
namespace {

float saturate(float v)
{
	return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

float dot3(const Float3 &a, const Float3 &b)
{
	return a.x * b.x + a.y * b.y + a.z * b.z;
}

} // namespace

Float3 env_brdf_approx2(const Float3 &f0, float alpha, float nov)
{
	// Verbatim scalarisation of the Streamline §4.2.1 listing (header comment carries the
	// provenance). HLSL `mul(M, v)` is rows-dot-vector; the coefficient ORDER in the
	// float2x2/float3x3 constructors below is row-major, exactly as listed. The .xyw /
	// .xzw swizzle asymmetry between the bias and scale denominators is in the original.
	nov = std::fabs(nov);

	const float x1 = nov, x2 = nov * nov, x3 = nov * x2; // X = (1, NoV, NoV², NoV³)
	const float y1 = alpha, y2 = alpha * alpha, y3 = alpha * y2;

	// float2x2 M1 = float2x2(0.99044f, -1.28514f, 1.29678f, -0.755907f);
	const float bias_num =
		(0.99044f * 1.0f + -1.28514f * x1) * 1.0f +
		(1.29678f * 1.0f + -0.755907f * x1) * y1;
	// float3x3 M2 = float3x3(1, 2.92338, 59.4188, 20.3225, -27.0302, 222.592,
	//                        121.563, 626.13, 316.627);  over X.xyw, Y.xyw
	const float bias_den =
		(1.0f * 1.0f + 2.92338f * x1 + 59.4188f * x3) * 1.0f +
		(20.3225f * 1.0f + -27.0302f * x1 + 222.592f * x3) * y1 +
		(121.563f * 1.0f + 626.13f * x1 + 316.627f * x3) * y3;
	// float2x2 M3 = float2x2(0.0365463f, 3.32707f, 9.0632f, -9.04756f);
	const float scale_num =
		(0.0365463f * 1.0f + 3.32707f * x1) * 1.0f +
		(9.0632f * 1.0f + -9.04756f * x1) * y1;
	// float3x3 M4 = float3x3(1, 3.59685, -1.36772, 9.04401, -16.3174, 9.22949,
	//                        5.56589, 19.7886, -20.2123);  over X.xzw (!), Y.xyw
	const float scale_den =
		(1.0f * 1.0f + 3.59685f * x2 + -1.36772f * x3) * 1.0f +
		(9.04401f * 1.0f + -16.3174f * x2 + 9.22949f * x3) * y1 +
		(5.56589f * 1.0f + 19.7886f * x2 + -20.2123f * x3) * y3;

	float bias = bias_num / bias_den;
	const float scale = scale_num / scale_den;

	// "This is a hack for specular reflectance of 0" — the listing's own comment.
	bias *= saturate(f0.y * 50.0f);

	// mad(SpecularColor, max(0, scale), max(0, bias)) — the clamps are part of the
	// listing and load-bearing: at alpha=0, NoV=1 the raw bias is negative.
	const float s = scale > 0.0f ? scale : 0.0f;
	const float b = bias > 0.0f ? bias : 0.0f;
	return Float3{ f0.x * s + b, f0.y * s + b, f0.z * s + b };
}

float nov_remap(float nov)
{
	// GBufferResolve.usf: float x = NoV * NoV; NoV = x / (x + (1 - NoV) * (1 - NoV));
	const float x = nov * nov;
	const float d = x + (1.0f - nov) * (1.0f - nov);
	return d > 0.0f ? x / d : 0.0f; // d == 0 only at the never-produced NoV == 0 exactly
}

Float3 compute_f0(float specular, const Float3 &base_colour, float metallic)
{
	// ShadingCommon.ush:77-97: lerp(DielectricSpecularToF0(Specular).xxx, BaseColor,
	// Metallic), with DielectricSpecularToF0(s) = 0.08 * s.
	const float dielectric = 0.08f * specular;
	return Float3{
		dielectric + (base_colour.x - dielectric) * metallic,
		dielectric + (base_colour.y - dielectric) * metallic,
		dielectric + (base_colour.z - dielectric) * metallic,
	};
}

Float3 diffuse_colour(const Float3 &base_colour, float metallic)
{
	// DeferredShadingCommon.ush:608: BaseColor - BaseColor * Metallic.
	const float k = 1.0f - metallic;
	return Float3{ base_colour.x * k, base_colour.y * k, base_colour.z * k };
}

Float3 decode_gbuffer_a_normal(const Float3 &encoded)
{
	const Float3 n{ encoded.x * 2.0f - 1.0f, encoded.y * 2.0f - 1.0f,
		encoded.z * 2.0f - 1.0f };
	const float len2 = dot3(n, n);
	if (len2 < 1e-8f)
		return Float3{ 0.0f, 0.0f, 1.0f }; // degenerate encode: never NaN, pick +Z
	const float inv = 1.0f / std::sqrt(len2);
	return Float3{ n.x * inv, n.y * inv, n.z * inv };
}

std::uint32_t decode_shading_model_id(float gbuffer_b_alpha)
{
	// Encode is (ShadingModelID | SelectiveOutputMask) / 255 with the id in the LOW
	// nibble (SHADINGMODELID_MASK = 0xF) and the mask above it. (DeferredShadingCommon.ush
	// :419-480 region, RESEARCH-RR-GBUFFER.md §6.)
	const float a = saturate(gbuffer_b_alpha); // UNORM range; a stray value must not UB the cast
	const std::uint32_t packed = static_cast<std::uint32_t>(a * 255.0f + 0.5f);
	return packed & 0xFu;
}

float srgb_to_linear(float c)
{
	// IEC 61966-2-1: linear below 0.04045/12.92, gamma-2.4 curve above.
	if (c <= 0.04045f)
		return c / 12.92f;
	return std::pow((c + 0.055f) / 1.055f, 2.4f);
}

float nov_from_screen(float screen_x, float screen_y, float proj00, float proj11,
                      const Float3 &n_view)
{
	// The view-space ray through the pixel: clip.xy = view.xy * (P00, P11), w = view.z,
	// so screen = view.xy * (P00, P11) / view.z and every point on the pixel's ray has
	// direction (screen.x/P00, screen.y/P11, 1) — depth-independent. V is surface→camera,
	// the ray's negation. (Centred projection assumed; header comment.)
	const Float3 ray{ screen_x / proj00, screen_y / proj11, 1.0f };
	const float ray_len2 = dot3(ray, ray);

	const float n_len2 = dot3(n_view, n_view);
	if (n_len2 < 1e-8f || ray_len2 < 1e-8f)
		return 0.0f;

	const float inv = 1.0f / (std::sqrt(ray_len2) * std::sqrt(n_len2));
	return saturate(-dot3(ray, n_view) * inv);
}

} // namespace stray_dlss::gbr
