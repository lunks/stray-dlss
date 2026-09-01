// DLSS-NR HDR colour codec, stage 1 of 2: build the display-referred proxy image the network
// actually gets to see.
//
//     proxy = SrgbEncode(SoftClip(max(color, 0) * scale))
//
// Ported from neural_rendering_encode.comp.slang @ aa90a180 in
// github.com/lunks/dxvk-remix-plus-dlssnr — see src/core/nr_codec.hpp for full provenance and
// tests/test_nr_codec.cpp for the executable proof of the math.
//
// The engine's colour image is never written here: nothing in this shader touches it, and the
// decode restores its alpha from it verbatim.
//
// The colour input is bound as a UAV, not an SRV, deliberately: it arrives from the SR/RR
// evaluate in D3D12_RESOURCE_STATE_UNORDERED_ACCESS and reading it through a UAV avoids a
// transition pair we would otherwise have to record and undo around this dispatch. The
// reference shader binds it the same way.

#include "nr_codec.hlsli"

RWTexture2D<float4> InColor  : register(u0); // the engine image, linear HDR, read only here
RWTexture2D<float4> OutProxy : register(u1); // our proxy, R16G16B16A16_FLOAT

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
	if (tid.x >= NrImageSize.x || tid.y >= NrImageSize.y)
		return;

	const float4 source = InColor[tid.xy];

	// A NaN/Inf pixel must not reach the network, and must not reach the decode's subtraction
	// either, so it is flushed to black here. The decode leaves the corresponding original
	// pixel completely alone, so the two halves agree about what a broken pixel means.
	const float3 scene_linear =
		NrIsFinite3(source.rgb) ? max(source.rgb, 0.0f) : float3(0.0f, 0.0f, 0.0f);

	const float3 display_referred = scene_linear * NrScale;
	const float3 proxy = NrSrgbEncode(NrSoftClip(display_referred));

	// The proxy's own alpha is never read back — the decode takes the alpha it writes from the
	// untouched engine image — and DLSSNR is an RGB network, so a constant is both sufficient
	// and safer here than forwarding the source alpha, which could carry a NaN into the
	// snippet's input texture.
	OutProxy[tid.xy] = float4(proxy, 1.0f);
}
