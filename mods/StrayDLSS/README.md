# StrayDLSS — a UE4SS C++ plugin (0.0.1: the attach-timing probe)

The first step of `docs/superpowers/plans/2026-09-01-dlss-sr-ue4ss-plugin.md`: a UE4SS C++ mod
that **renders nothing and changes nothing on screen**. It exists to answer, from inside the
process, the one timing question the whole native-hook design depends on (assessment §6.2):

* does `start_mod()` run **before** the game's `D3D12CreateDevice`?
* what does an export hook installed from a C++ mod actually **see** when ReShade is loaded as
  `dxgi.dll` in the same process — the real vkd3d device, or ReShade's proxy?
* from a throwaway device (the late-attach path), is the **real** device reachable through
  `ID3D12Resource::GetDevice` without any vkd3d interop interface?

Everything it learns goes to `<game>/stray-dlss-plugin.log`, timestamped in both local time
and UTC so the lines can be laid against `ReShade.log` (local) and `ue4ss/UE4SS.log` (UTC).

Only `src/Mod.cpp` includes a UE4SS header. `Probe.cpp`, `Log.cpp` and `Platform.cpp` are
plain Win32 and are compiled and linked under mingw by CI as a proxy for the MSVC build.

## Install

1. Build (CI: `.github/workflows/dlss-plugin.yml`, artifact `stray-dlss-plugin`).
2. `<game>/ue4ss/Mods/StrayDLSS/dlls/main.dll`; add `StrayDLSS : 1` to `ue4ss/Mods/mods.txt`
   **above** the `Keybinds` line.
3. UE4SS itself must already be loading (`dwmapi.dll` proxy; the box already has it). With
   ReShade as `dxgi.dll` the launch options must **merge** both overrides:
   `WINEDLLOVERRIDES="dwmapi,dxgi=n,b"`.

## What it does NOT do

No hidden window and no dummy swapchain (the plan's Task 2 lists one): the observer build
that follows needs no `Present` hook while ReShade owns the frame, and a Wine toplevel under
gamescope has a documented focus-steal freeze (CLAUDE.md §6). It is deferred, not forgotten.
