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

// Captures a root constant push, which ReShade's state_tracking does not track at all.
void note_push_constants(
	reshade::api::command_list *cmd_list,
	reshade::api::shader_stage stages,
	reshade::api::pipeline_layout layout,
	uint32_t layout_param,
	uint32_t first,
	uint32_t count,
	const void *values);

// Re-establishes the compute state the game had before our pass ran.
//
// This exists because ReShade's state_block CANNOT do it. state_tracking.cpp registers only
// bind_render_targets, bind_pipeline, bind_pipeline_states, bind_viewports, bind_scissor_rects
// and bind_descriptor_tables — there is no push_descriptors or push_constants handler — so
// apply() can never replay the game's ROOT descriptors, and UE4's D3D12 RHI binds its uniform
// buffers as root CBVs. Restoring with apply() alone leaves the game's own TAA dispatch running
// with undefined root arguments every frame we inject, which is the visible corruption.
//
// It is also replayed NATIVELY rather than through ReShade. We changed the root signature on
// the native list, which ReShade's proxy never saw; its bind_descriptor_tables caches the
// current root signature and would skip re-setting it, then bind the game's tables against OUR
// layout. Setting it natively re-syncs reality with both ReShade's cache and UE4's own.
void restore_game_compute_state(reshade::api::command_list *cmd_list);

void reset_command_list_state(reshade::api::command_list *cmd_list);
void forget_all_command_lists();

// --- resource liveness ---
//
// ReShade never calls destroy_resource_view on D3D12, so its view->resource map keeps entries
// for views whose descriptor slot UE4 has since recycled for a different resource. Asking it
// for the resource behind such a view hands back a POINTER TO A DESTROYED RESOURCE.
//
// That is not a theoretical hazard. Building an SRV from one faults inside the driver:
// GetDesc reads freed-but-still-mapped memory and returns entirely plausible values
// (2560x1440, R32G8X24_TYPELESS), and then vkd3d dereferences the destroyed VkImage —
// "err:vulkan:vkCreateImageView Exception 0xc0000005" in the Proton log, taking the game with
// it. UE4 rotates these buffers constantly, so it happens within seconds of gameplay.
//
// So track lifetime ourselves from ReShade's init_resource/destroy_resource events and refuse
// to touch anything not known to be alive.
void note_resource_created(reshade::api::resource res);
void note_resource_destroyed(reshade::api::resource res);
bool is_resource_live(std::uint64_t handle);
void forget_all_resources();

} // namespace stray_dlss
