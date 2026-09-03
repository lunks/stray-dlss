# Auditing stray-dlss against NVIDIA's current UE5 DLSS / Streamline plugin

`docs/RESEARCH-OFFICIAL-DLSS-UE-PLUGIN.md` (828 lines, 2026-08-31) already established the
architecture question — hook vs. stage, `ITemporalUpscaler`, `ISceneViewExtension`, Streamline's
present-time model — against a UE5-era mirror of a DLSS2/3-vintage plugin and the generic
Streamline 2.12.0 SDK guides. That document is not repeated here. This one exists because NVIDIA
kept shipping: DLSS4, Multi Frame Generation, new presets, a real DLSS4 UE5.6 sample project with
a bundled Streamline Core plugin. The question is narrower and more concrete: **now that we can
read the actual current source, where does our hand-built SR/FG implementation diverge from what
NVIDIA does today, and is any divergence worth fixing?**

Companion documents, not repeated: `docs/RESEARCH-OFFICIAL-DLSS-UE-PLUGIN.md` (architecture),
`docs/RESEARCH.md` (NGX D3D12 SDK / UE 4.27 internals, 228 claims), `docs/STRAY-RENDERING-FACTS.md`
§32 (the raw `nvngx_dlssg.dll` parameter contract, reverse-engineered from Streamline 2.12.0's
snippet binary — cited extensively below because it turns out to cross-validate against the real
UE5 source read here), and the `dlss-exposure` branch (auditing our exposure handling against the
UE4 plugin — not duplicated; §3 below covers only what changed for UE5/DLSS4).

**Architecture note, since the companion document is ReShade-era and this one must not silently
inherit that framing.** `docs/RESEARCH-OFFICIAL-DLSS-UE-PLUGIN.md` (2026-08-31) describes "our"
side as a ReShade add-on intercepting the TAA compute dispatch through ReShade's device/
command-list event API. **That is no longer how this project loads or hooks.** As of this
writing we are a **UE4SS C++ plugin** (`mods/StrayDLSS`, built by
`.github/workflows/dlss-plugin.yml` against RE-UE4SS) loaded in-process on the game's main
thread, ahead of `D3D12CreateDevice`; the actual D3D12 interception — device/command-queue/
command-list/swapchain vtable patching, our own descriptor and root-argument shadow
(`src/backend_native/descriptor_shadow.cpp`, `root_shadow.cpp`), the TAA-dispatch capture, the
present-twice frame-generation path (`src/backend_native/present_owner.*`, `fg_present.*`) — is
a plain native hook layer (`src/backend_native/`) that does not depend on ReShade's add-on
events at all. A `src/backend_reshade/` backend still exists in the tree; it is not how the
project is run today. `docs/RESEARCH-UE4SS-MIGRATION.md` (2026-09-01, HARD, this repo) is the
assessment that led to this, and its verdict matters directly for §1 below: **UE4SS itself
contributes UObject/Blueprint reflection, cvars, and deterministic camera control — it
"contains no D3D12 or DXGI code whatsoever... and cannot contribute a line of it"** to the
render-level interception, which remains a plain Win32/D3D12 hook exactly as it was under
ReShade. Where a claim below depends on this distinction, it says so explicitly; the
TAA-dispatch interception *technique* itself (identify by hash, capture by register, replace
the dispatch) is unchanged — only the mechanism that installs the hooks and the process
that hosts them changed.

**Provenance labels**, per `CLAUDE.md` §0.5: **HARD** — read directly from the source quoted;
**SOFT** — documentation, release notes, a vendor blog, or a claim whose engine/SDK version
differs from what we read; **UNCONFIRMED** — searched for and not found, or inferred without a
citation, with a note on what would confirm it.

---

## 0. Sources examined, and how far each can be trusted

