# RR-1 via config: disable the denoiser, keep the composite (no dispatch-skipping)

Research spike, no code. Verdict: **skip-dispatch RR-1 is dead, and it should be — a config
toggle does the same thing correctly.** Suppressing `FSSDTemporalAccumulationCS` darkened the
scene because that pass IS the SSGI diffuse-indirect denoiser, and its output is what the
composite adds to scene colour; skip the dispatch and the composite adds an unwritten buffer
(nothing) → the ambient bounce vanishes. UE 4.27 has a cvar that removes the denoiser AND
composites the raw noisy signal instead — exactly the RR-feedable image we want.

All source citations are the UE 4.27.2 mirror (`AlexMercer-MA/UnrealEngine-4.27`), verbatim,
fetched 2026-09. **HARD-via-mirror** unless marked otherwise.

---

## 1. What darkened, proven in source

`IndirectLightRendering.cpp` `RenderDiffuseIndirectAndAmbientOcclusion` (the SSGI/AO path):

```cpp
int32 DenoiseMode = CVarDiffuseIndirectDenoiser.GetValueOnRenderThread();   // r.DiffuseIndirect.Denoiser, default 1
...
IScreenSpaceDenoiser::FDiffuseIndirectOutputs DenoiserOutputs;
if (DenoiseMode != 0) {
    DenoiserOutputs = DenoiserToUse->DenoiseScreenSpaceDiffuseIndirect(...);  // dispatches FSSDTemporalAccumulationCS
} else {
    DenoiserOutputs.Color = DenoiserInputs.Color;                            // <-- RAW noisy SSGI, COMPLETE
    DenoiserOutputs.AmbientOcclusionMask = DenoiserInputs.AmbientOcclusionMask;
}
```
(`ilr.cpp:403, 435-471`, cvar declared `:26-30`, default **1**.)

Then the composite adds `DenoiserOutputs.Color` into scene colour:
```cpp
if (DenoiserOutputs.Color || DenoiserOutputs.AmbientOcclusionMask) {
    PassParameters->DiffuseIndirectTexture = DenoiserOutputs.Color;
    PassParameters->RenderTargets[0] = FRenderTargetBinding(SceneColorTexture, ELoad);
    ... FDiffuseIndirectCompositePS ... additive blend BO_Add BF_One BF_One
}
```
(`ilr.cpp:528-582`.)

So `FSSDTemporalAccumulationCS` = the **ScreenSpaceDiffuseIndirect (SSGI) denoiser** (`ssd.cpp`
signal family `ESignalProcessing::ScreenSpaceDiffuseIndirect`, `:215`). Our skip-dispatch left
`DenoiserOutputs.Color` unwritten → the composite added a stale/empty buffer → **the ambient
bounce lighting disappeared** = the darkening. This is GI, not AO or reflections (§3, §4).

**The fix is the `else` branch above:** `r.DiffuseIndirect.Denoiser=0` composites
`DenoiserInputs.Color` — the RAW noisy SSGI trace, which is COMPLETE (full lighting, just
un-denoised), not nothing. Scene stays lit; it just gets noisy. That noise flows into scene
colour → TAA `t1` → RR. **This is RR-1, done by config.**

## 2. Does config-disable still run FSSDTemporalAccumulationCS? (Q3) — NO

`r.DiffuseIndirect.Denoiser=0` takes the `else` branch, so `DenoiseScreenSpaceDiffuseIndirect`
(which dispatches `FSSDTemporalAccumulationCS`) is **never called**. The pass is gone from the
frame. Consequences:
* **Our RR-0 hook is unaffected** — no suppression, no `NgxRR=3`, no relocated trigger. The
  guide capture still fires at whatever SSD passes remain (or, if none remain, the RR-0 SSD
  trigger must be reconsidered — see §5 caveat). The noise arrives at scene colour naturally.
* **The whole skip-dispatch machinery (`NgxRR=3`) is superseded** by this config path, which
  is strictly better (no dark scene, no GPU-order coupling, no wedge class). Recommend keeping
  `NgxRR=2` (RR-0) and adding the Engine.ini below; `NgxRR=3` becomes dead code to retire once
  this is confirmed on-box.

