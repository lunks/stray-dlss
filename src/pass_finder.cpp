#include "pass_finder.hpp"

#include "frame_state.hpp"
#include "log.hpp"
#include "taa_hook.hpp"

#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace stray_dlss::pass_finder {
namespace {

// Two consecutive frames recorded out of every 30 — roughly twice a second at 60 fps. The
// pair is load-bearing: the round-trip comparison needs ADJACENT frames (see pass_finder.hpp).
constexpr std::uint64_t kSampleInterval = 30;
constexpr std::uint64_t kRecordedFramesPerInterval = 2;

// Dispatch grid gate: every pass on the tonemap chain is at least quarter-res of the render
// rect, so anything under 16x9 groups (128x72 px at the 8x8 TAA tile, CLAUDE.md §2.3) is
// bloom-tail noise not worth a descriptor-table resolve.
constexpr std::uint32_t kMinGridX = 16;
constexpr std::uint32_t kMinGridY = 9;

// A full-screen post-process draw is a 3-vertex triangle or a 4/6-vertex quad. Only those
// get their SRVs resolved — resolving per geometry draw would be thousands of table walks.
constexpr std::uint32_t kFullscreenDrawMaxVerts = 6;

// Chains and verdicts beyond this length would flood a log the user has to paste back.
constexpr int kVerdictLogBudget = 8;

std::mutex g_mutex;
bool g_enabled = false;
std::atomic<bool> g_recording{ false };

struct PipelineInfo
{
	std::uint64_t hash = 0;
	bool is_compute = false;
};
std::unordered_map<std::uint64_t, PipelineInfo> g_pipelines;

struct ListRecord
{
	std::vector<FrameEvent> events;        // record order; sequence assigned at execute
	std::vector<BoundTexture> current_rts; // RTVs + DSV of the open render pass
	bool draw_recorded_for_rts = false;    // draws are coalesced per render-target set
	std::uint64_t bound_compute_hash = 0;
	std::uint64_t bound_graphics_hash = 0;
};
std::unordered_map<reshade::api::command_list *, ListRecord> g_lists;

// The frame under assembly: everything executed since the last present.
std::vector<FrameEvent> g_frame;
std::uint64_t g_execute_counter = 0;

// History round-trip accumulation from recorded consecutive-frame pairs. Kept separately
// from taa_hook's tracking because that one only observes u0 of dispatches which survive
// its reporting throttle; this one sees every recorded output of every recorded dispatch.
std::unordered_map<std::uint64_t, std::vector<std::uint64_t>> g_prev_outputs;
std::uint64_t g_prev_recorded_frame = UINT64_MAX;
std::unordered_map<std::uint64_t, bool> g_roundtrip;

// Log the verdict when it CHANGES, within a budget — a stable verdict repeated twice a
// second would drown the very log the user pastes back.
std::uint64_t g_last_verdict_hash = UINT64_MAX;
std::string g_last_reason;
int g_verdicts_logged = 0;

const char *kind_name(FrameEvent::Kind k)
{
	switch (k)
	{
	case FrameEvent::Kind::dispatch: return "dispatch";
	case FrameEvent::Kind::draw:     return "draw";
	case FrameEvent::Kind::copy:     return "copy";
	}
	return "?";
}

const char *walk_format_name(TexFormat f)
{
	switch (f)
	{
	case TexFormat::r32_float_x8x24_typeless: return "DEPTH";
	case TexFormat::x32_typeless_g8x24_uint:  return "STENCIL";
	case TexFormat::r16g16b16a16_unorm:       return "RGBA16_UNORM";
	case TexFormat::r16g16b16a16_float:       return "RGBA16F";
	case TexFormat::r16g16_float:             return "RG16F";
	case TexFormat::r32g32b32a32_float:       return "RGBA32F";
	case TexFormat::r11g11b10_float:          return "R11G11B10F";
	case TexFormat::r10g10b10a2_unorm:        return "RGB10A2";
	case TexFormat::r32_float:                return "R32F";
	case TexFormat::r16_float:                return "R16F";
	case TexFormat::r8g8b8a8_unorm:           return "RGBA8";
	case TexFormat::b8g8r8a8_unorm:           return "BGRA8";
	case TexFormat::unknown:                  return "other";
	}
	return "?";
}

bool recording_now()
{
	return g_recording.load(std::memory_order_relaxed);
}

// Feeds the round-trip map from a recorded frame, then runs the walk and logs.
void finalise_frame(std::uint64_t frame_index, std::vector<FrameEvent> &&events,
                    std::uint64_t presented)
{
	{
		std::lock_guard<std::mutex> lock(g_mutex);

		// Round-trip: a dispatch on frame N+1 binding, as an SRV, anything the SAME hash
		// wrote on frame N. Only adjacent recorded frames count — the TAA history buffers
		// ping-pong with period two, so any wider gap compares a buffer against itself.
		if (g_prev_recorded_frame != UINT64_MAX && g_prev_recorded_frame + 1 == frame_index)
		{
			for (const auto &e : events)
			{
				if (e.kind != FrameEvent::Kind::dispatch || e.pipeline_hash == 0)
					continue;
				const auto prev = g_prev_outputs.find(e.pipeline_hash);
				if (prev == g_prev_outputs.end())
					continue;
				for (const auto &s : e.srvs)
				{
					if (s.width <= 1 && s.height <= 1)
						continue; // a 1x1 dummy is not a history rebind
					for (const std::uint64_t out : prev->second)
					{
						if (s.resource == out)
						{
							g_roundtrip[e.pipeline_hash] = true;
							break;
						}
					}
				}
			}
		}

		g_prev_outputs.clear();
		for (const auto &e : events)
		{
			if (e.kind != FrameEvent::Kind::dispatch || e.pipeline_hash == 0)
				continue;
			for (const auto &o : e.outputs)
				if (o.width > 1 || o.height > 1)
					g_prev_outputs[e.pipeline_hash].push_back(o.resource);
		}
		g_prev_recorded_frame = frame_index;
	}

	// The walk itself: pure, CI-proven, and fed both round-trip sources. Do NOT hold
	// g_mutex here — the oracle takes it per query.
	const auto owns_history = [](std::uint64_t hash) {
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			const auto it = g_roundtrip.find(hash);
			if (it != g_roundtrip.end() && it->second)
				return true;
		}
		return taa_hook::owns_temporal_history(hash);
	};

