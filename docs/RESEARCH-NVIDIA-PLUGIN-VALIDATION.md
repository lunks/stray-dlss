# Validating our DLSS integration against NVIDIA's own UE 4.27 plugin

Validation report, 2026-09-04. Every claim this project makes about DLSS conventions —
motion-vector sign and scale, jitter, pre-exposure, create flags, the Ray Reconstruction
G-buffer recipe, the frame-generation camera constants — was reconstructed from public
documentation, third-party mirrors, leaked binaries and reasoning. **We now have read
access to NVIDIA's own tree**, so each of those can be checked against the vendor's
shipping code for the exact engine version Stray uses.

Provenance labels follow CLAUDE.md §0.5: **HARD** = read verbatim from a primary source;
**SOFT** = credible secondary source; **UNCONFIRMED** = inference.

## Source

`NvRTX/UnrealEngine`, branch **`dlss3/sl2-4.27-dlss-plugin`**, head
`32c3e4d5e0ee9ea7b792d1d09bcc2549917ad252` — NVIDIA's own fork of `EpicGames/UnrealEngine`
at **UE 4.27**, i.e. *the same engine major/minor Stray ships* (CLAUDE.md §2.1). Two
plugins matter:

* `Engine/Plugins/Runtime/Nvidia/DLSS/` — DLSS Super Resolution and Ray Reconstruction.
* `Engine/Plugins/Runtime/Nvidia/Streamline/` — DLSS Frame Generation, via Streamline 2.x.

The repository is **private and must never be quoted at length or copied**. Every finding
below is prose plus a `path:line` citation; where a constant or a one-line convention is
the whole point, at most a fragment is quoted to pin it. No implementation is reproduced.

Two provenance upgrades this source enables, both worth stating once:

* Citations previously carried as **HARD-via-mirror** (`AlexMercer-MA/UnrealEngine-4.27`
  for the engine, `moumee/ProjectS` for the plugin) are now **HARD**, first-party, at the
  right engine version.
* Where the mirror we cited was the **UE5** plugin (v8.3.0), the 4.27 vendor tree is a
  *different and older* plugin (3.7.x-era SDK). Agreement across both is stronger evidence
  than either alone; disagreement is noted explicitly below.

---

## 0. Verdict up front — the DIFFERS and WE-ARE-WRONG rows

Ranked by how much each would move the image on this title.

### 0.1 WE ARE WRONG (or at least: unvalidated where a validated answer exists)

| # | Finding | Where |
|---|---|---|
| **W1** | **RR: we feed hardware (reversed-Z device) depth with `DepthInverted` set; NVIDIA feeds LINEAR world-unit depth and DELIBERATELY CLEARS `DepthInverted` for RR.** The clear is an explicit `if (DenoiserMode == Off)` guard, so it is a decision, not an omission. Their G-buffer resolve emits a fifth render target holding `FGBufferData.Depth` (= `CalcSceneDepth`, i.e. `ConvertFromDeviceZ`), and that texture replaces the scene depth as the RR depth input. Our RR path is not merely differently configured — it is an *untested* corner of the parameter space, and RR is the one feature where the depth guide drives reprojection of a denoised signal. | §1.1 |
| **W2** | **RR: our G-buffer resolve invents two behaviours NVIDIA's does not have** — an unlit/sky special case (albedo 0.5, roughness forced to 1) and a 0.05 floor on both albedos. NVIDIA writes `GBuffer.DiffuseColor` and the `EnvBRDFApproxRTG` result unmodified, with no shading-model branch and no clamp. A floor of 0.05 on *specular* albedo in particular is not small: physically-correct dielectric F0 is ~0.04, so our floor **raises every dielectric surface's specular guide above its true value**. | §1.2 |
| **W3** | **SR: `InFrameTimeDeltaInMsec` is never set on our SR evaluate** (memzero leaves it 0.0). NVIDIA sets it on every evaluate, SR and RR alike, from `View.Family->DeltaWorldTime * 1000`. Our RR path sets it; SR does not. The header's own comment says it "helps in determining the amount to denoise or anti-alias based on the speed of the object from motion vector magnitudes and fps". | §3.1 |
| **W4** | **CLAUDE.md's citation `DLSSUpscalerPrivate.h` for `PreExposure(View.PreExposure)` does not hold in the 4.27 vendor tree** — that file there is a bare `DECLARE_LOG_CATEGORY_EXTERN`, nothing else. The *claim* is correct and is now HARD from a better anchor; the file reference was inherited from the UE5 mirror and reads as first-party when it is not. | §5.2 |

### 0.2 DIFFERS (deliberate, defensible, or latent — but not what the vendor ships)

