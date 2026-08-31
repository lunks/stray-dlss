# stray-dlss — agent operating manual

A ReShade **add-on** (not an effect/shader preset) that injects **NVIDIA DLSS Super Resolution**
into **Stray** (Unreal Engine 4.27.2, **D3D12**) by intercepting the engine's own temporal AA
compute dispatch and replacing it with an NGX evaluation.

Read this file completely before touching anything. It is the contract for how this project is
built, what is known versus assumed, and what will silently produce a wrong image.

Depth lives in two companion documents, both of which are load-bearing:

* **`docs/STRAY-RENDERING-FACTS.md`** — what was measured against the running game. Verbatim.
* **`docs/RESEARCH.md`** — verified external research: the ReShade 6.8 API, the NGX D3D12 SDK, UE
  4.27 internals, the Proton/vkd3d chain, and CI. 228 claims, each adversarially verified. **When
  this file and `docs/RESEARCH.md` disagree, RESEARCH.md wins** — it carries the citations.

---

## 0. Prime directives

1. **You cannot test this yourself.** No game, no Windows box, no GPU. Every build happens in
   GitHub Actions with MSVC. The only feedback loop is the human running the artifact on their
   Linux/Stray machine and pasting back a log. A round-trip is expensive, so each one must carry
   the maximum diagnostic payload.
2. **Prefer a loud failure to a quiet wrong image.** A crash gets a stack trace. A wrong
   motion-vector sign gets "it looks smeary" three days later and costs a week. Every convention
   must be asserted and logged, never assumed. NVIDIA's guide is explicit that wrong resource usage
   flags can produce *"the output may be black without further indication"* — that class of failure
   is the enemy.
3. **Never claim something works because it compiled.** The only evidence DLSS is engaged is a log
   line from the user's machine, a screenshot, or the DLSS on-screen indicator. Follow
   `superpowers:verification-before-completion`.
4. **Everything provable in CI must be tested in CI.** DXBC hashing, matrix math, jitter
   conversion, velocity decode, quality-mode selection — all pure functions. Keep the untestable
   NGX surface as thin as possible.
5. **HARD / SOFT / UNCONFIRMED.** Label the provenance of every technical claim you write down.
   "Read it in the header" is HARD. A forum post is SOFT. Everything else is UNCONFIRMED and must
   be verified before code depends on it.

---

## 1. Target environment

The human runs Stray on Linux. Proton is the target, not an afterthought.

| | |
|---|---|
| GPU | NVIDIA RTX 4090 |
| Driver | 610.43.02, open kernel modules |
| Host | Linux 6.17.13, SteamOS guest |
| Proton | `GE-Proton-dxvk301-ds5-clean-nowl` (vkd3d 3.0.1-era) |
| D3D12 layer | **vkd3d-proton** |
| Compositor | gamescope, DRM backend, `--hdr-enabled --hdr-itm-enabled` |
| ReShade | **6.8.0.2155**, add-on support enabled |
| Game | Stray, Steam AppID **1332010** |

### Feasibility: resolved

**D3D12 NGX DLSS works under vkd3d-proton.** It is an NVIDIA-authored path present since
vkd3d-proton 2.5, routed through `NvAPI_D3D12_*` cubin entry points in DXVK-NVAPI into
`ID3D12DeviceExt` and `VK_NVX_binary_import`. `nvngx_dlss.dll` never touches native handles — it
makes ordinary D3D12 descriptors — so `ID3D12Resource` being a `VkImage` underneath is a non-issue.

**Consequences, and they are absolute:**

* **Pass plain `ID3D12Resource*` to NGX and nothing else.** Never touch `ID3D12DXVKInteropDevice`,
  `GetVulkanResourceInfo`, or any vkd3d interop API. Never write a D3D12→Vulkan bridge. Never use
  the Vulkan NGX path.
* **Initialise NGX with `device::get_native()`**, which is the *original* vkd3d `ID3D12Device`, not
  ReShade's proxy. This bypasses ReShade's descriptor remapping, which is exactly what we want —
  ReShade's `convert_to_original_cpu_descriptor_handle` has no release-build validation and would
  corrupt any handle it did not mint. Never fetch ReShade's proxy device via `GetPrivateData`.
* **ReShade ≥ 6.8.0 is a hard prerequisite.** ReShade-addon + D3D12-DLSS under vkd3d was a known
  crash; crosire fixed it in two commits, both in tag v6.8.0. 6.7.3 is still broken. Log the
  ReShade version at startup and warn below 6.8.0.
* **`PROTON_ENABLE_NVAPI` does not exist.** NVAPI is on by default. Only `PROTON_DISABLE_NVAPI` and
  `PROTON_FORCE_NVAPI` exist. Never set `PROTON_HIDE_NVIDIA_GPU=1`.
* **Do not gate availability on `NVSDK_NGX_D3D12_GetFeatureRequirements`** — Proton's `_nvngx.dll`
  does not implement it and reports unsupported even when DLSS works. Gate on `Init` succeeding
  plus `NVSDK_NGX_Parameter_SuperSampling_Available`.
* **Do not set `VKD3D_CONFIG=descriptor_heap`.** Leave `PROTON_ENABLE_NGX_UPDATER` off.
* Detect vkd3d for free: `QueryInterface(IID_ID3D12GraphicsCommandListExt,
  77a86b09-2bea-4801-b89a-37648e104af1)` on the native command list. ReShade uses this itself.

