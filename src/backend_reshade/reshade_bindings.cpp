#include "reshade_bindings.hpp"

#include "d3d12_restore.hpp"
#include "log.hpp"

#include <d3d12.h>

#include <descriptor_tracking.hpp>
#include <state_tracking.hpp>

#include <atomic>
#include <algorithm>
#include <cstring>
#include <mutex>
#include <utility>
#include <vector>
#include <unordered_map>
#include <unordered_set>

namespace stray_dlss::rsb {
namespace {

// Root descriptors bypass the descriptor heap entirely, so they have to be remembered as they
// go by. Keyed by command list, cleared when the list is reset.
struct RootDescriptors
{
	std::vector<BoundTexture> srvs;
	std::vector<BoundTexture> uavs;
	// Every root constant buffer CURRENTLY bound, one per root parameter (last write wins, as
	// D3D12 has it). UE4 binds several, and which one carries the View uniform buffer is not
	// fixed — keeping only one lost it on most frames. This used to be an append-only list
	// for the life of the command list: run F showed 31 entries with the same offset
	// repeated seven times at one TAA dispatch, i.e. a HISTORY of every push since Reset,
	// not what was bound — which made the oracle the wrong side of every `cb:` diff.
	std::unordered_map<uint32_t, icept::BufferRange> root_cbv_range;

	// --- everything needed to REPLAY the game's compute root arguments ---
	//
	// Keyed by ROOT PARAMETER INDEX (not shader register), because that is what
	// SetComputeRoot* takes, and valid ONLY for `layout`. UE4 binds dozens of different
	// compute root signatures per command list; replaying arguments harvested under one
	// signature into another writes to whatever that index happens to be there — a descriptor
	// table, or a parameter that does not exist at all. Everything is discarded the moment the
	// layout changes.
	std::uint64_t layout = 0;
	std::unordered_map<uint32_t, std::uint64_t> root_cbv_va;
	std::unordered_map<uint32_t, std::uint64_t> root_srv_va;
	std::unordered_map<uint32_t, std::uint64_t> root_uav_va;
	std::unordered_map<uint32_t, std::vector<uint32_t>> root_constants;

