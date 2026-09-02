// The differential observer: the native backend's answer for a dispatch, diffed against the
// ReShade backend's — the known-good oracle — before the native backend is ever allowed to
// drive (plan Task 15, assessment §8.1). The comparison is pure and CI-tested; the publish/
// consume pair is a thread-local slot keyed by the native command list, because the native
// Dispatch hook fires INSIDE ReShade's forward of the very call the ReShade backend just
// resolved, on the same thread.
#pragma once

#include "intercept/types.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace stray_dlss::diff {

// Which side a differing slot convicts. The two trackers each carry their own liveness —
// ReShade's from its init/destroy_resource events, the native registry's from a destruction
// sentinel on the resource itself — and a resource one side names while the OTHER side's
// liveness says it is dead is that side's stale map, not a real binding (CLAUDE.md §5: ReShade's
// view->resource map is never cleared on D3D12). Everything else stays ambiguous and is said so.
enum class Verdict : std::uint8_t
{
	reshade_stale,      // the oracle named a resource its own liveness says is dead, OR one the native
	                    // shadow saw DIE after that very slot was written (tombstone; address reused)
	reshade_copy_stale, // the slot was COPIED; ReShade's own view map for the copy's SOURCE agrees
	                    // with the native answer, its online tracking does not: its copy bookkeeping
	reshade_view_recreated, // the slot was COPIED from an offline view that the game RE-CREATED for
	                    // another resource AFTER the copy; ReShade reports the re-created view's
	                    // resource for the online slot, whose descriptor bytes still hold the old one
	reshade_liveness_gap, // the native side names a resource whose sentinel is LIVE (the runtime
	                    // would have released it otherwise) but ReShade's event-fed liveness calls
	                    // dead: its init/destroy_resource events never carried it (measured: the
	                    // swapchain's own back buffers, run J)
	native_blind,       // the oracle names a live resource the registry never saw, or a slot the
	                    // shadow never saw written since attach (no entry / no heap span)
	liveness_conflict,  // the two liveness trackers disagree about the same resource
	native_missed,      // the oracle names a resource live on both sides; the native walk has no such slot
	oracle_missed,      // the native side names a live resource; the oracle has nothing there
	both_live,          // both sides name a resource live on both sides and they differ: ambiguous
	unadjudicated,      // no liveness available (the pure comparison alone)
};
constexpr int kVerdictCount = 10;
const char *verdict_name(Verdict v);

// The evidence compare() may ask for. Any of the first three null makes every slot
// `unadjudicated`; the last two refine mismatches and unknowns when present.
struct Adjudicator
{
	bool (*oracle_live)(icept::ResourceId) = nullptr; // ReShade's own is_resource_live
	bool (*native_live)(icept::ResourceId) = nullptr; // the registry's is_live (sentinel-backed)
	// Whether the registry EVER registered it this life: tells "died, per the sentinel" apart
	// from "never seen" (created before attach or through an unhooked entry point).
	bool (*native_seen)(icept::ResourceId) = nullptr;
	// ReShade's OWN view->resource map for a view handle (0 if unknown) — asked about the
	// SOURCE slot of a copy the native shadow recorded, so ReShade can be checked against itself.
	icept::ResourceId (*oracle_view_resource)(icept::DescriptorId view) = nullptr;
	// The native shadow's whole entry for a slot: what it holds, at which write sequence, whether
	// it was copied and from where, and whether that resource has since died (tombstone).
	bool (*native_slot)(icept::DescriptorId slot, icept::ResourceId &res, std::uint64_t &seq, bool &via_copy,
	                    icept::DescriptorId &src, bool &dead) = nullptr;
};

struct Result
{
	// Per-slot verdicts, every one a line for the log on a disagreement.
	std::vector<std::string> mismatches; // both sides have the slot, values differ
	std::vector<std::string> unknown;    // the oracle has it, the native side does not
	std::vector<std::string> extra;      // the native side has it, the oracle does not
	// Heap identities that differ. NOT a disagreement: under ReShade the oracle names its
	// PROXY heap object and the native side the real one beneath (measured 2026-09-01:
	// every TAA dispatch agreed on every register and differed only here). Reported so a
	// real heap difference is still visible.
	std::vector<std::string> heap_identity;
	// Per differing slot (mismatch, unknown, extra), which side it convicts.
	std::uint32_t verdicts[kVerdictCount] = {};
	bool agree() const { return mismatches.empty() && unknown.empty() && extra.empty(); }
	// A disagreement whose every differing slot convicts ReShade: the native side is right.
	bool oracle_wrong() const;
};

// Pure. `expected` is the oracle's (ReShade's) resolve, `actual` the native backend's. With an
// Adjudicator every differing line carries both liveness answers and its verdict.
Result compare(const icept::DispatchBindings &expected, const icept::DispatchBindings &actual,
               const Adjudicator *adj = nullptr);

// --- the live machinery (Windows hosts only call these; the functions are still pure C++) ---

void set_enabled(bool enabled);
bool enabled();

// Called by the application after the driver's resolve of a dispatch it is about to let
// through: parks the oracle's answer for the native hook to consume.
void publish_expected(void *native_list, std::uint64_t shader_hash, std::uint32_t x, std::uint32_t y,
                      const icept::DispatchBindings &expected);
// Whether an expectation is parked for this list on this thread — the native Dispatch hook's
// cheap pre-check before it resolves anything.
bool has_expected(void *native_list);
// Called by the native Dispatch hook: compares, counts, logs, and clears the slot. False if
// nothing was published for this list on this thread (the dispatch was not observed).
// `native_note` is appended to the DIFF line (the native side's raw counts: what its root
// shadow held), so a wholesale absence reads differently from a per-slot difference.
bool consume_and_compare(void *native_list, const icept::DispatchBindings &actual,
                         std::uint64_t native_unknown_lookups, const Adjudicator *adj = nullptr,
                         const char *native_note = nullptr);

struct Summary
{
	std::uint64_t dispatches = 0;  // compared
	std::uint64_t agree = 0;
	std::uint64_t mismatch = 0;    // dispatches with >= 1 mismatch line
	std::uint64_t unknown = 0;     // dispatches with >= 1 unknown slot
	std::uint64_t extra = 0;
	std::uint64_t heap_identity = 0; // dispatches whose only difference was heap identity
	std::uint64_t unconsumed = 0;  // published but never consumed (the dispatch was suppressed, or the hook never fired)
	std::uint64_t taa_dispatches = 0;  // of the compared, those whose hash is a known TAA permutation
	std::uint64_t taa_disagree = 0;
	// Differing SLOTS by verdict, cumulative (a dispatch with 31 unknown cbs contributes 31).
	std::uint64_t verdicts[kVerdictCount] = {};
	// Disagreeing DISPATCHES by adjudication: every differing slot convicts ReShade
	// (oracle_wrong), or at least one slot is unresolved / convicts the native side.
	std::uint64_t dispatches_oracle_wrong = 0;
	std::uint64_t dispatches_unresolved = 0;
	std::uint64_t taa_dispatches_unresolved = 0;
};
Summary summary();

// Logs the summary line and resets nothing (the counters are cumulative for the session).
void log_summary(const char *when);

} // namespace stray_dlss::diff
