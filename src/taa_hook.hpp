// Identification of Stray's TAA dispatch, and the report that settles what it is bound to.
//
// Phase A: observe and log only. intercept_dispatch always returns false, so the engine's own
// TAA still runs and nothing about the image changes.
#pragma once

#include "core/rr_guides.hpp"
#include "core/taa_signature.hpp"
#include "core/view_params.hpp"

#include "intercept/types.hpp"

#include <cstddef>
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
// DLSS RAY RECONSTRUCTION, [STRAYDLSS] NgxRR: 0 off (default), 1 = probe DLSSD's existence on
// this stack, 2 = evaluate RR in place of SR with a per-frame SR fallback.
//
// ITS GUIDE SOURCE IS THE ENGINE'S OWN NAME, and that is the whole of what changed on
// 2026-09-04. The heuristic finder that used to identify GBufferA-E by descriptor SHAPE was
// deleted 2026-09-03 (docs/RESEARCH-ENGINE-TAA-HOOK.md §13) and must not come back; the
// replacement is FRenderTargetPool::FindFreeElement's own `const TCHAR* InDebugName` argument,
// read by the forwarding recorder (src/pool_name_hook.hpp, [STRAYDLSS] PoolNames=3) and turned
// into the four RR guides by src/gbuffer_resolve.hpp using NVIDIA's own UE-plugin recipe.
//
// EVERY REFUSAL IS COUNTED AND NAMED (rr_reason_counters below, the [rr] line). A frame that
// falls back to SR because the guide set was not usable must never be indistinguishable from a
// frame RR was not asked about — that rule has already cost this project one round trip on the
// SR path and it applies verbatim here.
//
// AND THE PRIOR QUESTION IS STILL OPEN: docs/RESEARCH-RR-REFLECTION-DENOISE.md argues RR may
// have no denoising job left in this title (r.RayTracing=False, r.SSGI.Enable=0, and Stray's own
// shipped r.SSR.Temporal=1 already running a temporal filter over SSR before the composite), and
// that reflections are the content RR is least equipped for because our motion vectors describe
// the surface, not the reflected geometry. One launch with DumpShaders=1 settles it (§4). RR
// running is not RR being worth running.
void set_ngx_rr(int mode);

// RR telemetry. `evaluates` counts frames Ray Reconstruction carried; `fallbacks` counts frames
// it was asked for and SR carried instead. Their sum is the number of frames RR was ASKED about,
// which is the denominator every rate here should be read against.
void rr_counters(std::uint32_t &evaluates, std::uint32_t &fallbacks);

// Why the fallbacks happened. The first rrguides::kRefusalCount entries are the guide-set
// verdict (rrguides::refusal_name); the rest are the evaluate-side reasons below.
constexpr std::size_t kRrGuideReasonCount = rrguides::kRefusalCount;
constexpr std::size_t kRrReasonCount = kRrGuideReasonCount + 5;
const char *rr_reason_name(std::size_t index);
void rr_reason_counters(std::uint32_t out[kRrReasonCount]);


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
// `wrong_view` counts candidates that decoded as A View buffer but described a DIFFERENT view
// than the dispatch - the search skipping past them instead of stopping. Non-zero is the fix for
// facts §36.18 working; each one is a frame that used to lose DLSS SR entirely.
// `suspect_small` is the SAME skip for the opposite failure and it IS gated now (facts §36.20,
// report §16.5): a candidate below the engine's own kMinTAAUpsampleResolutionFraction of the
// dispatch is an impostor too SMALL for `view_fits_dispatch` to catch, and it used to WIN the
// search from a lower root parameter - which created DLSS features at 64x41 -> 3840x2160 and put
// a magnified corner of the frame on screen. Each count is now the search rejecting one and
// continuing to the real view, not a frame we got wrong.
// amb_claimed / amb_other count dispatches where a SECOND surviving View would have given
// DIFFERENT ClipToPrevClip / jitter / CameraCut - i.e. the slot-order search was guessing rather
// than having its answer forced. Split by whether the ENGINE claimed the dispatch, because
// look-alikes outnumber real upscales here and an undifferentiated count cannot answer the only
// question that matters: was DLSS SR itself ever fed a view we had to guess at?
// [STRAYDLSS] NgxLetterboxHold (default ON). When a scripted transition shrinks the view rect,
// evaluate at the LIVE feature's extent instead of rebuilding or declining - which is what keeps
// DLSS SR, and therefore NR, running through the slide. Off restores the decline.
void set_letterbox_hold(bool on);

// `held` counts frames that took that path. `refused_by_reason` is indexed by
// `core::HoldRefusal` and says why the others did not - read `originMoved` first, because a
// non-zero count there means this title does NOT anchor its shrinking rect at the top left and
// the whole mechanism is wrong for it.
void hold_counters(std::uint64_t &held, std::uint64_t *refused_by_reason, std::size_t count);

void view_row135_counters(std::uint64_t &ok, std::uint64_t &bad, std::uint64_t &wrong_view,
                          std::uint64_t &suspect_small, std::uint64_t &amb_claimed,
                          std::uint64_t &amb_other);

// WHAT THE VIEW-CB SEARCH ITSELF COSTS: every bound root CBV it TRIED, one describe_resource and
// one 2448-byte buffer read each. Divided by the dispatch count it is candidates-per-dispatch,
// and it is the only part of the descriptor machinery that replacing the search with identity
// from the engine would actually retire. **It bounds that saving from above, and the bound is
// low** — the shadow's expensive half is `shadow-copy`, the SRV/UAV table walk that
// colour-by-register and the `u0` output still require whatever happens to the CB search.
// docs/RESEARCH-ENGINE-TAA-HOOK.md §15.4 has the arithmetic.
std::uint64_t view_cb_read_count();

} // namespace stray_dlss::taa_hook
