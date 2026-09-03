// DLSSNR.ControlMask fill.
//
// Writes one constant RGBA value over the whole mask rect. That is deliberately the least
// interesting shader in this repository, and it is the first thing to run: a mask that is
// SILENTLY IGNORED and a mask that is bound and doing nothing look identical on a screenshot, so
// the plumbing, the format and the registration have to be proven with a value whose effect is
// known before any heuristic is written (CLAUDE.md §0.2, and the "one clean frame produced a
// premature conclusion" trap in §5).
//
// The channels, per src/core/nr_mask_plan.hpp: R is the per-pixel final blend weight, G scales
// local tone, B scales local structure. A is not a documented channel and is written as 1.0 so
// nothing in the texture is ever uninitialised. 1.0 in every channel is the identity.
//
// The extension point for a content-driven mask is here — the pass already has the mask's rect
// and a UAV over it, and nr_hook.cpp already holds the depth and dense motion vectors the TAA
// hook publishes — but it is deliberately not taken yet.

// Root 32-bit constants, not a constant buffer: six DWORDs is far below the 64-DWORD root limit,
// and it removes an upload buffer and its per-frame versioning from a pass that runs only when
// the value changes.
cbuffer Params : register(b0)
{
	float4 gValue;  // what every pixel gets
	uint2  gExtent; // the mask rect, in pixels
};

RWTexture2D<float4> OutMask : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
	if (tid.x >= gExtent.x || tid.y >= gExtent.y)
		return;
	OutMask[tid.xy] = gValue;
}
