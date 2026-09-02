# RenoDX "DLSS5 Generic" — a study of the third-party addon, focused on what we can reuse

Research report, 2026-08-31. Subject: the closed-source ReShade add-on **"RenoDX DLSS5
Generic"** (`renodx-dlss5.addon64`), studied from two binaries on this machine plus web
research. The goal is narrow: **what, if anything, this addon lets us borrow for our own DLSS
work in Stray** — especially the Ray-Reconstruction (RR) milestone tracked in
`docs/RESEARCH-RR-GBUFFER.md` — and what to fear when it co-runs with us.

**No code was executed.** These are Windows DLLs; everything below is static analysis
(`strings`, `rabin2 -I/-i/-E/-S/-zz`, raw byte extraction of embedded strings and the PE
version resource) cross-checked against web sources.

Artifacts studied:

| Build | File | Size | Compiled (PE) | FileVersion | Subsystem |
|---|---|---|---|---|---|
| **v4.1.5** | `…/edb7b93b/tmp/renodx-rar/renodx-dlss5.addon64` | 573,440 | Sat Aug 29 20:54:06 2026 | 0.2026.0828.2110 | Windows **GUI** |
| **v2.5** | `~/Downloads/renodx-dlss5-v2.5.addon64` | 391,168 | Fri Aug 28 00:21:56 2026 | 0.2026.0827.2036 | Windows **CUI** |

