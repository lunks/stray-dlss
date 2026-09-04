// The live half of the render-target-pool name hook (src/core/pool_locator.hpp): mapping
// Stray's own module, locating FRenderTargetPool::FindFreeElement by caller-literal agreement,
// standing in for it (forwarding, image byte-identical) so the engine hands us the debug name
// of every pooled render target it allocates, and turning each name into an ID3D12Resource*.
//
// WHAT THIS IS FOR. DLSS Ray Reconstruction needs albedo, specular albedo, normal and
// roughness. Those are GBufferA-E, and the ~3 970-line heuristic finder that used to identify
// them by descriptor SHAPE was deleted on 2026-09-03 precisely so it would not be rebuilt the
// wrong way (docs/RESEARCH-ENGINE-TAA-HOOK.md §13). This is the right way: the engine passes
// each target's name as a live function argument and we read it.
//
// THE LADDER, [STRAYDLSS] PoolNames, and it is the seam's verbatim:
//   0 — off. Nothing is mapped, nothing is scanned.
//   1 — DISCOVER: scan, log the verdict and the residual, INSTALL NOTHING. The default.
//   2 — OBSERVE: additionally install the forwarding recorder, resolve each known name to an
//       ID3D12Resource*, and ASSERT it against the routes that already answer for the same
//       textures — L1's FPassInputs depth and velocity, and the extent the View CB's row 132
//       reports. Nothing consumes the map; the image is byte-identical.
//   3 — SUPPLY (RR's guides) is DECLARED, NOT IMPLEMENTED. Wiring Ray Reconstruction is a
//       separate decision after level 2 has run clean in GAMEPLAY. A request for 3 logs at
//       ERROR and runs 2.
//
// THE ESCALATION, STATED PLAINLY. Level 2 writes an inline trampoline into the GAME'S CODE.
// wrong target is a crash rather than a wrong number. Three things stand between here and that:
// the static bar (>= 3 distinct enclosing functions AND >= 4 distinct name literals agreeing on
// one .pdata function start, runner-up strictly beaten — pool::locate); the install is refused
// outright when the bar is not met, so the level DEGRADES to 1 and says so; and the trampoline
// itself is MinHook's, supplied by the plugin host rather than linked here, so a host that does
// not own MinHook (the ReShade add-on) can never install one.
//
// Guards are L1's verbatim (report §12.5): VirtualQuery before every read through a derived
// offset, SEH around it and around the one engine call (FRHITexture::GetNativeResource, slot 7,
// HARD on this exe since L1), a fault latches the mechanism off for the session at ERROR naming
// the address, and every refusal is a named, counted fallback.
//
// WINDOWS-PORTABLE ONLY. PE sections, the PE exception directory, MSVC x86-64 encodings, the
// engine's own objects and a real ID3D12Resource*. Nothing consults vkd3d, DXVK or Wine.
#pragma once

#include "core/pool_locator.hpp"

#include <cstddef>
#include <cstdint>

namespace stray_dlss::poolhook {

// The trampoline installer, supplied by the HOST rather than linked here.
//
// `stray_dlss_native` is linked by both hosts and neither has ever needed a code patcher; the
// UE4SS plugin host already owns MinHook (mods/StrayDLSS/src/Host.cpp) and registers itself
// here once MH_Initialize has succeeded. Without an installer the mechanism reports
// `no-installer` and stays at discovery — which is the correct behaviour for the ReShade
// add-on host, not a degradation to paper over.
using InstallFn = bool (*)(void *target, void *replacement, void **original);
using RemoveFn = bool (*)(void *target);
void set_installer(InstallFn install, RemoveFn remove);

// Reads [STRAYDLSS] PoolNames, runs discovery, and installs at level 2 if — and only if —
// discovery cleared the bar and an installer is registered.
void configure(int level);
pool::Level level();
// True once the static scan found FRenderTargetPool::FindFreeElement.
bool discovered();
// True while the forwarding recorder is installed.
bool hooked();

// What we know about one named pooled render target, as of the last call the engine made.
// Every pointer here is a plain ID3D12Resource* that no allocator owns; liveness is checked
// against our own registry at READ time, never assumed from the record.
struct Record
{
	bool seen = false;
	pool::RecordStatus status = pool::RecordStatus::ok;
	std::uint64_t pooled = 0;        // IPooledRenderTarget*, IDENTITY ONLY - never dereferenced outside the thunk
	std::uint64_t targetable = 0;    // ID3D12Resource* behind RenderTargetItem.TargetableTexture
	std::uint64_t shader_resource = 0; // ...and behind ShaderResourceTexture (usually the same object)
	std::uint32_t width = 0;         // from OUR resource registry's creation-time snapshot
	std::uint32_t height = 0;
	std::uint32_t dxgi_format = 0;
	std::uint64_t frame = 0;         // the frame the record was last written
	std::uint64_t calls = 0;         // how many times the engine has asked for this name
};
// A snapshot of one name's record. False when the name has never been seen.
bool record(pool::Target t, Record &out);

// THE LEVEL-2 ASSERTION. Called from the engine seam's AddPasses thunk, on the render thread,
// with what the OTHER engine routes resolved for the same frame: L1's FPassInputs depth and
// velocity (docs/RESEARCH-ENGINE-TAA-HOOK.md §12.9) and the scene-buffer extent the View CB's
// row 132 carries (§19). Each pair is judged, counted and — on the first disagreement per name
// — logged once. Nothing is gated on the result; this is an oracle beside two others.
void note_engine_frame(std::uint64_t depth_res, std::uint64_t velocity_res,
                       std::uint64_t colour_res,
                       std::uint32_t buffer_width, std::uint32_t buffer_height,
                       std::uint64_t frame);

// The TAA pass's own bind stream, from the descriptor walk at claim time: t1 is
// InputSceneColor (CLAUDE.md §2.3). Compared against the pooled SceneColorDeferred as an
// OBSERVATION with no prediction attached — whether the post-chain colour TAA reads is the same
// allocation AllocSceneColor made is exactly the sort of thing this project should measure
// rather than reason about. Safe to call with 0.
void note_taa_colour(std::uint64_t colour_res);

// One line for the periodic report and the same numbers for stray-dlss-status.txt.
int format_report(char *buffer, std::size_t size);
void log_report(const char *when);

// Removes the trampoline if it is still ours. MUST run before this DLL can be unloaded: a
// detour into a module that is gone is an address-0 crash on the next call, which this project
// has already paid for once (docs/RESEARCH-UE4SS-MIGRATION.md, the device-recreate reload).
void shutdown();

} // namespace stray_dlss::poolhook
