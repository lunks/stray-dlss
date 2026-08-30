# stray-dlss — agent operating manual

A ReShade **add-on** (not an effect/shader preset) that injects **NVIDIA DLSS Super Resolution**
into **Stray** (Unreal Engine 4.27.2, **D3D12**) by intercepting the engine's own temporal AA
compute dispatch and replacing it with an NGX evaluation.

Read this file completely before touching anything. It is the contract for how this project is
built, what is known versus assumed, and what will silently produce a wrong image.

---

## 0. Prime directives

1. **You cannot test this yourself.** No game, no Windows box, no GPU. Every build happens in
   GitHub Actions with MSVC. The only feedback loop is the human running the artifact on their
   Linux/Stray machine and pasting back a log. Design accordingly: a round-trip is expensive, so
   each one must carry the maximum diagnostic payload.
2. **Prefer a loud failure to a quiet wrong image.** A crash gets a stack trace. A subtly wrong
   motion-vector sign gets "it looks smeary" three days later and costs a week. Every convention
   (jitter sign, MV direction, MV scale, depth polarity, colour space) must be *asserted and
   logged*, never assumed.
3. **Never claim something works because it compiled.** Compiling proves the types line up. The
   only evidence that DLSS is engaged is a log line from the user's machine or a screenshot.
   Follow `superpowers:verification-before-completion`: evidence before assertions, always.
4. **Everything provable in CI must be tested in CI.** DXBC hashing, matrix math, jitter
   conversion, velocity decode, resolution/quality-mode selection — all pure functions, all
   unit-tested on the runner. The un-testable part (the actual NGX call) must be as thin as
   possible so the untested surface is small.
5. **HARD / SOFT / UNCONFIRMED.** When you write down a technical claim in this repo, label its
   provenance. "Read it in the header" is HARD. "A forum post said so" is SOFT. Anything else is
   UNCONFIRMED and must be verified before code depends on it.

---

## 1. Target environment (the only one that matters)

The human runs Stray on Linux. This is **not** a Windows-first project that happens to work on
Proton; Proton is the target.

| | |
|---|---|
| GPU | NVIDIA RTX 4090 |
| Driver | 610.43.02, open kernel modules |
| Host | Linux 6.17.13, SteamOS guest |
| Proton | `GE-Proton-dxvk301-ds5-clean-nowl` |
| D3D12 layer | **vkd3d-proton** (D3D12 → Vulkan) |
| Compositor | gamescope, DRM backend, `--hdr-enabled --hdr-itm-enabled` |
| ReShade | 6.8.0.2155, add-on support enabled |
| Game | Stray, Steam AppID **1332010** |

**The single biggest open risk in this project** is whether `NVSDK_NGX_D3D12_*` works at all under
vkd3d-proton. On Linux the NGX runtime is reached through Proton's `nvngx.dll` / `_nvngx.dll`
shim and the Linux driver's `libnvidia-ngx.so`; the well-trodden path is the **Vulkan** NGX API,
because DXVK and vkd3d-proton are Vulkan underneath. Whether the D3D12 NGX entry points are
bridged, and how `ID3D12Resource` maps to the `VkImage` NGX actually needs, must be established
before writing the evaluation path — not after. Relevant knobs: `PROTON_ENABLE_NVAPI=1`,
`PROTON_HIDE_NVIDIA_GPU=0`, `VKD3D_CONFIG`, `dxvk-nvapi`, and vkd3d-proton's interop interfaces
(`ID3D12DeviceExt`, `ID3D12DXVKInteropDevice::GetVulkanResourceInfo`).

Data point in our favour: Luma-Framework ships ReShade add-ons that do DLSS and its release notes
say "Proton should be fully compatible out of the box" (with `msvcrt40` + `vcrun2022` via
protontricks). But Luma is **DX11/DXVK**, not DX12/vkd3d-proton. Do not generalise from it
without evidence.

---

## 2. Everything we know about Stray

All of this was **measured on hardware, read out of the game's own DXBC, or read from the game's
own files** — not inferred. The verbatim source document is `docs/STRAY-RENDERING-FACTS.md`;
this section carries the load-bearing facts inline so they are always in context.

### 2.1 Engine and process

