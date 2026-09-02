// The interception seam: what a consumer may ASK the framework (Backend) and what the
// framework TELLS a consumer (Sink). One driver at a time; no ReShade, UE4SS or ImGui type.
//
// Everything here is answerable by both the ReShade add-on and the native vtable-hook layer.
// The one ReShade-only site, reshade_begin_effects, is deliberately absent: it has a documented
// inertness trap (CLAUDE.md §5, never fires with an empty preset) and stays a ReShade-backend
// mode that the native backend refuses loudly.
#pragma once

#include "intercept/types.hpp"

namespace stray_dlss::icept {

class Backend
{
public:
	virtual ~Backend() = default;
	virtual const char *name() const = 0; // "reshade" | "native" | "null"

	// The bindings of the compute pipeline currently bound on the list: descriptor tables
	// walked into shader registers, root descriptors merged in (root wins where they overlap).
	virtual bool resolve_compute_bindings(const CommandContext &ctx, DispatchBindings &out) = 0;
	// The graphics root signature's SRVs only — the pass finder's full-screen-draw anchor.
	virtual bool resolve_graphics_srvs(const CommandContext &ctx, std::vector<BoundTexture> &out) = 0;

	// Describes one bound view into `out` — liveness-checked FIRST, like every view this
	// project touches (CLAUDE.md §5, "Two descriptor hazards"). Appends nothing for a dead,
	// unknown or null view.
	virtual void describe_view(DescriptorId view, std::uint32_t reg, std::vector<BoundTexture> &out) = 0;
	// False unless the resource is KNOWN LIVE.
	virtual bool describe_resource(ResourceId res, ResourceInfo &out) = 0;
	// False if the view is unknown or its resource is dead.
	virtual bool resource_from_view(DescriptorId view, ResourceId &out) = 0;
	// A CPU read of an upload-heap buffer (the View CB, CLAUDE.md §2.6). `bytes` from
	// `range.offset`; false if the buffer is not mappable, too small, or not live.
	virtual bool read_buffer(const BufferRange &range, std::uint64_t bytes, void *out) = 0;
	virtual bool is_resource_live(ResourceId res) = 0;

	// Re-establishes the compute state the game had before our pass ran on `ctx.native`:
	// heaps, root signature, tables, root arguments and the PSO. (src/d3d12_restore.hpp)
	virtual void restore_game_compute_state(const CommandContext &ctx) = 0;

	// A resource barrier recorded onto the present-time list of `ctx` in a way that the
	// backend GUARANTEES gets flushed (ReShade's immediate list only flushes what its own API
	// recorded, CLAUDE.md §5). `before`/`after` are D3D12_RESOURCE_STATES bits.
	virtual void present_barrier(const PresentContext &ctx, ResourceId res,
	                             std::uint32_t before, std::uint32_t after) = 0;

	// Dumps whatever the backend tracks for the list, for a failed resolve to explain itself.
	virtual void dump_tracker_state(const CommandContext &ctx, const char *why) = 0;
};

struct Sink
{
	virtual ~Sink() = default;
	virtual void on_device(::ID3D12Device *native, bool created) = 0;
	virtual void on_pipeline(std::uint64_t pso, const void *dxbc, std::size_t len, ShaderKind kind,
	                         bool created) = 0;
	virtual void on_command_list(const CommandContext &ctx, bool created) = 0;
	virtual void on_command_list_reset(const CommandContext &ctx) = 0;
	virtual void on_bind_pipeline(const CommandContext &ctx, std::uint64_t pso) = 0;
	// true = suppress the game's dispatch. The only skip-capable event on our path.
	virtual bool on_dispatch(const CommandContext &ctx, std::uint32_t x, std::uint32_t y, std::uint32_t z) = 0;
	virtual void on_render_targets(const CommandContext &ctx, std::uint32_t count,
	                               const DescriptorId *rtvs, DescriptorId dsv) = 0;
	virtual void on_draw(const CommandContext &ctx, std::uint32_t vertex_or_index_count) = 0;
	virtual void on_copy(const CommandContext &ctx, ResourceId src, ResourceId dst) = 0;
	virtual void on_execute(const CommandContext &ctx) = 0;
	virtual void on_swapchain(const ResourceId *back_buffers, std::uint32_t count, bool created) = 0;
	virtual void on_present(const PresentContext &ctx) = 0;
};

// Exactly one DRIVER at a time. Never null: with nothing installed a null backend answers
// every question with "no" and logs once, so a missing set_backend is a loud failure rather
// than a crash.
Backend *backend();
void set_backend(Backend *backend);

} // namespace stray_dlss::icept
