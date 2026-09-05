// THE BACK BUFFER'S D3D12 STATE, FROM THE ENGINE'S OWN BARRIERS ([STRAYDLSS] EngineSeamBackBufferState).
//
// The NR present stage names a StateBefore for the back buffer and D3D12 cannot be asked what
// the state is; today the constant is ASSUMED (PRESENT, [STRAYDLSS] NgxNRStageBackBufferState —
// required by the API at Present, unverified on this stack). The engine STATES every transition
// it makes through ID3D12GraphicsCommandList::ResourceBarrier, and FD3D12Viewport::Present ends
// each frame by transitioning the back buffer to PRESENT and flushing (D3D12Viewport.cpp:820-829)
// before IDXGISwapChain::Present. This module records those statements for the swapchain-class
// resources only — the real buffers, or frame generation's replacements — and replays them in
// GPU order.
//
// GPU ORDER IS EXECUTE ORDER, NOT RECORDING ORDER, and that is the whole design of the ledger
// (core/frame_seams.hpp, StateLedger): UE 4.27 resolves a resource's FIRST transition on a
// command list at submission, onto a barrier list that executes BEFORE the list
// (FD3D12CommandListHandle::AddPendingResourceBarrier, D3D12CommandList.cpp:14), so the
// PRESENT -> RENDER_TARGET a frame begins with is recorded AFTER the RENDER_TARGET -> PRESENT it
// ends with. Barriers are therefore queued per list and applied when ID3D12CommandQueue::
// ExecuteCommandLists names the list.
//
//   level 0  off      nothing recorded; the stage uses its assumed constant
//   level 1  observe  recorded and, at every present the stage runs, ASSERTED against the
//                     assumed constant (agree / disagree / unknown, counted, one WARN)
//   level 2  engine   the recorded state is what the stage names; the constant is the fallback
//                     while the engine has not yet transitioned the resource
//
// Two more things the ledger yields for free, because they are the same barriers:
//   * the resource the engine last left in PRESENT — the second engine witness of "which back
//     buffer was this frame's" (frame_seams.hpp item 1b), asserted against the render-pass route
//     and the GameIndexMirror on the [fg] line;
//   * `beforeMismatch`: every StateBefore the engine wrote that disagrees with the state this
//     ledger held. UE tracks per-resource state itself; if the two ever disagree, one of them is
//     wrong, and this is how we would know.
//
// The hooks it feeds from are the native backend's own (d3d12_hooks.cpp): ResourceBarrier,
// Reset and the queue's ExecuteCommandLists. Anything recorded under OwnCodeScope — our present
// list, FG's copies — is not the game's and is not recorded.
#pragma once

#include "core/frame_seams.hpp"

#include <cstddef>
#include <cstdint>

struct ID3D12GraphicsCommandList;
struct ID3D12CommandList;
struct ID3D12CommandQueue;
struct D3D12_RESOURCE_BARRIER;

namespace stray_dlss::native::bbstate {

void configure(int level);
fseam::Level level();

// The swapchain-class candidate set: the REAL back buffers (present_owner, when FG is not armed)
// or FG's REPLACEMENTS (fg_present, at arm). Replaces the set; every state becomes unknown.
// `why` is logged. Also the membership test every engine-named back buffer is held to.
void set_candidates(const std::uint64_t *res, unsigned n, const char *why);
void clear_candidates(const char *why);
fseam::Candidates candidates();
bool is_candidate(std::uint64_t res);

// ---- from the native hooks, for GAME calls only (never under OwnCodeScope) ----
// OUR OWN LISTS ARE NOT THE ENGINE. The present owner and the FG present-twice path record
// their own transitions of the swapchain-class buffers (real ring and generated targets into and
// out of PRESENT) on lists created through the same hooked device, so without this the ledger's
// "last PRESENT witness" is as often FG's own copy as the engine's FD3D12Viewport::Present -
// measured 2026-09-05 as `idVsBarrier disagree=260` of 1011 against a mirror that agreed 1011/1011.
// Each creator registers its lists once; barriers, resets, executes and markers on them are
// skipped and counted (`ownSkipped=` on the [bbstate] line). UNCONFIRMED that this alone takes
// the witness to 0 disagreements; that is what the next box read decides.
void mark_own_list(ID3D12GraphicsCommandList *list);
void on_barriers(ID3D12GraphicsCommandList *list, unsigned n, const D3D12_RESOURCE_BARRIER *barriers);
void on_list_reset(ID3D12GraphicsCommandList *list);
void on_execute(unsigned n, ID3D12CommandList *const *lists);
// The HUD-less copy was recorded on `list` for `res`, assuming it sat in `assumed`; judged when
// the list executes.
void on_marker(ID3D12GraphicsCommandList *list, std::uint64_t res, std::uint32_t assumed);

// ---- queries ----
std::uint32_t state_of(std::uint64_t res, bool *known);
std::uint64_t last_present_resource();

// THE PRESENT-TIME DECISION for the NR stage: which StateBefore to name for `back_buffer`, given
// the constant the stage would otherwise assume. Counts the verdict; at level 2 returns the
// recorded state when known.
std::uint32_t present_state(std::uint64_t back_buffer, std::uint32_t assumed);

struct Stats
{
	std::uint64_t barriers_seen = 0;      // transition barriers on tracked resources (recording)
	std::uint64_t executes = 0;           // ExecuteCommandLists calls (game)
	std::uint64_t lists_executed = 0;
	std::uint64_t applied = 0;            // transitions replayed
	std::uint64_t before_mismatch = 0;    // the engine's StateBefore != the ledger's state
	std::uint64_t markers_ok = 0;
	std::uint64_t markers_bad = 0;
	std::uint64_t present_seen = 0;       // executes that left a tracked resource in PRESENT
	std::uint64_t verdict[static_cast<int>(fseam::StateVerdict::count)] = {};
	std::uint64_t used_recorded = 0;      // present_state() answered with the ledger (level 2)
	std::uint64_t used_assumed = 0;
	unsigned own_lists = 0;               // lists registered by mark_own_list (present owner + FG)
	std::uint64_t own_skipped = 0;        // barrier/reset/execute/marker calls ignored because the list is ours
	unsigned candidates = 0;
	std::uint64_t candidate_epoch = 0;
};
Stats stats();
int format_report(char *buffer, std::size_t size);
void log_report(const char *when);

} // namespace stray_dlss::native::bbstate
