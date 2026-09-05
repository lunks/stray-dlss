#include "taa_hook.hpp"

#include "core/exposure_plan.hpp"
#include "core/feature_recreate.hpp"
#include "core/reset_plan.hpp"
#include "engine_seam_hook.hpp"
#include "view_params_hook.hpp"
#include "pool_name_hook.hpp"
#include "u0_rhi_hook.hpp"
#include "exposure_texture.hpp"

#include "ngx_nr.hpp"
#include "nr_hook.hpp"
#include "perf.hpp"

#include "intercept/backend.hpp"
#include "log.hpp"
#include "input_dump.hpp"
#include "mv_mask.hpp"
#include "gbuffer_resolve.hpp"
#include "mv_resolve.hpp"
#include "ngx_fg.hpp"
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
// Row 135's self-check, counted over every frame that found a View CB. The buffer is located by
// SEARCH (`view_params_plausible` is a shape test that the wrong buffer can satisfy), so this is
// the one cheap thing that says whether the search is right: row 135 must read
// (denormal, P, 1/P, 0.0) and `y*z == 1.0` is true BY CONSTRUCTION, so it cannot survive a wrong
// offset or a wrong buffer. CLAUDE.md §2.6.
std::atomic<std::uint64_t> g_view_row135_ok{ 0 };
std::atomic<std::uint64_t> g_view_row135_bad{ 0 };
// Candidates that decoded as A View buffer but described a DIFFERENT view than this dispatch.
// Non-zero is the fix working: each one is a frame the old search would have taken the wrong
// View for, and refused the real TAA dispatch over.
std::atomic<std::uint64_t> g_view_cb_rejected{ 0 };
// THE SEARCH'S OWN COST. Every bound root CBV the scan TRIES — one describe_resource and one
// 2448-byte buffer read each — which is the work identity from the engine would replace. Against
// the dispatch count it is candidates-per-dispatch, so "deleting the search saves X" becomes
// arithmetic over a measurement rather than over a guess. Read it beside the criterion-4 note in
// docs/RESEARCH-ENGINE-TAA-HOOK.md §15.4: the shadow's expensive half is the SRV/UAV table walk,
// which colour-by-register and the u0 output still require, so this number bounds the saving from
// above and is expected to be small against `shadow-copy`.
std::atomic<std::uint64_t> g_view_cb_reads{ 0 };
// THE OTHER HALF OF THE WRONG-VIEW BUG, AND IT IS NOW GATED (report §16.5, facts §36.20).
// `view_fits_dispatch` bounds a candidate only from ABOVE, so an impostor whose rect is too
// SMALL — a shadow, planar-reflection, cubemap-face or scene-capture view — passed plausibility,
// row 135 and the fit bound alike, and one sitting on a lower root parameter won the search.
// It counted 162 in the measured session and gated nothing; the consequence was 37 of 62 DLSS
// features created at rects like 64x41 -> 3840x2160, i.e. the top-left corner of the frame
// magnified over the whole screen. This counter now means "a candidate REJECTED for being below
// the engine's own kMinTAAUpsampleResolutionFraction, so the search kept looking" — each one is
// the fix firing, not a frame we got wrong.
std::atomic<std::uint64_t> g_view_suspect_small{ 0 };
// More than one plausible View survived the filter on one dispatch, so the pick was a choice.
// SPLIT BY WHETHER THE ENGINE CLAIMED THE DISPATCH. A session offers ~10 000 look-alikes beside
// ~9 000 real upscales, so an undifferentiated count is diluted by an order of magnitude and
// cannot answer the actual question: was DLSS SR ITSELF ever fed the wrong view?
std::atomic<std::uint64_t> g_view_amb_claimed{ 0 };
std::atomic<std::uint64_t> g_view_amb_other{ 0 };
std::atomic<std::uint32_t> g_view_amb_logged{ 0 };
// THE LETTERBOX HOLD (src/core/feature_recreate.hpp). `held` counts frames that evaluated at the
// LIVE feature's extent instead of rebuilding for a shrinking view rect - each one a frame that
// keeps DLSS SR, and therefore NR, running through a scripted transition. The refusals say why a
// frame could not be held, and `originMoved` is the one that would mean the whole idea is wrong
// for this title.
std::atomic<std::uint64_t> g_hold_frames{ 0 };
std::atomic<std::uint64_t> g_hold_refused[static_cast<std::size_t>(core::HoldRefusal::count)];
std::atomic<std::uint32_t> g_hold_logged{ 0 };
bool g_letterbox_hold = true;
std::atomic<std::uint32_t> g_resolve_skipped_stale{ 0 };
bool g_render_size_logged = false;
// [STRAYDLSS] NgxEvaluate. Off by default: this is the first switch that can change what the
// player sees, so it is opt-in and separate from EnableNGX (which only brings NGX up).
bool g_ngx_evaluate = false;
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
// One WARN per pass when the engine seam and the heuristic matcher disagree (observe mode),
// and one per pass per assertion in authoritative mode: an announced pass whose hash is not in
// the cooked table, or whose dispatched rect is not the one the engine announced.
std::unordered_map<std::uint64_t, bool> g_seam_disagreement_logged;
std::unordered_map<std::uint64_t, bool> g_seam_hash_assert_logged;
std::unordered_map<std::uint64_t, bool> g_seam_rect_assert_logged;
// One WARN per pass when register t1 holds something that cannot be InputSceneColor.
std::unordered_map<std::uint64_t, bool> g_colour_reg_rejected_logged;
bool g_seam_first_engine_claim_logged = false;

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
	kGateNotAnnounced,     // EngineSeam authoritative: the engine announced no primary upscale this fits
	kGateNoSeam,           // EngineSeam authoritative, seam not live, EngineSeamFallback=0
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
	"the engine's ITemporalUpscaler::AddPasses announced no primary temporal upscale this "
	"dispatch fits ([STRAYDLSS] EngineSeam=3). For a look-alike (DOF, light shafts, SSR, water, "
	"a planar reflection) this is the CORRECT outcome and the only line it will ever produce",
	"EngineSeam=3 was requested, the seam is not live, and EngineSeamFallback=0 - DLSS is "
	"refused on every dispatch by configuration (see the ENGINE SEAM MODE line at startup)",
};
std::unordered_map<std::uint64_t, std::uint32_t> g_gate_logged; // hash -> reason bitmask

// --- DLSS Ray Reconstruction (taa_hook.hpp) -----------------------------------------------
//
// [STRAYDLSS] NgxRR: 0 off, 1 probe (ngx_backend does that at feature creation), 2 evaluate.
std::atomic<int> g_ngx_rr_mode{ 0 };
std::atomic<std::uint32_t> g_rr_evaluates{ 0 };
std::atomic<std::uint32_t> g_rr_fallbacks{ 0 };
std::atomic<std::uint32_t> g_rr_reasons[kRrReasonCount] = {};
// One first-occurrence WARN per reason, with the specifics the counter cannot carry. The
// counters give the RATE, this gives the STORY, and conflating the two has cost this project a
// round trip before (CLAUDE.md §2.3, the seam's per-reason counters).
bool g_rr_reason_logged[kRrReasonCount] = {};
bool g_rr_logged_once = false;

// The evaluate-side reasons, i.e. everything that is not the guide set's own verdict. Indices
// continue where rrguides::Refusal stops.
enum RrEvalReason : std::size_t
{
	kRrRowsImplausible = kRrGuideReasonCount + 0, // View row 12-15's rotation failed its check
	kRrResolveNotReady = kRrGuideReasonCount + 1, // gbr::initialise refused (device/alloc)
	kRrResolveFailed   = kRrGuideReasonCount + 2, // gbr::record refused
	kRrCreateFailed    = kRrGuideReasonCount + 3, // ngx::ensure_feature_rr refused
	kRrEvaluateFailed  = kRrGuideReasonCount + 4, // ngx::evaluate_rr returned false
};

void note_rr_reason(std::size_t index, const char *detail)
{
	if (index >= kRrReasonCount)
		return;
	g_rr_reasons[index].fetch_add(1, std::memory_order_relaxed);
	if (g_rr_reason_logged[index])
		return;
	g_rr_reason_logged[index] = true;
	STRAY_LOG_WARN("DLSS RR fell back to SR, reason \"%s\" (first occurrence): %s. SR carries "
		"this frame, so the image is the SR image and nothing is lost - but a reason whose "
		"counter never stops growing on the [rr] line is a configuration problem, not noise.",
		rr_reason_name(index), detail != nullptr ? detail : "");
}

