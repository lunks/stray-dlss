#include "taa_hook.hpp"

#include "gbuffer_finder.hpp"
#include "gbuffer_resolve.hpp"
#include "ngx_nr.hpp"
#include "nr_history.hpp"
#include "nr_hook.hpp"
#include "perf.hpp"

#include "intercept/backend.hpp"
#include "log.hpp"
#include "input_dump.hpp"
#include "mv_resolve.hpp"
#include "ngx_backend.hpp"


#include <d3d12.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <mutex>
#include <unordered_map>

namespace stray_dlss::taa_hook {

using icept::DispatchBindings;
namespace {

std::mutex g_mutex;
std::unordered_map<uint64_t, std::uint64_t> g_pipeline_hashes;                  // pipeline -> DXBC hash
std::unordered_map<ID3D12GraphicsCommandList *, uint64_t> g_bound;             // cmd list -> pipeline
std::unordered_map<std::uint64_t, std::uint64_t> g_prev_output;                 // hash -> its u0 last frame
std::unordered_map<std::uint64_t, std::uint32_t> g_report_count;                // hash -> reports emitted
std::unordered_map<std::uint64_t, bool> g_steady_reported;                      // hash -> saw a non-cut frame
std::unordered_map<std::uint64_t, bool> g_roundtrip_logged;                     // hash -> round-trip already noted
bool g_resolve_ran = false;
bool g_resolve_failed_logged = false;
bool g_stale_resource_logged = false;
// How often the resolve is attempted versus skipped because ReShade named a dead resource.
// A skip is safe but not free: that frame contributes no motion vectors, and a high rate would
// mean our view->resource lookups are unreliable in general, not just occasionally.
std::atomic<std::uint32_t> g_resolve_attempts{ 0 };
std::atomic<std::uint32_t> g_resolve_skipped_stale{ 0 };
bool g_render_size_logged = false;
// [STRAYDLSS] NgxEvaluate. Off by default: this is the first switch that can change what the
// player sees, so it is opt-in and separate from EnableNGX (which only brings NGX up).
bool g_ngx_evaluate = false;
// [STRAYDLSS] NgxRR (taa_hook.hpp). Mode 2 turns the evaluate below RR-first.
std::atomic<int> g_ngx_rr_mode{ 0 };
std::atomic<std::uint32_t> g_rr_evaluates{ 0 };
std::atomic<std::uint32_t> g_rr_fallbacks{ 0 };
// Refusal reasons, indexed per kRrRefusalNames (taa_hook.hpp). Bumped by try_evaluate_rr
// on every false return so the periodic line can say WHY frames fell back to SR.
enum RrRefusalIndex
{
	kRrNotArmed = 0,
	kRrNoCandidate = 1,
	kRrStaleBind = 2,
	kRrRolesMissing = 3,
	kRrLiveness = 4,
	kRrRowsImplausible = 5,
	kRrResolveFailed = 6,
	kRrCreateFailed = 7,
	kRrEvaluateFailed = 8,
	kRrGuidesStale = 9, // GBufferResolveAt=ssd: no guide record landed this frame
	kRrResolveOnly = 10, // GBufferResolveOnly=1: the evaluate is deliberately skipped
};
std::atomic<std::uint32_t> g_rr_refusals[kRrRefusalCount] = {};
// Successful gbuffer_resolve records under RR — the guide-dump key. Keyed on RECORDS, not
// the shared evaluate-attempt counter: a session where create/evaluate fails still
// resolves guides, and they must still dump (the whole point of the offline B/C check).
std::atomic<std::uint64_t> g_rr_records{ 0 };

// RR-1 ([STRAYDLSS] NgxRR=3): SSD temporal-accumulation suppression, COUPLED to RR success.
// The SSD dispatch we suppress runs BEFORE the TAA-point RR evaluate, so we cannot know at
// suppression time whether RR will succeed THIS frame. Resolution: an ARMED latch, set only
// after RR has evaluated successfully for kRr1ArmFrames consecutive frames and cleared on any
// RR failure/fallback (updated at each TAA evaluate). The SSD suppression reads that latch —
// i.e. it acts on the PREVIOUS frame's RR outcome, an excellent predictor since armed RR is
// ~100% stable. A rare post-arm failure costs a single frame of noisy SR and self-corrects
// (the failure disarms, so the next frame's SSD is not suppressed). Suppression additionally
// requires the guides to have been captured THIS frame (the frame RR will consume), so a
// frame RR cannot carry never loses its denoiser. Warm-up (before arming) runs as RR-0.
constexpr std::uint32_t kRr1ArmFrames = 30;
std::atomic<bool> g_rr1_armed{ false };
std::atomic<std::uint32_t> g_rr_success_streak{ 0 };
std::atomic<std::uint64_t> g_rr1_suppressed_total{ 0 };
std::atomic<std::uint32_t> g_rr1_suppressed_this_frame{ 0 };
std::atomic<std::uint32_t> g_rr1_last_frame_suppressed{ 0 };
// [STRAYDLSS] NgxDryRun. Suppresses the pinned pass's engine dispatch WITHOUT running DLSS, so
// nothing writes u0 at all.
//
// This is the experiment that decides whether our output can reach the screen. If the image
// visibly breaks, the pass we replace really does drive the picture and DLSS's result is being
// used. If the image is unchanged, the pass is irrelevant and evaluating into it proves nothing
// — which is exactly the ambiguity an identical-looking DLSS frame leaves open.
// 0 = off, 1 = suppress only the pinned pass, 2 = suppress EVERY pass we structurally match.
//
// Mode 2 is the stronger experiment and comes first: if suppressing everything we match still
// leaves the image unchanged, the matcher never sees the pass that draws the picture, and
// bisecting candidates one at a time would be wasted effort.
int g_ngx_dry_run = 0;
// [STRAYDLSS] DryRunHash=0x...[,0x...] — suppress the named passes and write nothing.
//
// This is how a candidate is tested: the pass that drives the picture is the one whose
// suppression CHANGES the image. Independent of the matcher, so a pass the strict signature
// rejects can still be tested. A LIST suppresses a whole permutation family in one session —
// e.g. all nine cooked FSSDTemporalAccumulationCS permutations for the denoiser experiment,
// which stays valid across resolution changes.
std::uint64_t g_dry_run_hashes[16];
std::size_t g_dry_run_hash_count = 0;

bool is_dry_run_hash(std::uint64_t hash)
{
	for (std::size_t i = 0; i < g_dry_run_hash_count; ++i)
		if (g_dry_run_hashes[i] == hash)
			return true;
	return false;
}
// [STRAYDLSS] NgxPassHash=0x... — the pass DLSS replaces, named explicitly.
//
// Preferred over the history-round-trip heuristic, which is necessary but not sufficient:
// several passes bind their own previous output and it kept selecting ones whose suppression
// changed nothing on screen. Suppressing 0xd2e4d8c23c362ed1 alone FREEZES the image, so that
// pass's output is the picture.
std::uint64_t g_ngx_pass_override = 0;
// [STRAYDLSS] DryRunAlternate=<frames> — suppress the named pass for N frames, then let it run
// for N frames, forever.
//
// Comparing runs is hopeless once the camera moves, because the scene changes too. Alternating
// inside ONE session puts both states in front of similar content, and the add-on logs each
// transition so a screenshot's timestamp identifies which state produced it.
std::uint32_t g_dry_run_alternate = 0;
std::atomic<std::uint64_t> g_present_frame{ 0 };
std::atomic<bool> g_alt_phase_suppressing{ false };
bool g_ngx_logged_once = false;      // the evaluate result
bool g_ngx_skip_logged = false;      // why we did not evaluate — a SEPARATE flag, because
                                     // sharing one meant the skip warning consumed the budget
                                     // and the successful evaluate never reported at all
bool g_ngx_dims_logged = false;
bool g_ngx_inputs_logged = false;
bool g_ngx_registers_logged = false;
// [STRAYDLSS] NgxForceReset — pass InReset=1 on every frame.
//
// The decisive bisection of DLSS itself. With reset asserted, DLSS discards its history and
// ignores the motion vectors, so the output is essentially the current frame upscaled. If the
// image becomes sane, the fault is in the temporal half — our motion vectors or the history
// handling. If it stays wrong, the fault is in the spatial inputs, which would mean colour or
// depth is still not what we think it is.
//
// Worth doing because every input has now been corrected on its own terms — colour comes from
// the shader's declared register, render and output resolutions are separated — and the failure
// is unchanged. That points at something the inputs' identity cannot explain.
// [STRAYDLSS] NgxPaint — clear the captured output to magenta instead of evaluating.
bool g_ngx_paint = false;
// Per-stage counters for the NAMED pass. One-shot logs cannot show that a pass stops
// qualifying LATER — which is exactly what happened: the gate logs fired before NGX had even
// initialised, so they proved nothing about the frames that mattered.
std::atomic<std::uint32_t> g_named_seen{ 0 };
std::atomic<std::uint32_t> g_named_phaseb{ 0 };
std::atomic<std::uint32_t> g_named_live{ 0 };
std::atomic<std::uint32_t> g_named_recorded{ 0 };
std::atomic<std::uint32_t> g_named_evaluated{ 0 };
bool g_ngx_gate_logged = false;
bool g_ngx_gate2_logged = false;
bool g_ngx_gate3_logged = false;
// The ONE pass DLSS is allowed to replace.
//
// The structural matcher legitimately matches several passes — the log shows many distinct
// hashes reporting a history round-trip — and evaluating on each of them means suppressing
// several of the engine's dispatches, not just its TAA. Pin to the first pass we successfully
// evaluate and refuse every other from then on.
std::atomic<std::uint64_t> g_ngx_pass_hash{ 0 };
// ...but a pin must not outlive its pass. UE 4.27 picks FTAAScreenPercentageDim from the
// input:output ratio EVERY frame (TemporalAA.cpp:726-750), so the TAA pass's DXBC — and
// therefore our hash — CHANGES with the screen percentage: range 1 below 71%, range 0 at 1:1.
// A pin taken at one ratio then refuses the real pass at every other ratio FOREVER, and used
// to do so in complete silence. Release a pin whose pass has stopped dispatching.
std::atomic<std::uint64_t> g_ngx_pass_last_frame{ 0 };
constexpr std::uint64_t kPinStaleFrames = 300;
// Hashes that have demonstrated the history round-trip, i.e. UE4 bound their previous u0 back
// as an SRV the following frame. CLAUDE.md §2.9 calls this the decisive test for which pass
// owns the temporal history — and it is a far better gate than "the first pass that happens to
// evaluate", which is how DLSS previously chose and which picked 0xda289b0ddfa934c6, neither of
// the structural TAA candidates measured at this resolution.
std::unordered_map<std::uint64_t, bool> g_roundtrip_seen;
std::unordered_map<std::uint64_t, bool> g_candidate_logged;

// Why a structurally matched TAA pass never reached DLSS.
//
// Every one of these paths used to refuse in SILENCE: the dispatch report says the pass
// matched, and then nothing at all says why it went no further. That is exactly how "no DLSS
// feature is ever created at 100% screen percentage" cost a whole round trip to localise, and
// a round trip is the most expensive thing this project spends. One line per pass per reason.
// (CLAUDE.md §0.1)
enum GateReason
{
	kGateUnknownHash = 0,  // not a cooked FTAAStandaloneCS permutation
	kGateNoViewCb,         // no bound constant buffer decoded as a plausible View
	kGateDeadInputs,       // depth or velocity missing, or not known live
	kGatePinnedElsewhere,  // DLSS is pinned to a different pass
	kGateNoRoundTrip,      // this pass has not proved it owns the temporal history
	kGateNotPrimaryView,   // aspect ratio or upscale factor says cubemap face / reflection capture
	kGateReasonCount,
};
const char *const kGateReasonText[kGateReasonCount] = {
	"its hash is not a cooked FTAAStandaloneCS permutation (regenerate taa_hashes.hpp, or "
	"drop a stray-dlss-hashes.txt beside the add-on)",
	"no bound constant buffer decoded as a plausible View",
	"its depth or velocity SRV is missing or not known live",
	"DLSS is pinned to a different pass",
	"it has not proved it owns the temporal history yet (no u0 round-trip seen)",
	"its render/output shape is not the primary view - the aspect ratio or the upscale factor "
	"is out of range, so it is a cubemap face or a reflection capture, not FTAAStandaloneCS",
};
std::unordered_map<std::uint64_t, std::uint32_t> g_gate_logged; // hash -> reason bitmask

bool g_dry_run_all_logged = false;
bool g_dry_run_mode2_logged = false;
bool g_ngx_waiting_logged = false;

// Traces the first few evaluate cycles step by step. The crash follows a SUCCESSFUL evaluate
// within about a second, the UE4 dump carries an empty callstack, and the add-on's last line is
// simply whatever it logged before dying — so the only way to localise it is to say what we are
// about to do, every time, until we have seen it survive.
// Restore runs on EVERY intercepted dispatch, many times a frame, while evaluate only starts
// once the feature exists — so a single shared budget is spent long before the interesting part.
// The trace therefore only opens after the first successful evaluate.
std::atomic<bool> g_ngx_evaluated_once{ false };
// Every evaluate attempt, independent of the named-pass tracing counters, so the input dump
// fires even without NgxPassHash.
std::atomic<std::uint64_t> g_evaluate_attempts{ 0 };
std::atomic<int> g_ngx_trace_budget{ 40 };
// Always takes at least one argument: MSVC does not support the GNU ##__VA_ARGS__ elision,
// so a zero-argument variadic macro is a syntax error there.
#define NGX_TRACE(fmt, ...) \
	do { \
		if (g_ngx_evaluated_once.load(std::memory_order_relaxed) && \
			g_ngx_trace_budget.fetch_sub(1, std::memory_order_relaxed) > 0) \
			STRAY_LOG_INFO("  ngx-trace: " fmt, __VA_ARGS__); \
	} while (0)
// [STRAYDLSS] MvResolve, default on. A switch so the pass can be bisected on the target
// machine without a rebuild, which is a slow round trip.
bool g_mv_resolve_enabled = true;
// [STRAYDLSS] MvRestoreHeaps, default on. A switch so the heap restore can be A/B'd against
// the crash without another build-and-deploy cycle.
bool g_restore_heaps = true;
// [STRAYDLSS] MvRestoreState, default on.
bool g_restore_state = true;
// [STRAYDLSS] MvDispatch, default on. Off records our state changes but skips the GPU work.
int g_mv_dispatch_mode = 2;

// A crash-survivable breadcrumb. The Phase B path dies with an access violation after
// surviving many frames, so the trigger is something that CHANGES rather than the first call.
// Writing the current step to a tiny file on every attempt means the file names the exact step
// after the crash, instead of costing another guess-and-run cycle.
void mark(int stage, const char *what)
{
	std::FILE *f = nullptr;
	if (fopen_s(&f, "stray-dlss-stage.txt", "w") == 0 && f != nullptr)
	{
		std::fprintf(f, "%d %s\n", stage, what);
		std::fclose(f); // close, not just flush: the process is about to die
	}
}

// Per-shader outcome census. One dispatch report shows one shader; this shows the whole
// field, which is what actually answers "does the TAA pass ever reach the resolver".
struct HashStats
{
	std::uint64_t resolved = 0;
	std::uint64_t failed = 0;
	std::uint32_t gx = 0;
	std::uint32_t gy = 0;
	std::uint32_t srvs = 0;
	std::uint32_t uavs = 0;
	MatchVerdict verdict = MatchVerdict::no_match;
};
std::unordered_map<std::uint64_t, HashStats> g_stats;
std::unordered_map<std::uint64_t, bool> g_failure_dumped;
int g_failure_dumps = 0;
bool g_summary_dumped = false;

Diagnostics g_diag;

const char *verdict_name(MatchVerdict v)
{
	switch (v)
	{
	case MatchVerdict::no_match:            return "no_match";
	case MatchVerdict::structural_only:     return "structural_only";
	case MatchVerdict::hash_and_structural: return "hash_and_structural";
	case MatchVerdict::excluded:            return "excluded";
	}
	return "?";
}

// One line, the first time each pass hits each gate. Deliberately WARN: an intercepted TAA
// pass that never reaches DLSS is the failure this add-on exists to avoid, and it must never
// again be indistinguishable from "the pass was not there".
void log_gate_refusal(std::uint64_t hash, GateReason reason)
{
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		std::uint32_t &mask = g_gate_logged[hash];
		const std::uint32_t bit = 1u << static_cast<unsigned>(reason);
		if ((mask & bit) != 0)
			return;
		mask |= bit;
	}
	STRAY_LOG_WARN("DLSS did not run for pass 0x%016llx: %s. First occurrence for this pass "
		"and reason only.", static_cast<unsigned long long>(hash), kGateReasonText[reason]);
}

const char *hook_format_name(TexFormat f)
{
	switch (f)
	{
	case TexFormat::r32_float_x8x24_typeless: return "R32_FLOAT_X8X24 (depth)";
	case TexFormat::x32_typeless_g8x24_uint:  return "X32_G8X24_UINT (stencil)";
	case TexFormat::r16g16b16a16_unorm:       return "RGBA16_UNORM (velocity)";
	case TexFormat::r16g16b16a16_float:       return "RGBA16_FLOAT (colour)";
	case TexFormat::r16g16_float:             return "RG16_FLOAT";
	case TexFormat::r32g32b32a32_float:       return "RGBA32_FLOAT";
	case TexFormat::r11g11b10_float:          return "R11G11B10_FLOAT (colour)";
	case TexFormat::r10g10b10a2_unorm:        return "R10G10B10A2_UNORM";
	case TexFormat::r32_float:                return "R32_FLOAT";
	case TexFormat::r16_float:                return "R16_FLOAT";
	case TexFormat::r8g8b8a8_unorm:           return "R8G8B8A8_UNORM";
	case TexFormat::b8g8r8a8_unorm:           return "B8G8R8A8_UNORM";
	case TexFormat::unknown:                  return "other";
	}
	return "?";
}

// ---- RR guide recording and the RR-first evaluate ([STRAYDLSS] NgxRR=2) ----
//
// The guide resolve and the DLSSD evaluate are two separately-placed halves. Measured
// 2026-08-31 (guide dumps 600/900): at the TAA dispatch the identified A/B/C resources are
// ALIVE — liveness passes, bind-age 0 — but their CONTENT is already recycled: GBufferA
// read near-black (normals mean = normalized(-1,-1,-1)), ShadingModelID decoded 0, and the
// unlit fallback covered ~95% of every guide. The pool reuses the targets' MEMORY within
// the frame; the end-of-Render refcount release (RESEARCH-RR-GBUFFER.md §1.1) holds for
// the resource OBJECT, not its content. So the resolve records at a point where content is
// alive BY CONSTRUCTION: the first FSSDTemporalAccumulationCS dispatch of the frame, a
// pass that READS the G-buffers as SRVs at that instant.

// [STRAYDLSS] GBufferResolveAt (set_gbuffer_resolve_at): true = record at the first SSD
// temporal-accumulation dispatch each frame (the content-alive default under NgxRR=2);
// false = record at the TAA hook, kept for A/B measurement of the content-death finding.
std::atomic<bool> g_resolve_at_ssd{ true };

// The handoff between the two halves, all under one mutex: which present-frame the guides
// were recorded for, whether they currently sit in NON_PIXEL_SHADER_RESOURCE (the record
// needs them back in UAV; the evaluate needs SRV), and the identification that fed them —
// kept for the evaluate log line. Recording threads differ (the SSD and TAA dispatches can
// be on different command lists), but the evaluate only proceeds after observing
// g_guides_frame == this frame under the mutex, which orders the state transitions the
// same way queue submission orders the GPU work.
// Consume-side ordering basis (why list B may read what list A produced, with no fence of
// our own): the TAA dispatch's colour input transitively depends on the SSD pass's output
// (lighting composites the denoised signal), so UE has already ordered the SSD list's
// execution — whatever queue it recorded on — before the TAA list runs; on one queue,
// submission order does the same. The evaluate additionally consumes ONLY guides whose
// record completed THIS frame (frame tag + ready flag below), so a torn or failed record
// can never be read.
std::mutex g_rr_guides_mutex;
std::uint64_t g_guides_frame = ~0ull;
bool g_guides_ready = false;  // set when THIS frame's record fully completed
bool g_guides_in_srv = false;
gbuffer_finder::Identification g_guides_id;

// [STRAYDLSS] GBufferResolveOnly: the isolation instrument — record (and dump) the guides
// at the SSD trigger but skip the RR evaluate entirely, SR carrying every frame. One run
// with this on isolates record-side faults from evaluate-side faults.
std::atomic<bool> g_rr_resolve_only{ false };

// One first-occurrence line per refusal reason, with specifics — plus the indexed
// counters, so the periodic line carries rates and the log carries the story.
bool g_rr_reason_logged[kRrRefusalCount] = {};
bool rr_refuse(int reason)
{
	g_rr_refusals[reason].fetch_add(1, std::memory_order_relaxed);
	const bool first = !g_rr_reason_logged[reason];
	g_rr_reason_logged[reason] = true;
	return first; // the caller logs its specifics on the first occurrence only
}

// Defined below; the SSD trigger needs it before its definition point.
bool read_view_cb(const icept::BufferRange &cb, ue4::ViewParams &out);

// Records the guide resolve onto `native` and leaves the four guides in
// NON_PIXEL_SHADER_RESOURCE, reporting the identification that fed them. Shared by both
// trigger points. Every failure path names its reason through rr_refuse and returns false
// — the caller falls back to SR, never guesses.
bool record_guides(ID3D12Device *native_device, ID3D12GraphicsCommandList *native,
                   std::uint32_t render_w, std::uint32_t render_h,
                   const ue4::ViewParams &view, gbuffer_finder::Identification &out_id)
{
	// The identification is re-captured EVERY record (pointer rotation measured on 29 of
	// 30 stable frames) and served role-keyed from the freshest accepted candidate of any
	// accepted shape — the shape-locked rule starved RR to 0%. (gbuffer_finder.hpp)
	gbuffer_finder::Identification id;
	std::uint32_t stale_age = 0;
	switch (gbuffer_finder::current_identification(id, &stale_age))
	{
	case gbuffer_finder::IdentRefusal::ok:
		break;
	case gbuffer_finder::IdentRefusal::not_enabled:
	case gbuffer_finder::IdentRefusal::not_armed:
		if (rr_refuse(kRrNotArmed))
			STRAY_LOG_INFO("RR: identification not ARMED yet (no candidate shape has held "
				"30 frames); SR carries the frames. First occurrence only; the periodic "
				"line carries the rate.");
		return false;
	case gbuffer_finder::IdentRefusal::no_candidate:
		if (rr_refuse(kRrNoCandidate))
			STRAY_LOG_INFO("RR: armed but no accepted G-buffer bind recorded yet this "
				"session; SR carries the frame. First occurrence only.");
		return false;
	case gbuffer_finder::IdentRefusal::stale_bind:
		if (rr_refuse(kRrStaleBind))
			STRAY_LOG_WARN("RR: freshest accepted G-buffer bind is %u presents old (max 2) "
				"- the base pass stopped binding, or the finder's tap missed this frame. "
				"SR carries the frame. First occurrence only.", stale_age);
		return false;
	case gbuffer_finder::IdentRefusal::roles_missing:
		if (rr_refuse(kRrRolesMissing))
			STRAY_LOG_WARN("RR: the freshest candidate has an unknown A/B/C slot (licensee "
				"format?) - refusing to guess. SR carries the frames. First occurrence "
				"only.");
		return false;
	}

	// Liveness BEFORE any dereference, per the §5 discipline — the pool can have destroyed
	// a G-buffer between its bind and this dispatch (save-load transitions measured doing
	// exactly this to depth/velocity).
	if (!icept::backend()->is_resource_live(id.gbuffer_a) || !icept::backend()->is_resource_live(id.gbuffer_b) ||
		!icept::backend()->is_resource_live(id.gbuffer_c))
	{
		if (rr_refuse(kRrLiveness))
			STRAY_LOG_WARN("RR: a G-buffer resource died between bind and dispatch "
				"(A=%p live=%d, B=%p live=%d, C=%p live=%d, bind age %u frames); SR "
				"carries the frame. First occurrence only.",
				reinterpret_cast<void *>(id.gbuffer_a),
				icept::backend()->is_resource_live(id.gbuffer_a) ? 1 : 0,
				reinterpret_cast<void *>(id.gbuffer_b),
				icept::backend()->is_resource_live(id.gbuffer_b) ? 1 : 0,
				reinterpret_cast<void *>(id.gbuffer_c),
				icept::backend()->is_resource_live(id.gbuffer_c) ? 1 : 0, id.age_frames);
		return false;
	}

	// The identified extent must COVER the render rect (suspect (b) of the wedge round):
	// the resolve indexes A/B/C at ViewRectMin + thread id across the render rect, and a
	// smaller texture turns those loads into out-of-bounds reads — zeros, i.e. unlit-
	// decoded garbage guides. Menu and gameplay measured equal so far (1920x1080), but a
	// mismatch must refuse loudly, not resolve garbage.
	if (id.extent_width < render_w || id.extent_height < render_h)
	{
		if (rr_refuse(kRrResolveFailed))
			STRAY_LOG_WARN("RR: identified G-buffer extent %ux%u does not cover the render "
				"rect %ux%u - refusing to resolve (would read out of bounds). First "
				"occurrence only.", id.extent_width, id.extent_height, render_w, render_h);
		return false;
	}

	// View rows 12-15 (TranslatedWorldToView, mirror-verified after the first guess of
	// row 8 turned out to be ClipToWorld — ue4_view.hpp) must actually hold a rotation
	// before anything trusts them. A wrong matrix biases specular albedo silently — the
	// §0.2 class of failure — so implausible rows mean SR, loudly, once. Every diagnostic
	// line carries the RR token: a values line without it was lost to a filtered log once.
	if (!ue4::world_to_view_rotation_plausible(view.translated_world_to_view))
	{
		if (rr_refuse(kRrRowsImplausible))
		{
			STRAY_LOG_ERROR("RR: View rows 12-15 (TranslatedWorldToView) do not hold a "
				"rotation - SR carries the frames while this holds. All 16 floats follow.");
			const float *m = view.translated_world_to_view.m;
			for (int r = 0; r < 4; ++r)
				STRAY_LOG_ERROR("RR: row %d: [%.6f %.6f %.6f %.6f]", 12 + r,
					m[r * 4 + 0], m[r * 4 + 1], m[r * 4 + 2], m[r * 4 + 3]);
			// The member-order measurement: orthonormality verdict for every slice of the
			// seven-matrix block. Expected pattern for the mirror-verified order is
			// no,no,no,YES,YES,YES,YES (the four view<->world transforms are rigid; the
			// three clip-involved ones never are). Any other pattern names the licensee's
			// actual member order in one line.
			char verdicts[256];
			int off = std::snprintf(verdicts, sizeof(verdicts), "RR: block verdicts:");
			for (int i = 0; i < 7; ++i)
				if (off > 0 && off < static_cast<int>(sizeof(verdicts)))
					off += std::snprintf(verdicts + off, sizeof(verdicts) - off,
						" %s=%s", ue4::view_matrix_block_name(i),
						ue4::world_to_view_rotation_plausible(view.view_matrix_block[i])
							? "YES" : "no");
			STRAY_LOG_ERROR("%s", verdicts);
		}
		return false;
	}

	if (!gbr::initialise(native_device, render_w, render_h))
	{
		if (rr_refuse(kRrResolveFailed))
			STRAY_LOG_ERROR("RR: gbuffer_resolve initialise failed (%s); SR carries the "
				"frames. First occurrence only.", gbr::last_error());
		return false;
	}

	gbr::ResolveInputs gi;
	gi.gbuffer_a = id.gbuffer_a;
	gi.gbuffer_b = id.gbuffer_b;
	gi.gbuffer_c = id.gbuffer_c;
	gi.render_width = render_w;
	gi.render_height = render_h;
	gi.view_rect_min[0] = view.view_rect_min.x;
	gi.view_rect_min[1] = view.view_rect_min.y;
	// ViewToClipNoAA diagonal (row 32, measured): the jitter-free projection terms the
	// NoV ray math needs. Row-major m[r*4+c].
	gi.proj00 = view.view_to_clip_no_aa.m[0];
	gi.proj11 = view.view_to_clip_no_aa.m[5];
	// The TRANSPOSED upper 3x3: UE stores row-vector matrices while the resolve shader
	// computes dot(row, n). The untransposed rows apply the INVERSE rotation — silently
	// wrong NoV — which is why the extraction is a tested core helper, not inline math.
	ue4::nov_rotation_rows(view.translated_world_to_view, gi.world_to_view);

	// The guides must be in UAV state for the record. A frame whose evaluate never ran
	// (SR fallback after a successful record) leaves them in SRV state; fix that here, on
	// this list, before the dispatch that writes them.
	{
		std::lock_guard<std::mutex> lock(g_rr_guides_mutex);
		if (g_guides_in_srv)
		{
			gbr::transition_outputs(native, /*to_shader_resource=*/false);
			g_guides_in_srv = false;
		}
	}

	bool gbr_recorded;
	{
		perf::Scope perf_gbuf(perf::kGBufferResolve);
		gbr_recorded = gbr::record(native, gi, /*dispatch_mode=*/2);
	}
	if (!gbr_recorded)
	{
		if (rr_refuse(kRrResolveFailed))
			STRAY_LOG_ERROR("RR: gbuffer_resolve record failed (%s); SR carries the frames. "
				"First occurrence only.", gbr::last_error());
		return false;
	}

	// Written as UAVs; NGX (and the dump copies) read them as shader resources.
	gbr::transition_outputs(native, /*to_shader_resource=*/true);
	{
		std::lock_guard<std::mutex> lock(g_rr_guides_mutex);
		g_guides_in_srv = true;
	}

	// The guide-dump channel keys on successful RESOLVE RECORDS, not the shared
	// evaluate-attempt counter: a session where the RR create or evaluate fails still
	// produced guides, and dumping them is the whole point of the offline B/C check —
	// under the old keying a 0%-RR session could never produce a guide file.
	const std::uint64_t rr_rec_no = g_rr_records.fetch_add(1, std::memory_order_relaxed) + 1;
	if (input_dump::wants(rr_rec_no))
	{
		input_dump::capture(native_device, native, gbr::normals_roughness(),
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, "guide_normals", rr_rec_no);
		input_dump::capture(native_device, native, gbr::roughness(),
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, "guide_roughness", rr_rec_no);
		input_dump::capture(native_device, native, gbr::diffuse_albedo(),
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, "guide_diffuse", rr_rec_no);
		input_dump::capture(native_device, native, gbr::specular_albedo(),
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, "guide_specular", rr_rec_no);
	}

	out_id = id;
	return true;
}

// The SSD-dispatch trigger: called from intercept_dispatch for every large dispatch whose
// hash is in the FSSDTemporalAccumulationCS family. That pass reads the G-buffers as SRVs,
// so their CONTENT is alive at this instant by construction — the property the TAA-point
// record measurably lacks. Records on the SSD pass's own command list, BEFORE its dispatch
// proceeds (RR-0 never suppresses it); first sighting per frame only.
//
// RR-1 LOOKAHEAD (comment only): when the SSD family is suppressed via DryRunHash, the
// suppression return in intercept_dispatch fires BEFORE this trigger is reached, so the
// trigger disappears with the pass. The candidate replacements for that phase are (a) the
// first dispatch after the last base-pass RT unbind (the finder already sees every RT
// bind), or (b) the deferred-lighting pass's first draw — both sit between base-pass
// completion and the pool's content reuse. Alternatively, hoist this call above the
// dry-run suppression so a suppressed SSD dispatch still donates its bindings.
// RR-1: update the suppression arm latch from an RR evaluate outcome. Called at the TAA
// evaluate point every frame; only mode 3 arms. Arming needs kRr1ArmFrames consecutive
// successes; any failure disarms immediately so the denoiser comes straight back.
void update_rr1_arm(bool ok)
{
	if (g_ngx_rr_mode.load(std::memory_order_relaxed) != 3)
		return;
	if (ok)
	{
		const std::uint32_t streak =
			g_rr_success_streak.fetch_add(1, std::memory_order_relaxed) + 1;
		if (streak >= kRr1ArmFrames &&
			!g_rr1_armed.exchange(true, std::memory_order_relaxed))
			STRAY_LOG_WARN("RR-1 suppression ARMED after %u consecutive RR evaluates: the "
				"SSD temporal-accumulation family will now be SUPPRESSED so RR denoises the "
				"raw screen-space signal reaching scene colour. SR stays the per-frame "
				"safety net; a fallback disarms this.", kRr1ArmFrames);
	}
	else
	{
		g_rr_success_streak.store(0, std::memory_order_relaxed);
		if (g_rr1_armed.exchange(false, std::memory_order_relaxed))
			STRAY_LOG_WARN("RR-1 suppression DISARMED: RR fell back to SR, so the denoiser is "
				"re-enabled to avoid noisy SR. Re-arms after %u more consecutive RR "
				"evaluates.", kRr1ArmFrames);
	}
}

// RR-1: whether this SSD temporal-accumulation dispatch should be skipped so its noise
// reaches scene colour raw. Coupled to (a) the armed latch (RR reliably running) and (b)
// guides captured THIS frame — if either is false the SSD runs, so a frame RR cannot carry
// keeps its denoiser (never noisy SR by our choice).
bool should_suppress_ssd_for_rr1(std::uint64_t hash)
{
	if (g_ngx_rr_mode.load(std::memory_order_relaxed) != 3 || !is_ssd_temporal_hash(hash))
		return false;
	if (!g_rr1_armed.load(std::memory_order_relaxed))
		return false;
	const std::uint64_t frame = g_present_frame.load(std::memory_order_relaxed);
	std::lock_guard<std::mutex> lock(g_rr_guides_mutex);
	return g_guides_frame == frame && g_guides_ready;
}

void maybe_record_guides_at_ssd(const icept::CommandContext &ctx, std::uint64_t hash)
{
	// Mode 2 captures at the SSD trigger only when GBufferResolveAt=ssd; mode 3 (RR-1)
	// ALWAYS captures here — the TAA-hook alternative reads dead content (measured), which
	// is fatal when we are about to suppress the very passes that keep it alive.
	const int mode = g_ngx_rr_mode.load(std::memory_order_relaxed);
	const bool want = (mode == 2 && g_resolve_at_ssd.load(std::memory_order_relaxed)) ||
		mode == 3;
	if (!want)
		return;
	if (!is_ssd_temporal_hash(hash))
		return;

	// First sighting per frame only — the family dispatches several times (SSR/SSGI/AO
	// instances), and one record per frame is both sufficient and cheapest. The frame is
	// CLAIMED here, before any recording: two family dispatches on concurrently-recording
	// lists would otherwise both pass a check-at-success latch and interleave two sets of
	// state transitions across two lists — undefined transition ordering, the Xid-109
	// shape. Claiming first means a failed record forfeits the frame (guides_ready stays
	// false and the evaluate refuses) rather than risking a second, racing attempt.
	const std::uint64_t frame = g_present_frame.load(std::memory_order_relaxed);
	{
		std::lock_guard<std::mutex> lock(g_rr_guides_mutex);
		if (g_guides_frame == frame)
			return;
		g_guides_frame = frame;
		g_guides_ready = false;
	}

	DispatchBindings b;
	if (!icept::backend()->resolve_compute_bindings(ctx, b))
	{
		if (rr_refuse(kRrResolveFailed))
			STRAY_LOG_ERROR("RR: could not resolve the SSD dispatch's bindings for the View "
				"CB; guides cannot record at the SSD trigger. First occurrence only.");
		return;
	}

	// The View CB, exactly as the TAA path finds it: try every bound CB, keep the one
	// that parses plausibly. The SSD pass carries the same View buffer.
	ue4::ViewParams view{};
	bool view_ok = false;
	for (const auto &cb : b.constant_buffers)
	{
		ue4::ViewParams candidate{};
		if (read_view_cb(cb.second, candidate) && ue4::view_params_plausible(candidate))
		{
			view = candidate;
			view_ok = true;
			break;
		}
	}
	if (!view_ok)
	{
		if (rr_refuse(kRrResolveFailed))
			STRAY_LOG_ERROR("RR: no plausible View CB on the SSD dispatch; guides cannot "
				"record at the SSD trigger. First occurrence only.");
		return;
	}

	const float vw = view.view_size_and_inv_size.x;
	const float vh = view.view_size_and_inv_size.y;
	const auto render_w = static_cast<std::uint32_t>(vw);
	const auto render_h = static_cast<std::uint32_t>(vh);

	ID3D12GraphicsCommandList *native = ctx.native;
	gbuffer_finder::Identification id;
	if (!record_guides(ctx.device, native, render_w, render_h, view, id))
		return; // the reason was counted and logged inside; the frame stays claimed-not-ready

	// THE WEDGE FIX (attempt 4, Xid 109 minutes into an RR menu session): record_guides
	// just changed root signature, PSO, heaps and root parameters on the GAME'S list — and
	// unlike the TAA site, the game's dispatch here is NOT suppressed: the SSD pass
	// proceeds on this list. Without restoring, it ran with OUR compute state — our shader
	// under its group counts, its own output never written, its root state corrupted for
	// everything downstream of it on this list. Restore replays the game's captured root
	// state natively, exactly as the TAA site does after NGX clobbers it.
	{
		perf::Scope perf_restore(perf::kRestore);
		icept::backend()->restore_game_compute_state(ctx);
	}

	{
		std::lock_guard<std::mutex> lock(g_rr_guides_mutex);
		g_guides_id = id;
		g_guides_ready = true;
	}

	static std::atomic<int> s_trigger_logged{ 0 };
	if (s_trigger_logged.fetch_add(1, std::memory_order_relaxed) == 0)
		STRAY_LOG_INFO("RR: guide resolve recorded at the SSD trigger (hash=0x%016llx, "
			"frame %llu, %ux%u) - the content-alive point; the TAA-hook evaluate consumes "
			"these guides later this frame. First occurrence only.",
			static_cast<unsigned long long>(hash),
			static_cast<unsigned long long>(frame), render_w, render_h);
}

// The RR-first evaluate ([STRAYDLSS] NgxRR=2). Consumes the guides recorded at the SSD
// trigger (or records them here under GBufferResolveAt=taa — the A/B mode that measures
// the content-death finding), then evaluates DLSSD. Every missing precondition returns
// false so the caller falls back to the SR evaluate — SR is the safety net EVERY frame,
// never just at startup.
bool try_evaluate_rr(ID3D12Device *native_device, ID3D12GraphicsCommandList *native,
                     const ngx::EvaluateInputs &ei, const ngx::FeatureDesc &fd,
                     const ue4::ViewParams &view)
{
	static bool s_rr_ok_logged = false;

	// The isolation instrument: guides record (and dump) at the SSD trigger, the evaluate
	// is skipped, SR carries every frame. Counted under its own reason so the periodic
	// line stays self-consistent.
	if (g_rr_resolve_only.load(std::memory_order_relaxed))
	{
		if (rr_refuse(kRrResolveOnly))
			STRAY_LOG_WARN("GBufferResolveOnly=1: guides record at the SSD trigger and "
				"dump as usual; the RR evaluate is SKIPPED and SR carries every frame. "
				"Isolation instrument - record-side faults reproduce, evaluate-side "
				"faults cannot.");
		return false;
	}

	gbuffer_finder::Identification id;
	if (g_resolve_at_ssd.load(std::memory_order_relaxed))
	{
		bool have_guides = false;
		const std::uint64_t frame = g_present_frame.load(std::memory_order_relaxed);
		{
			std::lock_guard<std::mutex> lock(g_rr_guides_mutex);
			// Frame tag AND ready AND state: a claimed-but-failed record (ready false), a
			// stale frame, or guides not left in SRV all refuse — the evaluate can never
			// consume a torn record.
			if (g_guides_frame == frame && g_guides_ready && g_guides_in_srv)
			{
				id = g_guides_id;
				have_guides = true;
			}
		}
		if (!have_guides)
		{
			if (rr_refuse(kRrGuidesStale))
				STRAY_LOG_WARN("RR: no guide record this frame (no SSD temporal-accumulation "
					"dispatch seen yet this frame, or its record refused - see any earlier "
					"RR lines); SR carries the frame. First occurrence only. "
					"GBufferResolveAt=taa restores the in-hook record for A/B.");
			return false;
		}
	}
	else
	{
		if (!record_guides(native_device, native, ei.render_width, ei.render_height, view, id))
			return false; // the reason was counted and logged inside
	}

	bool ok = false;
	if (!ngx::ensure_feature_rr(native, fd))
	{
		// ngx_backend logged the create failure (latched, once per size); the counter
		// carries the per-frame rate.
		rr_refuse(kRrCreateFailed);
	}
	else
	{
		ngx::EvaluateInputsRR er;
		er.base = ei;
		er.diffuse_albedo = gbr::diffuse_albedo();
		er.specular_albedo = gbr::specular_albedo();
		er.normals_roughness = gbr::normals_roughness();
		er.roughness = gbr::roughness();
		// WorldToView = rows 12-15 (mirror-verified, rotation-checked at record time);
		// ViewToClip = ViewToClipNoAA rows 32-35 (measured) — jitter reaches NGX
		// separately, so the unjittered projection is the consistent pairing. Both
		// row-major, as stored.
		std::memcpy(er.world_to_view, view.translated_world_to_view.m,
			sizeof(er.world_to_view));
		std::memcpy(er.view_to_clip, view.view_to_clip_no_aa.m, sizeof(er.view_to_clip));
		er.have_matrices = true;
		er.frame_time_delta_ms = view.delta_time * 1000.0f;

		{
			perf::Scope perf_rr(perf::kNgxRr);
			ok = ngx::evaluate_rr(native, er);
		}

		if (ok)
		{
			// The serving shape is worth a line the first time and on every flip (rate
			// limited): it says WHICH accepted candidate fed the guides.
			static int s_last_shape = -1;
			static int s_shape_flips_logged = 0;
			const int shape = id.velocity_in_set ? 1 : 0;
			if (!s_rr_ok_logged)
			{
				s_rr_ok_logged = true;
				STRAY_LOG_INFO("DLSS RR evaluate OK: %ux%u -> %ux%u with guides "
					"(A=%p B=%p C=%p swapBC=%d shape=%s bind-age=%u resolve-at=%s). SR "
					"remains the per-frame fallback.",
					ei.render_width, ei.render_height, fd.output_width, fd.output_height,
					reinterpret_cast<void *>(id.gbuffer_a),
					reinterpret_cast<void *>(id.gbuffer_b),
					reinterpret_cast<void *>(id.gbuffer_c), gbr::bc_swapped() ? 1 : 0,
					id.velocity_in_set ? "with-velocity" : "velocity-free", id.age_frames,
					g_resolve_at_ssd.load(std::memory_order_relaxed) ? "ssd" : "taa");
			}
			else if (shape != s_last_shape && s_shape_flips_logged < 4)
			{
				++s_shape_flips_logged;
				STRAY_LOG_INFO("RR: serving shape changed to %s (both are accepted "
					"candidates; role-keyed serving). Logged at most 4 times.",
					id.velocity_in_set ? "with-velocity" : "velocity-free");
			}
			s_last_shape = shape;
		}
		else if (rr_refuse(kRrEvaluateFailed))
		{
			STRAY_LOG_ERROR("DLSS RR evaluate FAILED (%s); SR carries the frames. First "
				"occurrence only.", ngx::last_error());
		}
	}

	// Back to UAV for the next record, evaluate or not.
	gbr::transition_outputs(native, /*to_shader_resource=*/false);
	{
		std::lock_guard<std::mutex> lock(g_rr_guides_mutex);
		g_guides_in_srv = false;
	}
	return ok;
}

// Reads the View constant buffer at the moment of the dispatch. It must be read here, at
// command-recording time on the thread that just set the root arguments, because UE4's
// FD3D12FastConstantAllocator sub-allocates from an upload ring that the CPU writer will
// advance past later in the frame. (docs/RESEARCH.md §2.6)
bool read_view_cb(const icept::BufferRange &cb, ue4::ViewParams &out)
{
	// Liveness and facts from the backend, never a dereference of ours: the ReShade backend
	// checks liveness first, the native backend answers from its creation-time snapshot.
	icept::ResourceInfo ri{};
	if (!icept::backend()->describe_resource(cb.buffer, ri) || !ri.is_buffer || !ri.upload_heap)
		return false;

	// BOUNDS CHECK, and it is not a formality — its absence was the access violation.
	//
	// Every bound constant buffer gets tried until one decodes as a plausible View, and most
	// of them are small: UE4's $Globals allocations are a few hundred bytes. Copying a fixed
	// 2448 bytes out of one that sits near the end of its page reads unmapped memory and kills
	// the process, with the fault landing inside memcpy where it is hard to attribute.
	//
	// BufferRange::size is kUnknownSize for root CBVs, where the backend does not know the
	// extent, so the resource's own size is the authority.
	if (cb.size != icept::kUnknownSize && cb.size < ue4::kViewPrefixBytes)
		return false;
	if (ri.buffer_size < cb.offset + ue4::kViewPrefixBytes)
		return false;

	unsigned char copy[ue4::kViewPrefixBytes];
	if (!icept::backend()->read_buffer(cb, sizeof(copy), copy))
		return false;

	return ue4::parse_view_params(copy, sizeof(copy), out);
}

void report(std::uint64_t hash, const DispatchBindings &b, const MatchResult &m,
            const ue4::ViewParams &view, bool view_ok,
            uint32_t gx, uint32_t gy, uint32_t gz)
{
	STRAY_LOG_INFO("=========== DISPATCH REPORT  0x%016llx ===========",
		static_cast<unsigned long long>(hash));
	STRAY_LOG_INFO("verdict = %s (%s)%s", verdict_name(m.verdict), m.reason,
		m.camera_cut_dummies ? "  [CAMERA-CUT FRAME: velocity/history are 1x1 dummies]" : "");
	if (m.output_width != 0)
		STRAY_LOG_INFO("output = %ux%u  upsampling = %s", m.output_width, m.output_height,
			m.is_upsampling ? "YES" : "no");
	STRAY_LOG_INFO("dispatch = %ux%ux%u  ->  covers %ux%u px at 8x8", gx, gy, gz, gx * 8, gy * 8);

	for (const auto &t : b.srvs)
		STRAY_LOG_INFO("  t%-2u res=0x%016llx %-26s %ux%u", t.slot,
			static_cast<unsigned long long>(t.resource), hook_format_name(t.format), t.width, t.height);
	for (const auto &t : b.uavs)
		STRAY_LOG_INFO("  u%-2u res=0x%016llx %-26s %ux%u", t.slot,
			static_cast<unsigned long long>(t.resource), hook_format_name(t.format), t.width, t.height);

	if (m.verdict == MatchVerdict::hash_and_structural || m.verdict == MatchVerdict::structural_only)
		STRAY_LOG_INFO("  resolved: depth=t%u stencil=t%u velocity=t%u colour=t%u,t%u out=u%u ds=%d",
			m.depth_srv, m.stencil_srv, m.velocity_srv, m.colour_srv_a, m.colour_srv_b,
			m.output_uav, m.has_downsample_uav ? 1 : 0);

	if (!view_ok)
	{
		STRAY_LOG_INFO("  View CB: NOT READABLE or implausible (cb valid=%d reg=b%u)",
			b.view_cb_valid ? 1 : 0, b.view_cb_register);
	}
	else
	{
		STRAY_LOG_INFO("  View CB at b%u, offset %llu", b.view_cb_register,
			static_cast<unsigned long long>(b.view_cb.offset));
		STRAY_LOG_INFO("    ViewRectMin        = %.1f %.1f",
			view.view_rect_min.x, view.view_rect_min.y);
		STRAY_LOG_INFO("    ViewSizeAndInvSize = %.1f %.1f %.6f %.6f",
			view.view_size_and_inv_size.x, view.view_size_and_inv_size.y,
			view.view_size_and_inv_size.z, view.view_size_and_inv_size.w);
		STRAY_LOG_INFO("    BufferSize         = %.1f %.1f",
			view.buffer_size_and_inv_size.x, view.buffer_size_and_inv_size.y);
		STRAY_LOG_INFO("    TemporalAAJitter   = %.6f %.6f %.6f %.6f",
			view.temporal_aa_jitter.x, view.temporal_aa_jitter.y,
			view.temporal_aa_jitter.z, view.temporal_aa_jitter.w);
		STRAY_LOG_INFO("    TemporalAAParams   = %.1f %.1f %.6f %.6f",
			view.temporal_aa_params.x, view.temporal_aa_params.y,
			view.temporal_aa_params.z, view.temporal_aa_params.w);
		STRAY_LOG_INFO("    PreExposure=%.6f  NearPlane=%.4f  DeltaTime=%.6f  CameraCut=%.1f",
			view.pre_exposure, view.near_plane, view.delta_time, view.camera_cut);
		for (int r = 0; r < 4; ++r)
			STRAY_LOG_INFO("    ClipToPrevClip[%d]  = %+.6f %+.6f %+.6f %+.6f", r,
				view.clip_to_prev_clip.m[r * 4 + 0], view.clip_to_prev_clip.m[r * 4 + 1],
				view.clip_to_prev_clip.m[r * 4 + 2], view.clip_to_prev_clip.m[r * 4 + 3]);
		STRAY_LOG_INFO("    plausible = %s", ue4::view_params_plausible(view) ? "YES" : "no");
	}
	STRAY_LOG_INFO("========================================================");
}

} // namespace

