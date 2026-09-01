#include "core/nr_codec.hpp"

#include <cmath>

namespace stray_dlss::nrc {
namespace {

// HLSL's own definitions, reproduced so this file and shaders/nr_codec.hlsli execute the same
// arithmetic. In particular `lerp` is `a + t*(b-a)` — NOT std::lerp, whose different form
// would break the exact-identity property this codec is built around.
float saturate1(float v)
{
	if (!(v > 0.0f)) // NaN-safe: a NaN takes this branch and becomes 0
		return 0.0f;
	return v < 1.0f ? v : 1.0f;
}

float lerp1(float a, float b, float t) { return a + t * (b - a); }

Float3 lerp3(Float3 a, Float3 b, float t)
{
	return Float3{ lerp1(a.x, b.x, t), lerp1(a.y, b.y, t), lerp1(a.z, b.z, t) };
}

Float3 scale3(Float3 a, float s) { return Float3{ a.x * s, a.y * s, a.z * s }; }

Float3 sub3(Float3 a, Float3 b) { return Float3{ a.x - b.x, a.y - b.y, a.z - b.z }; }

Float3 max3(Float3 a, float v)
{
	return Float3{ a.x > v ? a.x : v, a.y > v ? a.y : v, a.z > v ? a.z : v };
}

} // namespace

float srgb_encode_channel(float c)
{
	c = saturate1(c);
	if (c <= 0.0031308f)
		return c * 12.92f;
	// c is already saturated, so the max() only guards pow(0, 1/2.4) on implementations that
	// are unhappy with an exact zero base. Same guard as the reference shader.
	const float base = c > 0.00000001f ? c : 0.00000001f;
	return 1.055f * std::pow(base, 1.0f / 2.4f) - 0.055f;
}

Float3 srgb_encode(Float3 c)
{
	return Float3{ srgb_encode_channel(c.x), srgb_encode_channel(c.y),
		srgb_encode_channel(c.z) };
}

float srgb_decode_channel(float c)
{
	c = saturate1(c);
	if (c <= 0.04045f)
		return c / 12.92f;
	const float shoulder = (c + 0.055f) / 1.055f;
	return std::pow(shoulder > 0.0f ? shoulder : 0.0f, 2.4f);
}

Float3 srgb_decode(Float3 c)
{
	return Float3{ srgb_decode_channel(c.x), srgb_decode_channel(c.y),
		srgb_decode_channel(c.z) };
}

float soft_clip_channel(float v)
{
	if (v <= kSoftClipKnee)
		return v;
	const float headroom = 1.0f - kSoftClipKnee;
	return kSoftClipKnee +
		headroom * (1.0f - std::exp(-kSoftClipShoulder * (v - kSoftClipKnee)));
}

Float3 soft_clip(Float3 v)
{
	return Float3{ soft_clip_channel(v.x), soft_clip_channel(v.y), soft_clip_channel(v.z) };
}

float bt709_luminance(Float3 c)
{
	return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z;
}

bool is_finite3(Float3 c)
{
	return std::isfinite(c.x) && std::isfinite(c.y) && std::isfinite(c.z);
}

float proxy_scale(float paper_white, float fallback_paper_white)
{
	// A non-finite knob value falls back rather than poisoning the frame — the reference's
	// `neuralRenderingProxyScale` does the same with its static scale.
	float pw = std::isfinite(paper_white) ? paper_white : fallback_paper_white;
	if (!std::isfinite(pw))
		pw = 1.0f;
	// `calcProxyScale`: staticExposure / std::max(paperWhiteScale(), 0.01f), staticExposure 1,
	// with the reference RTX_OPTION's own [0.01, 64] bounds on the knob.
	if (pw < kMinPaperWhite)
		pw = kMinPaperWhite;
	if (pw > kMaxPaperWhite)
		pw = kMaxPaperWhite;
	float scale = 1.0f / pw;
	if (!std::isfinite(scale))
		scale = 1.0f;
	if (scale < kScaleMin)
		scale = kScaleMin;
	if (scale > kScaleMax)
		scale = kScaleMax;
	return scale;
}

float proxy_scale_tracked(float paper_white, float fallback_paper_white, float exposure_factor)
{
	const float static_scale = proxy_scale(paper_white, fallback_paper_white);
	// A frame whose View constant buffer could not be read hands us nothing usable here. Falling
	// back to the static scale keeps the codec in a known operating point; multiplying by a
	// garbage value would put the network somewhere arbitrary with no diagnostic.
	if (!std::isfinite(exposure_factor) || exposure_factor <= 0.0f)
		return static_scale;

	float scale = static_scale * exposure_factor;
	if (!std::isfinite(scale))
		return static_scale;
	if (scale < kScaleMin)
		scale = kScaleMin;
	if (scale > kScaleMax)
		scale = kScaleMax;
	return scale;
}

Float3 encode(Float3 source, float scale)
{
	// A pixel the engine already broke must not reach the network, and must not reach the
	// decode's subtraction either, so it is flushed to black. `decode` leaves the
	// corresponding original completely alone, so the two halves agree.
	const Float3 scene_linear = is_finite3(source) ? max3(source, 0.0f) : Float3{};
	return srgb_encode(soft_clip(scale3(scene_linear, scale)));
}

bool decode(Float3 source, Float3 proxy_texel, Float3 neural_texel, float scale,
            float color_strength, float transfer_strength, Float3 &out)
{
	if (!is_finite3(source))
		return false;

	// The max() is what makes the identity argument hold only for a non-negative original.
	// Scene colour is a sum of radiances and non-negative by construction, so in practice this
	// is the identity; a negative channel is clamped to zero, which is the right thing to do
	// to it anyway.
	const Float3 original = max3(source, 0.0f);

	const Float3 proxy = srgb_decode(proxy_texel);
	const Float3 neural = srgb_decode(neural_texel);

	// (*) — the scene-linear change the network asked for. Exactly zero when it asked for none.
	// Written as a division, not a multiply by a precomputed reciprocal, because that is what
	// the reference shader does and this file's only job is to be its twin.
	const Float3 delta_display = sub3(neural, proxy);
	const Float3 neural_delta = Float3{ delta_display.x / scale, delta_display.y / scale,
		delta_display.z / scale };
	if (!is_finite3(neural_delta))
		return false;

	const Float3 transferred = max3(
		Float3{ original.x + neural_delta.x, original.y + neural_delta.y,
			original.z + neural_delta.z }, 0.0f);

	// Chroma safety valve: reproduce the transferred luminance on the ORIGINAL's chromaticity.
	//
	// This is the one place that divides by something which can go to zero. What comes out of
	// `original * (transferredLuminance / originalLuminance)` always has luminance exactly
	// transferredLuminance, so the RESULT is bounded no matter how small the denominator gets —
	// but its individual CHANNELS are not. Reaching a given luminance on a fully saturated blue
	// needs 1/0.0722 = 13.8x that luminance in the blue channel (4.7x red, 1.4x green), so a
	// near-black pixel whose hue is noise comes back as a saturated speckle where the network
	// asked for near-neutral dark grey. Hence the fade rather than a hard
	// `originalLuminance > 0` branch, which was also a discontinuity: a pixel at exactly zero
	// took the network's colour while one a single denormal above took a saturated primary.
	//
	// Property 1 still holds EXACTLY. At neural_delta == 0, transferred == original and the two
	// luminances are equal, so luminance_ratio is exactly 1.0, original * 1.0 == original, and
	// lerp(original, original, w) == original for every w — including inside the faded region,
	// where both ends of the lerp are `original`.
	const float original_luminance = bt709_luminance(original);
	const float transferred_luminance = bt709_luminance(transferred);

	// `scale` is clamped to [1e-6, 1e6], so chroma_floor lands in [1e-9, 1e3] and is never zero.
	const float chroma_floor = kMinChromaLuminance / scale;
	const float chroma_weight = saturate1(original_luminance / chroma_floor);
	// The guard is still needed even though chroma_weight is zero when it trips: 0/0 is a NaN
	// and the lerp would propagate it through the zero weight.
	const float luminance_ratio = original_luminance > 0.0f
		? transferred_luminance / original_luminance
		: 1.0f;
	const Float3 luminance_only =
		lerp3(transferred, scale3(original, luminance_ratio), chroma_weight);

	const Float3 graded = lerp3(luminance_only, transferred, color_strength);
	const Float3 result = lerp3(original, graded, transfer_strength);

	// Writing a value R16G16B16A16_FLOAT cannot hold turns the pixel into an infinity.
	out.x = result.x < 0.0f ? 0.0f : (result.x > kMaxHalf ? kMaxHalf : result.x);
	out.y = result.y < 0.0f ? 0.0f : (result.y > kMaxHalf ? kMaxHalf : result.y);
	out.z = result.z < 0.0f ? 0.0f : (result.z > kMaxHalf ? kMaxHalf : result.z);
	return true;
}

} // namespace stray_dlss::nrc