| Fact | Value |
|---|---|
| Engine | **Unreal Engine 4.27.2** |
| Executable | `Stray-Win64-Shipping.exe`, PE32+ (x86-64) |
| Graphics API | **D3D12** |
| UE project name | `Hk_project` |
| Version string on title screen | `v1.54368 (Revision 26632)` |

Observed swapchains, from `IDXGISwapChain::ResizeBuffers`:

* `BufferCount = 3, Width = 3840, Height = 2160, NewFormat = 24`
* `BufferCount = 3, Width = 2560, Height = 1440, NewFormat = 24`

`NewFormat = 24` is `DXGI_FORMAT_R10G10B10A2_UNORM`.

### 2.2 Filesystem layout

Game directory on this install:

```
<SteamLibrary>/steamapps/common/Stray/Hk_project/Binaries/Win64/
```

Config and saves live in the **Proton prefix**, not the game directory:

```
<compatdata>/1332010/pfx/drive_c/users/steamuser/AppData/Local/Hk_project/Saved/
    Config/WindowsNoEditor/Engine.ini
    SaveGames/
    Crashes/UE4CC-Windows-<GUID>_0000/     CrashContext.runtime-xml, UE4Minidump.dmp
    Logs/                                   (observed empty on this install)
```

Two configuration facts:

* **`Engine.ini` settings take effect. Command-line arguments do not.** Anything we need to force
  (screen percentage, TAA upsampling) must go through `Engine.ini`.
* There are two `compatdata` trees for this title on this machine. The live one is
  `/home/deck/.local/share/Steam/steamapps/compatdata/1332010`. The one on the secondary library
  (`GamesLinux`) is a ~6.1 MB skeleton that is **not read**.

### 2.3 The TAA pass — our interception point

Stray uses UE 4.27's standalone temporal AA compute shader, `FTAAStandaloneCS`.

**Primary pass, identified by fnv1a64 over its DXBC:**

```
0x1708ec956099e259
```

Measured binding signature — compute, shader model 5.0, all resources 1920×1080 at the resolution
it was measured at:

| Register | Role | Format |
|---|---|---|
| `t0` | depth | `r32_g8_typeless` |
| `t2` | velocity | `r16g16b16a16_unorm` |
| `t5`, `t6` | colour | `r16g16b16a16_float` |
| `u0` | `OutComputeTex` — the TAA output | `r16g16b16a16_float` |
| `u1` | `OutComputeTexDownsampled` (optional, declared by the shader) | — |

**A second TAA candidate exists in the same title:**

```
0x52101a15e1a0c5cc     t0 depth, t3 velocity, t7 colour, t8 r16g16_float
```

UE 4.27 compiles `FTAAStandaloneCS` in more than one permutation — `ETAAPassConfig::Main` and
`ETAAPassConfig::MainUpsampling` produce different DXBC and therefore different hashes. **The
existence of a second permutation is directly relevant to the SR phase**: if one of these is the
upsampling permutation, forcing screen percentage may switch Stray onto it, changing the hash we
must match and the binding layout we must read.

**A measured false positive**, recorded so it is not re-discovered:
`0x901e041a7cadc9db` scores confidence 150 on a class-quorum test with colour=1, depth=2,
velocity=0. Do not let a heuristic re-select it.

**Shader census on this install:** 728 distinct PS/CS shaders in gameplay, `not_dxbc=0`,
`dxil=0` — every pixel/compute shader observed is **DXBC**, none DXIL. During the main menu the
same census reads ~150; it rises to ~728 on entering gameplay. (PS and CS only; says nothing
about DXR.)

### 2.4 Depth

* The resource bound at `t0` is **`r32_g8_typeless`** — typeless, planar depth-stencil.
* The SRV the game creates over it is **`r32_float_x8_uint`**.
* UE 4.27 renders with **reversed-Z** → set `NVSDK_NGX_DLSS_Feature_Flags_DepthInverted`.

A depth statistic gathered during the main menu and loading screens reads
`below 0.25: 3456000, above 0.75: 0, mean 0.00000` — **menu and load frames carry no usable depth
range; gameplay frames do.** This is a usable gate: do not engage DLSS on a frame whose depth is
degenerate, and use this as a cheap "are we actually in gameplay" test.

