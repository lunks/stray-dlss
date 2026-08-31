#include "taa_hook.hpp"

#include "frame_state.hpp"
#include "log.hpp"
#include "input_dump.hpp"
#include "mv_resolve.hpp"
#include "ngx_backend.hpp"

#include <state_tracking.hpp>

#include <d3d12.h>

#include <cstdio>
#include <cstring>
#include <atomic>
#include <mutex>
#include <unordered_map>

namespace stray_dlss::taa_hook {
namespace {

std::mutex g_mutex;
std::unordered_map<uint64_t, std::uint64_t> g_pipeline_hashes;                  // pipeline -> DXBC hash
std::unordered_map<reshade::api::command_list *, uint64_t> g_bound;             // cmd list -> pipeline
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
// [STRAYDLSS] DryRunHash=0x... — suppress exactly ONE named pass and write nothing.
//
// This is how a candidate is tested: the pass that drives the picture is the one whose
// suppression CHANGES the image. Independent of the matcher, so a pass the strict signature
// rejects can still be tested.
std::uint64_t g_dry_run_hash = 0;
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
// Hashes that have demonstrated the history round-trip, i.e. UE4 bound their previous u0 back
// as an SRV the following frame. CLAUDE.md §2.9 calls this the decisive test for which pass
// owns the temporal history — and it is a far better gate than "the first pass that happens to
// evaluate", which is how DLSS previously chose and which picked 0xda289b0ddfa934c6, neither of
// the structural TAA candidates measured at this resolution.
std::unordered_map<std::uint64_t, bool> g_roundtrip_seen;
std::unordered_map<std::uint64_t, bool> g_candidate_logged;
bool g_dry_run_hash_logged = false;
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

const char *tex_format_name(TexFormat f)
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
	case TexFormat::unknown:                  return "other";
	}
	return "?";
}

// Reads the View constant buffer at the moment of the dispatch. It must be read here, at
// command-recording time on the thread that just set the root arguments, because UE4's
// FD3D12FastConstantAllocator sub-allocates from an upload ring that the CPU writer will
// advance past later in the frame. (docs/RESEARCH.md §2.6)
bool read_view_cb(reshade::api::device *device, const reshade::api::buffer_range &cb, ue4::ViewParams &out)
{
	const reshade::api::resource_desc rd = device->get_resource_desc(cb.buffer);
	if (rd.heap != reshade::api::memory_heap::upload && rd.heap != reshade::api::memory_heap::unknown)
		return false;

	// BOUNDS CHECK, and it is not a formality — its absence was the access violation.
	//
	// Every bound constant buffer gets tried until one decodes as a plausible View, and most
	// of them are small: UE4's $Globals allocations are a few hundred bytes. Copying a fixed
	// 2448 bytes out of one that sits near the end of its page reads unmapped memory and kills
	// the process, with the fault landing inside memcpy where it is hard to attribute.
	//
	// buffer_range::size is UINT64_MAX for root CBVs, where ReShade does not know the extent,
	// so the resource's own size is the authority.
	if (cb.size != UINT64_MAX && cb.size < ue4::kViewPrefixBytes)
		return false;
	if (rd.buffer.size < cb.offset + ue4::kViewPrefixBytes)
		return false;

	void *mapped = nullptr;
	if (!device->map_buffer_region(cb.buffer, cb.offset, ue4::kViewPrefixBytes,
			reshade::api::map_access::read_only, &mapped) || mapped == nullptr)
		return false;

	unsigned char copy[ue4::kViewPrefixBytes];
	std::memcpy(copy, mapped, sizeof(copy));
	device->unmap_buffer_region(cb.buffer);

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
			static_cast<unsigned long long>(t.resource), tex_format_name(t.format), t.width, t.height);
	for (const auto &t : b.uavs)
		STRAY_LOG_INFO("  u%-2u res=0x%016llx %-26s %ux%u", t.slot,
			static_cast<unsigned long long>(t.resource), tex_format_name(t.format), t.width, t.height);

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
void set_ngx_dry_run(int mode) { g_ngx_dry_run = mode; }
void set_dry_run_hash(std::uint64_t hash) { g_dry_run_hash = hash; }
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

void set_bound_pipeline(reshade::api::command_list *cmd_list, uint64_t pipeline_handle)
{
	std::lock_guard<std::mutex> lock(g_mutex);
	if (g_pipeline_hashes.count(pipeline_handle) != 0)
		g_bound[cmd_list] = pipeline_handle;
}

void forget_command_list(reshade::api::command_list *cmd_list)
{
	// Without this a reset command list keeps its previous pipeline attribution, so a dispatch
	// can be blamed on a shader that is no longer bound.
	std::lock_guard<std::mutex> lock(g_mutex);
	g_bound.erase(cmd_list);
}

