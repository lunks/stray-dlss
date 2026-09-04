# Ambient occlusion in Stray — what exists, what is reachable, what is worth a launch

**Verdict: cvars only. No plugin work is warranted.** UE 4.27 already ships a horizon-based AO
(GTAO) behind a runtime cvar, every one of its shaders is cooked into Stray's own global shader
cache, and none of the AO cvars is `ECVF_ReadOnly`. NVIDIA HBAO+ *is* reachable as a standalone
D3D12 library under a permissive licence — that finding is real and is recorded below — but it is
**dominated** by GTAO here, for a reason that has nothing to do with image quality: HBAO+ hands
you an AO buffer and we have no seam at which to apply it correctly, whereas GTAO's output is
already wired into the one place in the frame where AO is applied to exactly the lighting Stray
bakes.

Everything below is read from the UE 4.27.2 mirror `AlexMercer-MA/UnrealEngine-4.27` @ `306a7e9`
(the mirror CLAUDE.md §5 already uses), from Stray's own cooked config in `docs/game-config/`, or
from `NVIDIAGameWorks/HBAOPlus` @ `master`. Nothing here has been run against the game.

---

## 0. Read this first: four lines of your current config do nothing

| Line in your `[SystemSettings]` | Verdict | Why |
|---|---|---|
| `r.AOQuality=2` | **INERT** | This is *distance-field* AO quality (`DistanceFieldAmbientOcclusion.cpp:36-42`), not SSAO. Stray ships `r.GenerateMeshDistanceFields=False` (`DefaultEngine.ini:40`) — a cook-time setting, so **no mesh distance fields exist in the pak** — and `r.DistanceFieldAO=0` in its own `WindowsEngine.ini:27`. There is nothing for it to select. **HARD.** |
| `r.AmbientOcclusion.Denoiser=0` | **INERT** | Declared in `RayTracing/RayTracingAmbientOcclusion.cpp:32-38` and read only at `:203`, inside `RenderRayTracingAmbientOcclusion`. With `r.RayTracing=False` that function never runs. **HARD.** |
| `r.Shadow.Denoiser=0` | **INERT — the cvar does not exist** | 4.27 has `r.Shadow.Denoiser.ReconstructionSamples` / `.PreConvolution` / `.TemporalAccumulation` / `.HistoryConvolutionSamples` (`ScreenSpaceDenoise.cpp:24,29,34,39`) but no bare `r.Shadow.Denoiser`. An unknown key in an ini section is skipped. **HARD.** |
| `r.Reflections.Denoiser=0` | **INERT for SSR** | It exists (`ScreenSpaceDenoise.cpp:43-49`) but SSR's denoise is `bDenoise = DenoiserMode != 0 && r.SSR.ExperimentalDenoiser` (`IndirectLightRendering.cpp:771`), and `r.SSR.ExperimentalDenoiser` defaults to **0** (`IndirectLightRendering.cpp:31-34`). SSR is already undenoised; this line changes nothing. It would matter only for ray-traced reflections. **HARD.** |

`r.DiffuseIndirect.Denoiser=0` is the one denoiser line that is **live** — see §4.

**Why your other overrides do work:** `[SystemSettings]` is applied as `ECVF_SetBySystemSettingsIni`
(`0x04000000`), which outranks the `[/Script/Engine.RendererSettings]` block in Stray's own
`WindowsEngine.ini` (`ECVF_SetByProjectSetting`, `0x03000000`) and the scalability groups
(`ECVF_SetByScalability`, `0x01000000`) — `IConsoleManager.h:105-126`. **HARD.**

---

## 1. What UE 4.27 actually offers

### 1.1 The four routes, ranked by reachability

| Route | Reachable here? | One line |
|---|---|---|
| **SSAO quality ladder** | **Yes, ini/console** | Stray ships the *cheapest possible* permutation. Unclamping it is the single biggest free win. |
| **GTAO** (`r.AmbientOcclusion.Method=1`) | **Yes, ini/console** | A ground-truth horizon-based AO, in-engine, all shaders cooked, correctly applied, velocity-aware temporal filter. This is "HBAO, but already in the game". |
| **RTAO** | No | Needs `r.RayTracing=True`, measured at ~2.9× frame time in gameplay for near-zero visual gain because Stray's lighting is baked (CLAUDE.md §5). Dismissed. |
| **NVIDIA HBAO+** | Reachable, but dominated | §5. |

### 1.2 Stray ships the lowest SSAO permutation there is

`FSSAOHelper::GetAmbientOcclusionQualityRT` (`PostProcessAmbientOcclusion.cpp:152-163`):

```cpp
float CVarValue = CVarAmbientOcclusionMaxQuality.GetValueOnRenderThread();
if (CVarValue < 0) return FMath::Clamp(-CVarValue, 0.0f, 100.0f);   // ENFORCE |value|
else               return FMath::Min(CVarValue, View.FinalPostProcessSettings.AmbientOcclusionQuality);
```

