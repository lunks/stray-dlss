// THE HUD-LESS FRAME: a copy of the back buffer taken on the game's OWN command list at the
// render pass Slate draws the UI in (src/rhi_gfx_hook.hpp), before the UI's first draw — so it
// carries the tonemapped scene and nothing drawn over it. Consumed at present time by DLSS-G as
// DLSSG.HUDLess and by the NR stage as DLSSNR.Color (with DLSSNR.Backbuffer = the final frame).
//
// NGX-FREE ON PURPOSE, like src/nr_stage.hpp: one committed texture ring and one CopyResource
// wrapped in four barriers, so the WARP lane can judge the barriers under the debug layer
// (tests/warp/warp_hudless.inc) and read the pixels back.
//
// WHY THE GAME'S LIST, and why that is safe where the SR path's own list would not be
// (CLAUDE.md §5, "the SR path cannot move to our own command list"): the copy has to land
// BETWEEN the scene's last write and the UI's first draw, and both are on the list the RHI
// thread is recording; a list of ours executed at Present would run after the UI. Inserting a
// copy mid-list is legal D3D12 - CopyResource binds nothing and unbinds nothing, the barriers
// put the back buffer back into the state the engine holds it in - and it is exactly what
// ngxfg::publish already does for depth and motion vectors at the TAA site.
//
// THE BACK BUFFER'S STATE AT THE COPY IS RENDER_TARGET, from the engine's own source and not
// from a ledger: Slate transitions it to RTV before its pass (SlateRHIRenderer.cpp:847) and
// RHIBeginRenderPass binds it as render target 0 (D3D12Commands.cpp:1039-1116, :1851 transitions
// every bound RTV to D3D12_RESOURCE_STATE_RENDER_TARGET). The ledger (backbuffer_state.hpp)
// carries a MARKER for each copy and judges the assumption when the list executes -
// `markers bad` on the [bbstate] line must stay 0.
//
// ONE TEXTURE WOULD DO - the copy for frame N+1 is on the game's list of frame N+1, which the
// queue executes after the present list of frame N that read frame N's copy - but a ring of two
// costs 66 MB at 4K and removes an ordering argument from the path.
#pragma once

#include <cstddef>
#include <cstdint>

struct ID3D12Device;
struct ID3D12GraphicsCommandList;
struct ID3D12Resource;
struct D3D12_RESOURCE_DESC;

namespace stray_dlss::hudless {

constexpr unsigned kRing = 2;

// Idempotent; rebuilds on an extent/format change and retires the old ring behind the present
// ring rather than releasing it under an in-flight list.
bool ensure(ID3D12Device *device, const D3D12_RESOURCE_DESC &like, std::uint64_t frame);

// Records `hudless <- back_buffer` on `list`, with the back buffer in `bb_state` before and
// after. Returns the destination (non-owning; rests in COMMON) and advances the publication
// sequence, or null (counted, last_error set) when the ring is not ready.
ID3D12Resource *record_copy(ID3D12GraphicsCommandList *list, ID3D12Resource *back_buffer,
                            std::uint32_t bb_state, std::uint64_t frame);

// The newest copy for the present-time consumers. `sequence` is a PUBLICATION counter: a
// consumer that remembers the last sequence it used sees "no copy this frame" as an unchanged
// number, exactly the guide-freshness rule nr_hook_plan.hpp uses.
struct Current
{
	ID3D12Resource *texture = nullptr;
	std::uint64_t sequence = 0; // 0 = never copied
	std::uint32_t width = 0;
	std::uint32_t height = 0;
	unsigned format = 0;        // DXGI_FORMAT
};
Current current();

// Present boundary: releases a retired ring the present ring says the GPU is done with.
void collect(std::uint64_t frame);
void shutdown();

// The one-shot validation dump ([STRAYDLSS] NgxDumpInputs): the copy `count` is handed to
// `wants`, and when it says so the texture is captured through `dump` under the name "hudless"
// (straydlss_hudless_<count>.bin; tools/rawdump2png.py rgb10a2). Installed by the application so
// this module stays NGX- and dump-free for the WARP lane.
using DumpFn = bool (*)(ID3D12Device *device, ID3D12GraphicsCommandList *cmd, ID3D12Resource *resource,
                        unsigned state_before, const char *name, std::uint64_t count);
using WantsFn = bool (*)(std::uint64_t count);
void set_dump(DumpFn dump, WantsFn wants);

struct Stats
{
	std::uint64_t copies = 0;
	std::uint64_t created = 0;
	std::uint64_t retired = 0;
	std::uint64_t released = 0;
	std::uint64_t dumps = 0;
	std::uint64_t not_ready = 0;
	std::uint64_t bytes = 0; // the live ring's footprint
	std::uint32_t width = 0, height = 0;
	unsigned format = 0;
};
Stats stats();
const char *last_error();

} // namespace stray_dlss::hudless
