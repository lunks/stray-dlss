# StrayDLSS — the UE4SS C++ plugin host (0.1.0)

DLSS Super Resolution (and DLSS Neural Rendering) for Stray, loaded by UE4SS, **with no
ReShade in the process**. This is plan Stage 4 (`docs/superpowers/plans/2026-09-01-dlss-sr-ue4ss-plugin.md`):
the same application and native D3D12 backend the ReShade add-on ships, hosted by a UE4SS mod.

* `start_mod()` runs ~970 ms before the game's `D3D12CreateDevice` (facts §12), so the host
  detours that export (MinHook, loading `d3d12.dll` itself), sees the game's device as it is
  created, unwraps a proxy if one sits above it (a throwaway resource's `GetDevice`), and
  installs the native backend in **drive** mode on the real device.
* The native present owner (`src/backend_native/present_owner.cpp`) patches the DXGI factory
  and swapchain vtables, so the frame boundary and a command list of ours on the presenting
  queue exist without ReShade.
* NGX runs on the native device; `ext_unhook` stays on and is inert without ReShade.
* Config: `<mod>/StrayDLSS.ini`, `[STRAYDLSS]`, the same keys as ReShade.ini. Log:
  `<game>/stray-dlss-plugin.log` (plus `stray-dlss-status.txt` in the game directory).
* No overlay, no screenshot channel, no UI. Status is the NGX indicator (the registry key
  `NGXCore\ShowDlssIndicator=0x400` in the prefix) plus the log.

Only `src/Mod.cpp` includes a UE4SS header. `Host.cpp` and `Platform.cpp` are plain Win32 and
are compiled under mingw by CI as the fast proxy for the MSVC build.

## Install

1. Build (CI: `.github/workflows/dlss-plugin.yml`, artifact `stray-dlss-plugin`).
2. Copy `StrayDLSS/` into `<game>/ue4ss/Mods/` (`dlls/main.dll`, `StrayDLSS.ini`); add
   `StrayDLSS : 1` to `ue4ss/Mods/mods.txt` **above** the `Keybinds` line.
3. `nvngx_dlss.dll` (and `nvngx_dlssnr.dll` for NR) beside the game executable, as for the add-on.
4. Launch with UE4SS loading (`dwmapi=n,b`) and `-dx12`. **DXGI must be DXVK's, never
   wine-builtin** — the game's D3D12 is vkd3d-proton, and wine-builtin DXGI (`dxgi=b`) cannot
   create a D3D12 swapchain on a vkd3d-proton device (a NULL-deref in `vkd3d_instance_get_vk_instance`,
   facts §20). Keep `dxgi=n,b` (native-first) so DXGI resolves to DXVK's `dxgi.dll`.
   * **Config A (no ReShade):** rename the game-dir ReShade `dxgi.dll` out of the way (e.g.
     `dxgi.dll.off`); native resolution then falls to the prefix's DXVK `system32\dxgi.dll`.
   * **Config B (ReShade coexists):** keep ReShade as `dxgi.dll`; the plugin unwraps its proxies.
   In both cases **do not** also run the ReShade add-on (`stray-dlss.addon64`) — two drivers of
   one TAA pass.

## Frame generation (`NgxFG`, default 0)

DLSS Frame Generation through the NGX core, **without Streamline** (facts §31). The plugin's
present owner hands the game replacement back buffers, presents twice per game frame and paces
the pair on a worker thread; `nvngx_dlssg.dll` (beside the game executable, like `nvngx_dlss.dll`)
is driven exactly as SR is. Keys, all `[STRAYDLSS]` in `StrayDLSS.ini`:

* `NgxFG=1` turns it on. `NgxFGMode=1` is the stage-1 experiment (the generated frame is the
  previous real frame under a magenta band, no NGX: proves the present path alone);
  `NgxFGMode=2` (default) generates through DLSS-G.
* `NgxFGPacing` 1 worker thread (default), 0 back to back (control), 2 game thread with
  `NgxFGWaitMs` between the presents. `NgxFGValidate=1` keeps a black or stale generated frame
  off the screen. `NgxFGReflex` 0/1/2. `NgxFGHDR`, `NgxFGCameraFar`, `NgxFGMvecScale`,
  `NgxFGOutputReal`, `NgxFGWarmupFrames` are the DLSS-G feature's knobs (`src/ngx_fg.hpp`).
* Read the `[fg]` log line and the `fg_*` status-file keys: presents issued per game present
  (~2x), refusals by reason, the pacer's estimate, whether the issued-interval histogram is
  bimodal (= not paced), and the crop gate's verdicts.
