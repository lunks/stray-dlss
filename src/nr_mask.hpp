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

// THE MASK FORMAT, and the honest state of the evidence for it.
//
// The runtime does NOT inspect a caller-supplied texture's DXGI_FORMAT anywhere on this path:
// the guide-rect builder at 0x18001c520 reads only Width and Height out of the resource
// description, and the registration call at 0x18001cbb5 passes the resource and a usage word and
// nothing else. So the format is interpreted entirely by the CUDA texture object the driver
// mints from the D3D12 descriptor, and the kernel's assumed channel type has to match it. That
// is exactly the class of thing that produces a wrong image with no error [CLAUDE.md §0.2], so
// it is a KNOB rather than a constant, logged with its probe result, and one run can A/B two
// candidates instead of costing a round trip each.
//
// R16G16B16A16_FLOAT is the default because it is the format the runtime uses for every internal
// texture it creates for this same kernel family — the two it names in its own log lines are
// `dlssnr_prev_output` and `dlssnr_network_output_scratch`, both "RGBA16F", and the create
// descriptor it fills in for them carries format enum 2 at +0x0c (0x180019636). That is a SOFT
// argument by analogy, not a HARD reading of how the mask itself is sampled, and it is labelled
// as such here so nobody later mistakes it for one. [STRAYDLSS] NgxNRMaskFormat overrides it with
// a raw DXGI_FORMAT number.
constexpr int kDefaultMaskFormat = 10; // DXGI_FORMAT_R16G16B16A16_FLOAT

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