### 2.5 Velocity — and the sparse-velocity problem

The velocity buffer at `t2` is `r16g16b16a16_unorm`, and it is **sparse**: UE 4.27 writes it only
for pixels covered by **moving objects**. Static geometry carries no velocity and its motion must
be **reconstructed from depth and the camera matrices**.

This is the highest-value piece of real work in the project. DLSS wants a dense, full-screen
motion vector field. Feeding it UE4's sparse buffer directly means every static pixel reads as
"did not move", which is wrong the instant the camera moves — and it produces smearing and
ghosting rather than a crash. **We must write a compute pass that produces dense MVs**: take the
sparse velocity where it is valid, and elsewhere reconstruct camera motion from reversed-Z depth
and `ClipToPrevClip`.

**The encoding, from UE 4.27 `Engine/Shaders/Private/Common.ush:1537-1570`:**

```
EncodedV.xy = V.xy * (0.499f * 0.5f) + 32767.0f / 65535.0f          // encode
V.xy        = EncodedV.xy * InvDiv - 32767.0f / 65535.0f * InvDiv   // decode
InvDiv      = 1.0f / (0.499f * 0.5f)
```

**Both constants were located in Stray's own DXBC**, not merely taken from engine source:

| Constant | Value | Bit pattern | Notes |
|---|---|---|---|
| `InvDiv` (decode scale) | `4.00801611f` | `0x408041AB` | bytes `AB 41 80 40` |
| Folded MAD bias | `2.00397754f` | `0x4000412B` | appears **negated** in a `mad` as `0xC000412B`, bytes `2B 41 00 C0` |
| Bias term | `32767/65535 = 0.49999237f` | `0x3EFFFF00` | the bias is **not** 0.5 |

`0.49999237f * 4.00801611f = 2.00397754f`. The decode's second term is folded into a MAD immediate
by the compiler rather than appearing as a separate subtract.

The game's own decode helper is named `DecodeVelocityFromTexture`.

### 2.6 The View constant buffer

Stray's TAA shader carries the stock UE 4.27.2 `View` uniform buffer at register **`b1`**.

Observed sizes across permutations in the same session — total size varies, **row offsets do not**:

* `size = 126 float4s (2016 bytes)`
* `size = 131 float4s (2096 bytes)`
* `size = 145 float4s (2320 bytes)`

Row offsets (float4 rows = byte offset / 16). The stock UE 4.27.2 layout was established twice
independently — read out of `VIEW_UNIFORM_BUFFER_MEMBER_TABLE` (`SceneView.h:582-774`) and
recomputed by a layout script over the same declaration list:

| Field | Row | Byte offset |
|---|---|---|
| `ViewToClip` | 28 | 448 |
| `ViewToClipNoAA` | 32 | 512 |
| `ClipToPrevClip` | 122 | 1952 |
| `TemporalAAJitter` | 126 | 2016 |
| `ViewRectMin` | 129 | 2064 |
| `ViewSizeAndInvSize` | 130 | 2080 |
| `LightProbeSizeRatioAndInvSizeRatio` | 131 | 2096 |
| `TemporalAAParams` | 152 | 2432 |

The six rows a jitter recovery needs — `proj=28 noaa=32 clip=122 jitter=126 size=130 params=152` —
were located in Stray's running View buffer and reported at the strongest tier (`tier=full`).

`ClipToPrevClip` at row 122 was confirmed **in Stray's own TAA shader by pure DXBC instruction
analysis**, with no reflection names involved.

Three traps when reading this buffer:

* `LightProbeSizeRatioAndInvSizeRatio` at row 131 is `(1,1,1,1)` — a **decoy** for any naive search
  that expects an identity-looking row.
* The shader declares `dcl_constantbuffer cb1[131]`. That 131 is the **highest row the shader
  indexes**, not the buffer's size — `ViewSizeAndInvSize` ends at byte 2096.
* These offsets are fixed for a given engine build but are **not invariant across a licensee edit**
  to the member table. They were checked rather than trusted; keep checking them at runtime.

**Jitter convention**, from engine source and consistent with the above:

