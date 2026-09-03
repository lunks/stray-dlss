#include "reshade_backend.hpp"

#include "reshade_bindings.hpp"

#include <cstring>

namespace stray_dlss::rsb {
namespace {

reshade::api::device *g_device = nullptr;

reshade::api::command_list *list_of(const icept::CommandContext &ctx)
{
	return reinterpret_cast<reshade::api::command_list *>(ctx.backend_cookie);
}

} // namespace

ReshadeBackend &backend()
{
	static ReshadeBackend b;
	return b;
}

void set_device(reshade::api::device *device) { g_device = device; }
reshade::api::device *device() { return g_device; }

icept::CommandContext context_for(reshade::api::command_list *cmd_list)
{
	icept::CommandContext ctx;
	if (cmd_list == nullptr)
		return ctx;
	// get_native() returns uint64_t and hands back the ORIGINAL vkd3d objects, not ReShade's
	// proxies — which is what NGX must be given. (docs/RESEARCH.md §1.2)
	ctx.native = reinterpret_cast<ID3D12GraphicsCommandList *>(cmd_list->get_native());
	reshade::api::device *dev = cmd_list->get_device();
	ctx.device = dev != nullptr ? reinterpret_cast<ID3D12Device *>(dev->get_native()) : nullptr;
	ctx.backend_cookie = reinterpret_cast<std::uint64_t>(cmd_list);
	return ctx;
}

icept::PresentContext present_context_for(reshade::api::command_queue *queue,
                                          reshade::api::swapchain *swapchain, std::uint64_t frame)
{
	icept::PresentContext pc;
	pc.frame = frame;
	if (queue != nullptr)
	{
		pc.queue = reinterpret_cast<ID3D12CommandQueue *>(queue->get_native());
		// ReShade's OWN immediate command list, which the D3D12 present path flushes at
		// dxgi_swapchain.cpp:1009 — after this event and after the game has submitted every
		// list of the frame to the same queue. THREAD SAFETY: the D3D12 branch of
		// DXGISwapChain::on_present takes unique_direct3d_device_lock on the queue's mutex
		// before invoking any event, precisely so "the immediate command list may be accessed".
		if (reshade::api::command_list *il = queue->get_immediate_command_list())
		{
			pc.present_list = reinterpret_cast<ID3D12GraphicsCommandList *>(il->get_native());
			pc.backend_cookie = reinterpret_cast<std::uint64_t>(il);
		}
	}
	if (swapchain != nullptr)
	{
		pc.swapchain = reinterpret_cast<IDXGISwapChain3 *>(swapchain->get_native());
		pc.back_buffer = reinterpret_cast<ID3D12Resource *>(swapchain->get_current_back_buffer().handle);
	}
	return pc;
}

bool ReshadeBackend::resolve_compute_bindings(const icept::CommandContext &ctx, icept::DispatchBindings &out)
{
	reshade::api::command_list *cl = list_of(ctx);
	return cl != nullptr && rsb::resolve_compute_bindings(cl, out);
}

void ReshadeBackend::describe_view(icept::DescriptorId view, std::uint32_t reg, std::vector<BoundTexture> &out)
{
	if (g_device != nullptr)
		rsb::describe_bound_view(g_device, reshade::api::resource_view{ view }, reg, out);
}

bool ReshadeBackend::describe_resource(icept::ResourceId res, icept::ResourceInfo &out)
{
	// Liveness FIRST: get_resource_desc dereferences the resource, and ReShade's own maps
	// outlive the resource on D3D12 (CLAUDE.md §5, "Two descriptor hazards").
	if (g_device == nullptr || res == 0 || !rsb::is_resource_live(res))
		return false;
	const reshade::api::resource_desc rd = g_device->get_resource_desc(reshade::api::resource{ res });
	out = icept::ResourceInfo{};
	out.is_buffer = rd.type == reshade::api::resource_type::buffer;
	if (out.is_buffer)
	{
		out.buffer_size = rd.buffer.size;
	}
	else
	{
		out.width = rd.texture.width;
		out.height = rd.texture.height;
		out.mip_levels = rd.texture.levels;
		out.array_size = rd.texture.depth_or_layers;
		out.sample_count = rd.texture.samples;
		out.dxgi_format = static_cast<std::uint32_t>(rd.texture.format);
		out.format = tex_format_from_dxgi(out.dxgi_format);
		out.is_3d = rd.type == reshade::api::resource_type::texture_3d;
	}
	// `unknown` is accepted alongside `upload`, exactly as the View CB read always has: ReShade
	// reports unknown for heaps it could not classify, and refusing those lost real reads.
	out.upload_heap = rd.heap == reshade::api::memory_heap::upload ||
		rd.heap == reshade::api::memory_heap::unknown;
	out.allow_uav = (static_cast<std::uint32_t>(rd.usage) &
		static_cast<std::uint32_t>(reshade::api::resource_usage::unordered_access)) != 0;
	return true;
}

bool ReshadeBackend::resource_from_view(icept::DescriptorId view, icept::ResourceId &out)
{
	// IDENTITY ONLY, deliberately not liveness-checked: the two callers compare the answer
	// against known back-buffer identities and never dereference it without is_resource_live.
	// ReShade's view->resource map can hand back a destroyed resource's address here.
	out = 0;
	if (g_device == nullptr || view == 0)
		return false;
	out = g_device->get_resource_from_view(reshade::api::resource_view{ view }).handle;
	return out != 0;
}

bool ReshadeBackend::read_buffer(const icept::BufferRange &range, std::uint64_t bytes, void *out)
{
	if (g_device == nullptr || range.buffer == 0 || out == nullptr)
		return false;
	void *mapped = nullptr;
	if (!g_device->map_buffer_region(reshade::api::resource{ range.buffer }, range.offset, bytes,
			reshade::api::map_access::read_only, &mapped) || mapped == nullptr)
		return false;
	std::memcpy(out, mapped, static_cast<size_t>(bytes));
	g_device->unmap_buffer_region(reshade::api::resource{ range.buffer });
	return true;
}

bool ReshadeBackend::is_resource_live(icept::ResourceId res)
{
	return rsb::is_resource_live(res);
}

void ReshadeBackend::restore_game_compute_state(const icept::CommandContext &ctx)
{
	if (reshade::api::command_list *cl = list_of(ctx))
		rsb::restore_game_compute_state(cl);
}

void ReshadeBackend::present_barrier(const icept::PresentContext &ctx, icept::ResourceId res,
                                     std::uint32_t before, std::uint32_t after)
{
	auto *cl = reinterpret_cast<reshade::api::command_list *>(ctx.backend_cookie);
	if (cl == nullptr || res == 0)
		return;
	// api::resource_usage's values ARE the D3D12_RESOURCE_STATES bits — copy_dest 0x400,
	// shader_resource 0xC0, and convert_usage_to_resource_states (d3d12_impl_type_convert
	// .cpp:236) is a plain mask — so this is a reinterpretation, not a translation. Going
	// through the ReShade API rather than the native list is what marks the immediate list
	// as having commands, so that it is actually flushed at present (CLAUDE.md §5).
	cl->barrier(reshade::api::resource{ res },
		static_cast<reshade::api::resource_usage>(before),
		static_cast<reshade::api::resource_usage>(after));
}

void ReshadeBackend::dump_tracker_state(const icept::CommandContext &ctx, const char *why)
{
	if (reshade::api::command_list *cl = list_of(ctx))
		rsb::dump_tracker_state_for(cl, why);
}

} // namespace stray_dlss::rsb
