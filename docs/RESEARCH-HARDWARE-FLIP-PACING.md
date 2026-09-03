# Hardware frame pacing for DLSS Frame Generation on this stack — can we get it?

> **VERDICT, up front.** **The "hardware" form is blocked by the GPU and by gamescope. But a
> driver-side pacing API for exactly this purpose IS live on this exact card and driver, nobody
> in our chain uses it, and one open-source project has already built the thing we would build.
> That is roughly a weekend of work whose payoff is unmeasured.**
>
> Three facts decide it, and none of them was in either argument I gave the user.
>
> **1. Flip metering has two modes, and NVIDIA says so.** Streamline issue #65, from
> `kirill77` (NVIDIA): *"The Streamline detects that flip-metering is possible — so it doesn't
> meter the `Present()` calls and instead uses NVIDIA hardware to meter the flips. In the second
> case seems like flip-metering wasn't possible, and so — it goes into the mode where it meters
> the `Present()` calls."* So "hardware pacing" is one branch of a two-branch decision, and the
> other branch is **exactly what our pacer already is**. The hardware branch shifts pacing "to
> the display engine" and is, per NVIDIA's DLSS 4 material, **Blackwell**; of its predecessor,
> *"DLSS 3 Frame Generation used CPU-based pacing"*. This is Ada.
>
> **2. `VK_NV_present_metering` is live on this GPU and this driver.** A real vulkan.gpuinfo.org
> report for an **RTX 4090 on Linux with driver 610.43.02** — the box's driver to the patch
> level — shows `presentMetering == true`. The Vulkan spec's own words: *"Present Metering evenly
> paces out the next `numFramesPerBatch` `vkQueuePresentKHR` presents. This gives smoother pacing
> between presents in applications with frame generation integrations."* It is a `pNext` on
> `VkPresentInfoKHR`, with **no surface capability, no swapchain flag, and no documented
> requirement about fullscreen, direct flip, or compositors** — and gamescope's WSI layer
> shallow-copies the present info, so an unknown `pNext` reaches the driver unchanged (verified,
> §5.3). **DXVK, vkd3d-proton and dxvk-nvapi contain zero references to it.**
>
> **3. Someone has already done it.** `bevyengine/dlss_wgpu` PR #34 (open since 2026-08-17) adds
> NGX Vulkan Frame Generation with *"a `present_metering` module [that] adds VK_NV_present_metering
> so generated frames can be paced evenly"* — the same shape as ours, one `EvaluateFeature` per
> generated frame. Unmerged and untested, but it is a design reference, and it settles the
> "does it exist" half of the question: **yes.**
>
> **Both reasons I gave the user were wrong.** Reason 1 ("the private NVAPI entry does not
> exist"): the entry is **public**, documented since R570 — and the string in the box's
> `nvapi64.dll` is not evidence of an implementation, because DXVK-NVAPI compiles **all 522** of
> NVIDIA's function names into the binary as a lookup table while implementing **144** (§1.1).
> Reason 2 ("gamescope composites"): right conclusion, wrong route. It is not "compositors
> composite" — it is two specific unconditional gates in gamescope's DRM backend, both tripped by
> this command line, one of which fires only because NVIDIA has no AMD plane colour-management
> properties (§3.2). And even *with* direct scanout the driver still would not own the flip
> queue, because gamescope's own atomic commit does, one at a time (§3.3).
>
> **Two side findings that may be worth more than the headline:**
> 1. `--hdr-itm-enabled` costs a mandatory full-frame Vulkan composite every frame and, read from
>    gamescope's own source, **applies no inverse tone mapping** — the specialization constant
>    that switches the ITM shader on is never set (§3.2).
> 2. gamescope's WSI layer rewrites every client's present mode to **MAILBOX** toward the driver,
>    and for a MAILBOX client **only the newest commit in a wakeup becomes the paint source — the
>    others are silently dropped** (§3.4). A back-to-back present pair can lose the generated
>    frame with no error anywhere, and our crop gate cannot see it. That is the same symptom
>    vocabulary as the upstream "double image" / "every second frame is a duplicate" reports, and
>    it is a lead for the FG work in its own right.
>
> **And the upstream verdict, from an NVIDIA engineer, on vkd3d-proton PR #2938 (2026-04-21):**
> *"There are some issues with `vkQueueSubmit` attribution remaining, which can cause
> stutter/subpar latency in games with frame gen enabled. **Fixing this will require changes to
> both the NVIDIA driver and vkd3d-proton. We're still working out the details on this.**"* Nobody
> upstream has claimed the pacing job. Our pacer is not competing with a better one; it is
> competing with an unfinished one.

Written 2026-09-03. Report only; no source was changed and nothing was run on the box. Sources
and versions in §0; ranked verdict in §8; what could not be obtained in §9.

---

## 0. What was read, and at what version

| Source | Version / SHA read | Used for |
|---|---|---|
| `jp7677/dxvk-nvapi` | tag **v0.9.2**, `f9fdf268e888d17ae9875a4f01908164ff05f740` (2026-05-12) — **the box's version** | §1 |
| `jp7677/dxvk-nvapi` | master `d102231de42d64fe914f99a9a55a66029902cb59` (2026-08-28) | §1.3 |
| `NVIDIA/nvapi` (public headers) | `9b181ea572f680327fe01a14a0f1f41c78034104` — **the exact submodule commit v0.9.2 pins** | §1.1-1.2 |
| `HansKristian-Work/vkd3d-proton` | master `18c0887dfb4cd914f4061576ab5e2cd524c23ee8` (2026-09-01), cross-checked against tag **v3.0.1** (2026-05-05) | §4, §5.6 |
| `doitsujin/dxvk` | master `adeda6639a09ad1b6a1b7c4158a781ffaf68947d` (2026-08-31) | §4.4 |
| `ValveSoftware/gamescope` | master `e383171ff1c0f7b5b37f05f2bb67f7c5282b087f` (2026-09-02), cross-checked against tag `3.16.9` | §3, §5.3, §5.6 |
| `KhronosGroup/Vulkan-Docs` | `main`, `chapters/VK_KHR_surface/wsi.adoc` | §5.1 |
| `KhronosGroup/Vulkan-Headers` | `main`, `include/vulkan/vulkan_core.h` | §5.1 |
| vulkan.gpuinfo.org reports **49069** (4090/610.43.02) and the 575/580/590/595/610 branch set | retrieved 2026-09-03 | §5.2, §4.2 |
| `NVIDIA-RTX/Streamline` | tag **v2.12.0**, plus issue #65 | §2 |
| NVIDIA GeForce news, "NVIDIA DLSS 4 Introduces Multi Frame Generation" | retrieved 2026-09-03 | §2.4 |
| NVIDIA Linux driver README `nvpresent.html`, and Vulkan beta driver release notes | 595.58.03; betas to 580.94.18 | §4.3, §6.1 |
| vkd3d-proton issues #2325, #3232, PRs #2938, #2930 (`c8c8ab50`), #2751, #2317; dxvk-nvapi PR #351, issues #192, #245, #317, #336 | retrieved 2026-09-03 | §1.3, §6.3 |
| `bevyengine/dlss_wgpu` PR #34; `Nukem9/dlssg-to-fsr3` | retrieved 2026-09-03 | §5.3 |
| `docs/RESEARCH-STREAMLINE-INTERNALS.md` | branch `docs-dlss-ue5-audit` | starting point; §1.3 **corrects** its §7.2d |

Practical notes: gpuinfo needs a browser `User-Agent` (`curl -A "Mozilla/5.0"`); WebFetch gets
HTTP 403, and its DataTables endpoints 500 unless every `filter[...]` key and the full
`columns[i][data]` set are supplied. **GitHub's repo-scoped code search returns false zeros** —
every "not present" claim below is a local grep of a shallow clone at a named SHA, never a code
search.

Version note: **there is no upstream vkd3d-proton "3.1.0" release.** `v3.0.1` is the newest tag;
master's `meson.build:2` reads `version : '3.1.0'` (the post-release bump). So the box's
"vkd3d-proton 3.1.0" (CLAUDE.md §1, measured 2026-08-31) is a **post-v3.0.1 master snapshot**, and
everything §4 cites was checked against the `v3.0.1` tag as well as against master.

---

## 1. Question 1: the NVAPI call is PUBLIC, and it is NAME-ONLY on the box

### 1.1 The string in the DLL is expected and proves nothing. HARD.

DXVK-NVAPI exports no `NvAPI_D3D12_SetFlipConfig`; NVAPI is reached through
`NvAPI_QueryInterface(id)`. The id→name table is **NVIDIA's own header**, vendored as a submodule
and included verbatim:

* `meson.build:47` — `nvapi_headers = include_directories('./external/nvapi')`
* `src/nvapi_interface.cpp:2` — `#include "nvapi_interface.h"`
* pinned `NVIDIA/nvapi@9b181ea…/nvapi_interface.h:322` — `{ "NvAPI_D3D12_SetFlipConfig", 0xf3148c42 },`

That header carries **522** `{ "NvAPI_…", 0x… }` entries, every one of which becomes a string
literal in the compiled `nvapi64.dll`. `src/nvapi_interface.cpp` then dispatches with
`INSERT_AND_RETURN_WHEN_EQUALS(...)` — and there are **144** of those macros. Anything not among
the 144 falls off the end of the block:

```cpp
// src/nvapi_interface.cpp:225-227  (v0.9.2)
    log::info(str::format(n, " (", name, "): Not implemented method"));
    return registry.insert({id, nullptr}).first->second;
```

Searching `SetFlipConfig` in `src/nvapi_interface.cpp` finds **nothing**. So
`NvAPI_QueryInterface(0xf3148c42)` logs `Not implemented method` and returns `nullptr`.

**So `strings nvapi64.dll` finding `NvAPI_D3D12_SetFlipConfig` tells us only that the box runs
DXVK-NVAPI.** The same run's other hits confirm that reading rather than contradicting it:
`NvAPI_D3D12_NotifyOutOfBandCommandQueue is called with OUT_OF_BAND_IGNORE` is a *log message*
from `src/nvapi_d3d12.cpp:875`, and that call **is** one of the 144 — which is precisely why its
log strings appear beside the bare name of one that is not.

> The rule this earns, since the user named the trap: **DXVK-NVAPI's binary contains the complete
> NVAPI namespace as data. Never infer implementation from a name found in it.** The cheap checks
> are `grep INSERT_AND_RETURN_WHEN_EQUALS src/nvapi_interface.cpp` in source, and
> `DXVK_NVAPI_LOG_LEVEL=info` plus a grep for `Not implemented method` at runtime.

### 1.2 It is not a private entry point. HARD, and it is fully documented.

`NvAPI_D3D12_SetFlipConfig` is in NVIDIA's **public** header release with a complete doc block
(`NVIDIA/nvapi@9b181ea…/nvapi.h:15750-15773`), `\since Release: 570`,
`SUPPORTED OS: Windows 10 and higher`:

```c
//!   DESCRIPTION: This API configures flip metering for frame pacing control. When enabled, the display
//!                driver will equally space the flips for all frames within a batch, helping to achieve
//!                smoother frame delivery. The batch size and timing parameters are specified via the
//!                NV_FLIP_CONFIG structure. This function must be called right before the first
//!                Present() of each batch of frames.
NVAPI_INTERFACE NvAPI_D3D12_SetFlipConfig(__in IUnknown *pCommandQueue,
    __in UINT vidpnSrcID, __inout NV_FLIP_CONFIG *pFlipConfig);
```

with, at `nvapi.h:15886-15920`:

```c
#define NVAPI_MAX_FRAMES_PER_FLIP_BATCH 8
typedef enum {
    NVAPI_FLIP_CONFIG_FEEDBACK_OK = 0,
    NVAPI_FLIP_CONFIG_FEEDBACK_NO_IFLIP = NV_BIT(0),
    NVAPI_FLIP_CONFIG_FEEDBACK_MIRRORING_DETECTED = NV_BIT(1)
} NVAPI_FLIP_CONFIG_FEEDBACK_MASK;

typedef struct _NV_FLIP_CONFIG_V2 {
    NvU32 version;
    NvU32 nFramesPerBatch;          //!< 0 = flip metering disabled, must be <= 8
    NvU32 flipConfigFeedbackMask;   //!< [out]
    NvU32 nPresentedFramesPerBatch;
    NvF32 timePerBatchMs;           //!< <= 0 if unknown
    NvF32 minControlFrameTimeMs;
} NV_FLIP_CONFIG_V2;
```

(`NV_NGX_DRIVER_FEATURE_ID_SET_FLIP_CONFIG_V2 = 0x00343dcf`, `nvapi.h:25656`.)

Three things fall out of the struct alone, all HARD:

1. **The unit is a *batch*, not a frame.** This is the MFG shape: one rendered frame plus N-1
   generated ones, handed to the driver as a group whose flips are spaced evenly. Our
   present-twice pair *is* a batch of 2, and "call it right before the first `Present()` of each
   batch" maps cleanly onto `fg_present.cpp`'s pair — if the call existed.
2. **The driver reports when it cannot do it**, and the first failure bit defined is `NO_IFLIP`.
   That is the vendor conceding, in a header, that the D3D12 form wants the application's flips
   going straight to the display.
3. **`vidpnSrcID` is a WDDM display-path identifier.** There is no VidPN on Linux, so a
   translation layer would have to invent or ignore it. Friction, not a wall — and the Vulkan
   equivalent (§5) drops the parameter entirely, which is a hint about which surface NVIDIA
   expects Linux callers to use.

### 1.3 What v0.9.2 does instead is *report the capability without implementing it* — and the commit says so in as many words. HARD.

The one flip-related line in the whole repository:

```cpp
// src/nvapi_ngx.cpp:34-63  NvAPI_NGX_GetDriverFeatureSupport
        switch (info.featureId) {
            case NV_NGX_DRIVER_FEATURE_ID_SET_FLIP_CONFIG_V2:
                info.bSupported = NV_TRUE;
                break;
            default:
                info.bSupported = NV_FALSE;
```

It arrived in PR **#351** (merged 2026-04-03, Saancreed, commit `4d51e135`), whose commit message
is, verbatim and in full:

```
nvapi-ngx: Implement NvAPI_NGX_GetDriverFeatureSupport

And pretend that SET_FLIP_CONFIG_V2 is supported.
```

**PR #351 has an empty body and zero comments; there is no rationale on record anywhere, and no
issue or PR in any of these repositories has ever asked for `NvAPI_D3D12_SetFlipConfig`.** A
case-insensitive `flip` search over `src`, `inc` and `layer` finds nothing else — and **master
(2026-08-28) is identical**.

> **This corrects `docs/RESEARCH-STREAMLINE-INTERNALS.md` §7.2d**, which read the v0.9.2 release
> note (*"Implement several NVAPI entrypoints needed for limited/incomplete support for Dynamic
> Multi Frame Generation. This requires updated DLSS snippets and Streamline."*) and concluded
> *"dxvk-nvapi **v0.9.2** — our exact version — ships a `SET_FLIP_CONFIG_V2` implementation …
> **HARD**."* **It does not.** §7.2d's softer conclusion ("the pacer's inputs are partial, not
> missing") survives; its HARD label on that sentence does not. The error is the same shape as the
> one §7.2d itself warns against: **a release note is not source.**

**A hypothesis this raises for Streamline titles — not for us. UNCONFIRMED, and worth filing.**
Per §2.3, Streamline chooses between hardware flip metering and software Present metering by
*detecting whether flip metering is possible*. If `sl.dlss_g` consults
`NvAPI_NGX_GetDriverFeatureSupport(SET_FLIP_CONFIG_V2)` to make that choice, then since v0.9.2 it
is told **yes** under Proton, while `NvAPI_D3D12_SetFlipConfig` returns `nullptr` and IFLIP does
not exist — i.e. it may skip its own software pacer in favour of hardware metering that can never
be configured. That is a textbook "runs but paces wrong". **Timeline caveat, and it matters:
vkd3d-proton #2325 predates PR #351 by three years, so the pretend cannot be the original cause —
it would be a NEW risk introduced 2026-05.** This is the single question worth putting to
Saancreed.

**None of it affects this project**, because we drive `nvngx_dlssg.dll` directly with no
Streamline in the process (`docs/STRAY-RENDERING-FACTS.md` §32): we never make that selection, and
we are always our own pacer. Recorded here because it is the most plausible mechanism anyone has
for the upstream reports, and because it is cheap for someone to check.

### 1.4 vkd3d-proton has no receiving end. HARD.

A `FLIP|flip_config|FlipConfig` search over `libs/` and `include/` on master finds only
`DXGI_SWAP_EFFECT_FLIP_*` enum names in `include/vkd3d_dxgi.idl`. There is no
`ID3D12CommandQueueExt` method, no swapchain-ext method, and no Vulkan call for it. So even a
dxvk-nvapi patch would have nowhere to forward to today.

For contrast, the Reflex out-of-band call **is** plumbed end to end — dxvk-nvapi
`src/nvapi_d3d12.cpp:850` → `src/nvapi/nvapi_d3d12_command_queue.cpp:42` → vkd3d-proton
`libs/vkd3d/command_queue_vkd3d_ext.c:55` → `vkQueueNotifyOutOfBandNV` (`VK_NV_low_latency2`),
returning `E_NOTIMPL` when the extension is absent. That is what a working chain looks like, and
the shape a flip-metering chain would need.

**Answer to question 1: NAME-ONLY.** In the compiled id→name table, absent from the dispatch,
`nullptr` from `NvAPI_QueryInterface`, no receiver in vkd3d-proton. Nothing forwards. **But this
is the wrong door** — see §5.

---

## 2. Question 2: what flip metering actually requires

### 2.1 Two different requirements, and the user was right to separate them

From §1.2's header text: the application hands the driver a **batch** of up to 8 frames and the
**display driver equally spaces the flips within that batch**. Two requirements follow, and only
one is about DWM:

* **(a) Something other than the application must own a queue of pending flips it can space.**
* **(b) On Windows, (a) is spelled "Independent Flip"** — the GPU flipping directly to the
  application's back buffer, bypassing DWM composition.

(b) is unreachable here by definition. (a) is the real question — and it depends on *which*
"something": the display engine (§2.4, Blackwell, and gone under any compositor) or the driver's
own present path (§5, available on Ada).

### 2.2 The vendor's own words on IFLIP, and the scope is favourable to us. SOFT (documentation), verbatim.

`NVIDIA-RTX/Streamline` v2.12.0, `docs/ProgrammingGuideDLSS_G.md:1146-1161`:

> **22.6 IFLIP REQUIREMENT**
>
> "VSync with Frame Generation relies on Independent Flip (IFLIP) — a Windows presentation mode
> where the GPU flips directly to the application's back buffer on the display, bypassing Desktop
> Window Manager (DWM) composition. IFLIP is critical for low-latency VSync because it allows the
> driver to control presentation timing at the hardware level."
>
> "**If VSync is enabled with Frame Generation on a system that does not support IFLIP, high
> latency is expected.** On such systems it is better to not enable VSync."

**Read the scope.** IFLIP is stated as a requirement for **VSync + FG**, and the prescribed
behaviour without it is *don't use VSync* — not *don't use FG*. We present with
`SyncInterval = 0` (the game's own value, passed through `fg_present.cpp:711` and `:774`), i.e. we
are already in the regime the vendor prescribes for a system with no IFLIP. The same guide adds at
`:1141` that **"VSync with Frame Generation is only supported on D3D12"** and never on Vulkan, and
at `:1139` that VSync is unsupported for `DLSSGMode::eDynamic` and for `SyncInterval > 1` — more
evidence that the VSync+IFLIP path is a Windows-DXGI construct rather than the core of frame
generation.

The sentence that started this investigation is `§13.1`, at `:816`:

> "`MsBetweenPresents` is not suitable for measuring frame pacing quality because DLSS-G uses
> specialized hardware to delay the image after Present() has been called."

`SetFlipConfig` appears **nowhere** in Streamline's open source tree — consistent with the earlier
audit's finding that it lives inside the closed `sl.dlss_g.dll` ("RSync", named at `:1131`).

### 2.3 THE CLEAREST STATEMENT ANYWHERE: there are two pacing modes, and the software one is what we already do. HARD (vendor, on their own tracker).

NVIDIA-RTX/Streamline issue **#65**, "DLSS-G Frame Pacing regression 2.7.2", `kirill77`:

> "We're now metering display flips as opposed to `Present()` calls."
>
> "**The Streamline detects that flip-metering is possible — so it doesn't meter the `Present()`
> calls and instead uses NVIDIA hardware to meter the flips. In the second case seems like
> flip-metering wasn't possible, and so — it goes into the mode where it meters the `Present()`
> calls.**"

This is the answer to question 2, and it reframes the whole enquiry. "Hardware pacing" is not a
different *kind* of thing from what we built; it is the **preferred branch of a two-branch
decision whose fallback branch is a `Present()`-metering CPU pacer**. When flip metering is
impossible, NVIDIA's own product does what `core::fg::Schedule` does. We are not missing a
mechanism — we are permanently on the fallback branch, and so is NVIDIA's own code on any system
without IFLIP.

### 2.4 The hardware branch is Blackwell. SOFT (vendor material), quoted, unrefuted.

NVIDIA's DLSS 4 article, [*NVIDIA DLSS 4 Introduces Multi Frame Generation & Enhancements For All
DLSS Technologies*](https://www.nvidia.com/en-us/geforce/news/dlss4-multi-frame-generation-ai-innovations/),
retrieved 2026-09-03:

> "To address the complexities of generating multiple frames, **Blackwell uses hardware Flip
> Metering, which shifts the frame pacing logic to the display engine**, enabling the GPU to more
> precisely manage display timing."

> "**DLSS 3 Frame Generation used CPU-based pacing** with variability that can compound with
> additional frames, leading to less consistent frame pacing between each frame, impacting
> smoothness."

The display engine itself was changed for it — "twice the pixel processing capability to support
higher resolutions and refresh rates for hardware Flip Metering with DLSS 4" — which is why Multi
Frame Generation is not offered on Ada at all. **The target GPU is an RTX 4090. That is Ada.**

The honest split, which must be kept straight in any follow-up:

| Claim | Status |
|---|---|
| Streamline selects between HW flip metering and SW `Present()` metering, falling back to the latter | **HARD** (Streamline #65, §2.3) |
| Hardware flip metering (display-engine pacing) is a Blackwell capability | **SOFT** (vendor article), unrefuted |
| DLSS 3 FG on Ada paced on the CPU | **SOFT** (same article), consistent with #65 |
| The `VK_NV_present_metering` API is available, feature bool TRUE, on an RTX 4090 on Linux at driver 610.43.02 | **HARD** (§5.2) |
| Whether Ada's driver honours present metering in the display engine, in a driver thread, or not at all | **UNCONFIRMED** — `presentConfigFeedback` is the field that would say, and nobody has read it |

---

## 3. Question 3: where our present actually lands, hop by hop

### 3.1 The chain

| # | Hop | Who owns it | Can a driver-side pacer act here? |
|---|---|---|---|
| 1 | Game calls `IDXGISwapChain::Present` | our hook, `present_owner.cpp:150` patches the slot | no — this is us |
| 2 | Our pair scheduler holds the frame and calls the original `Present` twice | `fg_present.cpp:711`, `:774`; schedule `src/core/fg_plan.hpp:83-101`; worker `fg_present.cpp:305` | **this is our pacer** |
| 3 | DXVK's `DxgiSwapChain::Present` | `dxvk/src/dxgi/dxgi_swapchain.cpp` | no — thin, forwards to vkd3d's presenter |
| 4 | vkd3d-proton's `dxgi_vk_swap_chain`: blit into the swapchain image, `vkQueuePresentKHR` | `vkd3d-proton/libs/vkd3d/swapchain.c:3093` | **YES. All three candidate mechanisms live here — §4 present timing, §5 present metering, §5.6 GOOGLE display timing** |
| 5 | NVIDIA Vulkan **Wayland** WSI → `wl_surface` commit to gamescope, through gamescope's WSI layer | closed driver + `layer/VkLayer_FROG_gamescope_wsi.cpp` | partly — it is the driver, so it can delay its own commit (§5.4) |
| 6 | gamescope composites (or scans out) and issues `drmModeAtomicCommit` | `gamescope/src/Backends/DRMBackend.cpp:4113` | **no** — §3.3 |
| 7 | Kernel / nvidia-drm pageflip | kernel | **no** — no flip-batching uAPI exists; §5.5 |

**The answer to question 3 is hop 4, with hops 5 and 6 as the executors.** Below hop 5 is a
compositor and a kernel that take one flip at a time.

### 3.2 Direct scanout is impossible in this configuration — twice over. HARD, from gamescope's source.

The user's counter-argument was that `--backend drm --adaptive-sync -f` is the configuration in
which gamescope *can* hand a client buffer straight to a display plane. True in general, false for
this command line. The decision is inline in `CDRMBackend::Present`
(`src/Backends/DRMBackend.cpp:3625`); scanout is only attempted when
`!bNeedsFullComposite && !bWantsPartialComposite` (`:3680`), and two disqualifiers fire here
**independently of each other**:

```cpp
// src/Backends/DRMBackend.cpp:3664-3668
if ( g_bOutputHDREnabled )
{
    bNeedsFullComposite |= g_bHDRItmEnable;
    bNeedsFullComposite |= !SupportsColorManagement() && (layers.count() > 1 || layer0.colorspace != HDR10_PQ);
```

1. **`--hdr-itm-enabled` forces a full composite, unconditionally.** No guard, no per-layer test.
   The same three lines appear in every backend (`WaylandBackend.cpp:1082`,
   `OpenVRBackend.cpp:1784`).
2. **`SupportsColorManagement()` is AMD-only and returns false on NVIDIA.**
   `drm_supports_color_mgmt` requires `AMD_PLANE_CTM` and `AMD_PLANE_BLEND_TF` on the primary
   plane (`:3540-3549`); those properties do not exist on NVIDIA. Combined with the `!= HDR10_PQ`
   test and this title's SDR `R10G10B10A2_UNORM` back buffer (which maps to
   `GAMESCOPE_APP_TEXTURE_COLORSPACE_SRGB`, `rendervulkan.hpp:109-126`), **`--hdr-enabled` alone
   forces a full composite** whether ITM is on or not.

So dropping `--hdr-itm-enabled` would not open the gate; `--hdr-enabled` would have to go too
(then `ColorspaceIsHDR(SRGB)` is false at `:3673` and the gate opens). Other disqualifiers on the
same list, for completeness: `>= 3` layers → *partial* composite (`:3632` — the game plus mangoapp
is 2, so the Steam overlay is what pushes it over), any visible cursor layer (`:3659`, because
`cv_drm_cursor_plane` defaults to **false**), FSR/NIS/blur/ReShade, output rotation (`:3677`,
*"can't rotate planes at scanout"*), and `--force-composite` — which `ENABLE_VKBASALT=1` also sets
(`steamcompmgr.cpp:9545-9549`).

A further consequence, unrelated to metering but not to smoothness: compositing forces `eFlipType`
away from `Async` (`steamcompmgr.cpp:10202`), so tearing is off regardless — though
`--adaptive-sync` outranks tearing anyway (`:10188-10206`).

**And the ITM shader appears to be dead code. HARD from source, UNCONFIRMED empirically.** Two
independent proofs, either sufficient:

* The shader declares `layout(constant_id = 7) const bool c_itm_enable = false;`
  (`src/shaders/descriptor_set.h:10`), but `compilePipeline` builds
  `std::array<VkSpecializationMapEntry, 7>` with `constantID` **0 through 6 only**
  (`src/rendervulkan.cpp:1096-1134`) — so constant 7 is never specialized and keeps `false`.
* `pipeline(...)` takes `bool itm_enable = false` last (`rendervulkan.hpp:824`) and **all 11 call
  sites omit it** (`rendervulkan.cpp:4045, 4066, 4157, 4169, 4192, 4216, 4236, 4254, 4266, 4303`).

Identical in tag `3.16.9`. The ITM maths itself is real and unreachable —
`bt2446a_inverse_tonemapping` at `src/shaders/composite.h:101`, implementation
`src/shaders/colorimetry.h:172-215` (BT.2446 Method A, inverted). Consistent (SOFT) with the open
reports [gamescope#1034](https://github.com/ValveSoftware/gamescope/issues/1034) and
[#899](https://github.com/ValveSoftware/gamescope/issues/899), both "HDR ITM seems to not be
functional".

**So `--hdr-itm-enabled` buys a mandatory full-frame composite per frame and, as far as the source
says, no inverse tone mapping at all.** §7 turns that into an experiment — with a caveat, because
someone's *working* FG configuration under Proton includes those exact flags (§6.3).

### 3.3 Even WITH direct scanout, the NVIDIA driver would not own the flip queue. HARD.

This is what makes the answer structural rather than configuration-dependent.

* gamescope's per-frame flip is **its own** `drmModeAtomicCommit` (`DRMBackend.cpp:4113`), issued
  from the steamcompmgr thread (`steamcompmgr.cpp:10272` → `paint_all` → `Present` → `Commit`).
  The NVIDIA Vulkan WSI is not presenting to the display on this path at all; gamescope talks to
  libdrm directly, and `VK_KHR_display` / `VK_EXT_acquire_drm_display` appear nowhere in its tree.
* **Only one flip is ever in flight.** `Commit` ends with
  `drm->uPendingFlipCount.wait(uNewPendingFlipCount)` (`:4186-4190`), so despite
  `DRM_MODE_ATOMIC_NONBLOCK` the paint thread blocks until the pageflip event arrives. There is no
  queue of pending flips for anything to space.
* **The atomic property table has no deadline or target-time property** (`:2096-2236`). A client
  target present time *is* honoured by gamescope, but only as a latch gate —
  `if (entry.desiredPresentTime > next_refresh_time) { commits_before_their_time.push_back(entry);
  continue; }` (`steamcompmgr.cpp:7715-7718`) — and never reaches KMS.
* The clock is a **prediction**, re-phased off each real pageflip: `CVBlankTimer` is a `timerfd`
  (`vblankmanager.hpp:27`), `GetNextVBlank(offset) = GetLastVBlank() + interval - offset`
  (`vblankmanager.cpp:83-94`); `m_ulLastVBlank` is written only by `MarkVBlank`, whose only DRM
  caller is the pageflip handler on the "gamescope-kms" thread (`DRMBackend.cpp:759-776`). No
  `drmCrtcGetSequence`, no `drmWaitVBlank`.
* gamescope's `wp_presentation` timestamps are that **prediction**, not the pageflip time
  (`wlserver.cpp:1547-1592`, sending `g_SteamCompMgrVBlankTime.schedule.ulTargetVBlank` from
  `steamcompmgr.cpp:7808`), with the flags deliberately fudged: *"We don't set HW_COMPLETION
  because we actually kinda are using a timer here"* (`wlserver.cpp:1556-1570`).
* With `--adaptive-sync` live, `FlipType::VRR` sets `bShouldPaint = hasRepaint`
  (`steamcompmgr.cpp:10233`) — **gamescope commits as soon as a client commit is ready**, and the
  timer becomes a 300 µs floor rather than the authority (`vblankmanager.cpp:157-177`). That is
  the *best* case for us: the cadence we choose is largely the cadence that reaches the display.
  It also means there is nothing left for a display engine to meter, because gamescope is already
  flipping on demand.

**So requirement (a) in the display-engine sense — "the driver owns a queue of flips to the
display" — is not satisfiable under gamescope's DRM backend in any configuration.** Requirement
(b), IFLIP, is a Windows concept with no analogue here. What remains available is the driver
pacing its own `vkQueuePresentKHR` calls into the compositor (§5) or asking the compositor to hold
a frame until a stated time (§5.6).

### 3.4 A gamescope finding that matters for our present-twice path, independent of pacing. HARD.

Two mechanisms act directly on a client that presents twice per frame, and neither was known to
this project:

* gamescope's WSI layer **force-rewrites every gamescope swapchain's present mode to
  `VK_PRESENT_MODE_MAILBOX_KHR` toward the driver** — *"Force all present modes to MAILBOX to the
  underlying driver / We implement fifo ourselves"*
  (`layer/VkLayer_FROG_gamescope_wsi.cpp:1483-1497`) — forwarding the application's real intent
  over its private `gamescope_swapchain_set_present_mode` (`:1538`).
* gamescope's own FIFO is a **one-commit-per-vblank latch** (`steamcompmgr.cpp:7681-7726`, and
  `:7743-7789` for xdg). For a MAILBOX/IMMEDIATE client, all commits arriving in one wakeup are
  latched and **only the newest becomes the paint source — the others are silently dropped.** For
  a client that declared FIFO, the second commit is **deferred to the next vblank tick by
  gamescope**, not by the driver.

**So a back-to-back present pair can lose the generated frame entirely under gamescope**, with no
error anywhere and nothing our validation gate would catch — the crop gate reads the *generated
texture*, not the screen. That is an additional, previously unrecorded reason `NgxFGPacing=0`
reads as bad, an additional reason the phase-locked schedule matters, and a warning that a
measured 2.00× *presents* per game present is not evidence of 2.00× *displayed* frames.

It is also the same symptom vocabulary as the upstream reports: dxvk-nvapi
[#192](https://github.com/jp7677/dxvk-nvapi/issues/192) says *"enabling DLSS Frame Generation leads
to duplicated frames, **even on Ada Lovelace GPUs**"*; vkd3d-proton
[#2325](https://github.com/HansKristian-Work/vkd3d-proton/issues/2325) reports Cyberpunk
"frame generation seemingly doesn't actually happen, leading to a double-image effect"; #245
reports "every second frame is a duplicate of the previous frame". Whether the same mechanism is
behind them is **UNCONFIRMED** and worth one measurement: count presents *and* displayed frames.

---

## 4. Question 4: "ask for a present time" — and vkd3d-proton ALREADY implements it

### 4.1 `VK_EXT_present_timing` is wired into vkd3d-proton's swapchain, and the box's build has it. HARD.

`libs/vkd3d/vulkan_procs.h:418-422` declares the entry points, and
`libs/vkd3d/swapchain.c:2809-2947` is a complete present-time scheduler:

```c
static bool dxgi_vk_swap_chain_setup_present_timing_request(
        struct dxgi_vk_swap_chain *chain, uint64_t present_count, VkPresentTimingInfoEXT *timing_info)
```

It targets `timing_info->targetTime`, chooses between
`VK_PRESENT_TIMING_INFO_PRESENT_AT_RELATIVE_TIME_BIT_EXT` and an absolute time accumulated from
the driver's own timing feedback, sets
`VK_PRESENT_TIMING_INFO_PRESENT_AT_NEAREST_REFRESH_CYCLE_BIT_EXT` for fixed-refresh displays,
detects VRR (`refresh_interval == VRR_INTERVAL`) and then *stops compensating* — *"For confirmed
VRR we don't care, and want a pure pace"* — and re-aligns clock drift by eighths. It even reasons
about our compositor by name:

```c
/* It's possible for implementation to report a 60 Hz display that is limited to 30 Hz (think e.g. gamescope).
 * In this case duration is 33.3ms, while interval is 16.6ms. */
```

The intent is stated outright at `swapchain.c:56-64`:

```c
/* FIFO_LATEST_READY is intentionally excluded. ... When VK_EXT_present_timing becomes available,
 * VkPresentTimingInfoEXT.targetTime would be the appropriate pacing mechanism. */
```

All of this is in tag **`v3.0.1`** (`swapchain.c:2871`, released 2026-05-05), from PR #2751
"Add present timing support" (merged 2026-04-27). Its changelog entry is scoped, and the scope
matters: *"Implement `VK_EXT_present_timing`, allowing for smooth frame pacing **for SyncInterval
> 1** when supported."* **So it exists as a vsync-multiple pacer, not as an FG pacer** — but the
mechanism is general and the code is present in the box's build.

### 4.2 The driver-version gate, and the branch matrix. HARD.

```c
// libs/vkd3d/device.c:1958-1971  (also v3.0.1 device.c:1856)
if (info->vulkan_1_2_properties.driverID == VK_DRIVER_ID_NVIDIA_PROPRIETARY &&
    info->properties2.properties.driverVersion <= VKD3D_DRIVER_VERSION_MAKE_NV(595, 0, 0))
{
    WARN("Disabling present_id2, wait2 and timing on pre-595 NV drivers.\n");
    ...
    device->vk_info.EXT_present_timing = false;
```

One gpuinfo RTX 4090 Linux report per NVIDIA production branch (all HARD):

| | 575.64.05 | 580.159.03 | 590.48.01 | 595.45.04 | **610.43.02** |
|---|---|---|---|---|---|
| `VK_NV_present_metering` | r.1 | r.1 (true) | r.1 (true) | r.1 (true) | **r.1 (true)** |
| `VK_KHR_present_wait2` / `_id2` | absent | absent | r.1 | r.1 | **r.1 (true)** |
| `VK_EXT_present_timing` | absent | absent | absent | r.3 | **r.3 (true)** |
| `VK_GOOGLE_display_timing` | absent | absent | absent | absent | absent (but see §5.6) |

**`present_wait2`/`id2` arrive on 590, `present_timing` on 595.** The box's 610.43.02 is above
vkd3d-proton's gate and is the first of these branches with the full set — so **these options are
new to this driver**, which is part of why nothing upstream uses them yet. Do not map beta version
numbers onto production ones: the same features appeared in Linux *betas* 570.123.18 and
580.94.10 while production 580.159.03 had neither.

### 4.3 The gates it sits behind — and I have to walk back my own prediction

`dxgi_vk_swap_chain_update_wait_timing_capabilities` (`swapchain.c:1433-1521`) computes:

```c
chain->present.wait2 = id2_caps.presentId2Supported && wait2_caps.presentWait2Supported;
chain->present.timing = chain->present.wait2 && present_timing_caps.presentTimingSupported &&
        (present_timing_caps.presentStageQueries & useful_present_stages);
```

and the request is chained only when **all** of these hold:

1. device features `presentTiming` + `presentId2` + `presentWait2` — **TRUE on this driver** (§4.2);
2. the **surface** reports `presentTimingSupported` plus one of `REQUEST_DEQUEUED` /
   `IMAGE_FIRST_PIXEL_OUT` / `IMAGE_FIRST_PIXEL_VISIBLE`;
3. `present_mode_supports_timing(...)` — FIFO, FIFO_RELAXED, or FIFO_LATEST_READY with a non-zero
   interval (`swapchain.c:67-74`);
4. `use_present_id`, needing `present_mode_pacing_should_wait(...)` — **FIFO or FIFO_RELAXED only**
   (`swapchain.c:56-65`, chaining at `:3080-3088`);
5. non-zero `refresh_duration` feedback from a previous present.

**Gates 3 and 4 are closed for us today.** UE4 with vsync off presents `SyncInterval = 0`, so
vkd3d selects `IMMEDIATE`/`MAILBOX` (`swapchain.c:2180-2189`) or the "unlocked" member of a
maintenance1 mode group (`:2160-2168`, `:3024-3033`); no present id and no timing info are chained
at all.

**Gate 2 is the open question, and the per-surface check is normative, not advisory.** The
`VK_KHR_present_wait2` spec: *"If `presentWait2Supported` is `VK_FALSE`, it indicates that waiting
for presentation is not possible for this surface. Applications **must** not attempt to call
`vkWaitForPresent2KHR`…"* — and the extension's own proposal (Daniel Stone / Derek Foreman,
Collabora) says the point of the v2 extensions is that *"without knowing if a device can support
the extension or not, the extension must be universally disabled"*. So per-surface capability is
the whole design.

> **I predicted this gate would be CLOSED under gamescope, and I now think that was wrong.** The
> reasoning was that gamescope implements neither the commit-timing protocol nor `wp_fifo_v1`
> (zero hits in its tree) and its `wp_presentation` timestamps are its own prediction (§3.3), so
> the NVIDIA Wayland WSI would have no channel to honour a target time. **But NVIDIA's own beta
> release notes show they implement present timing on Wayland and have been fixing bugs in it:**
> Linux 580.94.18 (2026-02-17) — *"Fix inconsistent `vkQueuePresentKHR` times with
> `VK_EXT_present_timing` for **Wayland and direct display** [Linux]"*; Linux 580.94.11
> (2025-11-25) — *"Some minor `VK_EXT_present_timing` improvements for Linux"*. **SOFT** (release
> notes) but directly on point. So the driver may well advertise timing on a Wayland surface and
> satisfy it with its own scheduling before the commit. **Gate 2 is UNCONFIRMED and now looks
> more likely open than closed** — which makes §7's experiment 3 the highest-information cheap
> test in this document.

### 4.4 The levers that exist today, all reachable with no code

| Lever | Where | Effect |
|---|---|---|
| `VKD3D_SWAPCHAIN_PRESENT_MODE=FIFO` | `swapchain.c:2140-2155`, documented `README.md:239` | forces FIFO, opening gates 3 and 4 |
| `VKD3D_FRAME_RATE=<fps>` | `swapchain.c:3854-3864` | sets `frame_rate_limit.target_interval_ns` **and** `has_user_override` (suppressing DXVK's own setting); also engages a CPU sleep in vkd3d's present path |
| `dxgi.maxFrameRate` / `dxvk.maxFrameRate` in `dxvk.conf` | DXVK `src/dxgi/dxgi_options.cpp:172-173` → `dxgi_swapchain.cpp:1086` → `IDXGIVkSwapChain2::SetTargetFrameRate` | same target without `has_user_override`; a **negative** value sets the interval while leaving the CPU limiter disengaged until the measured rate exceeds it (`swapchain.c:1336-1345`, `:3500-3536`) |
| `VKD3D_CONFIG=skip_driver_workarounds` | `device.c:1943` | bypasses the pre-595 quirk (not needed at 610) |
| `VKD3D_DEBUG=info` | — | prints `Ensure maximum latency of %u frames with KHR_present_wait.`, and on failure `Implementation supports neither present_wait1 or present_wait2` / `Cannot implement fractional present timing with current setup, falling back to CPU limiter.` |

`IDXGIVkSwapChain2` is **not** reachable by `QueryInterface` from the game's swapchain — DXVK's
`DxgiSwapChain::QueryInterface` answers only the `IDXGISwapChain*` family and
`ID3DDestructionNotifier`. So `dxvk.conf` is the only way in from our side.

**The honest caveat about combining this with our pacer:** with `VKD3D_FRAME_RATE` set,
`frame_rate_limit.enable` is true and vkd3d sleeps in its own present path
(`dxgi_vk_swap_chain_delay_next_frame`, `swapchain.c:3485`). **Two pacers in series is worse than
one.** If this path engages, the right configuration is `NgxFGPacing=0` (back-to-back presents)
plus a FIFO/target-rate hand-off to vkd3d — a design change, not a knob, and not to be attempted
before experiment 3 says the surface supports timing at all.

**One more caveat on forcing FIFO, and it is a real risk.** With `SyncInterval = 0` today,
`entry.id` is zero and vkd3d's wait thread never calls `vkWaitForPresentKHR` (`swapchain.c:3632`);
it drains a semaphore instead. Forcing FIFO turns present-wait **on** — and present-wait under
NVIDIA + the gamescope WSI layer has a history: gamescope issue
[#1592](https://github.com/ValveSoftware/gamescope/issues/1592), *"[NVIDIA] frames stop being
presented (and appear to hang) sporadically when using the WSI layer and `VK_KHR_present_wait`"*,
**RTX 4090**, ~50% reproduction, **audio continues while the picture freezes**, NVIDIA internal
ticket #4924590, reported fixed in the 570.123.07 beta with further deadlock fixes after. **SOFT**
(an issue), old driver, presumably fixed — but it means experiment 3 should be run knowing what
its failure mode looks like, and it should not be left enabled casually.

---

## 5. The mechanism that IS available on this GPU: `VK_NV_present_metering`

### 5.1 What it is. HARD, from the Vulkan spec and headers.

`KhronosGroup/Vulkan-Docs`, `chapters/VK_KHR_surface/wsi.adoc:9661-9685`:

> **Present Metering**
>
> "Present Metering evenly paces out the next `numFramesPerBatch` `vkQueuePresentKHR` presents.
> This gives smoother pacing between presents in applications with frame generation integrations."
>
> "The metering configuration applies to all swapchains in the array in `VkPresentInfoKHR`. The
> configuration specified by `VkSetPresentConfigNV` applies to the next `numFramesPerBatch` calls
> to `vkQueuePresentKHR` and needs to be updated every `numFramesPerBatch` presents."

```c
// Vulkan-Headers, include/vulkan/vulkan_core.h  (VK_NV_present_metering, rev 1)
typedef struct VkSetPresentConfigNV {          // structextends="VkPresentInfoKHR"
    VkStructureType    sType;
    const void*        pNext;
    uint32_t           numFramesPerBatch;      // <= 8 (VUID-VkSetPresentConfigNV-numFramesPerBatch-10581)
    uint32_t           presentConfigFeedback;  // out
} VkSetPresentConfigNV;

typedef struct VkPhysicalDevicePresentMeteringFeaturesNV {
    VkStructureType    sType;
    void*              pNext;
    VkBool32           presentMetering;
} VkPhysicalDevicePresentMeteringFeaturesNV;
```

This is `NV_FLIP_CONFIG` with the WDDM baggage removed: same `numFramesPerBatch`, same feedback
word, no `vidpnSrcID`, no command queue. **And note what the spec does not say: there is no surface
capability, no swapchain creation flag, and no statement anywhere about fullscreen exclusive,
direct flip, or compositors.** Unlike `VK_EXT_present_timing` (§4.3), nothing lets an
implementation advertise per-surface unsupport; the only gate is the device feature bool. The
registry's `provisional` attribute was **removed in Vulkan 1.4.345** (2026-03-06) although the
extension appendix still carries stale provisional prose — so docs.vulkan.org's "should not be
used in production applications" is out of date, which matters if anyone proposes an upstream
patch. (Phoronix reported it arriving in Vulkan 1.4.308 for *"better DLSS 4 frame generation
support with Vulkan or software like VKD3D-Proton"* — SOFT, press, but it names our exact target.)

### 5.2 It is TRUE on an RTX 4090 on Linux at driver 610.43.02. HARD, fetched and parsed directly.

vulkan.gpuinfo.org report **[49069](https://vulkan.gpuinfo.org/displayreport.php?id=49069)**:

```
deviceName     NVIDIA GeForce RTX 4090      deviceID 0x2684
driverID       NVIDIA (Proprietary)         driverInfo 610.43.02   driverVersion 610.43.2.0
apiVersion     1.4.341                      platform Linux
presentMetering              true    VK_NV_present_metering
presentTiming                true    VK_EXT_present_timing
presentAtAbsoluteTime        true    VK_EXT_present_timing
presentAtRelativeTime        true    VK_EXT_present_timing
presentId  / presentId2      true    VK_KHR_present_id  / _id2
presentWait / presentWait2   true    VK_KHR_present_wait / _wait2
presentModeFifoLatestReady   true    VK_EXT_present_mode_fifo_latest_ready
swapchainMaintenance1        true    VK_EXT_swapchain_maintenance1
VK_NV_low_latency2           present
VK_NV_internal_nvpresent     present
VK_GOOGLE_display_timing     ABSENT
```

**That is the box's GPU and the box's driver to the patch level.** Breadth check over gpuinfo's
Linux corpus: 118 devices advertise the extension and **101 report the feature bool true**,
spanning Maxwell through Blackwell (GTX 750 Ti, GTX 960, RTX 2080, RTX 3090, RTX 4090, RTX 5090);
32 distinct RTX 4090 Linux reports have it true, on drivers 580.76.5 through 610.57.4, earliest
2025-08-15. **It is not generation-gated at the Vulkan feature-bool level.**

> **Correction to my own working note.** I first read gpuinfo's Linux *feature* coverage of 7.28%
> (against 24.47% extension coverage) as evidence of an Ada-vs-Blackwell split. It is not: those
> are shares of *all* Linux reporters, most of which are AMD/Intel/Mesa and expose nothing here,
> and the residual gap is VulkanCapsViewer vintage (older builds collect 283 extension-feature
> bools against 318 on newer ones, so the extension is listed and the bool row is missing).
> **Percentage coverage on a mixed-vendor corpus is not a capability gate.** One real report
> settled in a minute what an hour of ratio arithmetic could not.

`presentMetering == VK_TRUE` means the **entry point is accepted on Ada**. It does *not* establish
that the pacing happens in the display engine, and §2.4's Blackwell attribution stands unrefuted —
both can be true, because the driver can accept the request and satisfy it in software.
`presentConfigFeedback` is the field that would tell us, and nobody has read it on this stack.

### 5.3 Nobody uses it — except one open PR, which built exactly the thing we would build

`VkSetPresentConfigNV` / `present_metering` searches over **DXVK master**, **vkd3d-proton
master**, **dxvk-nvapi master** and **gamescope master** all return **zero hits** (local greps at
the SHAs in §0, not code search). Two data points beyond that, both HARD:

* **`bevyengine/dlss_wgpu` PR #34**, "Add DLSS Frame Generation support", opened 2026-08-17,
  **still open** — from the PR body: *"A new `frame_generation` module wraps NGX Vulkan Frame
  Generation, with one `EvaluateFeature` call per generated frame for 2x, 3x, and 4x modes. **A
  `present_metering` module adds VK_NV_present_metering so generated frames can be paced
  evenly**, though chaining it onto a present needs `Surface::set_next_present_chain` from wgpu 31
  (gfx-rs/wgpu#9847)."* **This is the only public code anywhere that paces NGX-generated frames
  with present metering, and its architecture is ours: one evaluate per generated frame, metering
  chained onto the present.** Caveats: unmerged, self-described as AI-assisted, empty Testing
  section. **Treat it as a design reference, not as validation** — but it answers the "does it
  exist" half of the user's question with a yes.
* **`Nukem9/dlssg-to-fsr3`**, `source/maindll/NGX/NvNGXVulkan.cpp` — a real NGX-Vulkan shim —
  queries the capability and logs `"Present metering interface is unimplemented. This is not an
  error."` So NGX's own Vulkan path *asks* for a present-metering interface from its host. That is
  a hint about where the runtime expects the metering call to come from, and it is another
  argument that our present owner is the right place for it.

**Three ways to close the gap, cheapest first.**

**(a) Our own implicit Vulkan layer. No upstream change. ~150 lines. Weekend.**
A Linux-side implicit layer — the same mechanism `VK_LAYER_NV_present` and
`VK_LAYER_FROG_gamescope_wsi` use; `misyltoad/vkroots` reduces it to a header plus a JSON — that
intercepts `vkQueuePresentKHR`, chains `VkSetPresentConfigNV{numFramesPerBatch = 2}` on every
other present, and logs `presentConfigFeedback`. Env-var gated, instantly A/B-able, needs nothing
from the game, from Proton, or from any upstream project.

*One risk is now closed, HARD.* gamescope's layer does
`VkPresentInfoKHR presentInfo = *pPresentInfo;` — a shallow copy that **preserves the `pNext`
chain** — and then patches only the specific structs it recognises
(`layer/VkLayer_FROG_gamescope_wsi.cpp:1436-1497`). **So an unknown `pNext` such as
`VkSetPresentConfigNV` is forwarded to the driver unchanged.** Layer ordering is therefore not
fatal either way.

*The remaining risks are real.* The batch boundary must align with our generated/real pair, which
from inside a layer means "every 2nd present" and is only correct while the ratio is exactly 2 (it
is: our design is 2×). And `presentConfigFeedback` comes back inside the Wine-hosted Vulkan call —
readable from the layer itself, which is on the Linux side, so this is fine, but it is **not**
readable from our D3D12 plugin, so the layer has to do its own logging.

**(b) A vkd3d-proton patch. ~30-50 lines, and the natural home. Weekend plus a packaging burden.**
Add `present_metering` to `device.c`'s extension table and feature-struct chain, a field on
`struct dxgi_vk_swap_chain`, an env var (or a new `IDXGIVkSwapChain` method) to set it, and the
`vk_prepend_struct(&present_info, &metering_info)` beside the existing `present_id` /
`timings_info` chaining at `swapchain.c:3080-3090`. The cost is not the patch, it is shipping a
custom `d3d12core.dll` into the Proton runtime and keeping it current.

**(c) The "proper" chain: dxvk-nvapi implements `NvAPI_D3D12_SetFlipConfig` and forwards into a new
vkd3d-proton receiver.** What upstream will eventually need for Streamline titles, and the exact
shape of the existing `NotifyOutOfBandCommandQueue` chain (§1.4). Two patches in two projects,
plus decisions about `vidpnSrcID` (ignore it) and about which swapchain a command queue maps to.
Weeks, not days. **Worth filing an issue regardless**, because the present state — advertising
`SET_FLIP_CONFIG_V2` as supported while the entry point returns `nullptr`, with the commit message
"And pretend that SET_FLIP_CONFIG_V2 is supported" (§1.3) — is at minimum surprising and is the
best available hypothesis for the upstream "runs but paces wrong" reports.

### 5.4 What it would actually buy us here, stated conservatively

Under gamescope, the driver's `vkQueuePresentKHR` is a `wl_surface` commit, not a flip. So
metering can only mean *the driver spacing its own commits*. That is:

* **nearer the display than our pacer** — inside the driver, after our thread, after DXVK, after
  vkd3d's blit and queue submit — so it should absorb jitter our pacer cannot see, including the
  Wine `condition_variable::wait_for` skew we had to spin around (`fg_present.cpp:721-751`);
* **still software on Ada** (SOFT, §2.4), so the ceiling is a better CPU pacer, not a hardware one;
* **still upstream of gamescope's latch**, so §3.4's dropped-present hazard is untouched — though
  even spacing plausibly keeps the two commits in different gamescope wakeups, which would help
  it.

A real but bounded prize, which is why §8 ranks it below the free experiments.

### 5.5 There is no Linux kernel equivalent, and that is the hard floor

DRM/KMS takes **one flip per atomic commit**. There is no ioctl to hand the kernel a batch of N
flips with a metering request, and gamescope keeps at most one commit outstanding anyway (§3.3).
So the *display-engine* form of flip metering has no Linux API to arrive through, on any GPU,
today. Adding one would be a kernel uAPI change plus a compositor change plus a driver change —
**a year-scale, multi-project effort**, and not something this project can influence.

### 5.6 A second lever nobody is using: gamescope serves `VK_GOOGLE_display_timing` itself. HARD.

NVIDIA does not expose `VK_GOOGLE_display_timing` on Linux (§5.2). **gamescope's WSI layer does**:

```cpp
// layer/VkLayer_FROG_gamescope_wsi.cpp:1031-1035
static constexpr std::array<VkExtensionProperties, 2> s_LayerExposedExts = {{
  { VK_EXT_HDR_METADATA_EXTENSION_NAME,      VK_EXT_HDR_METADATA_SPEC_VERSION },
  { VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME, VK_GOOGLE_DISPLAY_TIMING_SPEC_VERSION },
}};
```

and in `QueuePresentKHR` it forwards the request straight to the compositor:

```cpp
// layer/VkLayer_FROG_gamescope_wsi.cpp:1447-1458
if (pPresentTimes && pPresentTimes->pTimes) {
    gamescope_swapchain_set_present_time(
      gamescopeSwapchain->object,
      pPresentTimes->pTimes[i].presentID,
      pPresentTimes->pTimes[i].desiredPresentTime >> 32,
      pPresentTimes->pTimes[i].desiredPresentTime & 0xffffffff);
```

It also implements `vkGetPastPresentationTimingGOOGLE` and `vkGetRefreshCycleDurationGOOGLE`
(`:1640-1666`) — though the "actual" present time it reports is gamescope's own prediction
(§3.3), not a pageflip timestamp.

**And vkd3d-proton never uses it.** A `GOOGLE_display_timing|PresentTimesInfoGOOGLE|
desiredPresentTime` grep over vkd3d-proton's `libs/` and `include/` returns **zero hits**.

So there is a **compositor-level "do not present this frame before time T" channel, offered by the
entity that actually owns the flip, and nothing in our chain asks for it.** Its consumer is the
latch at `steamcompmgr.cpp:7715-7718`, so the semantics are "not before T", not "exactly at T" —
which is precisely what a pacer holding a generated frame needs. It requires no NVIDIA extension,
no kernel change, and no upstream patch: the same ~150-line layer from §5.3(a) could chain
`VkPresentTimesInfoGOOGLE` instead of, or as well as, `VkSetPresentConfigNV`, and it targets a
mechanism that is HARD-confirmed to be honoured rather than one whose behaviour is closed.

**On this stack, that may be the better first experiment of the two**, and it was not on the
original list of alternatives. What it cannot do is beat gamescope's vblank granularity, and its
effect under `FlipType::VRR` (where gamescope already commits on demand) is **UNCONFIRMED**.

---

## 6. Other things that exist and are worth knowing

### 6.1 The NVIDIA Linux driver has its own frame-generation present layer, and it supports Ada. SOFT (vendor README).

NVIDIA Linux driver README `nvpresent.html` (read at 595.58.03) documents **NVIDIA Smooth
Motion**: *"a driver-based AI feature"* that *"delivers smoother gameplay by inferring an
additional frame between two rendered frames"*, implemented as the **`VK_LAYER_NV_present`
implicit Vulkan layer, which overrides the application's presentation to inject additional
frames.** Supported on **"GeForce RTX 40 Series and newer GPUs"**. Knobs:
`NVPRESENT_ENABLE_SMOOTH_MOTION=1`, `NVPRESENT_LOG_LEVEL=4`, `NVPRESENT_LOG_FILE`,
`NVPRESENT_QUEUE_FAMILY=1`. It *"presents from an asynchronous compute queue which can cause
issues with third party overlays"*. The README says **nothing** about how it paces; gpuinfo report
49069 also lists the undocumented `VK_NV_internal_nvpresent`, almost certainly that layer's
private interface.

**Why this matters even though it is not our frame generator: it is a reference measurement.** One
launch with `NVPRESENT_ENABLE_SMOOTH_MOTION=1` and our FG **off** shows how well NVIDIA's own
present-injecting frame generator paces on this exact stack, under this exact gamescope, on this
exact card. If it is visibly better than ours there is something to chase and §5 is where it
lives; if it is the same or worse, our pacer is at the platform's ceiling and the answer to the
user's question is "we already have the best thing available here". Cheap, decisive, never made.

### 6.2 The one Reflex call we still do not make

`NvAPI_D3D12_NotifyOutOfBandCommandQueue` remains the single Reflex-surface call Streamline makes
that we do not — the earlier audit's §4 recommendation, unchanged and still **UNCONFIRMED as
necessary**. It is fully plumbed on this stack (§1.4), it is one call on the queue our present
owner submits the generated-frame present through, and it is the correct way to tell Reflex/PCL
that those submissions sit outside the game's instrumented frame. It is not a pacing mechanism and
will not fix a cadence, but it is cheap and on the same code path. It has also become more
interesting since §6.3: NVIDIA's own PR #2938 turns on exactly this attribution problem.

### 6.3 Upstream state: nobody has claimed the pacing job

**vkd3d-proton [#2325](https://github.com/HansKristian-Work/vkd3d-proton/issues/2325)** — "DLSS
Frame Generation causes stuttering and breaks VRR", **open since 2025-02-01**, 8 comments, no
label, no assignee. Titles: Deep Rock Galactic, Like a Dragon: Infinite Wealth, Kunitsu-Gami,
Bright Memory: Infinite, Tokyo Xtreme Racer; Cyberpunk 2077 was the exception where VRR engaged
but *"frame generation seemingly doesn't actually happen, leading to a double-image effect."*
RTX 4070 Super, driver 570.86.16, Proton 9.0-4. The reporter found that disabling VRR and matching
the refresh rate "alleviated" it "albeit still present due to uneven frame pacing", and that FSR 3
FG in the same games "performs without any stutter."

**The only maintainer comment, `doitsujin`, 2025-02-02 (HARD):**

> "In general I don't know how actionable frame gen issues are in general, I also don't know to
> which extent DLSS3 can even *support* even frame pacing (e.g. with FSR, the app is responsible
> for calling mid-frame Present at a relatively specific time to alleviate this), but even if it
> does this seems to be outside our control."

**No maintainer in that thread ever names a mechanism**, and nobody mentions flip metering,
present metering or the compositor. The nearest thing to a diagnosis is a non-maintainer
(FakeMichau, 2025-03-02, SOFT) observing that the same behaviour reproduces on **AMD** via
dlssg-to-fsr3 + OptiScaler, hence "some interaction between the sl pacer and linux". Still live as
of 2026-05-21 on an RTX 5070.

**The load-bearing upstream work is PR #2938** (merged 2026-04-21, `wcampbell-nv` at NVIDIA), and
it is about frame attribution rather than pacing:

> "vkd3d-proton interferes with this by deferring present calls to a background thread. … **The
> `VkPresentIdKHR` frame attribution scheme does not work with DLSS Frame Generation, which
> submits multiple OUT_OF_BAND presents with the same low latency frame id.**"

with `HansKristian-Work` reviewing: *"Locking LL2 ID to present ID has been rather miserable and
fixing that would remove most of the problems we have with LL2 I think."* It shipped in v3.0.1 as
*"Rather than trying to lock NVAPI present ID to our present ID, these can now be decoupled. This
helps frame-gen in particular."* And the author's own status, 2026-04-21:

> "There are some issues with `vkQueueSubmit` attribution remaining, **which can cause
> stutter/subpar latency in games with frame gen enabled. Fixing this will require changes to both
> the NVIDIA driver and vkd3d-proton. We're still working out the details on this.**"

**vkd3d-proton `c8c8ab50`** (PR #2930, 2026-04-09) is real but is **not** a pacing fix: it stops
`get_latency_info` zeroing *all* latency reports when any present ID is non-stride-aligned, which
had made Streamline conclude `eDLSSGStatusFailReflexNotDetectedAtRuntime` and **freeze** — *"…
particularly when switching workspaces on Wayland compositors like Hyprland."* Freezes, not
cadence.

**Does MFG pace correctly under Proton? Runs, partly; paces, no.** dxvk-nvapi #317 established
that the 3x/4x DRS override is delivered correctly (`jp7677`: *"we don't do anything wrong in this
regard"*) and that whether the game acts on it is the game's business. The user-report symptom
vocabulary is: "frames completely out of order", "double-image", "every second frame is a
duplicate", "inverse ghosting — as if the generated frame is being inserted after the next real
frame", tearing at the bottom of the screen. Worst on 40-series; several report 50-series MFG
"copes pretty well" — which is what §2.4 predicts. Workarounds, by strength of attestation:
replace the game's bundled Streamline with ≥2.8.0 (the actual resolution of dxvk-nvapi #245 and
vkd3d-proton #3232); cap frames below refresh/N with VRR and vsync off; toggle the multiplier at
runtime to clear the out-of-order state; `WINEDLLOVERRIDES="sl.dlss_g=d"`.

**And one that should temper §3.2's recommendation.** In those same threads, gamescope is reported
as a *fix* for FG problems rather than a cost — dxvk-nvapi #336, *"framegen seems to require
gamescope to work"*; and `grumd` on #245 reports a stable configuration of
`gamescope -f -w 3440 -h 1440 -r 240 --hdr-enabled --hdr-itm-enabled --force-grab-cursor
--adaptive-sync` → *"VRR, HDR, FG, RT/PT, no issues, very stable."* **That is very nearly the
user's own command line, including `--hdr-itm-enabled`.** So §3.2's finding stands as a *cost*
claim (a mandatory composite, and an ITM shader that source says is unreachable) and must **not**
be read as "these flags are breaking FG" — someone's working setup has them. SOFT, one user.

---

## 7. The experiments, cheapest first — and what each one settles

None needs a rebuild of our plugin. Items 1-4 are single launches.

1. **`NVPRESENT_ENABLE_SMOOTH_MOTION=1` with our FG off.** Settles §6.1: how well does *NVIDIA's
   own* present-injecting frame generator pace on this stack? This is the reference measurement
   our pacer has never had, and it is one environment variable. **Run this first** — it can tell
   us the whole enquiry is unnecessary.
2. **`VKD3D_DEBUG=info VKD3D_SWAPCHAIN_PRESENT_MODE=FIFO`, FG off.** Settles §4.3 gate 2 — does the
   gamescope Wayland surface report `presentTimingSupported`? Look for the absence of
   `Implementation supports neither present_wait1 or present_wait2` and for
   `Cannot implement fractional present timing with current setup, falling back to CPU limiter.`
   Do **not** combine with `VKD3D_FRAME_RATE` on the first run (one pacer at a time), and know the
   present-wait failure mode from §4.4 before running it.
3. **Count displayed frames, not presents.** Settles §3.4. Our `[fg]` line proves 2.00× presents;
   nothing has proved 2.00× *displayed* frames under gamescope's MAILBOX latch. gamescope's own
   `backend_info` ConCommand dumps present counters (`backend.cpp:187-202`); a capture at the panel
   would also do it. This is the experiment most likely to find a real bug.
4. **Drop `--hdr-itm-enabled` (and, separately, `--hdr-enabled`).** Settles §3.2's cost claim: a
   full-frame composite per frame for, from source, no tone mapping. Read gamescope's own verdict
   by enabling its DRM debug scope (`gamescope_log_drm='debug'` — the override is evaluated as
   Lua, so the quotes probably matter; **UNCONFIRMED**) and grepping for
   `can drm present %i layers` / `can NOT drm present %i layers` (`DRMBackend.cpp:2863-2866`).
   Judge the image, and keep §6.3's caveat in mind: someone's stable FG configuration includes
   these flags.
5. **Build the layer (§5.3a).** Two variants, and I would try them in this order:
   **`VkPresentTimesInfoGOOGLE`** first (§5.6 — the mechanism gamescope demonstrably honours), then
   **`VkSetPresentConfigNV`** (§5.1 — the mechanism whose stated purpose is exactly this, with
   `presentConfigFeedback` to read). Read `bevyengine/dlss_wgpu` PR #34's `present_metering` module
   before writing the second one.

## 7.1 Interaction with the ~1 Hz blip — and this needs saying plainly

The user is partly asking for hardware pacing *because* of a blip roughly every second. **Nothing
in this document would fix that**, and the reason is worth stating now rather than discovering
later:

* Every mechanism here — flip metering, present metering, present timing, GOOGLE display timing,
  our own scheduler — decides **when an already-submitted frame is shown**. None can manufacture a
  frame that was never presented, and none can absorb a stall in the process that produces the
  presents.
* This project has already characterised that symptom class: MEMORY records *"indicator+image
  blink with audio cuts = a ~25 ms single-frame stall under every host; attribute it, never bisect
  it as a DLSS refusal."* A stall in our own process is **upstream of hop 2** in §3.1.
* So if the cause is mundane — periodic diagnostic writes, as currently suspected — **the value of
  this whole line of enquiry drops sharply**, because pacing was never the problem. The pacer is
  already delivering a measured flat 6.0-6.2 ms.

Two findings here do interact, and both are *different bugs* rather than pacing fixes:

* **§3.4**, gamescope silently dropping a same-wakeup present, is the one mechanism in this
  document that could produce a periodic visible artefact. Experiment 3 tests it.
* **§4.4 / gamescope #1592** describes an NVIDIA + gamescope-WSI + `VK_KHR_present_wait` hang whose
  reported symptom is *"frames stop being presented … audio continues while the picture freezes"*
  on an **RTX 4090**. That is close enough to the recorded blink-with-audio-cut signature to be
  worth keeping on the list as a candidate cause — while noting it was reported on driver
  565.57.01 and fixed in a 570 beta, and that on our current `SyncInterval = 0` configuration
  vkd3d is **not** calling `vkWaitForPresentKHR` at all (§4.4). Filed here, not investigated.

---

## 8. Ranked verdict

**1. Reachable now, no code, and worth doing regardless of pacing:**
* `NVPRESENT_ENABLE_SMOOTH_MOTION=1` as a reference pacing measurement (§6.1, experiment 1). One
  env var, and it could end the enquiry.
* Count displayed frames rather than presents (§3.4, experiment 3). Most likely to find a real bug.
* `NvAPI_D3D12_NotifyOutOfBandCommandQueue` on the present-owner queue (§6.2) — one call, already
  plumbed end to end, still unproven as necessary, and now corroborated as the right *kind* of
  concern by NVIDIA's own PR #2938.
* Reconsider `--hdr-itm-enabled` (§3.2) — a mandatory full-frame composite for an ITM shader that
  source says is unreachable. A cost claim, not a breakage claim (§6.3).

**2. Reachable with a named change, and the change is ours, not upstream's:**
* **A ~150-line implicit Vulkan layer (§5.3a). A weekend.** Two payloads, and I would try the
  second-listed mechanism first:
  * `VkPresentTimesInfoGOOGLE` (§5.6) — gamescope serves the extension and **HARD**-confirmed
    forwards `desiredPresentTime` to the compositor that owns the flip; vkd3d-proton never asks.
  * `VkSetPresentConfigNV` (§5.1-5.2) — the API whose stated purpose is frame-generation batch
    pacing, **HARD**-confirmed live on this GPU and driver, `pNext` **HARD**-confirmed to survive
    gamescope's layer, with `presentConfigFeedback` to read and one open-source precedent to copy
    (`bevyengine/dlss_wgpu` PR #34).
  Bounded payoff (§5.4): a better CPU pacer sited inside the driver or the compositor, not a
  hardware one.
* `VK_EXT_present_timing` via vkd3d-proton's existing pacer (§4) — **already implemented
  upstream**, already in the box's build, gated on FIFO and on a surface capability that now looks
  more likely supported than not (§4.3). Free to test (experiment 2); a design change to use
  (hand pacing off to vkd3d and run `NgxFGPacing=0`), with a present-wait risk (§4.4).

**3. Reachable with a named upstream change, and someone should file it:**
* dxvk-nvapi implementing `NvAPI_D3D12_SetFlipConfig` and forwarding into a new vkd3d-proton
  receiver that chains `VkSetPresentConfigNV` (§5.3c). Two projects, weeks. Independently worth an
  issue: dxvk-nvapi today tells the NGX runtime `SET_FLIP_CONFIG_V2` is supported while the entry
  point returns `nullptr`, and if `sl.dlss_g` uses that answer to skip its own software pacer, that
  is a plausible cause of "runs but paces wrong" for every Streamline title under Proton since
  2026-05 (§1.3). It does not affect us.

**4. Genuinely blocked, and by what:**
* **Display-engine ("hardware") flip metering, blocked by the GPU.** Blackwell shifts pacing to the
  display engine; *"DLSS 3 Frame Generation used CPU-based pacing"*. This is Ada. **SOFT** (vendor
  material) but unrefuted — and per Streamline #65 the fallback branch NVIDIA itself takes in that
  case is a `Present()`-metering CPU pacer, i.e. **what we already have.**
* **Any path where the driver owns the queue of flips to the display, blocked by gamescope.** The
  flip is gamescope's own `drmModeAtomicCommit`, one outstanding, from its own thread, with no
  target-time property in the atomic property table. **HARD.** No gamescope flag changes it; only
  not running gamescope would, and that costs the HDR path (which §3.2 suggests is doing nothing
  for this title anyway). An option, not a recommendation.
* **A kernel flip-batching uAPI, blocked by not existing** (§5.5). Year-scale, multi-project, out
  of reach.

**So: the bar the user set — "beat a software pacer that already works" — is not met by anything
available today, and the strongest form of what was asked for does not exist for this card on any
operating system.** What *is* available is two ways to move the final wait out of our Wine-hosted
thread and into either the driver or the compositor, at about a weekend each, with unmeasured
payoff. The honest expectation is a modest jitter improvement, not a step change. And if the ~1 Hz
blip turns out to be periodic diagnostic writes, experiment 1 in §7 is worth more than all of it.

---

## 9. What I could not obtain, stated plainly

* **Whether the gamescope Wayland surface reports `presentTimingSupported` / `presentWait2Supported`.**
  gpuinfo report 49069 carries device features, not surface capabilities. §4.3's revised
  expectation (probably open, on the strength of NVIDIA's Wayland present-timing bug fixes) is
  **SOFT**, not measured. Experiment 2 settles it.
* **Whether Ada honours present metering in the display engine, in a driver thread, or not at
  all.** The feature bool is TRUE (**HARD**); the behaviour behind it is closed.
  `presentConfigFeedback` is the only channel and nobody has read it. There is also **no way to
  detect that a compositor has made metering meaningless** — the spec defines no such feedback.
* **Whether `VK_LAYER_NV_present` (Smooth Motion) itself uses present metering.**
  `VK_NV_internal_nvpresent` is in the report and undocumented.
* **`bevyengine/dlss_wgpu` PR #34's diff and comments** — rate-limited before they could be read.
  Only the PR body is quoted here.
* **Whether the ITM shader path is truly never taken at runtime.** The specialization-constant ID
  mismatch and the missing arguments are both HARD from source; nobody has run gamescope to
  confirm no tone mapping appears on screen.
* **libliftoff's plane-assignment test commits** — `subprojects/libliftoff` is empty in a shallow
  clone, so whether NVIDIA planes would accept two `R10G10B10A2` layers is runtime-only. Moot
  while §3.2's two gates force a composite.
* **`sl.dlss_g.dll` itself.** Closed. Everything about the SL pacer's algorithm, and about whether
  it consults `NvAPI_NGX_GetDriverFeatureSupport` to choose its pacing mode (§1.3's hypothesis),
  remains inference from strings, documentation and issue comments.
* **GitHub's code-search API** (401 without auth), so no repo-wide code grep; every "not present"
  claim here is a local grep of a shallow clone at a named SHA. Note that GitHub's *repo-scoped*
  code search returns **false zeros** — it reported no `present_wait` in gamescope, which
  demonstrably has it. Do not trust it for negatives.
* **The gamescope-side effect of a `desiredPresentTime` under `FlipType::VRR`**, where gamescope
  already commits on demand. §5.6's lever may be a no-op in exactly our configuration.

---

## Appendix: the method note, because it caught me three times in one document

Three structural arguments died the same way here, and each time a single primary source killed
them in under a minute:

1. *"The private NVAPI entry does not exist on Linux."* It is not private — NVIDIA publishes it,
   with a full doc block — and its presence in the box's DLL was a compiled name table, not an
   implementation. **A string is not an implementation, and "private" is not a property you can
   assume about a documented API.**
2. *"Hardware flip metering is Blackwell, therefore there is nothing to reach on Ada."* The
   *display-engine* claim survives; the *API* claim did not. One gpuinfo report for the exact GPU
   and driver showed `presentMetering == true` — after I had spent an hour trying to infer a
   hardware gate from percentage coverage on a mixed-vendor corpus. **Check one real record before
   deriving a capability from aggregate statistics.**
3. *"gamescope has no timing protocol, so the surface cannot support present timing."* True of
   `wp_commit_timing_v1` and `wp_fifo_v1`, and beside the point: gamescope implements
   `VK_GOOGLE_display_timing` in its **own WSI layer** and forwards `desiredPresentTime` to the
   compositor, and NVIDIA has been fixing `VK_EXT_present_timing` bugs specifically for Wayland.
   **Absence of the protocol you looked for is not absence of the capability.**

The rule `docs/RESEARCH-STREAMLINE-INTERNALS.md` §7.2d already wrote down applies again, and is
now several for several in this project: **when the argument is "platform X cannot do Y", go and
check whether someone has already made it do Y — especially when the vendor has an incentive to.**
In this case someone had: `bevyengine/dlss_wgpu` PR #34 is, as far as anyone can tell, the only
existing implementation of NGX frame generation paced with present metering, and it was opened
three weeks before this question was asked.
