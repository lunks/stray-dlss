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
	// ROW_MAJOR IS LOAD-BEARING. HLSL packs constant-buffer matrices COLUMN-major by default and
	// we do not compile with /Zpr, so without this qualifier fxc reads our row-major upload
	// transposed — `mul(v, M)` then computes v * transpose(M). The camera-reconstruction branch
	// below silently produced wrong previous-clip positions for every pixel UE4's sparse velocity
	// buffer did not write, i.e. all static geometry.
	//
	// Why it hid for so long: with a still camera ClipToPrevClip is essentially the IDENTITY, and
	// a transposed identity is still the identity. So still frames were correct and only camera
	// MOTION exposed it — measured as flickering while moving, moire on the in-game LCD panels,
	// and shadows breaking up during movement and re-forming on stop. One bug, three symptoms,
	// all of them motion-only.
	row_major float4x4 ClipToPrevClip; // row-major, built from jitter-free (NoAA) matrices
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
	// A/B for the row_major fix above: non-zero reproduces the OLD transposed behaviour, so the
	// two can be compared live in one session instead of across builds.
	float    LegacyTransposedClip;
	float    Pad0;
	float2   SparseSign;       // multiplies the decoded moving-object velocity
	float2   CameraSign;       // multiplies the reconstructed camera motion
	// [STRAYDLSS] MvStats. Non-zero turns on the per-pixel census below and NOTHING ELSE: the
	// vector written to OutMV is bit-identical either way. It takes one float out of what used
	// to be `float4 Padding` so the constant buffer's size and every other field's offset are
	// unchanged — a layout shift here produces a plausible-looking wrong motion field rather
	// than an error, which is the single worst failure this file can have.
	float    StatsEnable;
	float    Pad1;
	float    Pad2;
	float    Pad3;
};

Texture2D<float>    SceneDepth  : register(t0); // R32_FLOAT_X8X24 view, reversed-Z
Texture2D<float4>   VelocityTex : register(t1); // R16G16B16A16_UNORM, BUFFER-sized
RWTexture2D<float2> OutMV       : register(u0); // R16G16_FLOAT, render resolution

// The census counters, as a ROOT UAV (root parameter 2) rather than a descriptor-table slot.
// A root descriptor needs no heap entry, so the descriptor ring, its per-frame slice width and
// every offset computed from it are untouched by the instrument. (src/core/mv_census.hpp)
RWByteAddressBuffer StatsBuf : register(u1);

// MUST match `enum Counter` in src/core/mv_census.hpp, ordinal for ordinal.
#define MVC_TOTAL             0
#define MVC_SPARSE            1
#define MVC_CAMERA            2
#define MVC_CAMERA_REJECT     3
#define MVC_FAR               4
#define MVC_FAR_REJECT        5
#define MVC_SPARSE_SATURATED  6
#define MVC_SPARSE_STILL      7
#define MVC_MV_LT_025         8
#define MVC_MV_LT_1           9
#define MVC_MV_LT_4          10
#define MVC_MV_LT_32         11
#define MVC_MV_GE_32         12
#define MVC_SPARSE_MOVING    13
#define MVC_NON_FINITE       14
#define MVC_RESERVED         15
#define MVC_COUNT            16

// src/core/mv_census.hpp kMovingThresholdPx / kStillThresholdPx.
static const float kMovingThresholdPx = 0.5f;
static const float kStillThresholdPx  = 0.0625f;

// One tile's counters, reduced in LDS so the global atomics are 16 per THREADGROUP rather than
// 16 per pixel. At 1920x1080 that is 32 400 groups instead of 2 073 600 pixels contending on
// the same sixteen addresses.
groupshared uint gsCounters[MVC_COUNT];

// UE 4.27 Common.ush:1537-1570. The bias is 32767/65535, NOT 0.5, and both constants were
// located in Stray's own DXBC. Computed from the expression rather than hardcoded, so the
// value cannot drift from the unit test that pins it. (CLAUDE.md §2.5)
static const float kInvDiv     = 1.0f / (0.499f * 0.5f);
static const float kDecodeBias = (32767.0f / 65535.0f) * kInvDiv;

float2 DecodeVelocity(float4 encoded)
{
	return encoded.xy * kInvDiv - kDecodeBias;
}