bool owns_temporal_history(std::uint64_t hash)
{
	std::lock_guard<std::mutex> lock(g_mutex);
	const auto it = g_roundtrip_seen.find(hash);
	return it != g_roundtrip_seen.end() && it->second;
}

const Diagnostics &diagnostics() { return g_diag; }

void set_ngx_evaluate(bool enabled) { g_ngx_evaluate = enabled; }
void set_ngx_rr(int mode) { g_ngx_rr_mode.store(mode, std::memory_order_relaxed); }
void set_gbuffer_resolve_at(bool at_ssd)
{
	g_resolve_at_ssd.store(at_ssd, std::memory_order_relaxed);
}
void set_gbuffer_resolve_only(bool resolve_only)
{
	g_rr_resolve_only.store(resolve_only, std::memory_order_relaxed);
}
void rr_counters(std::uint32_t &rr_evaluates, std::uint32_t &sr_fallbacks)
{
	rr_evaluates = g_rr_evaluates.load(std::memory_order_relaxed);
	sr_fallbacks = g_rr_fallbacks.load(std::memory_order_relaxed);
}
void rr1_counters(bool &armed, std::uint64_t &suppressed_total,
                  std::uint32_t &suppressed_last_frame)
{
	armed = g_rr1_armed.load(std::memory_order_relaxed);
	suppressed_total = g_rr1_suppressed_total.load(std::memory_order_relaxed);
	suppressed_last_frame = g_rr1_last_frame_suppressed.load(std::memory_order_relaxed);
}
const char *const kRrRefusalNames[kRrRefusalCount] = {
	"not-armed", "no-candidate", "stale-bind", "roles-missing", "liveness",
	"rows-implausible", "resolve-failed", "create-failed", "evaluate-failed",
	"guides-stale", "resolve-only",
};
void rr_refusal_counters(std::uint32_t out[kRrRefusalCount])
{
	for (int i = 0; i < kRrRefusalCount; ++i)
		out[i] = g_rr_refusals[i].load(std::memory_order_relaxed);
}
void set_ngx_dry_run(int mode) { g_ngx_dry_run = mode; }
void set_dry_run_hashes(const std::uint64_t *hashes, std::size_t count)
{
	g_dry_run_hash_count = count < 16 ? count : 16;
	for (std::size_t i = 0; i < g_dry_run_hash_count; ++i)
		g_dry_run_hashes[i] = hashes[i];
}
void set_ngx_pass_hash(std::uint64_t hash) { g_ngx_pass_override = hash; }
void set_ngx_paint(bool enabled) { g_ngx_paint = enabled; }
void set_dry_run_alternate(std::uint32_t frames) { g_dry_run_alternate = frames; }

