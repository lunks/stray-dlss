// WHERE DLSS Neural Rendering runs — the second and third interception points.
//
// WHY THIS EXISTS. The TAA hook writes the engine's `u0`, and UE 4.27 makes that ONE resource
// serve two roles: `TemporalAA.cpp:696` is literally
// `NewHistoryTexture[0] = Outputs.SceneColor = NewHistoryTexture[0];`, and `:969` extracts the
// same texture as the next frame's history. So NR's answer re-enters the engine's temporal state
// every frame and compounds. MEASURED in the main menu with a static camera: mean brightness
// decays 82.9 -> 52.6 over ~30 s while high-frequency energy RISES, then snaps back; volumetric
// light shafts drain and wet-floor reflections fade. `NgxNR=0` removes it, `r.SSR.Quality=0` does
// not — there is no single conduit, so disabling consumers one at a time is not the fix.
//
// DLSS SR writing `u0` is correct and does not change: it is a like-for-like replacement for what
// TAA would have produced. Only NR is the problem, because it is an enhancement layered on top
// rather than a substitute.
//
// THE FIX IS PLACEMENT. On the desktop deferred path nothing after the tonemapper is carried into
// the next frame: every `QueueTextureExtraction` into `PrevFrameViewInfo` sits at
// `PostProcessing.cpp` 576/599/643 while `AddTonemapPass` is at 777. A post-tonemap hook has no
// feedback path BY CONSTRUCTION — not "we closed the known loops", but "there is no path". It also
// needs no pass identification: ReShade hands us the resource. Pass identification has cost this
// project more time than anything else and is the single biggest source of its past regressions.
//
// TWO COUPLED LOOPS, NOT ONE. Feature 18 keeps its own temporal accumulator (it consumes motion
// vectors and depth, and `DLSSNR.Reset` is `settings.resetAccumulation` in the reference), so the
// engine's loop was feeding NR's own history an input that already contained NR's previous output.
// Moving post-tonemap breaks the engine half and lets NR's accumulator converge against a stable
// input — which is why the move is expected to help more than a single-loop reading predicts.
//
// [STRAYDLSS] NgxNRHook selects between the three sites; `taa` is the default and is byte-
// identical to the shipped behaviour. The mode enum, the gate and the boundary rule are in
// src/core/nr_hook_plan.hpp, where CI can reach them.
#pragma once

#include "core/nr_hook_plan.hpp"
#include "reshade_all.hpp"

#include <cstdint>

struct ID3D12Resource;

namespace stray_dlss::nrhook {

// --- configuration, all read once at startup ---

void set_hook_mode(nrplan::HookMode mode);
nrplan::HookMode hook_mode();

// [STRAYDLSS] NgxNRPreUiBind, default 2. Which back-buffer render-target bind of the frame is the
// UI boundary; see nrplan::preui_boundary for the signal and what is UNCONFIRMED about it. The
// per-frame bind census is logged so one run settles this instead of a round trip per guess.
void set_preui_bind_ordinal(std::uint32_t ordinal);

// [STRAYDLSS] RestoreState, shared with the TAA path. Only consulted on `preui`, which records
// onto the GAME's command list; see nr_hook.cpp for why `present` deliberately restores nothing.
void set_restore_state(bool enabled);

// --- feeds ---

// Published by the TAA hook once per frame, immediately after a successful SR/RR evaluate, with
// the depth and motion vectors it just used. `frame` is the present index, so a post-tonemap
// trigger later in the SAME present sees an exact match and anything else is stale.
//
// `reset` is the camera-cut OR the TAA path already computes (CLAUDE.md §2.8) and MUST be carried:
// feature 18 keeps its own temporal history, and a cut that does not reset it is the "flicker
// between a frozen image and fog" class of bug this project has already been bitten by once.
void note_guides(std::uint64_t frame, ID3D12Resource *depth, ID3D12Resource *motion_vectors,
                 std::uint32_t render_width, std::uint32_t render_height, bool reset);

// The swapchain's back-buffer identities, cached so the `preui` trigger can answer "is this
// render target the back buffer?" without touching the swapchain from a recording thread.
void note_swapchain(reshade::api::swapchain *swapchain);
void forget_swapchain(reshade::api::swapchain *swapchain);

// Per-present boundary: resets the bind ordinal and the once-per-frame latch, retires staging
// allocations, and emits the periodic diagnostic.
void on_present(std::uint64_t frame);

// --- triggers ---

// addon_event::reshade_begin_effects. Runs on ReShade's OWN immediate command list, with the
// resource behind `rtv` in D3D12_RESOURCE_STATE_RENDER_TARGET (v6.8.0 runtime.cpp:745/4020).
void on_begin_effects(reshade::api::effect_runtime *runtime, reshade::api::command_list *cmd_list,
                      reshade::api::resource_view rtv, reshade::api::resource_view rtv_srgb);

// addon_event::bind_render_targets_and_depth_stencil. Runs on the GAME's command list, before the
// command. Fires NR on the configured back-buffer bind ordinal.
void on_bind_render_targets(reshade::api::command_list *cmd_list, std::uint32_t count,
                            const reshade::api::resource_view *rtvs,
                            reshade::api::resource_view dsv);

void shutdown();

// --- telemetry ---

struct Counters
{
	std::uint64_t triggered = 0; // times a trigger reached the gate
	std::uint64_t applied = 0;   // times the neural result actually reached the back buffer
	std::uint32_t reasons[nrplan::kPlanResultCount] = {};
	std::uint64_t begin_effects_seen = 0;   // reshade_begin_effects callbacks, ever
	std::uint32_t last_backbuffer_binds = 0; // back-buffer RTV binds in the previous frame
	std::uint32_t max_backbuffer_binds = 0;
	std::uint32_t frames_without_boundary = 0;
	std::uint64_t staging_bytes = 0;
};

Counters counters();

} // namespace stray_dlss::nrhook
