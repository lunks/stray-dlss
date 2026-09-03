// Who calls IDXGIAdapter3::QueryVideoMemoryInfo, and how often.
//
// WHY THIS EXISTS. The visible blink (facts §32.14-§32.16) is one frame per second in which the
// RHI thread blocks ~19 ms inside the NVIDIA kernel driver taking the RM lock, with the kernel
// stack ending in `_ctrl_convert_v2_NV2080_CTRL_CMD_FB_GET_INFO` — a framebuffer/VRAM info
// control. NVAPI and NVML were ruled out by their own log (silent after init), which leaves the
// DXGI path. This patches the adapter's QueryVideoMemoryInfo slot and records, per call, the
// CALLER'S RETURN ADDRESS and the module it lands in. That names the caller instead of inferring
// it: the game's exe means UE4, d3d12core/vkd3d means the translation layer, nvapi64 means
// DXVK-NVAPI.
//
// DIAGNOSTIC ONLY. It forwards every call unmodified and changes no behaviour. Default OFF
// ([STRAYDLSS] VramQueryWatch), so it ships inert.
#pragma once

struct ID3D12Device;

namespace stray_dlss::native::vramwatch {

// Resolves the adapter the device was created from and patches its QueryVideoMemoryInfo slot.
// Safe to call more than once; only the first install does anything. No-op when the config key
// is off. Returns true when the slot was patched.
bool install(::ID3D12Device *device);

// One line for the log: install state, call count, and the caller modules seen.
const char *report();

} // namespace stray_dlss::native::vramwatch
