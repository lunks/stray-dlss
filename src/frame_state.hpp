// Recovering, at dispatch time, exactly which resource the game bound to which register.
//
// Resources are tracked BY DESCRIPTOR REGISTER, never by pointer identity alone: the same
// texture turns up at different registers meaning different things, and telling those cases
// apart is the whole problem. (CLAUDE.md §2.9)
//
// Two binding paths both have to be covered, because UE4's D3D12 RHI uses both and which one
// carries a given resource is not knowable in advance:
//   * root descriptors  — SetComputeRootConstantBufferView etc, seen via push_descriptors
//   * descriptor tables — CreateCBV/CopyDescriptors/SetComputeRootDescriptorTable, resolved
//                         through ReShade's own descriptor_tracking utility
#pragma once

#include "core/taa_signature.hpp"

#include "reshade_all.hpp"

#include <vector>

namespace stray_dlss {

TexFormat to_tex_format(reshade::api::format f);
const char *format_name(reshade::api::format f);

// What a single compute dispatch had bound.
struct DispatchBindings
{
	std::vector<BoundTexture> srvs;
	std::vector<BoundTexture> uavs;
	reshade::api::buffer_range view_cb{};
	bool view_cb_valid = false;
	uint32_t view_cb_register = 0;
};

// Resolves the bindings for the compute pipeline currently bound on `cmd_list`, using
// ReShade's state_tracking (which tables are bound) plus descriptor_tracking (what is in
// them), and merges in any root descriptors we captured ourselves.
bool resolve_compute_bindings(reshade::api::command_list *cmd_list, DispatchBindings &out);

// Root-descriptor capture, fed from the push_descriptors event.
void note_push_descriptors(
	reshade::api::command_list *cmd_list,
	reshade::api::shader_stage stages,
	reshade::api::pipeline_layout layout,
	uint32_t layout_param,
	const reshade::api::descriptor_table_update &update);

// Dumps what the trackers hold for this command list. Callable on demand so a FAILED resolve
// can explain itself for a specific shader, rather than only dumping whichever dispatch
// happened to be first.
void dump_tracker_state_for(reshade::api::command_list *cmd_list, const char *why);

void reset_command_list_state(reshade::api::command_list *cmd_list);
void forget_all_command_lists();

} // namespace stray_dlss
