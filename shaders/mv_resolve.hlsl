// Dense motion vectors for DLSS, from UE 4.27's sparse velocity plus reconstructed camera
// motion. This is NVIDIA's own VelocityCombine.usf math, adapted to read Stray's buffers
// directly. (docs/RESEARCH.md §3.2)
//
// A resolve pass is mandatory for two independent reasons, either of which alone would force
// it: UE4 writes velocity ONLY for moving objects, so every static pixel is blank whenever the
// camera moves; and the format it writes, R16G16B16A16_UNORM, is not one DLSS accepts.
//
// Output contract, all three parts of which are load-bearing:
//   * R16G16_FLOAT, at RENDER resolution (1920x1080 here)
//   * units are render-resolution PIXELS, [0,0] at the upper-left
//   * vectors point BACKWARD — adding one to a pixel's position gives where it was last frame
// With that contract met, NGX InMVScaleX/Y are (1,1).

cbuffer Params : register(b0)
{
	float4x4 ClipToPrevClip;   // row-major, built from jitter-free (NoAA) matrices
	float2   RenderSize;       // View.ViewSizeAndInvSize.xy
	float2   ViewRectMin;      // View.ViewRectMin.xy
	float2   BufferSize;       // View.BufferSizeAndInvSize.xy
	// Per-branch sign control. UE4's velocity buffer is SPARSE, so this shader has two paths —
	// decoded object velocity and reconstructed camera motion — and they arrive by different
	// routes with independently-derived conventions. A user-reported artifact that appears ONLY
	// on moving objects while static geometry stays clean is exactly what one inverted branch
	// looks like, and it is not decidable by inspection here: both paths are documented as
	// current-minus-previous, but one of those is [derived], not measured. So expose it rather
	// than argue about it — the injector plugins all ship this as a switch for the same reason.
	// (1,1) is the current behaviour.
	float2   SparseSign;       // multiplies the decoded moving-object velocity
	float2   CameraSign;       // multiplies the reconstructed camera motion
	float2   Padding;
};

Texture2D<float>    SceneDepth  : register(t0); // R32_FLOAT_X8X24 view, reversed-Z
Texture2D<float4>   VelocityTex : register(t1); // R16G16B16A16_UNORM, BUFFER-sized
RWTexture2D<float2> OutMV       : register(u0); // R16G16_FLOAT, render resolution

// UE 4.27 Common.ush:1537-1570. The bias is 32767/65535, NOT 0.5, and both constants were
// located in Stray's own DXBC. Computed from the expression rather than hardcoded, so the
// value cannot drift from the unit test that pins it. (CLAUDE.md §2.5)
static const float kInvDiv     = 1.0f / (0.499f * 0.5f);
static const float kDecodeBias = (32767.0f / 65535.0f) * kInvDiv;

float2 DecodeVelocity(float4 encoded)
{
	return encoded.xy * kInvDiv - kDecodeBias;
}

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
	if (tid.x >= (uint)RenderSize.x || tid.y >= (uint)RenderSize.y)
		return;

	// The velocity and depth targets are BUFFER-sized, not view-sized, so they are indexed
	// with absolute buffer coordinates. (docs/RESEARCH.md §4.8)
	const uint2 buffer_pos = uint2(ViewRectMin) + tid.xy;

	// ViewportUVToScreenPos: (2u - 1, 1 - 2v). NDC, Y-up.
	const float2 uv = (float2(tid.xy) + 0.5f) / RenderSize;
	const float2 screen_pos = float2(2.0f * uv.x - 1.0f, 1.0f - 2.0f * uv.y);

	float2 velocity_ndc;

	const float4 encoded = VelocityTex.Load(int3(buffer_pos, 0));

	// UE4 clears velocity to 0 and its own TAA tests `EncodedVelocity.x > 0.0` — strict, red
	// channel only. A magnitude test would be wrong, and so would testing both components.
	// (CLAUDE.md §2.5)
	if (encoded.x > 0.0f)
	{
		velocity_ndc = DecodeVelocity(encoded) * SparseSign;
	}
	else
	{
		// No object velocity here, so reconstruct the camera's own motion from reversed-Z
		// depth. ClipToPrevClip is row-major and built from jitter-free matrices, so `mul(v, M)`
		// (row-vector convention) is correct and no jitter compensation is needed — both this
		// branch and the decoded branch are already jitter-free. (docs/RESEARCH.md §4.7)
		const float device_z = SceneDepth.Load(int3(buffer_pos, 0));
		const float4 this_clip = float4(screen_pos, device_z, 1.0f);
		const float4 prev_clip = mul(this_clip, ClipToPrevClip);

		if (prev_clip.w > 0.0f)
		{
			const float2 prev_screen = prev_clip.xy / prev_clip.w;
			velocity_ndc = (screen_pos - prev_screen) * CameraSign;
		}
		else
		{
			velocity_ndc = float2(0.0f, 0.0f); // behind the camera
		}
	}

	// Scale NDC to pixels and negate, so the vector points back to the previous frame. The Y
	// term flips because NDC is Y-up while DLSS wants Y-down.
	const float2 mv_pixels = velocity_ndc * float2(0.5f * RenderSize.x, -0.5f * RenderSize.y);
	OutMV[tid.xy] = -mv_pixels;
}