## 3. Reflections (SSR) — already composite RAW in Stray, no cvar needed

> **RETRACTED 2026-09-04. The conclusion of this section is WRONG, and its own stated condition
> is why.** It ends *"[HARD, conditional on Stray not overriding `r.SSR.Temporal` /
> `r.SSR.ExperimentalDenoiser`.]"* — and **Stray overrides `r.SSR.Temporal`**:
> `docs/game-config/Hk_project_Config_DefaultEngine.ini:63` ships `r.SSR.Temporal=1.0` in
> `[/Script/Engine.RendererSettings]`, extracted from the game's own pak. So
> `IsSSRTemporalPassRequired` is **true**, `bTemporalFilter` is **true**, and SSR goes through
> `ETAAPassConfig::ScreenSpaceReflections` — a full temporal accumulation before the composite.
> **RR does not already receive raw SSR; it receives denoised SSR.** Everything below the
> retraction line is correct source-reading with one false premise at the top.
>
> The lesson, recorded because it is cheap and this file paid for it: a conclusion written as
> conditional makes its own falsifier explicit, and `docs/game-config/` — the game's own cooked
> inis — is one grep away. **Check the condition when you write it, not when someone else finds
> it.** Full consequences in `docs/RESEARCH-RR-REFLECTION-DENOISE.md` §1.1.

`ilr.cpp:768-772`:
```cpp
bDenoise       = DenoiserMode != 0 && CVarDenoiseSSR.GetValueOnRenderThread();  // r.SSR.ExperimentalDenoiser, default 0
bTemporalFilter = !bDenoise && View.ViewState && IsSSRTemporalPassRequired(View);
```
and `ScreenSpaceRayTracing.cpp:146-155`:
```cpp
bool IsSSRTemporalPassRequired(const FViewInfo& View) {
    return View.AntiAliasingMethod != AAM_TemporalAA || CVarSSRTemporal.GetValueOnRenderThread() != 0;
}                                                       // r.SSR.Temporal default 0
```
Stray runs **TAA** and stock `r.SSR.Temporal=0`, so `IsSSRTemporalPassRequired` = `false || false`
= **false**; and `r.SSR.ExperimentalDenoiser=0` → `bDenoise=false`. Both false ⇒ the final
`else` (`ilr.cpp:838`) runs: `ReflectionsColor = DenoiserInputs.Color` = **RAW SSR**, composited
and cleaned by the main TAA (`ETAAPassConfig::ScreenSpaceReflections` is skipped). Since RR
replaces the TAA, **RR already receives raw SSR today** — reflections need no change for RR-1.
[HARD, conditional on Stray not overriding `r.SSR.Temporal` / `r.SSR.ExperimentalDenoiser`.]

`r.Reflections.Denoiser` (default 2, `ssd.cpp:42-44`) only matters if `r.SSR.ExperimentalDenoiser=1`
(licensee override, unknown). If it is, `r.Reflections.Denoiser=0` forces raw — precautionary
only.

## 4. AO — not the darkening; not an FSSDTemporalAccumulationCS user in Stray

SSAO in the deferred renderer is `PostProcessAmbientOcclusion` — a **separate** pass with its
own spatial filter, not the SSD denoiser. The SSD `AmbientOcclusion` signal family
(`ssd.cpp:212`) is for **ray-traced** AO (`RenderRayTracingAmbientOcclusion`, `ilr.cpp:490-499`),
which a raster D3D12 Stray does not run. So AO did not cause the darkening and needs no denoiser
cvar. Under `r.DiffuseIndirect.Denoiser=0` the AO mask also flows raw (the `else` sets
`AmbientOcclusionMask = DenoiserInputs.AmbientOcclusionMask`). [HARD for the SSAO/SSD split.]

## 5. The SSGI-enable caveat (the one open question)

