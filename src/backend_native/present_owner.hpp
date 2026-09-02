// The native present owner (plan Stage 4, `present_owner`): our own command list on the
// presenting queue, executed right before every Present, and the frame boundary the seam's
// on_present needs — without ReShade.
//
// HOW. A throwaway DXGI factory reaches the factory class's static vtable; its CreateSwapChain*
// slots are patched so the game's swapchain is seen the moment it exists, and THAT object's
// vtable (Present / Present1 / ResizeBuffers / ResizeBuffers1) is patched in turn. Both are
// one class each in DXVK's dxgi.dll and in ReShade's proxy dxgi.dll, so one object of the
// class is enough to reach every object of it (§11's static-vtable measurement, for DXGI:
// SOFT until the box run says so).
//
// WHICH QUEUE. The swapchain's "device" argument is the queue the game presents on — the real
// one when nothing proxies it, ReShade's proxy when ReShade is loaded, and a proxy queue
// expects proxy command lists. So the device hook records every REAL queue the game creates
// (ReShade's proxy device forwards CreateCommandQueue to the real device, whose vtable is
// ours), and core::pick_present_queue chooses by identity first, by type second.
//
// ORDERING. The game has submitted every command list of the frame to its queue before it calls
// Present; our list executes on that same queue inside the Present hook, so it runs after all
// of them and before the frame is presented — the same place ReShade's immediate list sat.
#pragma once

#include <cstdint>

struct IUnknown;
struct ID3D12Device;
struct ID3D12CommandQueue;

namespace stray_dlss::native::present {

// Installs the factory-side hooks and prepares the per-swapchain ring (3 allocators + lists +
// one fence) on `device` (the REAL device). Idempotent. False if nothing could be installed.
bool install(::ID3D12Device *device);
// Restores nothing itself (vtable_patch's restore_all_patches covers the slots); waits for the
// GPU to finish with the ring and releases it.
void uninstall();

// From the device hook: every ID3D12CommandQueue created, with its D3D12_COMMAND_LIST_TYPE.
void note_queue(::ID3D12CommandQueue *queue, int type);
// From the host's CreateDXGIFactory* export hooks: patch this factory's CreateSwapChain* slots,
// so the game's OWN factory is reached even when DXGI does not share one vtable per class
// (measured on DXVK: it does not). `factory` is an IUnknown that is really an IDXGIFactory.
void note_factory(::IUnknown *factory);
// From NativeBackend::present_barrier: something was recorded on this frame's present list,
// so it must be executed.
void note_present_list_used();

struct Stats
{
	std::uint64_t presents = 0;      // Present/Present1 hooks that delivered on_present
	std::uint64_t executed = 0;      // presents whose list carried commands and was executed
	std::uint64_t swapchains = 0;    // swapchains seen (created or re-hooked)
	std::uint64_t resizes = 0;
	std::uint64_t queues = 0;        // queues recorded
	std::uint64_t skipped = 0;       // presents let through untouched (test-only, no sink, no entry)
};
Stats stats();
const char *report();
// Diagnostics: log every swapchain-hook entry (which slot, which object, which thread). Off by
// default; the WARP harness turns it on around teardown to see what DXGI calls on its way out.
void set_hook_trace(bool on);
bool hook_trace();

} // namespace stray_dlss::native::present
