// A fake reshade::api::command_list, for testing the half of restore_game_compute_state that
// talks to ReShade rather than to D3D12.
//
// Why this exists: the native half is covered by a golden-output test against WARP, but the
// ReShade half — the count==0 resync, and binding tables one at a time while skipping
// parameters that are not tables — was reasoned about in comments and never executed by any
// test. That is also the leading suspect for the on-screen corruption, so it is exactly the
// half that needed a tripwire.
//
// reshade::api::command_list is a pure abstract interface, so a fake is straightforward. The
// two things restore_game_compute_state asks of it are both easy to provide:
//
//   * get_private_data<state_tracking>() — state_tracking is a plain struct, so a test can
//     hand-build any scenario, including ones that are awkward to reach in a real frame.
//   * get_native() — we return the harness's REAL WARP command list. That is the trick that
//     makes this more than a mock: the native calls the restore emits actually execute, so
//     the D3D12 debug layer judges them. Binding a table to a root-CBV parameter would raise
//     #708 here, caught by the same drain the other tests use.
//
// Everything else is a no-op. Only bind_descriptor_tables2 is recorded — and recording it is
// enough to see bind_descriptor_tables too, since the latter is a defaulted method that
// forwards to it (reshade_api_device.hpp:806 -> :1191).
#pragma once

#include <reshade_api.hpp>

#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace stray_dlss {
namespace test {

// The inert overrides below are transcribed from the interface, so they name its types
// unqualified. Scoped to this test namespace, which is why a using-directive is tolerable
// in a header here.
using namespace reshade::api;

// One recorded call to bind_descriptor_tables / bind_descriptor_tables2.
struct RecordedBind
{
	reshade::api::shader_stage stages;
	reshade::api::pipeline_layout layout;
	std::uint32_t first;
	std::uint32_t count;
	std::vector<reshade::api::descriptor_table> tables;
};

class FakeCommandList : public reshade::api::command_list
{
public:
	std::uint64_t native = 0;
	std::vector<RecordedBind> binds;

	// Associates a private-data pointer the way ReShade would, keyed like a GUID.
	template <typename T>
	void put_private_data(T *value)
	{
		set_private_data(reinterpret_cast<const std::uint8_t *>(&__uuidof(T)),
			reinterpret_cast<std::uint64_t>(value));
	}

	// --- api_object ---
	std::uint64_t get_native() const final { return native; }
	void get_private_data(const std::uint8_t guid[16], std::uint64_t *data) const final
	{
		std::uint64_t key;
		std::memcpy(&key, guid, sizeof(key));
		const auto it = private_data_.find(key);
		*data = it == private_data_.end() ? 0 : it->second;
	}
	void set_private_data(const std::uint8_t guid[16], const std::uint64_t data) final
	{
		std::uint64_t key;
		std::memcpy(&key, guid, sizeof(key));
		private_data_[key] = data;
	}

	// --- device_object ---
	reshade::api::device *get_device() final { return nullptr; }

