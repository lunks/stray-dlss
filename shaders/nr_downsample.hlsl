// NR model resolution, pass 1: an EXACT AREA AVERAGE of the full frame into the model's extent.
//
// Not a bilinear tap. A bilinear sample of a shrinking image reads four texels and ignores the
// rest, so most of the picture never reaches the model and what does is weighted by where the
// sample landed rather than by how much of the pixel it covers - aliasing on the way in, and the
// model's answer then changes with sub-pixel motion for no reason in the scene. Integrating the
// source over the exact footprint of the destination pixel is the correct box resample and costs
// a handful of loads at these ratios. (After OptiScaler_DLSSNR's downsample, hhkbble's.)
//
// Two identical outputs: OutIn is what the model was SHOWN and is what the resolve subtracts;
// OutModel is what the model overwrites with its answer. Writing both here is one pass cheaper
// than a copy and keeps the two bit-identical by construction.

cbuffer Params : register(b0)
{
	uint2 gSrc; // full-frame extent
	uint2 gDst; // model extent
};

Texture2D<float4>   Src      : register(t0);
RWTexture2D<float4> OutIn    : register(u0);
RWTexture2D<float4> OutModel : register(u1);

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
	if (tid.x >= gDst.x || tid.y >= gDst.y)
		return;

	// The destination pixel's footprint in source pixels, as a real-valued box.
	const float x0 = (float(tid.x)     * float(gSrc.x)) / float(gDst.x);
	const float x1 = (float(tid.x + 1) * float(gSrc.x)) / float(gDst.x);
	const float y0 = (float(tid.y)     * float(gSrc.y)) / float(gDst.y);
	const float y1 = (float(tid.y + 1) * float(gSrc.y)) / float(gDst.y);

	const uint ix0 = uint(floor(x0));
	const uint ix1 = min(uint(ceil(x1)), gSrc.x);
	const uint iy0 = uint(floor(y0));
	const uint iy1 = min(uint(ceil(y1)), gSrc.y);

	float4 sum = 0.0f;
	float weight = 0.0f;
	for (uint y = iy0; y < iy1; ++y)
	{
		// Vertical coverage of this source row by the footprint.
		const float wy = min(float(y + 1), y1) - max(float(y), y0);
		for (uint x = ix0; x < ix1; ++x)
		{
			const float wx = min(float(x + 1), x1) - max(float(x), x0);
			const float w = wx * wy;
			sum += Src.Load(int3(x, y, 0)) * w;
			weight += w;
		}
	}
	const float4 v = weight > 0.0f ? sum / weight : 0.0f;
	OutIn[tid.xy] = v;
	OutModel[tid.xy] = v;
}