// True when a dry run should suppress RIGHT NOW.
//
// Every dry-run mode must go through this. Two of the three modes were written without it and
// suppressed in BOTH halves of the alternation, which produces a null difference that looks
// exactly like a real negative — the single most misleading failure this project has had.
bool dry_run_phase_active()
{
	return g_dry_run_alternate == 0 ||
		g_alt_phase_suppressing.load(std::memory_order_relaxed);
}

void note_present(std::uint64_t frame)
{
	g_present_frame.store(frame, std::memory_order_relaxed);
	// Snapshot and clear the per-frame RR-1 suppression tally for the periodic report.
	g_rr1_last_frame_suppressed.store(
		g_rr1_suppressed_this_frame.exchange(0, std::memory_order_relaxed),
		std::memory_order_relaxed);
	if (g_dry_run_alternate == 0)
		return;
	const bool suppressing = ((frame / g_dry_run_alternate) & 1ull) != 0;
	const bool was = g_alt_phase_suppressing.exchange(suppressing, std::memory_order_relaxed);
	if (was != suppressing)
		STRAY_LOG_INFO("ALT PHASE %s at frame %llu", suppressing ? "SUPPRESSING" : "normal",
			static_cast<unsigned long long>(frame));
}

void configure(bool mv_resolve_enabled, bool restore_heaps, bool restore_state, int dispatch_mode)
{
	g_mv_resolve_enabled = mv_resolve_enabled;
	g_restore_heaps = restore_heaps;
	g_restore_state = restore_state;
	g_mv_dispatch_mode = dispatch_mode;
	STRAY_LOG_INFO("MV resolve=%s heapRestore=%s stateRestore=%s dispatchMode=%d",
		mv_resolve_enabled ? "on" : "off", restore_heaps ? "on" : "off",
		restore_state ? "on" : "off", dispatch_mode);
}