// The camera-motion reconstruction, factored out UNCHANGED so the census can evaluate it for a
// pixel that took the sparse branch — which is the only way to tell geometry that is genuinely
// moving from static geometry the base pass merely happened to write a velocity for. Stray
// ships `r.BasePassOutputsVelocity=True` (CLAUDE.md §2.3.1), so that distinction is exactly the
// unknown this instrument exists to settle.
//
// No object velocity here, so reconstruct the camera's own motion from reversed-Z
// depth. ClipToPrevClip is row-major and built from jitter-free matrices, so `mul(v, M)`
// (row-vector convention) is correct and no jitter compensation is needed — both this
// branch and the decoded branch are already jitter-free. (docs/RESEARCH.md §4.7)
bool ReconstructCamera(int2 buffer_pos, float2 screen_pos, out float2 ndc, out float device_z)
{
	device_z = SceneDepth.Load(int3(buffer_pos, 0));
	const float4 this_clip = float4(screen_pos, device_z, 1.0f);
	// Row-vector convention against a row-major matrix. mul(M, v) is the transpose and is
	// exactly what the missing row_major qualifier used to produce.
	const float4 prev_clip = LegacyTransposedClip != 0.0f
		? mul(ClipToPrevClip, this_clip)
		: mul(this_clip, ClipToPrevClip);

	if (prev_clip.w > 0.0f)
	{
		const float2 prev_screen = prev_clip.xy / prev_clip.w;
		ndc = screen_pos - prev_screen;
		return true;
	}

	ndc = float2(0.0f, 0.0f); // behind the camera
	return false;
}

