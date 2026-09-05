// The live half of the GRAPHICS seams (src/core/frame_seams.hpp): forwarding thunks on the
// FD3D12CommandContext vtable slots for RHIBeginDrawingViewport / RHIEndDrawingViewport and
// RHIBeginRenderPass / RHIEndRenderPass — the same vtable U0Hook found, extended by
// u0::discover_graphics_half — recording, on the RHI thread, which render passes target the
// back buffer and with what load action, resolving the engine's own back-buffer texture to the
// ID3D12Resource of THIS frame, and arming the HUD-less copy at the pass Slate draws the UI in.
//
// Two keys, two ladders, the seam's shape (docs/RESEARCH-ENGINE-TAA-HOOK.md §12):
//
//   [STRAYDLSS] EngineSeamBackBuffer — WHICH back buffer the game rendered this frame
//     0 off       nothing installed
//     1 observe   (DEFAULT) thunks installed (forwarding, image byte-identical); every frame the
//                 engine's answer is ASSERTED against core::fg::GameIndexMirror and against the
//                 PRESENT-barrier witness (backbuffer_state.hpp); counted on the [fg] and
//                 [rhigfx] lines
//     2 engine    the engine's answer SELECTS the replacement fg_present copies and generates
//                 from; the mirror is the assertion. Refused per frame (mirror used, counted)
//                 when the engine's resource is not in the candidate set.
//
//   [STRAYDLSS] EngineSeamHudless — the HUD-less frame for DLSS-G and NR
//     0 off       passes still classified for the identity route; no copy
//     1 observe   (DEFAULT) every back-buffer pass logged for the first frames with its action
//                 and its name pointer (only when it points into the game's .rdata; the string
//                 itself is not consulted for identification); per-frame counts `bbScene`,
//                 `bbHud`, brackets; the frame SHAPE judged every present
//     2 copy      at the HUD pass, once the shape has held kShapeAgreements frames and the
//                 texture is latched, CopyResource(hudless <- back buffer) is recorded on the
//                 game's own list before the UI's first draw (src/hudless.hpp) and handed to
//                 DLSS-G as DLSSG.HUDLess and — unless EngineSeamHudlessNR=0 — to NR as
//                 DLSSNR.Color with DLSSNR.Backbuffer = the final frame.
//
// PREREQUISITE: U0Hook >= 1, which is what discovers the vtable. With U0Hook=0 both keys log an
// ERROR and install nothing.
//
// Guards are U0Hook's, verbatim: every read of an engine object through a header-derived offset
// goes through VirtualQuery + SEH, the one engine call (FRHITexture::GetNativeResource, slot 7,
// HARD since L1) is refused unless the vptr is in the module with eight code slots, a fault
// latches the resolve off for the session at ERROR, and every decline is a named counter.
#pragma once

#include "core/engine_seam.hpp"
#include "core/frame_seams.hpp"
#include "core/u0_rhi_uav.hpp"

#include <cstddef>
#include <cstdint>

struct ID3D12GraphicsCommandList;

namespace stray_dlss::rhigfx {

struct Config
{
	int backbuffer_level = 1; // [STRAYDLSS] EngineSeamBackBuffer
	int hudless_level = 1;    // [STRAYDLSS] EngineSeamHudless
	bool hudless_nr = true;   // [STRAYDLSS] EngineSeamHudlessNR
	int log_passes = 2;       // [STRAYDLSS] EngineSeamHudlessLogFrames: frames whose back-buffer passes are logged
};
void configure(const Config &cfg);
const Config &config();
fseam::Level backbuffer_level();
fseam::Level hudless_level();

// From u0hook, inside its own discovery, once the compute half validated. `image` is the mapped
// game module (stable for the session). Runs discover_graphics_half and installs the thunks if
// either key is on. Must not call back into u0hook (its mutex is held).
void on_context_discovered(const u0::CtxDiscovery &ctx, const seam::Image &image);
bool hooked();

// The engine's own answer for THIS frame, read by fg_present inside the Present hook (same
// thread: RHIEndDrawingViewport -> FD3D12Viewport::Present -> our hook).
struct FrameIdentity
{
	std::uint64_t engine_res = 0; // ID3D12Resource* the in-bracket back-buffer pass resolved to; 0 = none
	bool candidate = false;       // ...and it is in the registered swapchain-class set
	std::uint64_t frame = 0;      // rhigfx's own frame counter (brackets closed)
};
FrameIdentity identity();

// From the Present hook (fg_present / present_owner), after the frame's identity was consumed:
// judges the frame's shape, advances the streak, resets the per-frame ledger. `mirror_res` and
// `barrier_res` are the two other witnesses, for the identity assertion; 0 = none.
void note_present(std::uint64_t mirror_res, std::uint64_t barrier_res, std::uint64_t present_frame);
// The swapchain was reconfigured (resize / FG re-arm): the engine recreates its reference
// texture (WindowsD3D12Viewport.cpp:392), so the latch is dropped.
void note_reconfigure(const char *why);

// ---- the HUD-less carrier ----
//
// The copy is armed inside the RHIBeginRenderPass thunk and RECORDED at the next hooked command
// on the same thread — SetPipelineState, ResourceBarrier or a Draw — because that call carries
// the ID3D12GraphicsCommandList the engine is recording the pass onto, which the RHI context does
// not expose (RHIGetNativeCommandBuffer returns nullptr on D3D12; measured `xor eax,eax; ret`).
// UE's ApplyState sets the PSO and flushes barriers before the first draw
// (D3D12StateCache.cpp:353-), so the copy lands before any UI pixel.
extern thread_local bool t_copy_pending;
void carry(ID3D12GraphicsCommandList *list);

struct Stats
{
	bool discovered = false;
	bool hooked = false;
	std::uint64_t frames = 0;             // brackets closed
	std::uint64_t begin_viewport = 0, end_viewport = 0, begin_pass = 0, end_pass = 0;
	std::uint64_t pass_decode_failed = 0;
	std::uint64_t pass_class[static_cast<int>(fseam::PassClass::count)] = {};
	std::uint64_t shape[static_cast<int>(fseam::FrameShape::count)] = {};
	std::uint64_t resolves = 0, resolve_failed = 0, resolve_not_candidate = 0;
	std::uint64_t relatch = 0;            // the latched texture stopped resolving to a candidate
	std::uint64_t id_vs_mirror[static_cast<int>(fseam::IdVerdict::count)] = {};
	std::uint64_t id_vs_barrier[static_cast<int>(fseam::IdVerdict::count)] = {};
	std::uint64_t selected_engine = 0, selected_mirror = 0, select_fell_back = 0;
	std::uint64_t copies = 0;             // HUD-less copies recorded
	std::uint64_t copy_refusal[static_cast<int>(fseam::CopyRefusal::count)] = {};
	std::uint64_t carrier_pso = 0, carrier_barrier = 0, carrier_draw = 0, carrier_lost = 0;
	std::uint64_t name_in_rdata = 0, name_elsewhere = 0;
	std::uint64_t faults = 0;
	bool disabled = false;
	bool latched = false;
	std::uint64_t latched_texture = 0;
	unsigned shape_ok_streak = 0;
	unsigned other_thread = 0;            // a seam fired on a thread other than the first one seen
};
Stats stats();
int format_report(char *buffer, std::size_t size);
void log_report(const char *when);

// Level-2 identity selection, for fg_present: the resource to treat as "the game's frame" given
// the mirror's answer; records the selection counters.
std::uint64_t select_game_frame(std::uint64_t mirror_res);

void shutdown();

} // namespace stray_dlss::rhigfx