	const WalkResult r = find_temporal_pass(events, presented, owns_history);

	bool log_it = false;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		if ((r.taa_hash != g_last_verdict_hash || g_last_reason != r.reason) &&
			g_verdicts_logged < kVerdictLogBudget)
		{
			g_last_verdict_hash = r.taa_hash;
			g_last_reason = r.reason;
			++g_verdicts_logged;
			log_it = true;
		}
	}

	if (!log_it)
		return;

	STRAY_LOG_INFO("======== PASS FINDER  frame %llu, %zu events ========",
		static_cast<unsigned long long>(frame_index), events.size());
	STRAY_LOG_INFO("verdict: %s", r.reason);
	if (r.taa_hash != 0)
		STRAY_LOG_INFO("TAA = 0x%016llx  <-- log-only; nothing acts on this yet",
			static_cast<unsigned long long>(r.taa_hash));
	for (std::size_t i = 0; i < r.chain.size(); ++i)
	{
		const WalkHop &h = r.chain[i];
		STRAY_LOG_INFO("  hop %zu: %-8s seq=%llu:%llu hash=0x%016llx via res=0x%016llx "
			"%s %ux%u  %s", i, kind_name(h.kind),
			static_cast<unsigned long long>(h.sequence >> 32),
			static_cast<unsigned long long>(h.sequence & 0xFFFFFFFFull),
			static_cast<unsigned long long>(h.pipeline_hash),
			static_cast<unsigned long long>(h.resource),
			walk_format_name(h.format), h.width, h.height, h.note);
	}
	STRAY_LOG_INFO("====================================================");
}

} // namespace

