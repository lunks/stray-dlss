#include "taa_hook.hpp"

#include "frame_state.hpp"
#include "log.hpp"
#include "mv_resolve.hpp"

#include <state_tracking.hpp>

#include <d3d12.h>

#include <cstdio>
#include <cstring>
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
// [STRAYDLSS] MvResolve, default on. A switch so the pass can be bisected on the target
// machine without a rebuild, which is a slow round trip.
bool g_mv_resolve_enabled = true;
// [STRAYDLSS] MvRestoreHeaps, default on. A switch so the heap restore can be A/B'd against
// the crash without another build-and-deploy cycle.
bool g_restore_heaps = true;
// [STRAYDLSS] MvRestoreState, default on.
bool g_restore_state = true;

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

void configure(bool mv_resolve_enabled, bool restore_heaps, bool restore_state)
{
	g_mv_resolve_enabled = mv_resolve_enabled;
	g_restore_heaps = restore_heaps;
	g_restore_state = restore_state;
	STRAY_LOG_INFO("MV resolve=%s heapRestore=%s stateRestore=%s",
		mv_resolve_enabled ? "on" : "off", restore_heaps ? "on" : "off",
		restore_state ? "on" : "off");
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

	// ---- Phase B: run our own motion-vector resolve on the game's command list ----
	//
	// Still returns false at the end, so the engine's TAA runs as normal and the image is
	// unchanged. This exists to get the resolve executing and provably correct before anything
	// depends on its output.
	if (g_mv_resolve_enabled &&
		(m.verdict == MatchVerdict::hash_and_structural || m.verdict == MatchVerdict::structural_only))
	{
		mark(1, "entered-phaseB");
		std::uint64_t depth_descriptor = 0;
		std::uint64_t velocity_descriptor = 0;
		for (const auto &t : b.srvs)
		{
			if (t.slot == m.depth_srv)
				depth_descriptor = t.descriptor;
			if (t.slot == m.velocity_srv && t.format == TexFormat::r16g16b16a16_unorm)
				velocity_descriptor = t.descriptor;
		}

		// On a camera-cut frame velocity is the 1x1 dummy, so there is nothing to resolve.
		// That is the pass resetting, not an error.
		if (view_ok && depth_descriptor != 0 && velocity_descriptor != 0 && !m.camera_cut_dummies)
		{
			mark(2, "descriptors-found");
			auto *native_device = reinterpret_cast<ID3D12Device *>(device->get_native());
			auto *native = reinterpret_cast<ID3D12GraphicsCommandList *>(cmd_list->get_native());

			if (mv::initialise(native_device, m.render_width, m.render_height))
			{
				mv::ResolveInputs inputs;
				inputs.depth_descriptor = depth_descriptor;
				inputs.velocity_descriptor = velocity_descriptor;
				inputs.render_width = m.render_width;
				inputs.render_height = m.render_height;
				inputs.view = &view;

				mark(3, "mv-initialised");

				// The heaps that owned this dispatch's own descriptors — precisely what the
				// game had bound here.
				::ID3D12DescriptorHeap *const *game_heaps = b.heaps;
				const unsigned int heap_count = g_restore_heaps ? b.heap_count : 0u;

				mark(4, "heaps-collected");

				if (mv::record(native, inputs))
				{
					mark(5, "recorded");
					if (!g_resolve_ran)
					{
						g_resolve_ran = true;
						STRAY_LOG_INFO("MV resolve ran: %ux%u R16G16_FLOAT from depth=t%u "
							"velocity=t%u", m.render_width, m.render_height,
							m.depth_srv, m.velocity_srv);
					}

					// Our pass replaced the root signature, PSO and descriptor heaps on the
					// game's own command list. D3D12 has no state getters, so the state is
					// re-applied from what ReShade tracked. ORDER MATTERS: the heaps must go
					// back first, because the tracked descriptor TABLES are GPU handles into
					// the game's heap and re-binding them while our heap is current is an
					// invalid binding — UE4 dies with LowLevelFatalError, not an error you can
					// read. (docs/RESEARCH.md §3.5)
					if (heap_count > 0)
						native->SetDescriptorHeaps(heap_count, game_heaps);

					mark(6, "heaps-restored");

					// Restore the rest of the state too, heaps first.
					//
					// The earlier reasoning that UE4 re-sets the root signature and PSO before
					// every dispatch was wrong: its D3D12 RHI CACHES what it believes is
					// currently bound and skips redundant sets, so changing them behind its
					// back leaves that cache lying and the next draw uses ours. With our own
					// access violation now fixed, the remaining crash sits entirely inside the
					// game, which is what a state desync looks like.
					if (g_restore_state)
					{
						if (auto *tracked = cmd_list->get_private_data<state_tracking>())
							tracked->apply(cmd_list);
						mark(7, "state-restored");
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

	return false; // Phase A never suppresses the engine's dispatch.
}

} // namespace stray_dlss::taa_hook
