// The D3D12 half of DLSSNR.ControlMask: our own texture, and the one compute pass that fills it.
//
// The DECISION half is src/core/nr_mask_plan.hpp — read that first; it carries the disassembly
// this is built on, including the two things binding a mask costs. This file gathers numbers from
// D3D12, acts on the verdict, and makes no decision of its own.
//
// NGX-FREE ON PURPOSE, exactly like nr_stage: a committed texture, a descriptor heap, a root
// signature, a PSO and one dispatch. The WARP lane can link it and judge it under the debug layer
// with GPU-based validation on, which is the only place any of this can be judged at all
// (CLAUDE.md §6).
//
// WHY OUR OWN TEXTURE AND OUR OWN HEAP. The same reason mv_resolve owns its own: NGX clobbers
// whatever descriptor heap, root signature and PSO are bound when it runs, so there is nothing to
// share and no benefit in trying. The fill is recorded on the present-time list — ours under the
// native backend, ReShade's immediate list under the ReShade one — where nothing of the game's is
// bound, so there is nothing of the game's to restore either (see src/nr_hook.cpp's closing note).
//
// WHEN IT RUNS. Only when the content it would write differs from what the live texture already
// holds, or the texture is new. A constant mask therefore costs exactly one dispatch per session
// per value, and nothing at all on a steady-state frame.
#pragma once

#include "core/nr_mask_plan.hpp"

#include <cstdint>

struct ID3D12Device;
struct ID3D12GraphicsCommandList;
struct ID3D12Resource;

namespace stray_dlss::nrmask {

// THE MASK FORMAT — no longer a guess. [HARD, 2026-09-03]
//
// `NGXCubinD3D12::GetInputTextureViewHandle64` (0x18005d640) takes a caller's texture, runs its
// DXGI_FORMAT through a pure typeless/sRGB/depth canonicalizer (0x18005da00), builds a TEXTURE2D
// SRV from the result and hands it to NvAPI's CUDA-texture entry point. There is no whitelist and
// no rejection path anywhere on that route, and the `NGXCubinFormat_*` enum the runtime carries is
// used only for its OWN allocations. So the format is resolved by the texture unit: a UNORM
// format comes back normalised to [0,1], a FLOAT format comes back raw, and both are correct.
// The one class that is NOT correct is `*_UINT` / `*_SINT`, which the kernel's
// `tex.2d.v4.f32.f32` reads as floats — undefined values, no error — and which
// nrmaskplan::format_is_integer refuses outright.
//
// R8G8B8A8_UNORM is the default, and the reason is the arithmetic rather than a preference: the
// kernel computes `saturate(DLSSNR.Intensity * mask.x)`, so the only meaningful range is [0,1] and
// a normalised 8-bit channel spends every one of its 256 codes inside it. It also halves the
// footprint against RGBA16F (14.7 MB against 29.5 MB at 2560x1440) and is the format the WARP lane
// round-trips texel-for-texel. FLOAT remains a legitimate A/B — [STRAYDLSS] NgxNRMaskFormat takes
// a raw DXGI_FORMAT number — and 10 (R16G16B16A16_FLOAT) is the obvious other candidate if 8-bit
// quantisation of a blend weight ever turns out to matter, which at 1/255 it should not.
constexpr int kDefaultMaskFormat = 28; // DXGI_FORMAT_R8G8B8A8_UNORM

void set_format(int dxgi_format);
int format();

// The typed-UAV verdict for the CURRENT mask format, cached and LOGGED ONCE per format. It goes
// through nrstage::probe_typed_uav — the single probe in this codebase — rather than
// nrstage::probe, whose one-format cache would thrash if it were asked about the back buffer and
// the mask alternately every present.
nrmaskplan::FormatSupport probe(ID3D12Device *device);

// Idempotent; safe to call every present. Rebuilds when the extent or the format moves and
// RETIRES the old texture and heap behind the present ring rather than releasing them — the GPU
// may still be reading them, and a release under an in-flight list is a fault with no error
// return. Returns false and records last_error() on failure.
bool ensure(ID3D12Device *device, std::uint32_t width, std::uint32_t height, std::uint64_t frame);

// Records the fill if, and only if, the live texture does not already hold `plan`'s values.
// Returns true when the texture is filled and ready to be bound — including on the frames where
// nothing was recorded because it was already correct.
//
// Leaves the texture in NON_PIXEL_SHADER_RESOURCE, which is the state NVIDIA's guide wants an NGX
// input in and therefore the state it rests in between frames (docs/RESEARCH.md §3.5). A
// re-fill transitions back to UNORDERED_ACCESS and returns it.
bool record_fill(ID3D12GraphicsCommandList *cmd, const nrmaskplan::Plan &plan);

// The mask texture, or null before a successful ensure() + record_fill().
ID3D12Resource *texture();

// Present boundary: releases retired textures the ring says the GPU is done with.
void collect(std::uint64_t frame);

// Immediate teardown, for device destruction only. The caller has made the GPU idle.
void shutdown();

const char *last_error();

struct Stats
{
	std::uint64_t created = 0;  // textures allocated
	std::uint64_t retired = 0;  // superseded by a resize or a format change
	std::uint64_t released = 0; // actually freed once the ring cleared them
	std::uint64_t fills = 0;    // dispatches recorded — should be tiny, and a large number is a bug
	std::uint32_t live_retired = 0;
	std::uint64_t bytes = 0;
	// What the live texture actually holds, so the periodic line can report the mask's real
	// content rather than the configuration that was meant to produce it.
	float value_r = 0.0f;
	float value_g = 0.0f;
	float value_b = 0.0f;
	float value_a = 0.0f;
	bool filled = false;
};
Stats stats();

} // namespace stray_dlss::nrmask
