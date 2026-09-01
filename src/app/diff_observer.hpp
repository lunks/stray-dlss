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

struct Result
{
	// Per-slot verdicts, every one a line for the log on a disagreement.
	std::vector<std::string> mismatches; // both sides have the slot, values differ
	std::vector<std::string> unknown;    // the oracle has it, the native side does not
	std::vector<std::string> extra;      // the native side has it, the oracle does not
	bool agree() const { return mismatches.empty() && unknown.empty() && extra.empty(); }
};

// Pure. `expected` is the oracle's (ReShade's) resolve, `actual` the native backend's.
Result compare(const icept::DispatchBindings &expected, const icept::DispatchBindings &actual);

// --- the live machinery (Windows hosts only call these; the functions are still pure C++) ---

void set_enabled(bool enabled);
bool enabled();

// Called by the application after the driver's resolve of a dispatch it is about to let
// through: parks the oracle's answer for the native hook to consume.
void publish_expected(void *native_list, std::uint64_t shader_hash, std::uint32_t x, std::uint32_t y,
                      const icept::DispatchBindings &expected);
// Called by the native Dispatch hook: compares, counts, logs, and clears the slot. False if
// nothing was published for this list on this thread (the dispatch was not observed).
bool consume_and_compare(void *native_list, const icept::DispatchBindings &actual,
                         std::uint64_t native_unknown_lookups);

struct Summary
{
	std::uint64_t dispatches = 0;  // compared
	std::uint64_t agree = 0;
	std::uint64_t mismatch = 0;    // dispatches with >= 1 mismatch line
	std::uint64_t unknown = 0;     // dispatches with >= 1 unknown slot
	std::uint64_t extra = 0;
	std::uint64_t unconsumed = 0;  // published but never consumed (the dispatch was suppressed, or the hook never fired)
	std::uint64_t taa_dispatches = 0;  // of the compared, those whose hash is a known TAA permutation
	std::uint64_t taa_disagree = 0;
};
Summary summary();

// Logs the summary line and resets nothing (the counters are cumulative for the session).
void log_summary(const char *when);

} // namespace stray_dlss::diff
