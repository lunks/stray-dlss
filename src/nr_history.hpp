// END-OF-FRAME HISTORY RESTORE: keep DLSS Neural Rendering out of UE 4.27's temporal state
// without changing a single pixel anyone sees.
//
// THE PROBLEM, settled. At `NgxNRHook=taa` (the default, and the configuration whose image
// quality the user wants to keep) we intercept the TAA compute dispatch, run DLSS SR into its
// output `u0`, then run NR and write the decoded result back into `u0` in place. But `u0` is
// BOTH this frame's scene colour AND the next frame's temporal history — `TemporalAA.cpp:696` is
// literally `NewHistoryTexture[0] = Outputs.SceneColor = NewHistoryTexture[0];` and `:969`
// extracts that same texture into `OutputHistory->RT[i]`. One resource, two roles, so the engine
// cannot be handed a different image for history than for display at this hook point.
//
// The consumer that closes the loop is screen-space reflections:
// `ScreenSpaceRayTracing.cpp:596-620`, inside `RenderScreenSpaceReflections`, falls through to
// `InputColor = View.PrevViewInfo.TemporalAAHistory.RT[0]`. The user's symptom is the slow
// SSR-flavoured drift where reflections and fine detail fade over tens of seconds and recover.
//
// THE FIX, and why it needs no new hook site. SSR reads the history on the NEXT frame, and every
// SAME-frame consumer of `u0` has already run by the time the game calls Present. So:
//
//     TAA hook:  DLSS SR writes u0
//                -> snapshot()    CopyTextureRegion(scratch <- u0)   pristine, pre-NR
//                -> NR decode writes into u0 in place
//                ... the rest of the frame — post-process, tonemap, UI — uses the NR image ...
//     present:   restore()        CopyTextureRegion(u0 <- scratch)   history never saw NR
//
// The displayed frame is byte-identical to today's. Only what the engine carries forward changes.
//
// SCOPE, stated honestly. This closes loops whose consumer reads the TEXTURE next frame — the
// SSR/TemporalAAHistory conduit, i.e. the one the drift was traced to. It does NOT close a loop
// whose consumer read `u0` within the same frame and stored the result elsewhere: the
// eye-adaptation histogram (`PostProcessing.cpp:626-648`) downsamples the post-TAA scene colour
// during the frame and still sees the NR image. That coupling has its own fix already
// (`NgxNRTrackExposure`, ngx_nr.hpp).
//
// [STRAYDLSS] NgxNRRestoreHistory, **default OFF**, and a live overlay toggle so it can be A/B'd
// inside one session — which is how every other hypothesis in this project got settled. It is
// INERT at the post-tonemap sites (`present` / `preui`), where there is no feedback path to
// close by construction, and says so once in the log rather than pretending to work.
//
// WHY OFF BY DEFAULT, since the mechanism above is the fix for a real, measured bug. Two reasons,
// and the second is the decisive one:
//
//  1. As of 2026-09-01 THE FADE IS NO LONGER REPRODUCING on the user's machine. One of the day's
//     other changes — the ClipToPrevClip transposition fix, MVecScale returning to 1.0, the
//     create-site shape gate that cut feature creations from 11 to 1 (each of which was a full
//     history reset), or the exposure work — appears to have addressed it. Which one is unknown,
//     and for this module it does not matter.
//  2. The restore records a copy at PRESENT TIME, on a command list that is not the game's, into
//     a resource whose D3D12 state we can only INFER. That inference has four UE 4.27 source
//     anchors and zero measurements (kImageStateAtPresent in src/nr_history.cpp), and it is the
//     same class of assumption that made the `preui` hook site wreck a frame. Running it on every
//     frame to fix a problem that is not currently occurring is a bad trade even if the code is
//     right.
//
// So this ships as a TOOL with its diagnosis already written down — reach for it the moment the
// fade returns, rather than reimplementing the analysis. Turning it on is one ini key or one
// overlay click, and the startup log says so in both states.
//
// The decision logic is pure and lives in src/core/nr_history_plan.hpp, where CI reaches it.
#pragma once

#include "core/nr_history_plan.hpp"
#include "intercept/types.hpp"

