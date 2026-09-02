// Resource liveness and facts WITHOUT ever dereferencing a resource after its creation
// (assessment §1.1 decision 2; the direct fix for CLAUDE.md §5's second descriptor hazard).
//
// A resource is registered when we first see it — its creation hook, or a view created over
// it (swapchain buffers never pass through the device's creation methods). The registration
// snapshots GetDesc, the heap type and the GPU address, and attaches an IUnknown sentinel via
// SetPrivateDataInterface; the runtime releases it when the resource dies (MEASURED on
// vkd3d-proton 3.1.0 and WARP: fired=1, docs/STRAY-RENDERING-FACTS.md §11), and that final
// Release erases the resource here and every descriptor slot that referenced it. Per-slot
// invalidation closes the address-reuse hole a global live set cannot (assessment §1.4).
#pragma once

#include "intercept/types.hpp"

#include <cstdint>

struct ID3D12Resource;

namespace stray_dlss::native::registry {

// Idempotent. Safe only with a resource that is CERTAINLY alive: inside its own creation, or
// while it is the argument of a call the game is making with it.
void note_created(::ID3D12Resource *res);

bool is_live(icept::ResourceId res);
// From the creation-time snapshot; false unless live.
bool describe(icept::ResourceId res, icept::ResourceInfo &out);
// A root CBV/SRV/UAV's D3D12_GPU_VIRTUAL_ADDRESS -> the buffer it lies in. Size is the
// buffer's remaining bytes from that offset.
bool buffer_for_va(std::uint64_t gpu_va, icept::BufferRange &out);

// The sentinel's final Release. Erases the resource and notifies the listener (the
// descriptor shadow) so every slot referencing it is forgotten.
void on_destroyed(icept::ResourceId res);
void set_destroy_listener(void (*listener)(icept::ResourceId));

struct Stats
{
	std::uint64_t registered = 0;
	std::uint64_t destroyed = 0;
	std::uint64_t live = 0;
	std::uint64_t sentinel_failures = 0; // SetPrivateDataInterface refused; liveness for those is UNKNOWN
};
Stats stats();
void clear_for_test();

} // namespace stray_dlss::native::registry