```
InJitterOffsetX = TemporalJitterPixels.X = TemporalAAParams.z = TemporalAAJitter.x * W *  0.5f
InJitterOffsetY = TemporalJitterPixels.Y = TemporalAAParams.w = TemporalAAJitter.y * H * -0.5f
```

**Note the Y term is negative.** This is exactly the kind of sign that silently ruins an image.

### 2.7 Camera cuts

UE 4.27 assigns `PrevViewMatrices = ViewMatrices` on any frame that is a camera cut. The
observable consequence in the View buffer is that **`View.TemporalAAJitter.zw` becomes equal to
`.xy`**. This was used as a live cut detector and confirmed working against the running game
(reported as `detector=LIVE`). It maps directly onto NGX's `InReset`.

Observed cut counts: **3** across splash and main menu, **5** by the time gameplay is running —
entering gameplay from the menu produces cuts.

### 2.8 TAA history

The resource written at `u0` (`OutComputeTex`) is extracted by UE 4.27 as the **next frame's
`HistoryBuffer[0]`**. **Overwriting `u0` therefore feeds whatever we write into the next frame's
temporal history** — which is precisely the hook that lets us replace TAA from outside the engine.

The same resource can also appear bound as this frame's **scene-colour input** (at the colour SRV
register) rather than as the history slot; the two cases are distinguishable **only by which
register it turns up on at a given dispatch**. Track resources by register, never by identity
alone.

### 2.9 Stability observations on this install

Environment facts, independent of any add-on — useful when triaging a crash report so we do not
chase our own tail:

* `gamescope-wl` segfaulted three times in one afternoon (11:40, 11:42, 14:03), and once the day
  prior. The nvidia driver was unloaded and reloaded at 11:43.
* One GPU `Xid 109 (CTX SWITCH TIMEOUT)` was recorded against `Stray-Win64-Shi`, channel
  `0x00000012`.
* UE4 crash dumps exist **from sessions with no third-party add-on installed at all**, with
  `ErrorMessage: Unhandled Exception: 0xe06d7363` (a C++ exception) and one
  `EXCEPTION_ACCESS_VIOLATION reading address 0x0000000000000010`. **Stray crashes on its own.**
  Do not assume a crash is ours.
* Killing the game leaves a `reaper` process (`SteamLaunch AppId=1332010`) behind. While it
  exists, Steam silently ignores further `steam://rungameid/1332010` launches.

### 2.10 Driving the game unattended (host facts)

* The physical DualSense is held by **Steam** via `/dev/hidraw0`. Nothing holds its evdev nodes.
* Steam Input re-emits it as **"Microsoft X-Box 360 pad 0"**, and that node is what the game reads.
  Its `eventN` number is **not stable** — Steam tears it down with the game.
* Writing `input_event` structs directly to `/dev/input/eventN` reaches `input_inject_event()` in
  the kernel and is seen by every reader of that node. No `uinput`, `ydotool` or `evemu` needed.
  Neither the pad nor the keyboard node is `EVIOCGRAB`'d.
* ReShade's screenshot bind is `KeyScreenshot=44` (`VK_SNAPSHOT`) = Linux `KEY_SYSRQ=99`.
  Injecting it on the real keyboard node makes ReShade write a 4K PNG into the game directory.
  **This is our screenshot channel for visual verification.**
* gamescope's `SIGUSR2` screenshot produced no file. `ffmpeg`'s `kmsgrab` cannot read its
  framebuffer, which is `XB30` (`XBGR2101010`, 10-bit HDR).

---

## 3. How the add-on works

The shape, end to end:

```
                    ┌─────────────────── ReShade add-on events ───────────────────┐
 game frame  ──▶ init_pipeline ──▶ hash DXBC ──▶ is this 0x1708ec956099e259?
                                                          │ yes
                    bind_pipeline ─────────────────────────┘  remember "TAA is bound"
                    push_descriptors ──▶ capture t0 depth, t2 velocity, t5/t6 colour,
                                         u0 output, b1 View CB
                    dispatch ──────────▶ INTERCEPT
                                          │
                                          ├─ read View CB rows 122/126/129/130/152
                                          ├─ detect camera cut (jitter.zw == jitter.xy) → InReset
                                          ├─ run our MV-resolve compute pass:
                                          │     dense MV = sparse velocity where valid,
                                          │     else camera motion from depth + ClipToPrevClip
                                          ├─ NGX EvaluateFeature(colour, depth, denseMV, jitter) → u0
                                          └─ return true  (skip the engine's TAA dispatch)
                                          │
                    u0 becomes next frame's HistoryBuffer[0] ──▶ engine continues
```

