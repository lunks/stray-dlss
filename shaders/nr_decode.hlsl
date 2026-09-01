// DLSS-NR HDR colour codec, stage 2 of 2: carry the network's answer back onto the untouched
// linear HDR original, in place, so nothing downstream has to know this pass exists.
//
//     result = original + (SrgbDecode(neural) - SrgbDecode(proxy)) / scale
//
// The full algebraic derivation — and the five properties that make this the right transfer
// rather than an obvious `SrgbDecode(neural) / scale` — lives in src/core/nr_codec.hpp above
// `decode`. Read it before changing anything here; the exact-identity property (neural ==
// proxy returns the original bit for bit) is what the whole design is for, and
// tests/test_nr_codec.cpp pins it.
//
// Ported from neural_rendering_decode.comp.slang @ aa90a180 in
// github.com/lunks/dxvk-remix-plus-dlssnr.
//
// This replaces what used to be a straight RGBA CopyResource of the neural target over the
// engine image. That copy both discarded the HDR range (the network's answer is display
// referred) and overwrote the alpha channel with the network's meaningless one.

#include "nr_codec.hlsli"

RWTexture2D<float4> InProxy    : register(u0); // what we fed the network
RWTexture2D<float4> InNeural   : register(u1); // what it answered
RWTexture2D<float4> InOutColor : register(u2); // the engine image, read and written in place

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
	if (tid.x >= NrImageSize.x || tid.y >= NrImageSize.y)
		return;

	const float4 source = InOutColor[tid.xy];

	// A pixel the engine already broke is left exactly as it was — the encode fed the network
	// black for it, so its neural answer means nothing here.
	if (!NrIsFinite3(source.rgb))
		return;

	// The max() is what makes the identity argument hold only for a non-negative original.
	// Scene colour is a sum of radiances and non-negative by construction, so in practice this
	// is the identity; a negative channel is clamped to zero, which is the right thing to do to
	// it anyway.
	const float3 original = max(source.rgb, 0.0f);

	const float3 proxy = NrSrgbDecode(InProxy[tid.xy].rgb);
	const float3 neural = NrSrgbDecode(InNeural[tid.xy].rgb);

	// The scene-linear change the network asked for. Exactly zero when it asked for none.
	const float3 neural_delta = (neural - proxy) / NrScale;

	// A non-finite delta can only come from a resource the snippet left in an unexpected state.
	// Never let one reach the engine image: a NaN there survives every downstream pass.
	if (!NrIsFinite3(neural_delta))
		return;

	const float3 transferred = max(original + neural_delta, 0.0f);

	// Chroma safety valve: reproduce the transferred luminance on the ORIGINAL's chromaticity.
	//
	// This is the one place in the pass that divides by something which can go to zero. What
	// comes out of `original * (transferredLuminance / originalLuminance)` always has luminance
	// exactly transferredLuminance, so the RESULT is bounded no matter how small the
	// denominator gets — but its individual CHANNELS are not. Reaching a given luminance on a
	// fully saturated blue needs 1/0.0722 = 13.8x that luminance in the blue channel (4.7x red,
	// 1.4x green), so a near-black pixel whose hue is noise comes back as a saturated speckle
	// where the network asked for near-neutral dark grey. Hence the fade rather than a hard
	// `originalLuminance > 0` branch, which was also a discontinuity: a pixel at exactly zero
	// took the network's colour while one a single denormal above took a saturated primary.
	//
	// The exact-identity property still holds. At neural_delta == 0 the two luminances are
	// equal, so luminance_ratio is exactly 1.0, original * 1.0 == original, and
	// lerp(original, original, w) == original for every w — including inside the faded region,
	// where both ends of the lerp are `original`.
	const float original_luminance = NrBt709Luminance(original);
	const float transferred_luminance = NrBt709Luminance(transferred);

	// NrScale is clamped to [1e-6, 1e6] on the CPU side, so chroma_floor lands in [1e-9, 1e3]
	// and can never be zero.
	const float chroma_floor = kNrMinChromaLuminance / NrScale;
	const float chroma_weight = saturate(original_luminance / chroma_floor);
	// The guard is still needed even though chroma_weight is zero when it trips: 0/0 is a NaN
	// and lerp() would propagate it through the zero weight.
	const float luminance_ratio = original_luminance > 0.0f
		? transferred_luminance / original_luminance
		: 1.0f;
	const float3 luminance_only = lerp(transferred, original * luminance_ratio, chroma_weight);

	const float3 graded = lerp(luminance_only, transferred, NrColorStrength);
	const float3 result = lerp(original, graded, NrTransferStrength);

	InOutColor[tid.xy] = float4(
		clamp(result, 0.0f, kNrMaxHalf),
		// DLSSNR is an RGB network and whatever it leaves in alpha is not meaningful. Carry the
		// ORIGINAL alpha through: on this title u0 becomes the next frame's TAA history
		// (CLAUDE.md §2.9), so a clobbered alpha would be fed straight back into the engine.
		source.a);
}