and `GetAmbientOcclusionShaderLevel` (`:166-174`) turns that percentage into the
`SHADER_PERMUTATION_INT("SHADER_QUALITY", 5)` dimension (`:677, :708, :1051, :1235`):

```cpp
return (Q > 75) + (Q > 55) + (Q > 25) + (Q > 5);
```

Stray's own `WindowsEngine.ini:11` sets **`r.AmbientOcclusionMaxQuality=1`**. That is
`Q = min(1, PPV) = 1` → **`SHADER_QUALITY = 0`**, and the shader's own table
(`PostProcessAmbientOcclusion.usf:51-76`) says what that is:

| `SHADER_QUALITY` | directions (`SAMPLESET_ARRAY_SIZE`) | `SAMPLE_STEPS` | GTAO taps (`:529-554`) |
|---|---|---|---|
| **0** (what Stray ships) | 3 | 1 | 4 |
| 1 | 3 | 1 | 6 |
| 2 (what you get today with `=100`) | 3 | 2 | 8 |
| 3 | 3 | 3 | 12 |
| 4 | **6** | **3** | **20** |

Your `r.AmbientOcclusionMaxQuality=100` means "don't clamp — use the post-process volume's own
`AmbientOcclusionQuality`". The engine default for that is **50** (`Scene.cpp:524`) → level 2. What
Stray's own post-process volumes set is **UNKNOWN** to us and is why level 2 is a guess, not a
fact.

**`r.AmbientOcclusionMaxQuality=-100` removes both the clamp and the guess**: the negative branch
*enforces* 100 regardless of the volume, giving `SHADER_QUALITY = 4` — 6 directions × 3 steps
against today's 3 × 2, and 20 GTAO taps against 8. **HARD** on the mechanism; the *cost* is
unmeasured (§6).

### 1.3 `r.AmbientOcclusionLevels` — you already have the max, and the extra levels are cheap

`ComputeAmbientOcclusionPassCount` (`:231-266`) returns `CVarLevel` verbatim when it is `>= 0`,
clamped to 3. You set 3. `AddPostProcessingAmbientOcclusion`
(`CompositionLighting.cpp:465-527`) shows what the levels buy:

* Levels 1 → one full-res pass.
* Levels 2 → + a half-res setup and a half-res step.
* Levels 3 → + a quarter-res setup and a quarter-res step.

The extra levels run at **half and quarter resolution** and are what supply *large-radius*
occlusion — the room-corner darkening, not the contact darkening. They are the cheap half of the
ladder and you already have them. The expensive half is the full-res final pass, which is the one
`SHADER_QUALITY` governs (§1.2). **HARD.**

Note the resolution this runs at: SSAO is computed at **render** resolution, so at 50% screen
percentage the "full-res" pass is 1920×1080, not 3840×2160. The quality lever is cheaper here
than it would be natively. **[derived]** from the render/output split in CLAUDE.md §2.3.1.

### 1.4 GTAO: present, cooked, and a genuine drop-in

`r.AmbientOcclusion.Method` (`PostProcessAmbientOcclusion.cpp:84-90`) — `0: SSAO (default)`,
`1: GTAO`, flags `ECVF_RenderThreadSafe | ECVF_Scalability`. **Not `ECVF_ReadOnly`**, so it is
settable from `[SystemSettings]` *and* from the live console. **HARD.**

The full cvar set (all `ECVF_RenderThreadSafe | ECVF_Scalability`, none ReadOnly):

| cvar | default | source | note |
|---|---|---|---|
| `r.AmbientOcclusion.Method` | 0 | `PostProcessAmbientOcclusion.cpp:84` | 1 = GTAO |
| `r.GTAO.UseNormals` | **0** | `:92` | help text claims "1: On (default)" — **the declared value is 0**. 0 derives normals from depth; 1 reads GBufferA |
| `r.GTAO.FilterWidth` | 5 | `:100` | 5×5 or 4×4 noise/filter pattern |
| `r.GTAO.ThicknessBlend` | 0.5 | `:108` | thin/thick-object occlusion bias; bigger = less occlusion |
| `r.GTAO.FalloffEnd` | 200.0 | `:117` | world units at which occlusion has fully fallen off |
| `r.GTAO.FalloffStartRatio` | 0.5 | `:123` | fraction of FalloffEnd where falloff starts |
| `r.GTAO.NumAngles` | 2 | `:130` | angles per pixel, 1–16 |
| `r.GTAO.PauseJitter` | 0 | `:137` | |
| `r.GTAO.Upsample` | 1 | `:144` | depth-aware upsample |
| `r.GTAO.Downsample` | **0** | `CompositionLighting.cpp:37` | help text claims "1: On (default)" — **declared value is 0**, i.e. GTAO runs at full render res |
| `r.GTAO.TemporalFilter` | 1 | `CompositionLighting.cpp:45` | |
| `r.GTAO.SpatialFilter` | 1 | `CompositionLighting.cpp:53` | |
| `r.GTAO.Combined` | 1 | `CompositionLighting.cpp:61` | |