Four stages, each independently testable in isolation as far as CI allows:

1. **Identify** — hash every compute shader's DXBC at `init_pipeline`, match the known TAA hash,
   fall back to a binding-signature heuristic that must *never* select `0x901e041a7cadc9db`.
2. **Capture** — record the bound SRVs/UAV/CB at `push_descriptors` / `bind_descriptor_tables`,
   keyed **by register**, and read the View CB rows.
3. **Resolve** — our own compute pass turning sparse velocity + depth + `ClipToPrevClip` into the
   dense MV field DLSS needs, in DLSS's expected units and sign.
4. **Evaluate** — NGX `CreateFeature` / `EvaluateFeature` into `u0`, then skip the engine dispatch.

### Staging

* **v0.1 — DLAA.** Render resolution == output resolution. No screen-percentage forcing, so the
  whole "make UE4 render smaller" problem is out of scope. This isolates one question: *are
  colour, depth, motion vectors and jitter correct?* If DLAA looks right, the plumbing is right.
* **v0.2 — DLSS SR.** Force UE4's internal render resolution down via `Engine.ini`
  (`r.ScreenPercentage` / `r.TemporalAA.Upsampling` / `r.SecondaryScreenPercentage.GameViewport`),
  handle the resulting subrect semantics and the possible switch to the `MainUpsampling` TAA
  permutation (hash `0x52101a15e1a0c5cc`?), and expose Quality/Balanced/Performance/UltraPerf.

Do not start v0.2 before v0.1 is confirmed correct **on the user's machine**.

---

## 4. Reference material

### Luma-Framework — the primary architectural reference

`Filoppi/Luma-Framework` is a ReShade-add-on modding framework that injects DLSS into games that
never shipped it (Prey 2017, Dishonored 2, Deus Ex MD, Metaphor, …). It is the closest existing
thing to what we are building and its patterns should be followed unless we have a reason not to.

**Study, in order:**

| Path in Luma | Why |
|---|---|
| `Source/Core/includes/super_resolution.h` | The `SR::SuperResolutionImpl` interface: `SettingsData`, `InstanceData`, `DrawData`. Clean separation of "what the upscaler needs" from "which upscaler". Copy this shape. |
| `Source/Core/dlss/DLSS.cpp` | The full NGX lifecycle: init-with-ProjectID, capability query, quality-mode selection, feature create/release, evaluate. |
| `Source/Core/fsr/FSR.cpp` | The same interface implemented for FSR — proof the abstraction holds, and our fallback if D3D12 NGX is blocked on Proton. |
| `Source/Core/core.hpp` | The add-on event wiring, resource tracking and overlay. Large (~17k lines); read the parts you need. |
| `Source/Games/_Template/main.cpp` | Minimal per-game integration. |
| `Source/Games/Dishonored 2/` + `Shaders/Dishonored 2/Luma_PreDLSS_CS.hlsl` | A game with a pre-DLSS compute pass — the closest analogue to our MV-resolve pass. |
| `.github/workflows/build_and_release.yml` | MSVC CI, per-addon packaging, NGX opt-in detection, GitHub release. |

**Concrete things Luma establishes** (all read directly from its source, HARD):

* **No NVIDIA app whitelist is needed.** Luma calls `NVSDK_NGX_D3D11_Init_with_ProjectID` with a
  self-generated GUID and `NVSDK_NGX_ENGINE_TYPE_CUSTOM`. We do the D3D12 equivalent with our own
  GUID.
* **Feature flags it sets**: `MVLowRes` always (unless MVs are at output resolution),
  `DepthInverted` when the game uses reversed-Z (Stray does), `MVJittered` if MVs carry jitter,
  `AutoExposure`, `IsHDR` when colour is linear HDR.