	// --- the one call we care about ---
	void bind_descriptor_tables2(reshade::api::shader_stage stages,
		reshade::api::pipeline_layout layout, std::uint32_t first, std::uint32_t count,
		const reshade::api::descriptor_table *tables, std::uint32_t, const std::uint32_t *) final
	{
		binds.push_back({ stages, layout, first, count,
			std::vector<reshade::api::descriptor_table>(tables, tables + count) });
	}

private:
	std::unordered_map<std::uint64_t, std::uint64_t> private_data_;

public:
	// --- everything else: inert ---
	void barrier(uint32_t, const resource *, const resource_usage *, const resource_usage *) final {}
	void end_render_pass() final {}
	void bind_render_targets_and_depth_stencil(uint32_t, const resource_view *, resource_view) final {}
	void bind_pipeline(pipeline_stage, pipeline) final {}
	void bind_pipeline_states(uint32_t, const dynamic_state *, const uint32_t *) final {}
	void bind_viewports(uint32_t, uint32_t, const viewport *) final {}
	void bind_scissor_rects(uint32_t, uint32_t, const rect *) final {}
	void push_constants(shader_stage, pipeline_layout, uint32_t, uint32_t, uint32_t, const void *) final {}
	void push_descriptors(shader_stage, pipeline_layout, uint32_t, const descriptor_table_update &) final {}
	void bind_index_buffer(resource, uint64_t, uint32_t) final {}
	void bind_vertex_buffers(uint32_t, uint32_t, const resource *, const uint64_t *, const uint32_t *) final {}
	void bind_stream_output_buffers(uint32_t, uint32_t, const resource *, const uint64_t *, const uint64_t *, const resource *, const uint64_t *) final {}
	void draw(uint32_t, uint32_t, uint32_t, uint32_t) final {}
	void draw_indexed(uint32_t, uint32_t, uint32_t, int32_t, uint32_t) final {}
	void dispatch(uint32_t, uint32_t, uint32_t) final {}
	void draw_or_dispatch_indirect(indirect_command, resource, uint64_t, uint32_t, uint32_t) final {}
	void copy_resource(resource, resource) final {}
	void copy_buffer_region(resource, uint64_t, resource, uint64_t, uint64_t) final {}
	void copy_buffer_to_texture(resource, uint64_t, uint32_t, uint32_t, resource, uint32_t, const subresource_box *) final {}
	void copy_texture_region(resource, uint32_t, const subresource_box *, resource, uint32_t, const subresource_box *, filter_mode) final {}
	void copy_texture_to_buffer(resource, uint32_t, const subresource_box *, resource, uint64_t, uint32_t, uint32_t) final {}
	void resolve_texture_region(resource, uint32_t, const subresource_box *, resource, uint32_t, uint32_t, uint32_t, uint32_t, format) final {}
	void clear_depth_stencil_view(resource_view, const float *, const uint8_t *, uint32_t, const rect *) final {}
	void clear_render_target_view(resource_view, const float[4], uint32_t, const rect *) final {}
	void clear_unordered_access_view_uint(resource_view, const uint32_t[4], uint32_t, const rect *) final {}
	void clear_unordered_access_view_float(resource_view, const float[4], uint32_t, const rect *) final {}
	void generate_mipmaps(resource_view) final {}
	void begin_query(query_heap, query_type, uint32_t) final {}
	void end_query(query_heap, query_type, uint32_t) final {}
	void copy_query_heap_results(query_heap, query_type, uint32_t, uint32_t, resource, uint64_t, uint32_t) final {}
	void begin_debug_event(const char *, const float[4]) final {}
	void end_debug_event() final {}
	void insert_debug_marker(const char *, const float[4]) final {}
	void dispatch_mesh(uint32_t, uint32_t, uint32_t) final {}
	void dispatch_rays(resource, uint64_t, uint64_t, resource, uint64_t, uint64_t, uint64_t, resource, uint64_t, uint64_t, uint64_t, resource, uint64_t, uint64_t, uint64_t, uint32_t, uint32_t, uint32_t) final {}
	void copy_acceleration_structure(resource_view, resource_view, acceleration_structure_copy_mode) final {}
	void build_acceleration_structure(acceleration_structure_type, acceleration_structure_build_flags, uint32_t, const acceleration_structure_build_input *, resource, uint64_t, resource_view, resource_view, acceleration_structure_build_mode) final {}
	void query_acceleration_structures(uint32_t, const resource_view *, query_heap, query_type, uint32_t) final {}
	void update_buffer_region(const void *, resource, uint64_t, uint64_t) final {}
	void update_texture_region(const subresource_data &, resource, uint32_t, const subresource_box *) final {}
	void begin_render_pass2(uint32_t, const render_pass_render_target_desc *, const render_pass_depth_stencil_desc *, render_pass_flags) final {}
};

} // namespace test
} // namespace stray_dlss
