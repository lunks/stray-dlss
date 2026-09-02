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
unsigned install_device_hooks(::ID3D12Device *device);
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

} // namespace stray_dlss::native::hooks
