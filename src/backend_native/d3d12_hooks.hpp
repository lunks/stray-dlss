// The device and command-list slot hooks, and the deserialized root signatures / hashed
// pipelines they collect. Installed by native::install; consumed by the native backend.
#pragma once

#include "core/root_signature_walk.hpp"

#include <cstdint>

struct ID3D12Device;
struct ID3D12RootSignature;

namespace stray_dlss::native::hooks {

// Patches the device slots and, through a throwaway command list of `device`, the
// command-list slots. Returns how many slots were patched (0 = failure).
// `query_device2`: whether to QueryInterface for ID3D12Device2 before patching its slot. On
// vkd3d's real device that is safe; on ReShade's PROXY, inside ReShade's own init_device
// callback, a QueryInterface re-enters ReShade and DEADLOCKS (measured 2026-09-01) — and the
// proxy implements every device version in one vtable anyway, so the slot is patched blind.
unsigned install_device_hooks(::ID3D12Device *device, bool query_device2 = true);
unsigned install_list_hooks(::ID3D12Device *device);

// The deserialized layout of a root signature created through the hooks. False if unknown.
bool layout_for(::ID3D12RootSignature *rs, core::RootLayout &out);
std::uint64_t root_signature_count();
std::uint64_t pipeline_count();
std::uint64_t pipeline_hash(void *pso);
std::uint32_t descriptor_increment(); // CBV_SRV_UAV, from the hooked device

// The compute-shader bytecode inside a pipeline-state stream, or nullptr. Exposed for the
// WARP test; `length` receives the byte count.
const void *find_cs_in_stream(const void *stream, std::size_t size, std::size_t &length,
                              const char **stop_reason);

// What the slot-47 (ID3D12Device2::CreatePipelineState) hook last forwarded to the runtime:
// the exact desc/stream pointer and the CachedPSO subobject it OBSERVED there. The WARP test
// asserts these equal what the caller passed - proof the hook neither copies the desc nor
// drops the cached blob, so it cannot be what defeats vkd3d's pipeline cache (facts §32.12).
struct ForwardProbe
{
	const void *desc = nullptr;
	const void *stream = nullptr;
	std::size_t stream_size = 0;
	const void *cached_blob = nullptr;
	std::size_t cached_size = 0;
	bool cs_found = false;
	std::uint64_t stream_creates = 0;
	std::uint64_t graphics_creates = 0;
};
ForwardProbe last_create_forward();

} // namespace stray_dlss::native::hooks
