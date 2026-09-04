// The live half of the engine's route to `u0` (src/core/u0_rhi_uav.hpp): finding UE 4.27's
// FD3D12CommandContext vtable from our own Dispatch hook's return address, standing in for
// RHISetComputeShader and the six compute bind slots around RHISetUAVParameter (forwarding,
// image byte-identical), recording the bind STREAM of each RHISetComputeShader ...
// RHIDispatchComputeShader bracket on its own thread — every register the pass bound, with
// the engine's own object — and ASSERTING it against what the descriptor walk resolved for
// the same registers: u0, t0..t5, and the View constant buffer's `b` register.
//
// The ladder is the seam's, and it is non-negotiable (docs/RESEARCH-ENGINE-TAA-HOOK.md §12):
//
//   [STRAYDLSS] U0Hook
//   0 — off. Nothing is mapped, nothing is scanned.
//   1 — DISCOVER: resolve the seed, find and validate the vtable, log the verdict. Installs
//       nothing; cannot change a pixel.
//   2 — OBSERVE: additionally install the forwarding thunks and, on every dispatch the engine
//       announced and we claimed, compare the bracket's registers against the descriptor
//       walk's. One WARN per pass per register on disagreement; every verdict counted.
//   3 — AUTHORITATIVE is DECLARED, NOT IMPLEMENTED. Replacing the SRV/UAV table walk and the
//       View-CB search is a separate decision after the level-2 assertion has run clean
//       across gameplay, not menu only. A request for 3 is logged at WARN and treated as 2.
//
// Guards are L1's verbatim: VirtualQuery before every read through a scanned or latched
// offset, SEH around it and around the one engine call (FRHITexture::GetNativeResource, the
// slot L1 already proved on this exe), a fault latches the resolve off for the session at
// ERROR naming the address, and every refusal is a named, counted fallback. A wrong answer is
// refused, never used — and at level 2 nothing is "used" anyway: the descriptor walk stays
// authoritative and this is an oracle beside it.
#pragma once

#include "core/u0_rhi_uav.hpp"

#include <cstddef>
#include <cstdint>

namespace stray_dlss::u0hook {

void configure(int level);
int level();
// True once discovery succeeded (level >= 1).
bool discovered();
// True while the thunks are installed (level >= 2 and discovery succeeded).
bool hooked();

// From the native ID3D12GraphicsCommandList::Dispatch hook, on the recording thread, for a
// GAME dispatch (never under OwnCodeScope). `return_address` is the hook's own _ReturnAddress():
// the instruction after `CommandListHandle->Dispatch(...)` inside
// FD3D12CommandContext::RHIDispatchComputeShader, which is the discovery seed. It also closes
// this thread's pending bracket as "the bracket for this dispatch", so a later claim on the
// same thread can never pair binds from a different dispatch.
void note_dispatch(const void *return_address);

// What the descriptor walk resolved for the dispatch being claimed, register by register.
struct WalkAnswer
{
	std::uint64_t u0 = 0;                     // the output UAV's resource (0 if none)
	std::uint64_t t[u0::kMaxTexRegs] = {};    // SRV register -> resource (0 if none)
	bool view_cb_valid = false;               // the View-CB search found a plausible buffer
	unsigned view_cb_register = 0;            // ...on this `b` register
};

// THE ASSERTION. Called from the TAA hook for a dispatch the engine announced and we claimed,
// on the thread that recorded it, still inside the game's Dispatch call (so every object the
// bracket bound is alive). Resolves the bracket's objects, judges each register against the
// walk, counts, and logs disagreements once per pass.
void assert_at_claim(const WalkAnswer &walk, std::uint32_t out_width, std::uint32_t out_height,
                     std::uint64_t pass_hash);

// One line for the periodic report and the status file.
int format_report(char *buffer, std::size_t size);
void log_report(const char *when);

// Restores every patched slot that is still ours. Must run before this DLL can be unloaded.
void shutdown();

} // namespace stray_dlss::u0hook