### The native-device rule has a trap, measured 2026-08-31

**Two configurations are self-consistent. The one we currently ship is only safe by luck.**

ReShade 6.8.0 added `source/d3d12/d3d12_extensions.cpp`. Inside its proxy's `QueryInterface`
for `IID_ID3D12DeviceExt`/`Ext1`/`Ext2` it patches vtable slots **7/8** (and 14/15 for `Ext2`)
of vkd3d's extension interface, so that `GetCudaTextureObject` / `GetCudaSurfaceObject` run
their descriptor handles through `convert_to_original_cpu_descriptor_handle`. Those are the
**CUDA texture and surface entry points — the path `nvngx_dlss.dll` takes under vkd3d.**

The conversion has no release-build guard:

```cpp
const size_t heap_index = (handle.ptr >> heap_index_start) & 0xFFFFFFF;
assert(heap_index < _descriptor_heaps.size() && _descriptor_heaps[heap_index] != nullptr);
return { _descriptor_heaps[heap_index]->_orig_base_cpu_handle.ptr + ... };
```

A **real vkd3d handle** yields a garbage `heap_index`, an out-of-bounds read, and a handle
pointing anywhere. DLSS then samples the wrong texture, silently. **That is what a colour cast
looks like.**

| NGX device | ext vtable patched? | Result |
|---|---|---|
| native (`get_native()`) | no | **works** — handles are never converted |
| ReShade proxy | yes | **works** — ReShade minted them, so conversion is correct |
| **native** | **yes** | **BROKEN** — real handles run through the conversion |

**Measured on the RTX 4090, 2026-08-31** (`test_vkd3d_ext_hook_reachability`): vkd3d-proton uses
a **single static vtable** for this interface, so one query against ReShade's proxy — *from
anywhere in the process* — patches the vtable that interfaces taken straight from the **original**
device also use. `before=…C645AD0 after=…D5680D0`, and the patched entry belongs to ReShade's
DLL. Reachability is **HARD**.

**What is still UNCONFIRMED: whether anything in the live game performs that query.** It is
plausible — the game's own device *is* ReShade's proxy, DXVK-NVAPI is active on this machine
(`DXVK_ENABLE_NVAPI=1`, `DXVK_NVAPI_VKREFLEX=1`), and DXVK-NVAPI queries the device it is handed
for `ID3D12DeviceExt`. If UE4 calls any `NvAPI_D3D12_*` entry point with its device, the patch
installs and we land in the broken row. **Do not treat §1's native-device rule as sufficient on
its own — check the vtable at NGX init and log loudly.**

Full chain, prerequisites and diagnostics: `docs/RESEARCH.md` §1.

---

## 2. Everything we know about Stray

Measured on hardware, read out of the game's own DXBC, or read from the game's own files — not
inferred. Verbatim source: `docs/STRAY-RENDERING-FACTS.md`. Facts *derived* from these by verified
research are marked **[derived]**.

### 2.1 Engine and process

| Fact | Value |
|---|---|
| Engine | **Unreal Engine 4.27.2** |
| Executable | `Stray-Win64-Shipping.exe`, PE32+ (x86-64) |
| Graphics API | **D3D12** |
| UE project name | `Hk_project` |
| Version string | `v1.54368 (Revision 26632)` |

Observed swapchains from `IDXGISwapChain::ResizeBuffers`:
`BufferCount = 3` at `3840×2160` and `2560×1440`, `NewFormat = 24` = `DXGI_FORMAT_R10G10B10A2_UNORM`.

**[derived]** Each output-resolution change requires a full NGX `ReleaseFeature` + `CreateFeature`.
Guard Evaluate to no-op when observed sizes differ from creation sizes.

### 2.2 Filesystem layout

```
<SteamLibrary>/steamapps/common/Stray/Hk_project/Binaries/Win64/
```

Config and saves live in the **Proton prefix**:

```
<compatdata>/1332010/pfx/drive_c/users/steamuser/AppData/Local/Hk_project/Saved/
    Config/WindowsNoEditor/Engine.ini
    SaveGames/
    Crashes/UE4CC-Windows-<GUID>_0000/     CrashContext.runtime-xml, UE4Minidump.dmp
    Logs/                                   (observed empty)
```

* **`Engine.ini` settings take effect. Command-line arguments do not.** Everything we need the
  engine to do goes through `[SystemSettings]` in that file — which **[derived]** is also the only
  way to set `ECVF_ReadOnly` cvars like `r.UsePreExposure`.
* Two `compatdata` trees exist on this machine. The live one is
  `/home/deck/.local/share/Steam/steamapps/compatdata/1332010`. The one on `GamesLinux` is a
  ~6.1 MB skeleton that is **not read**.

### 2.3 The TAA pass — our interception point

Stray uses UE 4.27's `FTAAStandaloneCS`. **[derived]** that is
`/Engine/Private/TemporalAA/TAAStandalone.usf`, entry `MainCS` — **`PostProcessTemporalAA.usf` does
not exist in 4.27**. Threadgroup 8×8, dispatch `ceil(W/8) × ceil(H/8)`.