**Two doc/value mismatches are flagged above and are HARD** — the help strings for
`r.GTAO.UseNormals` and `r.GTAO.Downsample` say "(default)" against the value they do *not* carry.
Do not trust the help text for these two.

**Does it need anything Stray does not do?**

1. **Shaders.** Every AO and GTAO shader is a *global* shader
   (`IMPLEMENT_GLOBAL_SHADER` at `:519, :604, :698, :731, :1080, :1172, :1264, :1355, :1481, :1588`)
   and every one of their `ShouldCompilePermutation` bodies is exactly
   `return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);`
   (`:502-505, :589-592, :687-690, :720-723, :1064-1067, :1154-1157, :1247-1250, :1323-1326,
   :1452-1455, :1569-1572`). **No project setting gates any of them.** Global shaders are cooked
   for every permutation that passes, which is why the pak carries all 27 `FTAAStandaloneCS`
   permutations (CLAUDE.md §5) rather than only the one the game selects. So GTAO cannot fail on a
   missing shader. **HARD on the source; [derived] one step, on the global-shader cooking rule.**
2. **Compute.** `GetGTAOPassType` (`:282-306`) returns `EGTAOType::ENonAsync` whenever
   `IsAmbientOcclusionAsyncCompute` is false, which it is with the default
   `r.AmbientOcclusion.Compute=0`. `ENonAsync` runs the whole GTAO stack on the graphics pipe
   (`AddPostProcessingGTAOAllPasses`, `CompositionLighting.cpp:276-360`). **No async-compute
   requirement. HARD.**
3. **Velocity.** GTAO's temporal filter reads the engine's velocity buffer and falls back to
   `ClipToPrevClip` exactly as our own MV resolve does
   (`PostProcessAmbientOcclusion.usf:1137-1156`, including the identical
   `if (EncodedVelocity.x > 0.0)` test). Stray ships `r.BasePassOutputsVelocity=True`
   (`DefaultEngine.ini:59`), so coverage is broad. GTAO's history will follow the cat correctly.
   **HARD.**
4. **Quality.** GTAO reuses `GetAmbientOcclusionShaderLevel` for its tap count
   (`CompositionLighting.cpp:213`), so `r.AmbientOcclusionMaxQuality` drives it too.
   `r.AmbientOcclusionLevels` must merely be **non-zero** for GTAO to run at all
   (`CompositionLighting.cpp:645`); the value itself is never used by the GTAO path. **HARD.**

**Unknowns, stated as such:** GTAO in 4.27 is lightly used in shipped titles and this project has
never run it. Whether it looks *better* than a maxed SSAO on Stray's content is **UNCONFIRMED**,
and `GTAO_MAX_PIXEL_SCREEN_RADIUS 256.0f` (`usf:534`) is a single screen-space radius clamp where
the SSAO ladder has three explicit mip levels — so GTAO may show *less* large-radius occlusion, not
more. That is a thing to look at, not to predict.

---

## 2. Is it worth it in a game with baked lighting?

**Yes, and the engine says so in a comment.** `DiffuseIndirectComposite.usf:49-53`:

```hlsl
// Apply AmbientOcclusion to the scene color. since before deferred direct lighting,
// we assume all lighting in SceneColor to be indirect ligthing
{
    float AOMask = (GBuffer.ShadingModelID != SHADINGMODELID_UNLIT);
    OutColor.a = lerp(1.0f, FinalAmbientOcclusion, AOMask * AmbientOcclusionStaticFraction);
}
```

with the blend `BO_Add, BF_One, BF_SourceAlpha` (`IndirectLightRendering.cpp:570`), i.e. the
destination is multiplied by that alpha. This pass runs **before deferred direct lighting**, so the
scene colour it darkens is precisely the **baked and indirect** contribution: lightmaps, the
skylight, reflection captures.

So the scepticism inverts. AO is not redundant with baked lighting — **AO is the thing that
darkens baked lighting**, and `AmbientOcclusionStaticFraction` is the dial for how much. The
engine's own help text agrees: *"only useful for baked lighting and if AO is on, allows to have
SSAO affect baked lighting as well"* (`SceneView.cpp:151-156`). **HARD.**

Stray ships `r.DefaultFeature.AmbientOcclusionStaticFraction=True` (`DefaultEngine.ini:50`), so the
*default* is 1.0 (`Scene.cpp:517`). **But a post-process volume can override it per level and we
cannot see Stray's volumes.** That uncertainty is removable in one line — see `r.AmbientOcclusionStaticFraction`
in §6.

**Where it will and will not show:**