// NDC -> the output contract's render-resolution pixels, pointing backward.
// Scale NDC to pixels and negate, so the vector points back to the previous frame. The Y
// term flips because NDC is Y-up while DLSS wants Y-down.
float2 NdcToOutputPixels(float2 velocity_ndc)
{
	const float2 mv_pixels = velocity_ndc * float2(0.5f * RenderSize.x, -0.5f * RenderSize.y);
	return -mv_pixels;
}

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID, uint gi : SV_GroupIndex)
{
	// The bounds test used to be an early `return`. It is a flag now because the census needs
	// two GroupMemoryBarrierWithGroupSync calls around the body, and a barrier that only some
	// lanes of a group reach is undefined. `StatsEnable` comes from the constant buffer and is
	// therefore uniform across the group, so both barriers are reached by all lanes or none.
	const bool inside = (tid.x < (uint)RenderSize.x) && (tid.y < (uint)RenderSize.y);
	const bool stats = (StatsEnable != 0.0f);

	// BOTH BARRIERS ARE UNCONDITIONAL, and that is deliberate rather than sloppy. `StatsEnable`
	// is a constant-buffer value and therefore uniform, so putting the syncs inside `if (stats)`
	// would very likely compile — but "very likely" is not a standard a group-wide barrier gets
	// to be held to: a barrier some lanes do not reach is undefined behaviour, and fxc's
	// divergence analysis is not something this project can test before shipping. Two syncs and
	// one LDS write per 8x8 group cost nothing measurable, and the vector written to OutMV is
	// bit-identical either way, which is the property that actually matters here.
	if (gi < MVC_COUNT)
		gsCounters[gi] = 0;
	GroupMemoryBarrierWithGroupSync();

	if (inside)
	{
		// The velocity and depth targets are BUFFER-sized, not view-sized, so they are indexed
		// with absolute buffer coordinates. (docs/RESEARCH.md §4.8)
		const uint2 buffer_pos = uint2(ViewRectMin) + tid.xy;

		// ViewportUVToScreenPos: (2u - 1, 1 - 2v). NDC, Y-up.
		//
		// The +0.5f is the PIXEL CENTRE, and it is deliberate. NVIDIA's own VelocityCombine.usf
		// changed convention between integrations: the UE 4.27 plugin (DLSS-SR 3.7.0) sampled the
		// pixel CORNER, `SvPositionToScreenPosition(float4(PixelPos.xy, 0, 1))`, while the current
		// UE5 plugin (8.7.2) samples `PixelPos.xy + 0.5f`. A uniform half-pixel bias in the
		// reconstructed static-geometry vectors is precisely the failure this project has already
		// shipped once (CLAUDE.md: bad motion vectors compound through the accumulation and never
		// look like a motion-vector bug), so the UE5 audit raised it as an open question against
		// this line.
		//
		// CHECKED 2026-09-03 by reading this shader: we already sample the centre, so we already
		// match the CURRENT plugin. Settled negative, no change — recorded here so the question is
		// not reopened. (docs/RESEARCH-DLSS-UE5-PLUGIN.md §2.4 and recommendation #3.)
		const float2 uv = (float2(tid.xy) + 0.5f) / RenderSize;
		const float2 screen_pos = float2(2.0f * uv.x - 1.0f, 1.0f - 2.0f * uv.y);

		float2 velocity_ndc;

		const float4 encoded = VelocityTex.Load(int3(buffer_pos, 0));

		// UE4 clears velocity to 0 and its own TAA tests `EncodedVelocity.x > 0.0` — strict, red
		// channel only. A magnitude test would be wrong, and so would testing both components.
		// (CLAUDE.md §2.5)
		const bool sparse = encoded.x > 0.0f;

		float2 camera_ndc = float2(0.0f, 0.0f);
		float device_z = 0.0f;
		bool camera_ok = true;

		if (sparse)
		{
			velocity_ndc = DecodeVelocity(encoded) * SparseSign;
			if (stats)
			{
				// Only the census needs the camera answer for a sparse pixel, and only to
				// classify it. The shipping path does not compute this.
				camera_ok = ReconstructCamera(int2(buffer_pos), screen_pos, camera_ndc, device_z);
			}
		}
		else
		{
			camera_ok = ReconstructCamera(int2(buffer_pos), screen_pos, camera_ndc, device_z);
			velocity_ndc = camera_ok ? (camera_ndc * CameraSign) : float2(0.0f, 0.0f);
		}

		const float2 out_mv = NdcToOutputPixels(velocity_ndc);
		OutMV[tid.xy] = out_mv;

		if (stats)
		{
			InterlockedAdd(gsCounters[MVC_TOTAL], 1u);

			// device_z is only loaded under `stats` for the sparse branch, so this covers the
			// whole frame either way.
			if (device_z == 0.0f)
			{
				InterlockedAdd(gsCounters[MVC_FAR], 1u);
				if (!camera_ok)
					InterlockedAdd(gsCounters[MVC_FAR_REJECT], 1u);
			}

			if (sparse)
			{
				InterlockedAdd(gsCounters[MVC_SPARSE], 1u);

				// The engine's velocity is R16G16B16A16_UNORM, so the encode CLAMPS: an NDC
				// component outside roughly +/-2.004 is stored at a rail and decodes to a lie.
				// A y that reached the LOW rail is indistinguishable from "not written" in the
				// x channel's own test, which is why both rails are counted rather than
				// argued about.
				if (encoded.x >= 1.0f || encoded.y >= 1.0f || encoded.y <= 0.0f)
					InterlockedAdd(gsCounters[MVC_SPARSE_SATURATED], 1u);

				if (length(out_mv) < kStillThresholdPx)
					InterlockedAdd(gsCounters[MVC_SPARSE_STILL], 1u);

				// Genuinely moving, as opposed to static geometry the base pass wrote anyway.
				const float2 camera_mv = NdcToOutputPixels(camera_ndc * CameraSign);
				if (!camera_ok || length(out_mv - camera_mv) > kMovingThresholdPx)
					InterlockedAdd(gsCounters[MVC_SPARSE_MOVING], 1u);
			}
			else
			{
				InterlockedAdd(gsCounters[MVC_CAMERA], 1u);
				if (!camera_ok)
					InterlockedAdd(gsCounters[MVC_CAMERA_REJECT], 1u);
			}

			// A NaN or Inf in the motion field is not a quality problem, it is a corrupted
			// accumulation. It must read zero; if it ever does not, nothing else on this line
			// is worth reading.
			if (!isfinite(out_mv.x) || !isfinite(out_mv.y))
				InterlockedAdd(gsCounters[MVC_NON_FINITE], 1u);

			// The buckets PARTITION the frame — every in-bounds pixel lands in exactly one,
			// which is the invariant src/core/mv_census.cpp checks before printing anything.
			const float mag = length(out_mv);
			uint bucket;
			if (mag < 0.25f)      bucket = MVC_MV_LT_025;
			else if (mag < 1.0f)  bucket = MVC_MV_LT_1;
			else if (mag < 4.0f)  bucket = MVC_MV_LT_4;
			else if (mag < 32.0f) bucket = MVC_MV_LT_32;
			else                  bucket = MVC_MV_GE_32;
			InterlockedAdd(gsCounters[bucket], 1u);
		}
	}

	GroupMemoryBarrierWithGroupSync();
	if (stats && gi < MVC_COUNT && gsCounters[gi] != 0u)
		StatsBuf.InterlockedAdd(gi * 4u, gsCounters[gi]);
}