| # | Finding | Where |
|---|---|---|
| **D1** | **NVIDIA's DEFAULT for DLSS-SR is DILATED, OUTPUT-RESOLUTION motion vectors** (`r.NGX.DLSS.DilateMotionVectors` defaults to **1**), which clears `MVLowRes`. We ship the non-dilated render-resolution path with `MVLowRes` set. Ours is legal and is exactly what NVIDIA *forces* under RR — but their own cvar help says the dilated path "can help with improving image quality of thin details", which is the class of content (fur, cables, railings) this project has repeatedly chased. This is the single largest *actionable* divergence on the SR path. | §2.1 |
| **D2** | **MV validity test: NVIDIA uses `all(EncodedVelocity.xy > 0)`; we use `encoded.x > 0.0f`.** Ours matches UE's own TAA convention; NVIDIA's is strictly narrower. They differ only where the encoded green channel is exactly 0, i.e. NDC y at or below the UNORM low rail (`V.y <= -2.004`). Live impact ~0 on this title (our own census measures UNORM clamping at 0.0129% of engine-written pixels), but the doc claim in CLAUDE.md §2.5 that this is *NVIDIA's* test is wrong. | §2.2 |
| **D3** | **Subrect bases: we send 0 for colour, depth and output; NVIDIA sends `SrcRect.Min` for colour and depth and `DestRect.Min` for output.** Latent on this title — Stray's `View.ViewRectMin` reads (0,0) — but it is exactly the assumption CLAUDE.md §5 tells us not to make ("read it, do not assume it"), and our own `mv_resolve.hlsl` already *does* read it for indexing. The two halves disagree with each other. | §3.2 |
| **D4** | **MV half-pixel: the 4.27 plugin samples the pixel CORNER, we sample the CENTRE.** `SvPositionToScreenPosition(float4(PixelPos.xy, 0, 1))` with an integer `PixelPos` yields `(p/size - 0.5)*2`; ours yields `((p+0.5)/size - 0.5)*2`. Our shader's own comment already claims the newer UE5 plugin moved to the centre and that we match it — this confirms the 4.27 half of that story from the vendor tree. Keep the centre. | §2.3 |
| **D5** | **RR: we send `pInWorldToViewMatrix` / `pInViewToClipMatrix`; NVIDIA sends neither** (both stay null through `GetCommonEvalParams` and the RR block). Extra input the vendor does not exercise. | §1.3 |
| **D6** | **We omit `NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality`** — NVIDIA sets all **six** preset hints, we set five. Inert on this title (`src/core/dlss_quality.cpp` never selects UltraQuality) but CLAUDE.md §5's "all five hint keys" is a miscount. We also never set `NVSDK_NGX_Parameter_FreeMemOnReleaseFeature`, which NVIDIA sets from a cvar defaulting to 1. | §3.3 |
| **D7** | **RR: our diffuse albedo ignores UE's subsurface-profile adjustment; NVIDIA's inherits it.** `DecodeGBufferData` runs `AdjustBaseColorAndSpecularColorForSubsurfaceProfileLighting` for `MSM_SubsurfaceProfile` — the cat's fur shading model (CLAUDE.md §2.3.1) — which under checkerboard SSS sets `BaseColor = bChecker` and `SpecularColor *= !bChecker`. NVIDIA therefore hands RR a **checkerboarded** albedo guide on those pixels. Ours is arguably the better guide; it is nonetheless not what the reference does, and which is right is UNCONFIRMED. | §1.4 |
| **D9** | **FG: we send the jitter-free `ViewToClipNoAA`; NVIDIA sends the JITTERED `ViewToClip` and its exact inverse** — while both also send `jitterOffset` separately. The only FG constant where we knowingly diverge from the vendor, and the only one this report found. | §4.5 |
| **D8** | **State restore around the evaluate.** NVIDIA does not save/restore D3D12 state; it invalidates UE's own `FD3D12StateCache` afterwards (`ForceSetComputeRootSignature` + `SetCurrentCommandList`). We cannot reach that cache, so our full manual restore is a *necessary* difference, not a defect — but it means "NVIDIA doesn't restore state" is not evidence that restoring is unnecessary for us. | §3.4 |

### 0.3 The large agreements, stated plainly

Everything else checked **AGREES**, and several previously `[derived]` or mirror-sourced
claims are now first-party HARD at the right engine version:

* The whole `EnvBRDFApprox2` / `EnvBRDFApproxRTG` listing, coefficient for coefficient,
  including the `saturate(SpecularColor.g * 50)` bias hack and the two `max(0, …)` clamps.
* The NoV remap `x = NoV²; NoV = x/(x + (1-NoV)²)` and the `NoV * 0.75f` scale — including
  the otherwise-unexplained 0.75.
* `alpha = Square(Roughness)`; `F0 = ComputeF0(Specular, BaseColor, Metallic) =
  lerp(0.08·Specular, BaseColor, Metallic)`; `DiffuseColor = BaseColor·(1-Metallic)`;
  world-space normal, normalised, roughness in `.w`; a separate roughness guide with
  `RoughnessMode = Unpacked`; all guides at render resolution with subrect base 0.
* The motion-vector output contract end to end: `R16G16_FLOAT` (`PF_G16R16F`),
  render-resolution pixels, `[0,0]` upper-left, pointing backward, `InMVScale = (1,1)`.
  The final expression is `-(V · (0.5, -0.5) · ViewSize)` in both trees.
* **NVIDIA also emits (effectively) zero when `PrevClipPos.w <= 0`** — CLAUDE.md carried
  this as an open question; it is now closed. See §2.4 for the exact — and slightly
  peculiar — form of their fallback.
* Jitter: `View.TemporalAAJitter`-derived `TemporalJitterPixels` passed **unmodified**, no
  sign flip, no scale. Confirmed twice over: `TemporalAAParams.zw` *is*
  `TemporalJitterPixels` (`SceneRendering.cpp:1517-1521`), and the plugin passes
  `View.TemporalJitterPixels` straight into `InJitterOffsetX/Y`.
* `InPreExposure = View.PreExposure` (not its reciprocal); the exposure **texture** is the
  only thing nulled under auto-exposure; `InExposureScale` is never set by the plugin;
  auto-exposure defaults ON.
* SR create flags: `IsHDR` always; `DepthInverted` from `ERHIZBuffer::IsInverted`;
  `MVLowRes` iff not dilated; `AutoExposure` from the cvar. Our `0x4B` is the correct
  combination *for our non-dilated configuration*.
* FG: `cameraNear = 0.01f`, `cameraFar = 75000.0f` as a shipped constant pair with help
  text saying they need not match the engine; `mvecScale = 1/ViewRect.Width,
  1/ViewRect.Height`; matrices handed over **row for row with no transpose**;
  `cameraMotionIncluded = true`, `motionVectors3D = false`;
  `minRelativeLinearDepthObjectSeparation = 40.0f`.

**One frame-generation constant is contradicted** — the projection matrix pair (§4.5). Every
other value in that block, all of it UNCONFIRMED before this report, is now HARD, except
`ColorBuffersHDR` (§4.3) and the camera position (§4.4), which this tree cannot settle.

---

## 1. Ray Reconstruction / the G-buffer resolve

Reference: `Engine/Plugins/Runtime/Nvidia/DLSS/Shaders/Private/GBufferResolve.usf`,
`.../Source/DLSSUtility/Private/GBufferResolvePass.cpp`,
`.../Source/DLSS/Private/DLSSUpscaler.cpp`, `.../Source/NGXRHI/Private/NGXRHI.cpp`,
`.../Source/NGXD3D12RHI/Private/NGXD3D12RHI.cpp`.
Ours: `shaders/gbuffer_resolve.hlsl`, `src/core/envbrdf.{hpp,cpp}`, `src/gbuffer_resolve.cpp`,
`src/ngx_backend.cpp`.