> **CORRECTED 2026-08-30, against the live game.** The original measurement had the shader
> identities **backwards**, and everything below supersedes it. The add-on now captures real
> bindings per dispatch, and a separate bytecode analysis confirmed the correction
> independently. `docs/STRAY-RENDERING-FACTS.md` is left as the historical record.

**The TAA pass is:**

```
0x901e041a7cadc9db          <-- previously mislabelled "a measured false positive"
```

Captured live at 3840×2160 output. Note every input is **1920×1080**: the game already runs
**temporal upsampling at 50% screen percentage**, so the pass is `ETAAPassConfig::MainUpsampling`,
not `Main`.

| Register | Role | Format | Size |
|---|---|---|---|
| `t0` | `EyeAdaptationTexture` | `R32G32B32A32_FLOAT` | 1×1 |
| `t1` | `InputSceneColor` | `R16G16B16A16_FLOAT` | 1920×1080 |
| `t2` | `SceneDepthTexture` | `R32_FLOAT_X8X24` | 1920×1080 |
| `t3` | `GBufferVelocityTexture` | `R16G16B16A16_UNORM` | 1920×1080 |
| `t4` | `StencilTexture` | `X32_G8X24_UINT` | **same resource as t2** |
| `t5` | `HistoryBuffer_0` | `R16G16B16A16_FLOAT` | 1920×1080 |
| `u0` | `OutComputeTex_0` | `R16G16B16A16_FLOAT` | **3840×2160** |

Dispatch is **480×270** = `ceil(3840/8) × ceil(2160/8)` — over the **output** rect, not the
render rect. Matching against the render rect rejects the real pass.

**Why the original identification failed, and the lesson.** The old heuristic scored this
shader `colour=1 depth=2 velocity=0` and rejected it. Both halves of that score are actually
the TAA's *signature*: `velocity=0` because the frame it sampled was a **camera cut**, where
UE4 substitutes the 1×1 `GSystemTextures::BlackDummy` for velocity and history; `depth=2`
because the depth SRV and the stencil SRV are **two views of one resource**. The gate rejected
the pass for exactly the properties that identify it. **`EyeAdaptationTexture` is also 1×1 and
present every frame, so "any 1×1 SRV" is not a cut signal** — only the velocity and colour
slots count.

**The strongest runtime discriminator** is the depth+stencil pair over a shared resource. No
other pass in this title binds one resource as both `R32_FLOAT_X8X24` and `X32_G8X24_UINT`.

**Two convincing look-alikes, both excluded:**

* `0x1708ec956099e259` — previously believed to be the primary TAA. It is a **reprojecting
  denoiser** (SSR/SSGI/AO family): it reads depth and velocity and reprojects with
  `ClipToPrevClip`, which is why its bindings look right. It declares `cb1[126]` and so never
  indexes View row 144 (`StateFrameIndexMod8`), which **every** `FTAAStandaloneCS` permutation
  reads — therefore it cannot be this shader. Dispatches 240×135, output 1920×1080.
* `0x52101a15e1a0c5cc` — eleven SRVs, two UAVs, `cb1[131]`. Not TAA.

**Bytecode evidence for the correction** (from the shipped DXBC, `cs_5_0`, `numthreads(8,8,1)`):

* Every `FTAAStandaloneCS` permutation must declare `cb1[145]`; only five dumped shaders do,
  and `0x901e` is one.
* `0x901e` declares `dcl_tgsm_structured` stride 16 count 64 — `float4[64]`, which compiles
  only under `AA_UPSAMPLE == 1` with `TAA_SCREEN_PERCENTAGE_RANGE == 1` (input under ~71% of
  output).
* The upsample-only kernel constants `0.905` and `-1.9` appear 7× in `0x901e` and in no other
  shader except its sibling permutation.

**Consequence for the DLSS work.** The game is *already* upsampling 1080p→4K. DLSS should be
created as **1920×1080 → 3840×2160** (a Performance-ratio feature), **not DLAA** — which
inverts the plan's staging, since there is no 1:1 pass to replace.

### 2.3.1 Confirmed from the game's own shipped configuration

Extracted from `Hk_project-WindowsNoEditor.pak` (`tools/pakextract.py`) and from the live prefix
config. Copies are kept in `docs/game-config/`. These retire several previously *derived* facts
to HARD — they are the developers' own settings, not inference:

| Setting | Value | Source | What it settles |
|---|---|---|---|
| `r.DefaultFeature.AntiAliasing` | **2** (TAA) | `DefaultEngine.ini:54` | TAA is the AA method |
| `r.TemporalAA.Upsampling` | **True** | `DefaultEngine.ini:69` | **TAAU ships enabled.** The upsampling TAA pass is the game's own default, not a user setting |
| `r.BasePassOutputsVelocity` | **True** | `DefaultEngine.ini:59` | Velocity is written in the base pass, so coverage is broader than stock UE4 — helpful for DLSS, but the resolve pass is still required |
| `r.PostProcessAAQuality` | **3** (Medium) | `WindowsEngine.ini:18` | Confirms the earlier derivation exactly |
| `r.DOF.TemporalAAQuality` | 0 | `WindowsEngine.ini:58` | |
| `ScreenPercentage` | **50** | live `GameUserSettings.ini` | Renders 1920×1080 |
| `ResolutionSizeX/Y` | **3840×2160** | live `GameUserSettings.ini` | Output is 4K |

