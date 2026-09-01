// Whether a frame should have its pristine (pre-Neural-Rendering) colour snapshotted, and
// whether that snapshot should be put back at the end of the frame.
//
// WHY THIS EXISTS AT ALL. At `NgxNRHook=taa` we write DLSS Neural Rendering's answer into the
// engine's `u0`, and UE 4.27 makes that ONE resource serve two roles:
// `TemporalAA.cpp:696` is literally `NewHistoryTexture[0] = Outputs.SceneColor =
// NewHistoryTexture[0];`, and `:969` extracts that same texture into `OutputHistory->RT[i]`.
// So NR's residual re-enters the engine's temporal state every frame and compounds — the
// measured slow drift where reflections and fine detail fade over tens of seconds and recover.
//
// The consumer that closes the loop is SCREEN-SPACE REFLECTIONS:
// `ScreenSpaceRayTracing.cpp:596-620`, inside `RenderScreenSpaceReflections`, falls through to
// `InputColor = View.PrevViewInfo.TemporalAAHistory.RT[0]` (the half-res branch needs
// `GSSRHalfResSceneColor`, and the live Engine.ini sets `r.SSR.HalfResSceneColor=0`, so the
// full-res branch is taken). That is exactly the resource our decode writes.
//
// AND SSR READS IT ON THE **NEXT** FRAME. Every same-frame consumer of `u0` — the post chain,
// the tonemapper, the composite, Slate — has already run by the time the game calls Present. So
// the fix needs no new hook site and no change to the image anyone sees: snapshot the pristine
// image before the NR decode, and put it back at end of frame.
//
//     TAA hook:  DLSS SR writes u0
//                -> snapshot(u0 -> scratch)          pristine, pre-NR
//                -> NR decode writes into u0 in place
//                ... the rest of the frame uses the NR image ...
//     present:   restore(scratch -> u0)              the engine's history never saw NR
//
// WHAT THIS DOES NOT CLOSE, stated so the scope is honest: loops whose consumer read `u0`
// WITHIN the same frame and squirreled the result away in a different texture. The eye-adaptation
// histogram (`PostProcessing.cpp:626-648`) downsamples the post-TAA scene colour into its own
// buffer during the frame, so it still sees the NR image; that coupling is addressed separately
// by `NgxNRTrackExposure` (see ngx_nr.hpp). Only resources the engine carries FORWARD as the
// texture itself are fixed here — which is the SSR/history conduit, i.e. the one that matters.
//
// The decision is pure numbers, so it lives here where CI can prove the refusal rules instead of
// the user's machine discovering them (CLAUDE.md §0.4). src/nr_history.cpp does nothing but
// gather the numbers from D3D12 and act on the verdict.
#pragma once

#include "core/nr_hook_plan.hpp"

#include <cstdint>

