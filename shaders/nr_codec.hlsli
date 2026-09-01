// Shared halves of the DLSS-NR HDR colour codec. The encode and the decode MUST agree on the
// transfer functions, so they live here rather than being written out twice.
//
// EVERY function here has a scalar reference twin in src/core/nr_codec.cpp, pinned by
// tests/test_nr_codec.cpp — change one and you MUST change the other. CI cannot execute this
// file (the developer has no game, no Windows box and no GPU, CLAUDE.md §0.1), so the twin is
// the only executable proof the math is right. Provenance for every constant is in
// src/core/nr_codec.hpp's header; the short form: ported from the 4090-tested deployment at
// github.com/lunks/dxvk-remix-plus-dlssnr commit aa90a180
// (rtx/pass/neural_rendering/neural_rendering_codec.slangh and its encode/decode pair).

#ifndef STRAY_DLSS_NR_CODEC_HLSLI
#define STRAY_DLSS_NR_CODEC_HLSLI

// Soft-clip shoulder, verbatim from the RenoDX DLSS5 encoder. Continuous in value at the knee
// and asymptotic to exactly 1.0, so no amount of input radiance can push the proxy out of the
// [0,1] domain sRGB encoding expects.
//
// 0.25 * 5.770780 = 1.442695 = 1/ln(2), so the slope jumps from 1.0 to ~1.44 at the knee: this
// curve is C0 but NOT C1. That is what the known working deployment ships, so it is reproduced
// exactly rather than "corrected". Do not smooth it.
static const float kNrSoftClipKnee = 0.75f;
static const float kNrSoftClipShoulder = 5.770780f;

// Below this DISPLAY-REFERRED luminance the original's chromaticity stops carrying usable
// information and the chromaticity-preserving path is faded out (see nr_decode.hlsl).
static const float kNrMinChromaLuminance = 0.001f;

// Our colour target is R16G16B16A16_FLOAT. Writing a value the format cannot hold turns the
// pixel into an infinity, which then poisons every pass downstream of it.
static const float kNrMaxHalf = 65504.0f;

// EXACT piecewise sRGB, both directions (IEC 61966-2-1). Deliberately NOT an x^2.2
// approximation: the network was trained on true sRGB imagery and the working deployment feeds
// it the exact piecewise curve.
//
// Written with per-channel scalar ternaries rather than a vector select so the source stays
// valid whichever vector-conditional rules the compiler applies.
float3 NrSrgbEncode(float3 color)
{
	color = saturate(color);

	const float3 toe = color * 12.92f;
	// color is already saturated, so the max() only guards pow(0, 1/2.4) on hardware that is
	// unhappy with an exact zero base. abs() additionally silences fxc's X3571 (pow of a
	// possibly-negative base), which /WX would turn into a build break; the value is
	// non-negative by construction.
	const float3 shoulder = 1.055f * pow(abs(max(color, 0.00000001f)), 1.0f / 2.4f) - 0.055f;

	return float3(
		color.r <= 0.0031308f ? toe.r : shoulder.r,
		color.g <= 0.0031308f ? toe.g : shoulder.g,
		color.b <= 0.0031308f ? toe.b : shoulder.b);
}

float3 NrSrgbDecode(float3 color)
{
	color = saturate(color);

	const float3 toe = color / 12.92f;
	const float3 shoulder = pow(abs(max((color + 0.055f) / 1.055f, 0.0f)), 2.4f);

	return float3(
		color.r <= 0.04045f ? toe.r : shoulder.r,
		color.g <= 0.04045f ? toe.g : shoulder.g,
		color.b <= 0.04045f ? toe.b : shoulder.b);
}

float NrSoftClipChannel(float value)
{
	if (value <= kNrSoftClipKnee)
		return value;

	const float headroom = 1.0f - kNrSoftClipKnee;
	return kNrSoftClipKnee +
		headroom * (1.0f - exp(-kNrSoftClipShoulder * (value - kNrSoftClipKnee)));
}

float3 NrSoftClip(float3 color)
{
	return float3(NrSoftClipChannel(color.r), NrSoftClipChannel(color.g),
		NrSoftClipChannel(color.b));
}

// Standard Rec. 709 weights (calcBt709Luminance in the reference's rtx/utility/color.slangh).
float NrBt709Luminance(float3 linear_color)
{
	return dot(linear_color, float3(0.2126f, 0.7152f, 0.0722f));
}

bool NrIsFinite3(float3 v)
{
	return !any(isnan(v)) && !any(isinf(v));
}

// The constant buffer both halves share. The C++ mirror is `Params` in src/nr_codec_pass.cpp;
// a field-order mismatch produces a plausible-looking but wrong image rather than an error, so
// keep the two in lockstep by eye every time either changes. Layout, checked by hand against
// HLSL's packing rules and the C++ struct: uint2 at 0..7, NrScale 8..11, NrColorStrength 12..15,
// NrTransferStrength 16..19, the three pads 20..31 — 32 bytes, which the C++ side static_asserts
// is a multiple of 16.
//
// The two fields the ENCODE reads (NrImageSize, NrScale) are deliberately first, so the two
// shaders agree on their offsets regardless of what the compiler does with the fields only the
// decode touches.
cbuffer NrCodecParams : register(b0)
{
	uint2  NrImageSize;       // the rect to process, in texels
	float  NrScale;           // scene-linear -> display-referred multiplier, = 1/paperWhite
	float  NrColorStrength;   // decode only; 1 = take the network's colour
	float  NrTransferStrength;// decode only; 0 = exact bypass
	float  NrPad0;
	float  NrPad1;
	float  NrPad2;
};

#endif // STRAY_DLSS_NR_CODEC_HLSLI