| Claim | Ours | NVIDIA's | Verdict |
|---|---|---|---|
| Specular-albedo approximation | `EnvBRDFApprox2`, `shaders/gbuffer_resolve.hlsl:67-96` + `src/core/envbrdf.cpp` | `EnvBRDFApproxRTG`, `GBufferResolve.usf:27`; identical M1-M4 coefficients, identical `bias *= saturate(SpecularColor.g * 50)`, identical `mad(F0, max(0,scale), max(0,bias))` | **AGREES** (HARD) |
| NoV remap | `x = nov²; nov = x/(x + (1-nov)²)` (`gbuffer_resolve.hlsl:161-163`) | `GBufferResolve.usf:97-98`, character for character | **AGREES** (HARD) |
| NoV scale into the approximation | `nov * kNovScale`, `kNovScale = 0.75f` (`:60`, `:167`) | `EnvBRDFApproxRTG(…, …, NoV * 0.75f)` — `GBufferResolve.usf:99` | **AGREES** (HARD) |
| Roughness → alpha | `roughness * roughness` (`:167`) | `Square(GBuffer.Roughness)` — `:99` | **AGREES** |
| F0 | `lerp(0.08·Specular, base, metallic)` (`:152`) | `GBuffer.SpecularColor` = `ComputeF0` (`ShadingCommon.ush:95-98`, `DielectricSpecularToF0 = 0.08f * Specular` at `:77-80`) | **AGREES** (HARD, engine source) |
| Diffuse colour | `base * (1 - metallic)` (`:151`) | `BaseColor - BaseColor*Metallic` — `DeferredShadingCommon.ush:608` | **AGREES** |
| Normal decode | `a.xyz*2-1`, renormalised (`:129-133`) | `DecodeNormal(N) = N*2-1` (`DeferredShadingCommon.ush:127-130`) then `normalize` under `bGetNormalizedNormal`, which `GetGBufferDataUint` defaults true | **AGREES**; our degenerate-input `+Z` fallback is an extra guard NVIDIA lacks |
| Normal output space | world space, roughness in `.w` | `OutNormal = float4(GBuffer.WorldNormal, GBuffer.Roughness)` — `:101` | **AGREES** |
| Roughness guide | separate texture, `RoughnessMode = Unpacked` (`src/ngx_backend.cpp:601`) | separate RT (`:102`); `InRoughnessMode` left 0 = `Unpacked` by memzero (`NGXRHI.cpp:488-497`) | **AGREES** |
| Shading-model id decode | `round(saturate(b.a)*255) & 0xF` (`gbuffer_resolve.hlsl:126`) | `DecodeShadingModelId` = `((uint)round(x*0xFF)) & SHADINGMODELID_MASK`, mask `0xF` (`DeferredShadingCommon.ush:293-296`, `ShadingCommon.ush:19`) | **AGREES** |
| Base-colour sRGB | hardware-decoding SRV, in-shader fallback (`:104-114`, `:150`) | `DecodeBaseColor` is the identity, with the comment "we use sRGB on the render target" (`DeferredShadingCommon.ush:182-186`) | **AGREES** |
| Guide extent / alignment | render resolution, top-left, subrect base 0 | `OutputExtent = View.ViewRect.Width/Height`, output viewport at the origin (`GBufferResolvePass.cpp:66`, `:120-122`); every RR subrect base set to 0 (`NGXD3D12RHI.cpp:346-357`) | **AGREES** |
| G-buffer indexing | `ViewRectMin + tid` (`gbuffer_resolve.hlsl:120`) | `SvPosition.xy + InputViewPort_ViewportMin` — `:84` | **AGREES** |
| **Depth guide** | device reversed-Z + `DepthInverted` + `InUseHWDepth = HW` | **linear** scene depth from the resolve pass; `DepthInverted` explicitly NOT set under RR | **WE ARE WRONG** — §1.1 |
| **Unlit / sky** | albedo 0.5, roughness forced 1 | no branch at all | **DIFFERS** — §1.2 |
| **Albedo floor 0.05** | both albedos floored | no floor | **DIFFERS / likely WRONG** — §1.2 |
| Subsurface-profile adjustment | not applied | applied via `DecodeGBufferData` | **DIFFERS** — §1.4 |
| RR matrices | sent | not sent | **DIFFERS** — §1.3 |
| Albedo format | `RGBA8_UNORM` (UAV) | `PF_FloatR11G11B10` (RTV), with NVIDIA's own `// TODO PF_B8G8R8A8 enough?` | **DIFFERS**, and their TODO says they consider 8-bit plausible |
| Roughness format | `R16F` | `PF_R32_FLOAT` | **DIFFERS**, cosmetic |
| Perf-quality value at create | `dlss_quality_for(...)` from the real ratio (`src/ngx_backend.cpp:824`) | `ToNGXQuality(DLSSQualityMode)` — the real ratio's mode | **AGREES**. (Note the throwaway RR *probe* at `src/ngx_backend.cpp:607-610` does hardcode `MaxQuality`; that feature is created and immediately released, so it never renders.) |

### 1.1 The depth guide — the one to fix first

`NGXRHI.cpp:455-465` builds the create flags for **both** SR and RR from one function, and
line 460-461 guards the inverted-depth bit:

> `if (DenoiserMode == ENGXDLSSDenoiserMode::Off) DLSSFeatureFlags |= bool(ERHIZBuffer::IsInverted) ? …DepthInverted : 0;`

So under RR the flag is cleared, unconditionally. The matching half is
`DLSSUpscaler.cpp:511`, which after running the G-buffer resolve **overwrites the depth
input**:

> `DLSSParameters.SceneDepthInput = ResolvedGBuffer.LinearDepth;`

and `GBufferResolve.usf:103` fills that target with `GBuffer.Depth`, which
`DecodeGBufferData` sets to `SceneDepth` (`DeferredShadingCommon.ush:584`), itself
`CalcSceneDepth(PixelPos)` (`:674`) — linear, world units, **not** normalised, **not**
reversed. `InUseHWDepth` is left at `NVSDK_NGX_DLSS_Depth_Type_Linear` (0) by the memzero
at `NGXRHI.cpp:489`.