* **Quality mode is derived, not chosen.** It loops every `NVSDK_NGX_PerfQuality_Value`, calls
  `NGX_DLSS_GET_OPTIMAL_SETTINGS`, and picks the mode whose optimal render resolution best matches
  the resolution actually in use — rather than picking a mode and forcing a resolution.
* **Feature creation can be rejected for a given `InPerfQualityValue`**; Luma retries with
  `Balanced` + `Render_Preset_Default`. Do the same.
* **Release the old feature and parameters before creating new ones** on any settings change, and
  release all NGX handles *before* `Shutdown1`.
* **Jitter phase count**: `8 * (output_h / render_h)^2`, Halton base 2/3 returning `[-0.5, 0.5]`.
* **Mip LOD bias**: `log2(render_h / max(render_h, output_h)) - 1`.
* **MV sign convention as Luma states it**: "MVs need to have positive values when moving towards
  the top left of the screen." Treat this as SOFT until confirmed against the DLSS Programming
  Guide — it is a load-bearing sign.

**Where we necessarily diverge from Luma:**

| | Luma | stray-dlss |
|---|---|---|
| API | D3D11 (`ID3D11Device`, immediate context only) | **D3D12** (`ID3D12Device`, `ID3D12GraphicsCommandList`, explicit barriers, descriptor heaps) |
| NGX entry points | `NVSDK_NGX_D3D11_*` | `NVSDK_NGX_D3D12_*` |
| Resource state | implicit | **explicit `D3D12_RESOURCE_STATES` transitions around evaluate** |
| Build | MSBuild `.sln` / `.vcxproj`, developed on Windows | **CMake + GitHub Actions only**, never built locally |
| Platform | Windows-first, Proton works | **Proton-only target** |

Other prior art worth reading, in decreasing relevance: `optiscaler/OptiScaler` (upscaler
replacement across APIs, including D3D12, and the most likely source of Proton-specific
knowledge), `clshortfuse/renodx`, NVIDIA's own UE4 DLSS plugin (`FDLSSUpscaler`, `NGXRHI`) for the
canonical UE4 conventions, and `crosire/reshade` `include/` + `examples/` for the add-on API
itself.

Reference checkouts are kept **outside** this repo (scratchpad), never vendored wholesale.

---

## 5. Domain notes

### ReShade add-on API

We target **ReShade 6.8.x** (the user runs 6.8.0.2155). The add-on API changed meaningfully
between 5.x and 6.x — **do not paste 5.x example code**. Ground truth is `crosire/reshade`
`include/`: `reshade.hpp`, `reshade_api_device.hpp`, `reshade_api_pipeline.hpp`,
`reshade_api_resource.hpp`, `reshade_events.hpp`, `reshade_overlay.hpp`.

Rules:

* Build a DLL named `*.addon64`. ReShade loads it and calls our `register_addon`.
* Get to raw D3D12 through `device::get_native()` / `command_list::get_native()`; check
  `device::get_api() == device_api::d3d12` and bail loudly otherwise.
* Returning `true` from an event callback **skips** the underlying call. That is how we suppress
  the engine's TAA dispatch.
* imgui is **version-pinned** to what ReShade 6.8 ships. A mismatched `imgui.h` compiles fine and
  then corrupts memory at runtime. Pin it exactly.
* Track the game's resources **by descriptor register**, not by resource pointer — §2.8 shows the
  same texture appears at different registers meaning different things.
* Constant buffers the game binds may live in an overwritten ring buffer. Read the View CB at the
  moment of the dispatch, and copy out only the rows we need.

### DLSS / NGX

Ground truth is `NVIDIA/DLSS` (`include/nvsdk_ngx*.h`) plus the DLSS Programming Guide in that
repo. Luma vendors a copy at `Source/External/NGX/`.

The conventions that silently ruin an image, all of which we must pin down and log:

* `InJitterOffsetX` / `InJitterOffsetY` — units and **sign**. UE4 gives us
  `TemporalAAParams.zw`, with a **negative Y factor** (§2.6).
* `InMVScaleX` / `InMVScaleY` — what these must be given UE4 stores velocity as an **NDC delta**,
  not a pixel delta, and given `MVLowRes`.