void set_enabled(bool value)
{
	g_enabled = value;
	if (value)
		STRAY_LOG_WARN("PASS FINDER enabled ([STRAYDLSS] PassFinder): recording %llu of every "
			"%llu frames and LOGGING the dataflow walk's verdict. Diagnostic only; it drives "
			"nothing.", static_cast<unsigned long long>(kRecordedFramesPerInterval),
			static_cast<unsigned long long>(kSampleInterval));
}

bool enabled()
{
	return g_enabled;
}

void note_pipeline(std::uint64_t pipeline_handle, std::uint64_t shader_hash, bool is_compute)
{
	if (!g_enabled)
		return;
	std::lock_guard<std::mutex> lock(g_mutex);
	g_pipelines[pipeline_handle] = PipelineInfo{ shader_hash, is_compute };
}

void forget_pipeline(std::uint64_t pipeline_handle)
{
	if (!g_enabled)
		return;
	std::lock_guard<std::mutex> lock(g_mutex);
	g_pipelines.erase(pipeline_handle);
}

void note_bind_pipeline(reshade::api::command_list *cmd_list, std::uint64_t pipeline_handle)
{
	if (!g_enabled)
		return;
	std::lock_guard<std::mutex> lock(g_mutex);
	const auto it = g_pipelines.find(pipeline_handle);
	if (it == g_pipelines.end())
		return;
	ListRecord &lr = g_lists[cmd_list];
	if (it->second.is_compute)
		lr.bound_compute_hash = it->second.hash;
	else
		lr.bound_graphics_hash = it->second.hash;
}

void note_render_targets(reshade::api::command_list *cmd_list, uint32_t count,
                         const reshade::api::resource_view *rtvs, reshade::api::resource_view dsv)
{
	if (!recording_now())
		return;

	reshade::api::device *device = cmd_list->get_device();

	// describe_bound_view is liveness-checked first, like every view this project touches:
	// ReShade's view->resource map outlives the resource on D3D12. (frame_state.hpp)
	std::vector<BoundTexture> rts;
	for (uint32_t i = 0; i < count; ++i)
		describe_bound_view(device, rtvs[i], i, rts);
	if (dsv.handle != 0)
		describe_bound_view(device, dsv, count, rts);

	std::lock_guard<std::mutex> lock(g_mutex);
	ListRecord &lr = g_lists[cmd_list];
	lr.current_rts = std::move(rts);
	lr.draw_recorded_for_rts = false;
}

void note_draw(reshade::api::command_list *cmd_list, uint32_t vertex_or_index_count)
{
	if (!recording_now())
		return;

	bool want_srvs = false;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		ListRecord &lr = g_lists[cmd_list];
		if (lr.current_rts.empty())
			return;

		// One event per render-target set, not per draw: the walk only needs the last
		// writer of each target, and a base pass is thousands of draws to one target set.
		if (!lr.draw_recorded_for_rts)
		{
			FrameEvent e;
			e.kind = FrameEvent::Kind::draw;
			e.pipeline_hash = lr.bound_graphics_hash;
			e.outputs = lr.current_rts;
			lr.events.push_back(std::move(e));
			lr.draw_recorded_for_rts = true;
		}

		want_srvs = vertex_or_index_count <= kFullscreenDrawMaxVerts &&
			!lr.events.empty() && lr.events.back().kind == FrameEvent::Kind::draw &&
			lr.events.back().srvs.empty();
	}

	// Only a full-screen draw's SRVs matter (the anchor is the tonemapper's 3D LUT), and
	// the resolve is done OUTSIDE our lock — it takes frame_state's own.
	if (want_srvs)
	{
		std::vector<BoundTexture> srvs;
		resolve_graphics_srvs(cmd_list, srvs);

		std::lock_guard<std::mutex> lock(g_mutex);
		ListRecord &lr = g_lists[cmd_list];
		if (!lr.events.empty() && lr.events.back().kind == FrameEvent::Kind::draw &&
			lr.events.back().srvs.empty())
			lr.events.back().srvs = std::move(srvs);
	}
}

