// The live half of the engine's route to `u0` (src/core/u0_rhi_uav.hpp): finding UE 4.27's
// FD3D12CommandContext vtable from our own Dispatch hook's return address, standing in for
// its RHISetComputeShader and the compute bind slots around RHISetUAVParameter (forwarding,
// image byte-identical), correlating the UAVIndex==0 bind with the dispatch that follows it on
// the same thread, and ASSERTING the engine's answer against the descriptor walk's `u0`.
//
// The ladder is the seam's, and it is non-negotiable (docs/RESEARCH-ENGINE-TAA-HOOK.md §12):
//
//   [STRAYDLSS] U0Hook
//   0 — off. Nothing is mapped, nothing is scanned.
//   1 — DISCOVER: resolve the seed, find and validate the vtable, log the verdict. Installs
//       nothing; cannot change a pixel.
//   2 — OBSERVE: additionally install the forwarding thunks and, on every dispatch the engine
//       announced and we claimed, compare the u0 the RHI bound against the u0 the descriptor
//       walk resolved. One WARN per pass on disagreement; every verdict counted continuously.
//   3 — AUTHORITATIVE is DECLARED, NOT IMPLEMENTED. Deleting the SRV/UAV table walk is a
//       separate decision after the level-2 assertion has run clean across gameplay, not menu
//       only. A request for 3 is logged at WARN and treated as 2.
//
// Guards are L1's verbatim: VirtualQuery before every read through a scanned or latched
// offset, SEH around it (MSVC), a fault latches the mechanism off for the session at ERROR
// naming the address, and every refusal is a named, counted fallback. A wrong answer is
// refused, never used — and at level 2 nothing is "used" anyway: the descriptor walk stays
// authoritative and this is an oracle beside it.
#pragma once

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
// FD3D12CommandContext::RHIDispatchComputeShader, which is the discovery seed. It also moves
// this thread's pending UAVIndex==0 bind into "the bind for this dispatch", so a later claim
// on the same thread can never pair a bind from a different dispatch.
void note_dispatch(const void *return_address);

// THE ASSERTION. Called from the TAA hook for a dispatch the engine announced and we claimed,
// on the thread that recorded it, with the descriptor walk's answer for u0 (0 if it had none)
// and the engine's announced output rect. Counts a verdict; one WARN per pass on disagreement.
void assert_at_claim(std::uint64_t walk_u0, std::uint32_t out_width, std::uint32_t out_height,
                     std::uint64_t pass_hash);

// One line for the periodic report and the status file.
int format_report(char *buffer, std::size_t size);
void log_report(const char *when);

// Restores every patched slot that is still ours. Must run before this DLL can be unloaded.
void shutdown();

} // namespace stray_dlss::u0hook