bool intercept_dispatch(reshade::api::command_list *cmd_list, uint32_t x, uint32_t y, uint32_t z)
{
	// Cheap rejection first: this runs on every dispatch, thousands of times a frame.
	// Anything smaller than a plausible full-screen tile grid cannot be the TAA pass.
	if (z != 1 || x < 32 || y < 18)
		return false;

	std::uint64_t hash = 0;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		++g_diag.large_dispatches;

		const auto b = g_bound.find(cmd_list);
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
			if (g_dry_run_hash != 0 && hash == g_dry_run_hash && dry_run_phase_active())
			{
				if (!g_dry_run_hash_logged)
				{
					g_dry_run_hash_logged = true;
					STRAY_LOG_WARN("DRY RUN (hash): suppressing 0x%016llx, writing nothing.",
						static_cast<unsigned long long>(hash));
				}
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
		const bool dry_running = g_dry_run_hash != 0 || g_ngx_dry_run != 0 ||
			g_ngx_pass_override != 0 || g_ngx_evaluate;
		if (!dry_running && g_report_count[hash] >= 2 && !is_known_taa_hash(hash))
		{
			// Still track the output resource each frame: the history round-trip (this
			// frame's u0 reappearing as an SRV next frame) is the decisive test for which
			// colour SRV is the history. (CLAUDE.md §2.3)
			return false;
		}
	}

	DispatchBindings b;
	if (!resolve_compute_bindings(cmd_list, b))
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
			dump_tracker_state_for(cmd_list, why);
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

	reshade::api::device *device = cmd_list->get_device();

	// Try every bound constant buffer and keep the first that decodes to a plausible View.
	// Guessing a register would be fragile: b3, b4 and b5 have all been observed carrying it
	// on different passes.
	ue4::ViewParams view{};
	bool view_ok = false;
	for (const auto &cb : b.constant_buffers)
	{
		ue4::ViewParams candidate{};
		if (read_view_cb(device, cb.second, candidate) && ue4::view_params_plausible(candidate))
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
		const bool resources_live = is_resource_live(depth_resource) &&
			is_resource_live(velocity_resource);
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
				reinterpret_cast<void *>(depth_resource), is_resource_live(depth_resource) ? 1 : 0,
				reinterpret_cast<void *>(velocity_resource),
				is_resource_live(velocity_resource) ? 1 : 0);
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

		// Camera-cut frames (1x1 dummy velocity/history) are evaluated too — with InReset set
		// via is_camera_cut — so the engine's TAA never runs once DLSS engages. Skipping them
		// let the engine's TAA blend against DLSS-written history, which flickered. The dummy
		// velocity is harmless: its out-of-bounds loads return zero, the decode's validity test
		// fails, the camera-motion branch yields ~zero vectors, and a reset frame ignores
		// motion vectors regardless.
		if (view_ok && resources_live)
		{
			mark(2, "descriptors-found");
			auto *native_device = reinterpret_cast<ID3D12Device *>(device->get_native());
			auto *native = reinterpret_cast<ID3D12GraphicsCommandList *>(cmd_list->get_native());

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

				const bool recorded = mv::record(native, inputs, g_mv_dispatch_mode);
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
					const std::uint64_t pinned =
						g_ngx_pass_hash.load(std::memory_order_relaxed);
					// An explicitly named pass wins outright over the round-trip heuristic.
					const bool eligible = g_ngx_pass_override != 0
						? (hash == g_ngx_pass_override)
						: (pinned != 0 ? (pinned == hash) : owns_temporal_history(hash));
					if (g_ngx_evaluate && ngx::status().super_sampling_available && !eligible &&
						pinned == 0 && !g_ngx_waiting_logged)
					{
						g_ngx_waiting_logged = true;
						STRAY_LOG_INFO("DLSS is waiting for a pass to prove it owns the temporal "
							"history before replacing anything.");
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
							if (u.slot == m.output_uav && is_resource_live(u.resource))
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
								if (t.slot == kSceneColourReg && is_resource_live(t.resource))
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
							return out_tex != nullptr && is_resource_live(t.resource) &&
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
							if (u.slot == m.output_uav && is_resource_live(u.resource))
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
						const bool dims_ok = fd.output_width >= fd.render_width &&
							fd.output_height >= fd.render_height &&
							fd.render_width > 0 && fd.output_width > 0;
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
								input_dump::capture(reinterpret_cast<ID3D12Device *>(
										device->get_native()), native, ei.color,
									D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, "colour",
									eval_no);
								input_dump::capture(reinterpret_cast<ID3D12Device *>(
										device->get_native()), native, ei.depth,
									D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, "depth",
									eval_no);
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
								ok = ngx::evaluate(native, ei);
							}
							if (ok)
							{
								g_ngx_evaluated_once.store(true, std::memory_order_relaxed);
								std::uint64_t expected = 0;
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
								input_dump::capture(reinterpret_cast<ID3D12Device *>(
										device->get_native()), native, ei.output,
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
						restore_game_compute_state(cmd_list);
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