`SupportScreenSpaceDiffuseIndirect` requires `r.SSGI.Enable>0` (`ScreenSpaceRayTracing.cpp:75-80`,
stock default **0**). Yet Stray demonstrably dispatches `FSSDTemporalAccumulationCS` in live
gameplay (`docs/RESEARCH-RR-GBUFFER.md §1.6`), so **SSGI must be enabled in Stray** (a licensee
ini/runtime — the documented stock-vs-live delta). Therefore `r.DiffuseIndirect.Denoiser=0` will
take effect. If the empirical test shows *no change* from that cvar, then the
`FSSDTemporalAccumulationCS` we saw is denoising a different signal (unlikely — Reflections and
AO denoisers are default-off), and we re-derive. [Inference, UNCONFIRMED until the on-box test.]

One thing to watch under §2: if `r.DiffuseIndirect.Denoiser=0` removes the *only* SSD pass in the
frame, the RR-0 `GBufferResolveAt=ssd` trigger loses its anchor (no SSD dispatch to capture at).
If so, capture reverts to needing the hoist-to-lighting-pass work — but that is a code question
for after the config is confirmed to produce a noisy-lit image; do not pre-solve it.

---

## 6. Candidate `[SystemSettings]` set — incremental, each cited, each predicted

Add to `Engine.ini` `[SystemSettings]` (the only section that takes effect — CLAUDE.md §2.2),
reversible, and mark the file read-only (the community guide's required step). Test **one at a
time**, watching brightness first.

| # | cvar | source | prediction | risk |
|---|---|---|---|---|
| **1** | `r.DiffuseIndirect.Denoiser=0` | `ilr.cpp:435-471` (else composites raw) | scene **stays lit, GI gets noisy**; `FSSDTemporalAccumulationCS` disappears from the frame | **MODERATE** — if it darkens instead, the raw-composite assumption is wrong; report back immediately |
| 2 | *(no cvar)* — reflections | §3 | SSR already raw under TAA; **no change expected** | none |
| 3 | `r.Reflections.Denoiser=0` | `ssd.cpp:42-44` | **no change** if SSR uses temporal/raw (expected); forces raw only if Stray set `r.SSR.ExperimentalDenoiser=1` | LOW |
| 4 | `r.SSR.Temporal=0` | `ScreenSpaceRayTracing.cpp:32-37` | already the default; **no change** — set only to pin it | none |
| — | `r.SSGI.Quality` (Stray ships 2) | `ScreenSpaceRayTracing.cpp:62` | leave as-is; raising it adds samples = **less** raw noise (easier for RR) if the grain is too heavy | none |

**Do NOT** set `r.SSGI.Enable=0` (turns GI off entirely = dark) or re-introduce any
`DryRunHash`/skip-dispatch. **Do NOT** touch `r.DefaultFeature.AntiAliasing` (RR still needs the
TAA jitter/velocity machinery, CLAUDE.md §4).

## 7. Recommended test order (on-box, reversible, user watching)

1. **`r.DiffuseIndirect.Denoiser=0` alone, RR OFF (`NgxRR=0`).** Confirm the scene is **lit and
   noisy**, not dark. This is the whole hypothesis: config-disable composites raw (SUCCESS) vs
   dark (FAIL → the raw-composite path is not what source says; stop and report). This is the
   single decisive test.
2. **Same cvar + RR-0 (`NgxRR=2`).** RR now denoises the raw SSGI. **This is RR-1 via config** —
   no `NgxRR=3`, no skip-dispatch. Judge: is the cyan/red neon shimmer cleaner than UE's
   denoiser (toward the measured 3.4× target), or does RR produce garbage on raster GI noise?
3. **Optionally `r.Reflections.Denoiser=0`** if reflections still look over-smoothed. Expected
   no-op; harmless.

The prize: **RR-1 becomes "RR-0 + one Engine.ini line,"** sidestepping the skip-dispatch
darkening and the whole `NgxRR=3` suppression-coupling problem entirely.

Sources: UE 4.27.2 mirror (`IndirectLightRendering.cpp`, `ScreenSpaceRayTracing.cpp`,
`ScreenSpaceDenoise.cpp`); community precedent
[Better Raytracing for Stray!](https://steamcommunity.com/sharedfiles/filedetails/?id=2992430097)
(confirms `r.Reflections.Denoiser=0` composites noisy reflections, not nothing).
