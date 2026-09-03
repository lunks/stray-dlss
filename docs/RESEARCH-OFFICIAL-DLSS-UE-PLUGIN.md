# How NVIDIA's own Unreal Engine plugins integrate DLSS

**Question this answers.** This project injects DLSS into Stray (UE 4.27.2, D3D12) from outside the
engine, by intercepting the engine's `FTAAStandaloneCS` compute dispatch. We are deciding whether
each of our three features should stay a **HOOK** — recorded into the game's own command list,
inside a dispatch the engine scheduled for something else — or become a **STAGE** — our own command
list at a point we choose. Before deciding: *how does NVIDIA do it, when it has the engine's
cooperation?*

Written 2026-09-02. Every claim is labelled:

* **HARD** — read directly from Epic's engine source, or from NVIDIA's own published documentation.
* **SOFT** — read from a third-party mirror of NVIDIA's plugin, or from a blog/forum.
* **UNCONFIRMED** — believed but not established from any source read here.

---

## 0. The four findings that bear on hook-versus-stage

1. **Our SR placement is the same as the official plugin's, to within a fraction of the frame.**
   UE 4.27 calls exactly **one** temporal upscaler per view, at exactly one point, and a
   third-party plugin gets there by *substituting the pointer* rather than by adding a pass. The
   engine's `FTAAStandaloneCS` does not run when a custom upscaler is installed. We achieve the
   same net effect by suppressing that dispatch and writing its output ourselves. **The official
   architecture does not merely permit our SR placement, it is the same placement.** (§A.2, §A.3.)

2. **The official plugin is a HOOK too, and its state restore is two lines.** NVIDIA records the
   NGX evaluate onto **the engine's own D3D12 command list** and afterwards calls only
   `StateCache.ForceSetComputeRootSignature()` and rebinds the descriptor cache to the current
   command list. It restores nothing else — because UE's `FD3D12StateCache` is an *invalidation*
   cache that re-issues every binding on the next draw. We restore a long list by hand only because
   we are outside that cache and cannot invalidate it. The hook itself is not the unusual part of
   our design; the manual restore is. (§A.5.)

3. **`ITemporalUpscaler` is a real, usable extension point — and it is unreachable from a
   DLL-injection add-on without executing engine code.** It is not exported, not a COM interface
   and not discoverable at runtime; registration means calling
   `FSceneViewFamily::SetTemporalUpscalerInterface` on the game thread with a pointer to a C++
   object whose vtable matches the exact engine build. Reaching it from an injected DLL means
   locating that method in the shipping binary and constructing an ABI-compatible object — a UE4SS
   / signature-scanning problem, not a ReShade one. It is not free, but it is not fantasy either.
   (§A.1, §E.2.)

4. **NVIDIA's own frame-generation plugin puts its capture exactly where we want to move NR: the
   very end of the post-process chain, post-tonemap and pre-UI.** The Streamline UE plugin
   subscribes to `EPostProcessingPass::VisualizeDepthOfField`, which is the **last** entry of that
   enum in 4.27, captures `SceneColorAfterTonemap`, and copies it into a texture it names
   `Streamline.SceneColorWithoutHUD`. That is the `preui` site this project already hypothesised,
   chosen by NVIDIA for the same reason: it is after everything the engine feeds back, and before
   the HUD. (§B.1, §B.3, §E.3.)

---

## A. DLSS Super Resolution in the official UE plugin

### A.1 The extension point is `ITemporalUpscaler`, registered per view family

**HARD.** UE 4.27.2, `Engine/Source/Runtime/Renderer/Private/PostProcess/TemporalAA.h:146-186`:

```cpp
/** Interface for the main temporal upscaling algorithm. */
class RENDERER_API ITemporalUpscaler
{
public:

    struct FPassInputs
    {
        bool bAllowDownsampleSceneColor;
        EPixelFormat DownsampleOverrideFormat;
        FRDGTextureRef SceneColorTexture;
        FRDGTextureRef SceneDepthTexture;
        FRDGTextureRef SceneVelocityTexture;
    };

    virtual ~ITemporalUpscaler() {};

    virtual const TCHAR* GetDebugName() const = 0;

    virtual void AddPasses(
        FRDGBuilder& GraphBuilder,
        const FViewInfo& View,
        const FPassInputs& PassInputs,
        FRDGTextureRef* OutSceneColorTexture,
        FIntRect* OutSceneColorViewRect,
        FRDGTextureRef* OutSceneColorHalfResTexture,
        FIntRect* OutSceneColorHalfResViewRect) const = 0;

    virtual float GetMinUpsampleResolutionFraction() const = 0;
    virtual float GetMaxUpsampleResolutionFraction() const = 0;

    static const ITemporalUpscaler* GetDefaultTemporalUpscaler();
    static int GetTemporalUpscalerMode();
};

extern RENDERER_API const ITemporalUpscaler* GTemporalUpscaler;
```

Three details matter.

* **In 4.27 `AddPasses` returns `void` and writes through four out-parameters.** In UE 5.x it was
  changed to return an `FOutputs` struct with `FullRes` / `HalfRes` members. Any plugin source you
  read must be matched to the engine version; the two signatures are not interchangeable. (HARD for
  4.27; the 5.x form is quoted in §A.4 from a plugin mirror.)
* **The interface receives exactly the three textures we capture ourselves** — scene colour, scene
  depth, scene velocity — and nothing else. Everything else it needs it reads off `FViewInfo`.
* **`OutSceneColorHalfResTexture` is an output the implementation may decline.** Note the
  correspondence to `u1` in our own hook: the engine asks the temporal pass to optionally produce
  the half-resolution scene colour as a by-product.

**HARD.** Registration, `Engine/Source/Runtime/Engine/Public/SceneView.h:1807-1817`:

```cpp
FORCEINLINE void SetTemporalUpscalerInterface(const ITemporalUpscaler* InTemporalUpscalerInterface)
{
    check(InTemporalUpscalerInterface);
    checkf(TemporalUpscalerInterface == nullptr, TEXT("View family already had a temporal upscaler assigned."));
    TemporalUpscalerInterface = InTemporalUpscalerInterface;
}

FORCEINLINE const ITemporalUpscaler* GetTemporalUpscalerInterface() const
{
    return TemporalUpscalerInterface;
}
```

It is **per `FSceneViewFamily`, set once, and asserts on a second assignment**. One upscaler per
view family, no stacking.