* **Will show:** contact darkening under and around *dynamic* geometry the lightmapper never saw —
  the cat's paws and body against the floor, movable props, the drones. Also fine creases and
  corners at a scale below the lightmap texel, everywhere.
* **Will show less than you expect:** static architecture in the Slums. The lightmapper already
  baked that occlusion, and AO on top of it double-darkens. Raising `AmbientOcclusionStaticFraction`
  past what the artists chose is exactly how a baked scene turns muddy.
* **Will not show at all:** anything `SHADINGMODELID_UNLIT` — the neon signs, the CRT screens
  (`usf:51`). The animated-CRT confound CLAUDE.md §5 warns about is, conveniently, immune here.
* **Will not show while SSGI is on.** §3.

**Where to look**, so the judgement is not a vibe: a still camera on the cat standing on a flat
lit floor near a wall, with the paws in frame. Contact shadow under the paws and the wall/floor
seam are the two places any of these settings can possibly change. Take the pair with
`tools/screenshot-gamescope.sh`, camera dead still (CLAUDE.md §5: a panning camera makes two
captures incomparable).

---

## 3. The finding that reorders everything: **SSGI overrides AO entirely**

`RenderDiffuseIndirectAndAmbientOcclusion` (`IndirectLightRendering.cpp:372-582`):

```cpp
399:  const bool bApplySSGI = ShouldRenderScreenSpaceDiffuseIndirect(View) && bSingleView;
400:  const bool bApplySSAO = SceneContext.bScreenSpaceAOIsValid;
...
417:  else if (bApplySSGI)  { RenderScreenSpaceDiffuseIndirect(..., &DenoiserInputs); }
...
504:  if (DenoiserOutputs.AmbientOcclusionMask)
506:      //ensureMsgf(!bApplySSAO, TEXT("Looks like SSAO has been computed for this view but is being overridden."));
509:      ConvertToExternalTexture(GraphBuilder, DenoiserOutputs.AmbientOcclusionMask, SceneContext.ScreenSpaceAO);
512:  else if (bApplySSAO)
513:      DenoiserOutputs.AmbientOcclusionMask = GraphBuilder.RegisterExternalTexture(SceneContext.ScreenSpaceAO);
```

SSGI **always** allocates and writes its own AO mask (`ScreenSpaceRayTracing.cpp:1096-1099`,
`SSRTAmbientOcclusion`, `PF_R16F`) — it is not optional and not quality-gated. When SSGI runs, that
mask **replaces** `SceneContext.ScreenSpaceAO` and is what the composite applies. Epic left the
assert that says so, commented out, on line 506. **HARD.**

Three consequences, and they decide the test plan:

1. **With `r.SSGI.Enable=1`, every SSAO and GTAO setting in this document is invisible.** The SSAO
   pass still *runs* (it is dispatched earlier, in `FCompositionLighting::ProcessAfterBasePass`,
   `CompositionLighting.cpp:645-685`) and its result is then discarded. You pay for it and see
   nothing. **[derived] from the source above; UNCONFIRMED live.**
2. **Therefore, with SSGI on, `r.AmbientOcclusionLevels=0` should be free performance with zero
   visual change.** Pass count 0 → `bScreenSpaceAOIsValid` stays false → the SSAO passes are never
   built, while SSGI keeps supplying the AO mask. This is a clean, falsifiable prediction and it is
   worth one A/B on its own. **[derived]; UNCONFIRMED live.**
3. **Your AO is currently 8 rays per pixel, undenoised.** `r.SSGI.Quality=2` → `RayCountPerPixel = 8`
   (`ScreenSpaceRayTracing.cpp:517-522`), full resolution (`r.SSGI.HalfRes` defaults 0,
   `:56-59`), and `r.DiffuseIndirect.Denoiser=0` takes the branch at
   `IndirectLightRendering.cpp:467-471` that passes the **raw** SSGI colour *and AO mask* straight
   through with no denoise at all. That noise is then what darkens the frame, and it goes into
   DLSS. **HARD.**

**This is why the config being in flux is not a complication but the whole question.** Tonight's
`r.SSGI.Enable=1` has already, silently, made every AO setting you have moot.

---

## 4. Interaction with what we run

**Where AO sits relative to our seam.** AO is computed after the base pass and composited into
scene colour before deferred direct lighting — all of it upstream of the TAA dispatch we
intercept. So DLSS accumulates whatever AO produces, exactly as the engine's own TAA would have.
**HARD** from the pass order above.

**The engine expects a temporal resolver to finish the job.** SSAO runs at
`AO_SAMPLE_QUALITY 3` — the shader's own comment reads *"efficient high frequency 4x4 pattern
**with jitter for TemporalAA**"* (`usf:48`) — and the dither offset is
`(View.State->GetCurrentTemporalAASampleIndex() % 8) * FVector2D(2.48f, 7.52f) / RandomizationSize.X`
(`PostProcessAmbientOcclusion.cpp:370-375`). That is an 8-frame rotating dither keyed to the same
jitter phase counter DLSS consumes. **HARD.**