| Source | What it is | Trust |
|---|---|---|
| `XPOL555/unreal-engine-5.7-dlss4` (GitHub, public, retrieved 2026-09-03) | A blank UE 5.7.0 project into which the author "copied/merged the content from NVIDIA's DLSS 4 sample project for UE 5.6" (the repo's own `README.md`) and made "minor code fixes to account for API and build system differences between UE 5.6 and UE 5.7." Every `.cpp`/`.h` file quoted below carries an intact `Copyright (c) 2020 - 2025 NVIDIA CORPORATION & AFFILIATES` or `Copyright (c) 2022 - 2025 NVIDIA CORPORATION & AFFILIATES` header with NVIDIA's proprietary-rights notice, unmodified | **HARD** that the quoted code is NVIDIA's own, by the same test the companion doc used (intact copyright); **SOFT** that any single line is byte-identical to a pristine UE 5.6 checkout — the porter's own README admits edits for 5.7 compatibility, though none are claimed to be functional |
| `Plugins/DLSS/DLSS.uplugin` | Version manifest inside the above | **HARD** — gives the exact plugin version examined: `8.3.0-NGX310.4.0-EXPERIMENTAL_FORUM` (the `-EXPERIMENTAL_FORUM` suffix is the porter's own addition per the README; the `8.3.0-NGX310.4.0` core is NVIDIA's version string), `EngineVersion 5.7.0` |
| `Plugins/StreamlineCore/StreamlineCore.uplugin` + `sl_version.h` | Same, for the bundled Streamline Core | **HARD** — `8.3.0-SL2.9.0`, and `sl_version.h` itself reads `SL_VERSION_MAJOR 2 / MINOR 9 / PATCH 0`. **This is older than the newest public Streamline SDK** (`NVIDIA-RTX/Streamline` v2.12.0, 2026-06-23 — the release `docs/STRAY-RENDERING-FACTS.md` §32 read its `nvngx_dlssg.dll` contract from). Anything version-gated between 2.9.0 and 2.12.0 (Dynamic Multi Frame Generation's `dynamicTargetFrameRate`, the VSync fixes, the RR responsivity-mask buffer) is **not** in the source read here — flagged inline where it matters |
| `NVIDIA-RTX/Streamline` `changelog.txt`, `docs/ProgrammingGuideDLSS_G.md` (GitHub, fetched live) | NVIDIA's own public SDK repo | **HARD** |
| Web search (DLSS 4.5, multi-frame-gen announcements) | NVIDIA/press coverage, January 2026 CES and later | **SOFT** — used only to date-stamp "DLSS4.5" as a real, later thing; no source code for it was obtained, so no claim below rests on it |
| `NvRTX/UnrealEngine` `dlss3/sl2-*-dlss-plugin` branches | NVIDIA's actual distribution mechanism for the UE-integrated plugin (linked to GitHub accounts that have accepted Epic's UE EULA) | **Could not obtain — 404 over the API, gated.** Not used for any claim below |
| `flygod1159/Nvidia-DLSS-Plugin` (used by the companion doc) | Stale, last commit 2022-02-18, DLSS2/3-era, UE 5.0 branch | Not re-read here; superseded by the DLSS4 source above wherever they overlap |

**What this means for citations below**: file paths are given relative to the plugin roots
(`Plugins/DLSS/...`, `Plugins/StreamlineCore/...`) as they exist in the source read, with line
numbers from that exact copy. A pristine NVIDIA UE 5.6 checkout could differ by a handful of
lines from build-system churn; no claim below depends on an exact line surviving that.

---

## 1. Where DLSS is placed in the frame

`docs/RESEARCH-OFFICIAL-DLSS-UE-PLUGIN.md` §A.1–A.2 and §D.2 established that `ITemporalUpscaler`
is a genuine engine interface, unreachable from ReShade's D3D12 event model. **That framing is now
stale in one specific way and needs re-examining, not re-argument-by-inertia, because we are no
longer a ReShade add-on.** We are a UE4SS C++ plugin: a native DLL loaded in-process, ahead of
device creation, with UE4SS's UObject/Blueprint reflection and pattern-scan/hook infrastructure
available as tooling. The honest question is whether that changes the answer, and the honest
answer has two parts.

**What genuinely changed: nothing about `ITemporalUpscaler` itself is UObject-reflected, so UE4SS's
own reflection does not reach it — and this project's own prior assessment already establishes
that precisely, for the adjacent D3D12 problem.** `FSceneViewFamily`, `ITemporalUpscaler` and
`ISceneViewExtension` are plain internal renderer C++ classes with no `UCLASS`/`UFUNCTION`
metadata — UE4SS's reflection targets UObjects (as `mods/StrayDualSense` already exercises,
hooking `COMP_CatScratchableComponent_C:SetPS5TriggerActivated`, a genuine `UFUNCTION`), not
private renderer-thread C++. `docs/RESEARCH-UE4SS-MIGRATION.md`'s own verdict, reached from
inside this project for a closely related question, says the same thing in stronger terms: UE4SS
"contains no D3D12 or DXGI code whatsoever... and cannot contribute a line of it" to the
render-level interception, which the migration built as "a plain Win32/D3D12 hook layer" instead
(`src/backend_native/`) — precisely because UE4SS's tooling does not extend to this class of
internal engine object. `SetTemporalUpscalerInterface` sits in exactly that same class: reaching
it would need pattern-scanning and hooking a specific, private, renderer-thread function
(something like `AddPostProcessingPasses` or the game's analogue of
`SetupMainGameViewFamily`/`BeginRenderViewFamily`), with real UE 4.27.2 struct-layout knowledge of
`FSceneViewFamily` — not a UFunction call, not a UObject property read, and not anything UE4SS's
SDK-dumping tooling generates automatically.

**What genuinely did not change: this is now a substantially more *plausible*, still entirely
*unbuilt*, engineering problem — not a solved one.** Being a native in-process DLL loaded early is
necessary for this kind of hook and was, in fact, equally true of a ReShade add-on (also a native
in-process DLL) — so "in-process" alone was never the blocker the earlier framing implied, and is
not one now either. What UE4SS adds is tooling that lowers the *cost* of the remaining problem:
AOB/pattern-scan infrastructure, a live SDK dump for this exact game build, and (per the migration
assessment) a track record of this project already doing the harder, adjacent version of this work
— its own hand-built descriptor/root-argument shadow replacing ReShade's equivalent
(`docs/RESEARCH-UE4SS-MIGRATION.md` §1.1, "L, and the only item with real technical risk"). No
part of that shadow work, and nothing in the current `mods/StrayDLSS/src/Mod.cpp` (94 lines, the
*only* file that includes a UE4SS header at all — its `on_unreal_init` is empty, and none of the
render-path code touches a UObject), currently reaches `FSceneViewFamily` or
`ITemporalUpscaler`. The View constant buffer is still read by raw byte offset
(`CLAUDE.md` §2.6), and the TAA pass is still identified by DXBC hash and intercepted as a compute
dispatch (`src/taa_hook.cpp`) — exactly the ReShade-era technique, now installed through our own
vtable patches instead of ReShade's `addon_event::dispatch`.

