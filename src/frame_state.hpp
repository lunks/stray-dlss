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

#include <utility>
#include <vector>

// Global scope on purpose: writing `struct ID3D12DescriptorHeap *` inside the namespace below
// declares a NEW type stray_dlss::ID3D12DescriptorHeap rather than referring to the real one.
struct ID3D12DescriptorHeap;

namespace stray_dlss {

TexFormat to_tex_format(reshade::api::format f);
const char *format_name(reshade::api::format f);

// What a single compute dispatch had bound.
struct DispatchBindings
{
	std::vector<BoundTexture> srvs;
	std::vector<BoundTexture> uavs;
	// EVERY constant buffer bound, not just the first. Which register carries UE4's View
	// uniform buffer varies between passes (b3, b4 and b5 all seen), so the only reliable
	// way to find it is to try each and keep the one that parses plausibly.
	std::vector<std::pair<uint32_t, reshade::api::buffer_range>> constant_buffers;
	reshade::api::buffer_range view_cb{};
	bool view_cb_valid = false;
	uint32_t view_cb_register = 0;

	// The descriptor heaps that owned THIS dispatch's descriptors — i.e. exactly what the game
	// had bound at this moment. Restoring these is precise, where picking the first heap found
	// across every tracked table is a guess that can restore the wrong one.
	::ID3D12DescriptorHeap *heaps[2] = {};
	unsigned int heap_count = 0;
};

// Resolves the bindings for the compute pipeline currently bound on `cmd_list`, using
// ReShade's state_tracking (which tables are bound) plus descriptor_tracking (what is in
// them), and merges in any root descriptors we captured ourselves.
bool resolve_compute_bindings(reshade::api::command_list *cmd_list, DispatchBindings &out);

// Our own pipeline-layout tracking. ReShade's descriptor_tracking utility cannot be used for
// this: its register_pipeline_layout only deep-copies the range array for
// pipeline_layout_param_type::descriptor_table, and keeps the caller's POINTER for
// descriptor_table_with_flags — which is the variant UE4 uses. That pointer dangles, and
// get_pipeline_layout_param then returns ranges with garbage binding/count values, so the
// descriptor walk silently visits nothing.
void note_pipeline_layout(
	reshade::api::device *device,
	uint32_t count,
	const reshade::api::pipeline_layout_param *params,
	reshade::api::pipeline_layout layout);

void forget_pipeline_layout(reshade::api::pipeline_layout layout);

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

// Recovers the descriptor heaps the game currently has bound, by resolving the heaps that own
// the descriptor tables ReShade tracked for this command list.
//
// D3D12 has no way to read back the bound heaps, and this matters more than it sounds: after
// our own pass swaps in a private heap, re-applying the game's descriptor TABLES without first
// restoring the heap that owns them is an invalid binding, and UE4 dies with
// LowLevelFatalError rather than anything diagnosable.
//
// `out` must have room for 2 (D3D12 allows one CBV/SRV/UAV heap and one sampler heap).
void collect_bound_heaps(reshade::api::command_list *cmd_list,
                         ::ID3D12DescriptorHeap **out,
                         unsigned int *count);

void reset_command_list_state(reshade::api::command_list *cmd_list);
void forget_all_command_lists();

} // namespace stray_dlss
