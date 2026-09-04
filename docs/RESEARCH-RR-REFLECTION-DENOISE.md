# DLSS Ray Reconstruction as Stray's reflection denoiser: the verdict, the design, and the one launch

Research spike, 2026-09-04, branch `rr-reflection-denoise`. The brief was *"bring DLSS Ray
Reconstruction back as the denoiser for screen-space reflections"*, with an explicit licence to
answer "the noise it would denoise is already gone" if that is what the evidence says.

**No RR machinery was built and nothing has run on the box.** What this branch ships is the
verdict below, the corrections it forces in three other documents, `tools/dispatch_census.py`,
and one repair without which §4's launch would have produced nothing (`DumpShaders` was inert
under the shipping host — §4's opening note).

Provenance labels follow CLAUDE.md §0.5. UE citations are 4.27.2 via the mirror
`AlexMercer-MA/UnrealEngine-4.27`, quoted second-hand here through
`docs/RESEARCH-RR-GBUFFER.md` and `docs/RESEARCH-RR1-DENOISER-CONFIG.md`, which read them
directly — **HARD-via-mirror-via-our-own-doc**, one notch weaker again, and flagged where it
matters.

---

## 0. Verdict, up front

**RR does not have a job as a reflection denoiser in Stray, and three independent arguments say
so. Only the first is contingent on a measurement.**

1. **There is probably no noise left.** Every stochastic signal RR denoises has been configured
   out of this game: `r.RayTracing=False` (no RT), `r.SSGI.Enable=0` (no screen-space GI), and
   `r.SSR.Temporal=1` — which is not a new override at all but **Stray's own shipped setting**
   (`docs/game-config/Hk_project_Config_DefaultEngine.ini:63`, `[/Script/Engine.RendererSettings]`),
   and which routes SSR through the engine's own dedicated temporal filter *before* it is
   composited into scene colour. The four `*.Denoiser=0` lines in the live ini are, under
   `r.RayTracing=False` + `r.SSGI.Enable=0`, **inert**, one by one:
   `r.AmbientOcclusion.Denoiser` is not a real top-level 4.27 cvar at all
   (`docs/RESEARCH-GFX-MOD-209.md` §2.4 could not locate a declaration; classic SSAO does not
   route through `IScreenSpaceDenoiser`); `r.Shadow.Denoiser` and `r.Reflections.Denoiser` gate
   **ray-traced** shadow and reflection denoising, which needs `r.RayTracing=True`; and
   `r.DiffuseIndirect.Denoiser` gates the SSGI denoiser, which needs `r.SSGI.Enable>0`. They gate
   signal families that are not being produced. The user's report that they see little noise
   today is exactly what this configuration predicts. **This is the one part a launch must
   confirm rather than assume** (§4).
2. **RR would replace a working engine denoiser, not an absent one.** The SSR temporal filter is
   already running — §1.3 gives already-measured evidence from our own box — and it is
   purpose-built for this signal. Replacing it with a network trained on ray/path-traced input
   is a trade of a known-good for an unknown, at the cost of restoring ~1 100 deleted lines, a
   level-3 rung on a hook that is at level 1 and has never seen a single render target, a new
   guide-record trigger, and **giving up DLSS SR**, since RR replaces it rather than stacking.
3. **Reflections are the content RR is *least* equipped for, and this argument holds even if the
   launch finds noise.** RR reprojects its own temporal history with the motion vectors we
   supply, which describe the *surface*; a reflection moves with the reflected geometry.
   NVIDIA's own remedy is specular motion vectors or specular hit distance
   (`RESEARCH-RR-GBUFFER.md` §2.1), and a screen-space trace gives us neither in any form we can
   read. The guides have the same defect: on a wet floor, `GBufferA/B/C` say "dark, smooth
   surface" while the pixels hold a moving reflected image — RR-GBUFFER §2.5's *"guides
   describing the wrong surface (mirrors) → cross-hatching / ghosting in reflections"* [SOFT,
   first-hand integrator]. **CLAUDE.md has already measured this exact mechanism defeating DLSS
   Neural Rendering on this exact content** and concluded: *"Treat 'temporal network +
   screen-space reflections' as a structural mismatch, not an open bug."* RR shares the motion
   vectors, keeps its own temporal history, and additionally consumes surface guides — so it
   inherits the mismatch and adds to it.