**So: reclassify, do not repeat verbatim.** The old framing ("unreachable from ReShade's D3D12
event model") is not wrong, it is simply the wrong *reason* now — the real constraint was never
ReShade specifically, it is that `ITemporalUpscaler` is non-reflected internal engine state
requiring ABI-level knowledge to reach from outside, which is exactly as true for a UE4SS native
plugin as it was for a ReShade add-on. What UE4SS changes is that the *next* step toward reaching
it — AOB-scanning and hooking the specific renderer-thread function that registers it — is now a
scoped, tooled, and precedented kind of work for this project, rather than a wall. **Investigate,
long-horizon, not now**: this is squarely a candidate for a future `docs/RESEARCH-UE4SS-MIGRATION.md`-
style assessment of its own, not something to attempt as a side effect of this audit.

**What changed since that read (registration mechanism, UE5.4+):** DLSS SR now registers through
a real `ISceneViewExtension`, not only through `ICustomStaticScreenPercentage`.
`Plugins/DLSS/Source/DLSS/Private/DLSSUpscaler.cpp:598-628`,
`FDLSSUpscalerViewExtension::BeginRenderViewFamily`:

```cpp
if (!ViewFamily.GetTemporalUpscalerInterface())
{
    GetGlobalDLSSUpscaler()->SetupViewFamily(ViewFamily);
}
else
{
    UE_LOG(LogDLSS, Error, TEXT("Another plugin already set FSceneViewFamily::SetTemporalUpscalerInterface()"));
    return;
}
```

**HARD.** `SetupViewFamily` (`:1265-1341`) is what actually calls
`ViewFamily.SetTemporalUpscalerInterface(new FDLSSSceneViewFamilyUpscaler(...))` — same
mechanism the companion doc found, reached one layer further out. This is a UE-version-driven
refactor (view extensions are the modern, ordered way plugins hook a view family), not a change
in what the interface hands DLSS: still exactly the three textures plus two downsample hints
per `AddPasses`, nothing more.

**New in this refactor: a "Modular Feature" indirection.** `DLSSUpscaler.cpp:17`
(`#include "DLSSUpscalerModularFeature.h"`), gated `#if ENGINE_SUPPORTS_UPSCALER_MODULAR_FEATURE`
(`:506-513`, `:1286-1294`): another plugin can register a functor that overrides, per view,
whether DLSS's view extension is active this frame and which quality mode it should use. **HARD**
that this exists; **not applicable to us** — it is a plugin-to-plugin extension point inside the
engine's own C++ object graph, reachable only from inside the process's UE code, which is exactly
the "would need real engine execution, not a D3D12 hook" wall the companion doc already names.

**Ray Reconstruction: refined, not new.** The companion doc's §C read RR's placement from the
*generic* Streamline guide ("Default up-scaling pass like TAAU goes here"). The actual UE plugin
source is more precise, and worth stating exactly because it resolves an ambiguity the generic
doc left open — RR does **not** run as a second, independent evaluate layered after a denoiser
callback. It is the *same* `ITemporalUpscaler` object, parameterized:

```cpp
// DLSSUpscaler.cpp:415-421 (GetDebugName)
ENGXDLSSDenoiserMode DenoiserMode = GetDenoiserMode(Upscaler);
return (DenoiserMode == ENGXDLSSDenoiserMode::DLSSRR) ? GDLSSRRSceneViewFamilyUpscalerDebugName : GDLSSSceneViewFamilyUpscalerDebugName;
```

`FDLSSDenoiser` (`DLSSDenoiser.h:20`, `class DLSS_API FDLSSDenoiser final : public IScreenSpaceDenoiser`)
is registered too, but only to **skip** the engine's own screen-space denoiser passes when RR is
active (`DLSSDenoiser.cpp:44-49`, `SkipDenoiser` returns true whenever `r.NGX.DLSS.DenoiserMode`
says DLSSRR) — it performs no denoising work itself; the real RR evaluate happens inside
`FDLSSSceneViewFamilyUpscaler::AddPasses`, the identical call site SR uses. **HARD.** So: one
temporal-upscaler slot handles SR and RR both, plus a companion no-op-shaped `IScreenSpaceDenoiser`
registration that exists purely to stop the built-in SSD from doing redundant work. This
refines, and does not contradict, the companion doc's conclusion that RR occupies SR's slot.

**Quality modes: six now, not five.** `DLSSUpscaler.cpp:454` asserts
`static_assert(int32(EDLSSQualityMode::NumValues) == 6, ...)`; the new value is `UltraQuality`,
sitting between `Quality` and `DLAA` (`:657-663`, `ToNGXQuality` maps it to
`NVSDK_NGX_PerfQuality_Value_UltraQuality`). **HARD.** Our own `DlssQuality` enum
(`src/ngx_backend.hpp`) has `dlaa`, `max_quality`, `balanced`, `max_performance`,
`max_performance` (ultra-performance folded into max_performance's branch) — no `ultra_quality`.
**Investigate:** a render/output ratio that would select `UltraQuality` on the real plugin
currently falls into whichever of our five buckets its ratio threshold lands in; low priority,
since Stray only ever runs at the game's two shipped screen percentages (50%/70%, `CLAUDE.md`
§2.3.1) and neither is close to `UltraQuality`'s band.

---

## 2. Motion vectors

DLSS SR's own `VelocityCombine.usf` (`Plugins/DLSS/Shaders/Private/VelocityCombine.usf`) is
**unchanged** from what the companion doc already quoted verbatim (same decode, same
`View.ClipToPrevClip` row-vector `mul`, same `(0.5,-0.5)` scale-and-negate, same
`MotionVectorScale = (1,1)`, same dilate-by-default cvar). Not re-quoted.

**New: Frame Generation has its own, separate velocity-combine pass**, and it differs from DLSS
SR's in three ways worth recording.

**2.1 — An "alternate motion vectors" extension point exists, for exactly the problem
`CLAUDE.md` already named as unfixable from our hook.** `CLAUDE.md`'s NR section concludes,
after ruling out every other explanation for a persistent reflection-tracking artefact:

> "A reflection does not move with its surface — it moves with the reflected geometry... Treat
> 'temporal network + screen-space reflections' as a structural mismatch, not an open bug."

NVIDIA's own reference has a compile-time extension point for precisely this, on **both** the
SR combine and the FG combine, gated by the same flag:

```cpp
// VelocityCombine.usf (StreamlineCore), inside the non-dilated branch
#if SUPPORT_ALTERNATE_MOTION_VECTOR
    const float2 EncodedAltVelocity = AlternateMotionVectorsTexture[PixelPos];
    if (EncodedAltVelocity.x > 0.0f)
    {
        float2 DecodedVelocity = DecodeVelocityFromTexture(float4(EncodedAltVelocity, 0.0f, 0.0f)).xy;
        // we encode in the orientation DLSS expects, so the extra negate it to make them
        // consistent with the ones generated above
        OutVelocity = -1.0f * DecodedVelocity * CombinedVelocity_ViewportSize;
    }
#endif
```

with the comment at the call site (`DLSSUpscaler.cpp:797`, `AlternateMotionVectorTexture`):
"replacement motion vectors for items like reflections that DLSS might prefer to track." **HARD**
that this exists in both the DLSS SR path (`DLSSUpscaler.cpp:797-807`, `SUPPORT_GUIDE_GBUFFER`)
and the FG path (`StreamlineShaders/Private/VelocityCombinePass.cpp`,
`FSupportAlternateMotionVectorDim`). **HARD, and important**: `SUPPORT_GUIDE_GBUFFER` is `#define`d
`0` in this build (`StreamlineViewExtension.cpp:44-46`) — the extension point exists but is
**compiled out** in the shipped plugin. A title gets reflection-aware motion vectors only by
authoring its own "guide G-buffer" (`SceneTextures.AlternateMotionVector`) and flipping that
define — engine-level work, not a config flag. **Investigate, long-horizon, not now, and harder
than §1's `ITemporalUpscaler` question**: this is the one piece of evidence that NVIDIA's own
engineers consider the reflection-motion problem real enough to reserve plumbing for, but the
plumbing here is a *compile-time* `#define` inside the engine plugin's own source — it needs a
new member on `FSceneTextures` and a base-pass write into it, i.e. changes to Stray's actual
engine build, not merely a hooked function call at runtime. That is a materially harder wall than
§1's (reaching a runtime-registered interface via a pattern-scanned hook, from our own DLL) — it
needs the shipped game's engine source or bytecode-level patching of the compiled shader/engine
binary, neither of which the UE4SS migration (`docs/RESEARCH-UE4SS-MIGRATION.md`) provides or
was ever scoped to provide. Not reachable by this project's current architecture, UE4SS-based or
not.

**2.2 — FG's own dilation default is off, opposite of SR's.**
`r.Streamline.DilateMotionVectors` defaults to **0** (`StreamlineViewExtension.cpp:76-81`,
comment: "pass low resolution motion vectors into DLSS Frame Generation (default)") — versus
`r.NGX.DLSS.DilateMotionVectors` defaulting to **1** for SR. **HARD.** Consistent with our own
choice not to dilate (`shaders/mv_resolve.hlsl` produces one vector per render pixel, no
dilation, for either SR or FG): **do not adopt** dilation for FG guides, we already match
NVIDIA's own FG-side default; the earlier "we don't dilate, unlike SR's own default-1" note from
the companion doc applies to SR only, and does not apply to FG at all.

**2.3 — FG's motion-vector scale convention, cross-validated from two independent readings.**
`docs/STRAY-RENDERING-FACTS.md` §32.3 reverse-engineered the raw `nvngx_dlssg.dll` snippet's
expectation from the Streamline *programming guide* (§7.0, HARD there): `mvecScale = {1,1}` if
the buffer is already `[-1,1]`, `{1/renderWidth, 1/renderHeight}` if it is pixel-space, and
concluded (since our resolve emits render-resolution pixels) that `DLSSG.MvecScaleX/Y` should be
`1/renderW, 1/renderH` — which is exactly what `src/ngx_fg.cpp:433-434` sends. The real UE5
plugin source computes the *identical* quantity independently:

```cpp
// StreamlineViewExtension.cpp:914-923
const float MotionVectorScale = CVarStreamlineMotionVectorScale.GetValueOnRenderThread(); // default 1.0f
if (bDilateMotionVectors)
    StreamlineArguments.MotionVectorScale = { MotionVectorScale / ViewInfo.GetSecondaryViewRectSize().X, MotionVectorScale / ViewInfo.GetSecondaryViewRectSize().Y };
else
    StreamlineArguments.MotionVectorScale = { MotionVectorScale / ViewInfo.ViewRect.Width(), MotionVectorScale / ViewInfo.ViewRect.Height() };
```

Non-dilated (our case): `1.0 / ViewRect.Width()`, `1.0 / ViewRect.Height()` — the same
`1/renderWidth, 1/renderHeight` §32.3 derived from the guide text alone. **HARD, two
independent sources agree. Do not adopt anything — this confirms our existing value is right**,
and retires the "UNCONFIRMED until an interpolated frame is judged" caveat §32.3 attached to
this specific convention (the *values* are now cross-validated from source; whether the
resulting interpolated frame looks correct on the box is still a separate, unconfirmed question).

**2.4 — The camera-cut/validity test differs by one detail: `.x` vs. `all(.xy)`.**
Both NVIDIA combine shaders (SR's and FG's) test `all(EncodedVelocity.xy > 0)` to decide
"is this pixel's velocity encoded, or do I need to reconstruct it from depth+camera". Our own
resolve shader, and Stray's own DXBC (`CLAUDE.md` §2.5, HARD from the shipped bytecode), test
`.x` alone: *"Validity test is `EncodedVelocity.x > 0.0` — strict, red channel only... reproduce
it exactly; never use a magnitude threshold."* These are two different shaders (the engine's own
TAA vs. NVIDIA's velocity-combine) checking the same encoded texture for different purposes, so
this is not necessarily a contradiction — but it is a real, citable divergence in how
conservative the two conventions are: `all(xy>0)` rejects strictly more pixels back to the
camera-reconstruction branch than `x>0` does. **Investigate, low priority**: Stray's own TAA
shader is the ground truth for what encodes here (HARD, DXBC-derived), so our `.x`-only test is
correct *for matching the engine's own convention* — but NVIDIA's own reference being more
conservative is worth a note in case a future title's velocity encode ever produces `x>0, y<=0`
for a genuinely moving pixel (would decode a wrong `y` under our test, would fall through to
camera reconstruction under NVIDIA's). Not actionable for Stray specifically without evidence
such a pixel exists in practice.

---

## 3. Exposure

**Unchanged from the UE4 plugin, at the source level.** `DLSSUpscaler.cpp:1111-1115`:

```cpp
check(PassParameters->EyeAdaptation);
PassParameters->EyeAdaptation->MarkResourceAsUsed();
DLSSArguments.InputExposure = PassParameters->EyeAdaptation->GetRHI();
DLSSArguments.PreExposure = Inputs.PreExposure;
DLSSArguments.bUseAutoExposure = bUseAutoExposure;
```

is byte-for-byte the same call shape the companion doc already quoted from the UE4/5.0-era
mirror, with `r.NGX.DLSS.AutoExposure` still defaulting to **1** (`:97-98`,
"Enable DLSS internal auto-exposure instead of the application provided one (default)"). **HARD,
nothing to report.** This project already implements both the texture path and the auto-exposure
path with the identical flag pairing (`src/ngx_backend.cpp:867-877`, cited against
`NGXRHI.cpp:537-546` in our own comments). Full audit of exposure is the `dlss-exposure` branch's
job — this section exists only to confirm there is nothing UE5/DLSS4-specific to add to it.

---

## 4. Feature flags and creation

**`DoSharpening` is gone, not merely deprecated.** A search of `DLSSUpscaler.cpp` for
`Sharpening` returns nothing — no flag, no cvar, no reference at all. **HARD** (absence). This
retires any lingering doubt about `CLAUDE.md`'s "Never set `DoSharpening` (deprecated, does
nothing)" — it is not even wired up anymore on NVIDIA's own side. **Do not adopt anything; we
already never set it.**

**Presets: the set has grown, and the deprecation boundary moved.** `DLSSUpscaler.cpp:44-86`:

```cpp
// r.NGX.DLSS.Preset (SR/DLAA)
//  0: Use default preset or ini value
//  1-6: Force preset A..F      (A-E deprecated per ToNGXDLSSPreset's ensureMsgf, F is NOT)
//  7: Force preset G
// 10-15: Force preset J..O     (10-13 = J,K,L,M — what CLAUDE.md documents — plus 14=N, 15=O)

// r.NGX.DLSSRR.Preset (Ray Reconstruction)
//  1-3: Force preset A..C (deprecated)
//  4-15: Force preset D..O (not deprecated)
```

```cpp
// DLSSUpscaler.cpp:267-278, ToNGXDLSSPreset
case EDLSSPreset::A: case EDLSSPreset::B: case EDLSSPreset::C: case EDLSSPreset::D: case EDLSSPreset::E:
    ensureMsgf(false, TEXT("ToNGXDLSSPreset should not be called with a deprecated value"));
case EDLSSPreset::Default: return NVSDK_NGX_DLSS_Hint_Render_Preset_Default;
case EDLSSPreset::F: return NVSDK_NGX_DLSS_Hint_Render_Preset_F;   // <-- F is a live, non-deprecated preset now
```

**HARD.** `CLAUDE.md` §5 currently reads *"Presets: only `0, J=10, K=11, L=12, M=13` are valid
(A–D removed, E/F deprecated)."* Against this source: **A–E are deprecated for SR (not A–D), F is
a live preset, and N (14) and O (15) exist and are not deprecated.** For Ray Reconstruction the
deprecation boundary is A–C, not A–D. This is worth a `CLAUDE.md` correction independent of
anything else in this report — the exact wording is now stale, not merely incomplete. **Adopt the
correction**; whether to also change our pinned preset from K to something in the newer range is
a separate, lower-confidence question below.

**Default behaviour differs: NVIDIA lets the driver choose per quality mode unless the project
overrides it; we hardcode K everywhere.** `GetNGXDLSSPresetFromQualityMode`
(`DLSSUpscaler.cpp:369-406`) starts from `EDLSSPreset::Default` and only overrides it from a
per-quality-mode project setting (`UDLSSSettings::DLSSPerformancePreset`,
`DLSSBalancedPreset`, ... one field per `EDLSSQualityMode`) or from the global
`r.NGX.DLSS.Preset` cvar if it is non-zero. **HARD.** `src/ngx_backend.cpp:71-81` instead pins a
single preset (K, `g_preset`) unconditionally for every quality mode via
`NgxPreset`/`NVSDK_NGX_DLSS_Hint_Render_Preset_K` on all five hint keys at once
(`ngx_backend.cpp:855-858`). **Investigate, not adopt outright**: our own user-judged A/B already
found J/K/M visually indistinguishable on this content (`CLAUDE.md` §5, "Gotchas ledger"), so
there is no measured reason to prefer the driver default over K specifically — but that
comparison predates presets N and O existing at all. Re-running that same A/B with N and O
added is cheap (a config flip, no code change) and would either confirm K still holds or find a
newer model that is visibly better; low urgency, non-zero expected value.

**Dynamic-resolution bookkeeping: not applicable.** `FDLSSUpscaler`'s constructor
(`DLSSUpscaler.cpp:629-671`) tracks `MinDynamicResolutionFraction`/`MaxDynamicResolutionFraction`
across all six quality modes' `FDLSSOptimalSettings`, feeding UE5's dynamic-resolution system.
Stray runs a fixed screen percentage (`CLAUDE.md` §2.3.1: 50% or 70%, chosen by the shipped
config, never adjusted at runtime) and this project does not implement dynamic resolution at
all — **not applicable**, nothing to adopt.

---

## 5. Frame generation

This is the section where the real UE5 source adds the most over the companion doc's generic-SDK
reading, because `docs/STRAY-RENDERING-FACTS.md` §32 already reverse-engineered the raw
`nvngx_dlssg.dll` parameter *names* from a 2.12.0 binary, and what follows either confirms those
names' *values and conventions* from real engine source, or finds a genuine value mismatch.

**5.1 — Guide capture is ONE engine hook, not several, and it sits after the full post-process
chain.** `FStreamlineViewExtension::SubscribeToPostProcessingPass` registers a single callback,
at `EPostProcessingPass::VisualizeDepthOfField` (`StreamlineViewExtension.cpp:639-649`) — despite
the name, this is Epic's designated "last callback in the post-process chain" slot, confirmed by
what the callback actually reads: `PostProcessPassAtEnd_RenderThread`
(`StreamlineViewExtension.cpp:702` onward) names its own local `SceneColorAfterTonemap =
SceneColor.Texture` (`:777-778`) and pulls Depth and Velocity straight from `FSceneTextures`
(`:794-799`), not from any DOF-specific resource. **HARD.** From this single hook, in one RDG
pass, NVIDIA builds and tags: `Depth` (raw `SceneTextures.Depth.Resolve`), `Velocity` (through
its own combine pass, §2 above), `SceneColorWithoutHUD` (a plain copy of the post-tonemap scene
colour, `AddDrawTexturePass(..., SceneColor.Texture, SLSceneColorWithoutHUD, ...)`,
`:836-841`), and `NoWarpMask` (custom depth, for the Latewarp feature, not applicable to us).
**This is materially simpler than what we built**: one engine callback with `FRDGBuilder` and
`FSceneTextures` access replaces our TAA-dispatch interception, per-register descriptor capture,
and separate `mv_resolve` compute dispatch — because NVIDIA is running *inside* the engine and we
are not. Nothing here is adoptable directly (same wall as §1's `ITemporalUpscaler` finding), but
it is worth stating plainly: **our architecture's extra complexity is a direct, measured cost of
staying outside the engine, not an implementation gap** — useful context for any future decision
about the UE4SS migration's expected value.

**5.2 — UI/HUD-less capture is a SECOND, separate hook, at actual present time, on the game
thread.** Not part of the RDG view extension at all: `RegisterStreamlineDLSSGHooks`
(`StreamlineDLSSG.cpp:366-395`) binds `DLSSGOnBackBufferReadyToPresent`
(`OnBackBufferReadyToPresentHandle`) to Slate's `SlateRenderer::OnBackBufferReadyToPresent`
delegate — this fires once per window, holding the actual composited backbuffer (UI included),
right before it presents. From there, when the engine has no dedicated UI-alpha buffer, NVIDIA
runs a compute pass (`AddStreamlineUIHintExtractionPass`,
`StreamlineShaders/Private/UIHintExtractionPass.cpp`) that thresholds the **backbuffer's own
alpha channel** against `r.Streamline.TagUIColorAlphaThreshold` to synthesize a UI mask —
i.e. even NVIDIA's reference falls back to an alpha heuristic rather than true UI/scene
separation when a title's UI doesn't carry a clean alpha. **HARD**, and genuinely useful to
record: **this exact two-hook shape (a pre-UI colour snapshot from inside the engine's
post-process chain, plus a present-time alpha threshold on the final composited image) is
precisely the design our own `NgxNRHook=preui` work already reasons toward for the NR
feedback-loop problem** (`CLAUDE.md`, "The hook site is now a choice"). We still cannot reach the
first half (`SceneColorWithoutHUD`) today — our present hook is at the swapchain/D3D12 level (now
UE4SS-loaded native code, `src/backend_native/present_owner.*`, not a ReShade proxy, but still
operating at the same point in the pipeline) and only ever sees the *final* composited backbuffer,
the same thing Slate's `OnBackBufferReadyToPresent` sees, never the pre-UI intermediate. Reaching
that intermediate needs exactly the same technique §1 already reclassifies for `ITemporalUpscaler`
— pattern-scan and hook the specific renderer-thread post-process callback point (Epic's
`SubscribeToPostProcessingPass`/`EPostProcessingPass::VisualizeDepthOfField` slot, per §5.1 above)
— which UE4SS's own reflection does not provide automatically, but which our now-native,
in-process, already-vtable-patching codebase is at least the right *kind* of program to attempt.
**Investigate, long-horizon, tied to §1's reclassification, not separately actionable today**: our
own `preui` render-target-identity heuristic (`NgxNRPreUiBind`) remains the closest available
substitute in the meantime, and there is no cheaper alternative to adopt right now.
`DLSSG.HUDLess`/`DLSSG.UI`/`DLSSG.UIAlpha` (confirmed present, optional, in
`docs/STRAY-RENDERING-FACTS.md` §32.2) remain unset by us — correctly, since we have no source
for them.

**5.3 — `DLSSG.CameraFar`: a concrete, actionable value mismatch.** `docs/STRAY-RENDERING-FACTS.md`
§32.3 already flagged this as open: *"`sl_consts.h:248-249`... `cameraNear/cameraFar` default
`INVALID_FLOAT` (must be set)."* `src/ngx_fg.hpp:36` currently defaults our own
`camera_far` to **0.0f**, documented as *"0 = pass 0 (infinite, what a reversed-Z infinite
projection means)"* — i.e. we treat 0 as a stand-in for "no far plane." The real UE5 plugin does
the opposite: it never tries to express "infinite" at all. `StreamlineViewExtension.cpp:90-95`:

```cpp
static TAutoConsoleVariable<float> CVarStreamlineCustomCameraFarPlane(
    TEXT("r.Streamline.CustomCameraFarPlane"),
    75000.0f,
    TEXT("Custom distance to camera far plane. Used for internal DLSS Frame Generation purposes, ")
    TEXT("does not need to match corresponding value used by engine. (default = 75000.0f)"),
    ECVF_RenderThreadSafe);
```

and this literal cvar value is what feeds `StreamlineArguments.CameraFar` (`:833`), unconditionally,
for every title the plugin ships in — including UE5 titles that, like Stray, may use an infinite
far plane in their actual projection matrix. **HARD.** So NVIDIA's own reference, when faced with
"the engine's real far plane may be infinite," does not pass 0 or attempt to signal infinity — it
substitutes an arbitrary large finite constant and says explicitly this need not match the
engine. Passing **0.0** is a materially different choice: `sl_consts.h`'s own default is a
sentinel (`INVALID_FLOAT`, i.e. "not set," not "infinite"), and there is no source read in this
project confirming that the closed `nvngx_dlssg.dll` snippet's internal camera-motion
reconstruction treats a literal `0.0` far plane as "infinite" rather than as a degenerate
near-zero-depth-range input. **Adopt/investigate — the highest-confidence, most concrete
finding in this report**: change `NgxFGCameraFar`'s default from `0.0f` to a large finite
constant (NVIDIA's own `75000.0f` is a reasonable starting point, or derive one from Stray's own
scene scale) and re-test. This is a one-line config change with a plausible, specific failure
mode behind it (the snippet's `1/depth`-style linearization, `docs/STRAY-RENDERING-FACTS.md`
§32.3, dividing by a degenerate range) and costs nothing to try.

**Our `CameraNear` choice is, if anything, more correct than NVIDIA's own default — no
change.** `CVarStreamlineCustomCameraNearPlane` defaults to a hardcoded `0.01f`
(`StreamlineViewExtension.cpp:89`, same "does not need to match" caveat). We instead pass
`c.near_plane` — the real, measured `View.NearPlane` read from row 142.x of the View constant
buffer (`ngx_fg.cpp:448`, `CLAUDE.md` §2.6). **Do not adopt** NVIDIA's hardcoded default; ours is
already the more faithful value and there is no evidence it needs to change.

**5.4 — Reset detection: NVIDIA's own FG-side code is simpler than SR's, and admits it. Ours is
already the more complete OR.** `StreamlineViewExtension.cpp:915`:

```cpp
// TODO STREAMLINE check for other conditions, similar to DLSS
StreamlineArguments.bReset = View.bCameraCut;
```

**HARD**, including the comment — a single-signal reset test with an open TODO acknowledging
DLSS SR's own reset logic (the multi-signal check the companion doc's §A quotes, and
`CLAUDE.md` §2.8 documents independently: `CameraCut` flag OR jitter `zw==xy` OR a 1×1
history/velocity texture) is more complete. Our project already propagates the same
multi-signal OR to both SR and FG (`CLAUDE.md` §2.8's "OR three signals for `InReset`", and
`ngx_fg.cpp:158`'s `g_pending_reset` comment citing exactly this). **Do not adopt anything —
this is a case where our implementation is already ahead of NVIDIA's own shipped FG code**,
worth recording for confidence rather than action.

**5.5 — Multi Frame Generation: real, but not applicable to our target hardware.**
`sl_dlss_g.h` (this build, SL 2.9.0) defines `DLSSGOptions::numFramesToGenerate` (default `1`,
i.e. classic 2x) with the comment *"For 2x only supporting devices, numFramesToGenerateMax is 1.
For 3x and 4x supporting devices, numFramesToGenerateMax is 3."* **HARD** that this ceiling is
read from the SDK itself, not inferred. `docs/STRAY-RENDERING-FACTS.md` §32.2 already confirms
the underlying raw NGX parameter names exist in the snippet (`DLSSG.MultiFrameCount`,
`DLSSG.MultiFrameIndex`, `DLSSG.MultiFrameCountMax`) and that **we already set them**
(`src/ngx_fg.cpp:245-246`: `MultiFrameCount=1, MultiFrameIndex=0`), reading `MultiFrameCountMax`
back after create and logging it (`:275-296`). Multi-frame generation beyond 2x is an Ada
(RTX 40-series, our target — `CLAUDE.md` §1: RTX 4090) hardware ceiling, not a software gap on
either NVIDIA's side or ours: **not applicable, no action** — this was already the right call,
made independently, before this report existed. `sl::DLSSGMode` in this exact SDK snapshot
(2.9.0) has only `eOff/eOn/eAuto`, no `eDynamic` — Dynamic Multi Frame Generation (`SOFT`, web
search: introduced in the Streamline 2.11.0 changelog with `dynamicTargetFrameRate`) postdates
this source and was not read; moot regardless, since it only selects *how many* extra frames to
generate on hardware that can generate more than one, which Ada cannot.

**5.6 — Fullscreen-menu detection: a different mechanism, same intent, no action needed.**
NVIDIA gates FG per-frame on `DLSSGFlags::eEnableFullscreenMenuDetection`
(`CVarStreamlineFullScreenMenuDetection`, `StreamlineDLSSG.cpp:94-98`) — the snippet itself
inspects the tagged buffers to guess "is this a fullscreen menu" and suppresses generation. We
instead set `DLSSG.MenuDetectionEnabled=0` and rely on our own guide-publish gate: no TAA-hook
dispatch (a loading screen, most menu states) means no published guides, means no generated
frame that presentation cycle (`ngx_fg.cpp:332`'s comment). **Do not adopt** — different
mechanism, same effect, and ours is grounded in something we can observe directly (whether our
own hook fired) rather than a heuristic over tagged content.

**5.7 — Reflex, present pacing, and swapchain ownership: intentionally out of scope here.**
`StreamlineReflex.cpp` confirms DLSS-G forces Reflex on automatically when active
(`DoActiveStreamlineFeaturesRequireReflex()`, `:195` and its use at `:269`) — consistent with,
and no update to, what `CLAUDE.md` already establishes about needing `fakenvapi`
`force_reflex=2`. A full comparison of Streamline's present-pacing algorithm, its resource-tag
lifetime rules, and its swapchain-proxy mechanics (the thing that was measured to break frame
generation for us under vkd3d-proton, `docs/STRAY-RENDERING-FACTS.md` §32) against our own
present-twice path and phase-locked scheduler is the subject of a **separate** document,
`docs/RESEARCH-STREAMLINE-INTERNALS.md`, read directly from NVIDIA's open-source Streamline SDK
rather than from this UE plugin wrapper. Not duplicated here.

---

## 6. Anything NVIDIA now does that we do not do at all

* **`DLSSGFlags::eRetainResourcesWhenOff`** (`sl_dlss_g.h:44`) — keep FG's internal resources
  allocated across an off→on toggle instead of tearing them down. **UNCONFIRMED** whether this
  would meaningfully change our own create/release cadence (`src/ngx_fg.cpp`'s
  `g_create_latched` teardown-on-resize path) — plausible minor win (avoids a
  `ReleaseFeature`+`CreateFeature` round trip if FG is toggled at runtime), not evaluated in
  this pass. **Investigate, low priority**: only matters if FG is toggled frequently at runtime,
  which is not this project's current usage pattern (set once via `Engine.ini`/launch config).
* **`DLSSGOptions::numBackBuffers`, `colorBufferFormat`, `mvecBufferFormat`, `depthBufferFormat`,
  `hudLessBufferFormat`** — creation-time format/count hints so the snippet can pre-validate or
  pre-allocate. We already pass the equivalent information at create time
  (`DLSSG.Width/Height/BackbufferFormat/ColorBuffersHDR`, `ngx_fg.cpp:238-244`,
  `docs/STRAY-RENDERING-FACTS.md` §32.2 confirms these names are read by the snippet) —
  **no gap**, already covered, different name surface (Streamline's `DLSSGOptions` vs. the raw
  `DLSSG.*` parameter names we write directly).
* **`DLSSGFlags::eShowOnlyInterpolatedFrame`** — a debug/visualization mode. Not applicable to a
  shipping configuration; no action.
* **`DLSSGQueueParallelismMode::eBlockNoClientQueues`** (`sl_dlss_g.h:47-56`) — Vulkan-only queue
  parallelism mode. Not applicable — this project is D3D12-only by the vkd3d-proton constraint
  (`CLAUDE.md` §0).
* **Dynamic-resolution-aware FG** (`DLSSGOptions::dynamicResWidth/Height`,
  `eDynamicResolutionEnabled`) — not applicable; Stray does not use engine dynamic resolution
  (`CLAUDE.md` §2.3.1, fixed screen percentage).

---

## What could not be obtained, stated plainly

* **A pristine NVIDIA UE 5.6 checkout.** NVIDIA's actual distribution channel for the UE-native
  plugin, `NvRTX/UnrealEngine` (`dlss3/sl2-5.0-dlss-plugin`, `sl2-5.1-dlss-plugin`,
  `sl2-4.27-dlss-plugin` branches — the last of which would have been directly relevant, a
  DLSS3-era Streamline build targeting our *exact* engine version), returned HTTP 404 over the
  GitHub API — gated to accounts that have linked a GitHub identity accepted by Epic's UE EULA
  process, which this session does not have. Every claim above instead rests on the
  `XPOL555/unreal-engine-5.7-dlss4` mirror, whose authenticity is established by intact NVIDIA
  copyright headers (2020-2025 / 2022-2025) across every file quoted, not by the distribution
  channel. **If exact byte-for-byte fidelity to a pristine 5.6 build ever matters, re-verify
  against a linked NvRTX checkout — this session could not.**
* **DLSS 4.5** (the "2nd Generation Super Resolution Transformer," Dynamic Multi Frame
  Generation, "240 FPS mode" — `SOFT`, January 2026 CES coverage per web search). No source for
  it was obtained; the plugin read here (`8.3.0-NGX310.4.0`, bundled Streamline `2.9.0`) may
  predate it. Presets N and O were confirmed to **exist** as enum values with live (not
  deprecated) NGX hint mappings, but nothing in the source read here says which model generation
  each corresponds to, or whether either is DLSS4.5-specific — that mapping is **UNCONFIRMED**.
* **Streamline SDK versions 2.10.0 through 2.12.0's actual source**, only their changelog
  entries (fetched from `NVIDIA-RTX/Streamline`'s public `changelog.txt`, itself HARD, but a
  changelog entry is a summary, not the code). Where this report says a Streamline-side feature
  "postdates" the 2.9.0 source read here, that is based on the changelog listing it under a later
  release, not on diffing source.
* **RTX Remix / DLSS Ray Reconstruction's exact per-guide-buffer contract beyond what
  `DLSSDenoiser.h`/`DLSSUpscaler.cpp` show.** This project's own `docs/RESEARCH-RR-GBUFFER.md`
  already covers that ground in more depth than this pass attempted; not duplicated or
  re-verified here.

---

## Recommendations, ranked by expected value

1. **Adopt/investigate — `NgxFGCameraFar` default.** Change from `0.0f` ("pass 0 for infinite")
   to a large finite constant (start from NVIDIA's own `75000.0f`, tune to Stray's scene scale
   if needed). Concrete, specific, one-line change; the failure mode it targets (a degenerate
   linearization inside the closed snippet) is plausible and cheap to rule out. §5.3.
2. **Adopt — correct `CLAUDE.md` §5's preset table.** "A–D removed, E/F deprecated" is stale:
   the current plugin deprecates A–E for SR (F is live) and A–C for RR. A documentation fix, not
   a behaviour change, but load-bearing if anyone chooses a preset by reading that table. §4.
3. **Investigate, cheap — re-run the J/K/L/M preset A/B with N and O included.** No code change,
   a config flip; the last comparison predates two newer presets existing at all. Low urgency
   since the existing comparison already found no visible difference among J/K/M. §4.
4. **Record, no action now — the reflection-motion "alternate motion vector" extension point.**
   NVIDIA's own engineers reserved plumbing for exactly the structural problem `CLAUDE.md`
   already named and accepted as unfixable from our hook, but this specific plumbing is a
   compile-time engine `#define` requiring Stray's own engine source — harder than, and not
   solved by, the UE4SS plugin migration already done. §2.1.
5. **Record, long-horizon — the hudless/UI-alpha two-hook design and the `ITemporalUpscaler`
   registration point, together.** Both need the same unbuilt capability: pattern-scanning and
   hooking a specific, private, renderer-thread C++ function from our now-native, in-process
   UE4SS plugin, with real UE 4.27.2 struct-layout knowledge — genuinely more plausible than under
   the old ReShade-add-on framing (see §1's reclassification), but still unbuilt and not a side
   effect of the UE4SS migration already completed. Worth its own scoped assessment, in the style
   of `docs/RESEARCH-UE4SS-MIGRATION.md`, if either is ever prioritised. Not adoptable today; our
   own `preui` render-target-identity heuristic remains the closest available substitute for the
   NR feedback-loop problem in the meantime. §1, §5.2.
6. **No action — motion-vector scale convention.** Independently cross-validated correct from
   two unrelated sources (our own §32 binary-string reverse-engineering and this UE5 source
   reading). §2.3.
7. **No action — reset detection.** Ours (a 3-signal OR, reaching both SR and FG) is already
   more complete than NVIDIA's own shipped FG-side code, which carries an open TODO admitting
   the same gap we already closed. §5.4.
8. **No action — exposure.** Unchanged from the UE4 plugin at the source level; full audit lives
   on the `dlss-exposure` branch. §3.
9. **Not applicable — Multi Frame Generation (N>1).** Hardware-gated to Ada's 2x ceiling; we
   already, independently, set `MultiFrameCount=1`. Revisit only if targeting Blackwell. §5.5.
10. **Not applicable — Streamline swapchain ownership, present pacing, dynamic resolution,
    Vulkan queue parallelism.** Either the subject of the forthcoming
    `docs/RESEARCH-STREAMLINE-INTERNALS.md`, or ruled out by this project's own vkd3d-proton /
    fixed-screen-percentage / D3D12-only constraints. §5.7, §6.

---

## Stale-elsewhere note (not fixed here — out of scope for this document)

While correcting this document's own framing to match the current UE4SS/native-backend
architecture (this document's own "Architecture note," above), the following were noticed
describing the project as a ReShade add-on in ways that are now inaccurate. Not touched here —
flagged for whoever next has that document open:

* `docs/RESEARCH-OFFICIAL-DLSS-UE-PLUGIN.md` — its entire premise (title, §0, and especially §D,
  "hook or stage, per feature") is framed against "ReShade's D3D12 event model" as the thing our
  side is constrained by. The technical content about NVIDIA's *own* plugin architecture (§A-C)
  is unaffected by this and remains reliable; only the passages characterizing *our* side need
  the same reclassification this document's §1 applies.
* `CLAUDE.md` §3 ("How the add-on works") and §5 ("ReShade 6.8 add-on API") describe the
  interception mechanism as ReShade add-on events (`init_pipeline`, `bind_pipeline`, `dispatch`,
  etc.) rather than the current native vtable-patch hook layer
  (`src/backend_native/`) — CLAUDE.md itself says elsewhere that such documents are "corrected in
  place" as facts change, so this is presumably known and pending, not missed.
