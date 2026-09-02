# Backlog: Stray's abandoned ray-tracing layer — the artifacts, and the tests that would explain them

**Status: set aside 2026-09-02. Not in scope for any running agent.** Written down so the tests are
not re-derived when someone picks it up. Nothing here is measured yet unless marked; the
diagnoses are SOFT reasoning from the symptoms and the UE 4.27 RT design.

## What we know (HARD)

* Stray ships `r.RayTracing=True`, but ray tracing in UE 4.27 needs D3D12 and the game launches
  under D3D11 by default. The RT layer is therefore **dead configuration for every ordinary
  player** and only wakes up under `-dx12`, which this project needs for NGX. Nobody shipped these
  artifacts to a customer; the layer reads as work started and abandoned behind the flag.
* With `-dx12`, `r.RayTracing=False` in `Engine.ini [SystemSettings]` nearly triples the frame rate
  (32 → 95 fps in gameplay) and removes the neon "blips" at the source (CLAUDE.md §5,
  "r.RayTracing=True is the single biggest problem"). That is the shipping default for this
  project and the cheapest fix for everything below.
* Suppressing UE's screen-space denoiser triples frame-to-frame shimmer (3.4× median), so the
  noise is real single-sample RT noise that only the denoiser's temporal accumulation hides.

## The three artifacts, the user's descriptions, and the working diagnosis (SOFT)

| # | Symptom (user, 2026-09-02) | Working diagnosis |
|---|---|---|
| 1 | **Blips**: cyan/red flashes on neon and other small bright emitters | Denoiser history loss. One ray per pixel against a tiny, extremely bright emitter is maximum variance; any history reset (disocclusion, fast turn, animated light, missing motion vectors) shows raw noise for a frame. Not fixable per object; only by fewer/more samples, effect selection, or a better denoiser (RR/NR). |
| 2 | **Circling / sweeping light on trees**: a smooth brightness gradient travelling around the canopy in a cycle, "like an RGB keyboard on rainbow mode but with the light" | Not noise: a structured wave is the **sampling or refit schedule made visible**. Foliage cards have wind animation (WPO) and no usable motion vectors, so temporal accumulation cannot blend across frames; whichever region got fresh samples this frame reads differently from the stale ones, and the region walks the surface in order. Two candidate mechanisms, distinguishable by test T2. |
| 3 | **Emissive fixtures shadowing themselves** (the hanging Japanese lanterns) | Emissive materials do not emit in UE4's ray tracer; the lanterns light the scene through placed lights tuned for the baked/raster path. RT shadows make those lights cast against the fixture mesh sitting on top of them, which the shadow maps never included. Per-light / per-mesh, artist-side work that was never done. |

## The tests, in the order to run them

Each is one launch with the tooling in `tools/` (safe launcher, gamescope screenshots, the bench
for stability). All need `r.RayTracing=True` restored in `Engine.ini` for the session, then put
back to `False`. Use the console enabler mod (`ue4ss/Mods/ConsoleEnablerMod`) or a Lua
`ExecuteConsoleCommand` hook to change cvars live; **hold the camera still and take n ≥ 5
screenshots per state** (CLAUDE.md §5's rule; this scene animates).

**T0 — Baseline capture.** Same save, fixed viewpoint with a tree, a lantern and a neon sign in
frame. 16 screenshots 0.5 s apart, RT on, all effects at shipped settings. This is the reference
every later test is compared against (consecutive-frame difference in a crop around each artifact).

**T1 — Effect bisection (which RT effect makes each artifact).** One cvar per run, everything else
shipped: `r.RayTracing.Shadows 0`, `r.RayTracing.AmbientOcclusion 0`, `r.RayTracing.Reflections 0`,
`r.RayTracing.Translucency 0`, `r.RayTracing.GlobalIllumination 0`. Record which artifact survives
which run. Prediction: #2 and #3 die with shadows (and #2 partly with AO); #1 dies with
reflections and/or translucency (neon through glass).

**T2 — The wave's mechanism (geometry refit vs denoiser).** With RT on and shadows on, freeze the
wind: `foliage.WindEnabled 0` if present, else set the wind source actor's speed to 0 via a Lua
property write (find `WindDirectionalSource` actors). Hold the camera still; 16 screenshots.
* Wave keeps moving with the wind frozen → the denoiser's history acceptance is walking the
  surface (the reprojection failing in blocks). Fix direction: motion vectors for foliage
  (`r.BasePassOutputsVelocity` is already on; check the foliage material's "output velocity due
  to WPO" flag) or take foliage out of the temporal denoiser.
* Wave freezes with the wind → the RT acceleration structure refit trails the raster geometry.
  Fix direction: `r.RayTracing.Geometry.*` refit cvars, or foliage excluded from RT (T3).

**T3 — Take foliage out of RT (the likely fix for #2, and a big GPU win).** On the live foliage
components (`FoliageInstancedStaticMeshComponent` / the tree meshes), set "visible in ray
tracing" off, or only "cast ray-traced shadows" off, via a Lua mod at level load (the fur mod's
pattern). Compare against T0: the wave must vanish, and the RT frame time should drop
measurably (foliage is the densest geometry in the acceleration structure).

**T4 — Alpha-masked traversal for foliage (the alternative fix for #2).** On the foliage master
material, enable masked evaluation during ray traversal (the "evaluate opacity mask in RT"
material flag; `r.RayTracing.Shadows.EnableMaterials 1` must be on). If the leaves' cutouts are
then respected, the card-shaped occlusion goes away. Costs GPU time; T3 is preferred unless
foliage RT shadows are wanted.

**T5 — Lantern audit and fix (#3).** Walk one level with screenshots, list every fixture that
shadows itself and its light actor (`FindAllOf("PointLightComponent")` etc., match by proximity
to the emissive mesh). Fix candidates, cheapest first: `CastRaytracedShadow=false` on the light;
`bCastRayTracedShadow=false` on the fixture mesh only; lighting channels so the light and its
own fixture do not see each other. Applied at level load by name. This is an audit list, not
engineering; budget it as a screenshot walk.

**T6 — Cheaper blips (#1), knowing they cannot be removed this way.** With reflections identified
in T1: `r.RayTracing.Reflections.MaxRoughness` down, `r.RayTracing.Reflections.SamplesPerPixel`
up, `r.RayTracing.Reflections.ScreenPercentage` down, and the SSD's accumulation strength.
Measure shimmer per CLAUDE.md §5 (consecutive-frame HF energy, n ≥ 16). The honest fix is a
denoiser trained on this input, i.e. the RR/NR track, so T6 is only worth running if RT is kept
on at all.

## Decision rule

The frame-rate case for `r.RayTracing=False` is strong (2.9×), and the artifacts are the
developers' unfinished tuning, not ours. Run T0–T3 only if there is a reason to keep RT on (RR
wanting its inputs, or a visual the baked path cannot give). If T3 removes #2 and T5's audit is
short, an "RT on, foliage and fixtures fixed" ini+Lua profile is a small mod. If not, leave RT
off and close this file.
