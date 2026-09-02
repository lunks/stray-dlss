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
//
// THE SENTINEL MUST SURVIVE THIS IMAGE. The ReShade host unloads and reloads this DLL across
// the game's startup device recreate (docs/STRAY-RENDERING-FACTS.md §14), and a resource
// registered in the first life can die in the second — when the runtime calls Release on an
// object whose code has been unmapped (§15's open hazard). So the sentinel is a hand-laid-out
// COM object whose vtable lives in a page VirtualAlloc'ed OUTSIDE the image, and detach()
// rewrites that vtable to three inert x64 stubs in the same page: after it, every Release the
// runtime ever makes on a first-life sentinel lands in code that is still mapped and does
// nothing. The sentinel objects themselves come from the process heap for the same reason.
#pragma once

#include "intercept/types.hpp"

#include <cstdint>

struct ID3D12Resource;

namespace stray_dlss::native::registry {

// Allocates this life's sentinel vtable page (idempotent). Until armed, note_created records
// the resource but attaches no sentinel (counted under Stats::unarmed).
bool arm();
// On host detach: makes every sentinel this life attached inert — its vtable now points at
// stubs outside the image — and forgets everything. A later arm() starts a new life; a
// resource still carrying a previous life's sentinel gets a fresh one on re-registration
// (SetPrivateDataInterface replaces, and the runtime's Release of the old one hits a stub).
void detach();
bool armed();
// The live vtable page, for the harness to prove it lies outside this module.
const void *sentinel_vtable_for_test();

// Idempotent. Safe only with a resource that is CERTAINLY alive: inside its own creation, or
// while it is the argument of a call the game is making with it.
void note_created(::ID3D12Resource *res);

bool is_live(icept::ResourceId res);
// Whether this life ever registered it — tells a resource that DIED (sentinel fired) apart
// from one the registry never saw (created before attach, or through an unhooked entry).
// Only ever grows within a life; the address-reuse caveat (CLAUDE.md §5) does not bite
// because is_live() is what answers "alive now".
bool ever_seen(icept::ResourceId res);
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
	std::uint64_t unarmed = 0;           // registered while not armed (no sentinel): liveness UNKNOWN
	std::uint64_t orphaned = 0;          // sentinels detach() made inert (this life's, cumulative)
};
Stats stats();
void clear_for_test();

} // namespace stray_dlss::native::registry
