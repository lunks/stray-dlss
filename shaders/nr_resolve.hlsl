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
// HOW THE EDIT COMES UP is [STRAYDLSS] NgxNRModelGuided:
//   0  bilinear: the delta is sampled with a plain bilinear tap. Distance only, so at an edge
//      the sign side's edit and the wall side's edit blend over a two-pixel band.
//   1  joint bilateral (Kopf et al. 2007): the four nearest small texels are weighted by
//      distance AND by how close their luminance (of what the model was SHOWN) is to this full-
//      res pixel's. A wall pixel takes the wall's edit. Its known limit (Chen et al. 2016,
//      fig. 8): it tends toward piecewise-constant output and cannot express a contrast change.
//   2  local affine (He et al. 2010 guided filter; Chen, Adams, Wadhwa, Hasinoff 2016
//      "Bilateral Guided Upsampling"): fit, from a 3x3 window of small texels, the line
//      answer ~= a * shown + b that best explains what the model did there, and apply it to the
//      full-res pixel: answer = a * P + b. The model's tone and contrast changes then act on the
//      frame's OWN fine detail instead of being smeared over it; BGU runs operators at 1/8
//      resolution this way with comparable output. Requires the operator to be roughly
//      scale-invariant, which an appearance edit is and a denoiser is not.
//
// The residual is CUBE-SCALED (hhkbble): the target is pulled back toward P along its own
// direction until every channel is inside [0, 1]. A per-channel clamp would rotate the hue of
// exactly the saturated pixels most likely to breach; one scalar on the whole triple cannot.
// A frame the model did not change comes back bit-exact in every mode: edit = 0, out = P.

cbuffer Params : register(b0)
{
	uint2 gDst;         // full-frame extent
	uint2 gSmall;       // model extent
	float gTransfer;    // NgxNRModelTransfer: 0 = the original, 1 = the full edit
	uint  gGuided;      // 0 bilinear, 1 joint bilateral, 2 local affine
	float gSigma;       // mode 1: luminance range sigma (display units); mode 2: epsilon
	float _pad;
};

Texture2D<float4>   SmallIn    : register(t0); // what the model was shown
Texture2D<float4>   SmallModel : register(t1); // what it answered
SamplerState        LinearClamp : register(s0);
RWTexture2D<float4> Frame      : register(u0); // the full frame, in place

static const float3 kLuma = float3(0.2126f, 0.7152f, 0.0722f);

float3 CubeScaleResidual(float3 P, float3 T)
{
	const float3 d = T - P;
	float alpha = 1.0f;
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

// Mode 0: the delta through the hardware bilinear tap.
float3 EditBilinear(float2 uv)
{
	return SmallModel.SampleLevel(LinearClamp, uv, 0).rgb - SmallIn.SampleLevel(LinearClamp, uv, 0).rgb;
}

// Mode 1: the same four texels, reweighted by luminance similarity to the full-res pixel.
// Falls back to the bilinear weights when no neighbour is similar (a denominator floor).
float3 EditBilateral(float2 pos_small, float lumaP)
{
	const float2 f = pos_small - 0.5f;
	const int2 base = int2(floor(f));
	const float2 t = f - float2(base);
	float3 sum = 0.0f;
	float wsum = 0.0f;
	const float inv2s2 = 1.0f / (2.0f * gSigma * gSigma + 1e-6f);
	[unroll]
	for (int j = 0; j < 2; ++j)
	{
		[unroll]
		for (int i = 0; i < 2; ++i)
		{
			const int2 c = clamp(base + int2(i, j), int2(0, 0), int2(gSmall) - 1);
			const float wb = (i == 0 ? 1.0f - t.x : t.x) * (j == 0 ? 1.0f - t.y : t.y);
			const float3 shown = SmallIn.Load(int3(c, 0)).rgb;
			const float dl = dot(shown, kLuma) - lumaP;
			const float w = wb * exp(-dl * dl * inv2s2);
			sum += w * (SmallModel.Load(int3(c, 0)).rgb - shown);
			wsum += w;
		}
	}
	// Single exit: fxc's flow analysis treats a second return as an uninitialised value.
	float3 edit = sum / max(wsum, 1e-4f);
	if (wsum < 1e-4f)
		edit = EditBilinear(pos_small / float2(gSmall));
	return edit;
}

// Mode 2: fit answer = a * shown + b over the 3x3 small texels around the sample, per channel,
// and apply the line to the full-res pixel. Guided-filter closed form: a = cov / (var + eps).
float3 EditLocalAffine(float2 pos_small, float3 P)
{
	const int2 c0 = int2(floor(pos_small - 0.5f)) ;
	float3 mean_in = 0.0f, mean_out = 0.0f, corr = 0.0f, var_in = 0.0f;
	[unroll]
	for (int j = -1; j <= 1; ++j)
	{
		[unroll]
		for (int i = -1; i <= 1; ++i)
		{
			const int2 c = clamp(c0 + int2(i, j), int2(0, 0), int2(gSmall) - 1);
			const float3 x = SmallIn.Load(int3(c, 0)).rgb;
			const float3 y = SmallModel.Load(int3(c, 0)).rgb;
			mean_in += x;
			mean_out += y;
			corr += x * y;
			var_in += x * x;
		}
	}
	mean_in /= 9.0f;
	mean_out /= 9.0f;
	corr = corr / 9.0f - mean_in * mean_out;
	var_in = var_in / 9.0f - mean_in * mean_in;
	const float3 a = corr / (var_in + gSigma);      // gSigma is epsilon here
	const float3 b = mean_out - a * mean_in;
	return (a * P + b) - P;
}

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
	if (tid.x >= gDst.x || tid.y >= gDst.y)
		return;

	const float4 original = Frame[tid.xy];
	const float3 P = saturate(original.rgb);
	// This full-res pixel's position in small-texture pixels (texel centres at +0.5).
	const float2 uv = (float2(tid.xy) + 0.5f) / float2(gDst);
	const float2 pos_small = uv * float2(gSmall);

	float3 edit;
	if (gGuided == 1)
		edit = EditBilateral(pos_small, dot(P, kLuma));
	else if (gGuided == 2)
		edit = EditLocalAffine(pos_small, P);
	else
		edit = EditBilinear(uv);
	edit *= gTransfer;

	const float3 result = CubeScaleResidual(P, P + edit);
	Frame[tid.xy] = float4(result, original.a);
}