The practical reading: **raising SSAO quality raises the per-frame tap count; the temporal
resolution comes from DLSS either way.** A maxed SSAO is not more DLSS-hostile than a minimal one —
if anything it is less, because more taps means less dither amplitude for DLSS to converge.

**Does better AO make DLSS Ray Reconstruction as a screen-space denoiser more or less
attractive?** *Less*, and the reasoning is worth stating precisely so `rr-reflection-denoise` can
use it rather than re-derive it:

* The noise DLSS is currently being asked to absorb in the AO term is **not SSAO's dither**. It is
  **SSGI's 8-ray-per-pixel Monte Carlo, undenoised** (§3.3). SSAO's dither is bounded, structured
  and 8-periodic; SSGI's is stochastic and unbounded.
* So "better AO" is not a lever on that noise at all — the lever is `r.DiffuseIndirect.Denoiser`
  (back to its default `1`, or `2`), or turning SSGI off. Both are one-line ini changes with no
  denoiser to build.
* If SSGI ends up **off** after tomorrow's judgement, the AO term becomes SSAO/GTAO, which is
  structured 8-frame dither that DLSS already resolves — and the case for RR *as the AO denoiser*
  collapses to nothing. Ray Reconstruction's case rests on SSR (CLAUDE.md §5's measured 3.4×
  shimmer gap), not on AO.
* If SSGI ends up **on**, the honest first move is still `r.DiffuseIndirect.Denoiser=1`, because
  the engine's own screen-space denoiser is free and already wired to that exact signal. RR is the
  answer only if that measurably is not enough.

**Recommendation to `rr-reflection-denoise`, not an instruction:** measure with
`r.DiffuseIndirect.Denoiser` at its default before attributing SSGI shimmer to a missing RR. This
document builds no denoiser and touches no file that agent owns.

---

## 5. NVIDIA HBAO+ — reachable, permissively licensed, and still the wrong tool

The user asked for HBAO by name. The honest answer has two halves and the second one is the
decision.

**It is reachable, and more so than the "GameWorks needed an engine branch" folklore suggests.**
`NVIDIAGameWorks/HBAOPlus` @ `master` is a standalone SDK, not an engine fork. All **HARD**, read
from the repository:

* `lib/GFSDK_SSAO_D3D12.win64.dll` (1 126 400 bytes) and `.lib` are checked in, alongside the
  D3D11 and Win32 variants. Full `src/` is present too (`Renderer_DX12.cpp`,
  `PipelineStateObjects_DX12.cpp`, `States_DX12.cpp`, …).
* `include/GFSDK_SSAO.h:904-976, 1023` gives a complete D3D12 entry point:
  `GFSDK_SSAO_CreateContext_D3D12(ID3D12Device*, NodeMask, const GFSDK_SSAO_DescriptorHeaps_D3D12&, …)`
  then `RenderAO(ID3D12CommandQueue*, ID3D12GraphicsCommandList*, const GFSDK_SSAO_InputData_D3D12&,
  const GFSDK_SSAO_Parameters&, const GFSDK_SSAO_Output_D3D12&)`.
* Its inputs are exactly what our TAA seam already captures: a full-res depth SRV
  (`:429-432`), an optional world-space normal SRV (`:483-485`), a 4×4 projection matrix with an
  explicit `GFSDK_SSAO_ROW_MAJOR_ORDER` layout flag (`:285-286`, `:356`), and a viewport. It never
  touches the engine.
* It wants **40 RTV descriptors and 60 CBV/SRV/UAV descriptors** from heaps we supply
  (`:984-1002`), and documents its expected resource states — inputs
  `D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE`, output `RENDER_TARGET` (`:914-915`).
* `LICENSE.txt` is the **Nvidia Source Code License (1-Way Commercial)**: §2.1 grants a
  perpetual, royalty-free right to reproduce, prepare derivative works of, sublicense and
  distribute, conditioned in §3.1 only on shipping the licence and keeping the notices. It permits
  redistribution.

**And it is dominated, for three reasons that are about placement, not quality.**

1. **We have no seam at which to apply it correctly.** `RenderAO` writes AO into a render target
   with a blend state, and the natural injection point available to us — the present stage, over
   the back buffer — is *post-tonemap and post-direct-lighting*. Multiplying AO there darkens
   direct light, emissives and the HUD. The engine applies AO before direct lighting for exactly
   this reason (§2). To apply HBAO+ correctly we would have to overwrite `SceneContext.ScreenSpaceAO`
   inside the AO pass, which is a whole new hook of the same class as the TAA one.
2. **This project has already paid this bill once, and wrote down the lesson.** CLAUDE.md §5, "NR
   IS A PRESENT STAGE, AND THAT DELETED HALF THE FEATURE": ~4 900 lines existed solely to make one
   wrong hook point survivable, and *"when a feature keeps growing machinery to compensate for
   where it runs, the placement is the bug."* HBAO+ at the present stage is that shape again,
   before a line is written.