So the render/output relationship measured at runtime — 1920×1080 in, 3840×2160 out — is
exactly what the shipped configuration asks for. **`ETAAPassConfig::MainUpsampling` is
confirmed by the game's own settings**, independently of the bytecode and binding evidence.

The only user-added override in the live `Engine.ini` is `r.BasePassOutputsVelocity=1`, which is
redundant since the game already ships it as True.

**Reading the pak.** It is version 11, **unencrypted**, 5.3 GiB, 55,120 entries.
`tools/paklist.py` lists it and `tools/pakextract.py` extracts by regex, both reading only the
index blobs rather than the archive — worth knowing, because a bulk copy is neither necessary
nor kind to a machine someone is playing on.

**Shader census:** 728 distinct PS/CS shaders in gameplay, `not_dxbc=0`, `dxil=0` — every one is
**DXBC**. ~150 in the main menu, rising to ~728 on entering gameplay.

### 2.4 Depth

* `t0` resource is `r32_g8_typeless`; the SRV is `r32_float_x8_uint`.
* UE 4.27 uses **reversed-Z** → `NVSDK_NGX_DLSS_Feature_Flags_DepthInverted`. **[derived]** near =
  1.0, far = 0.0, **infinite far plane**, `SceneDepth = Near / DeviceZ`. The TAA shader
  hard-`#error`s on non-inverted Z. Read `View.NearPlane` (row 142.x) rather than assuming 10 uu.

Depth over menu and loading screens reads `below 0.25: 3456000, above 0.75: 0, mean 0.00000` —
**menu and load frames carry no usable depth range; gameplay frames do.** Use this as a cheap
"are we in gameplay" gate.

### 2.5 Velocity — and the sparse-velocity problem

`t2` is `r16g16b16a16_unorm` and **sparse**: UE 4.27 writes it only for **moving objects**. Static
geometry carries nothing and its motion must be reconstructed from depth and the camera matrices.

**[derived] A resolve pass is mandatory regardless**, because DLSS only accepts `RG16_FLOAT` or
`RG32_FLOAT` motion vectors and `r16g16b16a16_unorm` is not an accepted format.

**[derived]** The velocity texture is at the full **scene-buffer** extent
(`BufferSizeAndInvSize`, row 132), *not* the view size — index it with absolute buffer coords.

**Encoding, from UE 4.27 `Common.ush:1537-1570`:**

```
EncodedV.xy = V.xy * (0.499f * 0.5f) + 32767.0f / 65535.0f          // encode
V.xy        = EncodedV.xy * InvDiv - 32767.0f / 65535.0f * InvDiv   // decode
InvDiv      = 1.0f / (0.499f * 0.5f)
```

**Both constants located in Stray's own DXBC:**

| Constant | Value | Bit pattern | Notes |
|---|---|---|---|
| `InvDiv` | `4.00801611f` | `0x408041AB` | bytes `AB 41 80 40` |
| Folded MAD bias | `2.00397754f` | `0x4000412B` | appears **negated** as `0xC000412B` |
| Bias term | `32767/65535 = 0.49999237f` | `0x3EFFFF00` | **not** 0.5 |

**[derived] Compute the bias in float as `(32767.0f/65535.0f) * InvDiv`; do not hardcode a rounded
constant.** `DecodeVelocityFromTexture` takes a `float4` and returns a `float3`.

**[derived] Validity test is `EncodedVelocity.x > 0.0`** — strict, red channel only, because the
target is cleared to 0. Reproduce it exactly; never use a magnitude threshold.

**[derived]** The stored value is `ScreenPos - PrevScreenPos` in NDC with **both** frames' jitter
removed — directly comparable to the `ClipToPrevClip`-derived camera motion. Both branches are
jitter-free and both are current-minus-previous.

### 2.6 The View constant buffer

Stray's TAA shader carries the stock UE 4.27.2 `View` uniform buffer at register **`b1`**. Observed
total sizes vary (126 / 131 / 145 float4s) but **row offsets do not**. The layout was established
twice independently from `VIEW_UNIFORM_BUFFER_MEMBER_TABLE` (`SceneView.h:582-774`), and **[derived]**
a third time by a recomputation that reproduced all seven measured anchors exactly.

| Field | Row | Byte | |
|---|---|---|---|
| `ViewToClip` | 28 | 448 | measured |
| `ViewToClipNoAA` | 32 | 512 | measured |
| `InvDeviceZToWorldZTransform` | 65 | 1040 | [derived] |
| `ClipToPrevClip` | 122 | 1952 | measured |
| `TemporalAAJitter` | 126 | 2016 | measured |
| `ViewRectMin` | 129 | 2064 | measured |
| `ViewSizeAndInvSize` | 130 | 2080 | measured |
| `LightProbeSizeRatioAndInvSizeRatio` | 131 | 2096 | measured — **decoy**, reads `(1,1,1,1)` |
| `BufferSizeAndInvSize` | 132 | 2112 | [derived] |
| `PreExposure` | 135.y | 2164 | [derived] |
| `OneOverPreExposure` | 135.z | 2168 | [derived] |
| `NearPlane` | 142.x | 2272 | [derived] |
| `DeltaTime` | 143.x | 2288 | [derived] |
| `CameraCut` | 145.x | 2320 | [derived] |
| `TemporalAAParams` | 152 | 2432 | measured |

Everything we need is in a single **2448-byte prefix**. **Rows beyond 152 were not verified — do
not use them.**