#include <cstdint>

struct ID3D12Device;
struct ID3D12GraphicsCommandList;
struct ID3D12Resource;

namespace stray_dlss::nrhist {

// --- configuration ---

// [STRAYDLSS] NgxNRRestoreHistory. Live: the snapshot and the restore both consult it, and
// turning it off simply stops both, leaving the shipped (drifting) behaviour.
void set_enabled(bool enabled);
bool enabled();

// [STRAYDLSS] NgxNRHook, mirrored here so the gate can refuse `site-inert` by itself rather than
// making every call site re-derive it. Set once at startup, alongside nrhook::set_hook_mode.
void set_site(nrplan::HookMode site);

// [STRAYDLSS] NgxNRRestoreState — the D3D12_RESOURCE_STATES bitmask `u0` is ASSUMED to be in at
// present time, when the restore copy is recorded. Default 0xC0
// (NON_PIXEL_SHADER_RESOURCE | PIXEL_SHADER_RESOURCE); 0 means "keep the default".
//
// This is the one genuine hypothesis in this module and the reason it is a knob rather than a
// constant: D3D12 has no way to ASK what state a resource is in, the restore runs on a different
// command list from the snapshot, and the engine has transitioned `u0` several times in between.
// The default is derived from four UE 4.27 source anchors — see the block comment above
// kImageStateAtPresent in src/nr_history.cpp — and is logged in full on the first restore, so a
// wrong guess costs a config edit rather than a round trip.
void set_image_state_at_present(std::uint32_t state);
std::uint32_t image_state_at_present();

// --- the two halves ---

// Records the snapshot on the GAME's command list, inside the intercepted TAA dispatch.
//
// MUST be called after the SR/RR evaluate has written `image` and BEFORE nr::apply, and `image`
// MUST be the same resource and the same rect nr::apply will be given. `width`/`height` are the
// rect the NR decode writes (ApplyInputs::output_width/height), not the texture's allocation.
//
// RESOURCE STATE — HARD, not a guess: at this point `image` is `u0` in
// D3D12_RESOURCE_STATE_UNORDERED_ACCESS, because the SR/RR evaluate has just written it through
// a UAV. ngx_nr.cpp derives the identical constant for its own barriers (`image_state`, the
// codec branch) and ngx_nr.hpp's Site::taa_dispatch states it in prose. It is left in that state.
void snapshot(ID3D12Device *device, ID3D12GraphicsCommandList *cmd, ID3D12Resource *image,
              std::uint32_t width, std::uint32_t height);

// The verdict of the nr::apply that followed the snapshot. False — a warmup refusal, a
// validation refusal, a degenerate output, a codec failure — means `u0` was never modified and
// the restore is skipped as a pointless copy of identical pixels.
void note_nr_applied(bool applied);

// Records the restore, once per present, on the backend's present-time list
// (PresentContext::present_list — ReShade's own immediate command list today). Call from the
// application's present handler.
void on_present(const icept::PresentContext &pc);

void shutdown();

// --- telemetry ---

struct Counters
{
	std::uint64_t snapshots = 0;  // pristine copies taken
	std::uint64_t restores = 0;   // copies put back
	// Frames where NR modified `u0` and no pristine copy existed to put back, i.e. frames whose
	// residual DID reach the engine's history. Must stay 0; anything else is the bug returning.
	std::uint64_t harmful_misses = 0;
	// A second TAA dispatch in one frame overwrote the pending snapshot before it was restored.
	// Not expected in this title (DLSS is pinned to one pass) — counted so it cannot be silent.
	std::uint64_t overwritten = 0;
	std::uint32_t snapshot_reasons[histplan::kStepCount] = {};
	std::uint32_t restore_reasons[histplan::kStepCount] = {};
	// Allocation accounting, mirroring nrp::Stats and gbr::Stats — the churn that OOM'd the GPU
	// once must stay measurable.
	std::uint64_t scratch_created = 0;
	std::uint64_t scratch_retired = 0;
	std::uint64_t scratch_released = 0;
	std::uint32_t live_retired = 0;
	std::uint64_t bytes_live = 0;
};

Counters counters();

} // namespace stray_dlss::nrhist
