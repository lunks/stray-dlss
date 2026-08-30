# stray-dlss

A **ReShade add-on** that injects **NVIDIA DLSS Super Resolution** into
[Stray](https://store.steampowered.com/app/1332010/Stray/), which shipped without it.

Stray runs on **Unreal Engine 4.27.2** over **D3D12**. The add-on finds the engine's own temporal
AA compute pass (`FTAAStandaloneCS`), takes the colour, depth, motion-vector and jitter inputs it
was about to consume, hands them to DLSS instead, and writes the result into the same UAV the
engine reads back as next frame's temporal history. The engine never knows the difference.

> **Status: early development. Nothing here is playable yet.**
> The plumbing is being built and verified stage by stage. See [Roadmap](#roadmap).

---

## Why this is not just "turn on DLSS"

Stray has no DLSS support to enable and no upscaler plugin to swap. Everything DLSS needs has to
be recovered from a running D3D12 process:

- **The right pass, out of 728 shaders.** UE 4.27 compiles `FTAAStandaloneCS` in several
  permutations. We identify the exact one by hashing its DXBC.
- **Jitter, from a constant buffer.** DLSS needs the sub-pixel jitter the engine used. It lives in
  UE4's `View` uniform buffer, which we read at known row offsets.
- **Dense motion vectors, which the game does not have.** UE 4.27 writes velocity *only for moving
  objects*. Every static pixel — the whole world, whenever the camera moves — carries nothing. We
  run our own compute pass that reconstructs camera motion from reversed-Z depth and the
  `ClipToPrevClip` matrix, and composites the engine's sparse velocity on top.
- **Camera cuts.** DLSS has to be told to reset its history on a cut, or it smears across the
  transition. UE4 leaks this: on a cut, `View.TemporalAAJitter.zw` becomes equal to `.xy`.

The engine-side facts this is built on were measured against the running game, not assumed. They
are written down in [`docs/STRAY-RENDERING-FACTS.md`](docs/STRAY-RENDERING-FACTS.md).

---

## Target platform

This is developed for **Linux / Proton**, which is where it will actually be used:

| | |
|---|---|
| GPU | NVIDIA RTX (developed against a 4090) |
| Driver | 610.43.02, open kernel modules |
| OS | SteamOS / Linux, Proton `GE-Proton-dxvk301-ds5-clean-nowl` |
| D3D12 | vkd3d-proton |
| Display | gamescope, DRM backend, HDR enabled |
| ReShade | 6.8.0.2155 or newer, **with add-on support** |

Windows is not currently a target. It may work; it is not tested.

> **Open question, tracked as the project's main technical risk:** whether the **D3D12** NGX entry
> points (`NVSDK_NGX_D3D12_*`) are usable under vkd3d-proton, where an `ID3D12Resource` is really
> a `VkImage`. On Linux the well-trodden DLSS path is the Vulkan NGX API. If the D3D12 path turns
> out to be blocked, the fallback is a Vulkan-interop or FSR-based evaluation behind the same
> internal interface.

---

## Installation

Not yet. Once there are releases:

1. Install **ReShade 6.8+ with add-on support** into
   `<SteamLibrary>/steamapps/common/Stray/Hk_project/Binaries/Win64/`.
2. Drop `stray-dlss.addon64` and `nvngx_dlss.dll` in the same folder.
3. On Linux, install `msvcrt40` and `vcrun2022` into the prefix with `protontricks`.
4. Launch Stray, press **Home** for the ReShade overlay, and open the **Stray DLSS** tab.

Note that Stray reads its configuration from the **Proton prefix**, not the game directory:

```
<compatdata>/1332010/pfx/drive_c/users/steamuser/AppData/Local/Hk_project/Saved/Config/WindowsNoEditor/Engine.ini
```

`Engine.ini` settings take effect on this title; **command-line arguments do not**. Anything the
add-on needs the engine to do (such as lowering internal render resolution for true
super-resolution) goes through that file.

---

## Building

There is no local build. Everything is compiled by **GitHub Actions with MSVC**, and releases are
produced as CI artifacts.

```
.github/workflows/     CI: build, unit tests, package, release
CMakeLists.txt         CMake + MSVC, x64, /MT, warnings as errors
src/                   add-on source
shaders/               our own HLSL (motion-vector resolve, debug views)
tests/                 unit tests for everything provable without a GPU
docs/                  measured engine facts and design notes
```

The parts that can be tested without a GPU are tested in CI: DXBC hashing, the UE4 velocity
decode, jitter conversion, the `ClipToPrevClip` reconstruction math, and DLSS quality-mode
selection. The part that cannot — the actual NGX evaluation — is kept as thin as possible.

---

## Roadmap

- **v0.1 — DLAA.** Render resolution equals output resolution, so there is no screen-percentage
  problem to solve. This exists to answer one question: are colour, depth, motion vectors and
  jitter correct? If DLAA looks right, everything downstream is built on solid ground.
- **v0.2 — DLSS Super Resolution.** Force UE4's internal render resolution down via `Engine.ini`,
  handle the render subrect and the second TAA permutation that screen percentage may switch the
  engine onto, and expose Quality / Balanced / Performance / Ultra Performance.
- **Later, maybe.** DLSS preset selection, sharpening, a Vulkan-interop path, an FSR fallback for
  non-NVIDIA hardware.

---

## Credits and prior art

- **[ReShade](https://github.com/crosire/reshade)** by crosire — the add-on API this is built on.
- **[Luma-Framework](https://github.com/Filoppi/Luma-Framework)** by Filoppi — a ReShade-add-on
  modding framework that injects DLSS into DX11 games that never shipped it. Its NGX lifecycle and
  its `SuperResolutionImpl` abstraction are the architectural reference for this project.
- **[OptiScaler](https://github.com/optiscaler/OptiScaler)** — upscaler replacement across APIs,
  and the deepest well of Proton-specific upscaler knowledge.
- **[NVIDIA DLSS SDK](https://github.com/NVIDIA/DLSS)** — the NGX headers and programming guide.

## Licence

The add-on source is MIT. `nvngx_dlss.dll` is NVIDIA's, redistributed under the NVIDIA RTX SDKs
licence and never committed to this repository — CI fetches it from the official DLSS release.

This is an unofficial fan project. It is not affiliated with or endorsed by BlueTwelve Studio,
Annapurna Interactive, or NVIDIA.
