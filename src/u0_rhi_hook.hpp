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
//   3 — AUTHORITATIVE (BUILT 2026-09-05, UNCONFIRMED on the box): on a dispatch a pending
//       announcement expects, the bracket SUPPLIES `DispatchBindings::srvs`/`::uavs` — u0 and
//       t0..t5 with the engine's own objects — in place of the descriptor walk's, and the
//       matcher, the colour pick, the output UAV, the eye-adaptation SRV and the history
//       round-trip all read them unchanged. Only a COMPLETE bracket answers
//       (src/core/u0_authority.hpp, decide_source): anything short of the seven registers is
//       the walk for that frame, counted on the [u0] line by reason and register (`fellBack:`).
//       While the walk still runs it stays the assertion's other side, so level 2's oracle is
//       live under level 3. The View-CB search is NOT replaced here — that is
//       [STRAYDLSS] EngineSeamViewParams' job and it reads root CBVs, not the shadow.
//
//       [STRAYDLSS] U0HookSkipWalk=1 (default 0) additionally stops the shadow's COPY half
//       and the resolve's table walk once u0auth::kSkipArmClaims claimed dispatches have been
//       answered by the bracket running with no fallback — one-way for the session, so a
//       fallback after that costs the frame loudly (`noWalk`) rather than trusting a shadow
//       whose slots went stale while recording was off. The WRITE half stays: the bind-stream
//       hop is a cross-match against exactly those Create*View records.
//
// Guards are L1's verbatim: VirtualQuery before every read through a scanned or latched
// offset, SEH around it and around the one engine call (FRHITexture::GetNativeResource, the
// slot L1 already proved on this exe), a fault latches the resolve off for the session at
// ERROR naming the address, and every refusal is a named, counted fallback. A wrong answer is
// refused, never used — and at level 2 nothing is "used" anyway: the descriptor walk stays
// authoritative and this is an oracle beside it.
#pragma once

#include "core/u0_authority.hpp"
#include "core/u0_rhi_uav.hpp"
#include "intercept/types.hpp"

#include <cstddef>
#include <cstdint>

namespace stray_dlss::u0hook {

// `skip_walk_key` is [STRAYDLSS] U0HookSkipWalk; it is inert below level 3 and says so.
void configure(int level, bool skip_walk_key);
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

// LEVEL 3. From the TAA hook, on the recording thread, right after the descriptor walk, for a
// dispatch a pending announcement expects (seamhook::announced_expects). Resolves this
// thread's closed bracket — t0..t5 through FRHITexture::GetNativeResource / the SRV
// cross-match, u0..u3 as bound — checks every answer against the resource registry, and on
// Source::bracket REPLACES `b.srvs` and `b.uavs` with them (each BoundTexture described from
// the registry; the stencil's view format from the shadow's record of its offline handle).
// Nothing else in `b` moves. On Source::walk `b` is untouched and the reason is returned. The
// decision and the resolved registers are kept for this thread's next assert_at_claim, which
// counts them against CLAIMED dispatches only and reuses the resolves rather than calling into
// the engine twice. Below level 3 this returns level_below_3 at once and counts nothing.
u0auth::Decision take_bindings(icept::DispatchBindings &b);

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
// walk, counts, and logs disagreements once per pass. Under level 3 `walk` must be the WALK's
// answer (the caller keeps a copy from before take_bindings substituted `b`), so the oracle
// keeps comparing two routes; once U0HookSkipWalk has armed the walk is empty and every
// verdict reads `walkAbsent` by design. This is also where level 3's per-claim counters and
// the skip latch are driven.
void assert_at_claim(const WalkAnswer &walk, std::uint32_t out_width, std::uint32_t out_height,
                     std::uint64_t pass_hash);

// One line for the periodic report and the status file.
int format_report(char *buffer, std::size_t size);
void log_report(const char *when);

// ---- shared with the graphics seams (src/rhi_gfx_hook.hpp) ----
//
// One discovery, two halves: the graphics thunks stand on the same vtable and use the same
// guarded reader, the same FRHITexture::GetNativeResource route and the same slot patcher, so
// none of that is duplicated. Each is safe to call from any thread; none takes u0hook's mutex
// (rhigfx::on_context_discovered runs INSIDE the discovery, which holds it).
bool guarded_read_u64(std::uint64_t va, std::uint64_t *out);
// FRHITexture* -> ID3D12Resource* through vtable slot seam::kRhiGetNativeResourceSlot, refused
// (0) unless the vptr is in the module with eight code slots; the call runs under SEH and a
// fault is counted against u0hook's own latch.
std::uint64_t resolve_texture_native(std::uint64_t rhi_texture);
// VirtualProtect-write-restore on one vtable slot; `original_out` receives what was there.
bool patch_vtable_slot(void **slot, void *replacement, void **original_out);
// True when `va` lies inside a READ-ONLY, non-executable section of the game module (.rdata):
// where a `TEXT("...")` literal lives. Identity of the pass NAME pointer, never its contents.
bool in_module_rdata(std::uint64_t va);
// The module's code / read-only ranges, for a caller that needs the image itself.
const seam::Image *module_image();

// Restores every patched slot that is still ours. Must run before this DLL can be unloaded.
void shutdown();

} // namespace stray_dlss::u0hook
