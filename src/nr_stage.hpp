// The D3D12 half of the DLSS Neural Rendering PRESENT STAGE: our own copy of the back buffer,
// and the two transfers that get the frame into it and the network's answer back out.
//
// NGX-FREE ON PURPOSE. Everything here is plain D3D12 — a committed texture, four barriers and
// two CopyResource calls — so the WARP lane can link it and judge it under the debug layer with
// GPU-based validation on. src/nr_hook.cpp adds the guides, the gate and the NGX evaluate; none
// of that can be built on a CI machine with no driver, and keeping the two apart is what makes
// the transfers testable at all (CLAUDE.md §6, "keep the untestable NGX surface as thin as
// possible").
//
// WHY A COPY AT ALL, rather than handing NGX the back buffer directly. Two reasons and the first
// is sufficient: NGX writes DLSSNR.Output through a UAV, and a swapchain buffer is not created
// with ALLOW_UNORDERED_ACCESS, so it can never be the output. The second is that a swapchain
// allocation is a special one and handing it to a CUDA-interop texture object is exactly the kind
// of thing that hangs rather than errors on this stack. NGX also has no defined behaviour for
// aliasing DLSSNR.Color with DLSSNR.Output, so the evaluate runs staging -> our neural texture
// and the answer is copied back over the staging copy by ngx_nr, then over the back buffer here.
//
// WHY THE BACK BUFFER'S OWN FORMAT rather than FP16. A copy cannot convert formats, so an FP16
// staging pair would need a conversion compute pass in EACH direction — and the write-back one
// would still need typed UAV store on the back buffer's format, so it would not even avoid the
// probe. Staging in the native format makes both transfers plain same-format copies with no
// shader of ours anywhere in the path. The image is display-referred either way; there is no HDR
// range here to lose (docs/STRAY-RENDERING-FACTS.md §32: R10G10B10A2_UNORM, no SetColorSpace1).
#pragma once

#include "nr_codec_pass.hpp" // nrp::TypedUavSupport, the ONE typed-UAV probe in this codebase

#include <cstdint>

struct ID3D12Device;
struct ID3D12GraphicsCommandList;
struct ID3D12Resource;

namespace stray_dlss::nrstage {

// The state our staging copy rests in between frames, and the state NVIDIA's guide wants an NGX
// input in (docs/RESEARCH.md §3.5). Keeping the resting state equal to the NGX state means the
// steady-state frame records two transition pairs and no more — and it is the state
// nr::Site::post_tonemap documents `image` arriving in and being left in.
// D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, spelled as a number so this header stays free
// of <d3d12.h>.
constexpr std::uint32_t kStagingRestState = 0x40;

// HOW THE BACK BUFFER'S BARRIERS ARE ISSUED, and why this is a callback rather than a plain
// ResourceBarrier.
//
// Under the native backend the present list is executed only if something was recorded on it, and
// `NativeBackend::present_barrier` is what says so (present_owner::note_present_list_used). Under
// ReShade the immediate list is flushed only if ReShade's OWN api recorded something
// (`_has_commands`, d3d12_impl_command_list_immediate.cpp:122), and its `barrier()` is what sets
// that. Both are `icept::Backend::present_barrier`. So the back buffer's transitions go through
// the caller's function, and only the copies and our own staging texture's transitions are
// recorded natively — exactly the split src/nr_history.cpp already runs on.
using BarrierFn = void (*)(void *ctx, ID3D12Resource *res, std::uint32_t before,
                           std::uint32_t after);

// Idempotent; safe to call every frame. Rebuilds when the extent or the format moves (a
// ResizeBuffers) and RETIRES the old texture behind a two-ring-cycle delay rather than releasing
// it — the GPU may still be reading it several frames back, and a release under an in-flight list
// is a fault with no error return. `frame` is the present index.
bool ensure(ID3D12Device *device, std::uint32_t width, std::uint32_t height, int dxgi_format,
            std::uint64_t frame);

// The staging texture, or null before a successful ensure().
ID3D12Resource *staging();

// The typed-UAV verdict for `dxgi_format`, cached and LOGGED ONCE per format. "The back buffer is
// R10G10B10A2_UNORM and this device cannot store to it through a typed UAV" is precisely the kind
// of fact that must not have to be inferred from a black screen (CLAUDE.md §0.2).
nrp::TypedUavSupport probe(ID3D12Device *device, int dxgi_format);

// back buffer -> staging. `colour_state` is the D3D12_RESOURCE_STATES the back buffer is in when
// we are called, and it is put back into it before this returns.
void record_capture(ID3D12GraphicsCommandList *cmd, ID3D12Resource *colour,
                    std::uint32_t colour_state, BarrierFn barrier, void *ctx);

// staging -> back buffer. Recorded only when the neural result actually landed in the staging
// copy; a refusal leaves the back buffer byte-for-byte the game's own.
void record_writeback(ID3D12GraphicsCommandList *cmd, ID3D12Resource *colour,
                      std::uint32_t colour_state, BarrierFn barrier, void *ctx);

// Present boundary: releases retired textures the ring says the GPU is done with.
void collect(std::uint64_t frame);

// Immediate teardown, for device destruction only.
void shutdown();

struct Stats
{
	std::uint64_t created = 0;   // staging textures allocated
	std::uint64_t retired = 0;   // superseded by a resize
	std::uint64_t released = 0;  // actually freed once the ring cleared them
	std::uint32_t live_retired = 0;
	std::uint64_t bytes = 0;     // the live staging texture's own footprint
};
Stats stats();

const char *last_error();

} // namespace stray_dlss::nrstage