`ClipToPrevClip` at row 122 was confirmed **in Stray's own TAA shader by pure DXBC instruction
analysis**, no reflection names involved.

Traps:

* Row 131 is `(1,1,1,1)` — a decoy for any search expecting an identity-looking row.
* `dcl_constantbuffer cb1[131]` — that 131 is the **highest row indexed**, not the buffer size.
* Offsets are fixed per engine build but not invariant across a licensee edit. Keep checking them.
* **[derived] Never read b0.** UE4 strips the DXBC reflection chunk (`D3DCOMPILER_STRIP_REFLECTION_DATA`),
  so `D3DReflect` is impossible; and loose params land in fxc's implicit `$Globals`, which fxc
  **compacts by removing unused globals**, so b0 offsets are unpredictable from source. b1 is an
  explicit `cbuffer View` with a layout fixed by `FShaderParametersMetadata`.

### 2.7 Jitter

```
InJitterOffsetX = TemporalJitterPixels.X = TemporalAAParams.z = TemporalAAJitter.x * W *  0.5f
InJitterOffsetY = TemporalJitterPixels.Y = TemporalAAParams.w = TemporalAAJitter.y * H * -0.5f
```

The negative Y factor is real — **but only in the derivation**. **[derived] Do not derive anything:**

> **`InJitterOffsetX = TemporalAAParams.z` (byte 2440), `InJitterOffsetY = TemporalAAParams.w`
> (byte 2444). Read and assign straight across. No negation, no scaling.**

`TemporalAAParams.zw` **is** `TemporalJitterPixels`, already in render-resolution pixels, in
`[-0.5, +0.5]`, and NVIDIA's own UE plugin passes it to NGX unmodified.

**[derived]** `TemporalAAParams` is `(JitterIndex, SequenceLength, JitterPixelsX, JitterPixelsY)`;
`TemporalAAJitter` is `(CurX, CurY, PrevX, PrevY)` in clip/NDC units.

**[derived]** In the shipped (non-upsampling) mode UE4 warps Halton through Box-Muller with
`sigma = 0.47 * r.TemporalAAFilterSize` — a distribution DLSS was **not** trained on. Forcing
`r.TemporalAA.Upsampling=1` switches to plain Halton in `[-0.5, 0.5]`, which is what DLSS *was*
trained on. See §4.

### 2.8 Camera cuts

UE 4.27 assigns `PrevViewMatrices = ViewMatrices` on a cut. The observable consequence is
**`View.TemporalAAJitter.zw == .xy`**. Confirmed working as a live cut detector against the running
game (`detector=LIVE`). Observed counts: **3** across splash and main menu, **5** once gameplay is
running.

**[derived] That signal alone is insufficient.** `bCameraCut = !InputHistory.IsValid() ||
View.bCameraCut`, and the history-invalid case never reaches the View buffer. OR three signals for
`InReset`:

1. `View.CameraCut != 0` (row 145.x);
2. `TemporalAAJitter.zw == .xy` (our measured heuristic);
3. **the history or velocity SRV is a 1×1 texture** — most reliable, directly reflects
   `!InputHistory.IsValid()`.

### 2.9 TAA history

The resource at `u0` is extracted by UE 4.27 as the **next frame's `HistoryBuffer[0]`**.
**Overwriting `u0` feeds whatever we write into the next frame's temporal history** — the hook that
lets us replace TAA from outside the engine.

The same resource can also appear as this frame's **scene-colour input**; the two cases are
distinguishable **only by which register it turns up on**. Track by register, never by identity
alone.

### 2.10 Stability observations

Environment facts, independent of any add-on — useful when triaging so we do not chase our own tail:

* `gamescope-wl` segfaulted three times in one afternoon (11:40, 11:42, 14:03) and once the day
  prior. The nvidia driver was unloaded and reloaded at 11:43.
* One GPU `Xid 109 (CTX SWITCH TIMEOUT)` against `Stray-Win64-Shi`, channel `0x00000012`.
* UE4 crash dumps exist **from sessions with no third-party add-on at all**:
  `Unhandled Exception: 0xe06d7363` (a C++ exception) and one `EXCEPTION_ACCESS_VIOLATION reading
  address 0x0000000000000010`. **Stray crashes on its own.** Do not assume a crash is ours.
* Killing the game leaves a `reaper` process (`SteamLaunch AppId=1332010`) behind. While it exists,
  Steam silently ignores further `steam://rungameid/1332010` launches.

### 2.11 Driving the game unattended

* The physical DualSense is held by **Steam** via `/dev/hidraw0`. Nothing holds its evdev nodes.
* Steam Input re-emits it as **"Microsoft X-Box 360 pad 0"** — that node is what the game reads. Its
  `eventN` number is **not stable**; Steam tears it down with the game.
* Writing `input_event` structs directly to `/dev/input/eventN` reaches `input_inject_event()` and
  is seen by every reader. No `uinput`, `ydotool` or `evemu` needed. Neither the pad nor the
  keyboard node is `EVIOCGRAB`'d.
* ReShade's screenshot bind is `KeyScreenshot=44` (`VK_SNAPSHOT`) = Linux `KEY_SYSRQ=99`. Injecting
  it on the real keyboard node makes ReShade write a 4K PNG into the game directory. **This is our
  screenshot channel for visual verification.**