void note_dispatch(reshade::api::command_list *cmd_list, uint32_t x, uint32_t y, uint32_t z)
{
	if (!recording_now())
		return;
	if (z != 1 || x < kMinGridX || y < kMinGridY)
		return;

	// The same resolver the interception path trusts, outside our lock.
	DispatchBindings b;
	if (!resolve_compute_bindings(cmd_list, b))
		return;

	std::lock_guard<std::mutex> lock(g_mutex);
	ListRecord &lr = g_lists[cmd_list];

	FrameEvent e;
	e.kind = FrameEvent::Kind::dispatch;
	e.pipeline_hash = lr.bound_compute_hash;
	e.srvs = std::move(b.srvs);
	e.outputs = std::move(b.uavs);
	lr.events.push_back(std::move(e));
}

void note_copy(reshade::api::command_list *cmd_list, reshade::api::resource source,
               reshade::api::resource dest)
{
	if (!recording_now())
		return;
	if (source.handle == 0 || dest.handle == 0)
		return;
	// Liveness BEFORE get_resource_desc — the desc of a destroyed resource reads freed
	// memory and returns plausible garbage. (CLAUDE.md §5, "Two descriptor hazards")
	if (!is_resource_live(source.handle) || !is_resource_live(dest.handle))
		return;

	reshade::api::device *device = cmd_list->get_device();
	const reshade::api::resource_desc sd = device->get_resource_desc(source);
	const reshade::api::resource_desc dd = device->get_resource_desc(dest);
	if (sd.type == reshade::api::resource_type::buffer ||
		dd.type == reshade::api::resource_type::buffer)
		return; // only texture-to-texture copies can be on the colour chain

	FrameEvent e;
	e.kind = FrameEvent::Kind::copy;
	e.srvs.push_back(BoundTexture{ 0, source.handle, to_tex_format(sd.texture.format),
		sd.texture.width, sd.texture.height });
	e.outputs.push_back(BoundTexture{ 0, dest.handle, to_tex_format(dd.texture.format),
		dd.texture.width, dd.texture.height });

	std::lock_guard<std::mutex> lock(g_mutex);
	g_lists[cmd_list].events.push_back(std::move(e));
}

void note_execute(reshade::api::command_list *cmd_list)
{
	if (!g_enabled)
		return;
	std::lock_guard<std::mutex> lock(g_mutex);
	const auto it = g_lists.find(cmd_list);
	if (it == g_lists.end() || it->second.events.empty())
		return;

	// (execute-order << 32) | record-order: ExecuteCommandLists fires this per list, which
	// is what makes the sequence a total order per queue.
	const std::uint64_t base = (++g_execute_counter) << 32;
	std::uint64_t i = 0;
	for (auto &e : it->second.events)
	{
		e.sequence = base | i++;
		g_frame.push_back(std::move(e));
	}
	it->second.events.clear();
}

void forget_command_list(reshade::api::command_list *cmd_list)
{
	if (!g_enabled)
		return;
	std::lock_guard<std::mutex> lock(g_mutex);
	g_lists.erase(cmd_list);
}

void on_present(std::uint64_t frame, reshade::api::resource back_buffer)
{
	if (!g_enabled)
		return;

	if (recording_now())
	{
		std::vector<FrameEvent> events;
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			events.swap(g_frame);
		}
		finalise_frame(frame, std::move(events), back_buffer.handle);
	}

	const std::uint64_t next = frame + 1;
	const bool record_next = (next % kSampleInterval) < kRecordedFramesPerInterval;
	if (record_next && !recording_now())
	{
		// Entering a recording window: drop stragglers from lists recorded in the previous
		// window but executed after its presents, so they cannot masquerade as this frame's
		// writers.
		std::lock_guard<std::mutex> lock(g_mutex);
		g_frame.clear();
	}
	g_recording.store(record_next, std::memory_order_relaxed);
}

} // namespace stray_dlss::pass_finder
