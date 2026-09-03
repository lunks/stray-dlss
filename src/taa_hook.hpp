// Identification of Stray's TAA dispatch, and the report that settles what it is bound to.
//
// Phase A: observe and log only. intercept_dispatch always returns false, so the engine's own
// TAA still runs and nothing about the image changes.
#pragma once

#include "core/taa_signature.hpp"
#include "core/view_params.hpp"

#include "intercept/types.hpp"

#include <cstdint>

namespace stray_dlss::taa_hook {

void set_pipeline_hash(uint64_t pipeline_handle, std::uint64_t hash);
void forget_pipeline(uint64_t pipeline_handle);
void set_bound_pipeline(const icept::CommandContext &ctx, uint64_t pipeline_handle);
void forget_command_list(const icept::CommandContext &ctx);

// Returns true to suppress the game's dispatch. Phase A always returns false.
bool intercept_dispatch(const icept::CommandContext &ctx, uint32_t x, uint32_t y, uint32_t z);

// True once this hash has demonstrated the history round-trip: UE4 bound its previous u0
// back as one of its SRVs on a later frame (CLAUDE.md §2.9). Accumulates over the session.
// Consumed by the pass finder's dataflow walk as one half of its owns_history oracle.
bool owns_temporal_history(std::uint64_t hash);

// Counters that localise WHY a dispatch was not reported. Each is a distinct early exit, so
// one glance at the status file says which stage is failing instead of costing a round trip.
struct Diagnostics
{
	std::uint64_t large_dispatches = 0;   // passed the size filter
	std::uint64_t no_bound_pipeline = 0;  // no pipeline recorded for this command list
	std::uint64_t no_hash = 0;            // bound pipeline was never hashed
	std::uint64_t resolve_failed = 0;     // descriptor resolve produced nothing
	std::uint64_t candidates_reported = 0;
	std::uint64_t best_hash = 0;
	std::uint32_t best_width = 0;
	std::uint32_t best_height = 0;
	bool view_seen = false;
};

void configure(bool mv_resolve_enabled, bool restore_heaps, bool restore_state, int dispatch_mode);

const Diagnostics &diagnostics();

// Dumps the per-shader outcome census. Call once, after gameplay has been running a while.
void dump_summary();

// How many times the resolve was attempted, and how many of those were skipped because
// ReShade reported a resource that is no longer live.
// [STRAYDLSS] NgxEvaluate. Separate from EnableNGX, which only brings NGX up: this is the
// first switch that changes what the player sees, so it is opt-in on its own.
void set_ngx_evaluate(bool enabled);
// [STRAYDLSS] StageFile, default OFF. Enables the per-dispatch crash breadcrumb
// (stray-dlss-stage.txt, taa_hook.cpp's mark()) — six file writes per dispatch, so it is a
// crash-naming tool to switch on deliberately, never something to leave running.
void set_stage_file(bool enabled);
// DLSS RAY RECONSTRUCTION IS NOT WIRED UNDER THIS HOST. The heuristic G-buffer finder and
// the guide-resolve pass that fed it were deleted 2026-09-03 (docs/RESEARCH-ENGINE-TAA-HOOK.md
// §13): they identified GBufferA-E by descriptor SHAPE, which is the same class of guessing the
// engine seam replaced for the TAA pass, and nothing on the SR, NR or FG path referenced them.
// The NGX side survives byte-identical in src/ngx_backend.{hpp,cpp} — `ensure_feature_rr`,
// `evaluate_rr`, `release_feature_rr` all take raw ID3D12Resource* and never named the finder.
// What RR needs to come back is a GUIDE SOURCE, and the intended one is the named RDG G-buffer
// textures reachable from the `const FViewInfo&` that `ITemporalUpscaler::AddPasses` already
// hands us — identity from the engine, exactly as L1 does for depth and velocity.
// `[STRAYDLSS] NgxRR` is refused loudly at startup (src/app/dlss_app.cpp) rather than silently
// doing nothing.


// Suppress the pinned pass without running DLSS, to establish whether that pass drives the
// visible image at all. An identical-looking DLSS frame is otherwise ambiguous.
// 0 = off, 1 = suppress only the pinned pass, 2 = suppress every structurally matched pass.
void set_ngx_dry_run(int mode);

// Suppress exactly one named pass and write nothing, regardless of the matcher's verdict. The
// pass that drives the picture is the one whose suppression changes the image.
// Comma-list capable: suppress every named pass (max 16). Empty count disables.
void set_dry_run_hashes(const std::uint64_t *hashes, std::size_t count);

// Name the pass DLSS replaces, overriding the history-round-trip heuristic — which is
// necessary but not sufficient, since several passes bind their own previous output.
void set_ngx_pass_hash(std::uint64_t hash);

// Pass InReset=1 every frame, so DLSS ignores history and motion vectors. Bisects the temporal
// half of DLSS away from the spatial half.
// Clear the captured output to magenta instead of evaluating: proves whether the output handle
// is the buffer the frame actually displays.
void set_ngx_paint(bool enabled);

// Alternate the named pass between suppressed and normal every N frames, so both states occur
// within one session and are seen against similar scene content. Transitions are logged.
void set_dry_run_alternate(std::uint32_t frames);
void note_present(std::uint64_t frame);

// seen / reached-phase-B / passed-live-and-cut / recorded / evaluated, for the named pass.
void named_pass_counters(std::uint32_t out[5]);

void resolve_counters(std::uint32_t &attempts, std::uint32_t &skipped_stale);

// Row 135's self-check, tallied over every frame that located a View CB. The buffer is found by
// SEARCH, and `view_params_plausible` is a shape test the WRONG buffer can satisfy — so this is
// the number that says whether the search is right. Row 135 must read (denormal, P, 1/P, 0.0);
// `y*z == 1.0` is true by construction (SceneRendering.cpp:1563-1564) and cannot survive a wrong
// buffer or a slipped offset. A `bad` rate near 100% means the CB search is what to fix, and
// that every jitter / ClipToPrevClip / CameraCut we have fed a temporal consumer is suspect.
void view_row135_counters(std::uint64_t &ok, std::uint64_t &bad);

} // namespace stray_dlss::taa_hook