// THE RAY-RECONSTRUCTION EVALUATE, and the whole of its guide sourcing.
//
// Called only when [STRAYDLSS] NgxRR == 2, from the one place in the frame where colour, depth,
// the dense motion vectors and the View are all known-good and known-fresh. Returns true when
// RR carried the frame; false means SR carries it and a NAMED reason has been counted.
//
// IDENTITY AND LIFETIME, spelled out because the project has been bitten by conflating them
// twice (CLAUDE.md §2.3):
//   * IDENTITY of the three inputs is the ENGINE'S OWN, read off FindFreeElement's debug-name
//     argument. It is warranted permanently and needs no re-checking.
//   * LIFETIME is warranted by nothing the engine told us. poolhook::guide_set re-asks OUR
//     resource registry at THIS call, rrguides::judge refuses the frame if any member is not
//     live, and gbr::record then AddRefs all three for as long as the GPU could be reading them.
//     Nothing caches a pointer across a frame boundary.
//
// THE WINDOW THAT REMAINS, stated rather than papered over. The liveness check and the AddRef
// are microseconds apart on THIS thread, but FindFreeElement runs on the render thread while
// this dispatch is recorded on the RHI thread (facts §36.10), so in principle the pool could
// release an element in between and the AddRef would touch freed memory. Two things make that
// not worth a new mechanism: the scene renderer holds GBufferRefCount = 1 across the whole of
// FDeferredShadingSceneRenderer::Render (RESEARCH-RR-GBUFFER.md §1.1, HARD-via-mirror), so the
// pool cannot hand these three elements anywhere while the frame is in flight; and this is
// exactly the discipline mv_resolve has used for depth and velocity for months. If it ever does
// bite, it bites as a fault inside gbr::record, not as a wrong image.
//
// WHERE THIS RECORDS, and it is the one thing a live run must judge. NVIDIA's own UE plugin
// resolves the G-buffer at exactly this point - AddGBufferResolvePass runs at the upscale point,
// DLSSUpscaler.cpp:578-589 (HARD, docs/RESEARCH-RR-GBUFFER.md §2.4) - and UE 4.27 holds the
// G-buffer refcount from before the base pass to after AddPostProcessingPasses (§1.1), so the
// pool cannot hand these elements to anything else while the frame is being rendered.
// AGAINST THAT: a 2026-08-31 measurement of the OLD, heuristically-identified resources found
// their CONTENT already recycled at this same point (~95% of every guide decoded as unlit). That
// was a measurement of resources whose identity was a guess, so it does not transfer - but it is
// not refuted either, and it is the single thing a first run has to look at. The instrument is
// the guide dump below: one look at straydlss_rr_diffuse_*.bin says whether the albedo is the
// scene or is mid-grey everywhere.
bool try_evaluate_rr(ID3D12Device *device, ID3D12GraphicsCommandList *cmd,
                     const ngx::FeatureDesc &fd, const ngx::EvaluateInputs &ei,
                     const ue4::ViewParams &view, std::uint64_t eval_no)
{
	rrguides::Set set;
	poolhook::guide_set(set);

	rrguides::Expect expect;
	expect.frame_now = g_present_frame.load(std::memory_order_relaxed);
	expect.buffer_width = static_cast<std::uint32_t>(view.buffer_size_and_inv_size.x);
	expect.buffer_height = static_cast<std::uint32_t>(view.buffer_size_and_inv_size.y);
	expect.render_width = ei.render_width;
	expect.render_height = ei.render_height;
	expect.supplying = poolhook::supplying();
	expect.hooked = poolhook::hooked();

	const rrguides::Refusal verdict = rrguides::judge(set, expect);
	if (verdict != rrguides::Refusal::none)
	{
		char detail[256];
		std::snprintf(detail, sizeof(detail),
			"A=%#llx(%ux%u fmt%u seen%d ok%d live%d ep%llu) B=%#llx(fmt%u) C=%#llx(fmt%u) "
			"expect %ux%u buffer, %ux%u render, supplying=%d hooked=%d",
			static_cast<unsigned long long>(set.a.resource), set.a.width, set.a.height,
			set.a.dxgi_format, set.a.seen ? 1 : 0, set.a.status_ok ? 1 : 0, set.a.live ? 1 : 0,
			static_cast<unsigned long long>(set.a.epoch),
			static_cast<unsigned long long>(set.b.resource), set.b.dxgi_format,
			static_cast<unsigned long long>(set.c.resource), set.c.dxgi_format,
			expect.buffer_width, expect.buffer_height, expect.render_width,
			expect.render_height, expect.supplying ? 1 : 0, expect.hooked ? 1 : 0);
		note_rr_reason(static_cast<std::size_t>(verdict), detail);
		return false;
	}

	// The NoV geometry. A wrong rotation biases specular albedo everywhere and silently, so the
	// row block is gated rather than trusted: rows 12-15 are mirror-verified but were never
	// among the MEASURED anchors of CLAUDE.md §2.6.
	if (!ue4::world_to_view_rotation_plausible(view.translated_world_to_view))
	{
		note_rr_reason(kRrRowsImplausible,
			"View rows 12-15 (TranslatedWorldToView) do not read as a rigid rotation, so NoV - "
			"and with it the whole specular-albedo guide - would be wrong in a way nothing "
			"downstream can notice");
		return false;
	}

	if (!gbr::initialise(device, ei.render_width, ei.render_height))
	{
		note_rr_reason(kRrResolveNotReady, gbr::last_error());
		return false;
	}

	gbr::ResolveInputs gi;
	gi.gbuffer_a = set.a.resource;
	gi.gbuffer_b = set.b.resource;
	gi.gbuffer_c = set.c.resource;
	gi.render_width = ei.render_width;
	gi.render_height = ei.render_height;
	gi.view_rect_min[0] = view.view_rect_min.x;
	gi.view_rect_min[1] = view.view_rect_min.y;
	// Matrix4::m is a FLAT float[16] with m[r*4+c] == M[r][c] (view_params.hpp), so the two
	// projection scale terms are m[0] and m[5], not m[0][0] and m[1][1].
	gi.proj00 = view.view_to_clip_no_aa.m[0];
	gi.proj11 = view.view_to_clip_no_aa.m[5];
	ue4::nov_rotation_rows(view.translated_world_to_view, gi.world_to_view);

	if (!gbr::record(cmd, gi, /*dispatch_mode=*/2))
	{
		note_rr_reason(kRrResolveFailed, gbr::last_error());
		return false;
	}

	// The guides are UAV-written; NGX reads them as shader resources.
	gbr::transition_outputs(cmd, /*to_shader_resource=*/true);

	bool ok = false;
	if (!ngx::ensure_feature_rr(cmd, fd))
	{
		note_rr_reason(kRrCreateFailed, ngx::last_error());
	}
	else
	{
		ngx::EvaluateInputsRR rr;
		rr.base = ei;
		rr.diffuse_albedo = gbr::diffuse_albedo();
		rr.specular_albedo = gbr::specular_albedo();
		rr.normals_roughness = gbr::normals_roughness();
		rr.roughness = gbr::roughness();
		// Row-major, straight across (ngx_backend.hpp: NGX's expected convention is
		// undocumented and only the specular-MV path we do not feed consumes them).
		std::memcpy(rr.world_to_view, view.translated_world_to_view.m, sizeof(rr.world_to_view));
		std::memcpy(rr.view_to_clip, view.view_to_clip_no_aa.m, sizeof(rr.view_to_clip));
		rr.have_matrices = true;
		rr.frame_time_delta_ms = view.delta_time * 1000.0f;

		if (input_dump::wants(eval_no))
		{
			// The four guides, as NGX receives them. This is the ONLY instrument that
			// distinguishes "RR ran on good guides and did not help" from "RR ran on mid-grey" -
			// the exact ambiguity that made the 2026-08-31 content-recycling finding cost a
			// whole round trip. They are in NON_PIXEL_SHADER_RESOURCE at this point.
			input_dump::capture(device, cmd, rr.diffuse_albedo,
				D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, "rr_diffuse", eval_no);
			input_dump::capture(device, cmd, rr.specular_albedo,
				D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, "rr_specular", eval_no);
			input_dump::capture(device, cmd, rr.normals_roughness,
				D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, "rr_normals", eval_no);
			input_dump::capture(device, cmd, rr.roughness,
				D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, "rr_roughness", eval_no);
		}

		ok = ngx::evaluate_rr(cmd, rr);
		if (!ok)
			note_rr_reason(kRrEvaluateFailed, ngx::last_error());
	}

	// Back to UAV for the next frame's resolve, whether or not the evaluate ran: the state we
	// claimed is the state the resource is in, and leaving it wrong desynchronises OUR own
	// tracking for every later frame.
	gbr::transition_outputs(cmd, /*to_shader_resource=*/false);

	if (ok && !g_rr_logged_once)
	{
		g_rr_logged_once = true;
		STRAY_LOG_INFO("DLSS RR evaluate OK: %ux%u -> %ux%u, guides from the ENGINE'S OWN names "
			"- GBufferA=%#llx B=%#llx C=%#llx, all three from allocation cycle %llu at %ux%u. "
			"This is the first frame Ray Reconstruction has ever carried in this project; "
			"whether the IMAGE is right is a separate question and is for the user's eyes.",
			ei.render_width, ei.render_height, fd.output_width, fd.output_height,
			static_cast<unsigned long long>(set.a.resource),
			static_cast<unsigned long long>(set.b.resource),
			static_cast<unsigned long long>(set.c.resource),
			static_cast<unsigned long long>(set.a.epoch), set.a.width, set.a.height);
	}
	return ok;
}


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
// [STRAYDLSS] StageFile, default ON: a tiny file rewritten on every dispatch attempt (see
// mark() below). Turn this off when chasing a periodic hitch — it is a per-dispatch (so
// roughly per-frame) file write and a plausible suspect, even though a single small write is
// individually cheap.
bool g_stage_file_enabled = false;