**What to do instead, if the launch finds reflections noisy.** Three ini lines beat this whole
branch on cost and risk: `r.SSR.Quality` (more rays), `r.SSR.HalfResSceneColor=0` (Stray ships
**1**, `Hk_project_Config_Windows_WindowsEngine.ini:12` — half-resolution SSR is by far the
largest reflection-quality lever available here and is a *resolution* deficit, which no denoiser
fixes), and leaving `r.SSR.Temporal=1` where it is.

**What this does NOT say.** It does not say RR is worthless on this title. The one case that
survives §0.2 and §0.3 is **RR as a better upscaler than SR**, using material guides to
reconstruct thin geometry — a different claim, with a different target, and one nobody has
evidence for on raster content (`RESEARCH-RR-GBUFFER.md` §2.6 marks it UNCONFIRMED and
unanswerable off-target). If that is the goal, say so, because it changes the acceptance
criterion from "less shimmer in reflections" to "sharper thin geometry at equal cost", and it
makes reflections a *risk* to be watched rather than the objective.

---

## 1. Is there noise? What the configuration and the existing measurements say

### 1.1 The correction that matters: `r.SSR.Temporal=1` is SHIPPED, not an override

`docs/RESEARCH-RR1-DENOISER-CONFIG.md` §3 concluded that **raw, undenoised SSR already reaches
our hook**, and therefore that "reflections need no change for RR-1". Its own text flags the
condition: *"[HARD, conditional on Stray not overriding `r.SSR.Temporal` /
`r.SSR.ExperimentalDenoiser`.]"*

**Stray does override it.** `docs/game-config/Hk_project_Config_DefaultEngine.ini:63`, inside
`[/Script/Engine.RendererSettings]` (lines 30-90):

```ini
r.SSR.Temporal=1.0
```

That is the game's own cooked project setting, extracted from the pak by `tools/pakextract.py`
long before this branch existed. It inverts the §3 chain exactly:

```cpp
// ScreenSpaceRayTracing.cpp:146-155
bool IsSSRTemporalPassRequired(const FViewInfo& View) {
    return View.AntiAliasingMethod != AAM_TemporalAA || CVarSSRTemporal.GetValueOnRenderThread() != 0;
}
// IndirectLightRendering.cpp:768-772
bDenoise        = DenoiserMode != 0 && CVarDenoiseSSR.GetValueOnRenderThread();   // false
bTemporalFilter = !bDenoise && View.ViewState && IsSSRTemporalPassRequired(View);  // TRUE
```

`IsSSRTemporalPassRequired` is `false || true` = **true**, so `bTemporalFilter` is true and SSR
runs through `AddTemporalAAPass` with `ETAAPassConfig::ScreenSpaceReflections` — a full temporal
accumulation over the SSR buffer, with history, before the composite. **The signal reaching our
TAA hook is denoised SSR, not raw SSR.** RR-1-by-config for reflections is therefore not "already
done"; it is "already done in the opposite direction".

**Consequence for the brief.** "Bring RR back as the denoiser for SSR" cannot mean "denoise what
arrives"; nothing noisy arrives. It can only mean **turn the engine's SSR temporal filter off
(`r.SSR.Temporal=0`) and have RR do that job instead** — which is a straight substitution of a
neural network for a purpose-built filter, on content the network was not trained on and whose
motion it cannot describe (§0.3).

### 1.2 The 3.4× shimmer gap does not transfer, and its configuration is gone

CLAUDE.md's denoiser-suppression experiment — *"Median frame-to-frame shimmer is 3.4x higher with
the denoiser off"* — is the quantified target the brief cites. Two reasons it cannot be the
target here:

* **It measured the SSGI diffuse-indirect denoiser, not reflections.**
  `RESEARCH-RR1-DENOISER-CONFIG.md` §1 identifies `FSSDTemporalAccumulationCS` in this title as
  the **ScreenSpaceDiffuseIndirect** signal family, and §3/§4 establish that reflections and AO do
  not route through it here. With `r.SSGI.Enable=0`, that signal is not produced at all — so the
  measurement is about a subsystem that is now off.
