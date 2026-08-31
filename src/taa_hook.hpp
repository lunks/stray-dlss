// Identification of Stray's TAA dispatch, and the report that settles what it is bound to.
//
// Phase A: observe and log only. intercept_dispatch always returns false, so the engine's own
// TAA still runs and nothing about the image changes.
#pragma once

#include "core/taa_signature.hpp"
#include "core/view_params.hpp"

#include "reshade_all.hpp"

#include <cstdint>

namespace stray_dlss::taa_hook {

void set_pipeline_hash(uint64_t pipeline_handle, std::uint64_t hash);
void forget_pipeline(uint64_t pipeline_handle);
void set_bound_pipeline(reshade::api::command_list *cmd_list, uint64_t pipeline_handle);
void forget_command_list(reshade::api::command_list *cmd_list);

// Returns true to suppress the game's dispatch. Phase A always returns false.
bool intercept_dispatch(reshade::api::command_list *cmd_list, uint32_t x, uint32_t y, uint32_t z);

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

// Suppress the pinned pass without running DLSS, to establish whether that pass drives the
// visible image at all. An identical-looking DLSS frame is otherwise ambiguous.
// 0 = off, 1 = suppress only the pinned pass, 2 = suppress every structurally matched pass.
void set_ngx_dry_run(int mode);

// Suppress exactly one named pass and write nothing, regardless of the matcher's verdict. The
// pass that drives the picture is the one whose suppression changes the image.
void set_dry_run_hash(std::uint64_t hash);

// Name the pass DLSS replaces, overriding the history-round-trip heuristic — which is
// necessary but not sufficient, since several passes bind their own previous output.
void set_ngx_pass_hash(std::uint64_t hash);

// Alternate the named pass between suppressed and normal every N frames, so both states occur
// within one session and are seen against similar scene content. Transitions are logged.
void set_dry_run_alternate(std::uint32_t frames);
void note_present(std::uint64_t frame);

void resolve_counters(std::uint32_t &attempts, std::uint32_t &skipped_stale);

} // namespace stray_dlss::taa_hook
