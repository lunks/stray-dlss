#include "taa_hook.hpp"

#include "frame_state.hpp"
#include "log.hpp"

#include <cstring>
#include <mutex>
#include <unordered_map>

namespace stray_dlss::taa_hook {
namespace {

std::mutex g_mutex;
std::unordered_map<uint64_t, std::uint64_t> g_pipeline_hashes;                  // pipeline -> DXBC hash
std::unordered_map<reshade::api::command_list *, uint64_t> g_bound;             // cmd list -> pipeline
std::unordered_map<std::uint64_t, std::uint64_t> g_prev_output;                 // hash -> its u0 last frame
std::unordered_map<std::uint64_t, bool> g_reported;                             // hash -> already logged

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
	STRAY_LOG_INFO("verdict = %s (%s)", verdict_name(m.verdict), m.reason);
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
		const auto b = g_bound.find(cmd_list);
		if (b == g_bound.end())
			return false;
		const auto h = g_pipeline_hashes.find(b->second);
		if (h == g_pipeline_hashes.end())
			return false;
		hash = h->second;

		if (g_reported[hash])
		{
			// Still track the output resource each frame: the history round-trip (this
			// frame's u0 reappearing as an SRV next frame) is the decisive test for which
			// colour SRV is the history. (CLAUDE.md §2.3)
			return false;
		}
	}

	DispatchBindings b;
	if (!resolve_compute_bindings(cmd_list, b))
		return false;

	reshade::api::device *device = cmd_list->get_device();

	ue4::ViewParams view{};
	bool view_ok = false;
	if (b.view_cb_valid)
		view_ok = read_view_cb(device, b.view_cb, view) && ue4::view_params_plausible(view);

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

	// Report every large dispatch once, whatever the verdict. During Phase A the point is to
	// see the whole field, not only what we already expect to find.
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		if (g_reported[hash])
			return false;
		g_reported[hash] = true;
		++g_diag.candidates_reported;
	}

	report(hash, b, m, view, view_ok, x, y, z);

	if (view_ok)
		g_diag.view_seen = true;

	if (m.verdict == MatchVerdict::hash_and_structural || m.verdict == MatchVerdict::structural_only)
	{
		g_diag.best_hash = hash;
		g_diag.best_width = w;
		g_diag.best_height = h;
	}

	// Remember this pass's output so the next frame can recognise it as the history.
	for (const auto &t : b.uavs)
	{
		if (t.slot == 0)
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			g_prev_output[hash] = t.resource;
		}
	}

	return false; // Phase A never suppresses the engine's dispatch.
}

} // namespace stray_dlss::taa_hook
