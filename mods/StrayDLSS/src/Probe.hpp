// StrayDLSS — the attach-timing probe. Plain Win32 + D3D12; no UE4SS types.
//
// Decides assessment §6.2 (docs/RESEARCH-UE4SS-MIGRATION.md): is start_mod() before or after
// the game's D3D12CreateDevice, and what does an export hook installed from a UE4SS C++ mod
// actually see when ReShade (as dxgi.dll) is also in the process?
#pragma once

namespace stray_dlss::probe {

// From the mod constructor (start_mod). Logs the clock, which graphics DLLs are already mapped,
// and hooks D3D12CreateDevice if d3d12.dll is present; otherwise Tick() keeps looking for it.
void Start();

// From on_update (~200 Hz on UE4SS's own thread). Cheap: a GetModuleHandle poll until the
// hook is installed, then the one-shot throwaway-device probe 30 s in.
void Tick();

// From the mod destructor. Disables the hook so a FreeLibrary'd mod leaves no dangling detour.
void Stop();

} // namespace stray_dlss::probe
