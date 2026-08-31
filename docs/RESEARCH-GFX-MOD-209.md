# "Stray Epic+ GFX Quality Optimized" (Nexus Mods Stray/209) — full cvar audit and DLSS-injection impact

Research report, 2026-08-31. Subject: a config-only Nexus mod for Stray —
`Stray Epic+ GFX Quality Optimized/Engine.ini`, ~196 `r.*`/`foliage.*`/`grass.*`/`a.*` cvars
under a single `[/Script/Engine.RendererSettings]` block, no DLL, no `.pak`, no shaders. It is
meant to be dropped into (or merged with) the user's runtime config file
`<compatdata>/.../Saved/Config/WindowsNoEditor/Engine.ini` (CLAUDE.md §2.2), i.e. exactly
where our own future `[SystemSettings]` recommendations (CLAUDE.md §4) also want to live.

The point of this report is narrow: **does this mod, run alongside our add-on, corrupt or
disable anything we depend on** — the TAA-pass identification (§2.3), the RR G-buffer
identification (`RESEARCH-RR-GBUFFER.md`), the SSD-denoiser candidate set, jitter, mip bias,
velocity — and if so, what override restores compatibility without giving up the mod's other
gains.

Provenance labels follow CLAUDE.md §0.5: **HARD** = read verbatim from a primary source (UE
4.27 engine source, the game's own shipped config, our own measured/committed code); **SOFT**
= a secondary source (community wiki, web-search synthesis) or the mod author's own inline
comment, cross-checked but not independently re-derived; **UNCONFIRMED** = inference that
needs an on-target measurement before anything depends on it.

UE citations are **UE 4.27.2** file:line, read from the same public mirror
`github.com/AlexMercer-MA/UnrealEngine-4.27` (branch `master`) that `RESEARCH-RR-GBUFFER.md`
already treats as HARD-via-mirror — confirmed again this session by re-deriving
`GetGBufferAFormat()` byte-for-byte at the same line numbers that document already cites
(`SceneRenderTargets.cpp:1026-1034` there; `:1018-1035` as fetched here — same function). Stray
is the licensee build `v1.54368 Revision 26632`; stock-source claims are marked accordingly.

Files read in full: the mod's `Engine.ini` (231 lines); `docs/game-config/*.ini` (Stray's own
shipped `DefaultEngine.ini`, `WindowsEngine.ini`, `Default/WindowsScalability.ini`); this
project's `CLAUDE.md`, `docs/RESEARCH.md` §4 and §4.11-4.12, `docs/RESEARCH-RR-GBUFFER.md`,
`src/core/gbuffer_classify.{hpp,cpp}`, `shaders/gbuffer_resolve.hlsl`, `src/core/taa_signature.hpp`.

---

## 0. Verdict up front