* gamescope's `SIGUSR2` screenshot produced no file. `ffmpeg`'s `kmsgrab` cannot read its
  framebuffer (`XB30`, 10-bit HDR).

---

## 3. How the add-on works

```
                    ┌────────────────── ReShade add-on events ──────────────────┐
 game frame ─▶ init_pipeline ──▶ fnv1a64 the DXBC ──▶ 0x1708ec956099e259?
                                                      + binding signature + dispatch size
                                                              │ yes
               bind_pipeline ───────────────────────────────── ┘   (stage is `all`, NOT compute!)
               push_descriptors / bind_descriptor_tables
                          └─▶ capture by REGISTER: t0 depth, t2 velocity,
                              t5/t6 colour, u0/u1 output, b1 View CB
               dispatch ──▶ INTERCEPT
                             ├─ map b1, copy the 2448-byte prefix, read rows
                             │    122 ClipToPrevClip · 129/130 rect · 132 buffer size
                             │    135.y PreExposure · 145.x CameraCut · 152 jitter
                             ├─ identify history vs scene colour via last frame's u0 pointer
                             ├─ InReset = CameraCut | jitter.zw==xy | 1x1 history/velocity
                             ├─ our MV-resolve CS → dense RG16_FLOAT at render res:
                             │     EncodedVelocity.x > 0 ? decode : camera motion from
                             │     depth + ClipToPrevClip;  then * (0.5W, -0.5H), negated
                             ├─ barrier inputs → NON_PIXEL_SHADER_RESOURCE, output → UAV
                             ├─ NGX EvaluateFeature(colour, depth, denseMV, jitter) → u0
                             ├─ RESTORE clobbered D3D12 state (heaps, root sig, PSO, ...)
                             ├─ produce u1 (half-res filter of u0) — required!
                             └─ return true  (skip the engine's dispatch)
                             │
               u0 becomes next frame's HistoryBuffer[0] ──▶ engine continues
```

Four stages, each testable in isolation as far as CI allows:

1. **Identify** — hash every compute shader's DXBC at `init_pipeline`; confirm with binding
   signature and dispatch size; never select `0x901e041a7cadc9db`; never hook `0x52101a15e1a0c5cc`.
2. **Capture** — record bound SRVs/UAVs/CB **by register** and read the View CB rows.
3. **Resolve** — our compute pass turning sparse velocity + depth + `ClipToPrevClip` into the dense
   `RG16_FLOAT` field DLSS requires, in DLSS's units and sign.
4. **Evaluate** — NGX into `u0`, restore state, produce `u1`, skip the engine dispatch.

### Staging

* **v0.1 — DLAA.** Render resolution == output resolution. No screen-percentage forcing. Isolates
  one question: *are colour, depth, motion vectors and jitter correct?*
* **v0.2 — DLSS SR.** See §4. Do not start v0.2 before v0.1 is confirmed correct **on the user's
  machine**.

---

## 4. The super-resolution path

**[derived]** The correct way is not to fight the engine. Set in `Engine.ini [SystemSettings]`:

```ini
r.ScreenPercentage=<N>
r.TemporalAA.Upsampling=1
```

Then `PrimaryScreenPercentageMethod` becomes `TemporalUpscale`, TemporalAA.cpp selects
`ETAAPassConfig::MainUpsampling`, `InputViewRect` is render res while `OutputViewRect` is display
res, and the engine allocates a **full-res output texture** and expects the TAA pass to upscale —
**exactly DLSS's contract**. Every downstream pass already uses the upscaled rect.

Three things come free: **plain Halton jitter** in `[-0.5, 0.5]` (what DLSS was trained on, versus
the Box-Muller-warped sequence in the shipped mode); **auto-scaled phase count** reproducing
NVIDIA's `Base × (Target/Render)²`; and the **correct texture mip bias**.

**The cost: the permutation and therefore the DXBC hash change.** `0x1708ec956099e259` will no
longer appear. Plan to re-derive it or match structurally.

Hard constraints:

* **`r.TemporalAA.Algorithm` must stay 0.** Gen5 replaces the single dispatch with a chain of six
  shaders and breaks the hook entirely.
* `r.TemporalAA.HistoryScreenPercentage` must stay 100 — above that switches to `MainSuperSampling`.
* Raise `r.TemporalAASamples` for SR modes; the default 8 is enough for DLAA but Performance needs
  32.
* **Never disable TAA via `r.DefaultFeature.AntiAliasing`.** NVIDIA's guide §8.1.3 is explicit:
  replace the TAA pass, but everything the engine does *because* TAA is on — jitter, velocity
  generation — must keep happening.

---

## 5. Domain quick-reference

The full detail is in `docs/RESEARCH.md`. These are the things that bite.

### ReShade 6.8 add-on API

* **Pin headers to tag `v6.8.0`.** `RESHADE_API_VERSION` is **20**; ReShade rejects anything newer
  than its own. Vendor the 8 headers; do not track `main`.
* **Do not define `RESHADE_API_LIBRARY`** — header-only is the supported path.
* **imgui is pinned to v1.92.5 (`IMGUI_VERSION_NUM == 19250`)**, enforced by `#error`. Headers only.
  Include `imgui.h` *before* `reshade.hpp`. A mismatch makes `register_addon` return **false** and
  the add-on never loads — log to a file *before* `register_addon` so this is diagnosable.
