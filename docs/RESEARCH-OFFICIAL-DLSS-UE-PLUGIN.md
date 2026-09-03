# How NVIDIA's own Unreal Engine DLSS plugin does it

Research for the **hook versus stage** decision. This project injects DLSS into Stray (UE 4.27.2,
D3D12) from outside the engine, by intercepting the engine's own `FTAAStandaloneCS` compute
dispatch. Before deciding whether each of our three features should stay a **HOOK** (recorded into
the game's command list, inside a dispatch the engine scheduled for something else) or become a
**STAGE** (our own command list at a point we choose), we wanted to know what NVIDIA's official
plugin actually does.

**Provenance labels**, per `CLAUDE.md` §0.5:

* **HARD** — read directly from UE 4.27.2 engine source, from NVIDIA's own plugin source with its
  copyright header intact, or from NVIDIA's own Streamline documentation.
* **SOFT** — a blog post, a forum, a vendor marketing page, or a claim resting on a mirror whose
  authenticity is established but whose engine version differs from ours.
* **UNCONFIRMED** — searched for and not found, or inferred without a citation.

## 0. Sources, and how far each can be trusted

| Source | What it is | Trust |
|---|---|---|
| `AlexMercer-MA/UnrealEngine-4.27` | Public mirror of UE 4.27.2. **Version confirmed in this browse**: `Engine/Build/Build.version` reads `MajorVersion 4, MinorVersion 27, PatchVersion 2, BranchName "++UE4+Release-4.27"` | **HARD** for anything quoted from it |
| `flygod1159/Nvidia-DLSS-Plugin` | Genuine NVIDIA DLSS UE plugin source. Every file carries `Copyright (c) 2020 NVIDIA CORPORATION` and the full NVIDIA proprietary notice; the module layout is NVIDIA's own (`DLSS`, `DLSSBlueprint`, `DLSSEditor`, `DLSSUtility`, `NGXD3D11RHI`, `NGXD3D12RHI`, `NGXRHI`, `NGXVulkanRHI`). It is **not** a stub or a reimplementation | **HARD** that this is NVIDIA's code; **SOFT** that it applies to 4.27 — see below |
| `mrcasty/DLSS4-alpha-unreal` | A DLSS4 / UE 5.6-era patch layer whose inner `plugins/DLSS/Source/` tree mirrors the above. Its README says it requires the official binaries | **SOFT**, and adds nothing the first mirror does not |
| Epic's documentation portal | `dev.epicgames.com` pages on temporal upscalers and on anti-aliasing | **HARD** for what Epic asserts, but the pages are UE5-era |
| `NVIDIA-RTX/Streamline` | NVIDIA's own public SDK documentation | **HARD** |

**The one version caveat that matters.** The plugin mirror is UE5-era: its `AddPasses` returns
`ITemporalUpscaler::FOutputs`, whereas 4.27.2's interface writes through out-parameters. The
plugin is written to compile against both engine generations — `DLSSDenoiser.h:44-54` branches on
`ENGINE_MAJOR_VERSION` with an `#error` for anything but 4 or 5 — so the **architecture** it shows
is the one it also used on 4.27, but any exact signature quoted from it is UE5's. Every structural
claim below is anchored in the 4.27.2 engine source independently.

---

## A. DLSS Super Resolution in the official plugin

### A.1 The extension point is `ITemporalUpscaler`, and it is a renderer interface, not a view extension

**HARD.** UE 4.27.2 declares the interface in
`Engine/Source/Runtime/Renderer/Private/PostProcess/TemporalAA.h:147-186` — note **`Private/`**, not
`Public/`; the header moved to `Renderer/Public/TemporalUpscaler.h` in UE5:

```cpp
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

**What `AddPasses` receives is exactly three textures** — scene colour, scene depth, scene velocity
— plus two downsample hints. **No jitter, no matrices, no exposure, no mip bias.** Everything else
the implementation needs it reads off `FViewInfo` itself. That is worth sitting with: the official
extension point hands over no more engine state than our dispatch hook already captures. It hands
over *less*, and NVIDIA's implementation goes to `View` for the rest, exactly as we go to the View
constant buffer.

**What it returns** is the upscaled colour texture, the output view rect, and optionally a
half-resolution copy. `SceneColorTexture` is an in/out: the caller passes `&SceneColor.Texture` and
the upscaler overwrites it.

**Registration is per view family, one-shot.** `Engine/Source/Runtime/Engine/Public/SceneView.h`:

```cpp
FORCEINLINE void SetTemporalUpscalerInterface(const ITemporalUpscaler* InTemporalUpscalerInterface)
{
    check(InTemporalUpscalerInterface);
    checkf(TemporalUpscalerInterface == nullptr, TEXT("View family already had a temporal upscaler assigned."));
    TemporalUpscalerInterface = InTemporalUpscalerInterface;
}                                                             // SceneView.h:1807-1812
...
const ITemporalUpscaler* TemporalUpscalerInterface;           // SceneView.h:1847, private
```

`ISpatialUpscaler` is a **separate** pair of slots on the same class (`SceneView.h:1819-1838`,
primary and secondary). Our work maps onto the temporal slot only.

**How NVIDIA's plugin actually registers.** `DLSSUpscaler.cpp:567-604`, in
`FDLSSUpscaler::SetupMainGameViewFamily` — which is an override of `ICustomStaticScreenPercentage`,
a *different* engine interface whose whole job is to be called once per game view family:

```cpp
ViewFamily.SetTemporalUpscalerInterface(GetUpscalerInstanceForViewFamily(this, DLSSQuality));

if (ViewFamily.EngineShowFlags.ScreenPercentage && !ViewFamily.GetScreenPercentageInterface())
{
    const float ResolutionFraction = GetOptimalResolutionFractionForQuality(DLSSQuality);
    ViewFamily.SetScreenPercentageInterface(new FLegacyScreenPercentageDriver(
        ViewFamily, ResolutionFraction));
}
```

**HARD** (NVIDIA's source). The class declaration is
`class DLSS_API FDLSSUpscaler final : public ITemporalUpscaler, public ICustomStaticScreenPercentage, public ICustomResourcePool`
(`DLSSUpscaler.h:80`). So the plugin plugs into **three** engine interfaces: the temporal upscaler
slot for the pass itself, the static-screen-percentage hook to install itself and to *set the render
resolution* from the DLSS quality mode, and a resource-pool tick for NGX lifetime.

Note what this means for the render resolution: **the plugin chooses it.** It does not adapt to a
screen percentage the game set; it installs an `FLegacyScreenPercentageDriver` with the fraction
`NGX_DLSS_GET_OPTIMAL_SETTINGS` recommends for the selected quality mode.

### A.2 It replaces the engine's temporal pass completely. The `FTAAStandaloneCS` dispatch never runs

**This is the load-bearing finding, and the engine source is unambiguous.**

**HARD**, `Engine/Source/Runtime/Renderer/Private/PostProcess/PostProcessing.cpp:522-567`, inside
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
        "ThirdParty %s %dx%d -> %dx%d", ... );

    ITemporalUpscaler::FPassInputs UpscalerPassInputs;
    UpscalerPassInputs.bAllowDownsampleSceneColor = bAllowSceneDownsample;
    UpscalerPassInputs.DownsampleOverrideFormat   = DownsampleOverrideFormat;
    UpscalerPassInputs.SceneColorTexture          = SceneColor.Texture;
    UpscalerPassInputs.SceneDepthTexture          = SceneDepth.Texture;
    UpscalerPassInputs.SceneVelocityTexture       = Velocity.Texture;

    UpscalerToUse->AddPasses(
        GraphBuilder, View, UpscalerPassInputs,
        &SceneColor.Texture, &SecondaryViewRect,
        &HalfResolutionSceneColor.Texture, &HalfResolutionSceneColor.ViewRect);
}
```

There is **no branch between "built-in TAA" and "custom upscaler."** The engine's own TAA is itself
an `ITemporalUpscaler` implementation, and `UpscalerToUse` selects one object. Exactly one
`AddPasses` runs.

`TemporalAA.cpp:1523-1574` is the default:

```cpp
class FDefaultTemporalUpscaler : public ITemporalUpscaler
{
public:
    virtual const TCHAR* GetDebugName() const { return TEXT("FDefaultTemporalUpscaler"); }

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

`AddGen4MainTemporalAAPasses` (`TemporalAA.cpp:1433-1519`) calls `::AddTemporalAAPass` at
`TemporalAA.cpp:1490`, and `AddTemporalAAPass` (`TemporalAA.cpp:629-978`) is the function that
issues the `FTAAStandaloneCS` dispatch — **the exact dispatch this project intercepts.**

> **So when NVIDIA's plugin is registered, `FTAAStandaloneCS` is never dispatched at all.** The
> engine calls `FDLSSUpscaler::AddPasses` in its place. The official architecture is a *replacement*,
> not an *interception*: the shader we hook does not run in the official configuration.

The selecting cvar is `r.TemporalAA.Upscaler` (`TemporalAA.cpp:70-76`), default **1**:

```
 0: Forces the default temporal upscaler of the renderer;
 1: GTemporalUpscaler which may be overridden by a third party plugin (default).
```

Do not confuse it with `r.TemporalAA.Upsampling`, the cvar this project already depends on
(`CLAUDE.md` §2.3.1, §4). They are different cvars with different jobs: `Upsampling` decides whether
the primary screen-percentage method is `TemporalUpscale`, `Upscaler` decides which
`ITemporalUpscaler` object gets called.

**And NVIDIA's plugin asserts on the first of those.** `DLSSUpscaler.cpp:317-318`:

```cpp
checkf(!PassInputs.bAllowDownsampleSceneColor, TEXT("The DLSS plugin does not support downsampling the scenecolor. Please set r.TemporalAA.AllowDownsampling=0"));
checkf(View.PrimaryScreenPercentageMethod == EPrimaryScreenPercentageMethod::TemporalUpscale, TEXT("DLSS requires TemporalUpscale. If you hit this assert, please set r.TemporalAA.Upscale=1"));
```

**HARD.** DLSS requires `EPrimaryScreenPercentageMethod::TemporalUpscale` — the same engine state
`CLAUDE.md` §5 establishes Stray is already in at every screen percentage, because Stray ships
`r.TemporalAA.Upsampling=True`. Our target game is, by the plugin's own precondition, in a valid
configuration for official DLSS.

**Epic's own documentation agrees**, though it is UE5-era. **HARD** as a statement by Epic, from
"Temporal Upscalers in Unreal Engine" (`dev.epicgames.com/documentation/en-us/unreal-engine/temporal-upscalers-in-unreal-engine`):

> "Temporal upscalers all work the same with Unreal Engine, whether they are Unreal Engine 4's
> Temporal Anti-Aliasing Upscaling (TAAU), Unreal Engine 5's Temporal Super Resolution, or a
> third-party distributed plugins such as NVIDIA's DLSS 2+ Super Resolution, AMD's FSR 2.0+, and
> Intel's XeSS."

and the API reference for `ITemporalUpscaler::AddPasses`:

> "Adds the necessary passes into RDG for temporal upscaling the rendering resolution to desired
> output res."

### A.3 Where it sits in the chain: after depth of field, before motion blur, bloom and the tonemapper

**HARD**, all from `PostProcessing.cpp`, same function, absolute line numbers:

| Line | Pass |
|---|---|
| 490 | `DiaphragmDOF::AddPasses(...)` — depth of field |
| **523-567** | **the `ITemporalUpscaler::AddPasses` call** |
| 620 | `AddMotionBlurPass(...)` |
| 710 | `AddBloomPass(...)` |
| 777 / 844 | `AddTonemapPass(...)` (two branches) |

Epic states the same thing in prose, **HARD** as Epic's assertion, from "Anti-Aliasing and Upscaling
in Unreal Engine": the upscalers "all plug into the post-processing chain in the same location —
between Depth of Field and Motion Blur."

**Therefore the official plugin's input and output are pre-tonemap, scene-linear HDR** — the same
colour space our hook sees, because it is *the same point in the frame*. Neither Epic page states
the colour space in those words; that specific phrasing is **UNCONFIRMED** from Epic, but it follows
directly from the ordering above, which is HARD, and it matches this project's own measurements
(`CLAUDE.md` §2.3: `R16G16B16A16_FLOAT` scene colour carrying pre-exposure).

Resolution: input at render resolution (`View.ViewRect`), output at the secondary/output rect. The
plugin computes both in its own pass-parameter struct, `DLSSUpscaler.cpp:52-56`:

```cpp
FDLSSPassParameters(const FViewInfo& View)
    : InputViewRect(View.ViewRect)
    , OutputViewRect(FIntPoint::ZeroValue, View.GetSecondaryViewRectSize())
{
}
```

And its `GetOutputExtent()` (`DLSSUpscaler.cpp:132-145`) is a **line-for-line copy of UE's own
`FTAAPassParameters::GetOutputExtent()`**, including the `Max(InputExtent, QuantizedUpscaleViewSize)`
that `CLAUDE.md` §5 dissects at length:

```cpp
FIntPoint InputExtent = SceneColorInput->Desc.Extent;
FIntPoint QuantizedPrimaryUpscaleViewSize;
QuantizeSceneBufferSize(OutputViewRect.Size(), QuantizedPrimaryUpscaleViewSize);
return FIntPoint(FMath::Max(InputExtent.X, QuantizedPrimaryUpscaleViewSize.X),
                 FMath::Max(InputExtent.Y, QuantizedPrimaryUpscaleViewSize.Y));
```

Our own matcher's `Max()` analysis was reasoning about NVIDIA's own arithmetic without knowing it.

### A.3.1 The official plugin writes its output into the engine's TAA history — the same feedback node we sit on

**HARD**, `DLSSUpscaler.cpp:519-536`, at the end of `AddDLSSPass`:

```cpp
if (!View.bStatePrevViewInfoIsReadOnly && OutputHistory)
{
    OutputHistory->SafeRelease();

    GraphBuilder.QueueTextureExtraction(Outputs.SceneColor, &OutputHistory->RT[0]);

    OutputHistory->ViewportRect = DestRect;
    OutputHistory->ReferenceBufferSize = OutputExtent;
}

if (!View.bStatePrevViewInfoIsReadOnly && OutputCustomHistoryInterface)
{
    if (!OutputCustomHistoryInterface->GetReference())
    {
        (*OutputCustomHistoryInterface) = new FDLSSUpscalerHistory(DLSSState);
    }
}
```

with `OutputHistory` bound at `DLSSUpscaler.cpp:326` to
`&(View.ViewState->PrevFrameViewInfo.TemporalAAHistory)`.

**The official plugin extracts DLSS's own output as `TemporalAAHistory.RT[0]`.** That is the exact
resource `CLAUDE.md` §5 identifies as the one screen-space reflections read next frame
(`ScreenSpaceRayTracing.cpp:596-620`). NVIDIA does deliberately what our `u0` write does as a
side effect. It is not an accident of our hook point and it is not something the official
architecture avoids — it is the contract.

The plugin *also* keeps a second, private history: `FDLSSUpscalerHistory` implements the engine's
`ICustomTemporalAAHistory` interface (`DLSSUpscalerHistory.h:29-30`) and carries the `FDLSSStateRef`
— the NGX feature handle. So there are two histories, the engine-visible colour texture and the
opaque NGX state, and the plugin threads both.

### A.4 Jitter, velocity, exposure, mip bias and the screen-percentage settings

**Jitter — passed straight through, unmodified.** `DLSSUpscaler.cpp:441` and `:471`:

```cpp
const FVector2D JitterOffset = View.TemporalJitterPixels;
...
DLSSArguments.JitterOffset = JitterOffset;
```

**HARD**, and it confirms `CLAUDE.md` §2.7's rule from NVIDIA's own code: `TemporalJitterPixels` is
what NGX wants, with no sign flip and no scaling.

**Velocity — a dedicated combine pass, and it is the shader our resolve was modelled on.**
`DLSSUpscaler.cpp:337-344`:

```cpp
const bool bDilateMotionVectors = CVarNGXDLSSDilateMotionVectors.GetValueOnRenderThread() != 0;
FRDGTextureRef CombinedVelocityTexture = AddVelocityCombinePass(GraphBuilder, View, PassInputs.SceneDepthTexture, PassInputs.SceneVelocityTexture, bDilateMotionVectors);
DLSSParameters.SceneColorInput = PassInputs.SceneColorTexture;
DLSSParameters.SceneVelocityInput = CombinedVelocityTexture;
DLSSParameters.SceneDepthInput = PassInputs.SceneDepthTexture;
DLSSParameters.bHighResolutionMotionVectors = bDilateMotionVectors;
```

The cvar, `DLSSUpscaler.cpp:77-82`, defaults to **dilated high-resolution** vectors:

```
r.NGX.DLSS.DilateMotionVectors
 0: pass low resolution motion vectors into DLSS
 1: pass dilated high resolution motion vectors into DLSS. This can help with improving image quality of thin details. (default)
```

The non-dilated branch of `DLSS/Shaders/Private/VelocityCombine.usf` is the arithmetic
`CLAUDE.md` §5 already tells us to copy rather than invent, and here it is at the source:

```hlsl
float4 EncodedVelocity = VelocityTexture[PixelPos];
float Depth = DepthTexture[PixelPos].x;

float2 Velocity;
if (all(EncodedVelocity.xy > 0))
{
    Velocity = DecodeVelocityFromTexture(EncodedVelocity).xy;
}
else
{
    float4 ClipPos;
    ClipPos.xy = SvPositionToScreenPosition(float4(PixelPos.xy, 0, 1)).xy;
    ClipPos.z = Depth;
    ClipPos.w = 1;

    float4 PrevClipPos = mul(ClipPos, View.ClipToPrevClip);

    if (PrevClipPos.w > 0)
    {
        float2 PrevScreen = PrevClipPos.xy / PrevClipPos.w;
        Velocity = ClipPos.xy - PrevScreen.xy;
    }
    else
    {
        Velocity = EncodedVelocity.xy;
    }
}

float2 OutVelocity = Velocity * float2(0.5, -0.5) * View.ViewSizeAndInvSize.xy;
OutVelocityCombinedTexture[OutputPixelPos].xy = -OutVelocity;
```

Note `mul(ClipPos, View.ClipToPrevClip)` — the **row-vector** convention, matching `CLAUDE.md` §5's
rule and the transposition fix this project already landed. Note also the `PrevClipPos.w > 0` guard
and the final negation with `(0.5, -0.5)`. **HARD**, and it is the direct upstream of our resolve
shader.

**The scale is 1.0, and that is deliberate.** `DLSSUpscaler.cpp:473-474`:

```cpp
DLSSArguments.MotionVectorScale = FVector2D(1.0f, 1.0f);
DLSSArguments.bHighResolutionMotionVectors = Inputs.bHighResolutionMotionVectors;
```

**HARD**, and it independently confirms the third gotcha in `CLAUDE.md` §5 ("Declaring a guide's
subrect AND scaling its vectors double-counts"): NVIDIA sends 1.0 and describes the vectors'
resolution with a separate flag, never by scaling.

**Exposure — the plugin sends BOTH a scalar and a texture, and defaults to letting NGX ignore
them.** `DLSSUpscaler.cpp:444-445`, `:493-497`:

```cpp
const float PreExposure = View.PreExposure;
const bool bUseAutoExposure = CVarNGXDLSSAutoExposure.GetValueOnRenderThread() != 0;
...
check(PassParameters->EyeAdaptation);
PassParameters->EyeAdaptation->MarkResourceAsUsed();
DLSSArguments.InputExposure = PassParameters->EyeAdaptation->GetRHI();
DLSSArguments.PreExposure = PreExposure;
DLSSArguments.bUseAutoExposure = bUseAutoExposure;
```

with the cvar (`DLSSUpscaler.cpp:84-88`) defaulting to **1**:

```
r.NGX.DLSS.AutoExposure
0: Use the engine-computed exposure value for input images to DLSS - in some cases this may reduce artifacts
1: Enable DLSS internal auto-exposure instead of the application provided one (default)
```

**HARD.** So the official default is the same `AutoExposure` flag this project ships
(`CLAUDE.md` §5, feature flags `0x4B`), and NVIDIA treats the engine-exposure path as the *fallback
for artifacts*, not the primary. That is useful context for our own finding that the NGX
exposure-texture path measured inert.

**Mip bias is not computed per pass. It is a global cvar override set once at module startup.**
`DLSS/Source/DLSS/Private/DLSS.cpp:459-468`:

```cpp
// If DLSS mip bias override is enabled, we unconditionally override it here on module startup even if DLSS is not supported.
// This way you won't see inconsistent behavior for something like TAAU depending on whether DLSS is supported on the current system.
if (CVarNGXDLSSAdjustMipBiasOffsetEnable.GetValueOnGameThread())
{
    static IConsoleVariable* CVarMinAutomaticViewMipBiasOffset = IConsoleManager::Get().FindConsoleVariable(TEXT("r.ViewTextureMipBias.Offset"));
    if (CVarMinAutomaticViewMipBiasOffset != nullptr)
    {
        CVarMinAutomaticViewMipBiasOffset->Set(-1.0f, EConsoleVariableFlags::ECVF_SetByCommandline);
    }
}

static auto* CVarTemporalAAAllowDownsampling = IConsoleManager::Get().FindConsoleVariable(TEXT("r.TemporalAA.AllowDownsampling"));
if (CVarTemporalAAAllowDownsampling != nullptr)
{
    CVarTemporalAAAllowDownsampling->Set(0);
}
```

**HARD**, and it is a genuinely useful piece of intelligence: the official plugin does **not** compute
a DLSS-specific mip bias. It leans on UE's existing automatic view mip bias (which already accounts
for the screen percentage) and merely shifts its floor by −1.0. It also forces
`r.TemporalAA.AllowDownsampling=0` globally, which is the precondition its own `AddPasses` asserts.
Both are ordinary cvar writes, reachable from an injected DLL through `Engine.ini [SystemSettings]`
exactly as `CLAUDE.md` §2.2 describes.

**`r.TemporalAA.Upsampling` itself is never referenced by the `ITemporalUpscaler` call site.**
`PostProcessing.cpp` mentions only `r.TemporalAA.Upscaler`. **UNCONFIRMED** whether the plugin reads
`Upsampling` anywhere; what it does instead is assert on the *consequence*
(`PrimaryScreenPercentageMethod == TemporalUpscale`) and install its own screen-percentage driver.

### A.5 The one thing the plugin does NOT do through `ITemporalUpscaler`: denoising

**HARD.** `DLSS/Source/DLSS/Private/DLSSDenoiser.h:29-30`:

```cpp
// wrapper for the default denoiser to add TAA after some passes
class DLSS_API FDLSSDenoiser final : public IScreenSpaceDenoiser
```

The DLSS2/3-era plugin registers a **second, separate** engine extension point for denoising —
`IScreenSpaceDenoiser`, the interface behind `FSSDTemporalAccumulationCS`, the shader family this
project measured and named in `CLAUDE.md` §2.3. NVIDIA's own architecture already treats
"upscale" and "denoise" as two distinct slots in the frame, filled by two distinct objects.

Neither mirror contains DLSS Ray Reconstruction or `DLSSD`. `gh search code` for `DLSSD` /
`RayReconstruction` across `mrcasty/DLSS4-alpha-unreal` returns nothing despite its DLSS4 vintage,
whose scope its own README limits to an alpha-channel fix. **So the UE extension point for RR is
UNCONFIRMED from these mirrors.**

## B. DLSS Frame Generation in the official plugin (Streamline)

**Source**: `NVIDIA-RTX/Streamline`, `docs/ProgrammingGuideDLSS_G.md` and
`docs/ProgrammingGuideManualHooking.md`, Streamline version 2.12.0 (both files carry that version
header). **HARD** throughout unless marked.

### B.1 Not `ITemporalUpscaler`. A swapchain-level API, engine-agnostic, fed by `ISceneViewExtension` in Unreal

The DLSS-G guide never mentions Unreal Engine, `ITemporalUpscaler` or `ISceneViewExtension` — it is
written entirely in terms of the engine-agnostic Streamline (`sl::`) API: `slSetTagForFrame`,
`slDLSSGSetOptions`, `slEvaluateFeature`, `slSetConstants`. **UNCONFIRMED from this doc** how any
specific engine wires it up, by construction — this is the SDK layer beneath the engine, not an
engine integration.

Epic's own FAQ closes that gap directly, and this is the mechanism the task set out to establish.
**HARD**, Epic Games, "Temporal Super Resolution Frequently Asked Questions for Unreal Engine"
(`dev.epicgames.com/documentation/unreal-engine/temporal-super-resolution-frequently-asked-questions-for-unreal-engine`),
under "Can TSR work with third-party frame interpolation / generation?":

> "The frame interpolation / generation plugin [is] free to access depth and motion vector because
> of `ISceneViewExtensions` without needing to set up a `ITemporalUpscaler` to replace TSR."

So Epic states explicitly, for its own engine, that frame generation is architecturally
**independent of the temporal-upscaler slot** — a *different* extension point,
`ISceneViewExtension`, supplies the buffers. This project's own hook (§2.3) captures depth and
velocity at the TAA dispatch; the official architecture instead has a scene-view extension read
them wherever the engine already produces them, with no need to sit inside — or replace — the
upscale pass at all.

**Confirmed independently in UE 4.27.2's own source**, and it is a stronger form of the same fact
than a doc quote: `Engine/Source/Runtime/Engine/Public/SceneViewExtension.h:101-108` defines

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

with `virtual void SubscribeToPostProcessingPass(EPostProcessingPass Pass, FAfterPassCallbackDelegateArray& InOutPassCallbacks, bool bIsPassEnabled) {}` (`:161`) as one of `ISceneViewExtension`'s
overridable hooks, alongside `PrePostProcessPass_RenderThread` (`:156`) which hands the extension
`const FPostProcessingInputs& Inputs` directly. `PostProcessing.cpp:460-465` calls
`View.Family->ViewExtensions[ViewExt]->SubscribeToPostProcessingPass(...)` for every registered
extension, for every one of those four pass points, every frame — **this exists in 4.27.2 itself**,
not just in the UE5 branch Epic's FAQ describes. A view extension in this engine version can
already read depth, velocity and scene colour, and inject work after motion blur or after the
tonemapper, with no `ITemporalUpscaler` registration and no replacement of anything.

### B.2 Presentation: an automatic swapchain interposer by default, or explicit hooks under manual mode — never a game-thread dispatch inside another pass

**HARD**, `ProgrammingGuideDLSS_G.md:618`, §6.7: *"DLSS-G intercepts `IDXGISwapChain::Present` and
when using Vulkan `vkQueuePresentKHR` and `vkAcquireNextImageKHR` calls and executes them
asynchronously."* Default integration is a **swapchain proxy**: the game calls the DXGI/Vulkan
present functions it always called, and Streamline's interposer (`sl.interposer.dll`) is what
actually receives them.

`ProgrammingGuideManualHooking.md` documents the alternative, for hosts that cannot tolerate a
global API-redirection proxy (§0: *"unnecessary overhead caused by the entire API redirection
through SL proxies"* and *"problems with tools and 3rd party libraries which do not expect to
receive SL proxies as inputs"*). Manual hooking is not a different *place* in the frame — it is a
different *mechanism* for reaching the same seven call sites, enumerated exhaustively in
`FunctionHookID` (§2.0):

```
eIDXGIFactory_CreateSwapChain, eIDXGIFactory_CreateSwapChainForHwnd,
eIDXGIFactory_CreateSwapChainForCoreWindow,
eIDXGISwapChain_Present, eIDXGISwapChain_Present1, eIDXGISwapChain_GetBuffer,
eIDXGISwapChain_ResizeBuffers, eIDXGISwapChain_ResizeBuffers1,
eIDXGISwapChain_GetCurrentBackBufferIndex, eIDXGISwapChain_SetFullscreenState,
eID3D12Device_CreateCommandQueue, eVulkan_Present, eVulkan_CreateSwapchainKHR, ...
```

The host must route each of those specific calls through an SL-provided proxy obtained via
`slUpgradeInterface`/`slGetNativeInterface` and, critically, **must itself invoke the common
plugin's `presentCommon()` exactly once per frame** (§2.0, boxed WARNING) — manual hooking still
funnels through the same internal present-time entry point, it just removes SL's own DXGI/Vulkan
proxy layer for every *other* call. **Either way, DLSS-G's evaluate happens at present, on
Streamline's own command list/queue plumbing — never inside a game-authored dispatch the way this
project's SR and NR hooks currently do.** That is the one point on which our own FG (already a
stage on our own command list at Present, per this project's design) already matches the official
architecture exactly.

**State restore after evaluate is the host's job, spelled out precisely, and it is our own restore
list.** `ProgrammingGuideManualHooking.md` §7.0, verbatim:

> "When manual hooking is used the host application is no longer using an SL proxy for the command
> lists (CL), hence it is not possible for SL to restore the CL state after each `slEvaluateFeature`
> call."

followed by the exact restoration code SL performs internally in the non-manual path — descriptor
heaps, root signature, every root parameter type (descriptor table, CBV, SRV, UAV, 32-bit
constants), and PSO/state-object:

```cpp
if (cmdList->m_numHeaps > 0)      cmdList->SetDescriptorHeaps(cmdList->m_numHeaps, cmdList->m_heaps);
if (cmdList->m_rootSignature)     cmdList->SetComputeRootSignature(cmdList->m_rootSignature);
                                   // ... root descriptor tables, CBVs, SRVs, UAVs, 32-bit constants
if (cmdList->m_pso)               cmdList->SetPipelineState(cmdList->m_pso);
if (cmdList->m_so)                static_cast<ID3D12GraphicsCommandList4*>(cmdList)->SetPipelineState1(cmdList->m_so);
```

closing with: *"Failure to restore command list(buffer) state correctly will cause your
application to crash or misbehave in some other form."*

**This is independent confirmation, from NVIDIA's own SDK, of the exact hazard `CLAUDE.md` §5
calls "the number-one corruption risk"** for our own NGX evaluate: descriptor heaps, root
signature, PSO, root params. NVIDIA's own interposer carries this same list as a first-class,
documented responsibility for any evaluate call, whether ours or theirs.

### B.3 The buffer contract: exact timing, the blending formula, and the documented cost of not separating UI

**HARD**, `ProgrammingGuideDLSS_G.md` §5.1–5.2. Required: Depth, Motion Vectors, and the backbuffer
(captured automatically by the swapchain proxy). For quality, **critically** two more:

> "For the best image quality, it is **critical** to provide a Hudless (pre-UI) buffer and a UI
> buffer. The frame generation algorithm can use these resources to reduce distortion of HUD
> elements during interpolation.
>
> - **Hudless** - The scene color *before* any UI/HUD elements are drawn.
> - **UI Buffer** - Choose one of: **UI Alpha** (Preferred, single-channel, most performant) or
>   **UI Color and Alpha** (full 4-channel). *If both buffers are tagged, Streamline will use the UI
>   Alpha buffer.*"

The tagging-time instruction, embedded directly in the code sample (§5.2), states exactly where in
the frame each capture must happen:

```cpp
// After post-processing pass but before UI/HUD is added tag the hud-less buffer
sl::ResourceTag hudLessTag = sl::ResourceTag {&hudLess, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eValidUntilPresent, &fullExtent };
```

i.e. **after** the full post-process chain (tonemap, bloom, colour grading — everything) and
**before** UI compositing. That is a *different* point in the frame from where DLSS SR sits
(§A.3: between depth of field and motion blur, pre-tonemap linear HDR) — confirming that Super
Resolution and Frame Generation are tapped at two structurally different stages even inside one
official pipeline, consistent with B.1's finding that FG does not go through the upscaler slot at
all.

**The blending contract is a literal formula, and the constraint is Porter-Duff over-compositing,
stated exactly** (§5.1 table, "UI Alpha OR UI Color and Alpha" row):

> "Must be 0.0 for pixels with no UI elements. Alpha must be non-zero for pixels with UI. Values
> provided must respect the standard blending formula:
> `Final_Color.RGB = UI.RGB + (1 - UI.Alpha) x Hudless.RGB`. When UI color is provided, the RGB
> channels must be pre-multiplied by alpha."

**Enabling this fully — "User Interface Recomposition" — is opt-in and interpolates the HUD and
scene as two separate streams**, §6.6:

> "When both Hudless and a UI buffer are tagged, User Interface Recomposition can be enabled by
> setting `DLSSGOptions::enableUserInterfaceRecomposition = eTrue`. When enabled, the HUD and scene
> are interpolated separately and composited later, providing significantly-improved UI
> interpolation quality. Using user interface recomposition has a slight performance and memory
> cost."

### B.4 What happens when a title cannot separate UI — the documented cost

**HARD**, §5.1, the "Hudless" row of the requirements table, stated as a direct consequence rather
than a separate fallback mode:

> "Should contain the full viewable scene, without any HUD/UI elements in it. **If some HUD/UI
> elements are unavoidably included, expect some image quality degradation on those elements.**"

There is no documented alternate code path for "can't separate UI" — Hudless/UI tagging is simply
optional (only Depth, Motion Vectors and the backbuffer are marked required in §5.1), and omitting
it is the fallback: DLSS-G still runs, generated frames still interpolate the *whole* composited
image including any UI baked into it, and the cost is confined to whatever moved with the UI
during interpolation (ghosting/smearing on HUD elements, since the interpolator has no way to know
those pixels should not move with the scene). The integration checklist (§0.0) lists "All the
required inputs are passed to Streamline: depth buffers, motion vectors, HUD-less color buffers"
as a checklist item, not gated behind a hard requirement — consistent with Hudless being a quality
opt-in, not a functional prerequisite.

**One more documented failure mode worth carrying into this project's own thinking**, §5.2:

> "If validity of tagged resources cannot be guaranteed (for example game is loading, paused, in
> menu, playing a video cut scene etc.) all tags should be set to null pointers to avoid stability
> or IQ issues."

— the same menu/load-screen caution this project already independently discovered and encodes in
`CLAUDE.md` §2.4 and §5 ("the menu runs the TAA pass too... at uncapped fps").

---

## C. DLSS Ray Reconstruction, briefly

**Source**: `NVIDIA-RTX/Streamline`, `docs/ProgrammingGuideDLSS_RR.md`, same version 2.12.0.
**HARD.**

**RR occupies the SAME call site as the temporal upscaler — it is chosen INSTEAD of TAAU, not
layered after a denoiser.** §7.0, "ADD DLSS-RR TO THE RENDERING PIPELINE", is explicit and the
sample code makes the substitution literal:

> "On your rendering thread, call `slEvaluateFeature` at the appropriate location where up-scaling
> is happening."

```cpp
if(useDLSSD)
{
    const sl::BaseStructure* inputs[] = {&myViewport};
    if(SL_FAILED(result, slEvaluateFeature(sl::kFeatureDLSS_RR, *frameToken, inputs, _countof(inputs), myCmdList)))
    {
        // Handle error
    }
    else
    {
        // IMPORTANT: Host is responsible for restoring state on the command list used
        restoreState(myCmdList);
    }
}
else
{
    // Default up-scaling pass like for example TAAU goes here
}
```

**"Default up-scaling pass like for example TAAU goes here" is the single most important line for
this section**: it places DLSS-RR in exactly the branch that would otherwise hold the engine's own
temporal upscaler — the same slot `ITemporalUpscaler::AddPasses` fills in Unreal (§A.2). RR is not
a post-process layered after the denoiser and the upscaler; it **is** the denoiser-and-upscaler,
called once, in their place.

**Its inputs confirm the same reframing.** §4.1.5, "Color Input": *"This is the **Noisy Ray Traced
Input Color**. Any standard 3-channel format provided at input resolution"* — i.e. RR's colour
input is explicitly the **undenoised** signal, at render resolution, not a post-SSD image. §4.1.14,
"Output": *"Destination for the Denoised full resolution frame"* — denoised **and** upscaled to
output resolution in one evaluate. Between those two it also takes diffuse albedo, specular
albedo, packed normal+roughness, specular motion vectors and specular hit distance (§4.1.1–4.1.9)
— G-buffer-shaped guides a screen-space denoiser and an upscaler would each want separately, fed to
one network instead of two passes.

State restoration after RR's evaluate is the identical documented obligation as DLSS-G's (§7.0
again references `ProgrammingGuideManualHooking.md` §7.0 directly): *"host is responsible for
restoring the command buffer(list) state after calling `slEvaluate`."*

**Consequence for how we should read our own NR path.** This project's neural-rendering feature
(feature 18, `CLAUDE.md`'s extensive NR sections) is architecturally closer to DLSS-RR than to
DLSS-SR or DLSS-G — a display-adjacent neural pass consuming colour plus guides, once — but our
current hook site (immediately after SR, still pre-tonemap) is **not** where the official RR
pipeline puts the analogous pass. RR's own contract wants the noisy, undenoised, pre-upscale
colour as input, i.e. it replaces the upscale-and-denoise step in the same place `ITemporalUpscaler`
would sit (§A.3) — not a stage bolted on after it. Neither Streamline guide describes anything
resembling this project's own HDR soft-clip/sRGB codec (§2.9 "The near-black NR output..." in
`CLAUDE.md`), because RR's networks are trained end-to-end on the noisy/denoised domain directly
rather than being handed a display-referred proxy of a already-upscaled image — a structural
difference this project's own investigation already discovered from first principles and is worth
keeping in mind as a difference, not an oversight, of our own port.

## D. What this means for stray-dlss: hook or stage, per feature

Grounded in A–C above, not opinion. Each answer cites the finding it rests on.

**1. Super Resolution: our placement is the same point in the frame as the official plugin's, but
reached by a different mechanism — and the official architecture gives no support for moving it.**

Colour space, resolution semantics and even the destination resource match exactly. Official DLSS
SR runs between depth of field and motion blur, pre-tonemap, scene-linear HDR (§A.3) — precisely
where our `FTAAStandaloneCS` hook already sits, because that dispatch *is* the pass being replaced.
Official DLSS SR also writes its result into `TemporalAAHistory.RT[0]` (§A.3.1), the identical
resource our `u0` write feeds into next frame's history — NVIDIA does this on purpose, not as a
side effect of a hook point it didn't choose. So there is no daylight between "where NVIDIA's SR
sits" and "where our SR sits": placement is right.

The *mechanism* differs, and that difference is the entire hook-versus-native-extension question.
Official SR reaches this point by being called from a first-class `ITemporalUpscaler::AddPasses`
invocation the engine itself branches to (§A.2) — a real RDG pass, with `FViewInfo` handed in
directly, no descriptor-table archaeology required. We reach the same point by intercepting a
compute dispatch the engine issued for its *own* TAA and substituting our work inside it, then
reading the View constant buffer by raw offset because nothing hands us an `FViewInfo`. **Nothing
in A–C suggests our SR should become "a stage" in the sense of moving to a different point in the
frame** — the frame position is right. What differs from official is only *how* we arrive there,
and there is no ReShade-reachable way to arrive by the official route (see point 3).

**2. `ITemporalUpscaler` is real, engine-native, and reachable only by hooking the engine itself —
not by anything ReShade's D3D12 event model exposes.**

The interface (§A.1) is registered on `FSceneViewFamily` before `AddPostProcessingPasses` runs,
via `SetTemporalUpscalerInterface()` — a plain C++ virtual-dispatch call the engine's own game-
thread view-family setup code makes (`ICustomStaticScreenPercentage::SetupMainGameViewFamily` in
NVIDIA's plugin, §A.1). Everything about it — the header it's declared in
(`Renderer/Private/PostProcess/TemporalAA.h`, not even a public header in 4.27.2), the `FViewInfo&`
and `FRDGBuilder&` parameters `AddPasses` receives, the `FSceneViewFamily` member it plugs into —
is pure engine C++ state, invisible to a graphics-API interception layer. ReShade's add-on events
(`init_pipeline`, `bind_pipeline`, `dispatch`, ...) operate at the D3D12 command-list level and
never see an `FViewInfo`, an `FSceneViewFamily`, or an `FRDGBuilder`; there is no `QueryInterface`
or descriptor trick that reaches them, because they are never marshalled through D3D12 at all.

**Using the real extension point from an injected DLL would require actual code execution inside
the engine's own address space with knowledge of its C++ types and symbol layout** — the kind of
access this project's UE4SS migration research (`docs/RESEARCH-UE4SS-MIGRATION.md`) already exists
to evaluate, not something reachable while staying inside ReShade's device/command-list hooking
model. This is a materially bigger step than converting a hook into a same-API-level "stage" (a
different D3D12 command list at a different D3D12-visible point, e.g. present); it is a change of
*what kind of program* is doing the interception. Worth naming as a real third option distinct from
"hook vs. stage," but not something decidable from ReShade alone.

**3. Ray Reconstruction, the closest official analogue to our NR, sits at the SAME pre-tonemap slot
SR does — and Frame Generation's post-tonemap tap is a read-only capture, not a neural pass.**

This bears directly on the "should NR move to a post-tonemap, pre-UI stage" question, and the
honest answer has two parts.

*Nothing in the official architecture puts a neural pass where we're considering moving ours.*
DLSS-RR replaces the upscale-and-denoise step in the identical slot as SR — "Default up-scaling
pass like for example TAAU goes here" is the alternate branch to RR's evaluate (§C) — consuming
undenoised, pre-upscale colour, not a display-referred image. The only official feature that
touches anything post-tonemap is Frame Generation, and what it takes there is a **read-only
Hudless capture** of the fully composited pre-UI frame (§B.3: "after post-processing pass but
before UI/HUD is added") — not a neural network evaluate that writes a residual back into the
frame graph. So moving our NR post-tonemap would not be reproducing anything NVIDIA does; it would
be a bespoke placement, justified entirely by our own measured feedback-loop problem
(`CLAUDE.md`'s SSR/eye-adaptation sections), not by precedent.

*That said, the case for it stands on its own, and the official docs strengthen one part of it
independently.* `CLAUDE.md` already derives, from UE 4.27.2 source, that nothing after the
tonemapper is carried into next frame's `PrevFrameViewInfo` — a genuine engine fact, not an
official-plugin fact, and it is what makes a post-tonemap NR site free of the feedback loop our
pre-tonemap NR hook has. NVIDIA's own state-restoration requirement (§B.2: heaps, root signature,
every root-parameter kind, PSO) applies identically wherever a neural evaluate is injected — moving
NR's *hook site* changes nothing about that cost, because it is a property of running NGX from
outside engine-authored command recording, not of *where* in the frame that happens. So: no
official precedent for the destination, but no official obstacle either, and the reasons this
project already has for the move (feedback-loop closure, HDR codec no longer needed once
display-referred) stand independent of anything found here.

**4. Frame Generation already matches the official architecture, and that is worth stating plainly
rather than re-deriving.** DLSS-G's evaluate happens at present time, through either an automatic
swapchain interposer or explicit manual-hook calls funnelled through the same `presentCommon()`
(§B.2) — never inside a game-authored dispatch. This project's FG already runs as a stage on its
own command list at Present. That is the one feature where "should this be a stage" is already
settled, by both the official architecture and our own implementation, in the same direction.