namespace stray_dlss::histplan {

// Why a frame was, or was not, snapshotted / restored. Every one is counted and named, for the
// same reason the TAA path's gate refusals and the NR hook's plan results are: a pass that
// silently does nothing must be impossible to confuse with a pass that is not there.
enum class Step
{
	ok = 0,
	// [STRAYDLSS] NgxNRRestoreHistory=0 — THE DEFAULT. See Config below for why.
	disabled,
	// The hook site is post-tonemap (`present` / `preui`). There is NO feedback path to close
	// there — on the desktop deferred path every QueueTextureExtraction into PrevFrameViewInfo
	// sits at PostProcessing.cpp 576/599/643 while AddTonemapPass is at 777 — so this whole
	// mechanism is INERT rather than merely unnecessary, and says so once in the log.
	site_inert,
	// NgxNR=0. DLSS SR alone is a like-for-like replacement for what the engine's own TAA would
	// have produced, so nothing needs undoing; there is no residual in `u0`.
	nr_disabled,
	// NR cannot apply yet (still in warmup, still validating, or validated as degenerate), so
	// `u0` is the SR/RR image untouched and copying it costs 66 MB a frame for nothing. This is
	// the gate that keeps a permanently-refusing NR from paying the snapshot forever.
	nr_cannot_apply,
	// The TAA hook handed us no colour resource.
	no_image,
	// A zero-sized rect, which is what a mid-resize or minimised frame looks like.
	zero_extent,
	// Mipped, arrayed or multisampled. Deliberately the SAME refusal ngx_nr's `mipped-input`
	// makes on the identical resource, so the two agree: if this fires, NR refused too and there
	// is genuinely nothing to restore. Named separately anyway, because "NR was refused" and
	// "NR ran and we could not undo it" want completely different responses.
	unsupported_image,
	// Our scratch texture could not be created. LOUD: without it NR's residual reaches the
	// engine's history and the user gets the drift back with no explanation.
	alloc_failed,
	// Restore only: nothing was captured this frame, so there is nothing to put back. Benign on
	// a frame where the snapshot itself refused for one of the reasons above (which was counted
	// there); a WARNING when NR nonetheless applied, because that is a frame whose residual DID
	// reach the history.
	no_snapshot,
	// Restore only: NR refused this frame, so `u0` still holds the SR/RR image byte for byte and
	// putting the snapshot back would be a pointless 66 MB copy of identical pixels. This is the
	// steady state of a session with NgxNR off, so it is counted but never warned about.
	nr_not_applied,
	// Restore only: the present-time queue offered no immediate command list to record on. Its
	// own name because it is a ReShade-side failure rather than anything about this frame, and
	// because it should be impossible: d3d12_impl_command_queue.cpp:20 creates one for every
	// DIRECT queue, and the swapchain's present queue is DIRECT by construction.
	no_command_list,
};

// NOTE: the count is duplicated in src/core/nr_history_plan.cpp's kStepNames — change both
// together. Same discipline as nrplan::kPlanResultCount and nr::kNrRefusalNames.
constexpr int kStepCount = 12;
const char *step_name(Step step);

// The two settings that gate the whole mechanism. `site` is read once at startup from
// NgxNRHook; `enabled` is the live toggle.
//
// DEFAULT OFF, and that is a deliberate risk judgement rather than caution for its own sake.
// The restore records a copy at PRESENT TIME, on a command list that is not the game's, into a
// resource whose D3D12 state we can only INFER (see kImageStateAtPresent in src/nr_history.cpp
// — four UE 4.27 source anchors, zero measurements). That is the same class of assumption that
// made the `preui` hook site wreck a frame. And as of 2026-09-01 the SSR fade this exists to fix
// is no longer reproducing on the user's machine — one of the day's other changes
// (ClipToPrevClip transposition, MVecScale back to 1.0, the create-site shape gate that cut
// feature creations from 11 to 1, or the exposure work) appears to have addressed it, and which
// one is unknown. Running an unverified per-frame state assumption to fix a problem that is not
// currently occurring is a bad trade even when the code is right.
//
// So this ships as a TOOL with its diagnosis already written down, reachable the moment the fade
// returns, rather than as a behaviour change nobody asked for. The startup log says so plainly
// so a future session finds it instead of reimplementing it.
struct Config
{
	bool enabled = false;                                       // [STRAYDLSS] NgxNRRestoreHistory
	nrplan::HookMode site = nrplan::HookMode::taa;              // [STRAYDLSS] NgxNRHook
};

// What the TAA hook knows at the moment DLSS SR/RR has written `u0` and NR has not yet run.
struct SnapshotInputs
{
	// nr::enabled() — the NgxNR master switch, which is also a live overlay toggle.
	bool nr_enabled = false;
	// Whether NR could replace the image on THIS frame: `nr::enabled() && nr::validated()`.
	//
	// Validation flips inside on_present (the readback drain), never inside apply(), so by the
	// time a frame's apply() can succeed this has ALREADY been true for at least one present.
	// That makes it exact rather than one-frame-late: there is no frame where NR applies and we
	// declined to snapshot. The restore path still counts `no_snapshot` if that ever stops being
	// true, so a future reordering shows up as a warning rather than as returning drift.
	bool nr_can_apply = false;
	bool have_image = false;
	// The rect the NR decode will actually write: ngx_nr's `cw`/`ch`, i.e. ApplyInputs::
	// output_width/height, which taa_hook derives from the dispatch's own group count rather
	// than from the texture's allocation (the GetOutputExtent Max() lesson, CLAUDE.md §5).
	// Snapshotting exactly that rect is both correct and cheaper than the whole allocation.
	std::uint32_t width = 0;
	std::uint32_t height = 0;
	// Plain single-mip, single-slice, non-MSAA — the same three the neural runtime requires.
	std::uint32_t mip_levels = 1;
	std::uint32_t array_size = 1;
	std::uint32_t sample_count = 1;
};

Step plan_snapshot(const Config &config, const SnapshotInputs &in);

// What the present-time trigger knows.
struct RestoreInputs
{
	// A snapshot was successfully recorded on the game's command list this frame.
	bool have_snapshot = false;
	// nr::apply() returned true, i.e. the engine's `u0` really was modified. A refusal (warmup,
	// validating, degenerate, a codec failure) leaves `u0` exactly as SR/RR wrote it.
	bool nr_applied = false;
};

Step plan_restore(const Config &config, const RestoreInputs &in);

// TRUE when a restore was skipped on a frame whose history genuinely was polluted — NR modified
// `u0` and we have no pristine copy to put back. Separated from plan_restore's verdict because
// it is the only `no_snapshot` that deserves a warning: every other one is a frame where the
// snapshot refused for a reason already counted at the snapshot site.
bool restore_miss_is_harmful(const Config &config, const RestoreInputs &in);

} // namespace stray_dlss::histplan