* **`reshade::log_message` no longer exists.** Use
  `reshade::log::message(reshade::log::level::info, buf)`. It takes no printf args.
* **In D3D12 `bind_pipeline` fires with `pipeline_stage::all`, NOT `compute_shader`.** Filtering on
  the compute stage silently misses every event.
* **`dispatch` is the only skip-capable event on our path** — return `true` to suppress. Everything
  else is `void`.
* **`push_descriptors`, `bind_descriptor_tables`, `bind_pipeline`, `create_pipeline` require
  `RESHADE_ADDON >= 2`** — the full add-on build. Assert at startup that we saw one in the first N
  frames, or "wrong ReShade build" is indistinguishable from "UE4 binds differently".
* **Registering the pipeline events has a side effect**: ReShade redirects all PSO creation through
  `ID3D12Device2::CreatePipelineState` and **drops the cached-PSO blob**. Expect first-run hitching
  under vkd3d. If it regresses, identify structurally and drop those events.
* `get_native()` returns **`uint64_t`** — reinterpret_cast it. All handles are
  `struct { uint64_t handle; }`.
* Track the game's resources **by descriptor register**, never by pointer alone (§2.9).
* Read the View CB **inside the `dispatch` callback**, never deferred — the upload ring has not
  advanced yet at recording time. Use `map_buffer_region(..., map_access::read_only, ...)`.
* **Copy `examples/utils/descriptor_tracking.{hpp,cpp}` and `state_tracking.cpp` verbatim.** Register
  descriptor tracking from the very first frame — attach late and there is no retroactive recovery.
* Register both a `nullptr` (settings) overlay and an `"OSD"` overlay. The OSD is the fastest remote
  diagnostic when the user can only send a screenshot.

### DLSS / NGX

* **Feature flags for Stray: `IsHDR | MVLowRes | DepthInverted | AutoExposure` = `0x4B`.** Never set
  `DoSharpening` (deprecated, does nothing).
* **Motion vectors: `RG16_FLOAT`, render-resolution pixels, [0,0] upper-left, pointing BACKWARD.**
  `MV_pixels = (PrevScreen - ThisScreen) * (0.5·W, -0.5·H)`, `InMVScaleX/Y = (1,1)`. Guard with
  `PrevClipPos.w > 0`. This is NVIDIA's own `VelocityCombine.usf` math — copy it, don't invent it.
* **Jitter: pass `TemporalAAParams.zw` straight through. No sign flip.**
* **Presets: only `0, J=10, K=11, L=12, M=13` are valid** (A–D removed, E/F deprecated). Use **K**.
  Set all five hint keys **before** `CreateFeature`; setting them after has no effect.
* **NGX clobbers D3D12 command-list state.** We must save and restore descriptor heaps, root
  signature, PSO, root params, topology, viewports and RTVs. ReShade does not do this for us. This
  is the number-one corruption risk.
* Inputs must be `NON_PIXEL_SHADER_RESOURCE`, output must be UAV with `ALLOW_UNORDERED_ACCESS`.
  Missing that flag can produce a **black output with no error**.
* Init with `Init_with_ProjectID` and our own GUID — **no NVIDIA whitelist needed**. Pass a
  `LoggingCallback` that forwards into `reshade::log::message`.
* Derive the quality mode from `NGX_DLSS_GET_OPTIMAL_SETTINGS` rather than choosing one; retry a
  rejected create with `Balanced` + `Preset_Default`.
* Results are a **bitmask** over `0xBAD00000` — use `NVSDK_NGX_SUCCEED`/`NVSDK_NGX_FAILED`, never
  `== Success`.
* Teardown on one thread, GPU idle first: `ReleaseFeature` → `DestroyParameters` → `Shutdown1`.
  **NGX is not thread-safe.** It holds no references to our resources — we must.

### UE 4.27

* The shader is `TAAStandalone.usf` / `MainCS`.
* `ClipToPrevClip` is built from **jitter-free (NoAA)** matrices, is **row-major**, and needs the
  **row-vector** convention (`mul(v, M)`). Transpose if you rebuild it in a column-vector library.
* Scene colour entering TAA **carries pre-exposure**; write our output back **still pre-exposed**.
* `View.ViewRectMin` is almost always (0,0) after UE4 shifts view rects to the buffer top-left —
  **but read it, do not assume it**.

---

## 6. Build, CI and testing

* **CMake + GitHub Actions, MSVC, x64 only.** No local build. `windows-latest` is now Windows Server
  2025 + VS 2026; MSVC v143 (14.44) is still available via `-T v143,version=14.44`.
* **Build Release `/MD` and link `nvsdk_ngx_d.lib`.** The NGX libs are real static libraries that
  expose C++ std types across the ABI — a mismatched `_ITERATOR_DEBUG_LEVEL` produces link errors.
* Warnings are errors, `/W4`.
* **Precompile every HLSL shader to DXBC at build time. Never call `D3DCompile` at runtime** —
  `d3dcompiler_47` is frequently absent from a Proton prefix, and this is OptiScaler's single most
  common Linux failure.
* **Unit-test every pure function in CI**: fnv1a64 over DXBC; the UE4 velocity decode against the
  measured constants; the jitter path; `ClipToPrevClip` reconstruction; quality-mode selection;
  Halton; mip bias. Build them for Linux too so they run fast.