**SOFT** (third-party mirror of NVIDIA's plugin — see §D for provenance). The plugin's own class,
`DLSS/Source/DLSS/Private/DLSSUpscaler.h:80`:

```cpp
class DLSS_API FDLSSUpscaler final : public ITemporalUpscaler, public ICustomStaticScreenPercentage, public ICustomResourcePool
```

and it registers itself through the *screen-percentage* callback rather than a view extension —
`DLSSUpscaler.cpp:565-604`, inside `SetupMainGameViewFamily(FSceneViewFamily& ViewFamily)`:

```cpp
checkf(GCustomStaticScreenPercentage == this, TEXT("GCustomStaticScreenPercentage is not set to a DLSS upscaler. Please check that only one upscaling plugin is active."));
...
ViewFamily.SetTemporalUpscalerInterface(GetUpscalerInstanceForViewFamily(this, DLSSQuality));

if (ViewFamily.EngineShowFlags.ScreenPercentage && !ViewFamily.GetScreenPercentageInterface())
{
    const float ResolutionFraction = GetOptimalResolutionFractionForQuality(DLSSQuality);
    ViewFamily.SetScreenPercentageInterface(new FLegacyScreenPercentageDriver(
        ViewFamily, ResolutionFraction));
}
```

So the plugin sets **two** interfaces on the family: the upscaler, and the screen-percentage driver
that makes the engine render at the fraction DLSS asked for. It does not implement
`ISceneViewExtension` for Super Resolution at all — that is the *frame generation* plugin's
mechanism (§B).

### A.2 It replaces TAA/TAAU completely. Decisive quote

**HARD.** `Engine/Source/Runtime/Renderer/Private/PostProcess/PostProcessing.cpp:523-567`, inside
`AddPostProcessingPasses`:

```cpp
// Temporal Anti-aliasing. Also may perform a temporal upsample from primary to secondary view rect.
if (AntiAliasingMethod == AAM_TemporalAA)
{
    ...
    int32 UpscaleMode = ITemporalUpscaler::GetTemporalUpscalerMode();

    const ITemporalUpscaler* DefaultTemporalUpscaler = ITemporalUpscaler::GetDefaultTemporalUpscaler();
    const ITemporalUpscaler* UpscalerToUse = ( UpscaleMode == 0 || !View.Family->GetTemporalUpscalerInterface())? DefaultTemporalUpscaler : View.Family->GetTemporalUpscalerInterface();

    const TCHAR* UpscalerName = UpscalerToUse->GetDebugName();

    // Standard event scope for temporal upscaler to have all profiling information not matter what, and with explicit detection of third party.
    RDG_EVENT_SCOPE_CONDITIONAL(
        GraphBuilder,
        UpscalerToUse != DefaultTemporalUpscaler,
        "ThirdParty %s %dx%d -> %dx%d",
        UpscalerToUse->GetDebugName(),
        View.ViewRect.Width(), View.ViewRect.Height(),
        View.GetSecondaryViewRectSize().X, View.GetSecondaryViewRectSize().Y);

    ITemporalUpscaler::FPassInputs UpscalerPassInputs;

    UpscalerPassInputs.bAllowDownsampleSceneColor = bAllowSceneDownsample;
    UpscalerPassInputs.DownsampleOverrideFormat = DownsampleOverrideFormat;
    UpscalerPassInputs.SceneColorTexture = SceneColor.Texture;
    UpscalerPassInputs.SceneDepthTexture = SceneDepth.Texture;
    UpscalerPassInputs.SceneVelocityTexture = Velocity.Texture;

    UpscalerToUse->AddPasses(
        GraphBuilder,
        View,
        UpscalerPassInputs,
        &SceneColor.Texture,
        &SecondaryViewRect,
        &HalfResolutionSceneColor.Texture,
        &HalfResolutionSceneColor.ViewRect);
}
```

**`UpscalerToUse` is a single pointer and there is exactly one `AddPasses` call.** It is either the
engine's default or the plugin's — never both. There is no "run TAA then run DLSS", and no
fall-through.

**HARD.** What the default is, `TemporalAA.cpp:1523-1573`:

```cpp
class FDefaultTemporalUpscaler : public ITemporalUpscaler
{
public:

    virtual const TCHAR* GetDebugName() const
    {
        return TEXT("FDefaultTemporalUpscaler");
    }

    virtual void AddPasses(...) const final
    {
        if (CVarTAAAlgorithm.GetValueOnRenderThread() && DoesPlatformSupportGen5TAA(View.GetShaderPlatform()))
        {
            ...
            return AddGen5MainTemporalAAPasses(...);
        }
        else
        {
            return AddGen4MainTemporalAAPasses(...);
        }
    }
    ...
};
```

and `AddGen4MainTemporalAAPasses` is what eventually calls `::AddTemporalAAPass`
(`TemporalAA.cpp:1490`) — the function that dispatches `FTAAStandaloneCS`. Its config selection,
`TemporalAA.cpp:1446-1448`, is the same line this project already documents:

```cpp
TAAParameters.Pass = View.PrimaryScreenPercentageMethod == EPrimaryScreenPercentageMethod::TemporalUpscale
    ? ETAAPassConfig::MainUpsampling
    : ETAAPassConfig::Main;
```

**So the engine's own TAA is itself an `ITemporalUpscaler` implementation.** Installing DLSS
substitutes one implementation of one slot for another. This is precisely Epic's own description.

**HARD** (Epic's published documentation, *Temporal Upscalers in Unreal Engine*,
`https://dev.epicgames.com/documentation/en-us/unreal-engine/temporal-upscalers-in-unreal-engine`):

> "Temporal upscalers all work the same with Unreal Engine, whether they are Unreal Engine 4's
> Temporal Anti-Aliasing Upscaling (TAAU), Unreal Engine 5's Temporal Super Resolution, or a
> third-party distributed plugins such as NVIDIA's DLSS 2+ Super Resolution, AMD's FSR 2.0+, and
> Intel's XeSS."

> "They all plug into the post-processing chain in the same location — between Depth of Field and
> Motion Blur."

**HARD.** There is also a kill switch, `TemporalAA.cpp:70-77`:

```cpp
static TAutoConsoleVariable<int32> CVarUseTemporalAAUpscaler(
    TEXT("r.TemporalAA.Upscaler"),
    1,
    TEXT("Choose the upscaling algorithm.\n")
    TEXT(" 0: Forces the default temporal upscaler of the renderer;\n")
    TEXT(" 1: GTemporalUpscaler which may be overridden by a third party plugin (default)."),
    ECVF_RenderThreadSafe);
```

`r.TemporalAA.Upscaler=0` forces the engine's own path regardless of what any plugin registered.

### A.3 Where it sits, and therefore what colour space it works in

**HARD.** Reading `PostProcessing.cpp` in order within `AddPostProcessingPasses`:

| Line | Pass |
|---|---|
| 490 | `DiaphragmDOF::AddPasses(...)` — **depth of field** |
| 508-514 | post-process material chain, `BL_BeforeTonemapping` |
| **523-567** | **the temporal upscaler — engine TAA or the plugin** |
| 588-599 | post-process material chain, `BL_SSRInput` (extracted for next frame) |
| 603-624 | **motion blur** |
| 710 | `AddBloomPass(...)` — **bloom** |
| 777 | `AddTonemapPass(...)` — **tonemapper** |
| 785-795 | FXAA |
| 809-819 | `EPass::VisualizeDepthOfField` — the last pass of the chain |

Epic's "between Depth of Field and Motion Blur" is exactly right for 4.27, and the ordering has a
consequence that decides everything downstream:

**The temporal upscaler runs BEFORE bloom and BEFORE the tonemapper. Its input and output are
linear HDR scene colour, pre-tonemap, carrying UE4's pre-exposure.** Its input is at the **primary**
(render) view rect; its output is at the **secondary** (display) view rect, which the very next line
records:

```cpp
//! SceneColorTexture is now upsampled to the SecondaryViewRect. Use SecondaryViewRect for input / output.
SceneColor.ViewRect = SecondaryViewRect;
```

**SOFT.** The plugin's own read of exposure confirms it treats the input as pre-exposed
(`DLSSUpscaler.cpp:441-444`, and `NGXD3D12RHI.cpp:292-293`):

```cpp
const FVector2D JitterOffset = View.TemporalJitterPixels;
...
const float PreExposure = View.PreExposure;
```

```cpp
DlssEvalParams.pInExposureTexture = InArguments.bUseAutoExposure ? nullptr : GetD3D12TextureFromRHITexture(InArguments.InputExposure, InArguments.GPUNode)->GetResource()->GetResource();
DlssEvalParams.InPreExposure = InArguments.PreExposure;
```

Note it hands NGX **both** — the engine's eye-adaptation texture *and* the scalar `PreExposure` —
and switches the texture off only when the `r.NGX.DLSS.AutoExposure` cvar is set. That is a
materially richer exposure story than our `AutoExposure` feature flag, and it is available to the
plugin because it is inside the engine and can call `GetEyeAdaptationTexture(GraphBuilder, View)`.

### A.4 Jitter, velocity, mip bias, screen percentage: who does what

**Jitter — the engine computes it, the plugin passes it through untouched. HARD + SOFT.**

Engine side, `SceneVisibility.cpp:3183-3186` and `:3327-3330`:

```cpp
bool bTemporalUpsampling = View.PrimaryScreenPercentageMethod == EPrimaryScreenPercentageMethod::TemporalUpscale;

// Apply a sub pixel offset to the view.
if (View.AntiAliasingMethod == AAM_TemporalAA && ViewState && (CVarTemporalAASamplesValue > 0 || bTemporalUpsampling) && View.bAllowTemporalJitter)
```

```cpp
View.TemporalJitterSequenceLength = TemporalAASamples;
View.TemporalJitterIndex = TemporalSampleIndex;
View.TemporalJitterPixels.X = SampleX;
View.TemporalJitterPixels.Y = SampleY;
```

Plugin side (SOFT), `DLSSUpscaler.cpp:441` reads `View.TemporalJitterPixels` and
`NGXD3D12RHI.cpp:296-297` assigns it straight across with **no sign flip and no scaling**:

```cpp
DlssEvalParams.InJitterOffsetX = InArguments.JitterOffset.X;
DlssEvalParams.InJitterOffsetY = InArguments.JitterOffset.Y;
```

This is independent confirmation of `CLAUDE.md` §2.7. `View.TemporalJitterPixels` is the same
quantity we read out of the View constant buffer as `TemporalAAParams.zw`.

**Velocity — the plugin runs its own resolve pass. SOFT.** `DLSSUpscaler.cpp:339`:

```cpp
FRDGTextureRef CombinedVelocityTexture = AddVelocityCombinePass(GraphBuilder, View, PassInputs.SceneDepthTexture, PassInputs.SceneVelocityTexture, bDilateMotionVectors);
```

`DLSS/Shaders/Private/VelocityCombine.usf` does exactly what our resolve pass does — reconstruct
camera motion from depth and `View.ClipToPrevClip`, override it where the sparse buffer is valid,
and convert to pixels:

```hlsl
float4 ThisClip = float4(PosN.xy, PosN.z, 1);
float4 PrevClip = mul(ThisClip, View.ClipToPrevClip);
float2 PrevScreen = PrevClip.xy / PrevClip.w;
float2 BackN = PosN.xy - PrevScreen;
...
float4 VelocityN = VelocityTexture.SampleLevel(VelocityTextureSampler, NearestBufferUV + VelocityOffset, 0);
bool DynamicN = VelocityN.x > 0.0;
if (DynamicN)
{
    BackN = DecodeVelocityFromTexture(VelocityN).xy;
}
BackTemp = BackN * CombinedVelocity_ViewportSize;

OutVelocityCombinedTexture[OutputPixelPos].xy = -BackTemp * float2(0.5, -0.5);
```

Three corroborations of our own implementation, all previously derived rather than seen:
`mul(v, M)` row-vector convention on `ClipToPrevClip`; the strict `> 0.0` red-channel validity
test; and the final `-BackTemp * float2(0.5, -0.5)` scale-and-sign. The shader `#error`s on
non-inverted Z, exactly as `TAAStandalone.usf` does.

The output goes to the **top-left corner** of its own texture, and the plugin then tells NGX the
motion-vector subrect starts at the origin and the scale is unity —
`NGXD3D12RHI.cpp:286-288, 299-300`:

```cpp
// The VelocityCombine pass puts the motion vectors into the top left corner
DlssEvalParams.InMVSubrectBase.X = 0;
DlssEvalParams.InMVSubrectBase.Y = 0;
...
DlssEvalParams.InMVScaleX = InArguments.MotionVectorScale.X;
DlssEvalParams.InMVScaleY = InArguments.MotionVectorScale.Y;
```

with `DLSSArguments.MotionVectorScale = FVector2D(1.0f, 1.0f);` set unconditionally at
`DLSSUpscaler.cpp:473`. **Subrect plus unit scale — never both.** This is the same rule this
project learned the hard way for feature 18's `MVecScaleX/Y`.

**Mip bias — entirely the engine's, gated on temporal upscaling. HARD.**
`SceneVisibility.cpp:3238-3245`:

```cpp
else if (View.PrimaryScreenPercentageMethod == EPrimaryScreenPercentageMethod::TemporalUpscale)
{
    ...
    View.MaterialTextureMipBias = -(FMath::Max(-FMath::Log2(EffectivePrimaryResolutionFraction), 0.0f) ) + CVarMinAutomaticViewMipBiasOffset.GetValueOnRenderThread();
    View.MaterialTextureMipBias = FMath::Max(View.MaterialTextureMipBias, CVarMinAutomaticViewMipBias.GetValueOnRenderThread());
}
```

The plugin contains no mip-bias code at all (`grep -i mipbias` over `DLSSUpscaler.cpp` is empty).
It gets the correct bias for free by putting the view on the `TemporalUpscale` path.

**Screen percentage and `r.TemporalAA.Upsampling` — the plugin hard-requires the upscale path.
SOFT.** `DLSSUpscaler.cpp:317-318`, the first two lines of `AddPasses`:

```cpp
// For TAAU, this can happen with screen percentages larger than 100%, so not something that DLSS viewports are setup with
checkf(!PassInputs.bAllowDownsampleSceneColor,TEXT("The DLSS plugin does not support downsampling the scenecolor. Please set r.TemporalAA.AllowDownsampling=0"));
checkf(View.PrimaryScreenPercentageMethod == EPrimaryScreenPercentageMethod::TemporalUpscale, TEXT("DLSS requires TemporalUpscale. If you hit this assert, please set r.TemporalAA.Upscale=1"));
```

This is the same constraint `CLAUDE.md` §4 documents for us, asserted by NVIDIA rather than
inferred. It also independently confirms the finding in `CLAUDE.md` §5 that **200% can never
work** — downsampling is refused outright.

The plugin sets the resolution fraction itself, via `FLegacyScreenPercentageDriver` with
`GetOptimalResolutionFractionForQuality(DLSSQuality)` (`DLSSUpscaler.cpp:597-602`), where the
fraction comes from `NGX_DLSS_GET_OPTIMAL_SETTINGS`. And it validates it — `DLSSUpscaler.cpp:398`:

```cpp
checkf(DestRect.Width()  < 100 || GetMinResolutionFractionForQuality(DLSSQualityMode) - 0.01f <= ScaleX && ScaleX <= GetMaxResolutionFractionForQuality(DLSSQualityMode) + 0.01f, TEXT("The current resolution fraction %f is out of the supported DLSS range [%f ... %f] for quality mode %d."), ...);
```

**Camera cut. SOFT.** `DLSSUpscaler.cpp:380`:

```cpp
const bool bCameraCut = !InputHistory.IsValid() || View.bCameraCut || !OutputHistory;
```

Two of the three terms in our own OR (`CLAUDE.md` §2.8), from inside the engine where
`InputHistory.IsValid()` is directly readable. Our third term — "the history or velocity SRV is a
1×1 texture" — exists only because we must infer `!InputHistory.IsValid()` from the outside. That
is a genuine information deficit of the hook approach, and our proxy for it is a good one.

### A.5 The official plugin is a HOOK, and its state restore is two lines

This is the finding with the most direct bearing on our decision.

**SOFT.** `DLSS/Source/NGXD3D12RHI/Private/NGXD3D12RHI.cpp:204-317`. `ExecuteDLSS` is invoked from
inside an RDG pass lambda via `RHICmdList.EnqueueLambda`, and it evaluates NGX onto the engine's
own command list:

```cpp
NVSDK_NGX_Result ResultEvaluate = NGX_D3D12_EVALUATE_DLSS_EXT(
    D3DGraphicsCommandList,
    InDLSSState->DLSSFeature->Feature,
    InDLSSState->DLSSFeature->Parameter,
    &DlssEvalParams
);
checkf(NVSDK_NGX_SUCCEED(ResultEvaluate), TEXT("NGX_D3D12_EVALUATE_DLSS_EXT failed! (%u %s), %s"), ResultEvaluate, GetNGXResultAsString(ResultEvaluate), *InDLSSState->DLSSFeature->Desc.GetDebugDescription());
InDLSSState->DLSSFeature->Tick(FrameCounter);

Device->GetCommandContext().StateCache.ForceSetComputeRootSignature();
Device->GetCommandContext().StateCache.GetDescriptorCache()->SetCurrentCommandList(Device->GetCommandContext().CommandListHandle);
```

**That is the entire post-NGX state restore: force the compute root signature to be re-set, and
point the descriptor cache at the current command list.** No descriptor heaps, no PSO, no root
parameters, no topology, no viewports, no render targets.

The reason is not that NGX clobbers less than we think. It is that UE's `FD3D12StateCache` is a
*lazy* cache: it remembers what it believes is bound and re-issues anything it does not believe is
current before the next draw or dispatch. Telling it "your compute root signature belief is stale"
is enough to make it re-bind everything on that path. **NVIDIA does not restore state; it
invalidates a cache and lets the engine restore state.**

We cannot do that, because we are not inside `FD3D12StateCache` and have no way to reach it. Our
long manual restore is the price of being outside the engine, not evidence that we chose a stranger
place to hook. The hook site itself is the same one NVIDIA uses.

Also note what is **not** there: NVIDIA does not create its own command list, does not fence, and
does not defer to Present. The official Super Resolution integration is a hook, recorded inline,
mid-frame, on the engine's queue.

**SOFT.** The plugin also allocates its own output rather than writing over the input,
`DLSSUpscaler.cpp:406-412`:

```cpp
FRDGTextureDesc SceneColorDesc = FRDGTextureDesc::Create2D(
    OutputExtent,
    PF_FloatRGBA,
    FClearValueBinding::Black,
    TexCreate_ShaderResource | TexCreate_UAV);

const TCHAR* OutputName = TEXT("DLSSOutputSceneColor");
```

**This is a real architectural difference from us and it is worth naming.** The plugin returns a
*new* texture to the caller; RDG then routes it onward. We write into `u0`, the engine's existing
TAA output — which on this title is also the next frame's `HistoryBuffer[0]` and this frame's scene
colour (`CLAUDE.md` §2.9). The plugin never has to think about that because it never writes into a
resource with two roles. **Our feedback-node problem is a consequence of writing in place, and the
official plugin does not have it.**

---

## B. DLSS Frame Generation in the official plugin (Streamline)

### B.1 It does NOT use `ITemporalUpscaler`. It uses an `ISceneViewExtension`

**HARD, negative result, stated honestly.** The public `NVIDIA-RTX/Streamline` repository does not
mention `ITemporalUpscaler`, `ISceneViewExtension`, or any Unreal-specific hook point anywhere in
`docs/` or `README.md`. The only Unreal reference is an `EngineType::eUnreal` enum value in
`docs/ProgrammingGuide.md` used so `slInit`'s `Preferences::engine` can be set for telemetry.
Streamline itself is engine-agnostic and hooks D3D/DXGI/Vulkan directly. **The Streamline repo
therefore cannot confirm what the UE plugin's own hook point is** — that lives in the
Marketplace-distributed plugin, not in the SDK.

**SOFT** (third-party mirror, §D). It is an `ISceneViewExtension`.
`Plugins/Streamline/Source/StreamlineCore/Private/StreamlineViewExtension.h:31-54` (class at :31, `SubscribeToPostProcessingPass` at :54, UE4 branch at :42):

```cpp
class FStreamlineViewExtension final : public FSceneViewExtensionBase
{
public:
    FStreamlineViewExtension(const FAutoRegister& AutoRegister, FStreamlineRHI* InStreamlineRHI);

    virtual void SetupViewFamily(FSceneViewFamily& InViewFamily) override;
    virtual void SetupView(FSceneViewFamily& InViewFamily, FSceneView& InView) override;
    virtual void SetupViewPoint(APlayerController* Player, FMinimalViewInfo& InViewInfo) override;
    virtual void BeginRenderViewFamily(FSceneViewFamily& InViewFamily) override;
    ...
    virtual void SubscribeToPostProcessingPass(EPostProcessingPass Pass, FAfterPassCallbackDelegateArray& InOutPassCallbacks, bool bIsPassEnabled) override;
```

Note the file is version-guarded for UE4 (`#if ENGINE_MAJOR_VERSION == 4 typedef
FRHICommandListImmediate FGraphBuilderOrCmdList;`), so this architecture is applicable to 4.27, not
only to UE5.

The two plugins also explicitly know about each other. **SOFT**, from the DLSS plugin's `NGXRHI.cpp`
in a UE5.6-era mirror: *"Streamline plugin also uses NGX so it's not safe for us to call NGX
shutdown functions from this plugin when Streamline is enabled"*, guarded by
`IPluginManager::Get().FindPlugin(TEXT("StreamlineCore"))`. **Super Resolution and Frame Generation
are two separate plugins with two separate engine hooks, sharing only the NGX runtime.**

### B.2 Where Streamline hooks presentation: interposer by default, manual hooking optional

**HARD**, `docs/ProgrammingGuideManualHooking.md` and `include/sl_hooks.h` in `NVIDIA-RTX/Streamline`
(version 2.12.0).

Default mode: **Streamline is a swapchain and device interposer.** `sl.interposer.dll` stands in for
`dxgi.dll` / `vulkan-1.dll` and intercepts, per the `enum class FunctionHookID` in `sl_hooks.h`:
`IDXGIFactory::CreateSwapChain*`, `IDXGISwapChain::Present` and `Present1`, `GetBuffer`,
`ResizeBuffers*`, `GetCurrentBackBufferIndex`, `SetFullscreenState`,
`ID3D12Device::CreateCommandQueue`, and the Vulkan present/swapchain equivalents.

Manual hooking (`PreferenceFlag::eUseManualHooking`, set before `slInit`) replaces the global hook
with explicit calls. There is **no** `slSetPresentCallback` or `slHookSwapchainPresent`; the actual
API is:

* `slUpgradeInterface(&ptr)` — hand Streamline a native pointer and get an SL proxy back in its
  place.
* `slGetNativeInterface(proxy, &native)` — the reverse.
* The host then routes each `FunctionHookID` call to the proxy at the point its own engine would
  have called the native API.

Quoted verbatim from `ProgrammingGuideManualHooking.md`:

> "you must ensure the common plugin's `presentCommon()` function is called every frame... the
> recommended approach is to call `slUpgradeInterface` immediately after creating the frame
> presentation interface (e.g., the swapchain on DirectX), so Streamline can integrate with your
> presentation path and invoke `presentCommon()`."

In manual mode Streamline can no longer track resource or command-list state, so the host must
supply correct `D3D12_RESOURCE_STATES` on every `sl::Resource` tag and must restore command-list
state after `slEvaluateFeature` itself. **That is the same bargain we make: leave the automatic
path and you inherit the state bookkeeping.**

### B.3 The buffers, and when each is tagged

**HARD**, `docs/ProgrammingGuideDLSS_G.md` §5.0 "TAG ALL REQUIRED RESOURCES", §5.1 "REQUIRED AND
OPTIONAL RESOURCES", §5.2 "TAGGING RECOMMENDATIONS".

| Buffer | Notes (verbatim where quoted) |
|---|---|
| `Backbuffer` | auto-intercepted via the SL swapchain; tag `sl::kBufferTypeBackbuffer` only to pass subrect info |
| `sl::kBufferTypeDepth` | "Same depth data used to generate motion vector data... same set of requirements as DLSS-SR, and the same depth can be used for both." |
| `sl::kBufferTypeMvec` | dense camera + dynamic-object motion, same requirements as SR |
| `sl::kBufferTypeHUDLessColor` | "The scene color *before* any UI/HUD elements are drawn." |
| `sl::kBufferTypeUIColorAndAlpha` **or** `sl::kBufferTypeUIAlpha` | "If both buffers are tagged, Streamline will use the UI Alpha buffer." |
| `sl::kBufferTypeBidirectionalDistortionField` | optional, only for strong post-process distortion |

**When.** Tagging is `slSetTagForFrame(*currentFrame, viewport, tags[], count, cmdList)`, called as
each resource becomes ready. Depth and motion vectors are tagged together right after they are
generated; **hudless colour is tagged "After post-processing pass but before UI/HUD is added"**; the
UI buffer is tagged whenever its target is populated. §5.2, verbatim:

> "tagged buffers are used during the `Swapchain::Present` call. If the tagged buffers are going to
> be reused, destroyed or changed in any way before the frame is presented, their life-cycle needs
> to be specified correctly."

The recommended default lifecycle is `sl::ResourceLifecycle::eValidUntilPresent`. And a rule that
matches our own gameplay gate: **if validity cannot be guaranteed — loading, paused, menu, cutscene
— every tag must be set to a null pointer.**

**The blending contract**, verbatim from the §5.1 table row "UI Alpha OR UI Color and Alpha":

> "`UI Alpha` is a single channel containing only the alpha values (0.0 to 1.0) of the UI... `UI
> Color and Alpha` also contains the RGB color of the UI... Prefer `UI Alpha` (single channel) for
> performance when available. If both are tagged, only `UI Alpha` will be used. Must be 0.0 for
> pixels with no UI elements. Alpha must be non-zero for pixels with UI. Values provided must
> respect the standard blending formula: `Final_Color.RGB = UI.RGB + (1 - UI.Alpha) x Hudless.RGB`.
> When UI color is provided, the RGB channels must be pre-multiplied by alpha."

### B.4 What happens when the UI cannot be separated

**HARD**, `ProgrammingGuideDLSS_G.md` §5.1, hudless row, verbatim:

> "Should contain the full viewable scene, without any HUD/UI elements in it. **If some HUD/UI
> elements are unavoidably included, expect some image quality degradation on those elements**"

NVIDIA's documentation does not use the word "ghosting"; it states the cost as image-quality
degradation **localised to the HUD/UI pixels**, which is the same phenomenon. The upside of getting
it right is stated in §6.6:

> "When both Hudless and a UI buffer are tagged, User Interface Recomposition can be enabled... the
> HUD and scene are interpolated separately and composited later, providing significantly-improved
> UI interpolation quality"

at "slight performance and memory cost". A checklist item at the top of the same document adds the
hard constraint: *"Ensure extent resolution or resource size, whichever is in use, for `Hudless`
and `UI Color and Alpha` buffers exactly match that of backbuffer."*

**Where the plugin actually captures hudless colour. SOFT, and this is the finding worth keeping.**
`StreamlineViewExtension.cpp:508-516`:

```cpp
void FStreamlineViewExtension::SubscribeToPostProcessingPass(EPostProcessingPass Pass, FAfterPassCallbackDelegateArray& InOutPassCallbacks, bool bIsPassEnabled)
{
    if (Pass == EPostProcessingPass::VisualizeDepthOfField)
    {
        check(StreamlineRHIExtensions);
        check(StreamlineRHIExtensions->IsStreamlineAvailable());
        InOutPassCallbacks.Add(FAfterPassCallbackDelegate::CreateRaw(this, &FStreamlineViewExtension::PostProcessPassAtEnd_RenderThread));
    }
}
```

**HARD** — why that pass. `Engine/Source/Runtime/Engine/Public/SceneViewExtension.h:101-108`:

```cpp
enum class EPostProcessingPass : uint32
{
    MotionBlur,
    Tonemap,
    FXAA,
    VisualizeDepthOfField,
    MAX
};
```

`VisualizeDepthOfField` is the **last** entry before `MAX`. Subscribing after it means running at
the very end of the post-processing chain — after motion blur, after the tonemapper, after FXAA,
and before Slate draws the UI. The method's own name says so: `PostProcessPassAtEnd_RenderThread`.

**SOFT.** What it does there, `StreamlineViewExtension.cpp:604, 638-650, 748`:

```cpp
// input color
FRDGTextureRef SceneColorAfterTonemap = SceneColor.Texture;
check(SceneColorAfterTonemap);
```

```cpp
FRDGTextureRef SLSceneColorWithoutHUD = SceneColor.Texture;

const bool bTagSceneColorWithoutHUD = GIsEditor ? CVarStreamlineTagEditorSceneColorWithoutHUD.GetValueOnRenderThread() : CVarStreamlineTagSceneColorWithoutHUD.GetValueOnRenderThread();
if(bTagSceneColorWithoutHUD)
{
    FRDGTextureDesc Desc = SceneColor.Texture->Desc;
    EnumAddFlags(Desc.Flags, TexCreate_ShaderResource | TexCreate_UAV);
    EnumRemoveFlags(Desc.Flags, TexCreate_Presentable);
    EnumRemoveFlags(Desc.Flags, TexCreate_ResolveTargetable);
    SLSceneColorWithoutHUD = GraphBuilder.CreateTexture(Desc, TEXT("Streamline.SceneColorWithoutHUD"));
    AddDrawTexturePass(GraphBuilder, ViewInfo, SceneColor.Texture, SLSceneColorWithoutHUD, FIntPoint::ZeroValue, FIntPoint::ZeroValue, FIntPoint::ZeroValue);
}
```

```cpp
TexturesToTag.Add({ bTagSceneColorWithoutHUD ? PassParameters->SceneColorWithoutHUD->GetRHI() : nullptr, SceneColor.ViewRect, EStreamlineResource::HUDLessColor});
```

It **copies** the post-tonemap scene colour into a texture it owns rather than tagging the engine's,
because the engine will keep using and recycling the original. Depth and motion vectors are pulled
from `FSceneTextures` in the same callback, with a UE4-specific branch
(`FSceneRenderTargets::Get(GraphBuilder.RHICmdList)`).

**So the official frame-generation integration is a hybrid, and this is the shape that matters
for us:** *guides are captured by a mid-frame render-thread hook; the feature itself is evaluated
in the presentation path* by the interposer, at Present, from the tags. Capture where the data is;
evaluate where the frame is.

### B.5 DLSS Super Resolution's own tagging, for contrast

**HARD**, `docs/ProgrammingGuideDLSS.md` §4.0. SR needs `sl::kBufferTypeScalingInputColor`,
`sl::kBufferTypeScalingOutputColor`, `sl::kBufferTypeDepth`, `sl::kBufferTypeMvec`, and optionally
`sl::kBufferTypeExposure` (1×1). There is **no HUD or UI concept at all** — SR runs pre-composite,
on the render, not on the presented backbuffer. It is evaluated with
`slEvaluateFeature(sl::kFeatureDLSS, ...)` at the point in the pipeline where the engine's own
upscale would otherwise run, and **the host is explicitly responsible for restoring command-list
state afterward** (§7). The same division of labour as the UE plugin's `ForceSetComputeRootSignature`.

---

## C. DLSS Ray Reconstruction, briefly

**HARD**, `docs/ProgrammingGuideDLSS_RR.md` in `NVIDIA-RTX/Streamline` (feature
`kFeatureDLSS_RR`, header `sl_dlss_d.h`). Ray Reconstruction **replaces the game's denoiser and the
upscaler at the same time.** Its §4.1 tag list is materially larger than Super Resolution's: diffuse
albedo, specular albedo, normals, roughness, colour input, motion vectors, depth, specular motion
vector reflections, specular hit distance, plus optional transparency-overlay,
colour-before-transparency, subsurface and depth-of-field guides, plus the output. It consumes
G-buffer material data a plain upscaler never needs, and it takes **noisy** ray-traced input
directly rather than sitting after a separately denoised buffer.

**SOFT.** In the UE plugin, the analogous engine seam for "replace the denoiser" already exists and
NVIDIA already uses it. `DLSS/Source/DLSS/Private/DLSSDenoiser.h:29-31`:

```cpp
// wrapper for the default denoiser to add TAA after some passes
class DLSS_API FDLSSDenoiser final : public IScreenSpaceDenoiser
{
public:
    FDLSSDenoiser(const IScreenSpaceDenoiser* InWrappedDenoiser, const FDLSSUpscaler* InUpscaler);
```

In the Super Resolution plugin this class only *wraps* the engine's denoiser; the interesting part
is that `IScreenSpaceDenoiser` is a per-signal interface (`DenoiseReflections`,
`DenoiseDiffuseIndirect`, `DenoiseAmbientOcclusion`, …), not a whole-image one. **Ray Reconstruction
is therefore not a post-process filter bolted after the upscaler; it is a replacement for a set of
engine passes that each have their own inputs.** That is the published architecture our own neural
pass is nearest to, and it is a reminder that RR's power comes from being fed G-buffer guides at the
point the denoisers run — not from being applied to a finished image.

Relevant to our own measured result in `CLAUDE.md` — that suppressing UE's
`FSSDTemporalAccumulationCS` while DLSS SR ran made shimmer 3.4× worse — this is exactly what the
published architecture predicts: SR is not a denoiser, and the only NVIDIA feature that claims that
job is RR, which demands the guides UE's denoisers already have and SR never sees.

---

## D. Sources and provenance

| Source | What it is | Label |
|---|---|---|
| `AlexMercer-MA/UnrealEngine-4.27` | Public mirror of Epic's UE4 source. Verified: `Engine/Build/Build.version` reads `MajorVersion 4, MinorVersion 27, PatchVersion 2, BranchName "++UE4+Release-4.27"` | **HARD** |
| `NVIDIA-RTX/Streamline` `docs/*.md` | NVIDIA's own published SDK documentation, v2.12.0. The repo ships both PDFs and Markdown twins; the Markdown was read | **HARD** |
| `dev.epicgames.com` documentation | Epic's own published docs | **HARD** |
| `flygod1159/Nvidia-DLSS-Plugin` | Third-party GitHub upload of NVIDIA's DLSS plugin. `DLSS.uplugin` reads `VersionName 2.3.2`, `EngineVersion 5.0.0`, `CreatedBy "NVIDIA"`. Source is version-guarded for UE4 (`#if ENGINE_MAJOR_VERSION == 4` branches throughout `DLSSDenoiser.h`), so the same tree targets 4.26/4.27 | **SOFT** |
| `Adriwin06/Ultimate-CommonUI-Menu-System` | A UE project with NVIDIA's DLSS **and** Streamline plugins vendored under `Plugins/`. Used for `StreamlineViewExtension.{h,cpp}`. UE4 branches present | **SOFT** |

**Why the plugin mirrors are SOFT even though they read as genuine.** They carry real NVIDIA
copyright headers, use RDG and NGX APIs correctly, are internally consistent with each other and
with Epic's public API surface, and match Epic's published description of the integration. But they
are not an NVIDIA-controlled distribution channel and no signature or checksum was verified. Treat
every quote from them as strong corroboration, never as the sole basis for a decision.

**Two version caveats a reader must carry.** First, `ITemporalUpscaler::AddPasses` returns `void`
with out-parameters in 4.27 and an `FOutputs` struct in UE 5.x; the plugin mirror read here is the
5.x form, so its signature does **not** match the 4.27 interface quoted in §A.1. Second, in the
newest plugins (UE 5.6-era) `FDLSSUpscaler` no longer derives from `ITemporalUpscaler` directly — a
separate `FDLSSSceneViewFamilyUpscaler` does. Neither change alters any conclusion here, but both
would break a naive copy of the code.

**One thing that stayed UNCONFIRMED.** The `NVIDIA-RTX/Streamline` repository does not document the
Unreal plugin's own hook point. §B.1's claim that Frame Generation uses `ISceneViewExtension` rests
on the plugin mirror alone, corroborated by the fact that Streamline is engine-agnostic and could
not plausibly do it any other way. If that claim ever becomes load-bearing for a decision, it wants
a primary source.

---

## E. What this means for this project

### E.1 Our SR placement is the official placement. Keep it

**Yes, and not approximately.** The official plugin's `AddPasses` receives scene colour, scene depth
and scene velocity, at the primary view rect, in linear pre-tonemap HDR carrying pre-exposure, and
returns an image at the secondary view rect. Our hook intercepts the dispatch that consumes exactly
those three textures at exactly that point and produces exactly that output. The engine's own TAA is
one implementation of that slot and DLSS is another; the plugin swaps the pointer, we suppress the
dispatch. **The net frame graph is the same.**

The differences that remain are real but are consequences of being outside the engine, not of having
picked a different site:

| | Official plugin | Us |
|---|---|---|
| Where NGX is recorded | the engine's D3D12 command list | the engine's D3D12 command list |
| Output resource | a **new** RDG texture, `DLSSOutputSceneColor` | **in place** into the engine's `u0` |
| State restore | invalidate `FD3D12StateCache` (2 calls) | save and restore ~8 categories by hand |
| Camera cut | `!InputHistory.IsValid() \|\| View.bCameraCut` | plus a 1×1-SRV proxy for the first term |
| Exposure | eye-adaptation texture **and** scalar `PreExposure` | `AutoExposure` flag |
| Jitter | `View.TemporalJitterPixels`, passed through | `TemporalAAParams.zw`, passed through — identical |
| Mip bias | free, from the engine | free, from the engine |

**Nothing here argues for moving SR to a stage.** A stage at Present would be strictly worse: the
image there is post-tonemap and display-resolution, which is not what a super-resolution feature
consumes, and the engine's own TAA would still be running. **SR must stay a hook.**

The one change §A.5 does suggest is worth weighing on its own merits: **writing to a scratch texture
and copying, rather than writing into `u0` in place.** The official plugin never touches a resource
with two roles, which is why it has no feedback-node problem. That is a smaller and better-understood
change than moving the hook.

### E.2 `ITemporalUpscaler` is reachable in principle, and here is the actual cost

An injected DLL *could* use it. What it would take, all established above:

1. **Get a pointer to the live `FSceneViewFamily`** on the game thread, before rendering begins. The
   plugin does this from `ICustomStaticScreenPercentage::SetupMainGameViewFamily`, a callback we do
   not receive. An injected DLL would have to reach the family another way — hooking a renderer
   entry point that takes one.
2. **Call `FSceneViewFamily::SetTemporalUpscalerInterface`.** It is `FORCEINLINE`
   (`SceneView.h:1807`), so there is no exported symbol to import and probably no out-of-line copy
   to call. In practice this means writing the `TemporalUpscalerInterface` member directly at its
   offset in the family — a structure-offset problem, solvable but build-specific.
3. **Supply an object whose vtable matches `ITemporalUpscaler` for this exact engine build**, and
   whose `AddPasses` can accept an `FRDGBuilder&`, an `FViewInfo&` and RDG texture refs, and build
   RDG passes. That is the expensive part: it means linking against, or reimplementing, enough of
   RDG to add a pass — and RDG's internals are not ABI-stable across builds.
4. **Set the screen-percentage interface too**, or the engine will not render at a reduced
   resolution and there will be nothing to upscale.

**Verdict: possible, and out of proportion to the benefit.** What it would buy is the two-line state
restore, the real `InputHistory.IsValid()`, and the eye-adaptation texture. What it would cost is an
RDG-compatible object built against a specific licensee build of 4.27.2, in a project whose only
feedback loop is a log file from another machine. Our dispatch hook already gets the same pixels at
the same point. **Do not pursue this for SR.** It is worth recording as the answer to "is there a
supported seam", because there is, and because a future UE4SS-hosted plugin — which already executes
inside the game's address space and already resolves engine symbols — changes the arithmetic enough
that the question should be re-asked then, not now.

### E.3 Yes: the official architecture supports SR staying put while NR moves post-tonemap

This is the clearest answer the research gives, and it comes from NVIDIA's own choices rather than
from our reasoning about them.

**NVIDIA splits by what the feature consumes, not by convenience.**

* **Super Resolution** consumes render-resolution, pre-tonemap, linear HDR scene colour plus depth
  and velocity. It is hooked **mid-frame, inline, on the engine's command list**, at the temporal
  upscale slot. (§A)
* **Frame Generation** consumes display-resolution, **post-tonemap**, display-referred colour with
  the HUD separated. Its guides are captured by a render-thread callback at
  `EPostProcessingPass::VisualizeDepthOfField` — **the last pass of the chain, after the tonemapper,
  before the UI** — and the feature is evaluated in the **presentation path**. (§B)

Two features from one vendor, in one engine, deliberately hooked at two different points, because
they eat different images. **That is exactly the split we are considering.**

For our neural pass specifically, three things follow:

1. **The post-tonemap, pre-UI site is not exotic — it is where NVIDIA puts its own post-tonemap
   consumer.** `EPostProcessingPass::VisualizeDepthOfField` being last in the enum
   (`SceneViewExtension.h:101-108`, HARD) makes "after the whole post chain, before Slate" a
   *named* place in the engine, not a heuristic. Our `preui` back-buffer-ordinal detector is trying
   to find that same boundary from outside; the ordinal is our proxy for a seam the engine actually
   has.
2. **A post-tonemap site removes the HDR codec problem by construction, not by tuning.** Feature 18
   is display-referred; the engine's post-tonemap colour is display-referred. Every one of our
   codec's moving parts — the soft-clip knee, the paper-white scale, exposure tracking — exists only
   because our current site hands the network raw pre-exposed linear HDR. NVIDIA's post-tonemap
   consumer needs none of that. Our own `CLAUDE.md` already documents that the codec is bypassed at
   the post-tonemap sites; this research says that is the intended shape, not a shortcut.
3. **NVIDIA copies rather than writing in place, and it does so precisely because the engine will
   reuse the original.** `Streamline.SceneColorWithoutHUD` is a fresh texture filled by
   `AddDrawTexturePass`. Our history-restore workaround is the in-place version of the same idea,
   paid for every frame with two full-rect copies and one unverified resource-state assumption. A
   post-tonemap stage needs neither, because nothing after the tonemapper is carried into the next
   frame.

**And one caution that cuts the other way.** NVIDIA does the post-tonemap capture from an
`ISceneViewExtension` callback **inside the render thread's RDG graph** — a place where the engine
hands it a well-defined `FScreenPassTexture` and manages barriers. Our `preui` site is a
render-target *identity* test on the game's command list, with the resource state inferred rather
than given. The site is right; the mechanism for reaching it is strictly harder from outside, and
that difficulty is where our risk actually lives. It is the same difficulty that made the first
`preui` attempt wreck a frame.

### E.4 Frame generation: our stage is already the official shape

Our FG runs as a stage on our own command list at Present. Streamline's default mode is a swapchain
interposer that runs at Present; its manual-hooking mode requires the host to route present through
`slUpgradeInterface` and guarantees `presentCommon()` runs every frame. **Presentation is where FG
belongs, in NVIDIA's architecture and in ours.** No change indicated.

The one gap worth naming: NVIDIA's FG gets **hudless colour and a UI alpha buffer**, tagged
separately, with a stated blending contract and a documented quality cost for failing to separate
them (§B.3, §B.4). Anything we do at Present sees the composited frame. That is a real quality
ceiling on any injected FG, and it is documented by NVIDIA rather than speculative — "expect some
image quality degradation on those elements". Knowing the exact contract
(`Final_Color.RGB = UI.RGB + (1 - UI.Alpha) x Hudless.RGB`) at least tells us what a hudless capture
would have to look like if we ever tried to produce one.

### E.5 Summary table

| Feature | Ours today | Official | Verdict |
|---|---|---|---|
| Super Resolution | hook, engine's command list, in the TAA dispatch | hook, engine's command list, at the temporal upscale slot | **Same. Keep it a hook.** |
| Neural Rendering | hook, immediately after SR, linear pre-tonemap `u0` | no direct analogue; RR replaces the *denoisers*, and Streamline's post-tonemap consumer captures at the end of the post chain | **Site is wrong for a display-referred network. A post-tonemap, pre-UI stage is the officially-shaped placement.** |
| Frame Generation | stage, our own command list, at Present | interposer or manual hook, at Present | **Same. Keep it a stage.** |