Provenance labels follow CLAUDE.md §0.5: **HARD** = read verbatim from a primary source (the
binary's own structure/strings, an NVIDIA/SDK header, our own measurements); **SOFT** =
credible secondary source or an inference from strings that the binary structure does not by
itself prove; **UNCONFIRMED** = must be verified before code depends on it. Because the addon is
closed-source, most *behavioural* claims are **SOFT** — read off log/error strings, not off
decompiled control flow — and are labelled as such. Web sources carry their URL inline.

---

## 0. Verdict up front

1. **This addon is NOT a Ray-Reconstruction implementation, and it does NOT lower our Phase-2
   guide-buffer burden.** [HARD] It creates **NGX feature 18** — `NVSDK_NGX_Feature_Reserved18`
   in the public SDK, occupied by NVIDIA's leaked pre-release **"DLSS Neural Rendering"
   (DLSSNR)** runtime — as a **post-process on top of the game's existing DLSS output**. Its
   feature-18 parameter block is **SR-shaped** (Color / Depth / MotionVectors / Output +
   subrects, exactly like DLSS Super-Resolution) and contains **zero** RR guide inputs. There is
   **no `DiffuseAlbedo`, `SpecularAlbedo`, `Normal`, `Roughness`, `DenoiseMode`, or
   `RayReconstruction` string anywhere in either binary.** So it neither creates a DLSSD feature
   nor synthesises the four G-buffer guides `docs/RESEARCH-RR-GBUFFER.md` is about. The
   distributor's "brings RR" wording is loose marketing that conflates this addon with a
   *separate*, per-game RR mod in the same ecosystem (§2, §7.5).

2. **It does prove one thing worth a lot to us: a leaked NGX "neural" feature runs with only the
   SR contract we already satisfy — colour, depth, motion, jitter — no guides.** [HARD from the
   parameter set; SOFT that it produces a good image] Whether that neural feature (feature 18)
   is *useful* to us is a separate, open question (§7.2); but it re-confirms that the
   colour/depth/MV/jitter plumbing our SR path already builds is the whole input contract for at
   least one shipping neural upscaler-class feature.

3. **In Stray specifically this addon is inert.** [HARD, from our own live log + the addon's own
   strings] It only acts when it intercepts a *game-created* DLSS/DLSSD feature. Stray ships no
   DLSS, so the addon reaches its `HOOKS ARMED - NO DLSS CREATE SEEN` state and does nothing to
   the image. That does not make it harmless (§5): its **device-init and per-present hooking
   machinery still runs**.

4. **It is an aggressive process-wide hooker and a credible co-run wedge suspect.** [HARD for the
   mechanism; UNCONFIRMED for causality] Microsoft Detours trampolines over every loaded copy of
   the NGX exports, thread-suspension (`SuspendThread`/`SetThreadContext`) during patch install,
   IAT rewriting of a *second* signed DLL, an optional "contested" Streamline interposer patch,
   and a `LoadLibrary` + `NVSDK_NGX_D3D12_Init_Ext` of the 158 MB `nvngx_dlssnr.dll` **at device
   creation**. Two addons both detouring NGX `CreateFeature`/`EvaluateFeature` in one process is
   the collision to design around (§5).

5. **The embedded sha256 allowlist is a genuine cross-confirmation.** [HARD] The v4.1.5 binary
   hard-codes the reference hash
   `E16BCF15E16E13F527491CDF7845B2FE6521A738D8F7C9C721866A8496E1FC8E`, which is exactly the
   community-attested SHA-256 of the genuine leaked `nvngx_dlssnr.dll` **v310.8.0.0**. Our live
   log shows the user's file hashing to a *different* value (`C114F0F2…`) → the addon logs
   *"custom runtime accepted; untested build"* — i.e. the user is running a **patched** NR DLL
   (the RTX-4090 needs the Ada-patched / "SF" build; the stock signed DLL targets Blackwell), so
   the very code path that says "untested" is the one that ran on the target.

---

## 1. What the two binaries are (facts)

### 1.1 Identity and toolchain [HARD]

* ReShade add-on exports resolve to `NAME = "DLSS 5 Neural Rendering"`, `DESCRIPTION = "Generic
  experimental DLSS Neural Rendering post-pass for DX12 games using NGX or Streamline DLSS"`
  (read from the PE export table pointers at file `0x62fb0`/`0x62fb8`).
* Config section `[RenoDX.DLSS5]`; the internal short name is `DLSS5 Generic`; overlay/banner
  string `RenoDX DLSS5 Generic v4.1.5 (build Aug 29 2026 15:54:01)` — assembled from the
  `RenoDX DLSS5 Generic`, `v4.1.5`, and ` (build Aug 29 2026 15:54:01) loaded (hotkeys: NR
  toggle` fragments.
* Standard ReShade addon surface: imports `ReShadeRegisterAddon`, `…RegisterEvent`,
  `…RegisterOverlay`, `…GetConfigValue`/`SetConfigValue`, `…GetImGuiFunctionTable`.
* Compiled MSVC, x64. `.pdb`/RSDS debug directory absent (stripped path), but **not** symbol-
  stripped otherwise. Version resource present (see table above).
* Imports beyond ReShade/CRT reveal the whole machinery: `USER32` `GetAsyncKeyState` /
  `MapVirtualKeyA` / `GetKeyNameTextA` (the F5/F6 hotkeys); `bcrypt` `BCryptOpenAlgorithmProvider`
  /`…CreateHash`/`…HashData`/`…FinishHash` (**the SHA-256 allowlist**, v4.1.5 only); `KERNEL32`
  `CreateToolhelp32Snapshot` / `Module32First/NextW` / `K32EnumProcessModules` (the "NGX module
  scan"); `SuspendThread`/`ResumeThread`/`Get/SetThreadContext`/`VirtualProtect`/
  `FlushInstructionCache` (Detours hook install); `dbghelp` `SymInitialize`… (crash-handler
  symbolication). PE sections `.detourc` / `.detourd` / `.fptable` are Microsoft Detours' own
  section names — **Detours is the hooking library**, in both builds.

### 1.2 What changed v2.5 → v4.1.5 [HARD unless noted]

* **v2.5 has no SHA-256 verification at all** — no `bcrypt` import, no embedded reference hash.
  The allowlist ("signed runtime sha256 … reference match / custom runtime accepted") is **new
  in v4.1.5**.
* v4.1.5 **adds PQ/ST.2084 transfer** to the colour codec (`LinearToPQ`/`PQToLinear`, and an
  `HdrMode == 2` branch for "PQ-encoded HDR (R10G10B10A2)") and an **OkLab hue-preservation**
  step (`ToOkLab`/`FromOkLab`/`HueOkLab`/`ClampAp1`). v2.5's codec is sRGB/soft-clip only, no PQ,
  no OkLab.
* v4.1.5 hardens `UpgradeToneMap` against a **black-frame failure mode** the v2.5 shader did not
  guard: *"The DLSSNR proxy output can be degenerate/empty for some HDR inputs … Preserve the
  original scene instead. `if (neural_y <= 1e-5) return original;`"* — SOFT evidence (a comment)
  that the neural DLL frequently returns empty output.
* v4.1.5 exposes `EnableHooks` (0/1/2) as a config knob; v2.5 has no `EnableHooks` string.
* Subsystem flipped CUI→GUI (cosmetic; neither is a console app in practice).
* The core design — Detours over NGX, feature-18 create/evaluate, the `[t0 Original / t1 Proxy /
  t2 Neural / t3 OutputOriginal → u0 Output]` codec, Streamline fallback hooks, the `DLSSNR.*`
  parameter namespace, the self-reported runtime version **`310.8.0`** — is **identical across
  both builds**. v4.1.5 is a hardening/HDR-correctness iteration, not a redesign.

---

## 2. Q1 — The "RR" path: how it brings "DLSS 5" to a DLSS-only game (and why it is not RR)

**This is the question that mattered most, and the answer is a clean negative for the RR
hypothesis.**

### 2.1 It creates NGX feature 18, not DLSSD [HARD]

The addon enumerates NGX features by name for logging — the string table literally contains the
list `DLSS/DLAA`, `DLSSD/RR`, `DLSSG/FrameGeneration`, `DeepDVC`, **`DLSSNR/reserved-18`**,
`other NGX feature`. Everything it *creates and evaluates itself* is logged as **"feature 18"**:
`feature 18 created via …`, `feature 18 create failed with 0x`, `feature 18 evaluate failed with
0x`, `inline feature 18 evaluation succeeded (count=`.

Feature 18 is `NVSDK_NGX_Feature_Reserved18` in the public NGX header
(`NVIDIA/DLSS/include/nvsdk_ngx_defs.h`: `…FrameGeneration=11, DeepDVC=12, RayReconstruction=13,
Reserved14…Reserved18=18`) [HARD, web:
`raw.githubusercontent.com/NVIDIA/DLSS/main/include/nvsdk_ngx_defs.h`]. The leaked pre-release
**DLSSNR** runtime occupies that reserved slot. So the addon is not touching DLSS SR (feature 1)
or DLSSD/RR (feature 13) create paths at all — it stands up the reserved neural feature.

### 2.2 The feature-18 input contract is SR-shaped — no guides [HARD]

Every parameter the addon sets on its feature-18 block is in the `DLSSNR.*` namespace and is a
1:1 analogue of the standard NGX Super-Resolution parameters:

```
DLSSNR.Color   DLSSNR.Depth   DLSSNR.MVec   DLSSNR.Output
DLSSNR.{Color,Depth,MVec,Output}SubrectBaseX/Y/Width/Height
DLSSNR.MVecScaleX/Y   DLSSNR.DepthInverted   DLSSNR.Reset
DLSSNR.Intensity   DLSSNR.LocalToneStrength   DLSSNR.LocalStructureStrength
DLSSNR.SkinStructureStrength   DLSSNR.UseAutoMask   DLSSNR.Style
DLSSNR.UICorrection   DLSSNR.Hint.Render.Preset   DLSSNR.Width/Height/Scale
```

There is **no albedo, normal, roughness, specular, hit-distance, or denoise-mode parameter** —
confirmed by exhaustive grep of both binaries (all of `Albedo|Reflected|Emissive|GBuffer|
Disocclusion|HitDistance|DenoiseMode|Denoise|Roughness|SpecularMotion|WorldToView|ViewToClip`
return **zero** hits). Web corroboration: RenoDX contributor *speedlemur* states the neural path
"seems to use the same inputs already available to regular DLSS upscaling", and NVIDIA describes
DLSS 5 as taking "a rendered 2D frame and motion vectors as input, while materials are *inferred
from that frame*" [SOFT/HARD, web: videocardz.com DLSS-5 coverage]. **Materials are inferred by
the network, not supplied as guides.** That is the architectural reason it needs none.

### 2.2.1 Which of those names the RUNTIME actually knows — and the one that bit us [HARD]

Measured 2026-08-31 by exact null-terminated string search over `nvngx_dlssnr.dll` (DLSSNR
310.8.0, the `dlssnr-remix` copy, md5 `eea91faf…`), cross-referenced against `strings` over
`renodx-dlss5.addon64`. **RenoDX's parameter set is a strict superset of ours: it writes 14
names we never did, and we write none it does not.** But only half of those 14 are real:

| RenoDX writes | In the 310.8.0 runtime? |
|---|---|
| `DLSSNR.Enabled` | **yes** |
| `DLSSNR.Hint.Render.Preset` | **yes** |
| `DLSSNR.ScalingRatio` | **yes** |
| `DLSSNR.SkinStructureStrength` | **yes** |
| `DLSSNR.Style` | **yes** |
| `DLSSNR.UICorrection` | **yes** |
| `DLSSNR.UseAutoMask` | **yes** |
| `DLSSNR.Scale` | **no** |
| `DLSSNR.InputWidth` / `InputHeight` | **no** |
| `DLSSNR.OutputWidth` / `OutputHeight` | **no** |
| `DLSSNR.Output.Width` / `Output.Height` | **no** |
| `DLSSNR.Upscaling` | **no** |

RenoDX writes the absent names too — defensive coverage across snippet builds, harmless because
an unknown key is simply never read. **That is exactly why the bug was invisible on our side:**
we set `DLSSNR.Scale`, which does not exist in this runtime, so **every scaling ratio we ever
sent was silently discarded**. §2.2's listing of `DLSSNR.Width/Height/Scale` is RenoDX's name
set, not the runtime's — do not read a parameter list off a *caller* and assume the callee
honours it.

**Consequence, and the general rule:** an NGX parameter block is an untyped string→value map
with no validation and no error for an unknown key, so a typo or a stale name is
indistinguishable from a value that was accepted and ignored. Any name we write must be
confirmed present in the runtime binary first.

#### The TYPE is as unvalidated as the name — and ours were wrong [SOFT, 2026-09-02]

The name audit above answers "does the runtime know this key". It does **not** answer "does the
runtime read it as the type we wrote", and that is a second, independent way to write a value
that is silently never used.

`NVSDK_NGX_Parameter` declares **separate virtual overloads** for `int` and `unsigned int`
(`third_party/ngx/include/nvsdk_ngx_params.h:58-59`), so `Set(name, 0u)` and `Set(name, 0)` are
different vtable slots storing under different type tags. The block has no validation and returns
nothing, so a type mismatch presents exactly like the `DLSSNR.Scale` bug: a value that looks set
and is never read.

**We wrote every subrect and extent through the UNSIGNED overload.** `DLSSNR.{Color,Depth,MVec,
Output}Subrect{BaseX,BaseY,Width,Height}` took `0u` and `std::uint32_t` extents;
`DLSSNR.Width`/`Height` took `std::uint32_t`. Sixteen rect parameters plus two extents, all as
`unsigned int`.

**The audit says signed.** A sibling port of this integration states, from its own disassembly of
the snippet: *"the subrect type question resolves in favour of what this port already does — the
snippet Gets subrects as int, which is what is written here, whereas that fork writes unsigned
and absorbs the mismatch in a type-agnostic parameter bag of its own rather than the driver's"*
(RemixProjGroup/dxvk-remix, branch `dlss-nr`, commit `2df9c812`). "That fork" is a DXVK
integration whose parameter bag is its own code, so the mismatch never reaches NGX there. **Ours
is the real `NVSDK_NGX_Parameter`, so ours would.**

**Status SOFT**, deliberately: we have not disassembled `nvngx_dlssnr.dll` ourselves for this, and
one reading of someone else's disassembly is not the same as reading the binary. But the change
costs nothing, it is the only available reading of the callee, and being wrong in the signed
direction costs at most a rect the runtime ignores — which is what the unsigned form may already
be doing.

**Fixed 2026-09-02.** The rect and extent block now comes from `src/core/nr_params.{hpp,cpp}`,
which carries the names AND the types as data, and `tests/test_nr_params.cpp` pins that every
subrect and extent is `Type::i32`, that the name set is exactly the runtime's, that
`DLSSNR.Scale` can never come back, and that a `uint32` extent above `INT32_MAX` is clamped rather
than wrapped into a negative the runtime would take at face value.

**Not audited, and therefore unchanged:** `DLSSNR.DepthInverted`, `DLSSNR.Enabled`,
`DLSSNR.UseAutoMask`, `DLSSNR.UICorrection` and `DLSSNR.Hint.Render.Preset` are still written as
`unsigned int`, and `DLSSNR.Reset` as `int`. The current configuration produces a correct image
with those types, so they are left alone until someone reads the binary; **UNCONFIRMED** either
way.

The mapping from RenoDX's shipped `[RenoDX.DLSS5]` ini keys to runtime parameters:

| ini key | value | runtime parameter |
|---|---|---|
| `NRIntensity` | 1.05 | `DLSSNR.Intensity` |
| `NRLocalTone` | 1.74 | `DLSSNR.LocalToneStrength` |
| `NRSkinStructure` | 1.33 | `DLSSNR.SkinStructureStrength` |
| `NRPreset` | 1 | `DLSSNR.Hint.Render.Preset` |
| `NRAutoMask` | 1 | `DLSSNR.UseAutoMask` |
| `NRUICorrection` | 1 | `DLSSNR.UICorrection` |
| `NREnableUpscaling` | 0 | (`DLSSNR.Upscaling` is absent; `ScalingRatio` carries it) |
| `NRPaperWhiteScale` | 1.605 | **none — RenoDX-side pre-processing** |
| `NRDepthMode` | 2 | **none — RenoDX-side** |
| `NeuralUplift` | 1 | **none — RenoDX-side** |

The last three are the interesting ones: they have no NGX parameter at all, so RenoDX must apply
them to the data before handing it over. A **paper-white scale in particular means they
normalise input luminance**, which is a live hypothesis for our near-black neural output
(validated at `max luminance 0.002709`) and the red-noise image.

### 2.3 It clones the game's own DLSS contract at the detour [HARD for the read; SOFT for flow]

The addon reads the standard NGX SR parameter keys straight off the game's intercepted
create/evaluate parameter block — the string table holds the exact NVSDK names it queries:
`Color`, `Depth`, `MotionVectors`, `Output`, `Jitter.Offset.X/Y`, `MV.Scale.X/Y`, `Reset`,
`PerfQualityValue`, `Width/Height/OutWidth/OutHeight`, and the namespaced
`DLSS.Render.Subrect.Dimensions.Width/Height`, `DLSS.Input.{Color,Depth,MV}.Subrect.Base.X/Y`,
`DLSS.Output.Subrect.Base.X/Y`, `DLSS.Pre.Exposure`, `DLSS.Exposure.Scale`,
`DLSS.Feature.Create.Flags`. It then re-emits an identically-shaped block under `DLSSNR.*` for
feature 18. If it missed the create (game created DLSS before hooks installed) it
"register[s] lazily from evaluate contract".

So the mechanism is: **let the game create/drive DLSS normally, snapshot that SR contract, and
run feature 18 as an extra evaluate reusing the same colour/depth/MV/output resources**, then
blend the neural result over the game's DLSS output with the colour codec (§4). It does **not**
rewrite the game's SR create into a DLSSD create, and it does **not** synthesise guides — both
hypotheses in the task are **disproven by the binary**.

### 2.4 If it cannot find a guide contract, it degrades, it does not fabricate [SOFT]

The error strings show the failure philosophy: *"NR skipped: the game's NGX contract has no guide
(input) dimensions … otherwise this title is not supported and NR stays off"*, *"NR upscaling
fell back to native: the signed runtime rejected the …"*, *"the game DLSS output was retained
(the image is unchanged)"*. When inputs do not fit, it **passes the original frame through** —
never invents buffers. (Note "guide" in this addon's vocabulary means colour/depth/MV, **not**
the RR G-buffer guides.)

### 2.5 The real RR mod is a *different* thing in the same ecosystem [HARD, web]

The "brings RR to DLSS-only games" claim traces to **per-game** RenoDX mods, exemplified by
SpeedLemur's *"Control — RR and DLSS 5 — RenoDX"* (nexusmods.com/control/mods/140), which adds a
denoiser selector (Game / DLSS SR / **DLSS Ray Reconstruction**) and *does* replace the SR
evaluate with a real DLSSD feature — but it requires **SSAO ON** in-game and user-supplied
`nvngx_dlssd.dll`, and **no public writeup explains how it sources RR's guide buffers** (the SSAO
requirement is suggestive but undocumented) [UNCONFIRMED mechanism]. That mod is out of scope
here — the binary we hold (`DLSS5 Generic`) is the feature-18 NR post-pass, not the RR path.

**Consequence for us:** `docs/RESEARCH-RR-GBUFFER.md` §3's conclusion — "nobody has done
guide-buffer scavenging from a raster game" — **stands**. This addon is not a counter-example and
gives us no guide-synthesis recipe. See §7 for what it *does* give us.

---

## 3. Q2 — The signed NR runtime (`nvngx_dlssnr.dll`, "the signed snippet")

### 3.1 What it is [HARD, web + binary cross-confirmed]

`nvngx_dlssnr.dll` is a **genuine, NVIDIA-Authenticode-signed, leaked pre-release NGX runtime for
DLSS 5 "Neural Rendering" (DLSSNR), version 310.8.0.0, ~158 MB** (≈3× DLSS-4 SR). It shipped by
mistake in **NBA 2K27's PC early-access build** (~Aug 26 2026) and was extracted by the modding
community [web: videocardz.com "DLSS 5 Neural Rendering DLL found in NBA 2K27"]. The addon's own
strings confirm the version: `signed DLSSNR 310.8.0 D3D12 runtime initialized`, `DLSSNR
v310.8.0`. Driver 610.47 (May 2026) already carried hidden "DLSS-NR" / "DLSS-NR Streamline" /
"DLSS-NR Presets" profile entries [web: videocardz.com], so "NR" is a real NVIDIA-internal
feature name, not a modder coinage.

**Cross-confirmation (HARD):** the reference SHA-256 hard-coded in the v4.1.5 binary,
`E16BCF15E16E13F527491CDF7845B2FE6521A738D8F7C9C721866A8496E1FC8E`, is exactly the
community-attested hash of the genuine v310.8.0.0 file [web:
github.com/kayle2203/dlssnr-signature-repair]. So the addon's allowlist entry *is* the real
DLL's fingerprint.

### 3.2 How the addon loads and validates it [HARD for the surface; SOFT for order]

* **The DLL is not embedded.** It must be placed by the user in the addon/game folder; strings:
  `nvngx_dlssnr.dll was not found in …`, *"Place NVIDIA's signed nvngx_dlssnr.dll in that folder
  and restart the game; NR stays off until then."*
* **Pre-loaded at device init:** `signed NR runtime (nvngx_dlssnr.dll) pre-loaded at device
  init` (with a lazy-retry fallback: `signed NR runtime pre-load failed; will retry lazily on
  first evaluate`). Loaded via `LoadLibraryW`/`LoadLibraryExW`.
* **Validated by SHA-256 (v4.1.5):** `signed runtime sha256 <hex>` → either ` (reference match)`
  or ` (custom runtime accepted; untested build, NR failures may be specific to it)`. Computed
  with `bcrypt` (see §1.1). **On the target the user's DLL took the "custom runtime accepted"
  branch** — our live log shows `C114F0F2…`, not the reference `E16BCF15…`. That is expected: the
  RTX-4090 requires an **Ada-patched** NR DLL ("Uncle Burrito"'s CUDA-swap, or ShortFuse's
  "310.8.SF" build) because the stock signed DLL targets Blackwell (RTX 50); any such patch
  **breaks NVIDIA's signature** and changes the hash [web: tomshardware, wccftech, RHI notes].
* **Initialised directly through NGX's extended init:** it resolves and calls
  `NVSDK_NGX_D3D12_Init_Ext`, `…_AllocateParameters`, `…_DestroyParameters`, `…_CreateFeature`,
  `…_EvaluateFeature`, `…_ReleaseFeature`, `…_Shutdown1` (all present as strings). Error strings:
  `direct Init_Ext failed with 0x…`, `NGX/Streamline runtime has no complete parameter or
  feature export set`.
* **It patches the signed DLL's import table:** `failed to make signed-feature IAT writable;
  security software may be protecting nvngx_dlssnr.dll` and `signed feature has no
  GetModuleFileNameW import; the nvngx_dlssnr.dll in the addon folder is not a valid signed NGX
  runtime`. [SOFT] It appears to interpose the NR DLL's `GetModuleFileNameW` import — plausibly so
  the snippet resolves its model/asset paths relative to the addon rather than the host process.
  This is a second, independent code-patch of another module at load time (a wedge-risk data
  point, §5).

### 3.3 "The NGX core" vs "the signed snippet" [SOFT]

Feature-18 create is logged as `feature 18 created via the NGX core` **or** `… via the signed
snippet`. So the addon has two backends: create the reserved feature through the process's normal
`_nvngx.dll` core if that already exposes it, else fall back to the standalone signed
`nvngx_dlssnr.dll` it pre-loaded. "The signed snippet" = that standalone NR runtime acting as a
self-contained NGX feature provider.

### 3.4 Source / distribution of the DLL and the addon [HARD, web]

* The **DLL** is community-redistributed (RenoDX Discord pins, then Nexus mirrors, RHI
  auto-download, Russian mirrors). It is a leak; NVIDIA has not released it.
* The **addon is closed-source.** No public repository, README, or changelog for it exists; the
  newest publicly-mirrored binary is **v2.5** (via `yumlevi/renodx-dlss-installer`, matching our
  391,168-byte file). v4.1.5 lives inside the RenoDX Discord and is quoted only via users pasting
  its overlay text on Nexus. Companion projects state plainly it is "closed-source,
  community-distributed" and "from its own author; this project does not include it" [web:
  github.com/jlrouzies-fr/DLSS5-Feeder, github.com/NIGos/dlss5-dx11-bridge].
* **Relationship to clshortfuse/renodx:** it is built on the RenoDX/ReShade framework by RenoDX-
  community members but is **absent from the public RenoDX repo** (which is HDR/tone-mapping
  tooling; zero dlss5/dlssnr/NGX references). clshortfuse ("ShortFuse") is separately credited
  with a modified "310.8.SF" NR DLL that extends GPU support to RTX 20–50; the individual author
  of the "Generic" addon binary is not publicly attributed [web: videocardz, RHI releases,
  nexusmods/control/140]. Treat "RenoDX DLSS5" as *RenoDX-community, ReShade-framework, closed
  binary* — not an official module of the upstream project.

---

## 4. Q3 — The evaluate-detour colour processing (PQ / paper-white)

### 4.1 What runs per evaluate [HARD for the shader; SOFT for the sequencing]

After the neural feature-18 evaluate, the addon runs **one compute pass ("codec")** that blends
three images into the output. Both builds embed the full HLSL source (compiled at runtime via
`D3DCompile`, target `cs_5_1`, entry `main`, `numthreads(16,16,1)`; it probes
`d3dcompiler_47/46/43`). Bindings:

```
t0 Original        — the game's own DLSS SR output (already upscaled, may be HDR)
t1 Proxy           — an SDR proxy the addon fed the NR network
t2 Neural          — the raw feature-18 (DLSSNR) output
t3 OutputOriginal  — the pre-existing output contents (for alpha restore)
u0 Output          — final image written back into the game's DLSS output resource
cbuffer b0: Size, SourceSize, SourceBase, ProxySize, PaperWhiteScale,
            TransferStrength, ColorStrength, HdrMode, Padding
```

The pipeline (from the shader body and comments):

1. **The NR network only understands SDR.** The addon builds an **SDR "Proxy"** from the HDR
   scene (linearise per `HdrMode`, soft-clip highlights, sRGB-encode), feeds *that* to feature
   18, and gets back "Neural".
2. **`UpgradeToneMap(original, proxy, neural)`** transfers the network's edit back onto the real
   (possibly HDR) frame by a **luminance-ratio** method with **OkLab hue preservation**
   (`HueOkLab` matches the neural chroma to the scaled result, clamped to AP1 gamut), then
   `lerp(original, scaled, TransferStrength)`.
3. **HdrMode branches:** `0` = SDR (already display-referred), `1` = scRGB/linear HDR, `2` =
   PQ/ST.2084 HDR in `R10G10B10A2` (uses `LinearToPQ`/`PQToLinear`, v4.1.5 only). `PaperWhiteScale`
   maps the scene's paper-white so the SDR proxy sits in a sane range; on the target it logs
   `paper_white=1.605000` (i.e. the `[RenoDX.DLSS5] NRPaperWhiteScale=0.05` × an internal factor —
   value read straight from our live log).
4. **`SampleNeural` (v2.5 comment, retained behaviour):** *"DLSSNR 310.8 writes the neural answer
   at its active network resolution even when the Output resource is larger … Sample that
   populated region explicitly"* — the network output can be **smaller than the output rect**, so
   the codec bilinearly upsamples from `ProxySize`. A concrete, useful fact about how the leaked
   runtime behaves.
5. **Black-frame guard (v4.1.5):** if `neural_y <= 1e-5` (degenerate/empty neural output, "common
   for … a PQ buffer linearized through sRGB") it returns the original scene rather than collapse
   to black.

### 4.2 Relevance to our exposure/sparkle work [SOFT]

* **Low direct reuse.** This codec exists to marry a *second* neural pass' SDR output back into an
  HDR frame — a problem we do not have; our DLSS SR output is the final image, pre-exposed, and we
  write it straight to `u0` (CLAUDE.md §5 "write our output back still pre-exposed").
* **Two transferable techniques, if a future need arises:** (a) the **OkLab hue-preservation +
  luminance-ratio transfer** (`ToOkLab`/`HueOkLab`) is a clean, self-contained way to change a
  pixel's luma/detail while holding its hue and chroma — relevant if we ever post-correct DLSS
  output for a colour cast; (b) the **`SampleNeural` insight** that an NGX neural feature may
  populate only a sub-rect of its output resource is a defensive check worth stealing for any
  neural evaluate. Both are HLSL we can lift verbatim (it is embedded plaintext in the binary and
  is standard published math — OkLab is Björn Ottosson's, PQ is ST.2084).
* Nothing here informs our **sparkle/temporal-stability** investigation (CLAUDE.md §"Temporal
  stability"): the codec is a per-pixel colour transform with no temporal component.

---

## 5. Q4 — Interop safety: what it does that could touch us or wedge vkd3d

The task flags an **unrecoverable GPU wedge (Xid 109, driver unloadable, host reboot)** correlated
with co-running this addon and ours on the RTX 4090 under vkd3d-proton, plus a first-session death
at device init. **Causality is UNCONFIRMED** (Xid 109 CTX-SWITCH-TIMEOUT is already in our own
environment log, CLAUDE.md §2.10, and gamescope/driver instability is independent). But the
binary shows several device-init-time and per-frame behaviours that are legitimate wedge suspects
and that **collide with our own NGX use**. Concrete flags for a co-run protocol:

### 5.1 It detours the NGX exports process-wide — the direct collision [HARD]

`D3D12 NGX hooks installed across N module copy(ies)`; it scans **every loaded module** for NGX
copies (`NGX module scan (loaded copies)`, `detoured NGX module copy [0] …\_nvngx.dll (core)`) via
Toolhelp/`EnumProcessModules` and Detours-patches `NVSDK_NGX_D3D12_CreateFeature`,
`…EvaluateFeature`, `…ReleaseFeature` (and `…EvaluateFeature_C`, which is absent from wine's NGX
shim — handled non-fatally, matching our live log). It re-scans **every present** ("the hook
reattaches every present"). **If our add-on calls NGX in the same process, its detour sits on our
calls too** — its "NGX/Streamline deduplication armed" logic will see and count our
evaluates (`skipping NR on NGX evaluate: feature (…) is not DLSS/DLSSD`). Reentrancy between two
NGX hookers sharing one `_nvngx.dll` in a wine process is the single most likely interaction
hazard. **Protocol: never run both with NGX active at once.** If ours is the one under test,
the other must be at `EnableHooks=0` (its "SAFE MODE (all hooks off)") — and even then see §5.4.

### 5.2 Thread suspension + code patching at hook install [HARD mechanism]

Detours installs trampolines by `SuspendThread`-ing other threads, `Get/SetThreadContext`,
`VirtualProtect`-ing code pages RW, writing, and `FlushInstructionCache`. Under wine this is a
known-delicate path. It happens at first hook install and again as new NGX module copies appear.
A process-wide thread freeze racing vkd3d's own submission threads is a plausible (unproven)
contributor to a boot-time death.

### 5.3 It `LoadLibrary`s and `Init_Ext`s a 158 MB NGX runtime at device creation [HARD]

`signed NR runtime (nvngx_dlssnr.dll) pre-loaded at device init` → `LoadLibraryW` of a 158 MB DLL
plus `NVSDK_NGX_D3D12_Init_Ext` **on the game's D3D12 device, at creation time**. That is a second
full NGX initialisation on the same device our add-on also initialises NGX on. Two NGX inits /
two feature-creating clients on one vkd3d `ID3D12Device`, through DXVK-NVAPI's cubin path, is
untested territory (our own SR path is already the delicate part per CLAUDE.md §1). **This is the
"first session died at device init" candidate.**

### 5.4 It rewrites a second DLL's IAT [HARD]

The `make signed-feature IAT writable` / `GetModuleFileNameW import` strings show it patches the
**NR DLL's** import table (§3.2). More `VirtualProtect`+write of code/headers belonging to a
module we do not control.

### 5.5 Optional Streamline interposer patch — self-described as dangerous [HARD]

`EnableHooks=1` additionally patches `sl.interposer.dll`/`sl.common.dll`; its own warning:
*"WARNING: Streamline hooks ON (EnableHooks=1). **Contested patch site** - if the game crashes at
boot, set EnableHooks=2."* Stray uses neither Streamline nor DLSS, so this path should be inert
here, but if the user ever set `EnableHooks=1` it is an explicit boot-crash risk. **Protocol: if
co-run is ever attempted, force `EnableHooks=0`.**

### 5.6 In Stray it does nothing useful anyway [HARD]

Our live log reaches `HOOKS ARMED - NO DLSS CREATE SEEN` — the game never creates a DLSS feature,
so feature 18 is never created or evaluated, and the codec never runs. The **only** things that
execute in Stray are the hook installs (§5.1–5.2), the NR-DLL pre-load (§5.3), and the per-present
re-scan. **There is zero image benefit and non-zero device-init risk. Recommendation: do not
co-run it with our add-on; if the user wants it present, `EnableHooks=0`.** Better: run them in
separate sessions.

---

## 6. Q5 — Web: source, upstream relationship, Linux/Proton reports

Full sourcing is in §2.5 and §3.4; the web-specific residue:

* **Distribution:** RenoDX Discord (pinned) → mirrored by RHI (RankFTW, auto-downloads &
  silently updates it; RHI 2.5.0 renamed it "DLSS5 Tool"), `yumlevi/renodx-dlss-installer`
  (hosts **v2.5**, our exact file), Nexus site-mods 2224/2228/2241 and game mods, plus Russian
  mirrors. [HARD]
* **Source availability:** none for the addon (closed). Companion/adjacent projects *are* open
  and document its externals: `jlrouzies-fr/DLSS5-Feeder` (wraps it, SEH-guards it, works around
  its "STANDBY latch"), `NIGos/dlss5-dx11-bridge` (documents the two hooked NGX entry points),
  `markitzeroo/dltb-dlss5-fix`, `kayle2203/dlssnr-signature-repair` (the reference hash),
  `rakanki911/DLSS5-Swapper`, `faisalkindi/DLSS5oneclick`. [HARD]
* **DLSS terminology (mid-2026):** DLSS **4.5** (CES Jan 2026) is the "2nd-gen transformer"
  release; DLSS **5** (announced GTC Mar 2026, launching fall 2026) is the neural-rendering model
  that "infuses pixels with photoreal lighting and materials" and "infers materials from the
  frame" — RTX-50-class hardware. "Preset" still = the model-preset letters (SR: Default/J/K/L/M;
  RR: D/E/F); the addon's `NRPreset` is currently **inert** ("the shipped NVIDIA DLL contains a
  single network"). [HARD, web: nvidia.com, videocardz]
* **Linux/Proton/vkd3d:** **complete dead end.** No report anywhere (reddit, ProtonDB, GitHub,
  gamingonlinux) of this addon or `nvngx_dlssnr.dll` running — or crashing — under Proton. No
  Xid/wedge report tied to it. The nearest thread is the generic pre-6.8.0 "ReShade-addon + DLSS
  under Proton crashes" issue our project already documents. Given the addon is 4 days old and
  needs an Ada-patched DLL + recent driver, the absence is unsurprising but means **we have no
  external Linux data point — our own session is the only one.** [negative result]
* **Ecosystem caution:** closed binary re-hosted by ≥5 unaffiliated parties within 96 hours; the
  (unrelated) DLSS-Swapper author has issued a malware warning about user-submitted DLLs and fake
  download sites. Only v310.8.0.0's hash is publicly attested; every RTX-40-capable variant fails
  NVIDIA signature validation by construction. [HARD, web: techspot]

---

## 7. What we take from this

Concrete items for our roadmap. Nothing here changes the SR (v0.1/v0.2) plan; it bears on the RR
milestone (`docs/RESEARCH-RR-GBUFFER.md`) and on co-run hygiene.

### 7.1 RR Phase 2 — the guide-buffer burden is NOT lowered. [HARD]
The hoped-for shortcut ("if it creates DLSSD with null/synthetic guides and it works, our Phase-2
burden drops") **does not exist in this binary.** The addon creates feature 18 (DLSSNR), not
DLSSD (feature 13), and feeds it only colour/depth/MV — the SR contract, no guides. It is no
evidence that DLSSD tolerates null/synthetic guides. `RESEARCH-RR-GBUFFER.md`'s plan — writer-side
G-buffer identification at the TAA hook, a `gbuffer_resolve` pass emitting the four guides — remains
the path, and its §3 claim that guide-scavenging from a raster game is unprecedented **still
holds.** Do not deprioritise the guide work on the strength of this addon.

### 7.2 A possible *alternative* to RR worth a cheap spike — feature 18 (DLSSNR). [SOFT]
The addon demonstrates a **guide-free neural quality feature** that runs on exactly the inputs we
already produce. If the goal is "better-looking DLSS output" rather than "Ray Reconstruction
specifically", feature 18 is a candidate that **sidesteps the entire guide-scavenging problem**.
Caveats before spending anything on it: it is a **leaked, unsigned-on-our-GPU, closed runtime**
(redistribution and stability both problematic); it needs an **Ada-patched DLL** on the 4090; it
is reported to cost ~50% performance and leak VRAM on Windows; and it is an SDR-only network
needing the HDR codec dance (§4). Recommendation: **note it as a known alternative, do not adopt
it** — it fails our "loud failure over quiet wrong image" and "never ship a leak" bars. Its value
to us is purely as proof that the reserved neural slot exists and takes the SR contract.

### 7.3 Reusable code, if ever needed. [HARD — it is embedded plaintext]
* **OkLab hue-preservation transfer** (`ToOkLab`/`FromOkLab`/`HueOkLab`/`ClampAp1`) — verbatim-
  liftable HLSL for changing luma/detail while holding hue+chroma. A tool for a future colour-cast
  correction on DLSS output (the §"native-device rule has a trap" colour-cast failure mode).
* **`SampleNeural` defensive upsample** — the fact that an NGX neural feature may write only a
  sub-rect (`ProxySize`) of its output resource; bilinearly cover the rest. Cheap insurance for
  any neural evaluate.
* **The colour codec's ST.2084/scRGB/sRGB `HdrMode` switch** — a compact, correct reference for
  Stray's `R10G10B10A2` HDR output format (CLAUDE.md §2.1) if we ever need to read/write it
  linearly.

These are standard published math (OkLab, PQ); reproduce from spec with a citing comment per
CLAUDE.md §7 — do not copy the leaked binary's text into our tree.

### 7.4 Co-run protocol — treat it as hostile to our NGX path. [HARD mechanism]
1. **Never have both add-ons NGX-active in the same process.** Two Detours hookers over one
   `_nvngx.dll` under wine is the top interaction risk (§5.1).
2. When testing ours, the other must be **removed** or at **`EnableHooks=0`** (SAFE MODE); never
   `EnableHooks=1` (its own boot-crash warning, §5.5).
3. Its device-init NR-DLL pre-load + second `Init_Ext` on our device (§5.3) is the leading
   suspect for the "died at device init" session — a reason to keep it out entirely during SR
   bring-up, not merely disabled.
4. Add a line to our bug-report guidance: when a wedge is reported, **ask whether any DLSS5 /
   RenoDX / RHI addon is present in the game folder**, since RHI installs and silently updates it.

### 7.5 Documentation hygiene.
The "RenoDX DLSS5" name spans two different things — the **feature-18 NR post-pass** (the binary we
hold) and **per-game RR mods** (SpeedLemur's DLSSD builds). Keep them distinct in any future note;
conflating them is what makes "it already does RR" look true when it is not.

### 7.6 NR follow-up (only if §7.2 is ever revisited).
Open questions a future spike would answer: does feature 18 evaluate at all under vkd3d-proton +
DXVK-NVAPI (the cubin path), or does `Init_Ext`/create fail as `0xBAD0000x` the way the signature
check does on a patched DLL? What does the network do to a *non-DLSS* frame (could we feed it our
own SR output as "Original+Proxy")? These are unfunded; record, do not pursue, until SR is
confirmed correct on target per staging (CLAUDE.md §3).

---

## 8. Method and evidence log

* Tools: `file`, `strings -n 6`, `rabin2 -I` (headers), `-iq` (imports), `-E` (exports), `-S`
  (sections), `-zz` (all strings incl. UTF-16), plus a Python raw-byte extractor for the embedded
  HLSL (`0x67b80`, 4287 B) and the PE `VS_VERSION_INFO` block. No execution.
* Key structural facts (HARD): Detours (`.detourc`/`.detourd`/`.fptable` + `vtable::Hook` strings);
  `bcrypt` SHA-256 import in v4.1.5 only; embedded reference hash `E16BCF15…FC8E`; the `DLSSNR.*`
  parameter namespace with no guide parameters; feature-18 create/evaluate strings; the embedded
  codec HLSL (both builds); `Init_Ext`/`CreateFeature`/`EvaluateFeature`/`ReleaseFeature` NGX
  imports; Toolhelp/`EnumProcessModules` module scan; `SuspendThread`/`SetThreadContext`/
  `VirtualProtect` hook machinery; `USER32 GetAsyncKeyState` hotkeys.
* Exhaustive **negative** greps (both binaries): `Albedo|SpecularAlbedo|Reflected|Emissive|
  GBuffer|Disocclusion|HitDistance|DenoiseMode|Denoise|Roughness|SpecularMotion|WorldToView|
  ViewToClip|RayReconstruction` → **0 hits**. `wine|vkd3d|proton|linux|Xid` → **0 hits** (it has
  no Proton awareness). No embedded `.exe` name spoof; the only "Control" strings refer to the
  game Control's colour-transfer parity, not process spoofing.
* Web sourcing inline in §2–§6 (videocardz, tomshardware, wccftech, nvidia.com, nexusmods, the
  RHI/DLSS5-Feeder/dlss5-dx11-bridge/dlssnr-signature-repair GitHub repos, techspot). The
  addon's self-reported `v4.1.5` / `DLSSNR v310.8.0` overlay text is independently attested by
  users quoting it on nexusmods.com/site/mods/2224.