	void reset_root_args(std::uint64_t new_layout)
	{
		layout = new_layout;
		root_cbv_range.clear();
		root_cbv_va.clear();
		root_srv_va.clear();
		root_uav_va.clear();
		root_constants.clear();
	}
};

std::mutex g_mutex;
std::unordered_map<reshade::api::command_list *, RootDescriptors> g_root;

// Deep copies of every pipeline layout's descriptor ranges, for both table variants.
struct LayoutParam
{
	bool is_table = false;
	std::vector<reshade::api::descriptor_range> ranges;
};
std::unordered_map<uint64_t, std::vector<LayoutParam>> g_layouts;

// What describe() refused, when the caller wants to know (the differential observer does:
// a slot the oracle drops in silence is otherwise indistinguishable from one it never had).
// Reason 3: the view maps to no resource; 4: the resource is dead per ReShade's own liveness.
thread_local std::vector<icept::DispatchBindings::Unresolved> *t_refusals = nullptr;
thread_local char t_refusal_kind = 't';

void describe(reshade::api::device *device, reshade::api::resource_view view,
              uint32_t reg, std::vector<BoundTexture> &out)
{
	if (view.handle == 0)
		return;

	const reshade::api::resource res = device->get_resource_from_view(view);
	if (res.handle == 0)
	{
		if (t_refusals != nullptr)
			t_refusals->push_back(icept::DispatchBindings::Unresolved{ t_refusal_kind, reg, view.handle, 3, 0 });
		return;
	}

	// Liveness FIRST. get_resource_desc dereferences the resource, and ReShade's view->resource
	// map outlives the resource on D3D12 — so for a descriptor slot UE4 has recycled this is a
	// read of freed memory, inside ReShade, before any of our own guards can run. Gating the
	// resolve later was not enough: the access violation happens here, during binding capture,
	// which is why the game still died on a frame the resolve correctly skipped.
	if (!is_resource_live(res.handle))
	{
		if (t_refusals != nullptr)
			t_refusals->push_back(icept::DispatchBindings::Unresolved{ t_refusal_kind, reg, view.handle, 4, res.handle });
		return;
	}

	const reshade::api::resource_view_desc vd = device->get_resource_view_desc(view);
	const reshade::api::resource_desc rd = device->get_resource_desc(res);

	BoundTexture t;
	t.slot = reg;
	t.resource = res.handle;
	t.descriptor = view.handle; // a real D3D12_CPU_DESCRIPTOR_HANDLE.ptr
	if (rd.type == reshade::api::resource_type::buffer)
	{
		// A buffer has no extent; reading rd.texture on one prints nonsense.
		t.format = TexFormat::unknown;
		t.width = 0;
		t.height = 0;
		out.push_back(t);
		return;
	}
	// The VIEW format is what matters, not the resource format: depth and stencil are two
	// views of one typeless resource and are only distinguishable this way. (CLAUDE.md §2.3)
	t.format = to_tex_format(vd.format != reshade::api::format::unknown ? vd.format : rd.texture.format);
	t.width = rd.texture.width;
	t.height = rd.texture.height;
	// A 3D SRV in an Unreal frame is the tonemapper's colour-grading LUT and nothing else;
	// the deleted dataflow pass finder anchored on it. Reported because it is free.
	t.is_3d = rd.type == reshade::api::resource_type::texture_3d;
	out.push_back(t);
}

// Resolves a bound buffer to its GPU virtual address NOW, while the resource is certainly
// alive. Storing the ID3D12Resource* and dereferencing it at replay time would reintroduce the
// stale-pointer hazard that ReShade's missing destroy_resource_view on D3D12 already caused
// once. (docs/RESEARCH.md §2.7)
std::uint64_t gpu_address(const icept::BufferRange &range)
{
	if (range.buffer == 0)
		return 0;
	auto *res = reinterpret_cast<::ID3D12Resource *>(range.buffer);
	return res->GetGPUVirtualAddress() + range.offset;
}

} // namespace

void note_pipeline_layout(
	reshade::api::device *device,
	uint32_t count,
	const reshade::api::pipeline_layout_param *params,
	reshade::api::pipeline_layout layout)
{
	(void)device;

	std::vector<LayoutParam> copy(count);
	for (uint32_t i = 0; i < count; ++i)
	{
		const auto &p = params[i];

		if (p.type == reshade::api::pipeline_layout_param_type::descriptor_table)
		{
			copy[i].is_table = true;
			copy[i].ranges.assign(p.descriptor_table.ranges,
			                      p.descriptor_table.ranges + p.descriptor_table.count);
		}
		else if (p.type == reshade::api::pipeline_layout_param_type::descriptor_table_with_flags)
		{
			// Slice the flags away: only the descriptor_range base is needed, and copying by
			// value here is exactly what ReShade's utility fails to do.
			copy[i].is_table = true;
			copy[i].ranges.reserve(p.descriptor_table_with_flags.count);
			for (uint32_t r = 0; r < p.descriptor_table_with_flags.count; ++r)
				copy[i].ranges.push_back(
					static_cast<const reshade::api::descriptor_range &>(p.descriptor_table_with_flags.ranges[r]));
		}
	}

	std::lock_guard<std::mutex> lock(g_mutex);
	g_layouts[layout.handle] = std::move(copy);
}

void forget_pipeline_layout(reshade::api::pipeline_layout layout)
{
	std::lock_guard<std::mutex> lock(g_mutex);
	g_layouts.erase(layout.handle);
}

// Defined further down, next to the resolve it explains.
void dump_tracker_state(reshade::api::device *device, state_tracking *state, descriptor_tracking *desc);

void dump_tracker_state_for(reshade::api::command_list *cmd_list, const char *why)
{
	reshade::api::device *device = cmd_list->get_device();
	auto *state = cmd_list->get_private_data<state_tracking>();
	auto *desc = device->get_private_data<descriptor_tracking>();
	STRAY_LOG_INFO("tracker dump (%s):", why);
	if (state == nullptr || desc == nullptr)
	{
		STRAY_LOG_ERROR("  tracker private data missing (state=%p desc=%p)",
			static_cast<void *>(state), static_cast<void *>(desc));
		return;
	}
	dump_tracker_state(device, state, desc);
}

void note_push_descriptors(
	reshade::api::command_list *cmd_list,
	reshade::api::shader_stage stages,
	reshade::api::pipeline_layout layout,
	uint32_t layout_param,
	const reshade::api::descriptor_table_update &update)
{
	(void)layout;

	// Compute only. In D3D12 ReShade reports all_compute for the compute root signature.
	if ((static_cast<uint32_t>(stages) & static_cast<uint32_t>(reshade::api::shader_stage::all_compute)) == 0)
		return;

	reshade::api::device *device = cmd_list->get_device();

	std::lock_guard<std::mutex> lock(g_mutex);
	RootDescriptors &rd = g_root[cmd_list];

	if (rd.layout != layout.handle)
		rd.reset_root_args(layout.handle);

	for (uint32_t i = 0; i < update.count; ++i)
	{
		const uint32_t reg = update.binding + i;

		switch (update.type)
		{
		case reshade::api::descriptor_type::constant_buffer:
		{
			const auto *ranges = static_cast<const reshade::api::buffer_range *>(update.descriptors);
			// Keep EVERY root constant buffer. UE4 binds several and which one carries the
			// View uniform buffer is not fixed, so overwriting a single slot loses it on most
			// frames — which is why the CB read intermittently failed.
			if (ranges[i].buffer.handle != 0)
			{
				const icept::BufferRange br = to_range(ranges[i]);
				// Last write wins per root parameter, which matches D3D12 semantics.
				rd.root_cbv_range[layout_param] = br;
				rd.root_cbv_va[layout_param] = gpu_address(br);
			}
			break;
		}
		case reshade::api::descriptor_type::shader_resource_view:
		{
			const auto *views = static_cast<const reshade::api::resource_view *>(update.descriptors);
			describe(device, views[i], reg, rd.srvs);
			break;
		}
		case reshade::api::descriptor_type::unordered_access_view:
		{
			const auto *views = static_cast<const reshade::api::resource_view *>(update.descriptors);
			describe(device, views[i], reg, rd.uavs);
			break;
		}
		case reshade::api::descriptor_type::buffer_shader_resource_view:
		case reshade::api::descriptor_type::buffer_unordered_access_view:
		{
			// These do NOT carry a buffer_range, despite sharing the struct with the constant
			// buffer case above. Verified in ReShade v6.8.0 source/d3d12/d3d12_command_list.cpp:
			// SetComputeRootShaderResourceView / ...UnorderedAccessView pass `&BufferLocation`
			// — a pointer to ONE 8-byte D3D12_GPU_VIRTUAL_ADDRESS on the wrapper's stack.
			// Reading that as a 24-byte buffer_range overruns it by 16 bytes, and then treating
			// the GPU address as an ID3D12Resource* and calling a virtual on it is a wild
			// dereference. The address is already exactly what a replay needs.
			const auto *addresses = static_cast<const std::uint64_t *>(update.descriptors);
			if (addresses[i] != 0)
			{
				if (update.type == reshade::api::descriptor_type::buffer_shader_resource_view)
					rd.root_srv_va[layout_param] = addresses[i];
				else
					rd.root_uav_va[layout_param] = addresses[i];
			}
			break;
		}
		default:
			break;
		}
	}
}

// One-shot dump of what the trackers actually hold. Written the first time a resolve is
// attempted, because "found nothing" has several possible causes and they need different
// fixes: no tables tracked at all, tables whose layout params are push descriptors, or heap
// offsets that do not resolve.
void dump_tracker_state(reshade::api::device *device, state_tracking *state, descriptor_tracking *desc)
{
	// desc is no longer consulted for layout params — we keep our own deep copies — but the
	// signature is kept so the dump can grow back into it if needed.
	(void)desc;

	STRAY_LOG_INFO("---- descriptor tracker state ----");
	STRAY_LOG_INFO("  descriptor_tables entries: %zu", state->descriptor_tables.size());

	for (const auto &entry : state->descriptor_tables)
	{
		STRAY_LOG_INFO("  stage_key=0x%08x layout=0x%016llx tables=%zu",
			static_cast<uint32_t>(entry.first),
			static_cast<unsigned long long>(entry.second.first.handle),
			entry.second.second.size());

		for (size_t param = 0; param < entry.second.second.size(); ++param)
		{
			const auto table = entry.second.second[param];
			if (table.handle == 0)
				continue;

			STRAY_LOG_INFO("    param %zu: table=0x%016llx",
				param, static_cast<unsigned long long>(table.handle));

			std::vector<reshade::api::descriptor_range> ranges;
			{
				std::lock_guard<std::mutex> lock(g_mutex);
				const auto lit = g_layouts.find(entry.second.first.handle);
				if (lit != g_layouts.end() && param < lit->second.size() && lit->second[param].is_table)
					ranges = lit->second[param].ranges;
			}
			const uint32_t range_count = static_cast<uint32_t>(ranges.size());

			STRAY_LOG_INFO("      ranges=%u", range_count);
			for (uint32_t r = 0; r < range_count && r < 8; ++r)
			{
				const reshade::api::descriptor_range &range = ranges[r];
				reshade::api::descriptor_heap heap = { 0 };
				uint32_t offset = 0;
				device->get_descriptor_heap_offset(table, range.binding, 0, &heap, &offset);
				STRAY_LOG_INFO("        r%u type=%d binding=%u reg=%u count=%u heap=0x%016llx off=%u",
					r, static_cast<int>(range.type), range.binding, range.dx_register_index,
					range.count, static_cast<unsigned long long>(heap.handle), offset);
			}
		}
	}
	STRAY_LOG_INFO("---------------------------------");
}

bool resolve_compute_bindings(reshade::api::command_list *cmd_list, DispatchBindings &out)
{
	reshade::api::device *device = cmd_list->get_device();

	auto *state = cmd_list->get_private_data<state_tracking>();
	auto *desc = device->get_private_data<descriptor_tracking>();
	if (state == nullptr || desc == nullptr)
	{
		static bool said = false;
		if (!said)
		{
			said = true;
			STRAY_LOG_ERROR("Tracker private data missing (state=%p desc=%p). The tracking "
				"utilities did not register.", static_cast<void *>(state), static_cast<void *>(desc));
		}
		return false;
	}

	static bool dumped = false;
	if (!dumped)
	{
		dumped = true;
		dump_tracker_state(device, state, desc);
	}

	// ReShade's D3D12 backend reports the compute root signature as
	// `all_compute | all_ray_tracing`, NOT plain `all_compute`, so an exact-match lookup finds
	// nothing and the whole resolve silently returns empty. Accept any key carrying the
	// compute bit. (docs/RESEARCH.md §2.6)
	constexpr auto kComputeBits = static_cast<uint32_t>(reshade::api::shader_stage::all_compute);

	for (const auto &entry : state->descriptor_tables)
	{
		if ((static_cast<uint32_t>(entry.first) & kComputeBits) == 0)
			continue;

		const reshade::api::pipeline_layout layout = entry.second.first;
		const std::vector<reshade::api::descriptor_table> &tables = entry.second.second;

		for (uint32_t param = 0; param < tables.size(); ++param)
		{
			if (tables[param].handle == 0)
				continue;

			// Our own deep copy, not descriptor_tracking's — see note_pipeline_layout.
			std::vector<reshade::api::descriptor_range> ranges;
			{
				std::lock_guard<std::mutex> lock(g_mutex);
				const auto lit = g_layouts.find(layout.handle);
				if (lit == g_layouts.end() || param >= lit->second.size())
					continue;
				if (!lit->second[param].is_table)
					continue;
				ranges = lit->second[param].ranges;
			}
			const uint32_t range_count = static_cast<uint32_t>(ranges.size());

			for (uint32_t r = 0; r < range_count; ++r)
			{
				const reshade::api::descriptor_range &range = ranges[r];

				for (uint32_t i = 0; i < range.count; ++i)
				{
					// An unbounded range would run away; UE4 does not use them here.
					if (range.count == UINT32_MAX)
						break;

					reshade::api::descriptor_heap heap = { 0 };
					uint32_t offset = 0;
					device->get_descriptor_heap_offset(tables[param], range.binding + i, 0, &heap, &offset);
					if (heap.handle == 0)
						continue;

					// Remember the heap this dispatch actually used, so it can be restored
					// exactly rather than guessed at afterwards.
					if (out.heap_count < 2)
					{
						auto *native_heap = reinterpret_cast<::ID3D12DescriptorHeap *>(heap.handle);
						bool known = false;
						for (unsigned int h = 0; h < out.heap_count; ++h)
							known = known || out.heaps[h] == native_heap;
						if (!known)
							out.heaps[out.heap_count++] = native_heap;
					}

					// The shader register, which is what every fact in CLAUDE.md §2.3 is
					// expressed in — not the descriptor's index in the table.
					const uint32_t reg = range.dx_register_index + i;

					// ONE-TIME: is the heap these descriptors live in shader-visible?
					//
					// mv_resolve copies them with CopyDescriptorsSimple, and D3D12 forbids a
					// shader-visible SOURCE — reproduced in CI as
					// "D3D12 ERROR #654: SrcDescriptorRangeStart points to a descriptor heap
					// type that is CPU write only, so reading it is invalid". vkd3d-proton has
					// no debug layer to say so, and an illegal copy there yields a descriptor
					// the GPU rejects only when something reads it: exactly why MvDispatch=0
					// survives and a single 1x1 dispatch hangs with Xid 109.
					//
					// Read-only, logged once, so the premise is measured rather than assumed.
					{
						static std::atomic<bool> reported{ false };
						bool expected = false;
						if (reported.compare_exchange_strong(expected, true) &&
							heap.handle != 0)
						{
							auto *native = reinterpret_cast<::ID3D12DescriptorHeap *>(heap.handle);
							const D3D12_DESCRIPTOR_HEAP_DESC hd = native->GetDesc();
							const bool visible =
								(hd.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) != 0;
							STRAY_LOG_INFO("Descriptor source heap: type=%d NumDescriptors=%u "
								"SHADER_VISIBLE=%s%s", static_cast<int>(hd.Type),
								hd.NumDescriptors, visible ? "YES" : "no",
								visible ? "  <-- CopyDescriptorsSimple from this is ILLEGAL "
								          "(D3D12 #654); this is very likely the hang"
								        : "  (legal copy source)");
						}
					}

					switch (range.type)
					{
					case reshade::api::descriptor_type::shader_resource_view:
						t_refusals = &out.unresolved;
						t_refusal_kind = 't';
						describe(device, desc->get_resource_view(heap, offset), reg, out.srvs);
						t_refusals = nullptr;
						break;
					case reshade::api::descriptor_type::unordered_access_view:
						t_refusals = &out.unresolved;
						t_refusal_kind = 'u';
						describe(device, desc->get_resource_view(heap, offset), reg, out.uavs);
						t_refusals = nullptr;
						break;
					case reshade::api::descriptor_type::constant_buffer:
					{
						const reshade::api::buffer_range br = desc->get_buffer_range(heap, offset);
						if (br.buffer.handle != 0 && !out.view_cb_valid)
						{
							out.view_cb = to_range(br);
							out.view_cb_valid = true;
							out.view_cb_register = reg;
						}
						break;
					}
					default:
						break;
					}
				}
			}
		}
	}

	// Root descriptors win where they overlap, since they were set most directly.
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		const auto rit = g_root.find(cmd_list);
		if (rit != g_root.end())
		{
			const RootDescriptors &rd = rit->second;
			out.srvs.insert(out.srvs.end(), rd.srvs.begin(), rd.srvs.end());
			out.uavs.insert(out.uavs.end(), rd.uavs.begin(), rd.uavs.end());
			for (const auto &cb : rd.root_cbv_range)
				out.constant_buffers.emplace_back(cb.first, cb.second);
			// Deterministic order (by root parameter), so two resolves of one state read alike.
			std::sort(out.constant_buffers.begin(), out.constant_buffers.end(),
				[](const auto &a, const auto &b) { return a.first < b.first; });
		}
	}