1. **The mod BREAKS DLSS Ray Reconstruction identification outright.** `r.GBufferFormat=3`
   changes GBufferA from `PF_A2B10G10R10` (our RGB10A2 anchor) to `PF_FloatRGBA` (RGBA16F).
   `src/core/gbuffer_classify.cpp`'s anchor check (`slot_tex[1]->format !=
   TexFormat::r10g10b10a2_unorm`) rejects the entire base-pass MRT set the instant this cvar is
   active — `gbuffer_resolve.hlsl` never dispatches. This is HARD, re-derived from UE 4.27
   source this session (§2.1).
2. **The normal ENCODING does not change, only the render-target's bit depth.** UE 4.27's
   `EncodeNormal()` (`DeferredShadingCommon.ush:79-82`) is `N*0.5+0.5` unconditionally on PC —
   the octahedral path is gated on `MOBILE_DEFERRED_SHADING` only, never on `r.GBufferFormat`.
   So the moment identification is fixed (§0.1's break), our existing plain-unit-vector decode
   math needs **zero changes** — this is a format-detection break, not a decode-math break
   (§2.1).
3. **`r.SceneColorFormat=4` is already safe.** `is_scene_colour_format()` in
   `gbuffer_classify.cpp` already accepts both `r11g11b10_float` (shipped) and
   `r16g16b16a16_float` (what value 4 produces) at slot 0. No code change needed (§2.2).
4. **The DLSS SR interception path (our current milestone) is NOT broken by this mod.** The TAA
   pass's DXBC hash is driven by `<PassConfig, Fast, ScreenPercentage, UpsampleFiltered,
   Downsample>` shader permutation bits, not by `r.TemporalAASamples` (a runtime CB value) or by
   any GBuffer/SceneColor cvar. The one permutation-affecting change,
   `r.TemporalAA.AllowDownsampling=0`, forces `TAA_DOWNSAMPLE` off — a **new** hash to learn,
   but the structural (register/format/dispatch-size) matcher in `taa_signature.hpp` still
   applies unchanged, and it actually **removes** our own "must always produce u1" correctness
   burden for the frames it governs (§2.3).
5. **The "denoiser bumps" mostly do NOT add new `FSSDTemporalAccumulationCS` dispatches.**
   Traced to source: `r.SSR.Quality=3` alone does not route SSR through the SSD family — that
   needs `r.SSR.ExperimentalDenoiser=1`, which the mod never sets (`ScreenSpaceRayTracing.cpp`
   / `IndirectLightRendering.cpp:31-34,771`). `r.Reflections.Denoiser` and the AO/Shadow
   equivalents gate **ray-traced** reflections/shadows/AO, which need
   `RayTracingReflectionOptions.bEnabled` / hardware DXR support neither confirmed nor denied
   for this title (Stray ships `r.RayTracing=True` as a capability flag, §2.4) — UNCONFIRMED
   whether that's actually active on this GPU/driver. `r.AmbientOcclusion.Denoiser` (bare, no
   suffix) could not be found as a real UE 4.27 cvar at all in the files searched — it may be a
   no-op. Net: **do not expect this mod to expand the known-9-hash SSD candidate set**, but flag
   it as something to re-measure if the denoiser-suppression experiment's counts change (§2.4).
6. **`r.MipMapLodBias=-1` stacks additively with UE4's own automatic TAAU mip bias
   (`View.MaterialTextureMipBias`, RESEARCH.md "Bonus 3"), not with anything DLSS itself
   applies — we set no mip bias of our own anywhere in `src/` or `shaders/`.** For the current
   1:1 (DLAA) milestone the automatic term is 0, so the mod's flat -1 is the *only* bias in
   effect; NVIDIA's own published DLSS mip-bias formula for a 1:1 ratio (`log2(1) - 1.0`)
   independently comes out to exactly -1, so this happens to land where NVIDIA's guide would
   put it (SOFT — a coincidence of the two independently-derived formulas, not a designed
   match). For the future SR milestone (§4, ~50% screen percentage) the two additive terms
   under-total NVIDIA's own recommendation by roughly a stop; not a correctness break, a
   sharpness/aliasing trade only visible in a screenshot (§2.5).
7. **Motion blur is disabled three times over** (`r.DefaultFeature.MotionBlur=0`,
   `r.MotionBlurQuality=0`, `r.MotionBlur.Amount=0`, `r.MotionBlurSeparable=0`). This removes
   one of the two passes CLAUDE.md's own §"0xd2e4d8c23c362ed1" note names as a freeze-confound
   for our suppression-based pass identification (`FMotionBlurFilterCS`) — strictly helpful for
   our own diagnostic methodology, orthogonal to DLSS image quality itself (§2.6).
8. **Nothing in the mod touches velocity encoding, jitter math, or the View CB layout.** The
   `r.Parallel*` family (including `r.ParallelVelocity=1`) is CPU-side command-list-recording
   parallelism only — no format, content, or shader-permutation change (§2.7).
9. **Bottom line: SAFE for the current SR/DLAA milestone as shipped; BREAKING for the RR
   milestone until either the mod's `r.GBufferFormat` is overridden back to 1, or the
   classifier is extended to accept `RGBA16F` at slot 1 (§3).** Exact override block in §3.

---

## 1. Full annotation, by subsystem

"Mod" = the value this Engine.ini sets. "Shipped" = Stray's own value from
`docs/game-config/*.ini` where the cvar appears there (HARD); otherwise "engine default" is
the stock UE 4.27 default (SOFT unless a citation is given — most of these ~196 cvars are
low-stakes scalar tuning knobs and are not independently re-derived from source; effect
descriptions lean on the mod author's own inline comments, cross-checked against general UE
4.27 documentation). Cvars re-derived from source this session are marked **[verified]**.

### 1.1 TAA / temporal upsampling / mip bias

| CVar | Mod | Shipped/default | Effect |
|---|---|---|---|
| `r.TemporalAA.Upscaler` | 1 | shipped: not set; **engine default 1** (RESEARCH.md §4.11) | No-op — restates the default. Chooses `GTemporalUpscaler` (pluggable) over the forced built-in. |
| `r.TemporalAA.AllowDownsampling` | 0 | engine default **1** | **Real change.** Forces the `TAA_DOWNSAMPLE` permutation bit off — see §2.3. |
| `r.TemporalAAFilterSize` | 1 | engine default **1.0** (SOFT, web-confirmed) | No-op. Governs history-fetch kernel width and (in non-upsampling mode only) the Box-Muller jitter sigma. |
| `r.TemporalAAUpsampleFitered` | 1 | shipped: not set (engine default is 1 — SOFT) | Likely no-op. Selects filtered vs. point history sampling under `AA_UPSAMPLE` — a real UE4 cvar name, the "Fitered" spelling is Epic's own typo, not the mod author's, per `RESEARCH.md`'s `FTAAUpsampleFilteredDim` permutation naming. |
| `r.TemporalAASamples` | 16 | engine default **8** | Runtime jitter-sequence length via the View CB, **not** a shader permutation — no DXBC hash change (§2.3). |
| `r.TemporalAA.Upsampling` | 1 | **shipped: already True** (`DefaultEngine.ini:69`) | No-op vs. the shipped game. Selects `ETAAPassConfig::MainUpsampling`; Stray already ships this on. |
| `r.MipMapLodBias` | -1 | shipped: not set (engine default **0**) | **Real change.** Global per-texture sampler mip bias — see §2.5, distinct from the streaming-pool bias below. |
| `r.Upscale.Panini.ScreenFit` | 1.0 | engine default | Panini-projection screen-fit for the *spatial* upscale pass only (WindowedFullscreen/secondary scaling); not on the TAAU path. |
| `r.Upscale.Quality` | 4 | **shipped: 2** (`WindowsEngine.ini`) | Real change (Lanczos-3 vs. Catmull-Rom), but this is the **secondary/windowed** upscale pass, not the primary `TemporalUpscale` path our hook sits on (`r.SecondaryScreenPercentage.GameViewport` defaults to 0 — RESEARCH.md §4.11). No interaction with our capture. |
| `r.Upscale.Softness` | 1 | engine default 1 | No-op; same pass as above. |
| `r.MaxAnisotropy` | 16 | engine default | Anisotropic texture filtering cap; independent of mip bias, orthogonal to DLSS. |

### 1.2 GBuffer / scene-colour format / material quality

| CVar | Mod | Shipped/default | Effect |
|---|---|---|---|
| `r.GBufferFormat` | 3 | **shipped: not set → engine default 1** | **BREAKING for RR.** GBufferA `PF_A2B10G10R10` → `PF_FloatRGBA`; GBufferB/C unaffected. See §2.1. **[verified]** |
| `r.SceneColorFormat` | 4 | **shipped: 3** (`WindowsEngine.ini:69`, matches CLAUDE.md §2.3.1) | `PF_FloatR11G11B10` → `PF_FloatRGBA`. Already accepted by our classifier at slot 0. See §2.2. **[verified]** |
| `r.MaterialQualityLevel` | 3 | **shipped: 1** (`WindowsEngine.ini`, commented `; High quality`) | Real change: **1=High → 3=Epic**. UE4's `EMaterialQualityLevel` enum order is famously non-alphabetical — `Low=0, High=1, Medium=2, Epic=3` — confirmed both by Stray's own inline comment on value 1 and by community reports of this exact ordering quirk (SOFT — Epic acknowledged the mislabeling on the forums but never renumbered it for back-compat). The mod author's own `"3 for epic* unknown?"` uncertainty is in fact correct. No interaction with our capture — this recompiles *material* shader permutations (BRDF/feature complexity), not the base-pass structural layout or the TAA shader. |
| `r.NormalMapsForStaticLighting` | 1 | **shipped: False** (`DefaultEngine.ini`) | Real change; adds a normal map term to static (baked) lighting only. No G-buffer layout or format change. |

### 1.3 Motion blur / depth of field

| CVar | Mod | Shipped/default | Effect |
|---|---|---|---|
| `r.DefaultFeature.MotionBlur` | 0 | **shipped: True** (`DefaultEngine.ini`) | Disables the project-default motion-blur feature. |
| `r.MotionBlurQuality` | 0 | **shipped: 3** (`WindowsEngine.ini`) | Real change: motion blur fully off. |
| `r.MotionBlur.Max` | 0 | engine default | Clamps max blur radius to 0 (redundant with Quality=0). |
| `r.MotionBlur.Amount` | 0 | **shipped: 0.5** (`DefaultEngine.ini`) | Zeroes the per-pixel blur amount — third independent way this mod disables motion blur. |
| `r.MotionBlurSeparable` | 0 | engine default | Moot once MB is off. |
| `r.MeshParticle.MinDetailModeForMotionBlur` | 0 | engine default | Moot once MB is off; would otherwise gate particle MB by detail mode. |
| `r.DepthOfFieldQuality` | 0 | **shipped: 2** (`WindowsEngine.ini`) | DOF fully disabled (shipped runs adaptive CircleDOF quality 2). |
| `r.DepthOfField.MaxSize`, `.NearBlurSizeThreshold`, `.FarBlur` | 0 | engine defaults | Moot once DOF is off; would otherwise clamp blur radius/near-threshold/far-blur. |
| `r.DOF.Scatter.NeighborCompareMaxColor` | 0 | engine default 10 | Moot once DOF is off. |
| `r.FastBlurThreshold` | 0 | **shipped: 3** (`WindowsEngine.ini`) | Disables the Gaussian-blur radius optimisation (affects DOF/bloom blur passes only, not TAA). |
| `r.DisableDistortion` | 1 | engine default 0 | Disables refractive distortion materials (heat haze, glass) — a base-pass/translucency material feature, not a post pass we touch. |

**Net effect for us:** removes `FMotionBlurFilterCS` from the display chain entirely — see §2.6.
DOF passes run downstream of TAA either way and never touch our capture registers.

### 1.4 Tonemapper / bloom / colour

| CVar | Mod | Shipped/default | Effect |
|---|---|---|---|
| `r.Tonemapper.Quality` | 5 | **shipped: 5** (`WindowsEngine.ini`) | No-op. Full tonemapper feature set (grain jitter etc.) |
| `r.Tonemapper.GrainQuantization` | 1 | **shipped: 1** | No-op. |
| `r.TonemapperFilm` | 1 | engine default | Forces the alternative film tonemapper curve. Downstream of our TAA hook — no interaction. |
| `r.SceneColorFringe.Max`, `r.SceneColorFringeQuality` | 0, 0 | shipped: not set / **1** | Disables chromatic aberration and its quality tier — post-tonemap, no interaction. |
| `r.BloomQuality` | 5 | **shipped: 5** | No-op. |
| `r.Filter.SizeScale` | 1.6 | **shipped: 0.8** | Real change — doubles bloom/DOF blur kernel footprint. Downstream, no interaction. |
| `r.UseHighQualityBloom`, `r.Bloom.Cross` | 1, 2 | engine defaults | Bloom convolution quality/cross-pattern; downstream. |
| `r.EmitterSpawnRateScale` | 2 | **shipped: 0.5** | Particle spawn density; unrelated subsystem. |
| `r.ParticleLightQuality` | 2 | **shipped: 1** | Particle-light shading quality; unrelated subsystem. |

### 1.5 Subsurface scattering / hair

| CVar | Mod | Shipped/default | Effect |
|---|---|---|---|
| `r.SubsurfaceScattering` | 1 | engine default | On (default). |
| `r.SSS.Quality` | 1 | **shipped: 1** (`WindowsEngine.ini`) | No-op. |
| `r.SSS.SampleSet` | 2 | **shipped: 1** | Real change: 13-tap vs 9-tap SSS kernel. |
| `r.SSS.Checkerboard` | 0 | engine default | Full-res (non-checkerboard) SSS lighting. |
| `r.SSS.HalfRes` | 0 | **shipped: 1** | Real change: SSS recombine runs full-res instead of half-res. |
| `r.SSS.Scale`, `r.SSS.Filter` | 1, 1 | shipped: 1 / not set | No-op / default. |
| `r.HairStrands.DeepShadow.SuperSampling` | 1 | engine default | Hair shadow supersampling; Stray uses `GFurPRO`, a third-party fur plugin (visible in `[Core.System] Paths`), so this cvar's actual reach is plugin-dependent — UNCONFIRMED whether Stray's fur uses UE4's native hair-strands pipeline at all. |

None of this subsystem touches the G-buffer MRT layout, scene colour format, or the TAA shader — it changes *material shading model* branches evaluated inside the base pass, compiled independently of `r.GBufferFormat`/`r.SceneColorFormat`.

### 1.6 Reflections / SSR / material quality

| CVar | Mod | Shipped/default | Effect |
|---|---|---|---|
| `r.DoTiledReflections` | 1 | engine default | Tiled deferred reflection-capture application (perf path, not a format/content change). |
| `r.SSR.Quality` | 3 | shipped: not found in `docs/game-config/*.ini` (SSR cvars absent → **engine default 0, off**) | **Real change: turns SSR on** (glossy, few samples). See §2.4 for the denoiser-routing consequence. |
| `r.SSR.HalfResSceneColor` | 0 | **shipped: 1** | Real change: SSR samples full-res scene colour (sharper, pricier). |
| `r.ReflectionEnvironment` | 2 | engine default | Reflection-capture environment mode. |
| `r.RefractionQuality` | 3 | **shipped: 2** | Real change, translucency-material feature. |
| `r.ReflectionCaptureGPUArrayCopy` | 1 | engine default | Perf path for building the capture cubemap array. |
| `r.ReflectionEnvironmentBeginMixingRoughness/EndMixingRoughness` | 0.0 / 1.0 | engine defaults | Roughness range over which capture reflections blend with SSR/planar. |
| `r.ReflectionEnvironmentLightmapMixBasedOnRoughness` | 1 | engine default | Lightmap/reflection-capture brightness matching. |
| `r.Reflections.Denoiser` | 2 | engine default **2** | No-op numerically, but see §2.4 — gates ray-traced reflections denoising specifically, not SSR. |
| `r.Reflections.Denoiser.ReconstructionSamples` | 16 | engine default 8 | Doubles reconstruction sample count *if* the RT reflections denoiser path is ever entered (§2.4) — a runtime loop-count parameter, not a permutation. |
| `r.ReflectionCaptureResolution` | 512 | engine default 128 | Reflection-capture cubemap resolution (a project setting, baked at capture time, not runtime-hooked). |
| `r.ReflectionEnvironmentCubemapPoolFixedNum` | 256 | engine default | Capture pool sizing. |
| `r.Roughness.Max`, `r.Roughness.Min` | 2, 0 | engine default 1 / 0 | The mod's own comment reads `"Only for non shipping built!"`. Stray runs `Stray-Win64-Shipping.exe` — these are `ECVF_Cheat`-style debug material-roughness remap cvars in stock UE4 and are conventionally stripped/no-op in Shipping builds (SOFT — not independently re-verified in source this session, but this is widely reported UE4 modder knowledge and the author's own comment agrees). If true, **likely a complete no-op in the live game.** |
| `r.SSR.MaxRoughness` | 2 | engine default | SSR roughness cutoff — if `r.Roughness.Max` is genuinely cheat-only, this remains the effective cap. |
| `r.FastVRam.SSR` | 1 | engine default | Console/ESRAM allocation hint, irrelevant on PC/vkd3d. |
| `r.NormalDeformer` | 1 | not a stock UE 4.27 renderer cvar found in the mirror's `Private/` tree — likely a `GFurPRO`-plugin cvar (plugin is present per `[Core.System] Paths`). UNCONFIRMED, out of scope. |

**Consequence for our capture (§2.4 has the full derivation):** turning SSR on (`r.SSR.Quality:
0→3`) adds SSR-reflected light into scene colour **before** our TAA hook sees it — expected,
harmless, and exactly the kind of lit content DLSS SR/RR are meant to reconstruct. It does
**not**, by itself, add new dispatches to the `FSSDTemporalAccumulationCS` family our
denoiser-suppression experiment already characterised, because `r.SSR.ExperimentalDenoiser`
(the cvar that actually redirects SSR through the SSD) stays at its default 0 in this file.

### 1.7 Light propagation volumes / light shafts (legacy dynamic GI)

| CVar | Mod | Shipped/default | Effect |
|---|---|---|---|
| `r.LPV.*` (9 cvars) | various | engine defaults | Light Propagation Volumes — a legacy, largely-unused-since-4.x dynamic-GI feature. Tuning only takes effect if a level actually has an LPV volume placed; UNCONFIRMED whether Stray uses LPV at all (no evidence either way in the files read). No interaction with our hook regardless — LPV injects into the deferred lighting pass, upstream of TAA, same as any other lighting contributor. |
| `r.LightShaftNumSamples`, `r.LightShaftBlurPasses` | 12, 2 | engine defaults | God-ray sample count/blur passes; separate post pass, no capture interaction. |

### 1.8 Ambient occlusion

| CVar | Mod | Shipped/default | Effect |
|---|---|---|---|
| `r.AmbientOcclusionMaxQuality` | 99 | **shipped: 1** (`WindowsEngine.ini`) | Real, large change (the cvar is a 0-100 quality-percentage cutoff, not a boolean). |
| `r.AOAsyncBuildQueue` | 1 | engine default | Async distance-field build queue (perf, no content change). |
| `r.AmbientOcclusionLevels` | 2 | **shipped: 1** | Real change: two AO mip levels instead of one. |
| `r.AmbientOcclusion.Denoiser` | 1 | **could not locate as a real top-level UE 4.27 cvar** — see §2.4 | Likely a no-op in this engine version; classic screen-space AO does not route through `IScreenSpaceDenoiser`. |
| `r.AmbientOcclusion.Denoiser.HistoryConvolution.SampleCount`, `.ReconstructionSamples` | 16, 16 | engine defaults 16 / 16 | No-op numerically even where the parent subsystem is reachable. |
| `r.AOQuality` | 2 | **shipped: 1** | Real change (a coarser scalability-style AO quality tier, distinct from `r.AmbientOcclusionLevels`). |
| `r.AmbientOcclusion.AsyncComputeBudget` | 3 | engine default 1 | Async-compute scheduling weight; perf only. |
| `r.AOGlobalDFResolution`, `.StartDistance`, `r.AOMaxViewDistance`, `.MaxObjectBoundingRadius`, `.MaxObjectsPerCullTile`, `r.AOSpecularOcclusionMode`, `.StepExponentScale`, `.ViewFadeDistanceScale`, `r.AOHeightfieldOcclusion`, `r.AOGlobalDistanceField*` (7 cvars), `r.AOSampleSet`, `r.AOScatterTileCulling` | various | engine defaults | **All Distance-Field AO (DFAO) tuning.** Stray's own scalability config forces `r.DistanceFieldAO=0` in **every** `[ShadowQuality@N]` bucket, 0 through Cine (`docs/game-config/Hk_project_Config_DefaultScalability.ini`, HARD), and the mod never sets `r.DistanceFieldAO=1` to re-enable it. Since `[SystemSettings]`/`ECVF_SetBySystemSettingsIni` outranks the scalability-group priority in UE4's cvar precedence, an explicit `r.DistanceFieldAO=1` *would* override this — the mod does not supply one. **So most of this block is very likely inert in the live game** (UNCONFIRMED without a live capture — the global distance field also feeds landscape/heightfield shadowing independent of DFAO, so "inert" is not absolute). |

### 1.9 Shadows

| CVar | Mod | Shipped/default | Effect |
|---|---|---|---|
| `r.ShadowQuality` | 5 | **shipped: 5** (`WindowsEngine.ini`) | No-op. |
| `r.MaxCSMRadiusToAllowPerObjectShadows` | 8000 | engine default | Distance below which per-object shadows are preferred over CSM. |
| `r.Shadow.DistanceScale` | 1.2 | **shipped: 1** | Real change: shadow draw-distance multiplier. |
| `r.Shadow.MaxNumFarShadowCascades` | 3 | engine default | Far-cascade count cap. |
| `r.Shadow.UnbuiltNumWholeSceneDynamicShadowCascades` | 4 | engine default | Editor/unbuilt-lighting preview only. |
| `r.Shadow.CSM.MaxCascades` | 12 | **shipped: 4** | Real, large change: up to 12 cascades vs. 4. |
| `r.Shadow.CSMReceiverBias` | 1 | engine default | Receiver-side bias to fight acne. |
| `r.Shadow.CSM.TransitionScale` | 1.0 | **shipped: 0.8** | Real change: wider cascade-transition blend band. |
| `r.Shadow.RadiusThreshold` | 0.03 | **shipped: 0.04** | Minor real change. |
| `r.Shadow.MaxCSMResolution` | 2048 | engine default | CSM texel budget cap. |
| `r.Shadow.MaxResolution` | 4096 | engine default | Per-light shadow-map resolution cap. |
| `r.Shadow.MaxSoftKernelSize` | 60 | engine default | PCSS-style soft-shadow kernel cap. |
| `r.Shadow.TexelsPerPixel`, `.TexelsPerPixelPointlight`, `.TexelsPerPixelSpotlight` | 1.8/1.8/2.8 | engine defaults | Shadow-map texel density per light type. |
| `r.Shadow.ShadowMapResolutionForAtlasedLightShadows` | 2048 | engine default | Atlas slot resolution for local-light shadows. |
| `r.Shadow.PerObject` | 1 | engine default | Enables per-object (character-on-world) shadows. |
| `r.Shadow.PointLightDepthBias`, `.RectLightDepthBias` | 0.03, 0.03 | engine defaults | Depth-pass bias to avoid peter-panning vs. acne. |
| `r.Shadow.SpotLightTransitionScale` | 512 | engine default | Distance falloff for spot-shadow softness. |
| `r.Shadow.Denoiser` | 1 | **engine default 2** (`CVarShadowUseDenoiser`, `LightRendering.cpp:75-80`) **[verified]** | See §2.4 — the shipped source's own help text says `"0: Disabled (default)"` while the literal default argument is `2`; a genuine Epic-source inconsistency, not this mod's error. Mode 1 forces the built-in denoiser directly rather than through the (usually identical, absent a third-party override) `GScreenSpaceDenoiser` indirection — functionally a wash in Stray. Only matters at all if ray-traced shadows are actually active (UNCONFIRMED). |
| `r.Shadow.Denoiser.ReconstructionSamples` | 16 | engine default 8 | Runtime sample-count parameter, same caveat. |
| `r.Shadow.UnbuiltWholeSceneDynamicShadowRadius` | 200000 | engine default | Editor/unbuilt preview only. |
| `r.Shadow.WholeSceneShadowCacheMb` | 512 | engine default | Cached-shadow memory budget. |
| `r.DFShadowQuality` | 3 | engine default | Distance-field shadow quality tier — gated the same way as §1.8's DFAO block; `r.DistanceFieldShadowing` is separately **1** in Stray's shipped `WindowsEngine.ini`, so unlike DFAO this one is plausibly live. |
| `r.CapsuleShadows` | 1 | **shipped: 1** (`WindowsEngine.ini`) | No-op. |
| `r.AllowLandscapeShadows` | 1 | engine default | Enables landscape as a shadow caster/receiver. |
| `r.Shadow.CacheWholeSceneShadows` | 1 | engine default | Caches static-primitive shadow depths for movable point/spot lights. |

None of §1.9 touches the TAA dispatch's own bindings (`t2`/`t4` depth+stencil, §2.3) — shadow-map
rendering and application both happen well upstream, in the shadow-depth and lighting passes.

### 1.10 Volumetric fog / clouds / sky atmosphere

| CVar | Mod | Shipped/default | Effect |
|---|---|---|---|
| `r.VolumetricCloud`, `.SkyAO`, `.ShadowMap`, `.ShadowMap.MaxResolution`, `.SkyAO.Filtering`, `.SkyAO.MaxResolution`, `.SampleMinCount`, `.ViewRaySampleMaxCount`, `.DistanceToSampleMaxCount` (9 cvars) | various | engine defaults | Volumetric cloud rendering/shadowing/AO quality. UNCONFIRMED whether Stray's levels contain a `SkyAtmosphere`/cloud actor at all (a largely outdoor-sky feature; Stray is set in an indoor/urban environment — plausibly inert in most levels). |
| `r.VolumetricFog.GridPixelSize`, `.Jitter`, `.InjectShadowedLightsSeparately`, `.InverseSquaredLightDistanceBiasScale`, `.LightFunctionSupersampleScale` | various | engine defaults; **shipped sets `r.VolumetricFog=1`** (`WindowsEngine.ini`, unchanged by the mod) | Volumetric-fog froxel-grid quality tuning. Independent lighting-pass feature, no interaction with our capture. |
| `r.Fog`, `r.FogDensity` | 1, 0.8 | engine defaults | Height-fog toggle/density override. |
| `r.SkyAtmosphere*` (7 cvars), `r.SkySpecularOcclusionStrength`, `r.SkylightIntensityMultiplier` | various | engine defaults | Sky/atmosphere LUT quality and skylight intensity. |
| `r.LUT.Size` | 32 | **engine default 32** | No-op. Note: this is the **colour-grading 3D LUT** (the tonemapper's volume texture — the one `taa_signature.hpp`'s `is_3d` dataflow anchor identifies, per its own header comment), *not* a sky-atmosphere LUT despite sitting next to those lines in the file. Unchanged either way, and even if it were changed, our anchor only tests "is a 3D texture", not its resolution. |

### 1.11 Translucency

| CVar | Mod | Shipped/default | Effect |
|---|---|---|---|
| `r.SeparateTranslucencyScreenPercentage` | 90 | engine default | Renders separate (post-DOF-composited) translucency at 90% of full res. |
| `r.SeparateTranslucency` | 1 | **shipped: True** | No-op. |
| `r.TranslucencyVolumeBlur` | 0 | **shipped: 1** | Real change: disables the translucent-lighting-volume blur pass. |
| `r.TranslucentLightingVolume` | 1 | engine default | Enables the volume-based translucent lighting path. |

Translucency composites after DOF, which is itself after TAA — no interaction with our capture
or with DLSS's colour input (translucent content historically is *excluded* from what DLSS sees
anyway, per NVIDIA's own guidance that translucency should be handled separately).

### 1.12 Streaming / shader compilation / CPU parallelism

All engine defaults or perf-only toggles; none change render-target formats, shader
permutations, or per-frame content. Listed for completeness, one line each:

| CVar | Mod | Note |
|---|---|---|
| `r.ForceAllCoresForShaderCompiling` | 1 | Shader-compile-worker count; build-time-adjacent, irrelevant at runtime. |
| `r.UseShaderPredraw`, `r.UseAsyncShaderPrecompilation` | 1, 1 | Predraw/async shader precompilation to avoid hitches — interacts with the **PSO-cache-drop** hazard CLAUDE.md §5 already documents (registering pipeline events makes ReShade bypass the cached-PSO blob); this mod does not make that worse or better, it is orthogonal. |
| `r.AsyncCreateLightPrimitiveInteractions` | 1 | CPU-side light/primitive interaction building. |
| `r.Streaming.UseBackgroundThreadPool`, `.PoolSize`, `.MaxTempMemoryAllowed`, `.Boost`, `.AmortizeCPUToGPUCopy`, `.MaxNumTexturesToStreamPerFrame`, `.MaxReferenceChecksBeforeStreamOut`, `.FramesForFullUpdate` | various | Texture-streaming pool sizing/pacing. |
| `r.Streaming.MipBias` | 0 | **shipped: 0** (`WindowsEngine.ini`) — no-op, and a **different mechanism from `r.MipMapLodBias`**: this one biases which mip is *streamed resident*, not which mip the sampler *selects* at render time. See §2.5. |
| `r.ShaderPipelines`, `r.Shaders.FastMath`, `r.CreateShadersOnLoad`, `r.ShaderComplexity.CacheShaders`, `r.ShaderPipelineCache.PreOptimizeEnabled`, `r.UseShaderCaching`, `r.CompileShadersForDevelopment` | 1 (all) | Shader-compilation/caching pipeline toggles. |
| `r.MultithreadedShadowmapEncode`, `r.MultithreadedLightmapEncode` | 1, 1 | Build-time lightmap/shadowmap encoding threading — no runtime effect in a shipped game (baked lighting is already built). |
| `r.MeshDrawCommands.AllowOnDemandShaderCreation`, `.DynamicInstancing` | 1, 1 | Draw-command recording/instancing strategy. |
| `r.ParallelBasePass`, `.ParallelGatherShadowPrimitives`, `.ParallelInitViews`, `.ParallelPrePass`, `.ParallelShadows`, `.ParallelTranslucency`, `.ParallelVelocity` | 1 (all) | CPU command-list-recording parallelism per pass. See §2.7 — no content/format change, ever. |

### 1.13 Misc / foliage / animation

| CVar | Mod | Shipped/default | Effect |
|---|---|---|---|
| `foliage.DensityScale` | 1.2 | **shipped: 1.0** | Real change: 20% more foliage instances. |
| `foliage.LODDistanceScale` | 1 | engine default | Foliage LOD transition distance multiplier. |
| `foliage.MinLOD` | 1 | engine default | Forces a minimum foliage LOD (never renders LOD0 close-up detail) — actually a *quality reduction* despite the mod's framing, worth flagging to the user. |
| `grass.DensityScale` | 1.2 | **shipped: 1.0** | Real change, same as foliage. |
| `a.URO.Enable` | 0 | engine default 1 | Disables Animation Update Rate Optimization — all skeletal meshes animate at full rate regardless of screen size/distance (quality up, CPU cost up). No interaction with any GPU pass we touch. |
| `r.ViewDistanceScale` | 1 | shipped: scalability-group-driven (`[ViewDistanceQuality@N]`, `DefaultScalability.ini`) — mod's flat `1` sits inside that range | Draw-distance multiplier; independent of our capture. |

---

## 2. DLSS-interaction analysis

### 2.1 `r.GBufferFormat=3` — breaks RR identification; decode math is unaffected

**The mechanism, from UE 4.27 source (`SceneRenderTargets.cpp`, this session's fetch):**

```cpp
static TAutoConsoleVariable<int32> CVarGBufferFormat(
    TEXT("r.GBufferFormat"),
    1,
    TEXT("Defines the memory layout used for the GBuffer.\n")
    TEXT("(affects performance, mostly through bandwidth, quality of normals and material attributes).\n")
    TEXT(" 0: lower precision (8bit per component, for profiling)\n")
    TEXT(" 1: low precision (default)\n")
    TEXT(" 3: high precision normals encoding\n")
    TEXT(" 5: high precision"),
    ECVF_RenderThreadSafe);                                            // :155-163

EPixelFormat FSceneRenderTargets::GetGBufferAFormat() const
{
    const bool bHighPrecisionGBuffers = (CurrentGBufferFormat >= EGBufferFormat::Force16BitsPerChannel); // >= 5
    const bool bEnforce8BitPerChannel = (CurrentGBufferFormat == EGBufferFormat::Force8BitsPerChannel);  // == 0

    EPixelFormat NormalGBufferFormat = bHighPrecisionGBuffers ? PF_FloatRGBA : PF_A2B10G10R10;

    if (bEnforce8BitPerChannel)
        NormalGBufferFormat = PF_B8G8R8A8;
    else if (CurrentGBufferFormat == EGBufferFormat::HighPrecisionNormals)   // == 3
        NormalGBufferFormat = PF_FloatRGBA;

    return NormalGBufferFormat;
}                                                                        // :1018-1035

// GetGBufferBFormat() / GetGBufferCFormat(): identical to each other, gated ONLY on
// bHighPrecisionGBuffers (>= 5) — GBufferFormat == 3 does NOT set this flag.
```

`EGBufferFormat` enum values (`EngineTypes.h`, SOFT — confirmed by web search cross-referencing
Epic's own docs page for `EGBufferFormat::Type`, not independently fetched this session):
`Force8BitsPerChannel=0, Default=1, HighPrecisionNormals=3, Force16BitsPerChannel=5`.

**At the mod's `r.GBufferFormat=3` (HighPrecisionNormals):**

| Target | Shipped (`GBufferFormat=1`) | Mod (`GBufferFormat=3`) |
|---|---|---|
| GBufferA (slot 1) | `PF_A2B10G10R10` = RGB10A2 | **`PF_FloatRGBA` = RGBA16F** |
| GBufferB (slot 2) | `PF_B8G8R8A8` | `PF_B8G8R8A8` — **unaffected** |
| GBufferC (slot 3) | `PF_B8G8R8A8` | `PF_B8G8R8A8` — **unaffected** |

Only GBufferA changes — exactly what the cvar's own help text promises ("high precision
**normals** encoding"), not a blanket 16-bit upgrade (that's `GBufferFormat=5`, which this mod
does not use, and which *would* also break the GBufferB/C 8-bit-RGBA classification at slots 2/3).

**Why identification breaks, decode does not.** `gbuffer_classify.cpp`'s anchor check runs
before any role is assigned:

```cpp
// THE anchor: GBufferA's RGB10A2 at slot 1 — the only RGB10A2 render target in a
// stock deferred frame (§1.5). An off-format slot 1 is the licensee-delta tripwire
// and must fail loudly, not classify quietly.
if (slot_tex[1]->format != TexFormat::r10g10b10a2_unorm)
{
    r.reason = "slot 1 is not RGB10A2 (the GBufferA anchor): not the stock G-buffer "
               "layout, or a licensee format delta - re-derive before trusting";
    return r;
}
```

With `GBufferFormat=3` this fires on every base-pass MRT set, `r.is_base_pass` never becomes
`true`, and `shaders/gbuffer_resolve.hlsl` — which depends on that verdict to know it has a
real base-pass set to read — never runs. **The RR guide-buffer pipeline goes dark, silently
(no crash, no log-visible error beyond whatever the classifier's own `reason` string reports)**
under CLAUDE.md's own Prime Directive #2 ("prefer a loud failure"), this is exactly the failure
mode to avoid shipping unflagged.

**The encode math itself is untouched.** `DeferredShadingCommon.ush:79-82`:

```hlsl
float3 EncodeNormal( float3 N )
{
    return N * 0.5 + 0.5;
    //return Pack1212To888( UnitVectorToOctahedron( N ) * 0.5 + 0.5 );
```

and `EncodeGBuffer` (`:356-360`) only switches to the commented-out octahedral path under
`#if MOBILE_DEFERRED_SHADING` — never on `r.GBufferFormat`, on any platform, in this engine
version. So `gbuffer_resolve.hlsl`'s `decode_gbuffer_a_normal` (`a.xyz * 2.0f - 1.0f`,
renormalised) stays byte-for-byte correct once a `GBufferFormat=3` frame is actually fed to it
— storing `N*0.5+0.5` in a 16-bit float channel instead of a 10-bit UNORM channel changes only
quantisation noise, not the pack/unpack formula. This also means **no DXBC hash changes**:
`GetGBufferAFormat()` only controls render-target *allocation*, never a compile-time shader
`#define`, so the base-pass pixel shader and every SSD/TAA hash our project already knows are
completely unaffected by this cvar.

**What would be needed to also support it.** Extend `is_base_pass`'s slot-1 anchor to accept
`r16g16b16a16_float` as an alternate, the same way slots 2/3 already tolerate ambiguity and are
resolved by slot position alone (`bc_order_by_slot_only`). The risk this introduces: GBufferA
(slot 1, RGBA16F) becomes format-identical to scene colour (slot 0, RGBA16F under either the
mod's `SceneColorFormat=4` or the engine's own default 4) and to our own TAA history/output —
but since the anchor is checked at a **specific slot**, not "any RGBA16F target anywhere", slot
position alone still disambiguates it, at the same confidence level the classifier already
accepts for B/C. This is a small, well-precedented change — not made here per the task's "do
not modify `src/`" instruction, but the shape of the fix is now known.

### 2.2 `r.SceneColorFormat=4` — already safe

Confirmed via the same UE mirror, `GetSceneColorFormat()`:

```cpp
switch(CurrentSceneColorFormat)
{
    case 0: SceneColorBufferFormat = PF_R8G8B8A8; break;
    case 1: SceneColorBufferFormat = PF_A2B10G10R10; break;
    case 2: SceneColorBufferFormat = PF_FloatR11G11B10; break;
    case 3: SceneColorBufferFormat = PF_FloatRGB; break;
    case 4:
        // default
        break;                          // leaves the pre-switch initialiser, PF_FloatRGBA
    case 5: SceneColorBufferFormat = PF_A32B32G32R32F; break;
}
```

Value 4 falls through to whatever `SceneColorBufferFormat` was initialised to before the
`switch` — `PF_FloatRGBA`, per the cvar's own help text (`" 4: PF_FloatRGBA 64Bit
(default,..."`, quoted verbatim in the mod's own inline comment) and general UE4.19-5.x
documentation consistency (SOFT on the initialiser value specifically, HARD on the case-4
control flow as fetched). Shipped Stray runs `r.SceneColorFormat=3` = `PF_FloatRGB`
(`R11G11B10F`, matches CLAUDE.md §2.3.1 exactly).

`gbuffer_classify.cpp`'s `is_scene_colour_format()` already accepts both:

```cpp
bool is_scene_colour_format(TexFormat f)
{
    return f == TexFormat::r11g11b10_float || f == TexFormat::r16g16b16a16_float;
}
```

No code change needed. **This is the one GBuffer-adjacent cvar the mod changes that our
classifier was already built to tolerate** — worth noting as the reason the header comment
in `gbuffer_classify.hpp` explicitly says "a config change does not silently break
identification": that design intent already covers this exact mod setting.

### 2.3 TAA/TAAU cluster — permutation, jitter, and the u1 requirement

**Permutation domain** (RESEARCH.md §4.4, HARD): `<FTAAPassConfigDim, FTAAFastDim,
FTAAScreenPercentageDim, FTAAUpsampleFilteredDim, FTAADownsampleDim>`. Of the mod's six
TAA-adjacent cvars:

* `r.TemporalAA.Upsampling=1` — already shipped True. No permutation change from baseline.
* `r.TemporalAA.Upscaler=1` — selects the pluggable-upscaler *indirection*, not a shader
  permutation dimension at all; already the engine default.
* `r.TemporalAAUpsampleFitered=1` — maps to `FTAAUpsampleFilteredDim`, but is a real UE4 cvar
  (Epic's own typo, reproduced correctly by the mod, not the mod's error) whose default is
  already 1 (SOFT) — likely no permutation change from baseline either.
* `r.TemporalAASamples=16` — **not** a permutation dimension. It sets
  `View.TemporalJitterSequenceLength`, read back at runtime from the View CB's
  `TemporalAAParams` row (§2.6/§2.7 of CLAUDE.md) — a pure data value, zero effect on the
  compiled DXBC or its hash. Raising it from 8→16 sits below the documented per-quality-mode
  minimum for anything past DLAA (RESEARCH.md §3.2: DLAA 8, Quality 18, Balanced 24,
  Performance 32) — not a break, but if the SR milestone ever targets a Performance-like ratio
  (Stray's own shipped 50% screen percentage is close to that band), 16 undershoots NVIDIA's
  documented minimum of 32 and may show more visible jitter-pattern noise before temporal
  convergence than a properly-tuned SR path would. Worth a note for whoever tunes the SR
  milestone, not a compatibility issue.
* **`r.TemporalAA.AllowDownsampling=0`** — the one real permutation change. Forces
  `TAA_DOWNSAMPLE` off unconditionally, which (per RESEARCH.md §4.4) otherwise requires
  `bDownsample → bUseFast → GetPostProcessAAQuality() == Medium`; Stray ships
  `r.PostProcessAAQuality=3` (Medium) unchanged by this mod, so without the override the
  downsample bit would normally be set. **This produces a new DXBC hash** — neither
  `0x901e041a7cadc9db` nor `0x1708ec956099e259`/`0xd2e4d8c23c362ed1` (the known
  `AA_UPSAMPLE`-permutation hashes) will necessarily match, since those were captured with
  downsampling in its default state. The structural matcher in `match_taa_dispatch()` does not
  care — it keys on the depth+stencil-over-one-resource signature and output-rect dispatch
  size, not solely the hash, exactly as CLAUDE.md §2.3 already mandates ("never gate on it
  alone"). **A positive side effect**: without `TAA_DOWNSAMPLE`, the shader never declares
  `u1 = OutComputeTexDownsampled`, so our own "we must always produce u1 or downstream
  bloom/DOF reads garbage" correctness burden (RESEARCH.md §4.4, CLAUDE.md's "produce u1"
  step) becomes unnecessary for any frame this mod governs — one less thing that can go wrong,
  provided our own u1-production logic correctly detects "no u1 was bound this frame" rather
  than assuming it is always required.
* **`r.MipMapLodBias=-1`** is adjacent to this cluster but is a *sampler*-level bias, not a
  shader-permutation dimension — covered separately in §2.5, since it interacts with DLSS's own
  guidance rather than with our identification logic.

**Jitter distribution**: CLAUDE.md §2.7 and RESEARCH.md §4.12 agree that the Box-Muller-warped
jitter (`sigma = 0.47 * r.TemporalAAFilterSize`) only applies in **non**-upsampling mode. Both
the shipped game and this mod run `r.TemporalAA.Upsampling=1`, so the plain-Halton-in-[-0.5,0.5]
sequence DLSS was trained on is already in effect regardless of the mod, and
`r.TemporalAAFilterSize=1` (a no-op vs. the engine default anyway, §1.1) has no bearing on
which sequence is used. It may still affect the **spatial reconstruction filter's kernel
radius** during history resampling — a separate use of the same cvar name from the
jitter-generation-time use — but that is a reconstruction-quality knob, not a jitter-statistics
one, and both configurations already sit in the mode DLSS expects.

**Net for the DLSS-SR interception**: **safe.** One new hash to learn (structural match still
works without it), no jitter-distribution regression, and one fewer correctness obligation.

### 2.4 The denoiser bumps — do NOT expand the known SSD candidate set

Traced directly in `ScreenSpaceRayTracing.cpp` / `IndirectLightRendering.cpp` /
`LightRendering.cpp` on the same UE 4.27 mirror this session:

**SSR.** `r.SSR.Quality:0→3` (real change, §1.6) turns screen-space reflections on. Whether SSR
denoises through the SSD `FSSDTemporalAccumulationCS` family is decided by a **separate** cvar
the mod never touches:

```cpp
static TAutoConsoleVariable<int32> CVarDenoiseSSR(
    TEXT("r.SSR.ExperimentalDenoiser"), 0,
    TEXT("Replace SSR's TAA pass with denoiser."),
    ECVF_RenderThreadSafe);                                    // IndirectLightRendering.cpp:31-34

// ...
bDenoise = DenoiserMode != 0 && CVarDenoiseSSR.GetValueOnRenderThread();   // :771
bTemporalFilter = !bDenoise && View.ViewState && IsSSRTemporalPassRequired(View);
```

With `r.SSR.ExperimentalDenoiser` at its default 0, `bDenoise` is always false for SSR
regardless of `r.Reflections.Denoiser`'s value, and SSR instead uses **its own dedicated
temporal reprojection pass** (`bTemporalFilter`, a distinct shader from the SSD family). So
turning SSR on via this mod adds a *new*, separate temporal shader to the frame — not one of
our known 9 `FSSDTemporalAccumulationCS` hashes, and not currently a candidate our
denoiser-suppression experiment accounts for at all. **This is worth flagging as a genuinely
new unknown, just not the one the cvar name (`r.Reflections.Denoiser`) suggests.**

**Ray-traced reflections/shadows.** `r.Reflections.Denoiser` and `r.Shadow.Denoiser` (both real
`FSSDTemporalAccumulationCS`-family gates, unlike the phantom `r.AmbientOcclusion.Denoiser`
below) only matter when the ray-traced path is actually taken:

```cpp
if (RayTracingReflectionOptions.bEnabled || bScreenSpaceReflections) { ... }
if (RayTracingReflectionOptions.bEnabled) { bDenoise = DenoiserMode != 0; RenderRayTracingReflections(...); }
```

`docs/game-config/Hk_project_Config_DefaultEngine.ini:88` ships `r.RayTracing=True` — this only
compiles the RT pipeline **in** as a capability; it is not itself proof that RT reflections or
RT shadows are the code path actually taken at runtime (that additionally needs
`RayTracingReflectionOptions.bEnabled`'s own gating, and functioning DXR support through
vkd3d-proton on this hardware). `r.RayTracing.Shadows` defaults to **1** in stock UE4
(`CVarRayTracingOcclusion`, `LightRendering.cpp:59-63`, "1: use ray tracing shadows (default)")
and Stray's own config never overrides it — so it is **plausible, not confirmed**, that
directional-light shadows in Stray are already ray-traced on this RTX 4090/vkd3d-proton stack,
in which case `r.Shadow.Denoiser`'s change (default 2 → mod's 1) is live rather than inert.
Both modes route through the same `FDefaultScreenSpaceDenoiser` implementation absent a
third-party override plugin (`GetDefaultDenoiser()` vs. `GScreenSpaceDenoiser`, and Stray shows
no evidence of a plugin overriding the latter) — so even where live, this specific change is
functionally a wash, not a quality or pass-count change. **UNCONFIRMED without a live capture
whether RT shadows are active at all** — flagged here because it is the one place this audit
could not close the loop from static source alone, exactly the kind of thing CLAUDE.md's
epistemic discipline says to label rather than guess.

**Ambient occlusion.** No top-level `r.AmbientOcclusion.Denoiser` cvar declaration could be
found searching `ScreenSpaceDenoise.cpp`, `DistanceFieldAmbientOcclusion.cpp`,
`IndirectLightRendering.cpp`, or `ScreenSpaceRayTracing.cpp` on the mirror. Only
`r.AmbientOcclusion.Denoiser.ReconstructionSamples`, `.PreConvolution`,
`.KernelSpreadFactor`, `.TemporalAccumulation`, and `.HistoryConvolution.{SampleCount,
KernelSpreadFactor}` exist as real sub-cvars (`ScreenSpaceDenoise.cpp:68-93`) — configuration
for a denoiser instance that must already be running via some *other* gate, not a mode toggle
itself. The nearest real top-level AO/GI denoiser toggle is `r.DiffuseIndirect.Denoiser`
(`IndirectLightRendering.cpp:26-29`, default 1), which the mod never sets. **UNCONFIRMED, but
the likeliest read is that `r.AmbientOcclusion.Denoiser=1` in this mod is a no-op cvar name in
UE 4.27** — it may be a genuine UE5-era cvar (a community wiki fetch used for the shadow/
reflections cross-check this session was drawn from a UE **5.4.4** cvar reference site) copied
into a 4.27 config by an author working from mixed-version notes.

**Net**: **do not expect this mod, as configured, to add new dispatches to the known 9-hash
`FSSDTemporalAccumulationCS` family** or to change which of the two named permutations
(`0x1708ec956099e259` #5, `0x52101a15e1a0c5cc` #0) appear. The one place this conclusion could
be wrong is if RT shadows are silently active — re-run the denoiser-suppression experiment's
census (dispatch count per hash) with and without this mod if the user reports a visible
change, rather than assuming either way.

### 2.5 `r.MipMapLodBias=-1` — stacks with UE4's own auto-bias, not with anything of ours

Two genuinely different mechanisms, easy to conflate by name:

* **`r.Streaming.MipBias`** (mod: 0, shipped: 0 — no-op either way) controls which mip level the
  **streaming system keeps resident** in the texture pool. It cannot make the sampler select a
  sharper mip than what is actually loaded.
* **`r.MipMapLodBias`** (mod: -1, shipped/engine default: 0 — **real change**) is a global,
  additive bias applied at the **hardware sampler** level for every 2D texture, independent of
  streaming residency, on top of whatever LOD the texture's own derivatives would otherwise
  select. This is the standard D3D/HLSL sampler-bias mechanism (`SampleBias`/the sampler
  state's `MipLODBias` field) — bias terms from different sources (a resource's own sampler
  state, a shader-supplied bias) are additive by construction in the GPU's LOD calculation
  (HARD, general D3D/graphics-API behaviour, not UE-specific).

**Our project sets no mip bias anywhere** — `grep -rn "MipBias" src/ shaders/` returns nothing.
We rely entirely on UE4's own automatic per-view term (RESEARCH.md §4.12, "Bonus 3"):

```
View.MaterialTextureMipBias = -max(-log2(fraction), 0) + r.MinAutomaticViewMipBiasOffset
```

where `fraction = renderRes/displayRes`, only nonzero once `r.TemporalAA.Upsampling=1` **and**
screen percentage is below 100.

* **Current 1:1 (DLAA) milestone:** `fraction≈1` → the automatic term is 0. The mod's flat -1
  is the *only* bias in effect. NVIDIA's own published DLSS mip-bias recommendation (SOFT — a
  web-search synthesis this session, not fetched from the PDF directly, but internally
  consistent and matching general recollection of the guide) is
  `NativeBias + log2(renderRes/displayRes) - 1.0`; at a 1:1 ratio that formula independently
  evaluates to exactly **-1** with `NativeBias=0`. **The mod's value happens to land where
  NVIDIA's own guide would put it for this ratio** — a coincidence of two independently-derived
  numbers, not a designed match, and worth an on-target visual check (a DLAA screenshot,
  A/B'd against `r.MipMapLodBias=0`) before trusting it, per CLAUDE.md's "measured, not
  assumed" discipline.
* **Future SR milestone (§4, ~50% screen percentage, `fraction=0.5`):** UE4's automatic term
  computes `-max(-log2(0.5),0) = -1`. Adding the mod's flat -1 (which, in NVIDIA's own formula,
  is exactly the `NativeBias` term the formula expects the *game* to already be applying) gives
  a combined **-2**, which is precisely what NVIDIA's formula recommends for this ratio
  (`log2(0.5) - 1.0 = -2`). **Without the mod, our SR path would under-bias by about one stop**
  relative to NVIDIA's own guidance, since it currently has no `NativeBias` term of its own —
  this mod's `-1` is one legitimate way to supply exactly that missing term. Not a
  correctness break either way; a sharpness/aliasing trade that only a screenshot can confirm.

**No conflict, no double-application** — we have never implemented a competing bias, so there
is nothing for the mod's `-1` to double up against. The one thing worth remembering is that
`r.MipMapLodBias` is a *global* texture-sampler bias, unrelated to the *per-view, TAAU-specific*
automatic term, and the two happen to be additive rather than one overriding the other.

### 2.6 Motion blur fully disabled — helps our own diagnostic methodology

`r.DefaultFeature.MotionBlur=0`, `r.MotionBlurQuality=0`, `r.MotionBlur.Amount=0`,
`r.MotionBlurSeparable=0` — four independent knobs, all pointing the same direction, fully
removes `FMotionBlurFilterCS` from the frame. CLAUDE.md's own §"suppressing it freezes the
picture" lesson names motion blur specifically as a freeze-confound for our pass-suppression
identification tests, because it "is a full-res HDR compute pass that runs *after* TAA"
(`PostProcessMotionBlur.cpp:366`) and would freeze the displayed image just as convincingly as
suppressing the real TAA pass would. With this mod active, that specific confound is
structurally absent from the frame — **helpful for anyone running the suppression/dry-run
methodology while this mod is installed**, and neutral (neither helpful nor harmful) for DLSS
image quality itself, since motion blur runs entirely downstream of our TAA-hook capture point
and never touches any texture we read or write.

### 2.7 `r.ParallelVelocity` and the rest of the `r.Parallel*` family — no data-path impact

All seven `r.Parallel*` cvars this mod sets (`ParallelBasePass`, `ParallelGatherShadowPrimitives`,
`ParallelInitViews`, `ParallelPrePass`, `ParallelShadows`, `ParallelTranslucency`,
`ParallelVelocity`) control whether the RHI **records command lists on worker threads** for
their respective pass, a CPU-side scheduling decision. None of them changes a render-target
format, a shader permutation, or per-frame content — the recorded commands are identical
whether generated on one thread or several. `r.ParallelVelocity=1` specifically parallelises
recording of the separate opaque-velocity pass draws (`RESEARCH-RR-GBUFFER.md` §1.3); it does
not change velocity's format (`PF_A16B16G16R16` regardless, per `FVelocityRendering::GetFormat`,
already HARD in that document), its encoding (`Common.ush`'s constants, CLAUDE.md §2.5,
untouched), or which primitives get a velocity draw at all. **No interaction with our resolve
pass or with the sparse-velocity reconstruction math.**

### 2.8 Everything else that could plausibly touch our capture — checked, no interaction found

* **`r.MaterialQualityLevel=3`** recompiles *material shader* permutations (BRDF complexity,
  feature branches inside the base pass), not the base-pass *structural* MRT layout the
  classifier keys on, and not the TAA/SSD shader identities (those are global shaders,
  independent of material quality level). No interaction.
* **`r.NormalMapsForStaticLighting=1`** adds a term to *baked* lighting only — does not touch
  GBufferA's stored normal (that is always the *dynamic* shading normal, unaffected).
* **`r.CustomDepth`** is not touched by this mod at all (Stray ships `r.CustomDepth=3` in
  `DefaultEngine.ini`, unchanged) — irrelevant here, noted only because it is a plausible thing
  to have worried about given it shares the word "depth" with our own capture registers (`t2`
  scene depth, §2.4 of CLAUDE.md); custom depth is an entirely separate resource UE4 uses for
  outline/highlight effects, never bound to `FTAAStandaloneCS`.
* **`r.SeparateTranslucency`/`.ScreenPercentage`/`r.TranslucencyVolumeBlur`** all resolve after
  TAA in the frame graph — never in our capture window.
* **`r.DisableDistortion=1`** removes a base-pass *material* feature (refractive distortion),
  not a G-buffer target or the TAA shader.

---

## 3. Bottom line

**SAFE to run alongside the SR/DLAA milestone as currently shipped.** Nothing in this mod
changes the TAA pass's identifying signature in a way our structural matcher cannot follow,
changes velocity's format/encoding, breaks jitter, or forces DLSS's own mip-bias assumption
into conflict (§2.1-§2.3, §2.5, §2.7 above). The one new unknown worth watching is SSR's own
dedicated temporal-reprojection shader (§2.4) — not currently one of our named candidates, but
also not gating anything our hook depends on; it just means "one more shader in the frame we
have not looked at," which is true of dozens of shaders in any UE4 title already.

**BREAKING for the DLSS Ray Reconstruction milestone, specifically and only because of
`r.GBufferFormat=3`.** This is unconditional and silent: `gbuffer_classify.cpp`'s RGB10A2
anchor at slot 1 rejects every base-pass MRT set the instant this cvar is active, and
`gbuffer_resolve.hlsl` never dispatches. Nothing else in the file causes this class of failure.

**Exact override**, appended to the *same* `[/Script/Engine.RendererSettings]` section, in the
*same* runtime `Saved/Config/WindowsNoEditor/Engine.ini` the mod itself is dropped into,
**after** the mod's own lines (UE4's ini cascade takes the last-seen value for a repeated key
within one section/file — CLAUDE.md §2.2's "Engine.ini settings take effect" applies to this
config layer exactly as it does to our own `[SystemSettings]` recommendations; the mod's choice
of `[/Script/Engine.RendererSettings]` rather than `[SystemSettings]` was cross-checked against
Stray's own shipped `[SystemSettings]` block, which contains only `r.HZBOcclusion=0` and some
`au.*`/`HK.*` custom cvars — none of the mod's or this override's keys collide with it, so there
is no cross-section priority hazard for anything listed below):

```ini
[/Script/Engine.RendererSettings]
; --- restore stray-dlss RR compatibility, keep the mod's other quality gains ---
r.GBufferFormat=1
```

That single line is the only cvar this audit found to be load-bearing for our add-on's
correctness. Two more are worth adding **only if the user wants to be conservative** rather
than because §2 found a break:

```ini
; optional, conservative: removes the one flat mip-bias term from the analysis in §2.5
; entirely, at the cost of the sharper texture sampling this mod is going for. Not required —
; §2.5's math says the current value is fine or even beneficial — but it removes a variable
; if a screenshot A/B ever looks aliased where it didn't before.
r.MipMapLodBias=0
```

`r.SceneColorFormat` needs **no override** — already covered (§2.2). None of the denoiser
cvars, the TAA/TAAU cvars, or the motion-blur cvars need overriding for either the SR or the RR
milestone (§2.3, §2.4, §2.6).

**If the RR classifier is ever extended to accept `RGBA16F` at the GBufferA slot** (§2.1's "what
would be needed" paragraph — not implemented here, `src/` was intentionally left untouched per
task scope), the `r.GBufferFormat=1` override becomes optional too, and this mod's higher-
precision normals would in principle *improve* RR's guide-buffer quality rather than merely
being tolerated. That is a future code change, not something this report performs.
