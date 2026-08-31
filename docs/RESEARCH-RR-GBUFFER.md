# DLSS Ray Reconstruction: sourcing the G-buffer guide inputs from outside the engine

Research report, 2026-08-31. Settles the design decision for the RR milestone: **how the
add-on obtains world normals, roughness, diffuse albedo and specular albedo per frame**,
after the velocity-anchored base-pass MRT hunt (`src/gbuffer_finder.cpp` +
`src/core/gbuffer_classify.cpp`) failed in live observation.

Provenance labels follow CLAUDE.md §0.5: **HARD** = read verbatim from a primary source
(engine source, NVIDIA's own docs/headers/sample code, our own measurements/code);
**SOFT** = credible secondary source; **UNCONFIRMED** = inference, must be verified before
code depends on it.

UE citations are **UE 4.27.2** file:line, read from the public mirror
`github.com/AlexMercer-MA/UnrealEngine-4.27` (master), whose `Engine/Build/Build.version`
reads `4.27.2, CompatibleChangelist 17155196, BranchName ++UE4+Release-4.27` — the exact
engine Stray ships (CLAUDE.md §2.1). Mirror provenance makes each of these one notch below
a first-party read; treat them as HARD-via-mirror. Note this is **stock** 4.27.2 — Stray is
a licensee build (`v1.54368 Revision 26632`) and §1.4 below documents one place where the
live game demonstrably deviates from stock.

---

## 0. Verdict up front

1. **The G-buffers are NOT released after lighting.** In 4.27 they live from before the
   base pass to the very end of `FDeferredShadingSceneRenderer::Render` — after all
   post-processing, TAA included. At our existing TAA interception point the current
   frame's GBufferA/B/C are alive and final. The "pooled-RT release after lighting"
   hypothesis is wrong for this engine version (§1.1).
2. **The velocity anchor was the wrong signature, twice over.** (a) `R16G16B16A16_UNORM`
   is the velocity format on every ray-tracing-capable shader platform *regardless of
   which pass writes it* — it never implied base-pass velocity. (b) The live game
   demonstrably runs a **6-RTV base pass without a velocity target**, contradicting what
   stock 4.27 + Stray's own inis predict (7 with velocity at slot 4) — a licensee delta or
   an unresolved runtime path; either way, anchoring on velocity guaranteed failure (§1.3–1.4).
3. **The observed 6-RTV sets are almost certainly the base pass itself**, identifiable by
   a velocity-free format-tuple signature that Stray's shipped config makes unusually
   strong: slot 0 = `R11G11B10F` (the game ships `r.SceneColorFormat=3`), slot 1 =
   `RGB10A2` (GBufferA, unique format in the frame), slots 2–3 = 8-bit RGBA (B, C), all at
   one extent, DSV = the known depth resource, draw-count dominance (§1.5).
4. **Recommended strategy: writer-side identification with the corrected signature,
   consumed at the TAA hook** — a `gbuffer_resolve` compute pass (clone of the proven
   `mv_resolve` machinery) reads the live GBufferA/B/C + depth right before the DLSS-D
   evaluate and emits the four RR guide textures using NVIDIA's own UE-plugin recipe
   (§4.4). Reader-side capture at the SSD-denoiser / lighting passes is the corroboration
   signal and the fallback, not the primary (§4.2–4.3). UEVR-style engine-memory access is
   precedented but a last resort (§4.5).
5. **Nobody has ever done this before.** Every shipped "RR mod" either flips a cvar on a
   game whose engine already integrates RR, proxies a native RR integration, or replaces
   the entire renderer. Guide-buffer scavenging from a raster game would be a first (§3).
6. **The color-signal question is the real quality risk, not the buffers.** RR's API is
   satisfied by our existing color/depth/MV/jitter path plus the four guides; whether the
   network produces an acceptable image on Stray's composited raster lighting is
   UNCONFIRMED and only the on-target experiment can answer it (§2.6, §5). The staged plan
   in §4.6 is designed so that failure is cheap and diagnosable.

---

## 1. Q1 — UE 4.27 G-buffer lifecycle, and the post-mortem of the MRT hunt

### 1.1 Allocation and release: alive from pre-base-pass to end of frame

* **Allocation** happens inside `FDeferredShadingSceneRenderer::Render`, before the base
  pass: `SceneContext.PreallocGBufferTargets(); SceneContext.AllocGBufferTargets(RHICmdList);`
  (DeferredShadingRenderer.cpp:1799-1803), with the load-bearing comment *"Even if
  !bShouldRenderVelocities, the velocity buffer must be bound because it's a compile time
  option for the shader."* (:1801). [HARD]
* `AllocGBufferTargets` (SceneRenderTargets.cpp:1099-1176) acquires GBufferA/B/C/D
  (+E when `bAllowStaticLighting`, +`SceneVelocity` when `bAllocateVelocityGBuffer`) from
  `GRenderTargetPool.FindFreeElement`, then sets `GBufferRefCount = 1` (:1175). [HARD]
* **Release** is the last thing the renderer does — *after* `AddPostProcessingPasses`
  (:2632, TAA runs inside it): an RDG pass added at DeferredShadingRenderer.cpp:2644-2654
  calls `SceneContext.AdjustGBufferRefCount(InRHICmdList, -1)` (:2653) and
  `SceneContext.SceneVelocity.SafeRelease()` (:2654). `AdjustGBufferRefCount` at refcount 0
  calls `ReleaseGBufferTargets()` (SceneRenderTargets.cpp:1209-1218), which `SafeRelease`s
  every GBuffer pointer (:1010-1019). The header states the contract: *"GBuffer: Geometry
  Buffer rendered in base pass for deferred shading, only available between
  AllocGBufferTargets() and FreeGBufferTargets()"* (SceneRenderTargets.h:475). [HARD]
* **Consequence:** at the TAA dispatch — our interception point — the current frame's
  GBufferA/B/C are allocated, written, and no longer being written (base pass, decals and
  lighting are all upstream). The failed hunt's premise that "nothing G-buffer-like is
  bindable at later interception points because of pooled-RT release after lighting" is
  **refuted** for 4.27. [HARD]
* **Pool survival between frames:** `ReleaseGBufferTargets` only drops the
  `TRefCountPtr<IPooledRenderTarget>` references; the pool keeps the element and hands the
  same one back next frame (`Found->UnusedForNFrames = 0` on reuse,
  RenderTargetPool.cpp:590). Elements become eviction candidates only after
  `UnusedForNFrames > 2` *and* pool budget pressure (`TickPoolElements`,
  RenderTargetPool.cpp:1014-1094). So the underlying `ID3D12Resource` pointers are stable
  in steady state and die at resolution changes, level transitions and pool churn — which
  matches the two measured Xid-109 crashes at save-load transitions (`src/mv_resolve.cpp`
  keep-alive comment). Same-frame use with liveness checks + a keep-alive ring is the
  correct consumption discipline; a cross-frame *cache* of pointers is what kills. [HARD]

### 1.2 The MRT layout, and what Stray's own config selects

`FSceneRenderTargets::GetGBufferRenderTargets` (SceneRenderTargets.cpp:734-779) builds the
base-pass MRT list in this exact order [HARD]:

| MRT | Target | Condition | Format under Stray's shipped config |
|---|---|---|---|
| 0 | SceneColor | always | `PF_FloatR11G11B10` — the game ships **`r.SceneColorFormat=3`** (WindowsEngine.ini:69, `docs/game-config/`) |
| 1 | GBufferA | `bUseGBuffer` | `PF_A2B10G10R10` = RGB10A2 (GetGBufferAFormat, SceneRenderTargets.cpp:1026-1034; high-precision variant needs `r.GBufferFormat>=3`, not set) |
| 2 | GBufferB | `bUseGBuffer` | `PF_B8G8R8A8` (:1048-1056) |
| 3 | GBufferC | `bUseGBuffer` | `PF_B8G8R8A8`, created with **`TexCreate_SRGB`** (:1058-1070, :1137) |
| 4 | SceneVelocity | `bAllocateVelocityGBuffer` | `PF_A16B16G16R16` (see §1.3) — *"The velocity buffer needs to be bound before other optionnal rendertargets"* (:749-751), `check(OutVelocityRTIndex == 4)` (:754) |
| next | GBufferD | `bUseGBuffer` | `PF_B8G8R8A8` |
| next | GBufferE | `bAllowStaticLighting` | `PF_B8G8R8A8`; `check(MRTCount == (bAllocateVelocityGBuffer ? 6 : 5))` before adding it (:771) |

The shader side agrees: `Out.MRT[4] = OutVelocity` under `GBUFFER_HAS_VELOCITY`, D at
`GBUFFER_HAS_VELOCITY ? 5 : 4`, E at `? 6 : 5` (BasePassPixelShader.usf:1491-1498). [HARD]

Stray ships `r.AllowStaticLighting=True` (DefaultEngine.ini:38) and
`r.BasePassOutputsVelocity=True` (:59). [HARD] So **stock 4.27.2 with Stray's own inis
predicts a 7-RTV base pass with velocity at slot 4**. Every user of this MRT list binds the
whole of it: the base pass (`GetGBufferRenderTargets(GraphBuilder, BasePassTextures, …)`,
BasePassRendering.cpp:845; bound for GBufferClear :878-891, the base pass proper :944-950,
ClearGBufferAtMaxZ :978) and the single-layer-water pass
(SingleLayerWaterRendering.cpp:711). No stock pass binds the set minus velocity. [HARD]

The ini→cvar chain was verified end to end, closing two suspected loopholes:

* The `[/Script/Engine.RendererSettings]` section is applied to cvars at startup
  (`ApplyCVarSettingsFromIni`, LaunchEngineLoop.cpp:2251) **before** `RenderUtilsInit()`
  latches the platform masks (:2601-2603), and in a shipping build the mask is set purely
  from the cvar: `GBasePassVelocityPlatformMask = ~0ull` when the cvar is nonzero
  (RenderUtils.cpp:1200-1204); the per-TargetPlatform refinement is `#if WITH_EDITOR` only
  (:1230-1323) and the `#else` branch clears the mask only on mobile (:1326-1330).
  `IsUsingBasePassVelocity` is just the mask bit (RenderUtils.h:552-556). [HARD]
* The ini value `True` for the int cvar is not mis-parsed as 0:
  `OnSetCVarFromIniEntry` runs `ConvertValueFromHumanFriendlyValue(Value)` first
  (ConfigCacheIni.cpp:4664-4669). Cook-time `UsesBasePassVelocity()` reads the *same* cvar
  (TargetPlatformBase.cpp:28-32), so cooked shaders and runtime cannot disagree in a stock
  build. [HARD]

### 1.3 Velocity: the format never meant what the classifier assumed

* `FVelocityRendering::GetFormat` (VelocityRendering.cpp:354-358):
  `return FDataDrivenShaderPlatformInfo::GetSupportsRayTracing(ShaderPlatform) ?
  PF_A16B16G16R16 : (IsAndroidOpenGLESPlatform ? PF_G16R16F : PF_G16R16);`
  On PC D3D12 SM5 — an RT-capable platform — velocity is `R16G16B16A16_UNORM` **whether it
  is written by the base pass or by the separate velocity pass**. The comment in
  `src/core/gbuffer_classify.cpp` ("consistent with FVelocityRendering::GetFormat returning
  PF_A16B16G16R16 *when base-pass velocity is enabled*") is wrong about 4.27 and must be
  corrected. The measured TAA input `t3 R16G16B16A16_UNORM` (CLAUDE.md §2.5) is therefore
  evidence of the *platform*, not of base-pass velocity. [HARD]
* **The separate opaque velocity pass runs in Stray even with base-pass velocity on**:
  *"If bBasePassCanOutputVelocity is set, basepass fully writes the velocity buffer unless
  bUseSelectiveBasePassOutputs is enabled"* — `if (bShouldRenderVelocities &&
  (!bBasePassCanOutputVelocity || bUseSelectiveBasePassOutputs)) RenderVelocities(…,
  EVelocityPass::Opaque, …)` (DeferredShadingRenderer.cpp:2078-2083), and Stray ships
  **`r.SelectiveBasePassOutputs=True`** (DefaultEngine.ini:85). Mechanism: with selective
  outputs, static-lit meshes do not export velocity in the base pass —
  `IsSeparateVelocityPassRequiredByVertexFactory = IsUsingSelectiveBasePassOutputs(…) &&
  bVertexFactoryUsesStaticLighting` (VelocityRendering.cpp:383-391). [HARD]
* The separate pass binds **one** RTV (the velocity texture) + depth
  (VelocityRendering.cpp:305-315), early-outs entirely when no primitive needs velocity
  (`!bHasAnyDraw && !bForceVelocity → continue`, :275-279), and skips camera-cut views
  (*"Skip camera cuts which effectively reset velocity for the new frame"*, :395). A menu
  scene with nothing moving produces **zero** velocity-RTV binds — consistent with the
  measured menu census. [HARD]

### 1.4 The anomaly: stock predicts 7-with-velocity; the live game shows 6-without

The measured facts (live observation runs, 2026-08-31, this project) [HARD]:

* Menu: 6-RTV sets bind ~600 times; **none** of the six is `R16G16B16A16_UNORM`.
* A set the classifier accepted (velocity at slot 4, slots 0-3 populated, one extent)
  appeared **exactly once**, at the save-load boundary (~frame 2895 in two independent
  runs), then never again through minutes of gameplay.
* Steady-state gameplay: no ≥4-RTV set carrying a velocity-format target, ever.

Stock source cannot produce the 6-RTV/no-velocity shape given Stray's inis (§1.2): with
the mask set, every base-pass bind carries velocity at slot 4 and counts 7. The
contradiction is real and unresolved; ranked hypotheses, each with its discriminating
observable:

1. **Licensee delta** (most likely): BlueTwelve disabled base-pass velocity in code — e.g.
   to sidestep the AMD hazard the engine's own comment describes at the velocity slot
   (SceneRenderTargets.cpp:749-751), or as a cost cut once SelectiveBasePassOutputs forced
   the separate pass to exist anyway — leaving the 6-RTV layout SC,A,B,C,D,E.
   *Discriminator:* the FAILED-window census should show narrow (1-RTV) velocity-format
   sightings during gameplay (the separate pass), and the 6-RTV sets' formats should read
   `R11G11B10F, RGB10A2, RGBA8, RGBA8, RGBA8, RGBA8`. The existing `GBUF` census already
   records both; one gameplay run answers it. [UNCONFIRMED]
2. **The once-at-load sighting is an allocation-frame artifact** — e.g. the `GBufferClear`
   raster pass (BasePassRendering.cpp:878-891) or another bind emitted only on the frame
   the scene-texture allocation changes, under whatever conditional the licensee added.
   *Discriminator:* log the full slot table (slot/format/extent/resource) of the **first**
   `is_base_pass` candidate immediately, not only after 30 stable frames — the current
   recorder discards exactly the evidence that would name this bind. [UNCONFIRMED]
3. **Observation-side artifact**: a liveness/describe failure hiding the velocity RTV.
   Rejected as the primary explanation because the RTV-count histogram buckets **raw**
   `count` (gbuffer_finder.cpp, `note_render_targets`), so a 7-RTV bind with an
   undescribable slot 4 would still land in bucket 7 — and the observed sets are raw-count
   6. Kept only as a caveat on the *classified* membership. [HARD for the histogram
   semantics; the inference is UNCONFIRMED]

**Could ReShade legitimately never see the main base-pass MRT set?** No specific mechanism
survives scrutiny: the add-on registers both `bind_render_targets_and_depth_stencil` and
`begin_render_pass` (src/addon.cpp:1082-1084) and the census proves the tap fires
(first-event log line, nonzero histograms). UE 4.27's D3D12 RHI reaches
`OMSetRenderTargets` through its state cache for every command list, including the
parallel base-pass lists — ReShade proxies every `ID3D12GraphicsCommandList` the device
creates, on whatever thread records it. [HARD for our registration and the census; the
RHI-path claim is HARD-via-mirror at the level of "no alternative bind path exists in
`GetGBufferRenderTargets`'s callers", though no line item proves the negative]

A tempting shortcut that does **not** exist: reading D3D12 debug names
(`GetPrivateData(WKPDID_D3DDebugObjectName)`) to find a resource literally named
"GBufferA". UE only names objects outside shipping builds: `#define NAME_OBJECTS
!(UE_BUILD_SHIPPING || UE_BUILD_TEST)` (D3D12RHIPrivate.h:94). [HARD]

### 1.5 The corrected identification signature (writer-side)

Stray's shipped config makes the base pass identifiable **without** velocity [HARD inputs,
signature itself is a design claim]:

* slot 0 `R11G11B10_FLOAT` at scene extent — `r.SceneColorFormat=3` is the game's own
  setting, and R11G11B10 scene colour was already observed live (the `u2` of the composite
  pass, CLAUDE.md §1);
* slot 1 `R10G10B10A2_UNORM` at the same extent — GBufferA, **the only RGB10A2 render
  target in a stock deferred frame** (the swapchain is RGB10A2 too, but its extent is the
  output resolution and it is never bound mid-frame with 5 siblings);
* slots 2-3 8-bit RGBA at the same extent (B, C; C's view may read as sRGB);
* ≥5 colour targets total, one shared extent, DSV present and equal to the known depth
  resource (the §2.4 R32G8X24 pair);
* the frame's dominant draw count (a base pass is hundreds-to-thousands of draws —
  the existing `gbuffer_finder` candidate logic already measures this);
* velocity at slot 4, **if present**, upgrades confidence and shifts D/E — it must be
  accepted, not required.

B and C remain format-indistinguishable from each other; the slot order (B=2, C=3) is
HARD in stock source (§1.2 table) rather than SOFT as `gbuffer_classify.hpp` currently
labels it — but a licensee reorder is still conceivable, so the runtime cross-check in
§4.3 (GBufferC feeds albedo-consuming passes; GBufferB feeds roughness-consuming ones)
and one visual sanity capture remain required before the values are trusted.

### 1.6 Which later passes bind the G-buffers as SRVs (reader-side map)

* The engine's two binding shapes [HARD]:
  * **Uniform-buffer flavor** — `FSceneTextureUniformParameters`, shader-side cbuffer
    `SceneTexturesStruct` (`IMPLEMENT_STATIC_UNIFORM_BUFFER_STRUCT(…,"SceneTexturesStruct",…)`,
    SceneRenderTargets.cpp:2286); member order SceneColor, SceneDepth, GBufferA…F,
    Velocity, SSAO, CustomDepth (SceneRenderTargetParameters.h:10-33). Used by the
    standard deferred light pixel shader (`FDeferredLightPS` via `GetRenderLightParameters`,
    LightRendering.cpp:1186-1202). On D3D12 the UB's member textures are flattened to
    `t`-registers through the shader's serialized resource table — the **same
    `FD3D12ShaderResourceTable` blob `tools/shaderlib_extract.py` already parses**, so the
    register of each member is derivable offline per permutation.
  * **Loose-parameter flavor** — `FSceneTextureParameters` (SceneTextureParameters.h:12-22,
    filled straight from `SceneContext.GBufferA/B/C…` at SceneTextureParameters.cpp:9-31).
    Used by the SSD denoiser family (`FSSDCommonParameters` includes it,
    ScreenSpaceDenoise.cpp:678-690), `FDiffuseIndirectCompositePS`
    (IndirectLightRendering.cpp:52-93) and `FReflectionEnvironmentSkyLightingPS`
    (:126-233). Only members the compiled permutation references are bound.
* `FSSDTemporalAccumulationCS` — the pass we already intercept (all nine cooked hashes,
  `src/core/taa_signature.hpp:133-141`) — binds, per permutation: the
  `FSceneTextureParameters` subset it uses, plus `PrevDepthBuffer`, **`PrevGBufferA`,
  `PrevGBufferB`** (its own extracted copies of last frame's normal/roughness buffers,
  ScreenSpaceDenoise.cpp:1060-1090, extraction at :1892-1901). The .usf samples
  `GBufferVelocityTexture` directly (SSDTemporalAccumulation.usf:467) and reaches
  normal/roughness either through `GetGBufferDataFromSceneTextures` (SSDCommon.ush:236) or
  through the compressed-metadata path (SSDMetadata.ush:125-135) depending on
  `CompressedMetadata` layout — which of the two Stray's cooked permutations use is
  decidable offline from the DXBC's declared resources. [HARD]
* Which SSD signal runs in Stray is **open**: stock gates say SSGI needs `r.SSGI.Enable>0`
  (default 0, ScreenSpaceRayTracing.cpp:47, gate at :75-95 — Stray ships only
  `r.SSGI.Quality=2`) and SSR uses its own TAA-family temporal pass rather than SSD unless
  `r.SSR.ExperimentalDenoiser` (default 0) is set (IndirectLightRendering.cpp:31-34, 769-771)
  — yet `0x1708ec956099e259` (SSD temporal accumulation, perm #5) demonstrably dispatches
  at 240×135 in live gameplay (CLAUDE.md §2.3). Another stock-vs-live delta; the
  permutation-ID decode (offline) names the signal. **Consequence: do not anchor anything
  load-bearing on the SSD passes running every frame.** [HARD for both sides of the
  contradiction; the "licensee enables SSGI at runtime" reading is UNCONFIRMED]

---

## 2. Q2 — What DLSS-RR actually requires, and how NVIDIA's own integrations feed it

Primary sources: the DLSS-RR Integration Guide PDF (NVIDIA/DLSS repo, SWE-DLSS-001-PGRF,
Dec 2025 revision, read in full), the Streamline `ProgrammingGuideDLSS_RR.md` (v2.12.0),
our vendored SDK v310.7.0 headers (`third_party/ngx/include/nvsdk_ngx_defs_dlssd.h`,
`nvsdk_ngx_helpers_dlssd.h`), and the source of three real integrations: the official UE
plugin v3.7.3 (public mirror), dxvk-remix, and RTXPT. Full URLs in §6.

### 2.1 Required inputs

**Required** (Streamline §4.1 lists them without an "optional" marker): Diffuse Albedo,
Specular Albedo, Normals, Roughness, Color, Motion Vectors, Depth, and (Specular MVs *or*
Specular Hit Distance + WorldToView + ViewToClip), plus Output. **Optional only**:
Transparency Overlay, Color-Before-Transparency, SSS guides, DoF guide — and, in the
headers but undocumented, `pInDisocclusionMask`, `DLSSD.ReflectedAlbedo`, ray-direction
guides (`nvsdk_ngx_helpers_dlssd.h:153-222`). [HARD]

The specular-motion pair is droppable in practice: the guide marks hit distance "only
needed if Specular Motion Vectors are not provided", dxvk-remix passes hit distance only
when it has one, the UE plugin passes **neither** specular MVs nor hit distance nor the
matrices, and a first-hand integrator reports no observed benefit from specular MVs
[HARD for guide/plugin/Remix; SOFT for the quality report]. That omitting both merely
degrades reflections rather than failing the evaluate is UNCONFIRMED — but NVIDIA's own
UE plugin ships that way, which is the strongest possible existence proof for a raster
integration.

Exposure is **not** an RR input: "Exposure, Auto-Exposure, Sharpness … are not supported
by DLSS Ray Reconstruction" (PDF §3.7); `InPreExposure` remains in the eval struct and
dxvk-remix passes it. [HARD]

### 2.2 Encodings and formats

| Buffer | Requirement (PDF §3.4 / SL §4.1) | What real integrations ship |
|---|---|---|
| Normals | "Shading Normals (Normalized). Can be View Space or World Space. **RGB16_FLOAT or RGB32_FLOAT**" — signed float, NOT [0,1]-packed | RGBA16F **world-space** in all three (UE plugin, Remix, RTXPT); RTXPT/UE pack roughness in .w |
| Roughness | "**Linear Roughness**", R channel of a standalone texture, or packed in normals.w with `NVSDK_NGX_DLSS_Roughness_Mode_Packed` (`nvsdk_ngx_defs_dlssd.h:23-27,57`) | R32F (UE plugin), R8_UNORM (Remix), packed (RTXPT) |
| Diffuse albedo | "diffuse component of Reflectance material… **sRGB formats are not supported**" | R11G11B10F (UE, RTXPT), RGB10A2_UNORM (Remix) |
| Specular albedo | "**average specular reflectivity given a view direction**" | R11G11B10F (UE, RTXPT), RGB10A2_UNORM (Remix) |
| Depth | "same depth data used to generate motion vector data (View Space Depth or HW Depth)", type declared at create via `DLSS.Use.HW.Depth` | UE plugin: **linear** depth it resolves itself, type Linear; Remix: linear virtual Z declared HW |
| MVs | RG16F/RG32F dense — identical to DLSS-SR semantics | our `mv_resolve` output already matches |
| Color | "Noisy Ray Traced Input Color", HDR mandatory, **before tonemapping**, DoF strictly after RR | UE plugin passes the ordinary post-lighting scene color of a raster frame |

"DLSS-RR uses formatted reads, therefore it should handle most input buffer formats"
(PDF §3.4). No integration ever passes [0,1]-biased normals — assume the DLL does not
decode them; we must decode GBufferA (`N*0.5+0.5`, §2.5) back to signed floats. [HARD]

### 2.3 The sanctioned SpecularAlbedo math

PDF appendix and SL §4.2.1 give the exact HLSL (`EnvBRDFApprox2`, the Ray Tracing Gems
ch. 32 preintegrated-GGX approximation): inputs are **specular F0**, **alpha = perceptual
roughness squared**, `NoV = |N·V|`; the same coefficient matrices appear verbatim in
dxvk-remix (`brdf.slangh:676-705`), RTXPT (`Microfacet.hlsli:282`) and the UE plugin
(`EnvBRDFApproxRTG` in `GBufferResolve.usf`). The UE plugin adds two empirical tweaks
worth copying: it remaps `NoV` as `x=NoV*NoV; NoV = x/(x+(1-NoV)*(1-NoV));` and scales the
result's NoV argument by **0.75**. [HARD]

### 2.4 The official UE recipe — our template

NVIDIA's UE plugin (the only official raster-engine RR integration) resolves everything
**from the standard deferred GBuffer in one pixel-shader pass at the upscale point**
(`AddGBufferResolvePass`, DLSSUpscaler.cpp:578-589; GBufferResolve.usf) [HARD]:

```
OutDiffuseAlbedo  = GBuffer.DiffuseColor                        // = BaseColor − BaseColor·Metallic
OutSpecularAlbedo = EnvBRDFApproxRTG(GBuffer.SpecularColor,     // F0
                                     Square(GBuffer.Roughness), // alpha
                                     NoV * 0.75)                // V from depth-reconstructed world pos
OutNormal         = float4(GBuffer.WorldNormal, GBuffer.Roughness)   // world-space, signed
OutRoughness      = GBuffer.Roughness
OutDepth          = GBuffer.Depth                               // linear; RR depth input REPLACED with this
```

Formats: albedos `PF_FloatR11G11B10`, normals `PF_FloatRGBA`, roughness/depth `PF_R32_FLOAT`,
all at the input rect, shifted to the top-left corner, subrect bases 0. Roughness mode
Unpacked, depth type Linear (create struct memzeroed). No specular MVs, no matrices, no
disocclusion mask. On create failure it falls back to DLSS-SR. [HARD]

The 4.27 GBuffer decode that feeds this recipe (all HARD, DeferredShadingCommon.ush /
ShadingCommon.ush):

* `GBufferA.rgb = WorldNormal * 0.5 + 0.5` world-space (`EncodeNormal` :121-124, used at
  :446); `GBufferA.a` = PerObjectGBufferData.
* `GBufferB = (Metallic, Specular, Roughness, ShadingModelID|SelectiveOutputMask)` (:459-462).
* `GBufferC.rgb = BaseColor` (sRGB-encoded storage — reading through the SRGB view yields
  linear, which is what RR wants); `GBufferC.a` = indirect irradiance × AO under
  ALLOW_STATIC_LIGHTING (:464-471).
* `DiffuseColor = BaseColor − BaseColor·Metallic` (:608);
  `SpecularColor(F0) = ComputeF0(Specular, BaseColor, Metallic) =
  lerp((0.08·Specular).xxx, BaseColor, Metallic)` (:601; ShadingCommon.ush:77-97).

### 2.5 Known failure modes (what "wrong guide buffers" looks like)

* Sky/background left uncleared in specular albedo — guide says default sky albedo to
  (0.5, 0.5, 0.5) (PDF §3.4.2). [HARD]
* Near-zero albedo guides make RR hallucinate brightness — Remix clamps ("near 0 albedos
  cause RR to think the pixel is supposed to be very brightly lit"), RTXPT floors
  diffuse+specular at 0.05. [HARD]
* Silent acceptance is the default: NGX validates none of the semantics. The sanctioned
  diagnostic is the dev-DLL debug overlay (CTRL+ALT+F12 cycles every guide buffer;
  production DLL has none) (PDF §8.1). Our on-target verification plan must budget for
  the dev DLL. [HARD]
* Guides describing the wrong surface (mirrors) → cross-hatching / ghosting in
  reflections. [SOFT, first-hand integrator report]
* DoF or dithering before RR breaks it; transparency composited into the color input gets
  "denoised" — the Transparency Overlay guide exists for that. [HARD]

### 2.6 Can RR run on a non-ray-traced, composited frame at all?

* Mechanically yes: "When DLSS-RR is enabled, it effectively overrides DLSS-SR execution"
  (PDF §3.2); placement is identical to SR (before tonemap, at the TAAU point), and the UE
  plugin feeds it a raster deferred frame's ordinary scene color. RR is created with
  `NVSDK_NGX_DLSS_Denoise_Mode_DLUnified` via `NGX_D3D12_CREATE_DLSSD_EXT` and needs
  driver ≥ 535 and `nvngx_dlssd.dll`. [HARD]
* Distributionally unknown: the network is trained on noisy RT/PT signals with
  independence assumptions (PDF §3.5); every shipped deployment sits on an actual
  ray/path tracer or on UE5's Lumen. "DLSS-RR is a little picky, and expects guide
  buffers to be calculated in the same way it was trained on." [SOFT] Whether RR's output
  on Stray's composited raster lighting beats DLSS-SR + UE's own denoisers is
  **UNCONFIRMED and unanswerable off-target** — this is the experiment, and the measured
  3.4× shimmer gap with the SSD suppressed (CLAUDE.md, denoiser-suppression experiment) is
  its quantified target.
* Note what Stray's "noise" actually is: with the SSD suppressed, the noisy signal
  reaching scene colour is **screen-space** traced SSR/SSGI — stochastic per-pixel rays,
  which is the signal class RR denoises, but produced by a tracer NVIDIA never trained
  against. [UNCONFIRMED as to quality]

---

## 3. Q3 — Precedent: has anyone injected RR (or replaced a denoiser) from outside?

Full survey with URLs in §6. The short version — **no one has done guide-buffer
scavenging; three classes of "RR mod" exist, none of which is this**:

1. **CVar toggles of a shipped engine integration.** STALKER 2 / Silent Hill 2R /
   Oblivion Remastered "RR mods" are `r.NGX.DLSS.denoisermode=1` ini/UE4SS tweaks plus
   dropping `nvngx_dlssd.dll` next to the game's own NVIDIA plugin, which supplies all
   guide buffers in-engine. Stray has no dormant integration to switch on. [HARD]
2. **Pass-through proxies.** OptiScaler supports "DLSS-D" only by intercepting a game's
   own `NVSDK_NGX_Feature_RayReconstruction` calls and forwarding the game's own
   parameter block; its `DLSSDFeature.cpp` never creates or reads a guide buffer.
   PureDark has shipped SR/FG/(leaked-)NR mods, never RR; NR is telling — it needs only
   color+MV, exactly the inputs an upscaler mod already has. The line no modder has
   crossed is precisely the guide-buffer line. [HARD for OptiScaler source and PureDark's
   own catalog; SOFT for the interpretation]
3. **Whole-renderer replacement.** RTX Remix's RR inputs are its own path tracer's
   outputs (`rtx_pathtracer_gbuffer.cpp` writes dedicated `*_DLSSRR` outputs); the game
   contributes intercepted fixed-function draw state only. Gilcher's PTGI and RTGL1
   likewise own their lighting and use their own denoisers. Not a capture precedent. [HARD]

The two adjacent techniques that ARE precedented, and that §4 builds on:

* **Consumer-pass capture (Luma pattern).** Luma identifies game passes by shader hash
  and captures depth/MV from known SRV slots of a known consumer pass (Prey: depth from
  `ps_shader_resources[16]` at the TAA pass; MV densified by its own CS) — exactly this
  project's existing architecture, extended to more inputs. DX11 only in Luma; RenoDX
  proves the hash-and-intercept pattern on D3D12. Nobody has captured normals/roughness/
  albedo this way yet. [HARD for the pattern; SOFT for per-game details]
* **Engine-pool interception (UEVR pattern).** UEVR inline-hooks
  `FRenderTargetPool::FindFreeElement` in shipped UE4/UE5 binaries and keys captures by
  the pool name string (`SceneDepthZ`), holding the engine's own
  `TRefCountPtr<IPooledRenderTarget>` for lifetime — proven across hundreds of UE titles
  on D3D12, at the cost of praydog's full heuristic apparatus (string-ref scanning,
  structure analysis, per-engine-generation signatures) and unproven under Proton. The
  pool names (`TEXT("GBufferA")` at SceneRenderTargets.cpp:1125) are exactly the
  identifiers we would key on. [HARD]
* **Copy discipline (ReShade Generic Depth).** On D3D12/Vulkan it always copies the
  captured buffer to its own texture "to circumvent problems in case application makes
  use of resource aliasing", with delayed destruction (~50 frames) for in-flight safety.
  Matches our keep-alive ring philosophy. [HARD]

Nothing anywhere injects NRD or any external denoiser into a shipped game. [UNCONFIRMED-
negative: absence after broad search]

---

## 4. Q4 — Strategy evaluation and recommendation

### 4.1 The options

| | Strategy | Verdict |
|---|---|---|
| S1 | **Writer-side: corrected base-pass signature (velocity-free), consume at TAA time** | **Primary.** §1.5's format tuple + draw dominance identifies the 6-RTV sets already observed; §1.1 guarantees the resources are alive and final at our existing hook; zero new interception machinery. |
| S2 | **Reader-side: capture at SSD / lighting / composite passes' SRV binds** | **Corroboration + fallback.** Resources are guaranteed alive and *semantically named by register* at the consumer — but per-pass availability is conditional (§1.6: the SSD gates are config-dependent and already contradict stock; deferred lights vary per scene), and register maps need offline derivation per permutation. |
| S3 | UEVR-style engine memory access (`FindFreeElement` hook / `FSceneRenderTargets` scan) | Last resort. Precedented on D3D12, but pattern-scanning a licensee binary, unproven under Proton, untestable in CI, and against the project's loud-failure ethos. Keep as the documented escape hatch if S1+S2 both fail on target. |
| S4 | Continue the velocity-anchored MRT hunt with corrected conditions | Dead. §1.3-1.4: the anchor is wrong on stock-source grounds alone, and the live game contradicts the stock prediction it rested on. |

### 4.2 Why S1 over S2 as primary

* **Availability**: the base pass runs every frame the world renders; the SSD family's
  dispatch conditions are config- and scene-dependent and already deviate from stock in
  Stray (§1.6); per-light shaders vary with the scene. The one S2 pass that is *probably*
  per-frame (reflection-environment / sky lighting composite) still needs its permutation
  census taken before it can be trusted.
* **Completeness**: SSD binds at most A+B (+ its own Prev copies); GBufferC (albedo) —
  which RR needs twice over — is only bound by lighting/composite passes. S1 gets A, B, C
  in one identification.
* **Machinery**: S1 reuses `gbuffer_finder` (running today) with a classifier change that
  is pure logic — CI-testable in `tests/test_gbuffer_classify.cpp` — plus the existing
  TAA-hook plumbing. S2 needs new per-pass bind resolution plus offline register-map
  tooling before first light.
* **The suppression concern resolves itself**: even in the final RR configuration where we
  suppress the SSD dispatches, ReShade still delivers `push_descriptors` /
  `bind_descriptor_tables` beforehand, and our own `on_dispatch` handler reads bindings
  *before* returning `true` to skip (src/addon.cpp:594-596 already orders it this way for
  the denoiser cross-check). A suppressed pass's bindings stay observable. But S2-as-primary
  would still tie identification to passes whose *presence* is conditional — the reason it
  is the fallback, not the plan.

### 4.3 The S2 cross-check (do build this part)

Reader-side evidence is the cheapest possible correctness proof for the S1 identification
and costs almost nothing given `gbuffer_finder`'s existing denoiser tap:

* Extend the tap from the single hash to the full known family (all nine
  `FSSDTemporalAccumulationCS` hashes; add `FSSDSpatialAccumulationCS`, `FSSDInjestCS`,
  `FSSDCompressMetadataCS` — shader files named at ScreenSpaceDenoise.cpp:1107-1111 —
  plus `FDeferredLightPS`, `FDiffuseIndirectCompositePS`,
  `FReflectionEnvironmentSkyLightingPS`, all global shaders extractable by
  `tools/shaderlib_extract.py`).
* A resource that S1 calls GBufferA appearing among an SSD/lighting dispatch's SRVs — and
  never among the ones it calls B/C — corroborates each slot's identity by *consumption
  semantics*, resolving the B/C order question (§1.5) without a screenshot round-trip.
* For the UB-flavor consumers (`FDeferredLightPS`), the offline resource-table parse maps
  `SceneTexturesStruct` members to `t`-registers per permutation, turning "this resource
  was bound at t6" into "this resource is GBufferCTexture" with no heuristics at all.

### 4.4 The recommended pipeline, concretely, for this codebase

**Phase RR-A — identification (log-only, one observation run):**

1. `src/core/gbuffer_classify.cpp`: replace the velocity anchor with the §1.5 signature
   (velocity optional at slot 4; scene-colour formats already accept `R11G11B10F`; add
   sRGB variants of RGBA8 to `is_rgba8_class`'s recorder mapping if `to_tex_format`
   currently collapses them to `unknown` — verify in `frame_state.cpp`). Correct the
   header's provenance comments per §1.2-1.3. CI: extend `tests/test_gbuffer_classify.cpp`
   with the observed menu tuple, the stock 7-RTV tuple, and the look-alike rejections.
2. `src/gbuffer_finder.cpp`: log the full slot table of the **first** classified candidate
   immediately (keep the 30-frame stability gate for the *stable* report); add narrow-bind
   velocity sightings and the 7-bucket histogram to every FAILED/stable block (mostly
   already present).
3. `tools/shaderlib_extract.py`: emit the §4.3 hash families + per-permutation SRV
   censuses (declared `dcl_resource` formats) and, for UB-flavor shaders, the
   `SceneTexturesStruct` member→register maps from the already-parsed resource table.
   Also decode which `ESignalProcessing` each cooked SSD permutation is (settles §1.6's
   "which SSD signal runs" and names what RR will replace).
4. One observation run answers: does gameplay show the 6-RTV base pass steadily? do the
   SSD/lighting SRVs corroborate the slots? what was the once-at-load 7-RTV bind?

**Phase RR-B — resolve pass (still SR output, guides validated):**

5. New `shaders/gbuffer_resolve.hlsl` + `src/gbuffer_resolve.cpp`, cloned from the
   `mv_resolve` pattern (build-time DXBC, ring-buffered descriptors, per-frame descriptor
   rewrite, liveness-checked inputs, AddRef keep-alive until GPU-done, retire-on-resize —
   all §5-hardened machinery). Inputs: GBufferA/B/C (from the S1 identification, consumed
   the same frame at the TAA hook) + the depth SRV we already capture. Outputs, per the
   UE-plugin recipe (§2.4): normals+roughness `RGBA16_FLOAT` (world-space signed, decode
   `N*2-1`; roughness in .w AND a standalone R roughness — costs one channel, keeps both
   modes testable), diffuse albedo and specular albedo `R11G11B10_FLOAT`
   (`DiffuseColor = BaseColor·(1−Metallic)`; `EnvBRDFApprox2(F0, Roughness², NoV·0.75)`
   with the UE NoV remap; F0 = `lerp(0.08·Specular, BaseColor, Metallic)`); sky/no-geometry
   pixels (far-plane depth) get albedo 0.5 and the albedo floor of 0.05 (§2.5). Every
   formula is a pure function — unit-test the encode/decode round trips in CI against the
   §2.4 constants.
6. Wire the resolve into the TAA hook behind `[STRAYDLSS] GBufferResolve=1`, output to
   dumpable textures (`rawdump2png.py` channel) — guides verified visually on target
   before RR exists.

**Phase RR-C — DLSS-D:**

7. `ngx_backend`: add the DLSSD create/eval path (`NGX_D3D12_CREATE_DLSSD_EXT` /
   `NGX_D3D12_EVALUATE_DLSSD_EXT`, `InDenoiseMode = DLUnified`), same native-device,
   state-restore and keep-alive discipline as SR. Feature flags as SR minus AutoExposure
   (RR ignores it; Remix masks it off — §2.1); pass `InPreExposure` from row 135.y.
   Depth: start with our existing HW reversed-Z + `DepthInverted` + `InUseHWDepth = HW`;
   if RR misbehaves, the UE plugin's linear-depth substitution is the documented
   alternative (emit linear depth from the resolve pass — one extra output). Presets: use
   Default (guide: "we recommend games stick to Preset_Default"); D/E are the named
   alternates. Raise `r.TemporalAASamples` ≥ 32 for RR (PDF §3.6) alongside the §4 SR
   config. CI fetches `nvngx_dlssd.dll` exactly as it does `nvngx_dlss.dll` (same
   redistribution terms).
8. **RR-0 experiment**: RR replaces the TAA dispatch exactly as SR does today, UE's
   denoisers untouched. Success = image ≥ SR quality → guides and plumbing are right.
   **RR-1 experiment**: add `DryRunHash`-suppression of the SSD family and compare
   temporal stability with the alternating harness against the measured 3.4× gap.
   Keep the stages separate — RR-0 failures are plumbing, RR-1 failures are signal.

### 4.5 If S1's identification does not survive the observation run

Fallback ladder, in order: (a) S2-primary — capture A/B/C pointers at whichever §4.3
consumer pass fired most recently, copy immediately (`CopyTextureRegion` on the same
command list, Generic-Depth discipline) instead of same-frame pointer consumption, since
consumer-time capture has no guaranteed adjacency to the TAA dispatch; (b) S3 — the UEVR
`FindFreeElement` hook keyed on the stock pool-name strings, accepting the pattern-scan
maintenance cost. Both are precedented; neither is needed unless the run says so.

### 4.6 Cost note

The resolve pass reads ~1920×1080 × (4+4+4+8) B ≈ 41 MB and writes ~29 MB per frame —
same order as `mv_resolve`; RR itself costs 1.11 ms on the 4090 at 1440p Performance
(PDF table). Not a risk. [HARD for the RR figure]

---

## 5. Open risks, honestly

1. **RR quality on a raster/composited signal is unproven anywhere** (§2.6). Every
   precedent feeds RR from a real ray/path tracer. RR-0/RR-1 staging bounds the cost of a
   negative answer; the DLSS on-screen indicator and the dev-DLL guide-buffer overlay
   (§2.5) are the diagnostics. [UNCONFIRMED]
2. **DLSSD under vkd3d-proton/DXVK-NVAPI is unverified on our stack.** SR's cubin path is
   proven on this machine; RR uses the same NGX transport but a different feature DLL and
   larger models. Community reports of RR working under Proton exist for native-RR games
   [SOFT], but our first RR-C step must be a create/eval probe long before capture work
   depends on it.
3. **The 6-vs-7 MRT anomaly is unexplained** (§1.4). The recommendation deliberately does
   not depend on resolving it — the corrected signature identifies either shape — but an
   unexplained stock-vs-live delta means other licensee deltas may exist (GBuffer format
   or slot-order edits would surface as classifier `unknown`s and must fail loudly, per
   the existing gbuffer_classify design).
4. **B/C order rests on stock slot order until §4.3's consumption cross-check confirms
   it.** Swapped B/C poisons *both* albedos and roughness — a wrong-but-plausible image.
   Do not ship RR before the cross-check or a visual guide-dump confirms it. [HARD risk,
   mitigations in-plan]
5. **GBufferC.a is not AO under Stray's config** — with static lighting allowed it stores
   pre-multiplied indirect irradiance (§2.4). Nothing in the recipe uses .a today; noted
   so nobody "improves" the resolve by reading it.
6. **The SSD passes we plan to suppress in RR-1 are not fully mapped** — nine temporal
   hashes are known; the spatial/injest/compress stages are enumerable offline but not yet
   enumerated, and which signal (SSR vs SSGI) they carry in Stray is open (§1.6).
   Suppressing only part of a denoiser chain may be worse than none.
7. **Screen-percentage interaction**: RR has no dynamic-resolution support (PDF §3.3) and
   the §4 SR plan changes render resolution via `r.ScreenPercentage`; every resolution
   change is a full DLSSD feature recreate (same rule as SR, CLAUDE.md §2.1).
8. **Menu census interpretation** ("the 6-RTV sets are the base pass") is inference from
   formats and stock source, not yet a measurement — the RR-A observation run's slot table
   is what hardens it. Until then it is the strongest available reading of HARD data, not
   itself HARD.

---

## 6. Source appendix

**Engine (UE 4.27.2 via the AlexMercer-MA/UnrealEngine-4.27 mirror, Build.version
verified 4.27.2 / CL 17155196):**
`Renderer/Private/PostProcess/SceneRenderTargets.cpp` (:734-779, :1010-1024, :1099-1176,
:1209-1218, :2286), `.h` (:475); `Renderer/Private/DeferredShadingRenderer.cpp`
(:1782-1803, :2078-2083, :2180, :2215, :2256, :2632, :2644-2654);
`Renderer/Private/VelocityRendering.cpp` (:26-32, :275-279, :305-315, :354-358, :371-391,
:395); `Renderer/Private/BasePassRendering.cpp` (:845, :878-891, :944-950, :978);
`Renderer/Private/SingleLayerWaterRendering.cpp` (:711);
`Renderer/Private/ScreenSpaceDenoise.cpp` (:678-690, :1025-1090, :1107-1111, :1892-1901);
`Renderer/Private/SceneTextureParameters.{h,cpp}`; `Renderer/Public/SceneRenderTargetParameters.h`
(:10-33); `Renderer/Private/LightRendering.cpp` (:430-460, :1186-1202);
`Renderer/Private/IndirectLightRendering.cpp` (:31-34, :52-93, :126-233, :769-771);
`Renderer/Private/ScreenSpaceRayTracing.cpp` (:47, :62, :75-95, :136-144);
`RenderCore/Public/RenderUtils.h` (:552-556); `RenderCore/Private/RenderUtils.cpp`
(:1200-1204, :1230-1330); `RenderCore/Private/RenderTargetPool.cpp` (:590, :1014-1094);
`Core/Private/Misc/ConfigCacheIni.cpp` (:4664-4669); `Launch/Private/LaunchEngineLoop.cpp`
(:2251, :2601-2603); `Developer/TargetPlatform/Private/TargetPlatformBase.cpp` (:28-32);
`D3D12RHI/Private/D3D12RHIPrivate.h` (:94); shaders `DeferredShadingCommon.ush` (:121-124,
:419-480, :601-614), `ShadingCommon.ush` (:77-97), `BasePassPixelShader.usf` (:1461,
:1491-1498), `ScreenSpaceDenoise/SSDTemporalAccumulation.usf` (:467), `SSDCommon.ush`
(:236), `SSDMetadata.ush` (:125-135).

**Stray's own config (HARD, `docs/game-config/`):** `Hk_project_Config_DefaultEngine.ini`
:38 (`r.AllowStaticLighting=True`), :58 (`r.ClearSceneMethod=1`), :59
(`r.BasePassOutputsVelocity=True`), :85 (`r.SelectiveBasePassOutputs=True`), :88
(`r.RayTracing=True`); `Hk_project_Config_Windows_WindowsEngine.ini` :69
(`r.SceneColorFormat=3`), :77 (`r.SSGI.Quality=2`).

**NVIDIA primary sources:**
DLSS-RR Integration Guide PDF (NVIDIA/DLSS repo, `doc/`, SWE-DLSS-001-PGRF Dec 2025):
<https://raw.githubusercontent.com/NVIDIA/DLSS/main/doc/DLSS-RR%20Integration%20Guide.pdf>;
Streamline ProgrammingGuideDLSS_RR.md v2.12.0:
<https://raw.githubusercontent.com/NVIDIAGameWorks/Streamline/main/docs/ProgrammingGuideDLSS_RR.md>;
vendored headers `third_party/ngx/include/nvsdk_ngx_defs_dlssd.h` (:23-27, :57-75),
`nvsdk_ngx_helpers_dlssd.h` (:119-222).
UE plugin v3.7.3 source (public mirror):
<https://github.com/Adriwin06/Ultimate-CommonUI-Menu-System/tree/main/Plugins/DLSS> —
`DLSSUpscaler.cpp:578-589`, `GBufferResolve.usf`, `GBufferResolvePass.cpp`,
`NGXRHI.cpp:486`, `NGXD3D12RHI.cpp:355-370`.
dxvk-remix: `rtx_ngx_wrapper.cpp`, `rtx_ray_reconstruction.cpp`,
`rtx_pathtracer_gbuffer.cpp`, `shaders/rtx/utility/brdf.slangh:676-705`,
`prepare_ray_reconstruction.comp.slang` (<https://github.com/NVIDIAGameWorks/dxvk-remix>).
RTXPT: `PostProcess.hlsl`, `StandardBSDF.hlsli:93-121`, `Microfacet.hlsli:282`,
`RenderTargets.cpp:186-199`, `Sample.cpp:1761,2680` (<https://github.com/NVIDIA-RTX/RTXPT>).

**Precedent survey:**
praydog UEVR write-up <https://praydog.com/reverse-engineering/2023/07/03/uevr.html> and
`src/mods/vr/RenderTargetPoolHook.cpp`; OptiScaler `Features.md`, `Changelog.md` (v0.6.1),
`OptiScaler/upscalers/dlssd/DLSSDFeature.cpp` (<https://github.com/optiscaler/OptiScaler>);
Luma Framework <https://github.com/Filoppi/Luma-Framework>; RenoDX
<https://github.com/clshortfuse/renodx>; REST
<https://github.com/4lex4nder/ReshadeEffectShaderToggler>; ReShade Generic Depth
`examples/09-depth/generic_depth_addon.cpp` (<https://github.com/crosire/reshade>);
RTX Remix <https://github.com/NVIDIAGameWorks/rtx-remix>; STALKER 2 RR cvar mod
<https://www.nexusmods.com/stalker2heartofchornobyl/mods/65>; Oblivion Remastered RR mod
<https://www.nexusmods.com/oblivionremastered/mods/5572>; PureDark catalog
<https://www.patreon.com/PureDark/about>, <https://github.com/PureDark/Skyrim-Upscaler>;
Marty McFly depth-only confirmation <https://www.martysmods.com/depth-buffer/>;
Gilcher PTGI interviews (wccftech Q&A; Digital Foundry, 2023); garamond13/ReShade-shaders
(DLSS-D headers vendored, never called) <https://github.com/garamond13/ReShade-shaders>;
Bevy Solari RR notes <https://jms55.github.io/posts/2026-04-12-solari-bevy-0-19/>;
NVIDIA UE plugin 8.1.0 release notes (RR support; NvRTX 5.4 extra buffers) [SOFT mirror].

**Live measurements referenced (this project, 2026-08-31):** the gbuffer_finder menu /
save-load / gameplay census; the SSD-suppression 3.4× shimmer result; the
`0x1708ec956099e259` = FSSDTemporalAccumulationCS perm #5 offline naming
(`tools/shaderlib_extract.py --find-hash`); TAA input formats (CLAUDE.md §2.3-2.5).
