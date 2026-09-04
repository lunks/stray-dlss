// The live half of src/core/view_cached.hpp: the View constant buffer from the engine's own
// `FViewInfo::CachedViewUniformShaderParameters`, read inside AddPasses and carried in the
// announcement, instead of searched for among the bound root CBVs at dispatch time.
//
// Why: the search takes a STALE RING COPY of the same view on 0.33% of claimed dispatches
// (facts §36.20), and no structural test can separate a stale copy from the current one. The
// CPU struct is per-FViewInfo and rebuilt per frame, so it cannot be a stale ring copy; and the
// search's own 2448-byte read is the validator that ties a candidate offset to a buffer the
// engine demonstrably bound (docs/RESEARCH-ENGINE-AWARE-REPLAN.md §2).
//
// WINDOWS-PORTABLE ONLY. `FViewInfo` is an engine object; nothing here touches vkd3d, DXVK or
// Wine, and nothing here maps a D3D12 resource - the pointee is ordinary heap memory.
//
// Guards, verbatim from L1: VirtualQuery before every read through the discovered offset, SEH
// around it, a fault latches the mechanism off for the session at ERROR naming the address, and
// every decline is a named, counted fallback to the search. A wrong answer is refused, never used.
#pragma once

#include "core/engine_seam.hpp"
#include "core/view_cached.hpp"
#include "core/view_params.hpp"
#include "engine_seam_hook.hpp"

#include <cstddef>
#include <cstdint>

namespace stray_dlss::vphook {

// [STRAYDLSS] EngineSeamViewParams (viewcached::Mode). Each level includes the ones below it.
//   0 — off. Nothing is scanned, nothing is read.
//   1 — DISCOVER: scan the FViewInfo inside every AddPasses, validate each candidate against the
//       announcement, compare the carried bytes against the search's at claim, latch, log. The
//       search still supplies the View to every consumer; the image is byte-identical.
//   2 — AUTHORITATIVE: once latched, the struct supplies the View; the search is the assertion
//       (byte-equality, one WARN per pass on disagreement, counted). `ambClaimed` goes to 0 by
//       construction because the search no longer chooses.
//   3 — EXCLUSIVE: the search is deleted. DECLARED, NOT BUILT - asking for it logs at ERROR and
//       runs level 2. A separate decision after level 2 runs clean in gameplay.
void configure(int level);
viewcached::Mode mode();

// Inside AddPasses, on the render thread, BEFORE the announcement is filed with the ledger: scan
// (or, once latched, verify) and fill the announcement's `view_*` fields. `depth_res` is the
// ID3D12Resource L1 resolved for this announcement (0 if none), used for prediction 5.
void scan_at_announce(const void *view, seam::Announcement &a, std::uint64_t depth_res);

// At CLAIM, on the recording thread, for a dispatch the engine announced. Joins what the
// announcement carried with what the search found for this dispatch, feeds the latch, and
// decides which View this dispatch uses. `search_prefix` may be null when the search found
// nothing (then nothing is compared).
struct ViewSource
{
	bool use_engine = false;        // the struct's View replaces the search's for this dispatch
	bool carried = false;           // the announcement carried a decoded View at all
	bool compared = false;          // both were present, so the bytes were compared
	bool bytes_equal = false;
	ue4::ViewParams params{};       // valid when use_engine
	std::uint32_t offset = 0;       // the offset the carried View came from
};
ViewSource resolve_at_claim(const seamhook::Verdict &v, bool search_ok,
                            const unsigned char *search_prefix, const ue4::ViewParams &search_view);

// One line for the periodic report and the status file:
//   viewParams: mode= latch= offset= latched= agree= disagree= preDisagree= uncompared=
//               unverified= ambiguous= empty= faults= off= used= fellBack=
int format_report(char *buffer, std::size_t size);
void log_report(const char *when);

} // namespace stray_dlss::vphook