	return !out.srvs.empty() || !out.uavs.empty() || !out.constant_buffers.empty();
}

void note_push_constants(
	reshade::api::command_list *cmd_list,
	reshade::api::shader_stage stages,
	reshade::api::pipeline_layout layout,
	uint32_t layout_param,
	uint32_t first,
	uint32_t count,
	const void *values)
{
	(void)layout;
	if ((static_cast<uint32_t>(stages) & static_cast<uint32_t>(reshade::api::shader_stage::all_compute)) == 0)
		return;
	if (values == nullptr || count == 0)
		return;

	std::lock_guard<std::mutex> lock(g_mutex);
	RootDescriptors &rd = g_root[cmd_list];
	if (rd.layout != layout.handle)
		rd.reset_root_args(layout.handle);
	std::vector<uint32_t> &dst = rd.root_constants[layout_param];
	if (dst.size() < first + count)
		dst.resize(first + count, 0);
	std::memcpy(dst.data() + first, values, static_cast<size_t>(count) * 4);
}

void restore_game_compute_state(reshade::api::command_list *cmd_list)
{
	auto *native = reinterpret_cast<::ID3D12GraphicsCommandList *>(cmd_list->get_native());
	auto *state = cmd_list->get_private_data<state_tracking>();
	if (native == nullptr || state == nullptr)
		return;

	constexpr auto kComputeBits = static_cast<uint32_t>(reshade::api::shader_stage::all_compute);

	// Find the game's compute layout and tables, and its graphics tables.
	reshade::api::pipeline_layout compute_layout = { 0 };
	const std::vector<reshade::api::descriptor_table> *compute_tables = nullptr;
	reshade::api::shader_stage compute_key = reshade::api::shader_stage::all_compute;
	reshade::api::pipeline_layout graphics_layout = { 0 };
	const std::vector<reshade::api::descriptor_table> *graphics_tables = nullptr;
	reshade::api::shader_stage graphics_key = reshade::api::shader_stage::all_graphics;

	for (const auto &entry : state->descriptor_tables)
	{
		if ((static_cast<uint32_t>(entry.first) & kComputeBits) != 0)
		{
			compute_key = entry.first;
			compute_layout = entry.second.first;
			compute_tables = &entry.second.second;
		}
		else
		{
			graphics_key = entry.first;
			graphics_layout = entry.second.first;
			graphics_tables = &entry.second.second;
		}
	}

	if (compute_layout.handle == 0)
		return; // nothing tracked to restore to

	// 1. THE KEY CALL. A count of 0 through ReShade's own API forces BOTH of its otherwise
	//    cached restore actions: SetDescriptorHeaps with the application's exact heap pair —
	//    taken from ReShade's own record, which is still truthful because our clobber was
	//    native and never touched it — and SetComputeRootSignature, which also re-syncs
	//    ReShade's cache so its later calls stop skipping redundant sets.
	//
	//    This replaces both the heap inference (which could only guess at a sampler heap the
	//    game bound but did not use here, and would silently unbind it) and the native
	//    SetComputeRootSignature.
	cmd_list->bind_descriptor_tables(compute_key, compute_layout, 0, 0, nullptr);

	// 2. Compute tables, ONE AT A TIME, skipping zero handles.
	//
	//    Binding the whole vector in one call — which is what ReShade's own state_block does —
	//    passes zero handles through for parameters that are not tables at all, making ReShade
	//    issue SetComputeRootDescriptorTable on what is actually a root CBV. A non-zero handle
	//    at index i is itself proof that i is a table in this layout, because state_tracking
	//    clears the vector whenever the layout changes.
	if (compute_tables != nullptr)
	{
		for (uint32_t param = 0; param < compute_tables->size(); ++param)
		{
			const auto table = (*compute_tables)[param];
			if (table.handle == 0)
				continue;
			cmd_list->bind_descriptor_tables(compute_key, compute_layout, param, 1, &table);
		}
	}

	// 3. Graphics tables, same rule. The heap change invalidates tables regardless of pipeline
	//    type. Graphics root descriptors and constants are NOT restored — we never touch the
	//    graphics root signature, so they were never invalidated.
	if (graphics_tables != nullptr && graphics_layout.handle != 0)
	{
		for (uint32_t param = 0; param < graphics_tables->size(); ++param)
		{
			const auto table = (*graphics_tables)[param];
			if (table.handle == 0)
				continue;
			cmd_list->bind_descriptor_tables(graphics_key, graphics_layout, param, 1, &table);
		}
	}

	// 4. Compute root arguments and the PSO, NATIVELY — the part ReShade cannot replay for us.
	//    Native because ReShade caches nothing for root descriptors, so there is nothing to
	//    desync, and because its push_descriptors path for root SRV/UAV mis-handles them.
	//
	//    Replayed only if captured under THIS layout; arguments harvested under a different
	//    root signature index different parameters and must never be replayed here.
	//
	//    Built as a plain snapshot and handed to restore_native_compute_state, which is the
	//    same function the WARP harness exercises — so the ordering and skip rules below are
	//    covered by a golden-output test rather than only by reasoning.
	NativeComputeState snapshot;
	snapshot.root_signature = reinterpret_cast<::ID3D12RootSignature *>(compute_layout.handle);

	{
		std::lock_guard<std::mutex> lock(g_mutex);
		const auto it = g_root.find(cmd_list);
		if (it != g_root.end() && it->second.layout == compute_layout.handle)
		{
			for (const auto &cbv : it->second.root_cbv_va)
				snapshot.root_cbv.emplace_back(cbv.first, cbv.second);
			for (const auto &srv : it->second.root_srv_va)
				snapshot.root_srv.emplace_back(srv.first, srv.second);
			for (const auto &uav : it->second.root_uav_va)
				snapshot.root_uav.emplace_back(uav.first, uav.second);
			for (const auto &rc : it->second.root_constants)
				snapshot.root_constants.emplace_back(rc.first, rc.second);
		}
	}

	// D3D12 has one PSO slot shared by graphics and compute, and the last thing the game bound
	// before this dispatch is its TAA compute PSO.
	const auto pso = state->pipelines.find(reshade::api::pipeline_stage::all);
	if (pso != state->pipelines.end() && pso->second.handle != 0)
		snapshot.pso = reinterpret_cast<::ID3D12PipelineState *>(pso->second.handle);

	// The root signature and tables were already re-bound through ReShade above, so they are
	// left out of the snapshot here to avoid issuing them twice.
	snapshot.root_signature = nullptr;
	restore_native_compute_state(native, snapshot);

	// 6. Nothing else. Viewports, scissors, render targets, blend factor and stencil reference
	//    are untouched by anything we do.
}