We ship the other corner: `src/ngx_backend.cpp:602`/`:835` set
`InUseHWDepth = NVSDK_NGX_DLSS_Depth_Type_HW`, `:616`/`:859` set `DepthInverted`, and
`evaluate_rr` passes the engine's own device depth. **That pairing is internally
self-consistent** — the enum exists, so the runtime accepts hardware depth — but it is a
combination NVIDIA does not ship or test in this plugin, and the DLSSD parameter block
validates nothing (CLAUDE.md §5, three times over). HARD on what NVIDIA does; **UNCONFIRMED
that the HW-depth path is equally good, and UNCONFIRMED that it is even wired in the
shipped runtime.**

Since our G-buffer resolve already samples nothing depth-related, adding a fifth output
holding `ConvertFromDeviceZ(deviceZ)` is a bounded change — `InvDeviceZToWorldZTransform`
is View row 65 (CLAUDE.md §2.6) — and would let the two configurations be A/B'd from one
build. **Not built here**: RR has no live guide source in this tree (CLAUDE.md §5, "RR IS
NOT WIRED"), so a change to it cannot be judged.

### 1.2 The sky case and the albedo floor

`GBufferResolve.usf:87-103` has no branch of any kind: the shader writes
`GBuffer.DiffuseColor`, the `EnvBRDFApproxRTG` result, the normal, the roughness and the
depth, whatever the shading model. Our `shaders/gbuffer_resolve.hlsl:136-145` substitutes
`(0.5, 0.5, 0.5)` for **both** albedos and forces roughness to 1 when the shading model is
`Unlit`, and `:170-171` floors both albedos at 0.05.

Both come from `docs/RESEARCH-RR-GBUFFER.md` §2.5, sourced to the RR Integration Guide PDF
and to RTXPT — **not** to this plugin. They are therefore not contradicted by the vendor
tree so much as *unsupported by it*, with one exception that is worth calling out on its
own: **flooring the SPECULAR albedo at 0.05 is above the physically-correct dielectric F0
of ~0.04.** Every non-metal in the frame gets a specular guide that is too bright, and the
guide is exactly what RR uses to separate lighting from albedo. The diffuse floor is
defensible (a black-albedo pixel really does make a denoiser hallucinate); the specular
floor, at that value, looks like an off-by-one-material-model.

**Recommendation, not applied:** put both behind config keys defaulting to NVIDIA's
behaviour (no branch, no floor) once RR has a guide source, so the deviation is a
measurement rather than a habit. Not done here because the code is unreachable at runtime
and an unreachable A/B is not a test.

### 1.3 Matrices

`GetCommonEvalParams` (`NGXD3D12RHI.cpp:193-230`) and the RR block (`:341-357`) never
touch `pInWorldToViewMatrix` or `pInViewToClipMatrix`; both stay null. Our
`src/ngx_backend.cpp` sets them whenever the View CB decoded. Harmless if ignored, but the
convention (row/column, jittered/NoAA, translated-world or not) is UNCONFIRMED and
untested by the vendor, so it is one more input that could be silently wrong. If RR ever
runs and looks off, **null them first** — it costs nothing and matches the reference.

### 1.4 The subsurface-profile checkerboard

`DecodeGBufferData` calls `AdjustBaseColorAndSpecularColorForSubsurfaceProfileLighting`
for any `UseSubsurfaceProfile` shading model (`DeferredShadingCommon.ush:601-608`), and
that function (`:513-532`) — when `View.bSubsurfacePostprocessEnabled` and, in
`SUBSURFACE_CHANNEL_MODE != 0`, `bCheckerboardSubsurfaceProfileRendering` — replaces
`BaseColor` with the checker bit and multiplies `SpecularColor` by its complement.
`DiffuseColor` is then derived from the *adjusted* BaseColor. NVIDIA's RR guide inherits
all of that.

Stray's cat uses `MSM_SubsurfaceProfile` (CLAUDE.md §2.3.1). So on the fur — the one
subject this project has spent a session on — NVIDIA's reference implementation would feed
RR a checkerboard albedo and we would not. **UNCONFIRMED which is better.** Recorded
because it is the kind of thing that reads as "our decode is wrong" when the reference is
inspected out of context: it is not; it is a deliberate divergence we made by not
reproducing an engine quirk.

---

## 2. Motion vectors

Reference: `DLSS/Shaders/Private/VelocityCombine.usf` and
`Streamline/Shaders/Private/VelocityCombine.usf` — **the non-dilated branches of the two
files are identical**, so SR and FG share one convention. Ours: `shaders/mv_resolve.hlsl`.

| Claim | Ours | NVIDIA's | Verdict |
|---|---|---|---|
| Output format | `R16G16_FLOAT` | `PF_G16R16F` (`VelocityCombinePass.cpp:86`) | **AGREES** |
| Output space | render-res pixels, Y-down, backward-pointing | `-(V · float2(0.5,-0.5) · View.ViewSizeAndInvSize.xy)` (`VelocityCombine.usf:195-197`) | **AGREES** |
| `InMVScale` | `(1,1)` | `FVector2D(1.0f, 1.0f)` (`DLSSUpscaler.cpp:673`) | **AGREES** |
| Velocity decode | `enc.xy * InvDiv - (32767/65535)*InvDiv` | `DecodeVelocityFromTexture` (engine `Common.ush`) | **AGREES** |
| Indexing | absolute buffer coords (`ViewRectMin + tid`) | `VelocityTexture[PixelPos]`, `DepthTexture[PixelPos]`, `PixelPos` including `Velocity_ViewportMin` (`:64`, `:167-168`) | **AGREES** |
| Reprojection | `mul(v, ClipToPrevClip)` row-vector | `mul(ClipPos, View.ClipToPrevClip)` (`:184`) | **AGREES** |
| Output subrect | top-left, base (0,0) | `InMVSubrectBase = 0,0` with the comment "The VelocityCombine pass puts the motion vectors into the top left corner" (`NGXD3D12RHI.cpp:213-215`) | **AGREES** |
| **Validity test** | `encoded.x > 0` | `all(EncodedVelocity.xy > 0)` (`:171`) | **DIFFERS** — §2.2 |
| **Half-pixel** | pixel centre | pixel corner (`:178`) | **DIFFERS** — §2.3 |
| **`w <= 0` fallback** | exact zero | `Velocity = EncodedVelocity.xy` (`:191`) — effectively zero | **AGREES in effect** — §2.4 |
| **Dilation / resolution** | never; render-res, `MVLowRes` set | **default ON**, output-res, `MVLowRes` cleared | **DIFFERS** — §2.1 |

### 2.1 Dilated high-resolution motion vectors are NVIDIA's default

`DLSSUpscaler.cpp:92-97`:

> `r.NGX.DLSS.DilateMotionVectors`, default **`1`** — *"1: pass dilated high resolution
> motion vectors into DLSS-SR. This can help with improving image quality of thin details.
> (default)"*

With it on, `AddVelocityCombinePass` allocates the combined-velocity texture at
`View.GetSecondaryViewRectSize()` — the **output** rect — and the `DILATE_MOTION_VECTORS`
branch runs (`VelocityCombine.usf:70-160`): for each *output* pixel it maps back through
`TemporalJitterPixels` to the nearest input pixel, does a 4-tap depth cross at ±1 texel,
takes the **nearest** (largest, under reversed-Z) sample, offsets the velocity fetch to
that neighbour, and scales by the output viewport size. `NGXRHI.cpp:462` then clears
`MVLowRes` because the vectors really are at output resolution.

Two things follow.

1. **Our configuration is the vendor's non-default path, not an error.** It is also
   precisely what NVIDIA forces under RR (`DLSSUpscaler.cpp:485-490`, with the comment
   *"DLSS-RR and mvec dilation can't be used together, DLSS-RR wins"*), so it is fully
   supported.
2. **It is nonetheless the largest quality lever on our SR path that we have never
   touched.** Depth-dilated MVs exist to stop thin foreground geometry inheriting the
   background's motion — and "thin detail smears / shimmers during motion" has been this
   project's recurring complaint. Building it means running the resolve at output
   resolution with a depth cross and jitter-aware output→input mapping, and clearing
   `MVLowRes`. That is a real change with a real cost, so: **described, not built.**

### 2.2 The validity test

`VelocityCombine.usf:171` is `all(EncodedVelocity.xy > 0)`. Ours
(`shaders/mv_resolve.hlsl:196`) is `encoded.x > 0.0f`, and its comment asserts that testing
both components would be wrong. Against UE's own TAA that comment is right; against
NVIDIA's plugin it is not — they test both.

The two differ only when the encoded green channel is exactly 0, which (encoding
`V.y*0.2495 + 0.49999` into UNORM) means `V.y <= -2.004` NDC. Our own census
(`[STRAYDLSS] MvStats`, CLAUDE.md §2.5) measures the whole UNORM-clamped class at
**0.0129%** of engine-written pixels, itself 3.91% of the frame — so the divergence is
bounded above by roughly one pixel in two million. **No change recommended**; the
documentation claim is what needs fixing.

### 2.3 The half-pixel

`VelocityCombine.usf:178` passes an integer `PixelPos` to `SvPositionToScreenPosition`,
which computes `(PixelPos/ViewSize - 0.5)*2` — the pixel **corner**. Ours computes
`((tid+0.5)/RenderSize)*2 - 1` — the **centre**. `shaders/mv_resolve.hlsl:174-186` already
records this question, having found from a UE5-plugin audit that NVIDIA moved to
`PixelPos.xy + 0.5f` in the newer plugin. The 4.27 tree confirms the older half of that
story first-hand. **Keep the centre**; it is both the newer vendor behaviour and the
correct one.

### 2.4 What NVIDIA does when the previous clip position is behind the camera

`VelocityCombine.usf:189-192`: on `PrevClipPos.w <= 0` they assign
`Velocity = EncodedVelocity.xy` — the **raw UNORM bits**, not the decoded vector. That
branch is only reachable when `all(EncodedVelocity.xy > 0)` was false, so at least one
component is 0; for a cleared, never-written pixel the whole texel is 0 and the result is
exactly zero. **So NVIDIA emits zero too**, and CLAUDE.md's open question — "we emit an
exact zero, which tells DLSS 'did not move'" — is settled: the vendor does the same thing,
and our census already measured the case at **0.0000%** on this title.

Worth noting for its own sake: NVIDIA's fallback is arguably a latent bug in their shader
(a partially-written texel would leak undecoded UNORM values into the motion field), which
is a good reason not to copy it verbatim. Our unconditional zero is the cleaner form of
the same behaviour.

---

## 3. Super Resolution: create flags, eval params, presets

| Claim | Ours | NVIDIA's | Verdict |
|---|---|---|---|
| `IsHDR` | always | always (`NGXRHI.cpp:459`) | **AGREES** |
| `DepthInverted` (SR) | always | `ERHIZBuffer::IsInverted`, i.e. always on this engine (`:461`) | **AGREES** |
| `MVLowRes` | always | iff not dilated (`:462`); default is dilated | **DIFFERS** — §2.1 |
| `DoSharpening` | never | iff `r.NGX.DLSS.Sharpness != 0`, default `0.0f` (`:463`, `DLSSUpscaler.cpp:86-90`) | **AGREES** in the default configuration |
| `AutoExposure` | default on | `r.NGX.DLSS.AutoExposure` default **1** (`:464`, `DLSSUpscaler.cpp:99-100`) | **AGREES** |
| `InSharpness` | `0.0f` | clamped cvar, default 0 | **AGREES** |
| `InJitterOffsetX/Y` | `TemporalAAParams.zw` straight through | `View.TemporalJitterPixels` straight through (`DLSSUpscaler.cpp:623`, `NGXD3D12RHI.cpp:220-221`) | **AGREES** (HARD) |
| `InPreExposure` | `View.PreExposure`, guarded by the row-135 self-check | `View.PreExposure`, unguarded (`DLSSUpscaler.cpp:630`, `NGXD3D12RHI.cpp:218`) | **AGREES**; our guard is an addition |
| `pInExposureTexture` | null under auto | `bUseAutoExposure ? nullptr : eyeAdaptation` (`NGXD3D12RHI.cpp:217`) | **AGREES** (HARD) |
| `InExposureScale` | set from our plan | **never set** by the plugin | **DIFFERS**; CLAUDE.md already says the plugin never sets it — confirmed |
| `pInBiasCurrentColorMask` | optional, off by default | never set | **UNVERIFIABLE** (vendor never exercises it) |
| `InReset` | camera-cut OR + gap latch | `!InputHistory.IsValid() \|\| View.bCameraCut \|\| !OutputHistory` (`DLSSUpscaler.cpp:556`) | **AGREES** — and note the vendor's own first term is the "history invalid" case CLAUDE.md §2.8 says the View CB cannot express |
| `InRenderSubrectDimensions` | `SrcRect` size | `SrcRect.Width()/Height()` (`NGXD3D12RHI.cpp:202-203`) | **AGREES** |
| `InEnableOutputSubrects` | not set (0) | `OutputColor size != DestRect.Size()` (`NGXRHI.cpp:481`) | **DIFFERS**, latent — see §3.2 |
| **`InFrameTimeDeltaInMsec`** | **SR: never set. RR: set.** | set for both (`NGXD3D12RHI.cpp:227`) | **WE ARE WRONG** — §3.1 |
| Subrect bases | all 0 | colour/depth `SrcRect.Min`, output `DestRect.Min`, MV 0 | **DIFFERS**, latent — §3.2 |
| Preset hints | five, before create | **six**, before create (`NGXRHI.cpp:569-574`) | **DIFFERS** — §3.3 |
| `FreeMemOnReleaseFeature` | never set | set from cvar, default 1 (`NGXRHI.cpp:566`) | **DIFFERS**, minor |

### 3.1 `InFrameTimeDeltaInMsec` on SR

`src/ngx_backend.cpp:1157` zero-initialises the eval struct and the SR path never assigns
`InFrameTimeDeltaInMsec`, so DLSS is told the frame took 0 ms. The RR path
(`src/ngx_backend.cpp:923`) does set it, from `view.delta_time * 1000.0f`
(`src/taa_hook.cpp:414`) — so the plumbing already exists and stops one line short of the
SR evaluate.

NVIDIA sets it on every evaluate, from `View.Family->DeltaWorldTime * 1000.0f`
(`DLSSUpscaler.cpp:625`, carried to `NGXD3D12RHI.cpp:227` through the shared
`GetCommonEvalParams`). The header (`nvsdk_ngx_helpers.h:441`) files it under
"OPTIONAL — only for research purposes" but documents a real use: *"helps in determining
the amount to denoise or anti-alias based on the speed of the object from motion vector
magnitudes and fps as determined by this delta"*. **UNCONFIRMED whether the shipped
runtime consumes it**; the vendor sends it, we should too. Small, obviously correct, and
the value is already computed — but it is not covered by a unit test in `tests/` (the
evaluate path is NGX-facing and CI-untestable), so per the brief it is **described, not
applied.** One line: `eval.InFrameTimeDeltaInMsec = in.frame_time_delta_ms;` beside the
existing jitter assignment, with `EvaluateInputs` gaining the field the RR struct already
has.

### 3.2 Subrect bases and `InEnableOutputSubrects`

`GetCommonEvalParams` (`NGXD3D12RHI.cpp:198-215`) anchors colour and depth at
`SrcRect.Min` and the output at `DestRect.Min`, and `GetNGXDLSSCreateParams`
(`NGXRHI.cpp:481`) sets `InEnableOutputSubrects` whenever the output texture is larger than
the destination rect — which, given `GetOutputExtent()`'s `Max()` (CLAUDE.md §5), is the
normal case at a quantised buffer size.

We send zeros for all of it. On Stray this is invisible because `View.ViewRectMin` reads
(0,0) and our output UAV is the engine's own upscale target — but the assumption is exactly
the one CLAUDE.md §5 warns against, and it contradicts our own `mv_resolve.hlsl`, which
*does* offset by `ViewRectMin`. If the two ever disagree in the live game the symptom is a
shifted image, not an error. **Described, not applied**: the values are already in
`ViewParams` and this is a small change, but it is also a change to the one path that is
demonstrably working, with no way to test it here.

### 3.3 The sixth preset hint

`NGXRHI.cpp:569-574` sets `…Hint_Render_Preset_DLAA`, `_UltraQuality`, `_Quality`,
`_Balanced`, `_Performance`, `_UltraPerformance` — **six**. `src/ngx_backend.cpp:1066-1070`
sets five, omitting `_UltraQuality`. `src/core/dlss_quality.cpp` never produces an
UltraQuality mode, so the omission is inert today; CLAUDE.md §5's "Set all five hint keys
before `CreateFeature`" is simply a miscount and is corrected in this change.

### 3.4 State around the evaluate

NVIDIA transitions only the output, and only through RDG:
`RHICmdList.TransitionResource(ERHIAccess::UAVMask, OutputColor)` at
`DLSSUpscaler.cpp:733`; the inputs are declared `SHADER_PARAMETER_RDG_TEXTURE`
(`:145-153`) so RDG transitions them to the SRV mask on its own. After the evaluate they do
**not** restore hardware state — they invalidate UE's own tracking
(`NGXD3D12RHI.cpp:369-370`, `ForceSetComputeRootSignature()` and
`SetCurrentCommandList(...)`).

This is the one place where the reference cannot be copied: we are not the engine and
cannot reach `FD3D12CommandContext::StateCache`, so the game's own cache still believes
whatever it last set. Our full save/restore (`src/d3d12_restore.cpp`) is the out-of-engine
equivalent and is **necessary**, not redundant. Recording it so nobody reads
"NVIDIA doesn't restore state" as permission to delete ours.

---

## 4. Frame generation

Reference: `Streamline/Source/StreamlineCore/Private/StreamlineViewExtension.cpp`,
`Streamline/Source/StreamlineRHI/Private/StreamlineRHI.cpp`,
`Streamline/Source/StreamlineRHI/Public/StreamlineConversions.h`,
`Streamline/Source/ThirdParty/Streamline/include/sl_consts.h`. Ours: `src/ngx_fg.cpp`,
`src/backend_native/fg_present.cpp`.

**Every constant we ship is confirmed.** This block was entirely UNCONFIRMED before today.

| Claim | Ours | NVIDIA's | Verdict |
|---|---|---|---|
| `mvecScale` | `1/renderW, 1/renderH` | `MotionVectorScale / ViewRect.Width(), / ViewRect.Height()`, `r.Streamline.MotionVectorScale` default `1.0` (`StreamlineViewExtension.cpp:60-64`, `:565-570`) | **AGREES** (HARD) |
| Camera near | `0.01f` | `r.Streamline.CustomCameraNearPlane` default `0.01f` (`:66-70`) | **AGREES** (HARD) |
| Camera far | `75000.0f` | `r.Streamline.CustomCameraFarPlane` default `75000.0f`, help text *"does not need to match corresponding value used by engine"* (`:72-76`) | **AGREES** (HARD) — §4.1 |
| Matrix convention | UE row-major rows sent as-is | `ToSL` copies `setRow(i, {M[i][0..3]})`, **no transpose** (`StreamlineConversions.h:17-28`) | **AGREES** (HARD) |
| `depthInverted` | 1 | `bIsDepthInverted = true` (`StreamlineViewExtension.cpp:558`) | **AGREES** |
| `cameraMotionIncluded` | 1 | `sl::eTrue` (`StreamlineRHI.cpp:296`) | **AGREES** |
| `motionVectors3D` / `MvecJittered` | 0 | `motionVectors3D = eFalse` (`:297`) | **AGREES** |
| `MvecDilated` | 0 | `motionVectorsDilated = bDilateMotionVectors` (`:295`) — see §4.2 | **AGREES** for our undilated field |
| Jitter | `TemporalAAParams.zw` | `ViewInfo.TemporalJitterPixels` (`StreamlineViewExtension.cpp:560`) | **AGREES** |
| `ClipToPrevClip` | View row 122 | `ViewUniformShaderParameters.ClipToPrevClip` (`:578`) | **AGREES** |
| `PrevClipToClip` | our inverse | `ClipToPrevClip.Inverse()` (`:579`) | **AGREES** |
| **`CameraViewToClip` / `ClipToCameraView`** | View **`ViewToClipNoAA`** (row 32) and our computed inverse (`src/ngx_fg.cpp:379-381`) | `ViewUniformShaderParameters.ViewToClip` and `ClipToView` — the **JITTERED** projection and its exact inverse (`StreamlineViewExtension.cpp:576`, `:588`) | **DIFFERS** — §4.5 |
| `cameraFOV` units | radians | `FMath::DegreesToRadians(CameraFOV)` (`StreamlineRHI.cpp:313`) | **AGREES** |
| `cameraPinholeOffset` | `(0,0)` | `FVector2f::ZeroVector` (`StreamlineViewExtension.cpp:590`) | **AGREES** |
| `clipToLensClip` | not sent / identity | `FMatrix44f::Identity` (`:577`) | **AGREES** |
| `MinRelativeLinearDepthObjectSeparation` | `40.0f` | `sl_consts.h:236` default `40.0f` | **AGREES** (HARD) |
| Camera position | `(0,0,0)` (translated world) | `ViewUniformShaderParameters.WorldCameraOrigin` (`:583`) | **DIFFERS** — §4.4 |
| `ColorBuffersHDR` | from format + colour space | not expressible: SL 2.x has no such constant | **UNVERIFIABLE** — §4.3 |
| Two-present pacing, swapchain ownership | ours | Streamline's interposer owns it | **UNVERIFIABLE** by construction |

### 4.1 The camera-plane pair

CLAUDE.md's 2026-09-03 correction — that `CameraFar = 0` was our invention, that
`sl_consts.h` makes the pair non-optional with `INVALID_FLOAT` as the unset sentinel, and
that NVIDIA ships a synthetic `0.01 / 75000.0` pair — is confirmed line for line in the
**4.27** Streamline plugin, i.e. at our exact engine version rather than by analogy with
UE5's 8.7.2. `sl_consts.h:90` defines `INVALID_FLOAT`; `:198-204` give `cameraNear`,
`cameraFar`, `cameraFOV` and `cameraAspectRatio` that default. Upgrade that note from
"HARD on what NVIDIA sends" to HARD **for this engine version**.

### 4.2 FG shares the SR motion-vector convention

`Streamline/Shaders/Private/VelocityCombine.usf`'s non-dilated branch is byte-for-byte the
same arithmetic as the DLSS plugin's: render-resolution pixels, `-(V·(0.5,-0.5)·ViewSize)`.
`mvecScale = 1/ViewRect` then normalises that to NDC-ish for SL. So **one motion-vector
field is correct for both SR and FG**, which is what our design assumes — now HARD instead
of inferred. Note `r.Streamline.DilateMotionVectors` exists here too and, if enabled,
switches the scale denominator to `GetSecondaryViewRectSize()` (`:566-568`).

### 4.3 `ColorBuffersHDR`

Not verifiable here. SL 2.x tags buffers and derives HDR itself; there is no
`colorBuffersHDR` constant in `sl_consts.h` and `StreamlineRHI.cpp:287-317` sets nothing of
the kind. `DLSSG.ColorBuffersHDR` is a raw snippet parameter of `nvngx_dlssg.dll`, reached
only because we drive the snippet directly. The box measurement stands as the only
evidence (CLAUDE.md §5: `R10G10B10A2` with no `SetColorSpace1` ⇒ 0).

### 4.4 Camera position

We send `(0,0,0)` on the reasoning that UE's translated world is camera-centred. NVIDIA
sends `ViewUniformShaderParameters.WorldCameraOrigin` — the **untranslated** world origin —
alongside `cameraUp/Right/Fwd` from the same struct. Both are self-consistent *provided the
matrices agree with the position*: SL's `clipToPrevClip` is the View CB's, which is built
in translated-world space, so NVIDIA is arguably mixing conventions and we are arguably
not. **UNCONFIRMED either way**, and the FG path measures healthy on the box, so no change.

### 4.5 We send the jitter-free projection where NVIDIA sends the jittered one

`SceneView.cpp:2413-2415` is unambiguous: `ViewUniformShaderParameters.ViewToClip =
InViewMatrices.GetProjectionMatrix()` (jittered), `ViewToClipNoAA =
GetProjectionNoAAMatrix()`, `ClipToView = GetInvProjectionMatrix()` (the inverse of the
**jittered** one). `StreamlineViewExtension.cpp:588` and `:576` send `ViewToClip` and
`ClipToView` — so Streamline receives the **jittered** projection pair, *and* a separate
`jitterOffset`.

`src/ngx_fg.cpp:379-381` sends `c.view_to_clip_no_aa` (View CB row 32) and its computed
inverse, alongside `DLSSG.JitterOffsetX/Y`. So DLSS-G is being handed a projection that
disagrees with the depth and motion-vector buffers by exactly the frame's jitter, while
also being told what that jitter is — which is either a double-correction or an
under-correction depending on what the snippet does with the pair.

Note the asymmetry with `ClipToPrevClip`, which we get right for the opposite reason: that
matrix is built from NoAA matrices *by the engine* (CLAUDE.md §5) and is the same row
NVIDIA sends. The jitter convention is per-matrix, not global.

**HARD** on what NVIDIA sends. **UNCONFIRMED** that it changes an interpolated frame —
`nvngx_dlssg.dll` is closed and the FG path currently measures healthy on the box. But this
is a one-line source change (`view_to_clip` instead of `view_to_clip_no_aa`, row 28 instead
of row 32) with an obvious A/B, and it is the only FG constant where we knowingly diverge
from the vendor. Worth a config key.

---

## 5. Corrections to our own documentation

### 5.1 Claims upgraded to HARD, first-party, at UE 4.27

* Jitter passes through unmodified (CLAUDE.md §2.7's boxed rule).
* `TemporalAAParams.zw == View.TemporalJitterPixels` (`SceneRendering.cpp:1517-1521`).
* `InPreExposure = View.PreExposure`, not its reciprocal; exposure texture nulled under
  auto; `InExposureScale` never set by the plugin.
* `EnvBRDFApproxRTG` and the NoV remap + 0.75 scale (previously "the official UE plugin,
  v3.7.3 mirror" in `docs/RESEARCH-RR-GBUFFER.md`).
* `ComputeF0` and `DiffuseColor` (previously HARD-via-mirror).
* Every frame-generation constant in §4.

### 5.2 Claims corrected

* **`DLSSUpscalerPrivate.h` is not the anchor for `PreExposure`** in the 4.27 tree — it is
  a log-category header there. The correct 4.27 anchors are `DLSSUpscaler.cpp:630` and
  `NGXD3D12RHI.cpp:218`. (The UE5 8.3.0 mirror citation was accurate *for that version*;
  the problem is that it reads as version-independent.) Same for `NGXD3D12RHI.cpp:275-276`,
  which in this tree is `NGXD3D12RHI.cpp:217`.
* **"Validity test is `EncodedVelocity.x > 0.0` — strict, red channel only. … never test
  both components"** (CLAUDE.md §2.5) attributes to NVIDIA a rule NVIDIA does not follow.
  It is UE's TAA rule. Ours matches UE; NVIDIA tests both. Both are defensible; the
  attribution is not.
* **"Set all five hint keys before `CreateFeature`"** (CLAUDE.md §5) — there are six.
* **"we emit an exact zero [on `w<=0`], which tells DLSS 'did not move'"** as an open
  worry (CLAUDE.md §2.5, §5) — closed. NVIDIA's fallback reduces to zero as well.
* **`docs/RESEARCH-DLSS-UE5-PLUGIN.md`'s half-pixel question** — the 4.27 plugin really did
  sample the corner. Our centre sampling matches the newer plugin. Settled from both sides.

### 5.3 Things this tree cannot speak to

* **DLSS Neural Rendering (feature 18 / CG2R) does not exist anywhere in this repository.**
  `nvngx_dlssnr.dll`, `DLSSNR.*` parameters, the structure/skin/style controls, the HDR
  codec question, `ControlMask` — none of it. Every NR finding in `CLAUDE.md` and
  `docs/RESEARCH-DLSSNR-STYLES.md` remains sourced to binary analysis and to third-party
  integrations, and is **UNVERIFIABLE** against the vendor plugin. Keep the two straight:
  **DLSSD / `NVSDK_NGX_Feature_RayReconstruction` is Ray Reconstruction** and *is* in this
  tree; DLSSNR is a different, unreleased feature and is not.
* Our present-time hosting: the swapchain replacement, the pacer, the crop gate, the
  descriptor shadow, the engine seam. Streamline's interposer owns the swapchain in the
  vendor design, so there is nothing to compare.
* `pInBiasCurrentColorMask` (`[STRAYDLSS] MvMask`) — the plugin never sets it, so its
  header presence remains the only evidence it does anything.

### 5.4 Header vintage

Our vendored `third_party/ngx/include/` is a **newer** NGX SDK (310.x) than the one the
4.27 plugin ships: our `nvsdk_ngx_defs_dlssd.h` adds
`NVSDK_NGX_RayReconstruction_Hint_Render_Preset` and roughly forty `DLSSD.*` guide names
(alpha, responsivity mask, transparency layers, depth-of-field and screen-space
subsurface/refraction guides) that the 4.27 header has no notion of. `nvsdk_ngx_params_dlssd.h`
is byte-identical. This is expected and benign — the helpers are inline and self-consistent
— but it means **the vendor plugin is not evidence about any parameter that exists only in
our header**, RR render presets included.

---

## 6. What to do, in order

1. **Set `InFrameTimeDeltaInMsec` on the SR evaluate.** One line, the value is already
   computed for RR, and the vendor sets it unconditionally. (§3.1)
2. **Add the sixth preset hint** and, optionally, `FreeMemOnReleaseFeature`. (§3.3)
3. **Send the real subrect bases** rather than zeros, from the `ViewParams` we already
   decode. Latent today; free insurance. (§3.2)
4. **Before RR is ever judged: switch it to NVIDIA's depth configuration** — linear depth
   from a fifth resolve output, `DepthInverted` cleared, `InUseHWDepth = Linear` — or make
   both configurations selectable and A/B them. Judging RR in a corner of the parameter
   space the vendor never ships is how a week gets spent on the wrong question. (§1.1)
5. **Put the sky branch and the albedo floors behind keys defaulting OFF** (= NVIDIA's
   behaviour), and reconsider the 0.05 specular floor specifically. (§1.2)
6. **Evaluate dilated high-resolution motion vectors for SR.** The biggest untried quality
   lever the vendor ships by default, aimed at exactly the artifact class this project
   keeps chasing. Costs an output-resolution resolve with a depth cross. (§2.1)

7. **Try the jittered `ViewToClip` for frame generation** behind a key. One row's
   difference, vendor-backed, trivially A/B-able. (§4.5)

Items 1-3 and 7 are small and safe; 4-6 are design changes and are deliberately left as
descriptions.