3. **GTAO is the same algorithm family, already correctly placed.** GTAO is horizon-based, runs at
   render resolution, has a velocity-aware temporal filter, and its output lands in
   `ScreenSpaceAO` where the composite applies it to indirect lighting only. Every problem HBAO+
   would create is one GTAO does not have, and GTAO costs a cvar.

**So: HBAO+ is a real option that we are declining on placement grounds, not availability.**
If GTAO is tried and is visibly wrong in a way the SSAO ladder also cannot fix, this is the fallback
and the SDK details above are what it needs. Otherwise, stop proposing it.

Two dead ends recorded so nobody re-checks them:

* **NVIDIA Control Panel / driver AO override** — the driver SSAO override is **D3D11/OpenGL only
  and not applied to D3D12 titles** (**SOFT**: consistently reported by NVIDIA's own forums and
  community sources; no primary doc found stating it). Stray is D3D12. And on Linux there is no
  `nvidia-settings` or DXVK-NVAPI equivalent of the per-application override (**UNCONFIRMED**, but
  moot given the D3D12 restriction).
* **ReShade AO shaders** (MXAO, qUINT, iMMERSE Launchpad) exist and are the other injected-AO shape,
  but they run inside ReShade's effect runtime on ReShade's own depth abstraction rather than as a
  library we call, and `martymcmodding/iMMERSE`'s LICENSE states *"Public propagation of this
  project or parts of it is strictly forbidden"* (**HARD**, read verbatim) — so we could not ship
  them. They also apply AO post-tonemap, i.e. reason 1 above.

---

## 6. The ranked changes

**These are all live-settable — you do not need to relaunch, and you should not edit
`Engine.ini` for the experiment.** `mods/StrayConsole` runs console commands from
`stray-console.cmd` through `UKismetSystemLibrary::ExecuteConsoleCommand`
(commit `0c22cd4`), and `FConsoleManager::ProcessUserConsoleInput` sets with
`ECVF_SetByConsole` = `0x09000000` (`ConsoleManager.cpp:1441`), the *highest* priority — above
your `[SystemSettings]`. It refuses only `ECVF_ReadOnly` and `ECVF_Cheat` cvars
(`ConsoleManager.cpp:1360-1364, 1434-1441`); **none of the cvars below is either**. **HARD.**

Ranked by expected effect per unit of risk, **assuming SSGI is OFF** (see §7 for the SSGI-on
branch, where none of 1–5 does anything):

| # | Change | What it changes | What it costs | What to look at |
|---|---|---|---|---|
| 1 | `r.AmbientOcclusionMaxQuality -100` | Forces `SHADER_QUALITY 4`: 6 directions × 3 steps instead of 3 × 2, and 20 GTAO taps instead of 8. Also removes the dependence on Stray's unseen post-process volumes. | ~3× the taps in the **full-res** AO pass only (render res, so 1920×1080 at 50% SP). The half/quarter passes are unchanged. **Unmeasured.** | Contact shadow under the cat's paws; the wall/floor seam. Sharper and darker at the contact, not different in the room corners. |
| 2 | `r.AmbientOcclusionStaticFraction 1.0` | Pins how much AO darkens **baked** lighting, overriding whatever the level's post-process volume says (declared `ConsoleManager.cpp:2225-2231`, default **-1**, `ECVF_Default` — not ReadOnly; read at `SceneView.cpp:2021-2029`, where `>= 0` REPLACES the volume's value). | No new pass — the composite already runs. ~0. | Whole-frame contrast in lit interiors. If the frame turns muddy, the artists' volume value was lower on purpose: try `0.5`, then revert to `-1`. |
| 3 | `r.AmbientOcclusion.Method 1` | GTAO instead of SSAO — horizon-based, its own velocity-aware temporal filter, one full-res pass instead of the 3-level pyramid. | Different, not obviously more: one pass + spatial + temporal filters at render res, against SSAO's 3-level pyramid. **Unmeasured, could go either way.** | Two things: (a) does large-radius occlusion in room corners *survive* — GTAO clamps to a 256-pixel screen radius where SSAO had explicit mip levels; (b) does it shimmer or ghost on the moving cat. |
| 4 | `r.AmbientOcclusionRadiusScale 1.5` | Multiplies `AmbientOcclusionRadius` (engine default 200 uu) by 1.5, clamped to [0.1, 15] (declared `ConsoleManager.cpp:2218-2223`, default 1.0, `ECVF_Scalability | ECVF_RenderThreadSafe`; read at `SceneView.cpp:2037-2042`). Widens the occlusion footprint. | Same tap count, worse cache locality. Small. | Softer, wider darkening. This is a taste knob; it is also the fastest way to make AO look wrong. Try only after 1–2. |
| 5 | `r.AmbientOcclusionMipLevelFactor 0.4` | Biases sampling toward HZB mip 0 (`PostProcessAmbientOcclusion.cpp:45-52`; 0 = always mip 0 and "memory cache trashing", 1 = higher mips and quality loss). Stray sets 0.6, the engine default is 0.5. | Cache pressure only. Small. | Fine detail crispness in the AO. The least interesting knob here. |

