#include "frame_state.hpp"

#include "log.hpp"

#include <descriptor_tracking.hpp>
#include <state_tracking.hpp>

#include <mutex>
#include <vector>
#include <unordered_map>

namespace stray_dlss {
namespace {

// Root descriptors bypass the descriptor heap entirely, so they have to be remembered as they
// go by. Keyed by command list, cleared when the list is reset.
struct RootDescriptors
{
	std::vector<BoundTexture> srvs;
	std::vector<BoundTexture> uavs;
	reshade::api::buffer_range view_cb{};
	bool view_cb_valid = false;
	uint32_t view_cb_register = 0;
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

void describe(reshade::api::device *device, reshade::api::resource_view view,
              uint32_t reg, std::vector<BoundTexture> &out)
{
	if (view.handle == 0)
		return;

	const reshade::api::resource res = device->get_resource_from_view(view);
	if (res.handle == 0)
		return;

	const reshade::api::resource_view_desc vd = device->get_resource_view_desc(view);
	const reshade::api::resource_desc rd = device->get_resource_desc(res);

	BoundTexture t;
	t.slot = reg;
	t.resource = res.handle;
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
	out.push_back(t);
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

TexFormat to_tex_format(reshade::api::format f)
{
	using reshade::api::format;
	switch (f)
	{
	case format::r32_float_x8_uint:
	case format::r32_g8_typeless:        return TexFormat::r32_float_x8x24_typeless;
	case format::x32_float_g8_uint:      return TexFormat::x32_typeless_g8x24_uint;
	case format::r16g16b16a16_unorm:     return TexFormat::r16g16b16a16_unorm;
	case format::r16g16b16a16_float:     return TexFormat::r16g16b16a16_float;
	case format::r16g16_float:           return TexFormat::r16g16_float;
	case format::r32g32b32a32_float:     return TexFormat::r32g32b32a32_float;
	default:                             return TexFormat::unknown;
	}
}

const char *format_name(reshade::api::format f)
{
	using reshade::api::format;
	switch (f)
	{
	case format::r32_float_x8_uint:    return "R32_FLOAT_X8X24_TYPELESS";
	case format::r32_g8_typeless:      return "R32G8X24_TYPELESS";
	case format::x32_float_g8_uint:    return "X32_TYPELESS_G8X24_UINT";
	case format::r16g16b16a16_unorm:   return "R16G16B16A16_UNORM";
	case format::r16g16b16a16_float:   return "R16G16B16A16_FLOAT";
	case format::r16g16_float:         return "R16G16_FLOAT";
	case format::r32g32b32a32_float:   return "R32G32B32A32_FLOAT";
	case format::r11g11b10_float:      return "R11G11B10_FLOAT";
	case format::r8g8b8a8_unorm:       return "R8G8B8A8_UNORM";
	case format::r10g10b10a2_unorm:    return "R10G10B10A2_UNORM";
	case format::r32_float:            return "R32_FLOAT";
	case format::r16_float:            return "R16_FLOAT";
	case format::r8_unorm:             return "R8_UNORM";
	case format::unknown:              return "unknown";
	default:                           return "other";
	}
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

	for (uint32_t i = 0; i < update.count; ++i)
	{
		const uint32_t reg = update.binding + i;

		switch (update.type)
		{
		case reshade::api::descriptor_type::constant_buffer:
		{
			const auto *ranges = static_cast<const reshade::api::buffer_range *>(update.descriptors);
			// The View uniform buffer is the large one. b0 ($Globals) is small and its layout
			// is unknowable because fxc compacts it, so it is deliberately ignored.
			// (CLAUDE.md §2.6)
			rd.view_cb = ranges[i];
			rd.view_cb_valid = ranges[i].buffer.handle != 0;
			rd.view_cb_register = layout_param;
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

					// The shader register, which is what every fact in CLAUDE.md §2.3 is
					// expressed in — not the descriptor's index in the table.
					const uint32_t reg = range.dx_register_index + i;

					switch (range.type)
					{
					case reshade::api::descriptor_type::shader_resource_view:
						describe(device, desc->get_resource_view(heap, offset), reg, out.srvs);
						break;
					case reshade::api::descriptor_type::unordered_access_view:
						describe(device, desc->get_resource_view(heap, offset), reg, out.uavs);
						break;
					case reshade::api::descriptor_type::constant_buffer:
					{
						const reshade::api::buffer_range br = desc->get_buffer_range(heap, offset);
						if (br.buffer.handle != 0 && !out.view_cb_valid)
						{
							out.view_cb = br;
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
			if (rd.view_cb_valid)
			{
				out.view_cb = rd.view_cb;
				out.view_cb_valid = true;
				out.view_cb_register = rd.view_cb_register;
			}
		}
	}

	return !out.srvs.empty() || !out.uavs.empty();
}

void reset_command_list_state(reshade::api::command_list *cmd_list)
{
	std::lock_guard<std::mutex> lock(g_mutex);
	g_root.erase(cmd_list);
}

void forget_all_command_lists()
{
	std::lock_guard<std::mutex> lock(g_mutex);
	g_root.clear();
}

} // namespace stray_dlss