void dump_summary()
{
	std::lock_guard<std::mutex> lock(g_mutex);
	if (g_summary_dumped)
		return;
	g_summary_dumped = true;

	STRAY_LOG_INFO("======== PER-SHADER DISPATCH CENSUS ========");
	STRAY_LOG_INFO("  %-18s %-11s %5s %5s %8s %8s  %s",
		"hash", "dispatch", "srv", "uav", "resolved", "failed", "verdict");
	for (const auto &e : g_stats)
	{
		STRAY_LOG_INFO("  0x%016llx %4ux%-6u %5u %5u %8llu %8llu  %s",
			static_cast<unsigned long long>(e.first), e.second.gx, e.second.gy,
			e.second.srvs, e.second.uavs,
			static_cast<unsigned long long>(e.second.resolved),
			static_cast<unsigned long long>(e.second.failed),
			verdict_name(e.second.verdict));
	}
	STRAY_LOG_INFO("===========================================");
}

void set_pipeline_hash(uint64_t pipeline_handle, std::uint64_t hash)
{
	std::lock_guard<std::mutex> lock(g_mutex);
	g_pipeline_hashes[pipeline_handle] = hash;
}

void forget_pipeline(uint64_t pipeline_handle)
{
	std::lock_guard<std::mutex> lock(g_mutex);
	g_pipeline_hashes.erase(pipeline_handle);
}