// A crash-survivable breadcrumb. The Phase B path dies with an access violation after
// surviving many frames, so the trigger is something that CHANGES rather than the first call.
// Writing the current step to a tiny file on every attempt means the file names the exact step
// after the crash, instead of costing another guess-and-run cycle.
void mark(int stage, const char *what)
{
	if (!g_stage_file_enabled)
		return;
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
	std::uint64_t skipped = 0; // turned away by the pre-resolve gate (seamhook::pre_gate)
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

// [STRAYDLSS] GBufferResolveOnly: the isolation instrument — record (and dump) the guides
// at the SSD trigger but skip the RR evaluate entirely, SR carrying every frame. One run
// with this on isolates record-side faults from evaluate-side faults.


// Reads the View constant buffer at the moment of the dispatch. It must be read here, at
// command-recording time on the thread that just set the root arguments, because UE4's
// FD3D12FastConstantAllocator sub-allocates from an upload ring that the CPU writer will
// advance past later in the frame. (docs/RESEARCH.md §2.6)
// `raw` (optional, kViewPrefixBytes) receives the exact bytes read, so the pick can later be
// compared BYTE FOR BYTE against the engine's own struct (src/view_params_hook.hpp).
bool read_view_cb(const icept::BufferRange &cb, ue4::ViewParams &out, unsigned char *raw = nullptr)
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

	if (raw != nullptr)
		std::memcpy(raw, copy, sizeof(copy));
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
		// The one case that silently offsets every NGX guide: we pass no subrect base, so a
		// non-origin view rect means colour, depth and output are all taken from the wrong
		// place with no error anywhere. See ue4::view_rect_min_is_origin for why the bases are
		// not plumbed instead.
		if (!ue4::view_rect_min_is_origin(view))
			STRAY_LOG_WARN("    ViewRectMin IS NOT THE ORIGIN (%.1f %.1f). We pass NO subrect "
				"base to NGX, so every guide is being taken from (0,0) while the engine's view "
				"rect starts elsewhere. This had never been observed on this title - 41/41 "
				"reads were (0,0) across 8 sessions, facts §40 - and it is observed NOW.",
				static_cast<double>(view.view_rect_min.x),
				static_cast<double>(view.view_rect_min.y));
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
		// THE ONLY LINE HERE THAT CAN CONVICT THE CB SEARCH. Everything above is a value that
		// "looks plausible", which is what a wrong buffer also produces. Row 135 carries three
		// different data types at predicted places and validates itself from ONE read:
		//   y*z == 1.0 exactly   - PreExposure * OneOverPreExposure, assigned on adjacent lines
		//                          from the same float (SceneRendering.cpp:1563-1564), so it is
		//                          true BY CONSTRUCTION and cannot survive a wrong offset
		//   x is a denormal      - int32 NumSceneColorMSAASamples == 1 reinterpreted as float
		//   w == 0.0 exactly     - padding
		// Read this before believing any other number in this block. CLAUDE.md §2.6.
		{
			const ue4::Float4 &r135 = view.pre_exposure_row;
			const double yz = static_cast<double>(r135.y) * static_cast<double>(r135.z);
			const bool denormal = r135.x != 0.0f && std::fabs(r135.x) < 1.0e-30f;
			STRAY_LOG_INFO("    View row 135 (ONE read): x=%.8e y=%.6f z=%.6f w=%.6f "
				"| y*z=%.6f (want 1.0) | x denormal=%d (want 1) | w==0=%d (want 1) => %s",
				static_cast<double>(r135.x), r135.y, r135.z, r135.w, yz,
				denormal ? 1 : 0, r135.w == 0.0f ? 1 : 0,
				ue4::pre_exposure_plausible(view)
					? "SELF-CHECK PASSES - this really is the View buffer"
					: "SELF-CHECK FAILS - the CB search picked the WRONG BUFFER, so jitter, "
					  "ClipToPrevClip and CameraCut are all suspect");
		}
		STRAY_LOG_INFO("    row135 self-check so far: ok=%llu bad=%llu (a rate near 100%% bad "
			"means the search, not the offsets, is what to fix)",
			static_cast<unsigned long long>(g_view_row135_ok.load(std::memory_order_relaxed)),
			static_cast<unsigned long long>(g_view_row135_bad.load(std::memory_order_relaxed)));
		for (int r = 0; r < 4; ++r)
			STRAY_LOG_INFO("    ClipToPrevClip[%d]  = %+.6f %+.6f %+.6f %+.6f", r,
				view.clip_to_prev_clip.m[r * 4 + 0], view.clip_to_prev_clip.m[r * 4 + 1],
				view.clip_to_prev_clip.m[r * 4 + 2], view.clip_to_prev_clip.m[r * 4 + 3]);
		STRAY_LOG_INFO("    plausible = %s", ue4::view_params_plausible(view) ? "YES" : "no");
	}
	STRAY_LOG_INFO("========================================================");
}

// [STRAYDLSS] EngineSeamReset (src/core/reset_plan.hpp).
std::atomic<resetplan::Mode> g_reset_mode{ resetplan::Mode::observe };
std::mutex g_reset_mutex;
resetplan::Counters g_reset_counters;            // under g_reset_mutex
bool g_reset_jitter_only_logged = false;         // under g_reset_mutex
std::atomic<std::uint64_t> g_reset_plan_disagree{ 0 }; // the OR and the plan differed on a frame that was not jitter-only (must stay 0)

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

void rr_counters(std::uint32_t &evaluates, std::uint32_t &fallbacks)
{
	evaluates = g_rr_evaluates.load(std::memory_order_relaxed);
	fallbacks = g_rr_fallbacks.load(std::memory_order_relaxed);
}

void rr_reason_counters(std::uint32_t out[kRrReasonCount])
{
	for (std::size_t i = 0; i < kRrReasonCount; ++i)
		out[i] = g_rr_reasons[i].load(std::memory_order_relaxed);
}

