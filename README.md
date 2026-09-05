# stray-dlss

UE4SS plugins for [Stray](https://store.steampowered.com/app/1332010/Stray/) — NVIDIA DLSS,
and the PS5 controller features the PC build dropped. Everything runs inside the game's own
process. **No ReShade.**

Stray is Unreal Engine 4.27.2 on D3D12. It ships no DLSS and no upscaler plugin to swap, so
the DLSS plugin hooks UE4's own `ITemporalUpscaler::AddPasses` seam, takes the colour, depth,
motion vectors and jitter the engine's temporal AA pass was about to consume, and hands them
to NGX instead.

Developed and used on **Linux / Proton** against an RTX 4090. Windows is untested.

## The plugins

| Plugin | What it does |
|---|---|
| **StrayDLSS** (C++) | DLSS **Super Resolution**, **Frame Generation** (present-twice, no Streamline) and **Neural Rendering** (feature 18, as a present-time stage over the back buffer). Also hosts the D3D12 backend, the motion-vector resolve, and the diagnostics everything else is read from. |
| **StrayDualSense** (C++) | Restores the PS5 features on PC — adaptive triggers, haptics on the voice coils, and controller-speaker audio — driven from the engine's own submixes rather than replayed assets. |
| **StrayFur** (Lua) | Better cat fur at runtime, on the live `GFurComponent` and its material, without touching the pak. |
| **StrayProbe** (Lua) | Writes `stray-game-state.txt` once a second — pawn, map, paused, ingame. Ground truth for "is the game in gameplay", used by the test tooling. |
| **StrayDebugMenu** (Lua) | Opens the debug UI Stray itself ships and never exposed. |

## Install

You need **UE4SS** and, for DLSS, NVIDIA's `nvngx_*.dll` runtimes. Nothing here redistributes
them.

1. Install UE4SS into the game's binary directory, so that `dwmapi.dll` and `ue4ss/` sit in:

   ```
   <SteamLibrary>/steamapps/common/Stray/Hk_project/Binaries/Win64/
   ```

2. Drop each plugin into `ue4ss/Mods/`:

   ```
   ue4ss/Mods/StrayDLSS/dlls/StrayDLSS.dll     C++ plugins: the DLL under dlls/,
   ue4ss/Mods/StrayDLSS/StrayDLSS.ini          its .ini beside it
   ue4ss/Mods/StrayFur/Scripts/main.lua        Lua mods: the script under Scripts/
   ```

3. Enable them in `ue4ss/Mods/mods.txt`, one per line:

   ```
   StrayDLSS : 1
   StrayDualSense : 1
   ```

4. For DLSS, put `nvngx_dlss.dll` (and `nvngx_dlssg.dll` for frame generation) in `Win64/`.

5. Set Steam launch options — `dwmapi` is what loads UE4SS, `-dx12` is what NGX needs:

   ```
   WINEDLLOVERRIDES="dwmapi=n,b" %command% -dx12
   ```

Configuration is `ue4ss/Mods/StrayDLSS/StrayDLSS.ini`, hot-reloaded while the game runs.
The plugin writes `stray-dlss-plugin.log` next to the executable; that log is the product's
second output and is where every verdict is read from.

### Engine settings

Stray reads `Engine.ini` from the **Proton prefix**, not the game directory, and
command-line arguments do not take effect on this title:

```
<compatdata>/1332010/pfx/drive_c/users/steamuser/AppData/Local/Hk_project/Saved/Config/WindowsNoEditor/Engine.ini
```

Two settings matter enough to call out. `r.RayTracing=False` roughly **triples** the frame
rate — Stray builds acceleration structures every frame for baked lighting that barely uses
them. And `r.ScreenPercentage` below 100 with `r.TemporalAA.Upsampling=1` is what puts the
engine into the upscaling configuration DLSS wants; 70% is the highest working value.

## Building

There is no local build. Everything is compiled by **GitHub Actions with MSVC**; releases are
CI artifacts.

```
mods/          the plugins
src/           DLSS plugin source: NGX, the D3D12 backend, the engine hooks
shaders/       our own HLSL, precompiled to DXBC at build time
tests/         everything provable without a GPU — hashing, matrix math, jitter,
               velocity decode, plus WARP and real-ReShade lanes in Windows CI
tools/         box tooling: launch, bench, screenshots, pak readers
docs/          measured engine facts and design research
```

`CLAUDE.md` is the operating manual — what is measured versus assumed, and what will
silently produce a wrong image. `docs/STRAY-RENDERING-FACTS.md` is the measured record and
`docs/BOX-USAGE.md` is how to drive the test machine.

## Credits

- **[UE4SS](https://github.com/UE4SS-RE/RE-UE4SS)** — the plugin host.
- **[NVIDIA DLSS SDK](https://github.com/NVIDIA/DLSS)** — NGX headers and programming guide.
- **[Luma-Framework](https://github.com/Filoppi/Luma-Framework)** by Filoppi — the
  architectural reference for the NGX lifecycle.
- **[OptiScaler](https://github.com/optiscaler/OptiScaler)** — the deepest well of
  Proton-specific upscaler knowledge.

## Licence

Plugin source is MIT. NVIDIA's runtimes are theirs, redistributed under the NVIDIA RTX SDKs
licence and never committed here.

Unofficial fan project. Not affiliated with or endorsed by BlueTwelve Studio, Annapurna
Interactive, or NVIDIA.

## Neural Rendering at model resolution

`NgxNRModelScale=0.5` runs DLSS Neural Rendering on an exact area-average of the frame at half size and lays only the model's difference back on the full-resolution frame (a matched residual, cube-scaled), with the guides at a 1.0 ratio. Benched on the target (vkd3d-proton, RTX 4090) it does NOT reduce frame time: NR costs ~2 ms a frame at any model size, a fixed per-evaluate term, so `1.0` (full resolution) is the better default there; see CLAUDE.md for the measurement and what it rules out. `NgxNRModelGuided` picks how the edit comes up: 0 bilinear, 1 joint bilateral, 2 local affine (default). `NgxNRModelTransfer=0` is the untouched frame. The reasoning and measurements are in CLAUDE.md, "NR at MODEL RESOLUTION".