void set_bound_pipeline(const icept::CommandContext &ctx, uint64_t pipeline_handle)
{
	std::lock_guard<std::mutex> lock(g_mutex);
	if (g_pipeline_hashes.count(pipeline_handle) != 0)
		g_bound[ctx.native] = pipeline_handle;
}

void forget_command_list(const icept::CommandContext &ctx)
{
	// Without this a reset command list keeps its previous pipeline attribution, so a dispatch
	// can be blamed on a shader that is no longer bound.
	std::lock_guard<std::mutex> lock(g_mutex);
	g_bound.erase(ctx.native);
}

bool intercept_dispatch(const icept::CommandContext &ctx, uint32_t x, uint32_t y, uint32_t z)
{
	// Cheap rejection first: this runs on every dispatch, thousands of times a frame.
	// Anything smaller than a plausible full-screen tile grid cannot be the TAA pass.
	if (z != 1 || x < 32 || y < 18)
		return false;

	// Everything from here is OUR work on the dispatch path. Placed after the size gate on
	// purpose: the gate rejects thousands of dispatches per frame and must stay free, while
	// the few dozen that survive are exactly the ones worth attributing. (src/perf.hpp)
	perf::Scope perf_dispatch(perf::kDispatchPath);

	std::uint64_t hash = 0;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		++g_diag.large_dispatches;

		const auto b = g_bound.find(ctx.native);
		if (b == g_bound.end())
		{
			++g_diag.no_bound_pipeline;
			return false;
		}
		const auto h = g_pipeline_hashes.find(b->second);
		if (h == g_pipeline_hashes.end())
		{
			++g_diag.no_hash;
			return false;
		}
		hash = h->second;
		if (g_ngx_pass_override != 0 && hash == g_ngx_pass_override)
			g_named_seen.fetch_add(1, std::memory_order_relaxed);

		// Named-pass suppression, BEFORE any early return.
		//
		// This used to sit further down and was silently defeated by the reporting throttle
		// below ("g_report_count[hash] >= 2"): once a hash had been reported twice,
		// on_dispatch returned before ever reaching the check, so the pass was suppressed for
		// two frames and ran normally for the rest of the session. The test looked like it ran
		// and measured nothing.
		{
			if (is_dry_run_hash(hash) && dry_run_phase_active())
			{
				static std::atomic<int> s_logged{ 0 };
				if (s_logged.fetch_add(1, std::memory_order_relaxed) < 16)
					STRAY_LOG_WARN("DRY RUN (hash): suppressing 0x%016llx, writing nothing.",
						static_cast<unsigned long long>(hash));
				return true;
			}
		}

		{
			HashStats &st = g_stats[hash];
			st.gx = x;
			st.gy = y;
		}

		// The reporting throttle must never apply while a dry run is active: it would suppress
		// a pass for its first two dispatches and then let it run, which is a test that looks
		// like it ran and measures nothing.
		// The throttle exists to stop log spam, and it must never gate BEHAVIOUR. It already
		// silently truncated a dry run once; here it let the named pass reach phase B exactly
		// three times out of 3558 dispatches, so DLSS could never evaluate. Disable it whenever
		// anything downstream depends on seeing the pass.
		const bool dry_running = g_dry_run_hash_count != 0 || g_ngx_dry_run != 0 ||
			g_ngx_pass_override != 0 || g_ngx_evaluate;
		if (!dry_running && g_report_count[hash] >= 2 && !is_known_taa_hash(hash))
		{
			// Still track the output resource each frame: the history round-trip (this
			// frame's u0 reappearing as an SRV next frame) is the decisive test for which
			// colour SRV is the history. (CLAUDE.md §2.3)
			return false;
		}
	}

	// The RR guide-resolve trigger ([STRAYDLSS] GBufferResolveAt=ssd): the first SSD
	// temporal-accumulation dispatch of the frame reads the G-buffers as SRVs, so their
	// content is alive HERE — record the guides on this list before the dispatch proceeds
	// (RR-0 never suppresses it; the throttle above cannot eat this call because every RR
	// session sets NgxEvaluate, which disables the throttle entirely).
	maybe_record_guides_at_ssd(ctx, hash);

	// RR-1 ([STRAYDLSS] NgxRR=3): the guides were just captured at this content-alive point
	// AND the game's compute state restored, so skipping the SSD dispatch here is clean.
	// Suppressing it makes the screen-space SSR/SSGI noise reach scene colour uncleaned and
	// thus TAA t1, which RR then denoises. Gated on RR reliably running + guides-this-frame.
	if (should_suppress_ssd_for_rr1(hash))
	{
		g_rr1_suppressed_total.fetch_add(1, std::memory_order_relaxed);
		g_rr1_suppressed_this_frame.fetch_add(1, std::memory_order_relaxed);
		static std::atomic<int> s_rr1_supp_logged{ 0 };
		if (s_rr1_supp_logged.fetch_add(1, std::memory_order_relaxed) < 4)
			STRAY_LOG_WARN("RR-1: suppressing SSD 0x%016llx (guides captured, RR armed); its "
				"noise reaches scene colour raw for RR to denoise. Logged 4x.",
				static_cast<unsigned long long>(hash));
		return true;
	}

	DispatchBindings b;
	if (!icept::backend()->resolve_compute_bindings(ctx, b))
	{
		// Say so once. A resolve that fails silently looks exactly like "the TAA pass never
		// ran", and telling those apart is otherwise a whole extra round-trip on a machine
		// the developer cannot iterate on quickly.
		bool dump_this = false;
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			++g_diag.resolve_failed;
			++g_stats[hash].failed;
			// Explain the failure for the shaders that actually matter, once each, rather
			// than only for whichever dispatch happened to be first overall.
			if (!g_failure_dumped[hash] && g_failure_dumps < 4 &&
				(is_known_taa_hash(hash) || hash == kSecondCandidateHash ||
				 hash == kDenoiserLookalikeHash || y >= 200))
			{
				g_failure_dumped[hash] = true;
				++g_failure_dumps;
				dump_this = true;
			}
		}

		if (dump_this)
		{
			char why[128];
			std::snprintf(why, sizeof(why), "resolve FAILED for 0x%016llx at %ux%u",
				static_cast<unsigned long long>(hash), x, y);
			icept::backend()->dump_tracker_state(ctx, why);
		}

		static bool warned = false;
		if (!warned)
		{
			warned = true;
			STRAY_LOG_WARN("resolve_compute_bindings found nothing for a %ux%u dispatch of "
				"0x%016llx. Descriptor/state tracking is registered but produced no compute "
				"bindings — check the shader_stage mask and that both trackers registered "
				"before the first frame.", x, y, static_cast<unsigned long long>(hash));
		}
		return false;
	}

	// Try every bound constant buffer and keep the first that decodes to a plausible View.
	// Guessing a register would be fragile: b3, b4 and b5 have all been observed carrying it
	// on different passes.
	ue4::ViewParams view{};
	bool view_ok = false;
	for (const auto &cb : b.constant_buffers)
	{
		ue4::ViewParams candidate{};
		if (read_view_cb(cb.second, candidate) && ue4::view_params_plausible(candidate))
		{
			view = candidate;
			view_ok = true;
			b.view_cb = cb.second;
			b.view_cb_valid = true;
			b.view_cb_register = cb.first;
			break;
		}
	}

	// The signature matcher needs the render rect. Prefer the View buffer; fall back to the
	// dispatch geometry so an unreadable CB does not blind the identification.
	const uint32_t w = view_ok ? static_cast<uint32_t>(view.view_size_and_inv_size.x) : x * 8;
	const uint32_t h = view_ok ? static_cast<uint32_t>(view.view_size_and_inv_size.y) : y * 8;

	DispatchSignature sig;
	sig.srvs = b.srvs;
	sig.uavs = b.uavs;
	sig.has_view_cb = b.view_cb_valid;
	sig.group_count_x = x;
	sig.group_count_y = y;
	sig.group_count_z = z;
	sig.shader_hash = hash;

	const MatchResult m = match_taa_dispatch(sig, w, h);

	{
		std::lock_guard<std::mutex> lock(g_mutex);
		HashStats &st = g_stats[hash];
		++st.resolved;
		st.srvs = static_cast<std::uint32_t>(b.srvs.size());
		st.uavs = static_cast<std::uint32_t>(b.uavs.size());
		st.verdict = m.verdict;
	}

	// Report every large dispatch once, whatever the verdict. During Phase A the point is to
	// see the whole field, not only what we already expect to find.
	// Report twice: the first sighting is almost always a camera-cut frame, where velocity and
	// history are 1x1 dummies, so a steady-state frame is needed to see the real bindings.
	bool should_report = false;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		const bool want_steady = m.verdict != MatchVerdict::excluded && !m.camera_cut_dummies &&
			!g_steady_reported[hash];
		if (g_report_count[hash] == 0 || want_steady)
		{
			if (!m.camera_cut_dummies)
				g_steady_reported[hash] = true;
			++g_report_count[hash];
			should_report = true;
			++g_diag.candidates_reported;
		}
	}

	if (should_report)
		report(hash, b, m, view, view_ok, x, y, z);

	if (view_ok)
		g_diag.view_seen = true;

	if (m.verdict == MatchVerdict::hash_and_structural || m.verdict == MatchVerdict::structural_only)
	{
		g_diag.best_hash = hash;
		g_diag.best_width = w;
		g_diag.best_height = h;
	}

	// A named pass can be suppressed regardless of what the matcher thinks of it, because the
	// whole point is to test passes the strict signature rejects.
	// ---- Relaxed candidate report ----
	//
	// §2.3's signature demands a depth+stencil SRV pair over one resource, which was measured on
	// ETAAPassConfig::MainUpsampling at 4K/50%. At 1:1 the engine picks ETAAPassConfig::Main, a
	// permutation whose bindings were never characterised — and suppressing every pass the
	// strict signature finds changes nothing on screen, so it is not finding the real TAA.
	//
	// Report anything with the TAA's ESSENTIAL shape regardless of permutation: a depth SRV, a
	// velocity SRV, and an HDR colour UAV. Log-only, once per hash, so the candidate list for
	// this configuration is explicit instead of inferred.
	{
		const BoundTexture *cand_depth = nullptr;
		const BoundTexture *cand_velocity = nullptr;
		for (const auto &t : b.srvs)
		{
			if (t.format == TexFormat::r32_float_x8x24_typeless && cand_depth == nullptr)
				cand_depth = &t;
			if (t.format == TexFormat::r16g16b16a16_unorm && cand_velocity == nullptr)
				cand_velocity = &t;
		}
		const BoundTexture *cand_out = nullptr;
		for (const auto &u : b.uavs)
		{
			if ((u.format == TexFormat::r16g16b16a16_float ||
			     u.format == TexFormat::r11g11b10_float) && u.width > 64 && cand_out == nullptr)
				cand_out = &u;
		}

		const bool is_relaxed_candidate =
			cand_depth != nullptr && cand_velocity != nullptr && cand_out != nullptr;

		// Mode 3 suppresses EVERY pass with the TAA's essential shape — a depth SRV, a velocity
		// SRV and an HDR colour UAV — not just the ones the strict §2.3 signature accepts.
		//
		// One run then answers whether the answer is in the candidate set at all. If the image
		// still does not change, the TAA is not a compute dispatch of this shape and testing the
		// candidates one by one would be wasted; the next question would be whether it is a
		// draw, which we do not hook.
		if (g_ngx_dry_run == 3 && is_relaxed_candidate && dry_run_phase_active())
		{
			if (!g_dry_run_all_logged)
			{
				g_dry_run_all_logged = true;
				STRAY_LOG_WARN("DRY RUN (all candidates): suppressing every pass with a depth "
					"SRV + velocity SRV + HDR colour UAV, writing nothing.");
			}
			return true;
		}

		if (is_relaxed_candidate)
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			if (!g_candidate_logged[hash])
			{
				g_candidate_logged[hash] = true;
				STRAY_LOG_INFO("TAA CANDIDATE 0x%016llx: depth=t%u %ux%u velocity=t%u %ux%u "
					"out=u%u %ux%u srvs=%zu uavs=%zu verdict=%s",
					static_cast<unsigned long long>(hash),
					cand_depth->slot, cand_depth->width, cand_depth->height,
					cand_velocity->slot, cand_velocity->width, cand_velocity->height,
					cand_out->slot, cand_out->width, cand_out->height,
					b.srvs.size(), b.uavs.size(), m.reason);
			}
		}
	}

	// ---- Phase B: our motion-vector resolve, and DLSS, on the game's command list ----
	//
	// Returns false unless DLSS actually produced this pass's output, in which case the
	// engine's own TAA dispatch must be suppressed — see where this is set.
	bool suppress_engine_dispatch = false;

	if (g_mv_resolve_enabled &&
		(m.verdict == MatchVerdict::hash_and_structural || m.verdict == MatchVerdict::structural_only))
	{
		mark(1, "entered-phaseB");

		// Trace the NAMED pass through every gate between here and Evaluate.
		//
		// DLSS silently never ran for it and the outer conditions all looked satisfied, which
		// means one of the inner gates rejected it. Guessing which cost a round trip; say it.
		const bool tracing = g_ngx_pass_override != 0 && hash == g_ngx_pass_override;
		if (tracing)
			g_named_phaseb.fetch_add(1, std::memory_order_relaxed);
		// The RESOURCES, not the game's descriptors: those live in UE4's bound shader-visible
		// heap, and D3D12 forbids copying out of one (#654). We build our own views instead.
		std::uint64_t depth_resource = 0;
		std::uint64_t velocity_resource = 0;
		for (const auto &t : b.srvs)
		{
			if (t.slot == m.depth_srv)
				depth_resource = t.resource;
			if (t.slot == m.velocity_srv && t.format == TexFormat::r16g16b16a16_unorm)
				velocity_resource = t.resource;
		}

		// On a camera-cut frame velocity is the 1x1 dummy, so there is nothing to resolve.
		// That is the pass resetting, not an error.
		// Refuse to touch a resource ReShade has already reported destroyed. Its view->resource
		// map outlives the resource on D3D12, and building an SRV from a dead one faults inside
		// the driver (vkCreateImageView, 0xc0000005) and takes the game with it.
		const bool resources_live = icept::backend()->is_resource_live(depth_resource) &&
			icept::backend()->is_resource_live(velocity_resource);
		if (depth_resource != 0 && velocity_resource != 0)
		{
			g_resolve_attempts.fetch_add(1, std::memory_order_relaxed);
			if (!resources_live)
				g_resolve_skipped_stale.fetch_add(1, std::memory_order_relaxed);
		}
		if (!resources_live && depth_resource != 0 && velocity_resource != 0 &&
			!g_stale_resource_logged)
		{
			g_stale_resource_logged = true;
			STRAY_LOG_WARN("Skipping the resolve: ReShade reported a resource that is no longer "
				"live (depth=%p live=%d, velocity=%p live=%d). Its view->resource map outlives "
				"the resource on D3D12.",
				reinterpret_cast<void *>(depth_resource), icept::backend()->is_resource_live(depth_resource) ? 1 : 0,
				reinterpret_cast<void *>(velocity_resource),
				icept::backend()->is_resource_live(velocity_resource) ? 1 : 0);
		}

		if (tracing && !g_ngx_gate_logged)
		{
			g_ngx_gate_logged = true;
			STRAY_LOG_WARN("GATE 0x%016llx: view_ok=%d resources_live=%d camera_cut_dummies=%d "
				"depth=%p velocity=%p", static_cast<unsigned long long>(hash), view_ok ? 1 : 0,
				resources_live ? 1 : 0, m.camera_cut_dummies ? 1 : 0,
				reinterpret_cast<void *>(depth_resource),
				reinterpret_cast<void *>(velocity_resource));
		}

		if (tracing && view_ok && resources_live)
			g_named_live.fetch_add(1, std::memory_order_relaxed);

		// Only run the resolve/evaluate machinery for passes that are actually cooked
		// FTAAStandaloneCS permutations (or an explicit override). Loading screens produce
		// structural look-alikes over violently short-lived resources — a resource can pass
		// the liveness gate at capture and be destroyed before CreateShaderResourceView, which
		// is the §5 vkCreateImageView crash. Measured 2026-08-31: session died at "about to
		// view velocity" while resolving structural candidate 0x56571dffb2076ce9 during a
		// save load. Now that identification is table-driven, unknown hashes have nothing to
		// gain from the resolve path.
		const bool worth_resolving = is_known_taa_hash(hash) ||
			(g_ngx_pass_override != 0 && hash == g_ngx_pass_override);

		// Say which gate stopped it, once. Only while an evaluate is actually wanted — in
		// observation-only sessions these paths are expected and silence is correct.
		if (g_ngx_evaluate)
		{
			if (!worth_resolving)
				log_gate_refusal(hash, kGateUnknownHash);
			else if (!view_ok)
				log_gate_refusal(hash, kGateNoViewCb);
			else if (!resources_live)
				log_gate_refusal(hash, kGateDeadInputs);
		}

		// Camera-cut frames (1x1 dummy velocity/history) are evaluated too — with InReset set
		// via is_camera_cut — so the engine's TAA never runs once DLSS engages. Skipping them
		// let the engine's TAA blend against DLSS-written history, which flickered. The dummy
		// velocity is harmless: its out-of-bounds loads return zero, the decode's validity test
		// fails, the camera-motion branch yields ~zero vectors, and a reset frame ignores
		// motion vectors regardless.
		if (worth_resolving && view_ok && resources_live)
		{
			mark(2, "descriptors-found");
			ID3D12Device *native_device = ctx.device;
			ID3D12GraphicsCommandList *native = ctx.native;

			// The RENDER rect comes from the View constant buffer, not from a texture extent.
			//
			// DispatchMatch::render_width is the DEPTH TEXTURE's width, and UE4 allocates depth
			// and velocity at the full scene-buffer extent, not the view size (CLAUDE.md §2.5).
			// At 50% screen percentage that is 2560x1440 where the render rect is 1280x720 — so
			// using it allocated four times the output we need and, worse, handed the shader a
			// RenderSize that makes every motion vector wrong. View.ViewSizeAndInvSize (row 130)
			// is the authoritative answer and we already parse it.
			std::uint32_t render_w = m.render_width;
			std::uint32_t render_h = m.render_height;
			const float vw = view.view_size_and_inv_size.x;
			const float vh = view.view_size_and_inv_size.y;
			if (vw >= 1.0f && vh >= 1.0f && vw <= static_cast<float>(m.render_width) &&
				vh <= static_cast<float>(m.render_height))
			{
				render_w = static_cast<std::uint32_t>(vw);
				render_h = static_cast<std::uint32_t>(vh);
			}
			if (!g_render_size_logged)
			{
				g_render_size_logged = true;
				STRAY_LOG_INFO("Render rect %ux%u from View.ViewSizeAndInvSize; depth/velocity "
					"are %ux%u at the scene-buffer extent.", render_w, render_h,
					m.render_width, m.render_height);
			}

			if (mv::initialise(native_device, render_w, render_h))
			{
				mv::ResolveInputs inputs;
				inputs.depth_resource = depth_resource;
				inputs.velocity_resource = velocity_resource;
				inputs.render_width = render_w;
				inputs.render_height = render_h;
				inputs.view = &view;

				mark(3, "mv-initialised");


				mark(4, "heaps-collected");

				bool recorded;
				{
					perf::Scope perf_mv(perf::kMvResolve);
					recorded = mv::record(native, inputs, g_mv_dispatch_mode);
				}
				if (tracing && !g_ngx_gate2_logged)
				{
					g_ngx_gate2_logged = true;
					STRAY_LOG_WARN("GATE2 0x%016llx: render=%ux%u depth=%p velocity=%p "
						"record=%d (%s)", static_cast<unsigned long long>(hash),
						render_w, render_h, reinterpret_cast<void *>(depth_resource),
						reinterpret_cast<void *>(velocity_resource), recorded ? 1 : 0,
						recorded ? "ok" : mv::last_error());
				}
				if (tracing && recorded)
					g_named_recorded.fetch_add(1, std::memory_order_relaxed);

				if (recorded)
				{
					mark(5, "recorded");

					// DLSS evaluation, opt-in and strictly after the motion-vector resolve —
					// it consumes that resolve's output.
					//
					// Everything here is looked up by REGISTER and liveness-checked, for the
					// same reason the resolve is: ReShade's view->resource map outlives the
					// resource, and a dead pointer here would fault inside the driver.
					// Only ever replace a pass that has proved it owns the temporal history.
					// Waiting a few frames for that proof costs nothing; picking the wrong pass
					// means DLSS's output goes somewhere the frame never reads.
					std::uint64_t pinned = g_ngx_pass_hash.load(std::memory_order_relaxed);
					// A pin must not outlive its pass. The TAA permutation — and therefore the
					// hash — changes with the screen percentage (see g_ngx_pass_last_frame), so
					// a pin taken at one ratio silently refuses the real pass at every other
					// ratio for the rest of the session. Release it once its pass has gone
					// quiet for kPinStaleFrames presents and let the round-trip test re-choose.
					const std::uint64_t now = g_present_frame.load(std::memory_order_relaxed);
					if (pinned == hash)
					{
						g_ngx_pass_last_frame.store(now, std::memory_order_relaxed);
					}
					else if (pinned != 0 &&
						now > g_ngx_pass_last_frame.load(std::memory_order_relaxed) +
							kPinStaleFrames)
					{
						std::uint64_t expected = pinned;
						if (g_ngx_pass_hash.compare_exchange_strong(expected, 0))
							STRAY_LOG_WARN("DLSS pin RELEASED: 0x%016llx has not dispatched for "
								"%llu presents. UE4 recompiles the TAA permutation when the "
								"screen percentage changes (CLAUDE.md §2.3), so the pinned hash "
								"can stop existing mid-session. Re-choosing a pass.",
								static_cast<unsigned long long>(pinned),
								static_cast<unsigned long long>(kPinStaleFrames));
						pinned = 0;
					}
					// An explicitly named pass wins outright over the round-trip heuristic.
					const bool eligible = g_ngx_pass_override != 0
						? (hash == g_ngx_pass_override)
						: (pinned != 0 ? (pinned == hash) : owns_temporal_history(hash));
					if (g_ngx_evaluate && ngx::status().super_sampling_available && !eligible)
					{
						if (pinned != 0)
						{
							log_gate_refusal(hash, kGatePinnedElsewhere);
						}
						else if (!g_ngx_waiting_logged)
						{
							g_ngx_waiting_logged = true;
							STRAY_LOG_INFO("DLSS is waiting for a pass to prove it owns the "
								"temporal history before replacing anything.");
							log_gate_refusal(hash, kGateNoRoundTrip);
						}
					}
					// Dry run. Mode 2 suppresses everything we match; mode 1 only the pinned
					// pass. Either way nothing is written in its place.
					if (g_ngx_dry_run == 2 && dry_run_phase_active())
					{
						if (!g_dry_run_mode2_logged)
						{
							g_dry_run_mode2_logged = true;
							STRAY_LOG_WARN("DRY RUN (all): suppressing EVERY structurally matched "
								"pass and writing nothing. If the image is unchanged, the matcher "
								"never sees the pass that draws the picture.");
						}
						suppress_engine_dispatch = true;
					}
					else if (g_ngx_dry_run == 1 && eligible && dry_run_phase_active())
					{
						std::uint64_t expected = 0;
						// Stamp the pin's liveness BEFORE publishing it: a pin whose
						// last-seen frame is still 0 would be released by the staleness
						// check the very next time another candidate dispatches.
						g_ngx_pass_last_frame.store(now, std::memory_order_relaxed);
						if (g_ngx_pass_hash.compare_exchange_strong(expected, hash))
							STRAY_LOG_WARN("DRY RUN: suppressing pass 0x%016llx and writing "
								"NOTHING. If the image is unchanged, this pass does not drive "
								"the picture.", static_cast<unsigned long long>(hash));
						suppress_engine_dispatch = true;
					}

					if (g_ngx_evaluate && !g_ngx_dry_run &&
						ngx::status().super_sampling_available && eligible)
					{
						// Decide WHICH of the two colour slots is scene colour.
						//
						// colour_srv_a and _b are scene colour and history in an order the
						// signature cannot determine. Last frame's u0 identifies the history
						// (CLAUDE.md §2.9), so the other slot is the scene colour DLSS needs.
						//
						// If we cannot tell them apart we must SKIP, not guess. Feeding DLSS the
						// history instead of the scene colour produces a plausible-looking image
						// that never converges — the worst kind of bug here, because it looks
						// like it works.
						// A candidate must be a real 2D colour texture of at least the render
						// rect. NGX rejects anything else outright — "input Color parameter
						// needs to be Tex2D resource" — and the slots can legitimately hold a
						// buffer, or the 1x1 BlackDummy UE4 substitutes on a camera cut.
						// describe() records width/height as 0 for a buffer, so this excludes
						// those too.
						// Find the output first: the scene colour must MATCH it.
						//
						// TAA reads and writes the same kind of buffer, so the scene colour has
						// the output's exact format and dimensions. The previous test — live,
						// 2D, at least render-size — accepted almost anything, and in the game
						// it picked an unrelated render target: the result was recognisably a
						// different view of the world rather than a degraded version of the
						// right one.
						const BoundTexture *out_tex = nullptr;
						for (const auto &u : b.uavs)
						{
							if (u.slot == m.output_uav && icept::backend()->is_resource_live(u.resource))
								out_tex = &u;
						}

						// Prefer the SHADER'S OWN register assignment over guessing from
						// formats and sizes.
						//
						// FTAAStandaloneCS binds fixed registers, documented in CLAUDE.md §2.3
						// from the pass's own DXBC: t1 InputSceneColor, t2 SceneDepthTexture,
						// t3 GBufferVelocityTexture, t5 HistoryBuffer_0, u0 OutComputeTex_0.
						// Those come from TAAStandalone.usf, so they are what the shader reads —
						// not an inference about what a colour buffer usually looks like.
						//
						// Every heuristic tried here has picked the wrong resource at least once:
						// "not last frame's u0" chose the history, "matches the output" chose a
						// full-res buffer under upsampling. A register is not a guess.
						//
						// Only trusted on a confirmed hash match, because the register assignment
						// belongs to THAT permutation; §2.3's table was read from the upsampling
						// variant. Anything else falls back to the heuristics below, which are
						// weaker but not permutation-specific.
						constexpr std::uint32_t kSceneColourReg = 1;   // t1, CLAUDE.md §2.3
						constexpr std::uint32_t kHistoryReg = 5;       // t5, CLAUDE.md §2.3
						const bool trust_registers =
							m.verdict == MatchVerdict::hash_and_structural;

						std::uint64_t reg_colour = 0;
						if (trust_registers)
						{
							for (const auto &t : b.srvs)
							{
								if (t.slot == kSceneColourReg && icept::backend()->is_resource_live(t.resource))
									reg_colour = t.resource;
							}
							if (!g_ngx_registers_logged)
							{
								g_ngx_registers_logged = true;
								STRAY_LOG_INFO("Using §2.3 register map (hash confirmed): "
									"colour=t%u history=t%u -> colour=%p",
									kSceneColourReg, kHistoryReg,
									reinterpret_cast<void *>(reg_colour));
							}
						}

						// The scene colour is at RENDER resolution, NOT the output's.
						//
						// I had this backwards. Matching the colour against the OUTPUT works only
						// at 1:1; under temporal upsampling the inputs are render-res and the
						// output is display-res (measured: inputs 1920x1080, u0 3840x2160). That
						// filter therefore rejected the real scene colour and accepted some
						// full-res buffer instead, so DLSS got InRenderSubrectDimensions of
						// 1920x1080 with a 3840x2160 colour texture and sampled the wrong quarter
						// of it — a coherent but wrong image, "the cat sideways".
						//
						// Match the render rect, which is what DLSS is told the inputs are, and
						// require the same format as the output since TAA's colour and result are
						// the same buffer kind. Width/height of 0 means a buffer, not a texture.
						const auto colour_candidate = [&](const BoundTexture &t) {
							return out_tex != nullptr && icept::backend()->is_resource_live(t.resource) &&
								t.width == render_w && t.height == render_h &&
								t.format == out_tex->format && t.width > 0 && t.height > 0;
						};

						std::uint64_t slot_a = 0, slot_b = 0;
						for (const auto &t : b.srvs)
						{
							if (t.slot == m.colour_srv_a && colour_candidate(t))
								slot_a = t.resource;
							else if (t.slot == m.colour_srv_b && colour_candidate(t))
								slot_b = t.resource;
						}

						std::uint64_t history = 0;
						{
							const auto prev = g_prev_output.find(hash);
							if (prev != g_prev_output.end())
								history = prev->second;
						}

						std::uint64_t colour_handle = reg_colour;
						const char *colour_reason = reg_colour != 0 ? "register t1" : "ok";
						if (reg_colour != 0)
						{
							// Already decided by register; nothing to infer.
						}
						else if (slot_a != 0 && slot_b != 0 && history != 0)
						{
							if (slot_a == history)      colour_handle = slot_b;
							else if (slot_b == history) colour_handle = slot_a;
							else                        colour_reason = "neither slot is last frame's u0";
						}
						else if (slot_a != 0 && slot_b == 0)
						{
							// Only one live colour slot: on a camera cut UE4 substitutes the 1x1
							// BlackDummy for history, so the survivor is the scene colour.
							colour_handle = slot_a;
						}
						else if (slot_b != 0 && slot_a == 0)
						{
							colour_handle = slot_b;
						}
						else if (history == 0)
						{
							colour_reason = "no history seen yet, cannot tell colour from history";
						}
						else
						{
							colour_reason = "no colour slot is a Tex2D of at least the render rect";
						}

						ID3D12Resource *colour =
							reinterpret_cast<ID3D12Resource *>(colour_handle);
						ID3D12Resource *output = nullptr;
						for (const auto &u : b.uavs)
						{
							if (u.slot == m.output_uav && icept::backend()->is_resource_live(u.resource))
								output = reinterpret_cast<ID3D12Resource *>(u.resource);
						}

						ngx::FeatureDesc fd;
						fd.render_width = render_w;
						fd.render_height = render_h;
						fd.output_width = m.output_width ? m.output_width : render_w;
						fd.output_height = m.output_height ? m.output_height : render_h;

						// The two dimensions come from DIFFERENT sources — render from the View
						// CB's view rect, output from this dispatch's coverage — so they can
						// disagree, and several passes match structurally with different output
						// sizes. NGX rejects input > output outright:
						//   "Input dimensions (2560 x 1440) must be <= output dimensions
						//    (1280 x 720)"
						// which is a half-res pass being handed the full-res view rect. Skip
						// those rather than thrashing CreateFeature against them.
						// A real primary-view upscale also PRESERVES THE ASPECT RATIO and lands
						// inside DLSS's range. Measured 2026-09-01: without this the creates
						// included 1024x176 (5.8:1), 512x88, 512x479 and, in an earlier session,
						// 248x248 and 1016x1016 — cubemap faces and reflection captures, which
						// bind the same shapes as FTAAStandaloneCS. Each cost an NGX feature
						// create/release, i.e. a frame spike, and put a differently-oriented view
						// of the scene on the display chain ("the cat sideways").
						//
						// This test lives HERE, not in match_taa_dispatch, because the two use
						// different sources: the matcher sees the depth SRV's extent while the
						// descriptor's render size comes from the View CB's view rect (see the
						// comment above). Gating the matcher left this path untouched — the
						// bogus creates continued with zero rejections logged.
						bool shape_ok = true;
						if (fd.render_width > 0 && fd.render_height > 0 &&
							fd.output_width > 0 && fd.output_height > 0)
						{
							const double in_aspect = static_cast<double>(fd.render_width) /
								static_cast<double>(fd.render_height);
							const double out_aspect = static_cast<double>(fd.output_width) /
								static_cast<double>(fd.output_height);
							// 4% absorbs UE4's 8-pixel tile quantisation at every ratio we run;
							// a genuine mismatch (5.8:1 against 1.78:1) is out by 225%.
							if (in_aspect > out_aspect * 1.04 || in_aspect < out_aspect * 0.96)
								shape_ok = false;
							// Ultra Performance is 3x linear; 3.5 leaves room for a future mode.
							if (static_cast<double>(fd.output_width) /
								static_cast<double>(fd.render_width) > 3.5)
								shape_ok = false;
						}

						const bool dims_ok = shape_ok &&
							fd.output_width >= fd.render_width &&
							fd.output_height >= fd.render_height &&
							fd.render_width > 0 && fd.output_width > 0;

						if (!shape_ok)
							log_gate_refusal(hash, kGateNotPrimaryView);
						if (!dims_ok && !g_ngx_dims_logged)
						{
							g_ngx_dims_logged = true;
							STRAY_LOG_INFO("Skipping DLSS for this pass: render %ux%u > output "
								"%ux%u, so it is not the full-res pass we want.",
								fd.render_width, fd.render_height,
								fd.output_width, fd.output_height);
						}

						if (dims_ok && colour != nullptr && output != nullptr &&
							ngx::ensure_feature(native, fd))
						{
							// Our resolve just wrote the motion vectors as a UAV; NGX reads
							// them as a shader resource. Without this the state is simply
							// wrong, and vkd3d validates none of it. (CLAUDE.md §3)
							NGX_TRACE("%s", "barrier mv -> SRV");
							mv::transition_output(native, /*to_shader_resource=*/true);

							ngx::EvaluateInputs ei;
							ei.color = colour;
							ei.depth = reinterpret_cast<ID3D12Resource *>(depth_resource);
							ei.motion_vectors = mv::output();
							ei.output = output;
							// TemporalAAParams.zw, straight through. (CLAUDE.md §2.7)
							ei.jitter_x = view.temporal_aa_params.z;
							ei.jitter_y = view.temporal_aa_params.w;
							ei.render_width = render_w;
							ei.render_height = render_h;
							// The three signals ORed inside is_camera_cut: View.CameraCut,
							// TemporalAAJitter.zw == .xy, and a 1x1 history/velocity dummy.
							ei.reset = ue4::is_camera_cut(view, m.camera_cut_dummies);
							ei.pre_exposure = view.pre_exposure;

							// [STRAYDLSS] NgxExposure=texture: the engine's eye-adaptation
							// texture (register t0 of this very dispatch — 1x1 RGBA32F,
							// present every frame, CLAUDE.md §2.3) becomes DLSS's exposure
							// source; .x is UE's ExposureScale and the official plugin
							// passes the texture unmodified alongside PreExposure
							// (ngx_backend.hpp carries the full derivation). Liveness-
							// checked like every capture. A miss cannot fall back to
							// AutoExposure per frame — that is a creation-time flag — so
							// the frame gets DLSS's default exposure 1.0 instead, loudly,
							// once; a PERSISTENT miss means flip NgxExposure back to auto.
							if (ngx::exposure_from_texture())
							{
								const std::uint64_t eye = find_eye_adaptation_srv(b.srvs);
								if (eye != 0 && icept::backend()->is_resource_live(eye))
								{
									ei.exposure = reinterpret_cast<ID3D12Resource *>(eye);
								}
								else
								{
									static bool s_eye_missing_logged = false;
									if (!s_eye_missing_logged)
									{
										s_eye_missing_logged = true;
										STRAY_LOG_WARN("NgxExposure=texture but no live 1x1 "
											"RGBA32F eye-adaptation SRV on this dispatch "
											"(found=%d live=%d); DLSS gets default exposure "
											"1.0 for such frames. If this persists, set "
											"NgxExposure=auto. First occurrence only.",
											eye != 0 ? 1 : 0,
											eye != 0 && icept::backend()->is_resource_live(eye) ? 1 : 0);
									}
								}
							}

							NGX_TRACE("evaluate colour=%p depth=%p mv=%p out=%p",
								static_cast<void *>(ei.color), static_cast<void *>(ei.depth),
								static_cast<void *>(ei.motion_vectors),
								static_cast<void *>(ei.output));
							if (tracing)
								g_named_evaluated.fetch_add(1, std::memory_order_relaxed);
							if (!g_ngx_inputs_logged)
							{
								g_ngx_inputs_logged = true;
								STRAY_LOG_INFO("DLSS render=%ux%u output=%ux%u — colour must be "
									"render-res, output display-res.", render_w, render_h,
									fd.output_width, fd.output_height);
								STRAY_LOG_INFO("DLSS inputs: colour=%p depth=%p mv=%p out=%p "
									"(out %ux%u fmt=%d)", static_cast<void *>(ei.color),
									static_cast<void *>(ei.depth),
									static_cast<void *>(ei.motion_vectors),
									static_cast<void *>(ei.output),
									out_tex ? out_tex->width : 0, out_tex ? out_tex->height : 0,
									out_tex ? static_cast<int>(out_tex->format) : -1);
							}
							const std::uint64_t eval_no =
								g_evaluate_attempts.fetch_add(1, std::memory_order_relaxed) + 1;
							if (input_dump::wants(eval_no))
							{
								// Both inputs sit in NON_PIXEL_SHADER_RESOURCE here — the
								// engine transitioned them for the dispatch we are replacing.
								input_dump::capture(native_device, native, ei.color,
									D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, "colour",
									eval_no);
								input_dump::capture(native_device, native, ei.depth,
									D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, "depth",
									eval_no);
							}

							// EXPOSURE DIAGNOSIS (NgxExposure=texture + NgxDumpInputs=1): DLSS
							// silently reverts to auto-exposure in the live run, which means it
							// received our texture and REJECTED it — likeliest an invalid
							// VALUE (0/negative/NaN => DLSS auto-exposes). The API side is
							// confirmed (flags 0x0b, one feature, no fallback warnings), so the
							// missing evidence is the texel itself. Capture it at the first
							// exposure evaluate and at eval 300/600, and log what we pass NGX
							// and the state we ASSUME t0 is in (we do NOT barrier it — it is
							// already NON_PIXEL_SHADER_RESOURCE from the game's own compute
							// dispatch, which is exactly what NGX needs to read it).
							if (ngx::exposure_from_texture() && input_dump::enabled())
							{
								static bool s_first_done = false;
								const bool at_point = !s_first_done || eval_no == 300 ||
									eval_no == 600;
								if (at_point)
								{
									s_first_done = true;
									STRAY_LOG_INFO("EXPOSURE eval %llu: exposure=%p (colour=%p "
										"depth=%p) InPreExposure=%.6f InExposureScale=%.6f "
										"([STRAYDLSS] NgxExposureScale; DLSS indicator's "
										"'Exposure level' should echo this) assumedState="
										"NON_PIXEL_SHADER_RESOURCE barriered=no (same state and "
										"discipline as colour/depth, which NGX consumes fine) "
										"createFlags=0x0b",
										static_cast<unsigned long long>(eval_no),
										static_cast<void *>(ei.exposure),
										static_cast<void *>(ei.color),
										static_cast<void *>(ei.depth), ei.pre_exposure,
										ngx::exposure_scale());
									if (ei.exposure != nullptr)
									{
										char label[32];
										std::snprintf(label, sizeof(label), "exposure_%llu",
											static_cast<unsigned long long>(eval_no));
										// Same state we hand NGX: the game bound t0 as a
										// compute SRV, so NON_PIXEL_SHADER_RESOURCE.
										input_dump::capture_texel(
											native_device, native, ei.exposure,
											D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
											label, eval_no);
									}
									else
									{
										STRAY_LOG_WARN("EXPOSURE eval %llu: ei.exposure is NULL "
											"despite NgxExposure=texture — the finder returned "
											"nothing live this frame (see any 'no live 1x1' "
											"warning above).",
											static_cast<unsigned long long>(eval_no));
									}
								}
							}
							bool ok;
							if (g_ngx_paint)
							{
								// Paint instead of evaluating: if the screen turns magenta the
								// output handle is right and the fault is inside the evaluate;
								// if the scene stays frozen the handle is wrong and nothing
								// about DLSS's inputs matters yet.
								ok = mv::paint(native, ei.output);
							}
							else
							{
								// RR-first when [STRAYDLSS] NgxRR=2; SR is the safety net
								// for ANY missing precondition, every frame.
								ok = false;
								const int rrm =
									g_ngx_rr_mode.load(std::memory_order_relaxed);
								if (rrm == 2 || rrm == 3)
								{
									ok = try_evaluate_rr(native_device, native, ei, fd, view);
									if (ok)
										g_rr_evaluates.fetch_add(1,
											std::memory_order_relaxed);
									else
										g_rr_fallbacks.fetch_add(1,
											std::memory_order_relaxed);
									// RR-1: arm/disarm SSD suppression from this outcome.
									update_rr1_arm(ok);
								}
								if (!ok)
									{
										perf::Scope perf_sr(perf::kNgxSr);
										ok = ngx::evaluate(native, ei);
									}

								// DLSS Neural Rendering (NGX feature 18), strictly AFTER
								// SR/RR: it consumes the image they just wrote into u0 and,
								// once validated non-degenerate, replaces it. Any failure
								// leaves the SR/RR image exactly as it is. (src/ngx_nr.hpp)
								if (ok && nr::enabled())
								{
									// PUBLISH THE GUIDES REGARDLESS OF THE HOOK MODE. The
									// post-tonemap sites have no way of their own to find the
									// depth and the motion vectors — this is the only place in
									// the frame where both are known-good and known-fresh — and
									// publishing unconditionally means flipping NgxNRHook takes
									// effect on the next frame rather than on the next launch.
									// `ei.reset` is the camera-cut OR from §2.8 and MUST travel
									// with them: feature 18 keeps its own temporal history.
									nrhook::note_guides(
										g_present_frame.load(std::memory_order_relaxed),
										ei.depth, ei.motion_vectors, ei.render_width,
										ei.render_height, ei.reset);

									// ...but only RUN here when the hook is at this site.
									// `present` and `preui` run the identical nr::apply later in
									// the same frame, from src/nr_hook.cpp, on a colour target
									// nothing carries into the engine's temporal state — which
									// is the entire point of moving it.
									if (nrhook::hook_mode() == nrplan::HookMode::taa)
									{
										perf::Scope perf_nr(perf::kNgxNr);

										// END-OF-FRAME HISTORY RESTORE, half one of two.
										//
										// `ei.output` is `u0`, and UE 4.27 makes that ONE resource
										// both this frame's scene colour AND the next frame's
										// HistoryBuffer[0] (TemporalAA.cpp:696 / :969), which
										// ScreenSpaceRayTracing.cpp:596-620 reads directly on the
										// NEXT frame. So NR's residual compounds through the
										// engine's temporal state — the measured slow drift.
										//
										// Copy the PRISTINE image aside here, before the decode
										// writes into it, and src/nr_history.cpp puts it back at
										// present, after every same-frame consumer has run. `u0`
										// is in UNORDERED_ACCESS at this exact point (the SR/RR
										// evaluate just wrote it through a UAV), which is the one
										// state the snapshot needs and the one place in the frame
										// where we know it for certain.
										//
										// The rect is the one the decode will write — ngx_nr's
										// `cw`/`ch`, i.e. the same fd.output_* handed to apply()
										// below — never the texture's allocation.
										// ([STRAYDLSS] NgxNRRestoreHistory; default ON.)
										nrhist::snapshot(native_device, native, ei.output,
											fd.output_width, fd.output_height);

										nr::ApplyInputs ni;
										ni.site = nr::Site::taa_dispatch;
										ni.image = ei.output;
										ni.render_color = ei.color;
										ni.depth = ei.depth;
										ni.motion_vectors = ei.motion_vectors;
										ni.render_width = ei.render_width;
										ni.render_height = ei.render_height;
										ni.output_width = fd.output_width;
										ni.output_height = fd.output_height;
										ni.reset = ei.reset;
										// View row 135.z. Only the TAA site's codec consumes it,
										// and only when NgxNRTrackExposure is on; a frame whose
										// View CB did not decode leaves it 0 and the codec falls
										// back to its static scale.
										// Row 135.z IS OneOverPreExposure. SceneRendering.cpp:1563-1564
										// assigns the pair on ADJACENT LINES from the same float:
										//   PreExposure        = PreExposure;
										//   OneOverPreExposure = 1.f / PreExposure;
										// so their product is 1.0 by construction and the layout
										// is right. An earlier "fix" here derived the reciprocal
										// instead, on the strength of a measured product of 3.05
										// — but that was two floats from two DIFFERENT reads, not
										// a layout error. Reading the row back restores the
										// self-check below, which is the only runtime detector we
										// have for a genuinely bad read.
										// Once per session, print the WHOLE of row 135 from one read
										// so the [derived] offset validates itself. Expect
										// (~1.4e-45, P, 1/P, 0.0): .x is an int32 MSAA count
										// reinterpreted as float and .w is padding. If .x is a
										// normal float or .w is not zero, we are reading the wrong
										// row and everything derived from it is suspect.
										static bool s_row135_logged = false;
										if (view_ok && !s_row135_logged)
										{
											s_row135_logged = true;
											const auto &r = view.pre_exposure_row;
											const bool x_denormal = r.x > 0.0f && r.x < 1e-30f;
											STRAY_LOG_WARN("View row 135 (one read): x=%.9g y=%.6f "
												"z=%.6f w=%.9g | y*z=%.6f (want 1.0) | x denormal=%d "
												"(want 1, it is an int32 MSAA count) | w==0=%d "
												"(want 1, it is padding). All three must hold or "
												"kPreExposureRow is the wrong offset.",
												static_cast<double>(r.x), static_cast<double>(r.y),
												static_cast<double>(r.z), static_cast<double>(r.w),
												static_cast<double>(r.y) * static_cast<double>(r.z),
												x_denormal ? 1 : 0, r.w == 0.0f ? 1 : 0);
										}
										ni.one_over_pre_exposure =
											view_ok ? view.one_over_pre_exposure : 0.0f;
										// The pair being reciprocals is what makes this free: a
										// product that is not 1 means the read is bad, whatever
										// the cause.
										ni.pre_exposure_ok =
											view_ok && ue4::pre_exposure_plausible(view);
										const bool nr_applied = nr::apply(
											native_device, native, ni);
										// Half two of the history restore: only a frame NR really
										// modified needs putting back. A refusal (warmup,
										// validating, degenerate, codec failure) leaves `u0`
										// exactly as SR/RR wrote it, and restoring it would be a
										// 66 MB copy of identical pixels.
										nrhist::note_nr_applied(nr_applied);
									}
								}
							}
							if (ok)
							{
								g_ngx_evaluated_once.store(true, std::memory_order_relaxed);
								std::uint64_t expected = 0;
								// See the dry-run pin: stamp liveness before publishing.
								g_ngx_pass_last_frame.store(now, std::memory_order_relaxed);
								if (g_ngx_pass_hash.compare_exchange_strong(expected, hash))
									STRAY_LOG_INFO("DLSS pinned to pass 0x%016llx; no other pass "
										"will be replaced.",
										static_cast<unsigned long long>(hash));
								// Suppress the engine's own TAA dispatch for this pass.
								//
								// Not optional. We have just written DLSS's result into u0; if
								// the engine then runs its TAA it overwrites that, so our work
								// is discarded AND both passes write the same UAV in one command
								// list with no barrier between them — a write-after-write hazard
								// on a resource the next frame reads back as history.
								// (CLAUDE.md §3: return true to skip the engine's dispatch.)
								suppress_engine_dispatch = true;
							}
							NGX_TRACE("evaluate returned %d", ok ? 1 : 0);
							if (ok && input_dump::wants(eval_no))
							{
								// The output is in UAV state; NGX has just written it.
								input_dump::capture(native_device, native, ei.output,
									D3D12_RESOURCE_STATE_UNORDERED_ACCESS, "output", eval_no);
							}

							// Back to UAV for next frame's resolve.
							mv::transition_output(native, /*to_shader_resource=*/false);
							NGX_TRACE("%s", "barrier mv -> UAV done");
							if (!g_ngx_logged_once)
							{
								g_ngx_logged_once = true;
								STRAY_LOG_INFO("DLSS evaluate %s: %ux%u -> %ux%u jitter=%.4f,%.4f "
									"reset=%d preExposure=%.3f", ok ? "OK" : "FAILED",
									render_w, render_h, fd.output_width, fd.output_height,
									ei.jitter_x, ei.jitter_y, ei.reset ? 1 : 0, ei.pre_exposure);
								if (!ok)
									STRAY_LOG_ERROR("  %s", ngx::last_error());
							}
						}
						else if (dims_ok && !g_ngx_skip_logged)
						{
							g_ngx_skip_logged = true;
							STRAY_LOG_WARN("DLSS evaluate skipped: colour=%p (%s) output=%p "
								"feature=%s", static_cast<void *>(colour), colour_reason,
								static_cast<void *>(output), ngx::last_error());
						}
					}
					if (!g_resolve_ran)
					{
						g_resolve_ran = true;
						STRAY_LOG_INFO("MV resolve ran: %ux%u R16G16_FLOAT from depth=t%u "
							"velocity=t%u", render_w, render_h,
							m.depth_srv, m.velocity_srv);
					}

					// Our pass replaced the root signature, PSO and descriptor heaps on the
					// game's own command list. D3D12 has no state getters, so the state is
					// re-applied from what ReShade tracked. ORDER MATTERS: the heaps must go
					// back first, because the tracked descriptor TABLES are GPU handles into
					// the game's heap and re-binding them while our heap is current is an
					// invalid binding — UE4 dies with LowLevelFatalError, not an error you can
					// read. (docs/RESEARCH.md §3.5)
					// Put the game's compute state back, in full and natively.
					//
					// state_block::apply used to be called here and is deliberately gone: it
					// registers no push_descriptors handler, so it cannot replay the root
					// CBVs UE4 binds its uniform buffers through, and it re-binds tables via
					// ReShade's proxy whose root-signature cache we desynced by setting ours
					// natively. Both faults were silent, and together they are the corruption.
					if (g_restore_state)
					{
						NGX_TRACE("%s", "restore begin");
						{
							perf::Scope perf_restore(perf::kRestore);
							icept::backend()->restore_game_compute_state(ctx);
						}
						NGX_TRACE("%s", "restore done");
						mark(6, "state-restored");
					}
				}
				else if (!g_resolve_failed_logged)
				{
					g_resolve_failed_logged = true;
					STRAY_LOG_ERROR("MV resolve record failed: %s", mv::last_error());
				}
			}
			else if (!g_resolve_failed_logged)
			{
				g_resolve_failed_logged = true;
				STRAY_LOG_ERROR("MV resolve init failed: %s", mv::last_error());
			}
		}
	}

	// The decisive test for which pass owns the temporal history: UE4 extracts a TAA pass's
	// u0 and binds it back as an SRV on the NEXT frame. Only the real TAA does that.
	// (CLAUDE.md §2.9)
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		const auto prev = g_prev_output.find(hash);
		if (prev != g_prev_output.end() && prev->second != 0 && !g_roundtrip_logged[hash])
		{
			for (const auto &t : b.srvs)
			{
				if (t.resource == prev->second)
				{
					g_roundtrip_logged[hash] = true;
					g_roundtrip_seen[hash] = true;
					STRAY_LOG_INFO("  HISTORY ROUND-TRIP: 0x%016llx bound last frame's u0 back "
						"at t%u — this pass owns the temporal history",
						static_cast<unsigned long long>(hash), t.slot);
					break;
				}
			}
		}
		for (const auto &t : b.uavs)
			if (t.slot == 0)
				g_prev_output[hash] = t.resource;
	}

	// Suppress ONLY when DLSS actually produced this pass's output. Every other path — no
	// match, no resolve, evaluate skipped or failed — must let the engine run, or the frame
	// simply loses its temporal anti-aliasing.
	return suppress_engine_dispatch;
}

void named_pass_counters(std::uint32_t out[5])
{
	out[0] = g_named_seen.load(std::memory_order_relaxed);
	out[1] = g_named_phaseb.load(std::memory_order_relaxed);
	out[2] = g_named_live.load(std::memory_order_relaxed);
	out[3] = g_named_recorded.load(std::memory_order_relaxed);
	out[4] = g_named_evaluated.load(std::memory_order_relaxed);
}

void resolve_counters(std::uint32_t &attempts, std::uint32_t &skipped_stale)
{
	attempts = g_resolve_attempts.load(std::memory_order_relaxed);
	skipped_stale = g_resolve_skipped_stale.load(std::memory_order_relaxed);
}

} // namespace stray_dlss::taa_hook
