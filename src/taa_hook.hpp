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

const Diagnostics &diagnostics();

// Dumps the per-shader outcome census. Call once, after gameplay has been running a while.
void dump_summary();

} // namespace stray_dlss::taa_hook