void reset_command_list_state(reshade::api::command_list *cmd_list)
{
	std::lock_guard<std::mutex> lock(g_mutex);
	g_root.erase(cmd_list);
}

// Every resource ReShade has told us about and not yet told us was destroyed.
//
// A set of LIVE resources rather than of dead ones, deliberately: D3D12 reuses addresses, so a
// freed pointer can come back as a different, valid resource. Keyed on the handle because that
// is all we may safely hold — dereferencing is exactly what we are protecting against.
std::mutex g_live_mutex;
std::unordered_set<std::uint64_t> g_live_resources;

void note_resource_created(reshade::api::resource res)
{
	if (res.handle == 0)
		return;
	std::lock_guard<std::mutex> lock(g_live_mutex);
	g_live_resources.insert(res.handle);
}

void note_resource_destroyed(reshade::api::resource res)
{
	if (res.handle == 0)
		return;
	std::lock_guard<std::mutex> lock(g_live_mutex);
	g_live_resources.erase(res.handle);
}

bool is_resource_live(std::uint64_t handle)
{
	if (handle == 0)
		return false;
	std::lock_guard<std::mutex> lock(g_live_mutex);
	return g_live_resources.find(handle) != g_live_resources.end();
}

void forget_all_resources()
{
	std::lock_guard<std::mutex> lock(g_live_mutex);
	g_live_resources.clear();
}

void forget_all_command_lists()
{
	std::lock_guard<std::mutex> lock(g_mutex);
	g_root.clear();
}

} // namespace stray_dlss::rsb
