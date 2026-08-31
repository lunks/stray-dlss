// The recorder half of the dataflow pass finder: turns ReShade events into the FrameEvents
// that core/pass_walk.hpp walks, and logs the verdict.
//
// Diagnostic ONLY, behind [STRAYDLSS] PassFinder=1 (default off). It never suppresses,
// never replaces, never touches the image — it logs its verdict and the full hop chain so
// one observation run on the target machine can be compared against what the CI-proven walk
// says the frame looks like. Do not let it drive suppression until that comparison has
// happened.
//
// Cost model: recording resolves descriptor tables per dispatch and per full-screen draw,
// which is far too expensive for every frame — so the finder records TWO CONSECUTIVE frames
// out of every kSampleInterval, walks each, and stays a no-op the rest of the time. Pairs,
// not single frames, because the history round-trip needs adjacent frames: UE4 ping-pongs
// the TAA history between two buffers with period two, so comparing frames 30 apart sees
// the same buffer on the same side and never the rebind.
#pragma once

#include "core/pass_walk.hpp"

#include "reshade_all.hpp"

#include <cstdint>

namespace stray_dlss::pass_finder {

// Read once at attach, before any event can fire.
void set_enabled(bool enabled);
bool enabled();

// Pipeline identity, fed from init_pipeline / destroy_pipeline. `is_compute` matters
// because bind_pipeline fires with pipeline_stage::all in D3D12 (CLAUDE.md §5), so the
// bound-pipeline bookkeeping needs the pipeline's own kind to know which slot it fills.
void note_pipeline(std::uint64_t pipeline_handle, std::uint64_t shader_hash, bool is_compute);
void forget_pipeline(std::uint64_t pipeline_handle);
void note_bind_pipeline(reshade::api::command_list *cmd_list, std::uint64_t pipeline_handle);

// Recording taps, called from the add-on's event handlers. Cheap no-ops outside a
// recording frame.
void note_render_targets(reshade::api::command_list *cmd_list, uint32_t count,
                         const reshade::api::resource_view *rtvs, reshade::api::resource_view dsv);
void note_draw(reshade::api::command_list *cmd_list, uint32_t vertex_or_index_count);
// Call ONLY for dispatches that will actually execute — a suppressed dispatch writes
// nothing, so recording it would enter a phantom writer into the last-writer table.
void note_dispatch(reshade::api::command_list *cmd_list, uint32_t x, uint32_t y, uint32_t z);
void note_copy(reshade::api::command_list *cmd_list, reshade::api::resource source,
               reshade::api::resource dest);

// Assigns execute-order sequence numbers and moves the list's recorded events into the
// frame. Fires per list at ExecuteCommandLists, which is what makes the order total per
// queue.
void note_execute(reshade::api::command_list *cmd_list);
void forget_command_list(reshade::api::command_list *cmd_list);

// Frame boundary: walks the recorded events, logs the verdict when it changes, and decides
// whether the next frame records. `back_buffer` is the walk's fallback anchor.
void on_present(std::uint64_t frame, reshade::api::resource back_buffer);

} // namespace stray_dlss::pass_finder