**Do not bother with:** `r.AOQuality`, `r.AmbientOcclusion.Denoiser`, `r.Shadow.Denoiser`,
`r.Reflections.Denoiser` — all inert (§0). And `r.AmbientOcclusionLevels` is already at its
maximum of 3.

**No cvar exists for `AmbientOcclusionIntensity`, `Power`, `Bias`, `Radius` or `Quality`
directly** — those are post-process-volume properties, and only `StaticFraction` and
`RadiusScale` got cvar overrides (`ConsoleManager.cpp:2218-2231`, read at `SceneView.cpp:2021-2042`). If after this session the remaining
want is "more/less AO strength", that is `AmbientOcclusionIntensity`, and it is reachable **only**
through UObject reflection on the level's post-process volume — a plugin change, and one to
propose separately after the cvars have been judged, not now.

### Revert

Everything above is a console `Set` at `ECVF_SetByConsole`, which is **not persisted**: killing the
game reverts every one of them. `Engine.ini` is untouched. If a value is later promoted into
`[SystemSettings]`, deleting the line restores Stray's own `[/Script/Engine.RendererSettings]`
value (§0's priority table).

---

## 7. What one launch should answer

**One launch, one gameplay session, no relaunches, no ini edits.** Drive to a fixed viewpoint —
the cat standing still on a flat lit floor near a wall, paws in frame — and hold the camera dead
still for every capture (CLAUDE.md §5). Then append one block at a time to `stray-console.cmd`,
wait ~2 s, and `tools/screenshot-gamescope.sh`.

**The question the launch must answer, in priority order:**

> **Q1 (the fork in the road). Does SSGI's AO override make every SSAO/GTAO setting invisible?**

This is first because if the source reading in §3 is right, questions 2–4 have no meaning while
SSGI is on, and the whole AO topic collapses into the SSGI decision you are already making
tomorrow.

```
# --- block A: baseline, SSGI as it is now
(capture)

# --- block B: Q1. Maximal AO change, SSGI ON. Source says: NO visible difference.
r.AmbientOcclusionMaxQuality -100
r.AmbientOcclusionStaticFraction 1.0
(capture)

# --- block C: Q1 control. Same settings, SSGI OFF. Source says: THIS one changes.
r.SSGI.Enable 0
(capture)
```

**Read it as:** B ≡ A and C ≠ A confirms §3 and settles it. B ≠ A refutes §3 and this document is
wrong about the most important thing in it — say so, and stop before spending time on 2–4.

> **Q2. Is a maxed SSAO visibly better than what ships?** (SSGI off.)

```
# --- block D: back to today's AO, SSGI still off
r.AmbientOcclusionMaxQuality 100
r.AmbientOcclusionStaticFraction -1
(capture)          # D vs C is the whole of lever 1 + 2
```

> **Q3. Is GTAO better than a maxed SSAO?** (SSGI off, quality already maxed.)

```
# --- block E
r.AmbientOcclusionMaxQuality -100
r.AmbientOcclusion.Method 1
(capture)          # E vs C
# then walk for ~10 s and capture again — GTAO's temporal filter and the
# 256-pixel radius clamp are things a still frame cannot judge.
```

> **Q4. With SSGI on, is the SSAO pass pure waste?** (The §3.2 prediction.)

```
# --- block F
r.AmbientOcclusion.Method 0
r.SSGI.Enable 1
r.AmbientOcclusionLevels 0
(capture)          # source predicts F ≡ A visually
```

If F ≡ A, run `tools/stray-bench.sh --runs 3 --label ao-levels0` against a matching
`--label ao-levels3` arm to see whether it is worth keeping — **and only on a quiet host**
(memory: *Bench invalid under host load*).

**One thing to check in the log, not the picture:** UE4SS should echo
`[StrayConsole] ran: <line>` for every command. A line that does not echo did not run, and a
capture taken after it is measuring nothing. That is the same discipline as the `NR HISTORY`
counters in CLAUDE.md §5 — instrument the thing that can silently no-op.

---

## 8. Optional: verify the GTAO shaders offline before the launch

§1.4 argues from the engine source that every GTAO shader is cooked. It can also be checked
directly against the shipped pak, on the box, with no game running — the type names
(`FGTAOHorizonSearchAndIntegrateCS`, `FGTAOHorizonSearchCS`, `FGTAOInnerIntegratePS`,
`FGTAOSpatialFilterCS`, `FGTAOTemporalFilterCS`, `FGTAOUpsamplePS`) are all present in
`tools/data/ShaderHashedNames.json`, and `tools/shaderlib_extract.py` already parses the cache's
per-section type table:

```bash
python3 tools/pakextract.py <...>/Hk_project-WindowsNoEditor.pak /tmp/shaderlib \
    'GlobalShaderCache-PCD3D_SM5\.bin$'
python3 - <<'PY'
import sys; sys.path.insert(0, 'tools')
from shaderlib_extract import parse_global_cache, load_type_names
buf = open('/tmp/shaderlib/Engine_GlobalShaderCache-PCD3D_SM5.bin','rb').read()
names = load_type_names()
for sec in parse_global_cache(buf, 'cache'):
    tn = [names.get(t, f'<0x{t:016x}>') for t in sec['type_hashes']]
    if any('GTAO' in n or 'AmbientOcclusion' in n for n in tn):
        print(sec['index'], len(sec.get('inline_shaders') or []), tn)
PY
```

**This snippet has NOT been run** — there is no pak on the authoring machine (CLAUDE.md §0). If it
throws, the engine-source argument in §1.4 still stands on its own; the check is belt and braces,
not the evidence.

---

## 9. Ledger

| Claim | Status |
|---|---|
| `r.AOQuality` is distance-field AO, and Stray cooked no distance fields | **HARD** — `DistanceFieldAmbientOcclusion.cpp:36-42`; `DefaultEngine.ini:40`; `WindowsEngine.ini:27` |
| `r.AmbientOcclusion.Denoiser` is read only inside the RTAO path | **HARD** — `RayTracingAmbientOcclusion.cpp:32-38, :203` |
| No bare `r.Shadow.Denoiser` cvar exists in 4.27 | **HARD** — `ScreenSpaceDenoise.cpp:24-44` |
| `r.Reflections.Denoiser` cannot affect SSR while `r.SSR.ExperimentalDenoiser=0` | **HARD** — `IndirectLightRendering.cpp:31-34, :771` |
| `[SystemSettings]` outranks the game's `RendererSettings` and scalability | **HARD** — `IConsoleManager.h:105-126` |
| Console `Set` outranks `[SystemSettings]`; no AO cvar is ReadOnly or Cheat | **HARD** — `ConsoleManager.cpp:1360-1364, :1441`; every declaration cited in §1 |
| Stray ships `SHADER_QUALITY 0`, the lowest permutation | **HARD** — `WindowsEngine.ini:11` + `PostProcessAmbientOcclusion.cpp:152-174` |
| `MaxQuality = -100` enforces quality 100 regardless of the post-process volume | **HARD** — `PostProcessAmbientOcclusion.cpp:154-162` |
| Every SSAO and GTAO shader is a global shader gated only on SM5 | **HARD** on the ten `ShouldCompilePermutation` bodies; **[derived]** one step on the global-shader cooking rule |
| GTAO runs on the graphics pipe with `r.AmbientOcclusion.Compute=0` | **HARD** — `PostProcessAmbientOcclusion.cpp:282-306` |
| GTAO's temporal filter is velocity-aware | **HARD** — `PostProcessAmbientOcclusion.usf:1137-1156` |
| AO is applied to indirect/baked lighting only, before direct lighting | **HARD** — `DiffuseIndirectComposite.usf:49-53`; `IndirectLightRendering.cpp:532, :570` |
| SSGI's AO mask replaces `ScreenSpaceAO` whenever SSGI runs | **HARD** — `IndirectLightRendering.cpp:504-513`; `ScreenSpaceRayTracing.cpp:1096-1099` |
| `r.SSGI.Quality=2` is 8 rays/pixel and, with `Denoiser=0`, undenoised | **HARD** — `ScreenSpaceRayTracing.cpp:517-522`; `IndirectLightRendering.cpp:467-471` |
| **With SSGI on, SSAO/GTAO settings are invisible and `Levels=0` is free** | **[derived]** from the two rows above. **UNCONFIRMED live. This is Q1.** |
| SSAO's dither is an 8-frame rotation keyed to the TAA jitter index | **HARD** — `PostProcessAmbientOcclusion.cpp:370-375`; `usf:48` |
| HBAO+ ships a standalone D3D12 library under a redistributable licence | **HARD** — `NVIDIAGameWorks/HBAOPlus` `lib/`, `include/GFSDK_SSAO.h:904-1023`, `LICENSE.txt` §2.1/§3.1 |
| Stray's post-process volumes' own AO values | **UNKNOWN.** Not extracted; this is why `MaxQuality=-100` and `StaticFraction=1.0` are phrased as *removing* a dependence rather than as improvements |
| GTAO looks better than a maxed SSAO on this content | **UNCONFIRMED.** Never run. Q3 |
| The cost of any change here | **UNMEASURED.** Every cost column above is structural (taps, passes), not a frame time |