const char *rr_reason_name(std::size_t index)
{
	if (index < kRrGuideReasonCount)
		return rrguides::refusal_name(static_cast<rrguides::Refusal>(index));
	switch (index)
	{
	case kRrGuideReasonCount + 0: return "rowsImplausible";
	case kRrGuideReasonCount + 1: return "resolveNotReady";
	case kRrGuideReasonCount + 2: return "resolveFailed";
	case kRrGuideReasonCount + 3: return "createFailed";
	case kRrGuideReasonCount + 4: return "evaluateFailed";
	default: return "?";
	}
}
void set_stage_file(bool enabled) { g_stage_file_enabled = enabled; }
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
	// The engine seam's ledger retires announcements by frame, so it needs the same boundary.
	// Inert unless [STRAYDLSS] EngineSeam=2 installed the stand-in.
	seamhook::note_present(frame);
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
	STRAY_LOG_INFO("  %-18s %-11s %5s %5s %8s %8s %8s  %s",
		"hash", "dispatch", "srv", "uav", "resolved", "failed", "skipped", "verdict");
	for (const auto &e : g_stats)
	{
		STRAY_LOG_INFO("  0x%016llx %4ux%-6u %5u %5u %8llu %8llu %8llu  %s",
			static_cast<unsigned long long>(e.first), e.second.gx, e.second.gy,
			e.second.srvs, e.second.uavs,
			static_cast<unsigned long long>(e.second.resolved),
			static_cast<unsigned long long>(e.second.failed),
			static_cast<unsigned long long>(e.second.skipped),
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
	// Every once-per-hash report for this hash is in the log: the first sighting and the first
	// steady (non-cut) frame - or the first sighting alone for a hash the matcher EXCLUDES by
	// identity, which never gets a steady report because `want_steady` refuses `excluded`.
	bool reports_done = false;
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
		{
			const std::uint32_t reports = g_report_count[hash];
			reports_done = reports >= 2 ||
				(reports >= 1 && g_stats[hash].verdict == MatchVerdict::excluded);
		}
		if (!dry_running && g_report_count[hash] >= 2 && !is_known_taa_hash(hash))
		{
			// Still track the output resource each frame: the history round-trip (this
			// frame's u0 reappearing as an SRV next frame) is the decisive test for which
			// colour SRV is the history. (CLAUDE.md §2.3)
			return false;
		}
	}

	// THE PRE-RESOLVE SKIP. The throttle just above never fires in a real session - `dry_running`
	// includes g_ngx_evaluate, which every session that runs DLSS sets - because the pin and the
	// round-trip proof needed to see every dispatch. Under [STRAYDLSS] EngineSeam=3 they decide
	// nothing: the ledger already knows which group counts a pending announcement expects, and a
	// dispatch that fits none of them cannot be claimed whatever the resolve finds. Ask it here,
	// with the group counts alone, BEFORE the table walk over the descriptor shadow, the View-CB
	// search (a 2448-byte read per bound root CBV) and the structural matcher - which is where
	// the 7 resolves a frame of the `resolve 0.539ms (7.0)` line went, six of them to be told
	// `not-announced`. Every counter keeps its meaning (seam::pre_gate_decide says how), and the
	// once-per-hash diagnostics still see each pass first: a hash is only skipped after both of
	// its DISPATCH REPORTs are in the log (one, for a hash the matcher excludes by identity -
	// the two SSD passes, which are exactly the look-alikes that ask every frame). The
	// bisection instruments need the full path
	// for every dispatch and turn this off outright: a named dry-run hash, a dry-run mode, or an
	// explicitly named pass.
	if (reports_done && g_dry_run_hash_count == 0 && g_ngx_dry_run == 0 && g_ngx_pass_override == 0 &&
		seamhook::pre_gate(x, y) == seam::PreGate::skip)
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		++g_stats[hash].skipped;
		return false;
	}

	// The RR guide-resolve trigger ([STRAYDLSS] GBufferResolveAt=ssd): the first SSD
	// temporal-accumulation dispatch of the frame reads the G-buffers as SRVs, so their
	// content is alive HERE — record the guides on this list before the dispatch proceeds
	// (RR-0 never suppresses it; the throttle above cannot eat this call because every RR
	// session sets NgxEvaluate, which disables the throttle entirely).

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

	// [STRAYDLSS] U0Hook=3: THE BIND STREAM SUPPLIES THE REGISTERS. On a dispatch a pending
	// announcement expects (the same ledger question the pre-gate asks, without its counter),
	// the RHI thunks' closed bracket - u0 and t0..t5 with the engine's own objects, bound on
	// this thread a moment ago - replaces `b.srvs`/`b.uavs` when it is COMPLETE, so the matcher,
	// the colour pick (t1), the output UAV (u0), the eye-adaptation SRV (t0) and the history
	// round-trip (t5) below all read the engine's answer unchanged. Anything short of complete
	// leaves the walk's answer in place for this frame and is counted by reason (`fellBack:` on
	// the [u0] line), never silent. The walk's own answer is kept so the level-2 assertion at the
	// claim still compares two routes; the View CB is untouched here (root CBVs, not the shadow;
	// EngineSeamViewParams owns it). src/core/u0_authority.hpp, src/u0_rhi_hook.hpp.
	DispatchBindings walk_b;
	u0auth::Decision l3;
	bool l3_asked = false;
	if (u0hook::level() >= 3 && seamhook::announced_expects(x, y))
	{
		walk_b.srvs = b.srvs;
		walk_b.uavs = b.uavs;
		l3 = u0hook::take_bindings(b);
		l3_asked = true;
	}
	const bool l3_substituted = l3_asked && l3.source == u0auth::Source::bracket;

	// Try every bound constant buffer and keep the first that decodes to a plausible View.
	// Guessing a register would be fragile: b3, b4 and b5 have all been observed carrying it
	// on different passes.
	ue4::ViewParams view{};
	bool view_ok = false;
	// The winner's exact bytes, for the comparison against the engine's own struct at claim.
	unsigned char view_prefix[ue4::kViewPrefixBytes];
	unsigned char cand_prefix[ue4::kViewPrefixBytes];
	// THE LOOP RUNS TO THE END even after a winner is accepted, and for two different reasons.
	// The first is a GATE: a candidate too large for the dispatch (facts §36.18) or below the
	// engine's own 0.5 minimum fraction (§36.21) is skipped and the search CONTINUES, so the
	// real view on a higher root parameter is found. The second is a MEASUREMENT of what is
	// left: `distinct_rival` counts a survivor that would hand DLSS DIFFERENT motion, which is
	// the only kind of ambiguity a slot-order search can get wrong.
	// It is NOT "how many candidates survived".
	// Two root parameters can point at one suballocation, or hold byte-identical copies of the
	// same view - neither is a choice the search can get wrong. Only a survivor that would hand
	// DLSS DIFFERENT ClipToPrevClip / jitter / CameraCut is ambiguity.
	bool distinct_rival = false;
	char cand_list[192];
	int cand_off = 0;
	cand_list[0] = '\0';
	for (const auto &cb : b.constant_buffers)
	{
		ue4::ViewParams candidate{};
		g_view_cb_reads.fetch_add(1, std::memory_order_relaxed);
		if (!read_view_cb(cb.second, candidate, cand_prefix) || !ue4::view_params_plausible(candidate))
			continue;
		if (cand_off >= 0 && cand_off < static_cast<int>(sizeof(cand_list)) - 1)
			cand_off += std::snprintf(cand_list + cand_off, sizeof(cand_list) - cand_off,
				" b%u=%.0fx%.0f", cb.first,
				static_cast<double>(candidate.view_size_and_inv_size.x),
				static_cast<double>(candidate.view_size_and_inv_size.y));
		// KEEP LOOKING IF THIS IS A DIFFERENT VIEW'S BUFFER. Plausibility (and row 135) only
		// establish that a buffer IS a View uniform buffer; a shadow, cubemap-face or
		// scene-capture view satisfies both. The search runs in slot order, so a wrong-but-
		// plausible candidate on a lower register used to win and stop the search - MEASURED
		// on the box as b3 carrying a 4088x4088 view beating the real one on b4, on ~1.2% of
		// frames, which made the matcher refuse the real TAA dispatch as "downsampling" and
		// is the visible flicker (facts §36.17-36.18). The dispatch covers the OUTPUT rect and
		// UE 4.27's OutputViewRect is never smaller than InputViewRect, so a view claiming to
		// be larger than the dispatch covers cannot be this one.
		if (!ue4::view_fits_dispatch(candidate, x * 8u, y * 8u))
		{
			// Gated on !view_ok so this keeps the meaning it had when the loop still broke at
			// the winner: candidates on LATER root parameters could never have won under either
			// selection rule, and counting them would make this incomparable with the 19 870
			// recorded in facts §36.19.
			if (!view_ok)
				g_view_cb_rejected.fetch_add(1, std::memory_order_relaxed);
			continue;
		}
		// AND KEEP LOOKING IF IT IS TOO SMALL, which is the other half of the same test and
		// used to be measured without being acted on (facts §36.20, report §16.5).
		// `view_fits_dispatch` above bounds a candidate only from ABOVE; a shadow, planar-
		// reflection, cubemap-face or scene-capture view is SMALLER than the dispatch and
		// passed plausibility, row 135 and that bound alike, so one on a lower root parameter
		// still won the search. UE 4.27 will not ask for a temporal upscale from below
		// `kMinTAAUpsampleResolutionFraction` = 0.5 of the output rect on either axis, and the
		// dispatch covers the output rect - so a candidate under that floor is not this
		// dispatch's view. SKIPPING it rather than refusing the frame is the point: the real
		// view usually sits on a HIGHER root parameter, so the search now finds it and DLSS
		// runs correctly instead of declining. `primary_view_shape_ok` at the create site is
		// the backstop for when it does not.
		if (!ue4::view_fraction_plausible(candidate, x * 8u, y * 8u))
		{
			if (!view_ok)
				g_view_suspect_small.fetch_add(1, std::memory_order_relaxed);
			continue;
		}
		if (view_ok)
		{
			// Keep scanning ONLY to judge ambiguity; the pick is unchanged (first accepted).
			if (ue4::views_differ_temporally(view, candidate))
				distinct_rival = true;
			continue;
		}
		view = candidate;
		view_ok = true;
		std::memcpy(view_prefix, cand_prefix, sizeof(view_prefix));
		b.view_cb = cb.second;
		b.view_cb_valid = true;
		b.view_cb_register = cb.first;
	}
	// THE CB WE PICKED WAS FOUND BY SEARCH, NOT BY NAME. `view_params_plausible` is a shape
	// test — it can be satisfied by the wrong buffer — and a wrong View means wrong jitter,
	// wrong ClipToPrevClip and a wrong CameraCut, which is exactly what temporal flicker looks
	// like. Row 135 is the one row that validates ITSELF from a single read: it must be
	// (denormal, P, 1/P, 0.0), so `y*z == 1.0` by construction (SceneRendering.cpp:1563-1564),
	// `x` is an int32 MSAA count reinterpreted, and `w` is padding. Count the frames it fails
	// on: a rate near zero exonerates the search, a rate near 100% convicts it. Costs nothing —
	// the quad is already parsed and the predicate already ships.
	if (view_ok)
	{
		if (ue4::pre_exposure_plausible(view))
			g_view_row135_ok.fetch_add(1, std::memory_order_relaxed);
		else
			g_view_row135_bad.fetch_add(1, std::memory_order_relaxed);
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

	// THE CROSS-CHECK. Every gate above this line is behavioural — a DXBC hash, a
	// depth+stencil SRV pair, dispatch-rect arithmetic — and all of it exists because this
	// project began as a ReShade add-on that could see nothing but D3D12 descriptors. With
	// [STRAYDLSS] EngineSeam=2 the ENGINE tells us which dispatch is its primary temporal
	// upscale, so the two answers can be compared instead of one being trusted.
	//
	// This changes no behaviour. It counts, and it names the disagreement once: a positive
	// match that the engine never announced is the wrong-pass class — the failure the user
	// sees as "the whole screen appears suddenly" — and until now it was indistinguishable
	// from a correct match. docs/RESEARCH-ENGINE-TAA-HOOK.md.
	// Which authority decides this dispatch. `heuristic` below authoritative mode or when the
	// seam is not live and the fallback is allowed; `engine` when the engine announced this
	// exact dispatch; a named refusal otherwise. (src/core/engine_seam.hpp, seam::decide)
	seamhook::Verdict seam_verdict;
	seamhook::EngineInputs engine_inputs;
	seam::Gate seam_gate = seam::Gate::heuristic;
	// True when EngineSeamViewParams=2 SUPPLIED the View from FViewInfo's own struct - read by the
	// reset plan far below, so it lives at this scope.
	bool view_from_engine = false;
	// THE `unclaimed` INSTRUMENT. A dispatch the matcher refused never reaches claim(), so an
	// announcement it should have claimed retires as `unclaimed` with no cause recorded. Ask the
	// ledger whether a pending announcement expected exactly these group counts: if one did, the
	// engine's own primary upscale just went past and OUR gate turned it away, and the matcher's
	// verdict and reason are the fix. If none did, this dispatch is unrelated and nothing counts.
	if (m.verdict != MatchVerdict::structural_only && m.verdict != MatchVerdict::hash_and_structural)
	{
		// Carry the matcher's OWN INPUTS, not just its verdict. "dispatch covers less than the
		// view rect" only tells us the arithmetic failed; the render rect it used, which CB that
		// came from, and whether row 135 vouched for that CB are what say WHY.
		seamhook::UnmatchedContext uc;
		uc.view_width = w;
		uc.view_height = h;
		uc.output_width = m.output_width;
		uc.output_height = m.output_height;
		uc.cb_register = b.view_cb_register;
		uc.view_ok = view_ok;
		uc.row135_ok = view_ok && ue4::pre_exposure_plausible(view);
		seamhook::note_unmatched_dispatch(x, y, verdict_name(m.verdict), m.reason, uc);
	}

	if (m.verdict == MatchVerdict::structural_only || m.verdict == MatchVerdict::hash_and_structural)
	{
		seam_verdict = seamhook::claim(x, y);
		seam_gate = seamhook::gate(seam_verdict.announced);

		// THE VIEW FROM THE ENGINE'S OWN STRUCT ([STRAYDLSS] EngineSeamViewParams). The
		// announcement carried the 2448-byte prefix of FViewInfo::CachedViewUniformShader-
		// Parameters, read on the render thread inside AddPasses; here it is judged against
		// the bytes the search just read from the bound constant buffer (prediction 6, the
		// one that ties the discovered offset to a buffer the engine BOUND), and - at level 2,
		// once latched - it SUPPLIES the View. A stale ring copy on a lower root parameter is
		// then the search disagreeing with the engine, counted as disagree=, never the reverse.
		// Below level 2, or before the latch, nothing here changes what DLSS sees.
		if (seam_gate == seam::Gate::engine)
		{
			const vphook::ViewSource vs = vphook::resolve_at_claim(seam_verdict, view_ok,
				view_ok ? view_prefix : nullptr, view);
			if (vs.use_engine)
			{
				view = vs.params;
				view_ok = true;
				view_from_engine = true;
			}
		}

		// THE QUIET-RESIDUE VERDICT, and it is only meaningful on a CLAIMED dispatch. The
		// question is not "was any dispatch's View ambiguous" - look-alikes outnumber real
		// upscales here - it is "was DLSS SR itself ever handed a view it had to guess at".
		// `distinct_rival` means a second surviving candidate would have given different
		// ClipToPrevClip / jitter / CameraCut, i.e. the slot-order search was guessing.
		// NOT counted when the engine's struct supplied the View: the search did not choose,
		// so it cannot have guessed - those events are disagree= on the [viewParams] line.
		if (view_ok && distinct_rival && !view_from_engine)
		{
			const bool claimed = (seam_gate == seam::Gate::engine);
			if (claimed)
				g_view_amb_claimed.fetch_add(1, std::memory_order_relaxed);
			else
				g_view_amb_other.fetch_add(1, std::memory_order_relaxed);
			if (claimed && g_view_amb_logged.fetch_add(1, std::memory_order_relaxed) < 8)
				STRAY_LOG_WARN("VIEW CB AMBIGUITY ON A CLAIMED DISPATCH: %ux%u groups (covers "
					"%ux%u px). More than one View survived plausibility + row135 + "
					"fits-dispatch + the 0.5 minimum fraction, AND they disagree on "
					"ClipToPrevClip / jitter / CameraCut - so the slot-order search GUESSED, "
					"and DLSS SR took the guess. We used b%u = %.0fx%.0f (fraction %.3f of the "
					"dispatch). All plausible candidates:%s. The too-SMALL impostors are gated "
					"out now (facts §36.20), so anything left here is a rival of a legal shape "
					"and would need the View CB by IDENTITY to settle (report §15.2). Logged 8 "
					"times.",
					x, y, x * 8u, y * 8u, b.view_cb_register,
					static_cast<double>(view.view_size_and_inv_size.x),
					static_cast<double>(view.view_size_and_inv_size.y),
					x != 0 ? static_cast<double>(view.view_size_and_inv_size.x) / (x * 8.0) : 0.0,
					cand_list);
		}
		// L1: the engine handed us its own scene colour, depth and velocity in FPassInputs.
		// Resolve them HERE - at dispatch time the graph has executed, so a texture that had
		// no RHI resource at AddPasses time has one now. Each is validated against our own
		// resource registry before it is used, so a wrong offset falls back rather than lies.
		if (seam_gate == seam::Gate::engine)
		{
			engine_inputs = seamhook::resolve_inputs(seam_verdict);

			// THE u0 ASSERTION ([STRAYDLSS] U0Hook=2). The engine bound its output UAV through
			// IRHIComputeContext::RHISetUAVParameter on this very thread a moment before this
			// dispatch; the descriptor walk resolved `u0` from the bound table. Compare them on
			// every engine-announced dispatch and count the verdict (src/u0_rhi_hook.hpp). The
			// walk stays authoritative here; a disagreement is one WARN per pass and the first
			// line to read if the image is wrong. Under U0Hook=3 `b` may already BE the bracket's
			// answer, so the assertion reads the WALK's copy kept above - two routes, still.
			u0hook::WalkAnswer walk;
			const DispatchBindings &wb = l3_substituted ? walk_b : b;
			for (const auto &u : wb.uavs)
			{
				if (u.slot == m.output_uav && icept::backend()->is_resource_live(u.resource))
					walk.u0 = u.resource;
			}
			for (const auto &t : wb.srvs)
			{
				if (t.slot < u0::kMaxTexRegs && icept::backend()->is_resource_live(t.resource))
					walk.t[t.slot] = t.resource;
			}
			walk.view_cb_valid = b.view_cb_valid;
			walk.view_cb_register = b.view_cb_register;
			u0hook::assert_at_claim(walk, seam_verdict.out_width, seam_verdict.out_height, hash);
			// ADJACENT ON PURPOSE, and it is the same `walk`: the render-target pool's map has
			// `SceneColorDeferred` from AllocSceneColor, and t1 is the InputSceneColor this pass
			// actually reads (CLAUDE.md §2.3). Whether they are the same allocation is an
			// OBSERVATION with no prediction attached - measured rather than reasoned about.
			// Inert unless [STRAYDLSS] PoolNames reached level 2 (src/pool_name_hook.hpp).
			poolhook::note_taa_colour(walk.t[1]);
		}

		if (seam_gate == seam::Gate::heuristic && seam_verdict.active && !seam_verdict.announced)
		{
			// Observe mode: the heuristic still gates, so a disagreement is worth a WARN.
			bool first = false;
			{
				std::lock_guard<std::mutex> lock(g_mutex);
				first = !g_seam_disagreement_logged[hash];
				g_seam_disagreement_logged[hash] = true;
			}
			if (first)
				STRAY_LOG_WARN("ENGINE SEAM DISAGREES about pass 0x%016llx: the matcher calls "
					"this dispatch (%ux%u groups) the TAA pass, and the engine's own "
					"ITemporalUpscaler::AddPasses announced no primary temporal upscale it "
					"fits. Either this is a look-alike the signature let through (DOF, light "
					"shafts, SSR, water, a planar reflection, a cubemap face) or the "
					"correlation rule is wrong. First occurrence for this pass only.",
					static_cast<unsigned long long>(hash), x, y);
		}
		else if (seam_gate == seam::Gate::engine)
		{
			// Authoritative mode: the engine's word is the gate; what the heuristic knows
			// becomes two ASSERTIONS, each said once per pass. Neither refuses.
			bool first_claim = false;
			bool assert_hash = false;
			bool assert_rect = false;
			const bool hash_known = is_known_taa_hash(hash) ||
				(g_ngx_pass_override != 0 && hash == g_ngx_pass_override);
			const bool rect_agrees = m.output_width == seam_verdict.out_width &&
				m.output_height == seam_verdict.out_height;
			{
				std::lock_guard<std::mutex> lock(g_mutex);
				if (!g_seam_first_engine_claim_logged)
				{
					g_seam_first_engine_claim_logged = true;
					first_claim = true;
				}
				if (!hash_known && !g_seam_hash_assert_logged[hash])
				{
					g_seam_hash_assert_logged[hash] = true;
					assert_hash = true;
				}
				if (!rect_agrees && !g_seam_rect_assert_logged[hash])
				{
					g_seam_rect_assert_logged[hash] = true;
					assert_rect = true;
				}
			}
			if (first_claim)
				STRAY_LOG_INFO("ENGINE SEAM AUTHORITATIVE: first announced pass claimed - "
					"0x%016llx, %ux%u groups, engine rect %ux%u, matcher rect %ux%u, "
					"render %ux%u, cooked-hash=%s. DLSS SR runs on this dispatch and on "
					"nothing the engine did not announce. Readable from the main menu: the "
					"menu runs the TAA pass too.",
					static_cast<unsigned long long>(hash), x, y,
					seam_verdict.out_width, seam_verdict.out_height,
					m.output_width, m.output_height, m.render_width, m.render_height,
					hash_known ? "yes" : "NO");
			if (assert_hash)
				STRAY_LOG_WARN("ENGINE SEAM ASSERTION: the engine announced pass 0x%016llx and "
					"its DXBC hash is NOT in the cooked FTAAStandaloneCS table. Proceeding on "
					"the engine's word (a game update recooked the shaders, or the table is "
					"incomplete - regenerate taa_hashes.hpp). Once per pass.",
					static_cast<unsigned long long>(hash));
			if (assert_rect)
				STRAY_LOG_WARN("ENGINE SEAM ASSERTION: pass 0x%016llx claimed the engine's "
					"%ux%u announcement but the matcher reads its output rect as %ux%u. The "
					"engine's rect is used. Once per pass.",
					static_cast<unsigned long long>(hash),
					seam_verdict.out_width, seam_verdict.out_height,
					m.output_width, m.output_height);
		}
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
		// THE ENGINE'S ANSWER WINS. `AddPasses` named these three textures; a resource the
		// engine is about to bind is alive by construction, and no register walk or
		// view->resource map can be more authoritative than that. The heuristic's answer is
		// kept as an assertion (one WARN per session) and as the fallback when L1 could not
		// resolve - which is counted, never silent.
		bool engine_depth_used = false;
		bool engine_velocity_used = false;
		if (engine_inputs.depth_ok())
		{
			if (depth_resource != 0 && depth_resource != engine_inputs.depth)
				seamhook::note_input_disagreement("depth", engine_inputs.depth, depth_resource);
			depth_resource = engine_inputs.depth;
			engine_depth_used = true;
		}
		if (engine_inputs.velocity_ok())
		{
			if (velocity_resource != 0 && velocity_resource != engine_inputs.velocity)
				seamhook::note_input_disagreement("velocity", engine_inputs.velocity,
					velocity_resource);
			velocity_resource = engine_inputs.velocity;
			engine_velocity_used = true;
		}

		// Refuse to touch a resource ReShade has already reported destroyed. Its view->resource
		// map outlives the resource on D3D12, and building an SRV from a dead one faults inside
		// the driver (vkCreateImageView, 0xc0000005) and takes the game with it. An
		// engine-supplied resource skips this: the registry already confirmed it live inside
		// resolve_inputs, and this is the check that was rejecting the real pass in the menu.
		const bool resources_live =
			(engine_depth_used || icept::backend()->is_resource_live(depth_resource)) &&
			(engine_velocity_used || icept::backend()->is_resource_live(velocity_resource));
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
		//
		// Under [STRAYDLSS] EngineSeam=3 the engine's announcement replaces that table as the
		// gate: an announced pass resolves whether or not its hash is cooked (the hash is an
		// assertion above), and an unannounced one never does. The loading-screen guard is
		// preserved by construction - a look-alike over short-lived resources is exactly what
		// the engine never announces.
		const bool heuristic_worth = is_known_taa_hash(hash) ||
			(g_ngx_pass_override != 0 && hash == g_ngx_pass_override);
		const bool worth_resolving = seam_gate == seam::Gate::engine ? true
			: seam_gate == seam::Gate::heuristic ? heuristic_worth
			: false;

		// Say which gate stopped it, once. Only while an evaluate is actually wanted — in
		// observation-only sessions these paths are expected and silence is correct.
		if (g_ngx_evaluate)
		{
			if (seam_gate == seam::Gate::refuse_not_announced)
				log_gate_refusal(hash, kGateNotAnnounced);
			else if (seam_gate == seam::Gate::refuse_no_seam)
				log_gate_refusal(hash, kGateNoSeam);
			else if (!worth_resolving)
				log_gate_refusal(hash, kGateUnknownHash);
			else if (!view_ok)
				log_gate_refusal(hash, kGateNoViewCb);
			else if (!resources_live)
				log_gate_refusal(hash, kGateDeadInputs);
		}
		// The CONTINUOUS half of the same fact. The WARN above fires once per pass by design,
		// which hid the RATE - and the rate is what makes this the user's "DLSS flip": every
		// refused frame publishes no guides, so NR declines it (guides-stale) and the next
		// evaluate carries a DLSSNR.Reset.
		// worth_resolving is unconditionally true under the engine's gate, so the only two ways
		// an announced dispatch stops here are a View CB we could not decode and inputs we
		// could not vouch for. They are different fixes, so they are different counters.
		if (seam_gate == seam::Gate::engine && !view_ok)
			seamhook::note_outcome(seam::SeamRefusal::view_unreadable);
		else if (seam_gate == seam::Gate::engine && !resources_live)
			seamhook::note_outcome(seam::SeamRefusal::dead_inputs);

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

			// THE OUTPUT RECT THIS DISPATCH IS FOR. Hoisted above the motion-vector resolve
			// because the letterbox hold below may substitute BOTH rects, and everything
			// downstream - the resolve's extent, the DLSS subrect, the guides NR consumes - has
			// to agree on one answer for the frame.
			std::uint32_t want_out_w = 0;
			std::uint32_t want_out_h = 0;
			if (seam_gate == seam::Gate::engine && seam_verdict.out_width != 0 &&
				seam_verdict.out_height != 0)
			{
				want_out_w = seam_verdict.out_width;
				want_out_h = seam_verdict.out_height;
			}
			else
			{
				want_out_w = m.output_width ? m.output_width : render_w;
				want_out_h = m.output_height ? m.output_height : render_h;
			}

			// HOLD THE LIVE FEATURE ACROSS A LETTERBOX SLIDE, so SR - and therefore NR - keeps
			// running through a scripted transition instead of being declined for its whole
			// second. The rects the engine announces during the slide are a shrinking PREFIX of
			// the ones the feature was built for, so evaluating at the CREATED extent puts the
			// engine's own rect exactly where it expects it; only rows below it, which nothing
			// displays, are computed from input the engine did not render this frame. The gate
			// is `core::plan_letterbox_hold` and every clause of it is a measurement - above
			// all `View.ViewRectMin`, which is what distinguishes a top-left slide (safe) from a
			// centred rect (not).
			if (g_letterbox_hold)
			{
				// The output UAV's own extent, found the same way the create site finds it.
				// Holding writes the CREATED target rather than the engine's smaller rect, so
				// the texture has to still be big enough for it; 0x0 refuses.
				std::uint32_t hold_uav_w = 0;
				std::uint32_t hold_uav_h = 0;
				for (const auto &u : b.uavs)
				{
					if (u.slot == m.output_uav &&
						icept::backend()->is_resource_live(u.resource))
					{
						hold_uav_w = u.width;
						hold_uav_h = u.height;
					}
				}
				const ngx::FeatureDesc livef = ngx::live_feature_desc();
				const core::FeatureRect live{ livef.render_width, livef.render_height,
					livef.output_width, livef.output_height };
				const core::FeatureRect want{ render_w, render_h, want_out_w, want_out_h };
				const core::HoldRefusal why = core::plan_letterbox_hold(live, want,
					view.view_rect_min.x, view.view_rect_min.y, hold_uav_w, hold_uav_h);
				g_hold_refused[static_cast<std::size_t>(why)].fetch_add(1,
					std::memory_order_relaxed);
				if (why == core::HoldRefusal::none)
				{
					if (g_hold_logged.fetch_add(1, std::memory_order_relaxed) < 3)
						STRAY_LOG_INFO("LETTERBOX HOLD: the engine asked for %ux%u -> %ux%u and "
							"the live DLSS feature is %ux%u -> %ux%u. Evaluating at the "
							"CREATED extent instead of rebuilding: the created scale is "
							"unchanged, View.ViewRectMin is (0,0) so the engine's rect is a "
							"prefix of ours, and the rows below it are outside every "
							"downstream view rect. DLSS SR and NR keep running through the "
							"slide. Logged 3 times; the count is on the [recreate] line.",
							render_w, render_h, want_out_w, want_out_h,
							live.render_w, live.render_h, live.output_w, live.output_h);
					render_w = live.render_w;
					render_h = live.render_h;
					want_out_w = live.output_w;
					want_out_h = live.output_h;
					g_hold_frames.fetch_add(1, std::memory_order_relaxed);
				}
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

				if (!recorded && seam_gate == seam::Gate::engine)
					seamhook::note_outcome(seam::SeamRefusal::mv_failed);
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
					else if (seam_gate != seam::Gate::engine && pinned != 0 &&
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
					// Under the engine's gate there is nothing to pin and no round-trip to
					// wait for: the announcement IS the selection, one per view per frame.
					const bool eligible = seam_gate == seam::Gate::engine ? true
						: g_ngx_pass_override != 0
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
						// THE ENGINE'S ANNOUNCEMENT IS A STRONGER WARRANT THAN THE HASH.
						// The §2.3 register map belongs to FTAAStandaloneCS, and being called
						// through ITemporalUpscaler::AddPasses proves this dispatch IS the
						// primary temporal upscale — which the cooked-hash table only ever
						// approximated, and which it misses entirely when a game update
						// recooks the shaders. Under EngineSeam=3 the hash is demoted to an
						// assertion everywhere else; leaving the colour path gated on it meant
						// an engine-announced pass with an unlisted hash silently fell back to
						// the weaker "which buffer looks like colour" heuristics.
						const bool trust_registers =
							m.verdict == MatchVerdict::hash_and_structural ||
							seam_gate == seam::Gate::engine;

						// THE COLOUR INPUT IS SHAPE-CHECKED LIKE EVERY OTHER DLSS INPUT.
						//
						// Until 2026-09-03 it was not, and it is the only one that was not:
						// depth, stencil and velocity are format-matched in match_taa_dispatch,
						// the output UAV must be an HDR float colour target, depth and velocity
						// additionally come from the engine's own FPassInputs (L1) — while the
						// register pick took whatever was live at t1, with no format, no extent
						// and no dimensionality test, and handed it to NGX as pInColor.
						//
						// It cannot be cross-checked against the engine either, and that is
						// structural rather than incidental: FPassInputs.SceneColorTexture is
						// the post-chain scene colour, whose ResourceRHI is assigned inside
						// FRDGBuilder::Execute(), so it resolves `rhi_null` at announce BY
						// DESIGN. The live log reads `l1: resolved=0 partial=103402` — depth and
						// velocity on every claim, colour on none — so the L1 disagreement
						// assertion below has never once executed for colour, and
						// docs/RESEARCH-ENGINE-TAA-HOOK.md §14.4's "nothing has ever suggested
						// colour was misidentified" is an artifact of never having looked.
						//
						// So assert what the shader itself requires of InputSceneColor, and
						// REFUSE rather than evaluate when t1 does not meet it. Refusing costs
						// one frame of the engine's own TAA; not refusing puts an arbitrary
						// texture through the upscaler and into u0.
						std::uint64_t reg_colour = 0;
						const BoundTexture *reg_colour_tex = nullptr;
						bool reg_colour_rejected = false;
						if (trust_registers)
						{
							for (const auto &t : b.srvs)
							{
								if (t.slot != kSceneColourReg ||
									!icept::backend()->is_resource_live(t.resource))
									continue;
								reg_colour_tex = &t;
								if (colour_input_acceptable(t, render_w, render_h,
										out_tex != nullptr ? out_tex->format : TexFormat::unknown))
									reg_colour = t.resource;
								else
									reg_colour_rejected = true;
							}
							if (reg_colour_rejected && reg_colour == 0)
							{
								bool first = false;
								{
									std::lock_guard<std::mutex> lock(g_mutex);
									first = !g_colour_reg_rejected_logged[hash];
									g_colour_reg_rejected_logged[hash] = true;
								}
								if (first)
									STRAY_LOG_WARN("COLOUR INPUT REFUSED on pass 0x%016llx: "
										"register t%u holds %p (%ux%u fmt=%d) which is not "
										"InputSceneColor — it must be a 2D HDR float colour "
										"texture (the output UAV is fmt=%d) of at least the "
										"render rect %ux%u. DLSS SR does not run on this "
										"dispatch; the engine's own TAA does. If this fires "
										"steadily, read the render rect first: it comes from "
										"the View CB slot-order search, and a wrong one makes "
										"every colour candidate fail. Once per pass.",
										static_cast<unsigned long long>(hash), kSceneColourReg,
										reinterpret_cast<void *>(reg_colour_tex->resource),
										reg_colour_tex->width, reg_colour_tex->height,
										static_cast<int>(reg_colour_tex->format),
										out_tex != nullptr ? static_cast<int>(out_tex->format) : -1,
										render_w, render_h);
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
						// The engine named the scene colour outright, so none of the
						// colour-versus-history inference below has to run.
						if (engine_inputs.colour_ok())
						{
							if (colour_handle != 0 && colour_handle != engine_inputs.colour)
								seamhook::note_input_disagreement("scene colour",
									engine_inputs.colour, colour_handle);
							colour_handle = engine_inputs.colour;
							colour_reason = "the engine's FPassInputs.SceneColorTexture";
						}
						else if (reg_colour != 0)
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

						// Both rects were settled before the motion-vector resolve, because
						// the letterbox hold may have substituted the live feature's extent for
						// the engine's shrinking one and every consumer must agree. The engine's
						// own OutputViewRect wins when it announced this dispatch: the matcher's
						// figure is `group count x 8` clamped to the UAV, i.e. rounded UP to a
						// multiple of 8 — identical at 3840x2160 (480 groups exactly) and wrong
						// for any rect that is not.
						ngx::FeatureDesc fd;
						fd.render_width = render_w;
						fd.render_height = render_h;
						fd.output_width = want_out_w;
						fd.output_height = want_out_h;

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
						// THE SHAPE TEST RUNS UNDER THE ENGINE'S GATE TOO, and it was wrong
						// not to. (CORRECTED 2026-09-03; the skip was added by f947ee4, the
						// commit that made EngineSeam=3 the default, reasoning "a cubemap
						// face or a reflection capture never reaches
						// ITemporalUpscaler::AddPasses".)
						//
						// That reasoning covers ONE of the test's two operands. `fd.output_*`
						// is the engine's own announced OutputViewRect and is trustworthy.
						// `fd.render_*` is `View.ViewSizeAndInvSize` out of a constant buffer
						// we located by SEARCHING every bound CB in slot order and keeping the
						// first plausible hit — nothing the engine announced. A shadow,
						// planar-reflection or scene-capture view IS a real View uniform
						// buffer, so it passes plausibility, the row-135 self-check and
						// `view_fits_dispatch` (which bounds the view only from ABOVE), and one
						// on a lower root parameter wins the search.
						//
						// MEASURED on the box, one 114,000-frame session: 37 of 62 DLSS
						// features were created at render rects that cannot be the primary view
						// — 64x34 through 64x52, 128x109, 128x126, 256x240, 1024x1024 — every
						// one of them upscaling to the announced 3840x2160. DLSS was then told
						// InRenderSubrectDimensions of 64x41 against the real 1920x1080 scene
						// colour, so it read the TOP-LEFT CORNER of the frame and magnified it
						// ~60x over the whole screen. Refusing costs one frame of the engine's
						// own TAA (`suppress_engine_dispatch` is only set on a successful
						// evaluate); not refusing puts an unrelated part of the world on the
						// display. Prime directive 2.
						const bool shape_ok = primary_view_shape_ok(
							fd.render_width, fd.render_height, fd.output_width, fd.output_height);

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

						// ensure_feature is called EXACTLY ONCE per dispatch. It creates and
						// releases the NGX feature, so calling it twice to answer "did it
						// fail?" separately from "shall we evaluate?" would have doubled the
						// create/release traffic on every frame.
						const bool roles_ok =
							dims_ok && colour != nullptr && output != nullptr;
						const bool feature_ok = roles_ok && ngx::ensure_feature(native, fd);
						// A shape refusal and a role refusal are different fixes, so they are
						// different counters: the first says the View CB search handed us
						// another view's size, the second that a binding could not be named.
						if (seam_gate == seam::Gate::engine && !shape_ok)
							seamhook::note_outcome(seam::SeamRefusal::bad_render_rect);
						else if (seam_gate == seam::Gate::engine && !roles_ok)
							seamhook::note_outcome(seam::SeamRefusal::role_unresolved);
						else if (seam_gate == seam::Gate::engine && !feature_ok)
							seamhook::note_outcome(seam::SeamRefusal::create_failed);

						if (feature_ok)
						{
							// Our resolve just wrote the motion vectors as a UAV; NGX reads
							// them as a shader resource. Without this the state is simply
							// wrong, and vkd3d validates none of it. (CLAUDE.md §3)
							NGX_TRACE("%s", "barrier mv -> SRV");
							mv::transition_output(native, /*to_shader_resource=*/true);

							// The bias-current-colour mask, if [STRAYDLSS] MvMask asked for one.
							// Null in every default configuration, and null is byte-identical
							// to the behaviour from before it existed. Recorded here, on the
							// same list and inside the same window as the resolve and the
							// evaluate, both of which already rebind descriptor heaps and
							// after which the caller restores the game's state.
							ID3D12Resource *bias_mask = nullptr;
							if (mvmask::enabled() &&
								mvmask::initialise(native_device, render_w, render_h))
							{
								bias_mask = mvmask::record(native, render_w, render_h,
									g_present_frame.load(std::memory_order_relaxed));
							}

							ngx::EvaluateInputs ei;
							ei.color = colour;
							ei.depth = reinterpret_cast<ID3D12Resource *>(depth_resource);
							ei.motion_vectors = mv::output();
							ei.output = output;
							ei.bias_mask = bias_mask;
							// TemporalAAParams.zw, straight through. (CLAUDE.md §2.7)
							ei.jitter_x = view.temporal_aa_params.z;
							ei.jitter_y = view.temporal_aa_params.w;
							ei.render_width = render_w;
							ei.render_height = render_h;
							// The three signals of CLAUDE.md §2.8 - View.CameraCut, TemporalAAJitter.zw
							// == .xy, and a 1x1 history/velocity dummy - through the reset plan
							// ([STRAYDLSS] EngineSeamReset, src/core/reset_plan.hpp): the OR below level
							// 2; at level 2, when the View is the engine's own struct, CameraCut | 1x1
							// decides and jitter-equality is an assertion. Every combination is
							// counted. The result travels to DLSS SR here, to DLSS-G (fc.reset) and
							// to the NR stage (note_guides) below - one decision, three accumulators.
							{
								resetplan::Signals rs;
								rs.camera_cut = view.camera_cut != 0.0f;
								rs.jitter_equal = ue4::jitter_indicates_camera_cut(
									ue4::Float2{ view.temporal_aa_jitter.x, view.temporal_aa_jitter.y },
									ue4::Float2{ view.temporal_aa_jitter.z, view.temporal_aa_jitter.w });
								rs.history_1x1 = m.camera_cut_dummies;
								rs.engine_view = view_from_engine;
								const resetplan::Decision rd = resetplan::decide(g_reset_mode.load(std::memory_order_relaxed), rs);
								ei.reset = rd.reset;
								{
									std::lock_guard<std::mutex> lock(g_reset_mutex);
									g_reset_counters.note(rs, rd);
									if (rd.jitter_only && !g_reset_jitter_only_logged)
									{
										g_reset_jitter_only_logged = true;
										STRAY_LOG_INFO("RESET PLAN: first frame where ONLY the jitter-equality heuristic fired "
											"(CameraCut=0, no 1x1 dummy, View %s the engine's) - %s. Once per session; "
											"the rate is `jitterOnly` on the [reset] line, split into fired/suppressed.",
											view_from_engine ? "IS" : "is NOT",
											rd.reset ? "the OR RESET all three temporal accumulators for it"
											         : "level 2 declined the reset on the engine's word");
									}
								}
								// The pre-plan OR is asserted beside the decision: at any level the two
								// may only differ on a jitter-only frame at level 2.
								const bool legacy = ue4::is_camera_cut(view, m.camera_cut_dummies);
								if (legacy != rd.reset && !rd.jitter_only)
									g_reset_plan_disagree.fetch_add(1, std::memory_order_relaxed);
							}
							ei.pre_exposure = view.pre_exposure;
							// The row-135 self-check, at the SR site. The NR path has always
							// gated this row (below) while SR forwarded it blind, so a misread
							// reached DLSS with nothing to notice — and a misread of 0 becomes a
							// literal 1.0 inside nvsdk_ngx_helpers.h:507, which tells DLSS the
							// colour buffer carries no pre-exposure while it carries ~0.45.
							ei.pre_exposure_ok = view_ok && ue4::pre_exposure_plausible(view);

							// The exposure texture, per [STRAYDLSS] NgxExposure.
							//
							//   texture — the engine's own eye-adaptation buffer, register t0 of
							//             this very dispatch (1x1 RGBA32F, present every frame,
							//             CLAUDE.md §2.3), liveness-checked like every capture.
							//             This is what the official plugin passes.
							//   owned   — our own 1x1 R32_FLOAT, written this frame with row
							//             135.y x NgxExposureValue and transitioned by us.
							//             src/exposure_texture.hpp explains why owning it is
							//             what makes the "is it inert?" question decidable.
							//
							// A miss cannot fall back to AutoExposure per frame — that is a
							// creation-time flag — so the frame gets DLSS's default exposure of
							// 1.0 instead, and ngx::evaluate counts and reports it.
							const exposure::Mode exposure_mode = ngx::exposure_mode();
							if (exposure_mode == exposure::Mode::engine_texture)
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
											"1.0 for such frames. NgxExposure=owned does not "
											"depend on finding this binding at all. First "
											"occurrence only.",
											eye != 0 ? 1 : 0,
											eye != 0 && icept::backend()->is_resource_live(eye) ? 1 : 0);
									}
								}
							}
							else if (exposure_mode == exposure::Mode::owned_texture)
							{
								const float texel = exposure::owned_texel_value(
									view.pre_exposure, ei.pre_exposure_ok,
									ngx::exposure_value_multiplier());
								ei.exposure = exposure_texture::update(native_device, native,
									texel);
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

								// THE ENGINE'S OWN SPARSE VELOCITY, undecoded. This is the only
								// resource in the frame that says WHICH PIXELS UE4 WROTE AN
								// OBJECT VELOCITY FOR, and that is a question our resolved
								// output cannot answer: mv_resolve.hlsl writes a plausible
								// vector either way — decoded object motion where
								// `EncodedVelocity.x > 0`, reconstructed camera motion
								// everywhere else (CLAUDE.md §2.5). The two are
								// indistinguishable downstream, so a pixel handed the wrong
								// branch looks like a pixel handed the right one.
								//
								// The reason to care, 2026-09-03: the cat is drawn by the GFur
								// shell-fur plugin, whose ~48 shell layers are a separate draw
								// from the skeletal body. If those shells write no velocity
								// then every fur pixel takes the camera-reconstruction branch,
								// which computes the motion of a STATIC world point at that
								// depth — precisely wrong for the one object in the scene that
								// is always moving relative to the camera. `R > 0` over the
								// cat's silhouette settles it in one look; tools/rawdump2png.py
								// renders exactly that mask from this file.
								//
								// Same state assumption as colour and depth: the game bound it
								// as an SRV of the compute dispatch we are replacing.
								if (velocity_resource != 0)
									input_dump::capture(native_device, native,
										reinterpret_cast<ID3D12Resource *>(velocity_resource),
										D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
										"velocity_raw", eval_no);

								// Our resolved dense field, as NGX and (through
								// nrhook::note_guides) feature 18 receive it. Already
								// transitioned to NON_PIXEL_SHADER_RESOURCE above for the
								// evaluate. Dumped beside the raw buffer so the resolve's two
								// branches can be compared against the mask that selected them.
								if (ei.motion_vectors != nullptr)
									input_dump::capture(native_device, native, ei.motion_vectors,
										D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, "mv",
										eval_no);
							}

							// THE EXPOSURE DIAGNOSTIC. It runs whenever an exposure texture
							// mode is configured — NOT behind NgxDumpInputs, which also turns
							// on 66 MB colour/depth dumps and was therefore never on during an
							// ordinary session. That gating is the reason "the texture mode
							// measured inert" has no log excerpt behind it.
							//
							// It fires at the first exposure evaluate and at evaluates 300 and
							// 600, and it reads back the texel we actually handed NGX. The
							// readback is a 1x1 copy (a few hundred bytes) at three points in a
							// session, so it costs nothing worth gating.
							if (exposure_mode != exposure::Mode::automatic)
							{
								static bool s_first_done = false;
								const bool at_point = !s_first_done || eval_no == 300 ||
									eval_no == 600;
								if (at_point)
								{
									s_first_done = true;
									STRAY_LOG_INFO("EXPOSURE eval %llu: mode=%s exposure=%p "
										"(colour=%p depth=%p) InPreExposure=%.6f "
										"pre_exposure_ok=%d InExposureScale=%.6f "
										"valueMul=%.4f ownedTexel=%.6f "
										"assumedState=NON_PIXEL_SHADER_RESOURCE (guide 310.6.0 "
										"3.4 requires exactly this for every D3D12 input; the "
										"engine's own compute dispatch already left t0 there, "
										"and the owned texture is transitioned by us)",
										static_cast<unsigned long long>(eval_no),
										exposure::mode_name(exposure_mode),
										static_cast<void *>(ei.exposure),
										static_cast<void *>(ei.color),
										static_cast<void *>(ei.depth), ei.pre_exposure,
										ei.pre_exposure_ok ? 1 : 0, ngx::exposure_scale(),
										ngx::exposure_value_multiplier(),
										exposure_mode == exposure::Mode::owned_texture
											? exposure_texture::last_value() : 0.0f);
									if (ei.exposure != nullptr)
									{
										char label[32];
										std::snprintf(label, sizeof(label), "exposure_%llu",
											static_cast<unsigned long long>(eval_no));
										// The state both textures are in at this point: the
										// engine's t0 because its own compute dispatch bound it
										// as an SRV, ours because exposure_texture::update left
										// it there. Not a guess in either case.
										input_dump::capture_texel(
											native_device, native, ei.exposure,
											D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
											label, eval_no);
									}
									else
									{
										STRAY_LOG_WARN("EXPOSURE eval %llu: no exposure texture "
											"this frame despite NgxExposure=%s — DLSS uses its "
											"default exposure of 1.0 for it.",
											static_cast<unsigned long long>(eval_no),
											exposure::mode_name(exposure_mode));
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
								// RAY RECONSTRUCTION GETS FIRST REFUSAL, SR IS THE PER-FRAME
								// FALLBACK. RR replaces SR rather than adding to it: it does
								// the upscaling too (docs/RESEARCH-RR-GBUFFER.md §2.6, "when
								// DLSS-RR is enabled it effectively overrides DLSS-SR
								// execution"). A frame RR cannot carry is carried by SR with a
								// counted, named reason - never dropped, never guessed at.
								bool rr_ok = false;
								if (g_ngx_rr_mode.load(std::memory_order_relaxed) == 2)
								{
									perf::Scope perf_rr(perf::kNgxRr);
									rr_ok = try_evaluate_rr(native_device, native, fd, ei, view,
										eval_no);
									if (rr_ok)
									{
										g_rr_evaluates.fetch_add(1, std::memory_order_relaxed);
										perf::stall_note_evaluate();
									}
									else
									{
										g_rr_fallbacks.fetch_add(1, std::memory_order_relaxed);
									}
								}
								ok = rr_ok;
								if (!rr_ok)
								{
									perf::Scope perf_sr(perf::kNgxSr);
									ok = ngx::evaluate(native, ei);
									if (ok) perf::stall_note_evaluate();
								}

								// DLSS Neural Rendering (NGX feature 18) runs at Present, not
								// here — but its guides can only be captured here. (ngx_nr.hpp)
								if (ok && nr::enabled())
								{
									// PUBLISH THE GUIDES. This is all the TAA hook does for
									// feature 18: the present stage has no way of its own to find
									// the depth and the motion vectors, and this is the only place
									// in the frame where both are known-good and known-fresh. The
									// evaluate itself happens later in the same frame, over the
									// back buffer (src/nr_hook.cpp). `ei.reset` is the camera-cut
									// OR from §2.8 and MUST travel with them: feature 18 keeps its
									// own temporal history.
									nrhook::note_guides(ei.depth, ei.motion_vectors,
										ei.render_width, ei.render_height, ei.reset);
								}
							}
							if (seam_gate == seam::Gate::engine)
								seamhook::note_outcome(ok ? seam::SeamRefusal::none
								                          : seam::SeamRefusal::eval_failed);
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

							// Frame generation's guides: copies of this frame's depth and dense
							// motion vectors into FG-owned textures, plus the constants, published
							// for the present-time DLSS-G evaluate (src/ngx_fg.hpp). Recorded here
							// because the engine's depth is a pooled target whose content at
							// present time is not guaranteed, and the MV output is rewritten by
							// next frame's resolve.
							if (ok && native::fg::enabled() && native::fg::config().mode == native::fg::Mode::ngx)
							{
								ngxfg::FrameConstants fc;
								fc.jitter_x = ei.jitter_x;
								fc.jitter_y = ei.jitter_y;
								fc.reset = ei.reset;
								fc.render_width = ei.render_width;
								fc.render_height = ei.render_height;
								std::memcpy(fc.clip_to_prev_clip, view.clip_to_prev_clip.m, sizeof(fc.clip_to_prev_clip));
								std::memcpy(fc.view_to_clip_no_aa, view.view_to_clip_no_aa.m, sizeof(fc.view_to_clip_no_aa));
								std::memcpy(fc.translated_world_to_view, view.translated_world_to_view.m, sizeof(fc.translated_world_to_view));
								fc.near_plane = view.near_plane;
								fc.delta_time_s = view.delta_time;
								fc.pre_exposure = view.pre_exposure;
								fc.frame = g_present_frame.load(std::memory_order_relaxed);
								ngxfg::publish(native_device, native, ei.depth, ei.motion_vectors, fc);
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

void set_letterbox_hold(bool on)
{
	g_letterbox_hold = on;
	if (!on)
		STRAY_LOG_WARN("NgxLetterboxHold=0: a scripted transition's shrinking view rect will be "
			"DECLINED frame by frame instead of evaluated at the live feature's extent, so DLSS "
			"SR - and therefore NR, which consumes the guides SR publishes - is off for the "
			"whole animation. That is the 2026-09-03 behaviour, kept for an A/B.");
}

void hold_counters(std::uint64_t &held, std::uint64_t *refused_by_reason, std::size_t count)
{
	held = g_hold_frames.load(std::memory_order_relaxed);
	const std::size_t n = count < static_cast<std::size_t>(core::HoldRefusal::count)
		? count
		: static_cast<std::size_t>(core::HoldRefusal::count);
	for (std::size_t i = 0; i < n; ++i)
		refused_by_reason[i] = g_hold_refused[i].load(std::memory_order_relaxed);
}

void view_row135_counters(std::uint64_t &ok, std::uint64_t &bad, std::uint64_t &wrong_view,
                          std::uint64_t &suspect_small, std::uint64_t &amb_claimed,
                          std::uint64_t &amb_other)
{
	ok = g_view_row135_ok.load(std::memory_order_relaxed);
	bad = g_view_row135_bad.load(std::memory_order_relaxed);
	wrong_view = g_view_cb_rejected.load(std::memory_order_relaxed);
	suspect_small = g_view_suspect_small.load(std::memory_order_relaxed);
	amb_claimed = g_view_amb_claimed.load(std::memory_order_relaxed);
	amb_other = g_view_amb_other.load(std::memory_order_relaxed);
}

std::uint64_t view_cb_read_count()
{
	return g_view_cb_reads.load(std::memory_order_relaxed);
}

void set_reset_level(int level)
{
	const resetplan::Mode m = resetplan::mode_from_level(level);
	g_reset_mode.store(m, std::memory_order_relaxed);
	STRAY_LOG_INFO("RESET PLAN MODE: %s ([STRAYDLSS] EngineSeamReset=%d). %s", resetplan::mode_name(m), level,
		m == resetplan::Mode::off ? "The three-way OR decides, uncounted."
		: m == resetplan::Mode::observe
			? "The three-way OR decides; every combination of CameraCut / jitter-equality / 1x1 is counted on the "
			  "[reset] line, and `jitterOnly fired=` is the number of whole-history wipes the heuristic added."
			: "When the View is the engine's struct, CameraCut | 1x1 decides and jitter-equality is an assertion "
			  "(`jitterOnly suppressed=`); otherwise the OR, counted as `fellBack=`. UNCONFIRMED on the box.");
}

resetplan::Mode reset_mode() { return g_reset_mode.load(std::memory_order_relaxed); }

void reset_counters(resetplan::Counters &out, std::uint64_t *plan_disagree)
{
	std::lock_guard<std::mutex> lock(g_reset_mutex);
	out = g_reset_counters;
	if (plan_disagree != nullptr)
		*plan_disagree = g_reset_plan_disagree.load(std::memory_order_relaxed);
}

int format_reset_report(char *buffer, std::size_t size)
{
	if (buffer == nullptr || size == 0)
		return 0;
	resetplan::Counters c;
	std::uint64_t disagree = 0;
	reset_counters(c, &disagree);
	return std::snprintf(buffer, size,
		"reset=%s frames=%llu resets=%llu fromEngine=%llu fellBack=%llu jitterOnly=%llu (fired=%llu suppressed=%llu) "
		"planDisagree=%llu | combos: none=%llu cut=%llu jitter=%llu cut+jitter=%llu 1x1=%llu cut+1x1=%llu jitter+1x1=%llu all=%llu",
		resetplan::mode_name(reset_mode()), static_cast<unsigned long long>(c.frames),
		static_cast<unsigned long long>(c.resets), static_cast<unsigned long long>(c.from_engine),
		static_cast<unsigned long long>(c.fell_back), static_cast<unsigned long long>(c.jitter_only),
		static_cast<unsigned long long>(c.jitter_only_fired), static_cast<unsigned long long>(c.jitter_only_suppressed),
		static_cast<unsigned long long>(disagree),
		static_cast<unsigned long long>(c.combo[0]), static_cast<unsigned long long>(c.combo[1]),
		static_cast<unsigned long long>(c.combo[2]), static_cast<unsigned long long>(c.combo[3]),
		static_cast<unsigned long long>(c.combo[4]), static_cast<unsigned long long>(c.combo[5]),
		static_cast<unsigned long long>(c.combo[6]), static_cast<unsigned long long>(c.combo[7]));
}

void log_reset_report(const char *when)
{
	if (reset_mode() == resetplan::Mode::off)
		return;
	char line[512] = {};
	format_reset_report(line, sizeof(line));
	STRAY_LOG_INFO("[reset] %s: %s  (planDisagree must stay 0; at level 2 fellBack should stay near 0 once the View latches)",
		when != nullptr ? when : "", line);
}

} // namespace stray_dlss::taa_hook
