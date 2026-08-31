#include "taa_hook.hpp"

#include "frame_state.hpp"
#include "log.hpp"
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
bool g_ngx_logged_once = false;      // the evaluate result
bool g_ngx_skip_logged = false;      // why we did not evaluate — a SEPARATE flag, because
                                     // sharing one meant the skip warning consumed the budget
                                     // and the successful evaluate never reported at all
bool g_ngx_dims_logged = false;
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
bool g_ngx_waiting_logged = false;

bool owns_temporal_history(std::uint64_t hash)
{
	std::lock_guard<std::mutex> lock(g_mutex);
	const auto it = g_roundtrip_seen.find(hash);
	return it != g_roundtrip_seen.end() && it->second;
}
// Traces the first few evaluate cycles step by step. The crash follows a SUCCESSFUL evaluate
// within about a second, the UE4 dump carries an empty callstack, and the add-on's last line is
// simply whatever it logged before dying — so the only way to localise it is to say what we are
// about to do, every time, until we have seen it survive.
// Restore runs on EVERY intercepted dispatch, many times a frame, while evaluate only starts
// once the feature exists — so a single shared budget is spent long before the interesting part.
// The trace therefore only opens after the first successful evaluate.
std::atomic<bool> g_ngx_evaluated_once{ false };
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

const Diagnostics &diagnostics() { return g_diag; }

void set_ngx_evaluate(bool enabled) { g_ngx_evaluate = enabled; }

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

		{
			HashStats &st = g_stats[hash];
			st.gx = x;
			st.gy = y;
		}

		if (g_report_count[hash] >= 2 && hash != kTaaMainHash)
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
				(hash == kTaaMainHash || hash == kSecondCandidateHash ||
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

	// ---- Phase B: our motion-vector resolve, and DLSS, on the game's command list ----
	//
	// Returns false unless DLSS actually produced this pass's output, in which case the
	// engine's own TAA dispatch must be suppressed — see where this is set.
	bool suppress_engine_dispatch = false;

	if (g_mv_resolve_enabled &&
		(m.verdict == MatchVerdict::hash_and_structural || m.verdict == MatchVerdict::structural_only))
	{
		mark(1, "entered-phaseB");
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

		if (view_ok && resources_live && !m.camera_cut_dummies)
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

				if (mv::record(native, inputs, g_mv_dispatch_mode))
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
					const bool eligible = pinned != 0 ? (pinned == hash)
					                                  : owns_temporal_history(hash);
					if (g_ngx_evaluate && ngx::status().super_sampling_available && !eligible &&
						pinned == 0 && !g_ngx_waiting_logged)
					{
						g_ngx_waiting_logged = true;
						STRAY_LOG_INFO("DLSS is waiting for a pass to prove it owns the temporal "
							"history before replacing anything.");
					}
					if (g_ngx_evaluate && ngx::status().super_sampling_available && eligible)
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
						const auto colour_candidate = [&](const BoundTexture &t) {
							return is_resource_live(t.resource) && t.width >= render_w &&
								t.height >= render_h && t.width > 0 && t.height > 0;
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

						std::uint64_t colour_handle = 0;
						const char *colour_reason = "ok";
						if (slot_a != 0 && slot_b != 0 && history != 0)
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
							const bool ok = ngx::evaluate(native, ei);
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

void resolve_counters(std::uint32_t &attempts, std::uint32_t &skipped_stale)
{
	attempts = g_resolve_attempts.load(std::memory_order_relaxed);
	skipped_stale = g_resolve_skipped_stale.load(std::memory_order_relaxed);
}

} // namespace stray_dlss::taa_hook