* Ship PDBs as artifacts (`/Zi /DEBUG /OPT:REF /OPT:ICF`).
* **Logging is the product's second output.** Version banner, ReShade version, vkd3d detection,
  device API, every detected shader hash, the chosen TAA hash *and why*, the raw View CB rows, the
  computed jitter and MV scale, feature flags, NGX result codes by name, every state transition.
  The user pastes this back; it must diagnose without a second round-trip.
### The three test lanes, and what each can actually prove

| Lane | Runs | Proves | Blind to |
|---|---|---|---|
| Unit (Linux, doctest) | every push | pure functions: hashing, matrix math, jitter, velocity decode | anything touching D3D12 |
| **WARP** (Windows CI) | every push | our D3D12 usage is *legal* — the debug layer and GPU-based validation judge it | vkd3d-proton's behaviour; NGX |
| **Real ReShade** (Windows CI) | every push | the add-on loads into the **shipped ReShade 6.8.0.2155** and its events fire; every harness test runs through ReShade's *real* device and command-list proxies | NGX; the game's actual bindings |
| **Hardware** (`tools/run-harness-proton.sh`) | by hand | our D3D12 usage *behaves* on the real driver through real vkd3d-proton | validation: vkd3d implements no `ID3D12InfoQueue`, so every "no validation errors" assertion is vacuous there |

They are complementary and none of them replaces seeing the game render.

* **The hardware lane needs a private display and a private Wine prefix.** Reusing the game's
  `DISPLAY` freezes it instantly: that display is gamescope's nested X server, Wine creates
  windows as soon as `winex11.drv` initialises, gamescope focuses the newest toplevel, and a
  harness that never presents leaves the compositor stuck on the game's last frame. It reads
  exactly like a game hang and is not one. Reusing the game's prefix joins its `wineserver`.
  The script uses `xvfb-run -a` and its own compatdata; keep it that way.
* **Wine gives the harness no console**, so its stdout is lost. The script redirects inside
  Windows via `cmd` and reads the file back.
* **A "no validation errors" assertion is only evidence if the detector can fire.** The harness
  therefore contains a negative control that deliberately binds a descriptor table to a
  root-CBV parameter and requires the debug layer to report
  `D3D12_MESSAGE_ID_SET_DESCRIPTOR_TABLE_INVALID = 708` (verified in `d3d12sdklayers.h:2726`
  and reproduced in our own CI). That is the mistake ReShade's own `state_block` makes.
* **ReShade attaches its add-on path inside the hooked `D3D12CreateDevice`**
  (v6.8.0 `source/d3d12/d3d12.cpp:40`), *not* at swapchain creation or `Present`, and it refuses
  WARP only in its D3D10/D3D11 paths (`d3d10.cpp:159`, `d3d11.cpp:150`) — there is no adapter
  check anywhere in `source/d3d12/`. **So a headless console app with no window and no swapchain
  is enough to load real ReShade and the real add-on.** Dropping the shipped DLL in as
  `d3d12.dll` makes our own `D3D12CreateDevice` return ReShade's proxy, so every harness test
  then runs through its descriptor-heap wrappers and handle conversion — the code §1 flags as
  having no release-build validation.
* The setup exe is a **self-extracting zip** (`setup/MainWindow.xaml.cs:987`).
  `tools/extract_reshade.py` pulls `ReShade64.dll` out of it; .NET's `ZipArchive` refuses the
  prepended PE, Python's `zipfile` does not.
* **Registering is not receiving.** The add-on logs a census on detach, and CI fails if it saw
  zero compute pipelines or zero dispatches — that is the automated form of the §5 warning that
  "wrong ReShade build" is otherwise indistinguishable from "the game binds differently".
* **The ReShade half of the restore is tested by a fake `command_list`**
  (`tests/warp/fake_reshade_command_list.hpp`). The interface is pure abstract, `state_tracking`
  is a plain struct, and `bind_descriptor_tables` is a defaulted forwarder to pure-virtual
  `bind_descriptor_tables2` — so overriding one records both. `get_native()` returns the
  harness's *real* command list, so the native calls execute and are validated for free.
* Give the user one copy-pasteable launch line for bug reports:
  `DXVK_NVAPI_LOG_LEVEL=info PROTON_LOG=1 VKD3D_DEBUG=warn %command%`, plus
  `DXVK_NVAPI_SET_NGX_DEBUG_OPTIONS=DLSSIndicator=1024` for visual proof DLSS is running (**1024,
  not 1** — 1 only works for develop builds).

---

## 7. House rules

* Follow the Superpowers process skills: `brainstorming` before creative work,
  `systematic-debugging` before proposing a fix, `test-driven-development` for anything with a
  testable core, `verification-before-completion` before any claim of doneness.
* No speculative features. Ship DLAA correctly before anything else exists.
* Keep files focused. Luma's 17k-line `core.hpp` is a thing to learn from, not imitate.
* Every constant from §2 gets a **named symbol with a comment citing §2.x**, never a bare literal.
* When observation on the user's machine contradicts §2, **update §2 and
  `docs/STRAY-RENDERING-FACTS.md` in the same change** that reacts to it.
* Never commit `nvngx_dlss.dll` or game assets. Redistribution is permitted only as part of an
  application with material additional functionality, never stand-alone — CI fetches it.