* MV **direction** — current→previous, and Luma's "positive toward top-left" note.
* `DepthInverted` — Stray is reversed-Z, so this is set.
* `IsHDR` + `AutoExposure` / `InPreExposure` — UE4 applies a pre-exposure to scene colour; DLSS
  needs to know.
* `InReset` — driven by the camera-cut detector of §2.7.
* D3D12 **resource states** at evaluate time, and whether NGX clobbers our descriptor heaps and
  root signature. In D3D12 this is our responsibility, unlike D3D11.

`nvngx_dlss.dll` redistribution is governed by the NVIDIA RTX SDKs licence. Prefer having CI
download it from the NVIDIA/DLSS release rather than committing a large binary.

### Unreal Engine 4.27

Ground truth is the UE 4.27 branch: `PostProcess/TemporalAA.cpp`,
`Shaders/Private/PostProcessTemporalAA.usf`, `TemporalAACommon.ush`, `Common.ush`, `SceneView.h`,
`VelocityRendering.cpp`.

Things to keep straight:

* `FTAAStandaloneCS` has **multiple permutations** (`ETAAPassConfig`, `TAA_UPSAMPLE`,
  `TAA_DOWNSAMPLE`, `TAA_RESPONSIVE`, quality dims). Binding layout differs per permutation —
  that is why Stray has two hashes (§2.3).
* Of the two colour SRVs at `t5`/`t6`, one is this frame's scene colour and one is the previous
  frame's history. **Feed DLSS the scene colour, never the history.** Getting this backwards
  produces a plausible-looking but progressively degrading image.
* Velocity is sparse; UE4's own TAA tests validity and falls back to camera motion. Find the exact
  test in `PostProcessTemporalAA.usf` and mirror it rather than inventing a threshold.
* `ClipToPrevClip` (row 122): confirm row-major vs column-major in the CB and whether it includes
  jitter, before doing any matrix math with it.
* `View.ViewRectMin` (129) and `View.ViewSizeAndInvSize` (130) give us the true render subrect —
  needed for `InRenderSubrectDimensions` and the `*SubrectBase` fields.
* Screen percentage is settable from `Engine.ini` `[SystemSettings]`; command-line is not (§2.2).

---

## 6. Build, CI and testing

* **CMake + GitHub Actions, Windows runner, MSVC, x64 only.** There is no local build. If it does
  not build in CI it does not build.
* Static-link the CRT (`/MT`) so the add-on does not drag a UCRT dependency into the Proton
  prefix. NGX ships `_s` (static) and `_d` (dynamic) import libs — pick the one matching.
* Warnings are errors. `/W4`.
* **Unit tests run in CI** for every pure function: fnv1a64 over DXBC, the UE4 velocity decode
  (assert against the measured constants `4.00801611f` / `2.00397754f` / `0.49999237f`), jitter
  conversion including the negative Y, `ClipToPrevClip` reconstruction math, quality-mode
  selection, Halton, mip bias. Prefer building these for Linux too so they run fast.
* Ship PDBs as artifacts so a crash from the user's machine can be symbolised.
* **Logging is the product's second output.** Version banner, device API, ReShade version, every
  detected shader hash, the chosen TAA hash and why, the View CB rows read (raw floats), the
  computed jitter, MV scale, feature flags, NGX result codes by name, and every state transition.
  The user pastes this back; it must be enough to diagnose without a second round-trip.
* Package a release zip: `.addon64` + `nvngx_dlss.dll` + README + a default `Engine.ini` snippet.

---

## 7. House rules

* Follow the Superpowers process skills: `brainstorming` before creative work,
  `systematic-debugging` before proposing a fix, `test-driven-development` for anything with a
  testable core, `verification-before-completion` before any claim of doneness.
* No speculative features. Ship DLAA correctly before anything else exists.
* Keep files focused. When a file grows past a few hundred lines it is doing too much — Luma's
  17k-line `core.hpp` is a thing to learn from, not to imitate.
* Every constant taken from the measured facts above gets a **named symbol with a comment citing
  §2.x**, never a bare literal.
* When a fact in §2 is contradicted by observation on the user's machine, **update §2 and
  `docs/STRAY-RENDERING-FACTS.md`** in the same change that reacts to it.
* Never commit `nvngx_dlss.dll` or game assets.
