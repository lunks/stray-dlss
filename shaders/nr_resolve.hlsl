// NR model resolution, pass 2: the MATCHED RESIDUAL. Only the model's difference comes up from
// the small extent; the full-resolution frame supplies every pixel of its own detail.
//
// Composing the model's small picture against the full frame would read the downsample's blur
// as headroom the frame has and the model never saw - a term that grows as the model shrinks,
// which is the resolution-dependent colour shift OptiScaler measured at 50%. Comparing the two
// small pictures (what the model was shown, what it answered) puts both at the same scale, so
// the only thing carried up is the edit itself. The frame here is the present stage's staging
// copy of the back buffer: display-encoded already, so it IS its own proxy and no encode is
// rebuilt - OptiScaler's "passthrough" branch, `fullProxy = saturate(original)`.
//
// The residual is CUBE-SCALED (hhkbble): the target P + edit is pulled back toward P along its
// own direction until every channel is inside [0, 1]. A per-channel clamp would rotate the hue
// of exactly the saturated pixels most likely to breach; one scalar on the whole triple cannot.
// A frame the model did not change comes back bit-exact: edit = 0, alpha irrelevant, out = P.

cbuffer Params : register(b0)
{
	uint2 gDst;         // full-frame extent
	uint2 gSmall;       // model extent
	float gTransfer;    // NgxNRModelTransfer: 0 = the original, 1 = the full edit
	float3 _pad;
};

Texture2D<float4>   SmallIn    : register(t0); // what the model was shown
Texture2D<float4>   SmallModel : register(t1); // what it answered
SamplerState        LinearClamp : register(s0);
RWTexture2D<float4> Frame      : register(u0); // the full frame, in place

float3 CubeScaleResidual(float3 P, float3 T)
{
	const float3 d = T - P;
	float alpha = 1.0f;
	// For each channel, how far along d before leaving [0,1]. d == 0 imposes nothing.
	[unroll]
	for (int i = 0; i < 3; ++i)
	{
		if (d[i] > 1e-6f)
			alpha = min(alpha, (1.0f - P[i]) / d[i]);
		else if (d[i] < -1e-6f)
			alpha = min(alpha, (0.0f - P[i]) / d[i]);
	}
	alpha = saturate(alpha);
	return P + alpha * d;
}

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
	if (tid.x >= gDst.x || tid.y >= gDst.y)
		return;

	// Sample both small pictures at the same place; their DIFFERENCE is what is interpolated,
	// so the interpolation blur lands on the edit, never on the frame.
	const float2 uv = (float2(tid.xy) + 0.5f) / float2(gDst);
	const float3 shown  = SmallIn.SampleLevel(LinearClamp, uv, 0).rgb;
	const float3 answer = SmallModel.SampleLevel(LinearClamp, uv, 0).rgb;
	const float3 edit = (answer - shown) * gTransfer;

	const float4 original = Frame[tid.xy];
	const float3 P = saturate(original.rgb);
	const float3 result = CubeScaleResidual(P, P + edit);
	Frame[tid.xy] = float4(result, original.a);
}