* **It was measured on 2026-08-31, with `r.RayTracing=True`-era content in the frame.** CLAUDE.md
  itself records that most of what looked like denoiser noise that week *was ray-traced noise*
  ("the cyan/red blips on neon sources ... were RT noise ... Turning the subsystem off removed
  them at the source").

**Nobody has measured what actually dispatches under tonight's configuration.** That is the gap
§4 closes.

### 1.3 Already-measured evidence that the SSR temporal filter IS running (strong, not yet HARD)

`docs/RESEARCH-ENGINE-TAA-HOOK.md` §10.2 records the engine seam catching two TAA-family
look-alikes on the box, by hash and by group count:

```
ENGINE SEAM DISAGREES about pass 0xe3ddca4be9830076 ... (240x135 groups)
ENGINE SEAM DISAGREES about pass 0x42af595f8ff91038 ... (120x68 groups)
```

The TAA tile is 8×8 and the render rect is 1920×1080.

| pass | groups | implied rect | reading |
|---|---|---|---|
| `0xe3ddca4be9830076` | 240×135 | 1920×1080 | a full-render-res temporal pass |
| `0x42af595f8ff91038` | 120×68 | 960×540 | **exactly half render res** — `ceil(960/8)=120`, `ceil(540/8)=68` |

Stray ships `r.SSR.HalfResSceneColor=1`, so SSR traces at **960×540**, and
`ETAAPassConfig::ScreenSpaceReflections` dispatches over the SSR buffer's rect. A half-render-res
member of the `FTAAStandaloneCS` family is what the SSR temporal filter looks like, and 120×68 is
not an approximate match but an exact one including the odd rounding of 540/8.

**This is inference from arithmetic, not a name.** §4 turns it into a name for free.

The §10.2 doc attributes the growth of these two to *"once the SSD passes were running every
frame"* — i.e. it read at least one of them as a screen-space-denoiser pass, which would mean
SSGI was *on* as of 2026-09-03. Tonight's `r.SSGI.Enable=0` postdates that. **Two readings of the
same two hashes, both plausible, both cheap to settle.**

---

## 2. Where RR would sit, what it replaces, and what happens to NR

Answering the brief's question 2 concretely, so that a future session does not have to re-derive
it whether or not this branch proceeds.

### 2.1 The site

**Exactly where DLSS SR sits, and instead of it.** `src/taa_hook.cpp:2112` is
`ok = ngx::evaluate(native, ei);`, reached only for a dispatch the engine announced through
`ITemporalUpscaler::AddPasses` (`EngineSeam=3`). RR is `ngx::ensure_feature_rr` +
`ngx::evaluate_rr` on the same command list, for the same rect, writing the same `u0`, suppressing
the same engine dispatch. `NVSDK_NGX_DLSSD` *"effectively overrides DLSS-SR execution"*
(RR-GBUFFER §2.6) — the two are never both evaluated on one frame.

**SR stays as the per-frame safety net**, exactly as the deleted wiring had it: any refused guide
means this frame falls back to `ngx::evaluate` and the reason is counted. That property is what
makes RR shippable at all, and it must not be traded away for a simpler control flow.

### 2.2 What happens to NR

Mechanically, nothing. `nrhook::note_guides(depth, mv, extent, reset)` at `taa_hook.cpp:2128`
publishes the guides the present-stage NR consumes; RR produces the identical depth, motion
vectors and camera-cut OR, so NR keeps receiving them.

**But NR should be OFF (`NgxNR=0`) for RR's first evaluation, and this is not caution — it is
attribution.** CLAUDE.md's SSR finding for NR and §0.3's finding for RR are *the same mechanism*
on *the same content*. Running both means a reflection artefact has two candidate owners and the
session proves nothing. FG is unaffected either way (it consumes present-time back buffers and
the same published guides).

### 2.3 What RR needs that does not exist

| need | state today |
|---|---|
| NGX side (`ensure_feature_rr`, `evaluate_rr`, `release_feature_rr`, `RRStatus`, `SuperSamplingDenoising.*`, matrix plumbing) | **intact and untouched**, `src/ngx_backend.{hpp,cpp}` |
| four guide textures (diffuse albedo, specular albedo, normal, roughness) | **absent** — `gbuffer_resolve` + `core/envbrdf` + `shaders/gbuffer_resolve.hlsl` deleted 2026-09-03 |
| identity of `GBufferA/B/C` | **absent** — the heuristic finder deleted; the named-pool route is at level 1 (§3) |
| a record point where G-buffer CONTENT is alive | **absent, and not solved by names** (§3.2) |
| `NgxRR` wiring | refuses at ERROR in `dlss_app.cpp:486`, correctly |

Restoring the resolve is cheap and should be done from git history
(`git show <deletion-commit>^:src/gbuffer_resolve.cpp`), never rewritten: it was CI-green, it
implements NVIDIA's own sanctioned `EnvBRDFApproxRTG` recipe verbatim (RR-GBUFFER §2.3-§2.4), and
it had its own tests. It comes back **without** `gbuffer_classify` and **without** the finder; its
three inputs become three named pool records.

---

## 3. The guide route, assessed against what is actually on `pool-name-hook`

Read 2026-09-04 from the author's live worktree, because the branch itself carries **zero
commits** — `pool-name-hook` is a bare pointer at `origin/main` (`1aa3986`) and does not exist on
origin. Everything below is a snapshot of work in flight and **is not an interface contract**.

### 3.1 What it gives, and what it does not

`stray_dlss::poolhook::record(pool::Target, Record&)` returns a by-value POD carrying
`targetable` / `shader_resource` as raw `std::uint64_t` `ID3D12Resource*`, plus extent and DXGI
format from our own registry. `pool::Target` is an enum with `gbuffer_a` … `gbuffer_f`,
`scene_depth_z`, `gbuffer_velocity`, `scene_color_deferred` and more; the lookup is enum-keyed,
not string-keyed. The name table is source-read from Epic's 4.27 tree with per-name file:line
provenance.

**Four things must land before RR can consume it, and none of them are RR's to build:**

1. **Level 3 (`supply`) does not exist.** `configure()` logs at ERROR and downgrades 3→2. The
   author has deliberately made feeding RR a separate decision, which is the right call.
2. **`Record::frame` cannot be used as a staleness test.** The thunk writes `rec.frame = 0` and
   `note_engine_frame()` then stamps *every* seen record with the caller's frame — so it reads
   "the last frame anyone called `note_engine_frame`", not "the frame this target was allocated".
   Records are sticky: `seen` is never cleared, so a pointer from 500 frames ago still reads
   `seen=true`. RR needs a per-record write-frame and a consume-once sequence counter of the
   `nrhook::note_guides` shape.
3. **`record()` performs no liveness check** despite the header saying it does; liveness is
   checked at *write* time inside the thunk. Any consumer must re-check
   `icept::backend()->is_resource_live()` itself — the §5 vice (`vkCreateImageView` faulting on a
   destroyed resource) is exactly what killed the resolve path on save-load transitions before.
4. **Nothing has ever been observed.** Zero names seen, no census line ever emitted, no ini key
   plumbed, `configure()` never called. Whether `GBufferA/B/C` resolve at all on
   `Stray-Win64-Shipping.exe` is an untested prediction.

**Do not build against it yet, and do not build a second guide source.** This branch therefore
ships no guide-fetch interface: a named seam with a loud null implementation would be exactly the
"machinery that exists to compensate for a decision not yet made" pattern CLAUDE.md warns about,
and it would set an API against a moving target. The four items above are the acceptance criteria
to hand the pool author *if and only if* §4 says RR has a job.

### 3.2 The problem names do NOT solve: content liveness

The deleted wiring recorded the guide resolve at **the first `FSSDTemporalAccumulationCS`
dispatch of the frame**, not at the TAA hook, and `src/taa_hook.cpp:370-380` still carries the
reason verbatim:

> Measured 2026-08-31 (guide dumps 600/900): at the TAA dispatch the identified A/B/C resources
> are ALIVE — liveness passes, bind-age 0 — but their CONTENT is already recycled: GBufferA read
> near-black (normals mean = normalized(-1,-1,-1)), ShadingModelID decoded 0, and the unlit
> fallback covered ~95% of every guide.

**The pool route fixes identity. It does not fix this.** And two things now cut against that
finding in opposite directions:

* **It may never have been true.** The measurement is only as good as the heuristic
  identification that produced it — and that identification is the thing we deleted for being
  unreliable. "Near-black GBufferA with ShadingModelID 0" is equally the signature of *reading
  the wrong texture*. Against it: UE 4.27 holds `GBufferA-C` at refcount 1 from
  `AllocGBufferTargets` until an RDG pass at the very end of `Render` (RR-GBUFFER §1.1, HARD), so
  a same-frame content recycle should not happen. **The pool route settles this in one log line**,
  which is a genuinely good reason to want it — independent of RR.
* **Its remedy may no longer exist.** If `r.SSGI.Enable=0` has removed the SSD family from the
  frame, the trigger the old code relied on is gone, and a new record point has to be found. §4
  answers this too.

---

## 4. The one launch

The launch answers §0.1 and §1.3 together and is decisive in both directions. **It needs this
branch's build**, for one reason found while writing the recipe:

> **`DumpShaders` WAS DEAD UNDER THE PLUGIN HOST, silently.** `shader_dump::initialise()` was
> called only from `src/backend_reshade/addon_entry.cpp`'s `DLL_PROCESS_ATTACH`; the UE4SS
> plugin — the shipping configuration — has no such entry, so `g_enabled` stayed false and the
> key `mods/StrayDLSS/StrayDLSS.ini:25` advertises did nothing. `DlssApp::configure_events()`
> had *always* assumed otherwise: its `needs.pipeline_events = hash_shaders ||
> shader_dump::enabled()` consulted a value nothing had ever set. Fixed on this branch —
> `initialise()` is now idempotent and called from `configure_events()` (both hosts), and
> `finish()` from `DlssApp::shutdown()` so the manifest is closed under the plugin host too.
> **The instrument the whole verdict rests on had to be repaired before it could be used**,
> which is its own small argument for measuring rather than reasoning.

### 4.1 What to set

`[STRAYDLSS] DumpShaders=1`. Everything else unchanged (`NgxRR=0`, current ini). This writes
`stray-dlss-shaders\manifest.txt` with one `dispatch 0x<hash>  XxYxZ` line for the first three
dispatches of **every compute shader the game actually runs**
(`src/shader_dump.cpp:103-118`, `src/app/dlss_app.cpp:1040-1058`), plus the DXBC of each.

Run the standard bench scenario (`tools/stray-bench.sh`, or one `stray-traverse.sh` cycle) so the
frame is a real gameplay frame, in **The Slums**, and stand somewhere wet and reflective — the
content this branch is about. Take the census **before** issuing any console command.

### 4.2 What to read, offline

```
python3 tools/dispatch_census.py stray-dlss-shaders/manifest.txt \
    --cache /tmp/shaderlib/Engine_GlobalShaderCache-PCD3D_SM5.bin
```

That names every dispatched global compute shader by its `.usf` path
(`tools/shaderlib_extract.py --find-hash` + CUE4Parse's table). The verdict is read off four
rows:

| look for | if present | if absent |
|---|---|---|
| `SSRT/SSRTReflections.usf` | SSR is tracing — reflections exist | there is nothing to denoise; **stop here** |
| a `FTAAStandaloneCS` permutation at **120×68 groups** | the SSR temporal filter is running (§1.3 confirmed) — the engine already denoises reflections | `r.SSR.Temporal` is not taking effect; §1.1 is wrong and RR-1-by-config is back on the table |
| `SSRT/SSRTDiffuseIndirect.usf` | SSGI is still on despite `r.SSGI.Enable=0` — the 3.4× target may still apply | SSGI is genuinely off; §1.2 holds |
| `ScreenSpaceDenoise/SSDTemporalAccumulation.usf` | the SSD family still dispatches — the old guide-record trigger still exists (§3.2) | that trigger is gone and a new record point is needed |

### 4.3 The optional second half, in the same session

Only if `SSRTReflections` is present. Using the existing `mods/StrayConsole` mod (drop lines into
`stray-console.cmd`; it runs them on the game thread through `ExecuteConsoleCommand`), alternate
**within one session**, camera **still**, at the same wet location:

```
r.SSR.Temporal 0      ... hold ~20 s, capture ...
r.SSR.Temporal 1      ... hold ~20 s, capture ...
```

Metric: **consecutive-frame difference**, not absolute sharpness — CLAUDE.md's own conclusion
that *"comparing a frame against ITSELF a moment later, camera still, is far better"*. n ≥ 15
pairs per phase, medians not means, crop away animated screens.

Pre-registered readings:

* **`Temporal 0` shimmers far more** → SSR really is a stochastic signal and the engine filter is
  doing real work. RR then has a *possible* job, with the engine's own filter as the baseline it
  must beat, and §0.3 as the reason to expect it will not. Next lever is `r.SSR.Quality` /
  `r.SSR.HalfResSceneColor=0`, not RR.
* **Indistinguishable** → SSR under this configuration is not noisy (half-res, few rays, and
  cleaned again by the main TAA downstream). **RR has no denoising job in this game at all.**
  Close the question, delete `NgxRR` rather than leaving a refusal to re-litigate, and reopen only
  under the different claim of §0's last paragraph.

### 4.4 What the launch deliberately does NOT do

It does not enable `PoolNames`, `NgxRR`, or anything that changes a pixel. Prime directive 2 cuts
both ways: a feature that cannot work must say so, and a feature whose *premise* is unmeasured
must be measured before it is built. This spike's entire cost is one ini key, one gameplay
session, and one offline command.

---

## 5. Provenance ledger

| Claim | Status |
|---|---|
| Stray ships `r.SSR.Temporal=1.0` in `[/Script/Engine.RendererSettings]` | **HARD** — `docs/game-config/Hk_project_Config_DefaultEngine.ini:63`, extracted from the game's own pak |
| Stray ships `r.SSR.HalfResSceneColor=1`, `r.SSGI.Quality=2`, `r.SceneColorFormat=3` | **HARD** — `Hk_project_Config_Windows_WindowsEngine.ini:12,77,69` |
| The live `Engine.ini [SystemSettings]` carries `r.SSGI.Enable=0`, `r.RayTracing=False` and the four `*.Denoiser=0` lines | **REPORTED BY THE USER, 2026-09-04, not read by us.** The box has not been touched from this branch. If any of it is wrong the §0.1 argument weakens; §4's census does not depend on it |
| `r.AmbientOcclusion.Denoiser` is not a real 4.27 cvar; `r.Shadow.Denoiser` / `r.Reflections.Denoiser` gate ray-traced signals; `r.DiffuseIndirect.Denoiser` gates SSGI | **HARD-via-`docs/RESEARCH-GFX-MOD-209.md` §2.4** and `RESEARCH-RR1-DENOISER-CONFIG.md` §1,§4 |
| `r.SSR.Temporal!=0` under TAA ⇒ `bTemporalFilter` ⇒ `ETAAPassConfig::ScreenSpaceReflections` | **HARD-via-mirror-via-`RESEARCH-RR1-DENOISER-CONFIG.md` §3**, which quotes `ScreenSpaceRayTracing.cpp:146-155` and `IndirectLightRendering.cpp:768-772` |
| `RESEARCH-RR1-DENOISER-CONFIG.md` §3's conclusion (raw SSR reaches TAA) is void | **HARD** — its own stated condition is falsified by the line above |
| `FSSDTemporalAccumulationCS` in Stray is the SSGI diffuse-indirect denoiser, not reflections or AO | **HARD-via-mirror-via-`RESEARCH-RR1-DENOISER-CONFIG.md` §1,§3,§4** |
| `0x42af595f8ff91038` at 120×68 groups is the SSR temporal filter | **UNCONFIRMED** — arithmetic only (960×540 at tile 8, exact incl. the 540/8 rounding). §4.2 names it |
| RR requires specular MVs or specular hit distance for correct reflection reprojection; we can produce neither | **HARD** for the requirement (`RESEARCH-RR-GBUFFER.md` §2.1, from NVIDIA's guide); **[derived]** that a screen-space trace cannot supply them |
| "Temporal network + screen-space reflections is a structural mismatch" | **measured for NR** (CLAUDE.md, the refuted-u0-feedback section, with the `NgxNRRestoreHistory` counters as the refutation); **[derived]** that RR inherits it |
| RR overrides SR rather than stacking | **HARD** — DLSS-RR Integration Guide §3.2, via `RESEARCH-RR-GBUFFER.md` §2.6 |
| At the TAA dispatch, G-buffer CONTENT is recycled | **SOFT, and now doubted** — measured 2026-08-31 through the heuristic identification we deleted for unreliability; contradicted by RR-GBUFFER §1.1's refcount lifetime (HARD) |
| `pool-name-hook` carries no commits, is level 1, has observed nothing, and level 3 is refused | **HARD** — read from the author's worktree 2026-09-04; a moving target, not a contract |
| `DumpShaders=1` writes a dispatched-shader census usable with `--find-hash` | **HARD** — `src/shader_dump.cpp`, `src/app/dlss_app.cpp:1040-1058`, `tools/shaderlib_extract.py:398` |
| Before this branch, `DumpShaders` was inert under the UE4SS plugin host | **HARD** — `shader_dump::initialise()` had exactly one caller, `src/backend_reshade/addon_entry.cpp:550`, and `g_enabled` defaults false. Fixed here; **the fix itself is UNCONFIRMED live** until the launch produces a manifest |
| The 3.4× shimmer gap does not describe reflections under tonight's configuration | **[derived]** from the two HARD rows about `FSSDTemporalAccumulationCS` and `r.SSGI.Enable=0`; §4 measures it |
