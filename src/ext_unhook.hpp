// Undo ReShade's vkd3d extension-vtable patch before NGX uses the extension interface.
//
// ReShade 6.8 patches vkd3d-proton's SINGLE STATIC ID3D12DeviceExt vtable — slots 7/8
// (GetCudaTextureObject/GetCudaSurfaceObject) and, since ID3D12DeviceExt2 support, slots 14/15
// (the merged-texture-sampler / independent-descriptor entries) — inside its proxy's
// QueryInterface, so descriptor handles run through convert_to_original_cpu_descriptor_handle.
// That conversion is only correct for handles ReShade minted. Our NGX runs on the NATIVE device
// (§1), so its handles are real vkd3d handles: the conversion mangles them and vkd3d rejects
// the result — measured live as NvAPI_D3D12_GetCudaMergedTextureSamplerObject failing with
// status -5 on every evaluate while NGX still reports success, leaving the output resource
// never written (the frozen-scene bug).
//
// The repair: capture the pristine slot pointers at device init, BEFORE anything routes a query
// through ReShade's proxy (measured: the patch installs at frame ~120 when the game's stack
// queries the interface), then write them back whenever a later check finds ReShade's pointers
// installed. Restoring is safe in this process: the only caller of the CUDA entry points is
// nvngx_dlss.dll itself — Stray never calls them, and DXVK-NVAPI merely queries the interface.
//
// [STRAYDLSS] ExtUnhook=0 disables the repair (the check still logs).
#pragma once

struct ID3D12Device;

namespace stray_dlss::ext_unhook {

void set_enabled(bool enabled);

// Save the pristine vtable pointers. Call at device init, before the game can have queried the
// extension through ReShade's proxy. Returns false (and saves nothing) when the interface is
// absent or a slot is already ReShade-owned — too late to learn the originals.
bool capture(ID3D12Device *native);

// Write back any saved slot that now points into ReShade. Call immediately before NGX feature
// creation and before every evaluate — the game can re-install the patch at any time with a
// single QueryInterface. Returns the number of slots repaired (0 = nothing to do), or -1 when
// capture never succeeded or the repair is disabled.
int repair();

} // namespace stray_dlss::ext_unhook
