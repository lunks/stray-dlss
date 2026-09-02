// The native backend: our own D3D12 vtable hooks as an icept::Backend. In `observe` mode it
// runs BESIDE the ReShade backend and diffs every observed dispatch against ReShade's answer
// (src/app/diff_observer.hpp); it never suppresses and never restores. `drive` is Stage 3
// and is refused in this version.
#pragma once

#include "intercept/backend.hpp"

#include <cstdint>

struct ID3D12Device;

namespace stray_dlss::native {

enum class Mode
{
	off,     // nothing installed
	observe, // hooks installed, dispatches diffed against the driver's answer, nothing changed
	drive,   // NOT IMPLEMENTED in this version: refused loudly, falls back to observe
};

Mode mode_from_string(const char *s);
const char *mode_name(Mode m);

// Installs the hooks on `real_device` (the ORIGINAL vkd3d/WARP device, never a proxy) and
// on the command-list vtable reached through a throwaway list of that device. Idempotent.
// Returns false if nothing could be installed; the report says what was and was not.
bool install(::ID3D12Device *real_device, Mode mode);
Mode mode();
::ID3D12Device *game_device();
const char *attach_report();

// While held on this thread, every hook passes straight through: our own recording onto
// the game's list (resolve, NGX evaluate, codec passes, the restore) must not enter the
// shadow, or it would replace the game's state with ours (assessment §8.3).
struct OwnCodeScope
{
	OwnCodeScope();
	~OwnCodeScope();
	OwnCodeScope(const OwnCodeScope &) = delete;
	OwnCodeScope &operator=(const OwnCodeScope &) = delete;
};
bool in_own_code();

class NativeBackend final : public icept::Backend
{
public:
	const char *name() const override { return "native"; }
	bool resolve_compute_bindings(const icept::CommandContext &ctx, icept::DispatchBindings &out) override;
	bool resolve_graphics_srvs(const icept::CommandContext &ctx, std::vector<BoundTexture> &out) override;
	void describe_view(icept::DescriptorId view, std::uint32_t reg, std::vector<BoundTexture> &out) override;
	bool describe_resource(icept::ResourceId res, icept::ResourceInfo &out) override;
	bool resource_from_view(icept::DescriptorId view, icept::ResourceId &out) override;
	bool read_buffer(const icept::BufferRange &range, std::uint64_t bytes, void *out) override;
	bool is_resource_live(icept::ResourceId res) override;
	void restore_game_compute_state(const icept::CommandContext &ctx) override;
	void restore_viewports_and_scissors(const icept::CommandContext &ctx) override;
	void present_barrier(const icept::PresentContext &ctx, icept::ResourceId res,
	                     std::uint32_t before, std::uint32_t after) override;
	void dump_tracker_state(const icept::CommandContext &ctx, const char *why) override;
};

NativeBackend &backend();

// Telemetry for the periodic line and the status file.
struct Stats
{
	std::uint64_t unknown_lookups = 0;   // the assessment §6.2 counter
	std::uint64_t unknown_copies = 0;
	std::uint64_t resolves = 0;          // native resolve_compute_bindings calls
	std::uint64_t resolves_no_layout = 0; // list had no root signature we deserialized
	std::uint64_t root_signatures = 0;
	std::uint64_t pipelines_hashed = 0;
	std::uint64_t resources_live = 0;
	std::uint64_t slots = 0;
	std::uint64_t heaps = 0;
	unsigned patches = 0;
};
Stats stats();
void log_stats(const char *when);

// The DXBC hash recorded for a pipeline created on the hooked device (0 if unknown).
std::uint64_t pipeline_hash(void *pso);

} // namespace stray_dlss::native
