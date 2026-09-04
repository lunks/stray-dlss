# Should we BE the engine's temporal upscaler? Re-asked with the doors we now have — and no

**The question, the user's framing:** *"Should we stop intercepting the engine's temporal upscale and
instead BE the engine's temporal upscaler?"* `ITemporalUpscaler` is an interface UE 4.27 invites third
parties to implement — `FSceneViewFamily::SetTemporalUpscalerInterface` (`SceneView.h:1807`) has **no
caller anywhere in the engine**, and `PostProcessing.cpp:535-566` prefers a registered upscaler over
the default one whenever `r.TemporalAA.Upscaler` is non-zero, which it is by default. NVIDIA's own
DLSS plugin for 4.27 works exactly this way.

**Why this is a fresh question rather than a settled one.** `docs/RESEARCH-ENGINE-TAA-HOOK.md` §4.3
declined this rung ("L2 — OWN IT") in one paragraph, on two grounds: that `AddPasses` would need RDG
pass authoring with no callable ABI, and that reading `const FViewInfo&` means reproducing a
Renderer-private layout. **Both grounds have moved.** The `FViewInfo` objection is dead — we read
`FViewInfo+5768` every frame today (`view_cached`, facts §36.22) and it is HARD. And three doors have
opened since: the seam is live and authoritative, the RHI bind stream names every register of the
pass, and the pool-name route makes render targets addressable by name. So the refusal is re-derived
here from scratch.

**Nothing in this document has run.** There was no box. Every engine claim carries `path:line`.

---

## 0. Verdict

**NO — and the reason is not the one the old refusal gave.**

| | |
|---|---|
| **Is registration feasible?** | **Yes, and it is a red herring.** We do not need it: the vtable patch we already ship *is* the `AddPasses` seat (§2). Registration adds two `FSceneView`-constructor side effects Stray does not need, and requires locating a null pointer in `.bss` with no self-validating constant |
| **Is implementing the body feasible?** | **No.** Not because `FRDGBuilder::AddPass` is a template — that was the old reason and it is only half of it — but because **every non-template way into RDG's pass list needs `FRDGPass`'s layout, `FRDGParameterStruct`'s layout, and a write into `FRDGBuilder`'s private `Passes` registry, none of which has a self-validating constant** (§4). And `QueueTextureExtraction`, which the history contract requires, is **`inline` in `RenderGraphBuilder.inl` and therefore has no callable symbol at all** (§4.3, new) |
| **Does any door we now have remove the need?** | **No — and two of them would be DESTROYED by it.** The RHI bind stream and the `u0` hook are both anchored on the engine's own TAA dispatch. Being the upscaler deletes that dispatch, and with it the bracket they key on (§7.2) |
| **What would it delete?** | **Almost nothing that the replan does not already delete by routes we hold.** Its unique deletion is the `u0` hook (~2 851 lines, in flight) — bought by writing an RDG pass we cannot write (§7) |
| **What would it risk?** | **The loss of every fallback.** Nine named refusal paths and the ~900-frame NGX lazy-init window currently cost *one frame of the engine's own TAA each*. As the registered upscaler there is no engine TAA: a decline leaves the frame un-upscaled with the post chain's rects mismatched (§8) |
| **Is a ladder expressible?** | **Yes — "register and delegate" is expressible** (`GetDefaultTemporalUpscaler`'s function-local static is findable from its vtable, which we already have). **And it is exactly what we ship today**, reached by a smaller mechanism |

**The one-sentence version.** *We already occupy the `AddPasses` seat and forward from it; the only
thing "being the upscaler" would add is the obligation to produce the output ourselves, and the
engine offers no callable way to schedule work inside its own render graph — while the engine's TAA
dispatch, which owning the pass would delete, is the vehicle our DLSS evaluate rides on.*

---

## 1. Sources

| Source | Trust |
|---|---|
| `AlexMercer-MA/UnrealEngine-4.27` @ `306a7e9` (`Build.version` 4.27.2 / `++UE4+Release-4.27`) — our exact engine version | **HARD** for anything quoted. Verified byte-identical to the local `/private/tmp/ue427` cache for every file used, so line numbers are unambiguous |
| `EpicGames/UnrealEngine` @ `4.27/3abfe77`, as quoted by `docs/RESEARCH-U0-IDENTITY.md` and `docs/RESEARCH-U0-EXTERNAL-PRIOR-ART.md` | **HARD**, inherited with its own citations |
| NVIDIA's DLSS plugin, 4.2x generation: `OldZhao/ue_test` @ `695d3c02`, `DLSSUpscaler.cpp` (838 lines, intact NVIDIA copyright) | **HARD** that the code is NVIDIA's; **SOFT** that it is exactly the 4.27 release (no `.uplugin` accompanies it; the out-parameter `AddPasses` signature dates it to 4.26/4.27). **The repo owner inserted texture-dumping code** at `:39-41`, `:295-355`, `:437-450`, `:581-583`, `:588-640` — subtracted from every reading below |
| NVIDIA's DLSS plugin v2.3.2 for UE5: `flygod1159/Nvidia-DLSS-Plugin` @ `f8304b66` | **HARD**; used only to separate NVIDIA's code from the third party's, by diff |
| GitHub code search over the mirror (used for "is this the only implementer") | **SOFT** for absences; the queries are named in §6.1 |

---

## 2. The reframing that changes the question: we already hold the seat

`src/engine_seam_hook.cpp:140-228` patches slot 2 of the `ITemporalUpscaler` vtable and its
`add_passes_thunk` is called by the engine, on the render thread, with `FRDGBuilder&`,
`const FViewInfo&`, `FPassInputs` and all four out-parameters. It reads `FPassInputs`, resolves the
engine's depth and velocity to `ID3D12Resource*`, **forwards unconditionally**, and then reads the
rect the engine wrote back.

```cpp
// src/engine_seam_hook.cpp:198-204
// Forward unconditionally. This stage OBSERVES; the engine's own upscale must still run,
// and our own dispatch interception downstream is what replaces it.
if (original != nullptr)
        original(self, graph_builder, view, pass_inputs, out_colour, out_rect,
                out_half_colour, out_half_rect);
```

**Deleting that `if` is all "being the upscaler" means.** The question is therefore not *can we get
called* — we are called, on frame 0, measured (facts §36) — but *can we write a body that satisfies
the contract*. Everything below is about the body.

Two consequences worth stating before anything else:

* **Registration (`GCustomStaticScreenPercentage`, `SetTemporalUpscalerInterface`) is not on the
  critical path.** It is a second way to reach a seat we already have. §6 assesses it anyway,
  because the brief asks, and because it turns out to be the *riskier* of the two.
* **The patch reaches strictly more call sites than registration would.** `GameViewportClient.cpp:1400`
  gates on `ViewFamily.ViewMode == VMI_Lit` and runs once per *game* view family; the vtable patch
  fires for every family that reaches `PostProcessing.cpp:559`. For Stray this is a non-difference —
  `FPostProcessing::ProcessPlanarReflection` calls `AddTemporalAAPass` directly and never touches the
  interface either way (`docs/RESEARCH-ENGINE-TAA-HOOK.md` §3.2) — but it means registration cannot
  *widen* our reach, only narrow it.

---

## 3. What `AddPasses` must actually do

### 3.1 The declaration and the call site

**HARD**, `TemporalAA.h:146-186`. Five vtable entries in declaration order: `~ITemporalUpscaler`,
`GetDebugName`, `AddPasses`, `GetMinUpsampleResolutionFraction`, `GetMaxUpsampleResolutionFraction`.
`FPassInputs` is `{bool bAllowDownsampleSceneColor; EPixelFormat DownsampleOverrideFormat;
FRDGTextureRef SceneColorTexture, SceneDepthTexture, SceneVelocityTexture;}`.

**HARD**, `PostProcessing.cpp:535-567`, the whole selection and call. `UpscalerToUse` is
`View.Family->GetTemporalUpscalerInterface()` when non-null and `r.TemporalAA.Upscaler != 0`
(default **1**, `TemporalAA.cpp:70-77`), else `ITemporalUpscaler::GetDefaultTemporalUpscaler()`.

### 3.2 The four out-parameters, and which are load-bearing — MEASURED against the consumers

| out-param | initialised to | what the engine does with it | verdict |
|---|---|---|---|
| `OutSceneColorTexture` ← `&SceneColor.Texture` | the **render-res input** | *is* scene colour for the whole post chain from `:588` on | **MANDATORY** |
| `OutSceneColorViewRect` ← `&SecondaryViewRect` | **`PrimaryViewRect`** (`:520`) | `SceneColor.ViewRect = SecondaryViewRect` (`:587-588`); drives motion blur, the downsample, the tonemapper, and `check(SceneColor.ViewRect.Size() == View.GetSecondaryViewRectSize())` at `:1058` | **MANDATORY** |
| `OutSceneColorHalfResTexture` | null | **`:626-636`: `if (!HalfResolutionSceneColor.Texture)` the engine builds it itself** with `AddDownsamplePass`, reassigning the whole `FScreenPassTexture` | **OPTIONAL — write `nullptr`** |
| `OutSceneColorHalfResViewRect` | — | overwritten with the texture at `:635` when the texture is null | **OPTIONAL** |

The engine says so in its own comment at `:525-527` (*"It may choose not to based on internal
context, in which case the output half resolution texture will remain null"*), `FDefaultTemporalUpscaler`
leaves it null on its Gen5 branch (`TemporalAA.cpp:1543`), and **NVIDIA writes `nullptr` and a zero
rect unconditionally** (`DLSSUpscaler.cpp:409-410`) with a hard
`checkf(!PassInputs.bAllowDownsampleSceneColor, TEXT("...set r.TemporalAA.AllowDownsampling=0"))` at
`:367`. **So the half-res pair is trivially satisfiable at the cost of one extra downsample pass.**
All **HARD**.

> **A cost this makes visible, and it applies to Stray specifically.** `bAllowDownsampleSceneColor`
> additionally needs `TAAParameters.bUseFast`, i.e. `r.PostProcessAAQuality == 3` (Medium)
> — `TemporalAA.cpp:1479` — and **Stray ships exactly that** (`WindowsEngine.ini:18`, CLAUDE.md
> §2.3.1). So on this title the default upscaler *does* fold the half-res downsample into the TAA
> dispatch today, and a replacement would give that back as a separate pass. Small, but real, and
> nobody has costed it.

### 3.3 The obligations that are NOT in the signature

`AddGen4MainTemporalAAPasses` (`TemporalAA.cpp:1433-1519`) and NVIDIA's `AddDLSSPass`
(`DLSSUpscaler.cpp:466-586`) agree on three more things the contract requires but does not state:

1. **Create the output texture.** NVIDIA: `FRDGTextureDesc::Create2D(OutputExtent, PF_FloatRGBA,
   FClearValueBinding::Black, TexCreate_ShaderResource | TexCreate_UAV)` →
   `GraphBuilder.CreateTexture(desc, TEXT("DLSSOutputSceneColor"))` (`:471-479`). Engine: the same
   shape at `TemporalAA.cpp:675-694`, name `kTAAOutputNames[Pass]` = `TEXT("TemporalAA")`.
2. **Author a pass that writes it.** NVIDIA: `GraphBuilder.AddPass(RDG_EVENT_NAME(...),
   PassParameters, ERDGPassFlags::Compute | Raster | SkipRenderPass, lambda)` over a
   `BEGIN_SHADER_PARAMETER_STRUCT` carrying four `SHADER_PARAMETER_RDG_TEXTURE` inputs and one
   `RDG_TEXTURE_ACCESS(SceneColorOutput, ERHIAccess::UAVCompute)` (`:100-115`, `:515-572`). The
   lambda runs on the render thread and *enqueues* another lambda via `RHICmdList.EnqueueLambda` so
   the NGX call lands on the RHI thread — the same hand-across shape our announcement already uses.
3. **Extract the history.** `OutputHistory->SafeRelease();
   GraphBuilder.QueueTextureExtraction(Outputs.SceneColor, &OutputHistory->RT[0]);
   OutputHistory->ViewportRect = DestRect; OutputHistory->ReferenceBufferSize = OutputExtent;`
   (NVIDIA `:574-586`; engine `TemporalAA.cpp:961-975`, gated on `!View.bStatePrevViewInfoIsReadOnly`).

**Point 3 is not optional and its omission is not silent-but-harmless.** `ViewState->PrevFrameViewInfo`
is cleared at `InitViews` (`SceneVisibility.cpp:3407-3416`, HARD, `RESEARCH-U0-IDENTITY.md` §3.2), so
without the extraction `RT[0]` is **null every frame**. Two consequences follow immediately:
`View.PrevViewInfo.TemporalAAHistory.IsValid()` is false forever — which is the first term of the
engine's own camera-cut OR (`TemporalAA.cpp:644`) and NVIDIA's (`DLSSUpscaler.cpp:428`) — and
**screen-space reflections lose their input**, since `RenderScreenSpaceReflections` falls through to
`InputColor = View.PrevViewInfo.TemporalAAHistory.RT[0]` (`ScreenSpaceRayTracing.cpp:596-620`,
already load-bearing in CLAUDE.md §5). A permanent reset plus broken SSR is not a degradation anyone
would ship.

---

## 4. The minimum authorable surface

### 4.1 What exists as callable code in a monolithic Shipping binary

`class RENDERCORE_API FRDGBuilder` (`RenderGraphBuilder.h:21`) — and `RENDERCORE_API` expands to
nothing in a monolithic target (`UEBuildModule.cs:545-554`, HARD), so **nothing is exported** and
every entry has to be located. But *out-of-line* members do at least exist as code to be located,
while templates and `inline` definitions do not exist at all.

| Member | Kind | Present in the binary as a callable function? |
|---|---|---|
| `CreateTexture(const FRDGTextureDesc&, const TCHAR*, ERDGTextureFlags)` | non-template, `.cpp:434-470` | **yes** |
| `RegisterExternalTexture` (2 overloads) | non-template, `.cpp:472-544` | **yes** |
| `SetupPass` / `SetupEmptyPass` / `SetupPassInternal` | non-template, `.cpp:1379/1461/1467`, **private** | **yes** |
| `SetTextureAccessFinal` | non-template | **yes** |
| `AddPass` (both overloads) | **template**, `.inl:189-251` | **no** — instantiated per call site |
| **`QueueTextureExtraction` (all three overloads)** | **`inline`, `.inl:253-282`** | **NO** |
| `Passes.Allocate<>` / `Passes.Insert` / `Allocator.AllocObject<>` | templates over private members | **no** |

**HARD**, read at each line; the `QueueTextureExtraction` row was confirmed by
`grep '^void FRDGBuilder::' RenderGraphBuilder.cpp` finding no out-of-line definition.

### 4.2 Why `AddPass` being a template is only half the problem

The old refusal stopped at "`AddPass` is a template over `FShaderParametersMetadata`, instantiated at
engine compile time, with no ABI". True, but incomplete — and the completion is what makes this a
**no** rather than a **hard**. Both overloads bottom out in **non-template members that do exist**:

```cpp
// RenderGraphBuilder.inl:241-250   (the parameterised overload)
FRDGPass* Pass = Allocator.AllocObject<LambdaPassType>(MoveTemp(Name), ParameterStruct,
        OverridePassFlags(Name.GetTCHAR(), Flags, LambdaPassType::kSupportsAsyncCompute),
        MoveTemp(ExecuteLambda));
Passes.Insert(Pass);
SetupPass(Pass);
```

So in principle: allocate an object with a vtable we author whose `ExecuteImpl` calls our code, put
it in `Passes`, call `SetupPass`. That is the honest minimum, and it needs **all** of:

| # | What we would have to author or derive | Self-validating constant? |
|---|---|---|
| 1 | `FRDGPass`'s full layout — `Name`, `ParameterStruct`, `Flags`, `Pipeline`, `Handle`, a packed bitfield, six `FRDGPassHandle`s, `Producers`, and the `FTextureState`/`FBufferState` maps (`RenderGraphPass.h:110-340`), several of them `#if RDG_ENABLE_DEBUG`-conditioned | **none** |
| 2 | A fabricated vtable: `~FRDGPass` (defaulted) and the one pure virtual `ExecuteImpl(FRHIComputeCommandList&)`. `Execute` is **not** virtual — it is a non-virtual out-of-line wrapper | **none** |
| 3 | `FRDGParameterStruct` = `{const uint8* Contents; const FRHIUniformBufferLayout* Layout;}` (`RenderGraphParameter.h:186-205`). The `Layout` comes from `ParameterStructType::FTypeInfo::GetStructMetadata()->GetLayout()` — **a static built by the `BEGIN_SHADER_PARAMETER_STRUCT` macro at engine compile time.** For the empty-parameter path it is `FEmptyShaderParameters`', a RenderCore static with no anchor | **none** |
| 4 | `FRDGEventName` (compiled down in Shipping, but its size is part of #1) | **none** |
| 5 | A write into `FRDGBuilder::Passes`, a private `TRDGPassRegistry` — needed because `SetupPassInternal` uses `Pass->Handle`, which only the registry assigns | **none** |
| 6 | Locating `SetupPass` / `SetupEmptyPass`: private, non-virtual, and their only string literals are `RDG_ENABLE_DEBUG` `checkf`s | **none** |

**Six derivations, zero validators.** Compare the two mechanisms this project *did* ship: the seam's
vtable scan requires a candidate to reproduce a name literal **and** `0.5` **and** `2.0` before
anything is installed (`docs/RESEARCH-ENGINE-TAA-HOOK.md` §4.1); the View-CB identity requires 2 448
bytes to match a buffer the engine bound, eight announcements running (§19.2). `docs/RESEARCH-ENGINE-TAA-HOOK.md`
§9's rule — *"do not call a discovered function to validate it; decode it"* — cannot even be applied
here, because what is being validated is a *layout we hand the engine*, not a function we read.

**And a wrong answer is not a refusal, it is a corrupted render graph.** RDG walks `Producers`,
merges subresource states and emits `RHITransition`s from the pass objects; a malformed one is a
wrong barrier or a freed pool element, on this stack with no debug layer to object.

### 4.3 The new blocker nobody had found: the history extraction has no symbol

`QueueTextureExtraction` is `inline` in `RenderGraphBuilder.inl:253-282`. Its body is three field
writes on the texture and an `Emplace` into the private `ExtractedTextures` map:

```cpp
Texture->bExtracted = true;
Texture->bCulled    = false;
ExtractedTextures.Emplace(Texture, OutTexturePtr);
if (Texture->AccessFinal == ERHIAccess::Unknown) Texture->AccessFinal = kDefaultAccessFinal;
```

**There is no function to locate and no function to call.** Satisfying §3.3's obligation therefore
means deriving three bitfield offsets inside `FRDGTexture` *and* the offset and internal shape of a
`TMap` inside `FRDGBuilder`, and writing into both. That is strictly worse than anything in §4.2, and
it is required — omitting it costs a permanent camera cut and SSR's history input (§3.3).

This is the single most decisive fact in the document, and it is new: it is not in
`docs/RESEARCH-ENGINE-TAA-HOOK.md` §4.3, not in `RESEARCH-U0-IDENTITY.md` §6, and not in
`RESEARCH-U0-EXTERNAL-PRIOR-ART.md` §6. **HARD**, `RenderGraphBuilder.inl:253-282` plus the absence
of an out-of-line definition in `RenderGraphBuilder.cpp`.

---

## 5. Can we produce an `FRDGTextureRef` for the output without authoring a pass?

The brief asks this directly, because it looks like the way round §4. **Follow it to the end; it
breaks, and it breaks somewhere useful to know.**

### 5.1 `CreateTexture` — callable, and the handle is useless to us

`CreateTexture` exists as code (§4.1) and can be located by UEVR's caller-literal technique with a
genuine validator: `FRDGResource::Name` is `const TCHAR* const Name` at `+8`, **unconditional in
Shipping** (`RenderGraphResources.h:61`), so the returned object must echo the name we passed. That
part is sound.

**But a `CreateTexture` handle has no RHI resource, and never will unless a pass references it.**
`ResourceRHI` is assigned in `BeginResourceRHI` → `GRenderTargetPool.FindFreeElementForRDG` →
`SetRHI`, driven by `CollectPassResources` per pass (`RenderGraphBuilder.cpp:1220-1233`, `:2053-2084`,
HARD via `RESEARCH-U0-IDENTITY.md` §1.2). If we return the handle as `*OutSceneColorTexture`, the
first *downstream* pass causes the allocation — at which point **we do not know which pool element
RDG chose** (that is precisely the `u0` problem) and we have no scheduled point at which to write it.

### 5.2 `RegisterExternalTexture` — callable, and it DOES give a live resource

This is the real loophole and it deserves to be stated at full strength:
`RegisterExternalTexture` calls `Texture->SetRHI(...)` **immediately** (`RenderGraphBuilder.cpp:513-516`),
so an externally registered texture has a live `FRHITexture*` — and therefore a live
`ID3D12Resource*` through `GetNativeResource` slot 7 — at registration time, with no pass involved.
The header even blesses the pattern: the parameterless `AddPass` doc-comment says *"It is never
permitted to access a created (i.e. not externally registered) RDG resource in an independent pass"*
(`RenderGraphBuilder.h:143-144`), i.e. an externally registered one **may** be touched outside RDG's
tracking.

Two costs, both payable:

* We need a real `FPooledRenderTarget`, not a fabricated one: `SetRHI` does
  `static_cast<FPooledRenderTarget*>(ExternalPooledTexture.GetReference())` (`:516`), so an
  `IPooledRenderTarget` of our own making is undefined behaviour. It has to come from
  `GRenderTargetPool` — which is exactly the function `praydog/UEVR` locates and hooks by its name
  argument (`RESEARCH-U0-EXTERNAL-PRIOR-ART.md` §2), so *calling* it is the same technique with the
  same validator, plus an `FPooledRenderTargetDesc` we build by hand.
* RDG asserts the registered texture arrives with `ERHIAccess::Unknown` state (`:530-532`), and would
  then emit its own transition when the first consumer reads it — so anything we wrote outside the
  graph has to leave the resource in the state RDG believes. That is the `NgxNRRestoreState` hazard
  class: a `[derived]` state constant with no validator, on the game's own resource.

### 5.3 Where the chain actually breaks: **execution, not allocation**

Suppose §5.2 succeeds and we hand back a texture whose `ID3D12Resource*` we know. **When does our
DLSS evaluate run?**

Our only handles on the game's `ID3D12GraphicsCommandList` are inside hooked list methods. Today the
answer is exact and free: the engine records a TAA dispatch at precisely the right point in the
graph, our `Dispatch` hook stands in for it, we record the NGX evaluate there and return `true`.
**Owning the pass deletes that dispatch.** What is left:

| Candidate execution point | Why not |
|---|---|
| An RDG pass of our own | §4 — the whole point |
| `ISceneViewExtension::PostRenderViewFamily_RenderThread`, which *does* run inside `Execute()` (`SceneRendering.cpp:3181-3200`, `RESEARCH-U0-EXTERNAL-PRIOR-ART.md` §3) | It is the **last** RDG pass of the scene renderer — after motion blur, bloom and the tonemapper. An upscale there is an entire post chain too late |
| `SubscribeToPostProcessingPass` / `PrePostProcessPass_RenderThread` | `EPostProcessingPass` is `{MotionBlur, Tonemap, FXAA, VisualizeDepthOfField}` (`SceneViewExtension.h:101-108`) — **no location at or near the upscale** — and `PrePostProcessPass_RenderThread` fires at `DeferredShadingRenderer.cpp:2612`, in graph *setup*, where no RHI resource exists. Both are still graph setup or too late, and both would need a pass authored inside them to do work |
| "The first game D3D12 command recorded after our announcement, on the RHI thread" | A behavioural heuristic about command ordering — exactly the class of reasoning the seam retired, re-introduced at a lower level and with no announcement to check it against |
| Our own present-time command list | Refused, **HARD**, `docs/RESEARCH-RESHADE-SHAPE-SWEEP.md` §13.4: Present is after every downstream consumer, and a mid-frame `ExecuteCommandLists` of our own list runs *before* the game's still-unsubmitted work |

**So the loophole is real on the texture side and dead on the execution side.** That is the sharpest
form of the answer, and it inverts the intuition the question starts from: *the engine's TAA dispatch
is not an obstacle we are working around, it is the vehicle we are riding.*

---

## 6. Registration, assessed anyway

### 6.1 What 4.27 offers an injected DLL

**HARD**, `CustomStaticScreenPercentage.h` (29 lines, full file read): `ICustomStaticScreenPercentage`
with `~`, `SetupMainGameViewFamily(FSceneViewFamily&) = 0`, `SetupViewFamily(...)` (defaulted),
`GetMinUpsampleResolutionFraction() const = 0`, `GetMaxUpsampleResolutionFraction() const = 0`; and
`extern ENGINE_API ICustomStaticScreenPercentage* GCustomStaticScreenPercentage`, defined as
`nullptr` in a 5-line `.cpp`.

Read once, `GameViewportClient.cpp:1400-1403`:

```cpp
if (GCustomStaticScreenPercentage && ViewFamily.ViewMode == EViewModeIndex::VMI_Lit)
{
        GCustomStaticScreenPercentage->SetupMainGameViewFamily(ViewFamily);
}
```

before the legacy screen-percentage driver is installed at `:1420` — which is what lets a plugin call
both `SetTemporalUpscalerInterface` and `SetScreenPercentageInterface` without tripping the
"already assigned" `checkf`. This is exactly where NVIDIA registers (`DLSS.cpp:441-451` sets
`GCustomStaticScreenPercentage`; `DLSSUpscaler.cpp:681-721` is `SetupMainGameViewFamily`, calling
`ViewFamily.SetTemporalUpscalerInterface(GetUpscalerInstanceForViewFamily(this, DLSSQuality))` at
`:710`).

Code search over the mirror (**SOFT** for the absences, the queries are `SetTemporalUpscalerInterface`,
`ITemporalUpscaler`, `GCustomStaticScreenPercentage`, `GTemporalUpscaler`):

* `SetTemporalUpscalerInterface` — **one hit, the declaration itself.** No engine caller.
* `GCustomStaticScreenPercentage` — three hits: header, the 5-line definition, the one call site. **No
  engine assigner.** Both globals exist purely as plugin seams.
* `FDefaultTemporalUpscaler` is the **only** `ITemporalUpscaler` implementer in the tree.
* `GTemporalUpscaler` is written once (`Renderer.cpp:56`) and **never read** — confirming
  `docs/RESEARCH-ENGINE-TAA-HOOK.md` §3.3. (NVIDIA's 4.2x plugin *does* assign it and reads its own
  assignment in `IsDLSSActive()` (`DLSSUpscaler.cpp:825-831`) — a self-check, not an engine path.)

### 6.2 What we would have to construct, and the one that has no validator

1. **An `ICustomStaticScreenPercentage` vtable of four entries** — fabricated, as UEVR fabricates
   `ISceneViewExtension`'s. Cheap and safe: we only ever hand it to the engine, never dereference
   engine memory through it.
2. **An `ITemporalUpscaler` vtable of five entries** — likewise.
3. **`GCustomStaticScreenPercentage`'s address.** This is the problem. It is a **null pointer in
   `.bss`** with no string near it, no constant to decode, and no callable neighbour. `UGameViewportClient::Draw`
   is a large function; anchoring on it is a signature scan, and `docs/RESEARCH-ENGINE-TAA-HOOK.md`
   §9 forbids installing on a guess. **A wrong write here is an arbitrary 8-byte store into engine
   globals with no diagnostic.**
4. **`FSceneViewFamily::TemporalUpscalerInterface`'s offset**, if we bypass (3) by writing the field
   ourselves. This one *is* tractable: it is the **second of exactly four trailing pointers**
   (`SceneView.h:1843-1849` — nothing is declared after them but the private copy ctor and the
   `FSceneRenderer` friend, read to `:1855`), and a scan validator exists (the other three: one
   non-null `ScreenPercentageInterface`, two null spatial upscalers). We would reach the family from
   the `const FViewInfo&` our thunk already receives, and the write would take effect **next** frame,
   since `:538` reads the field 21 lines before `:559` calls us.

**The `checkf` is a non-issue.** `SetTemporalUpscalerInterface` is `FORCEINLINE` with
`checkf(TemporalUpscalerInterface == nullptr, ...)` (`SceneView.h:1807-1812`), and `DO_CHECK` is 0 in
Shipping, so it compiles to a plain store — and we would be storing the field directly in any case,
since there is no out-of-line setter to call. Worth recording only so nobody plans around it.

**One genuine property registration has that the patch does not**, and it is worth naming because it
is the only one: `FSceneView`'s constructor forces `PrimaryScreenPercentageMethod = TemporalUpscale`
(`SceneView.cpp:837-841`) and `AntiAliasingMethod = AAM_TemporalAA` (`:937-941`) when the family
carries an upscaler. **Stray needs neither** — it ships `r.TemporalAA.Upsampling=True` and
`r.DefaultFeature.AntiAliasing=2` (CLAUDE.md §2.3.1).

**And one plausible future want, dismissed with its cheaper route.** Registration is how NVIDIA
drives the *render fraction* per DLSS quality mode
(`ViewFamily.SetScreenPercentageInterface(new FLegacyScreenPercentageDriver(ViewFamily,
GetOptimalResolutionFractionForQuality(DLSSQuality), false))`, `DLSSUpscaler.cpp:712-717`). We take
the screen percentage from the user's `GameUserSettings.ini` today. If we ever wanted DLSS to choose
it, `r.ScreenPercentage` is writable from a mod through `UObject::ProcessConsoleExec`
(**HARD**, `docs/RESEARCH-UE4SS-MIGRATION.md:587`) — one console command against an entire
registration surface.

### 6.3 The ladder the brief asks about IS expressible — and it is what we ship

*"Register but delegate to the default upscaler"* is constructible: `GetDefaultTemporalUpscaler()`
returns `&` a **function-local static** (`TemporalAA.cpp:1577-1581`), so the instance is a single
object in `.data`/`.bss` whose first qword is the `FDefaultTemporalUpscaler` vtable — **and we
already know that vtable's address**, because the seam scan finds it and validates it against three
constants. A qword scan of the writable data sections for that value locates the instance; slot 2 is
its `AddPasses`. So delegation is reachable with a validator, and the ladder
(0 off / 1 discover / 2 register-and-delegate / 3 own it) is expressible up to rung 2.

**Rung 2 is behaviourally identical to what ships**, reached by a larger mechanism with one extra
unvalidatable write (§6.2 item 3) or one extra derived offset (item 4). **Rung 3 is §4.** There is no
rung between them: a partial implementation that produces the output on some frames and delegates on
others is fine in principle — but the delegating frames run the engine's TAA, which is exactly
today's fallback, so the ladder's top rung buys the *removal* of the thing that makes the lower rungs
safe.

---

## 7. What it would delete, quantified

### 7.1 The honest accounting

Line counts are `wc -l` on this tree at `1aa3986`, whole files including their tests; per-frame costs
are the live `[perf]` line quoted in `docs/RESEARCH-RESHADE-SHAPE-SWEEP.md` §1.2
(`native hooks/frame … total 2.913ms (14% of 20.2ms)`).

| Machinery | Lines | Cost | Deleted by owning the pass? | Deleted by routes we ALREADY HOLD? |
|---|---|---|---|---|
| Structural matcher + hash table + pin + round-trip + shape gates (`taa_signature.*`, `taa_hashes.hpp`, its tests, the `taa_hook.cpp` regions) | 1 221 + ~550 | — | yes | **yes** — replan item 4, licensed by `unclaimed=0` |
| Descriptor shadow + `heap_math` + `va_map` + the table walk + 6 device hooks | 967 + ~485 | **1.694 ms** | yes | **yes** — replan item 1, the RHI bind stream |
| View-CB search | ~200 | ~0.2 ms | yes | **yes — already done** (`view_cached`, level 2 default) |
| `restore_game_compute_state` + root shadow | ~600 | **0.681 ms** | **no** — we would still clobber the game's list | no |
| `u0` RHI hook (`u0_rhi_hook.*`, `core/u0_rhi_uav.*`, tests) | **2 851** | — | **yes, uniquely** | no — this is its purpose |
| Engine seam (`engine_seam_hook.*`, `core/engine_seam.*`, tests) | 3 771 | — | **no** — we would still need the vtable, the discovery and the ledger | no |
| L1 input resolution | (in the seam) | — | **no** — `FPassInputs` is still where colour/depth/velocity come from | no |
| The suppression path (`suppress_engine_dispatch`, `taa_hook.cpp:1195/2285`) | ~10 | — | yes — there is no dispatch to suppress | no |

**Net unique deletion: the `u0` hook, ~2 851 lines, and ten lines of suppression.** Everything else
on that list is either already scheduled for deletion by mechanisms we hold, or survives.

**Net unique addition:** §4's six unvalidated derivations, §4.3's write into `FRDGBuilder`'s private
`TMap`, §5.2's pool call and state-restore constant, plus the half-res downsample pass §3.2 measures.

### 7.2 And two of the doors would be destroyed, not exploited

This is the part the question's premise gets backwards.

* **The RHI bind stream** (`docs/RESEARCH-ENGINE-AWARE-REPLAN.md` §1, `RESEARCH-U0-IDENTITY.md` §10)
  works because `FTAAStandaloneCS` is `SHADER_USE_PARAMETER_STRUCT` and its pass lambda binds every
  register through `IRHIComputeContext`, bracketed by `RHISetComputeShader` and
  `RHIDispatchComputeShader`. **That bracket is the engine's TAA pass.** Own the pass and there is no
  bracket — and our own work, recorded natively onto the D3D12 list, produces no `IRHIComputeContext`
  calls at all.
* **The pool-name route** (`RESEARCH-U0-EXTERNAL-PRIOR-ART.md` §2) names `u0` because the engine
  allocates it through `FindFreeElementForRDG` with `InDebugName == L"TemporalAA"`
  (`TemporalAA.cpp:554-562`, `:686`). Own the pass and no `L"TemporalAA"` element is ever requested.

Both are replaced by "we allocated it, so we know it" — which is a real simplification, and is the
whole of §7.1's unique deletion. But it is bought by writing the pass, and the pass is what cannot be
written. **The doors do not remove the need; the need removes the doors.**

---

## 8. What it would risk

### 8.1 A registered upscaler that fails does not fall back — it corrupts the frame

`SecondaryViewRect` is initialised to `PrimaryViewRect` (`PostProcessing.cpp:520`) and
`SceneColor.Texture` to the render-res input. So an `AddPasses` that writes nothing leaves the post
chain running at **render** resolution while the view family expects the **secondary** rect. The
engine documents the contract it breaks two hundred lines later:

```cpp
// PostProcessing.cpp:1058
check(SceneColor.ViewRect.Size() == View.GetSecondaryViewRectSize());
```

`DO_CHECK` is 0 in Shipping, so instead of an assert the frame renders — wrongly, and in the
"magnified / mis-placed rect" family this project already knows by sight
(`docs/RESEARCH-ENGINE-TAA-HOOK.md` §16.3). **There is no configuration in which declining produces
the engine's own TAA**, because the engine's own TAA is the thing we replaced.

That matters because **declining is how this project stays safe today.** Every one of these currently
costs exactly one frame of engine TAA, by construction — `suppress_engine_dispatch` is set only after
a successful evaluate:

`not-announced` · `viewUnreadable` · `deadInputs` · `roleUnresolved` · `badRenderRect` ·
`mvFailed` · `createFailed` · `evalFailed` · `RecreateAction::wait` (the 8-frame debounce,
`core/feature_recreate.hpp`) · every `plan_letterbox_hold` refusal (`originMoved`, `outputTooSmall`,
`ratioMoved`, `largerThanFeature`, `tooSmall`) · `EngineSeamFallback` · **and NGX's own lazy
initialisation, measured at ~900 frames of a session** (CLAUDE.md §5, FG section).

**Nine gates, five hold clauses and a fifteen-second startup window would all have to be re-answered
as "produce something".** The only expressible "something" is delegation (§6.3), which needs the
engine's `AddPasses` — so the design converges on *keep the engine's pass and displace it*, i.e. what
ships.

### 8.2 What a wrong answer looks like, ranked by how quietly it fails

| Failure | Symptom |
|---|---|
| `FRDGPass` layout wrong (§4.2 #1) | RDG walks garbage producers/state — a wrong barrier, a recycled pool element, or a crash inside `Execute()`. **No debug layer on this stack** |
| `Passes` registry write wrong (#5) | `Pass->Handle` unset or duplicated; RDG's dependency sort is wrong for the whole frame |
| `FRDGParameterStruct::Layout` wrong (#3) | RDG walks a bogus uniform-buffer layout to compute transitions — arbitrary reads |
| `QueueTextureExtraction` reimplementation wrong (§4.3) | **Silent.** `RT[0]` stays null → permanent camera cut + SSR reads nothing. Looks like "DLSS is unstable" |
| Registered-texture state constant wrong (§5.2) | Wrong `StateBefore` on the engine's own resource — vkd3d-proton softens it, real D3D12 does not. **The `NgxNRRestoreState` hazard, on a resource the engine owns** |
| `GCustomStaticScreenPercentage` address wrong (§6.2 #3) | An arbitrary 8-byte store into engine globals |

Five of six are silent or delayed. `docs/RESEARCH-ENGINE-TAA-HOOK.md` §9's rules and prime directive
2 both point the same way.

---

## 9. What would change this verdict, and the cheapest experiment

**The verdict rests on one falsifiable claim:** *there is no way to get our compute work executed
between the engine's temporal-upscale slot and the first downstream reader of scene colour, other
than displacing a dispatch the engine itself scheduled there.* §5.3 enumerates the candidates.

The two things that would falsify it, in order of how much they would be worth:

1. **A callable, locatable, validatable way into RDG's pass list.** The specific finding that would
   do it is an out-of-line `FRDGBuilder` member that takes an already-constructed `FRDGPass*` **and**
   a self-validating anchor for it. `SetupPass` is the former and has none of the latter; §4.2 item 5
   is the reason it is not enough on its own. Anyone reopening this should start by testing whether
   `Stray-Win64-Shipping.exe` ships with `USE_LOGGING_IN_SHIPPING=1` — a literal search for
   `%d MB, NewRT %s %s` (`RenderTargetPool.cpp:403`), already flagged in
   `RESEARCH-U0-EXTERNAL-PRIOR-ART.md` §2.5 — because a title that keeps its logging keeps
   in-function anchors in far more places than this one.
2. **A second engine callback inside `Execute()` that runs at the upscale's position.** §5.3 checks
   the two `ISceneViewExtension` families and both are wrong-place; nothing else was found.

**The smallest first step that would prove or disprove the whole thing in one launch — and it costs
no risk.** Do **not** start with registration or with a pass. Start with the cheapest question whose
answer is load-bearing for both:

> **Make the seam thunk stop forwarding for exactly N frames, behind a key, writing only
> `*OutSceneColorViewRect` and leaving `*OutSceneColorTexture` untouched, and screenshot.**

This is ~15 lines in `add_passes_thunk`, changes nothing when the key is 0, and answers the one
question §8.1 argues from source rather than from measurement: **what does the frame actually look
like when the registered upscaler declines?** If the frame is merely un-upscaled and coherent, §8.1's
"no safe null behaviour" weakens and a partial-ownership design becomes discussable. If it is the
mis-placed-rect artefact the source predicts, §8.1 is confirmed on the box and this question is
closed for good, at the cost of one launch and a few visibly wrong frames.

It also doubles as the honest disproof of the reframing in §2: if suppressing the forward produces a
recognisable "no TAA" frame, then the seat really is ours to use, and the only thing left in the way
is §4 — which is a source-level fact and needs no box at all.

---

## 10. Provenance ledger

| Claim | Status |
|---|---|
| `ITemporalUpscaler` has five vtable entries in declaration order; `FPassInputs` is `{bool, EPixelFormat, 3× FRDGTextureRef}` | **HARD**, `TemporalAA.h:146-186` |
| `FDefaultTemporalUpscaler` is the only implementer in the engine; `SetTemporalUpscalerInterface` and `GCustomStaticScreenPercentage` have no engine caller/assigner | **HARD** for each positive hit read at its line; **SOFT** for the absences (GitHub code search over the mirror, four queries) |
| `GTemporalUpscaler` is written at `Renderer.cpp:56` and never read | **HARD** |
| Out-params 1 and 2 are mandatory; 3 and 4 are optional and the engine builds the half-res itself when 3 is null | **HARD**, `PostProcessing.cpp:520`, `:587-588`, `:626-636`, plus `TemporalAA.cpp:1543` and NVIDIA `DLSSUpscaler.cpp:409-410` |
| On Stray the default upscaler *does* fold the half-res downsample in, because `r.PostProcessAAQuality == 3` | **HARD**, `TemporalAA.cpp:1479` + `WindowsEngine.ini:18` |
| NVIDIA's `AddPasses` creates the output with `CreateTexture`, authors one `AddPass` over a `BEGIN_SHADER_PARAMETER_STRUCT` with `RDG_TEXTURE_ACCESS(..., UAVCompute)`, and extracts into `PrevFrameViewInfo.TemporalAAHistory.RT[0]` | **HARD** that the code reads so (`DLSSUpscaler.cpp:357-412`, `:466-479`, `:515-572`, `:574-586`); **SOFT** that this exact file is the 4.27 release rather than 4.26 |
| NVIDIA's `AddPasses` never calls `RegisterExternalTexture` and never delegates to the default upscaler | **HARD** (an absence, searched in both plugin versions) |
| `CreateTexture`, `RegisterExternalTexture`, `SetupPass`, `SetupEmptyPass`, `SetTextureAccessFinal` are non-template out-of-line members | **HARD**, `RenderGraphBuilder.h` + `.cpp` at the cited lines |
| **`QueueTextureExtraction`'s three overloads are `inline` in `RenderGraphBuilder.inl:253-282` with no out-of-line definition** | **HARD**, and it is this document's decisive new fact |
| `AddPass`'s two overloads are templates bottoming out in `Passes.Insert` + `SetupPass` / `Passes.Allocate` + `SetupEmptyPass` | **HARD**, `RenderGraphBuilder.inl:189-251` |
| `FRDGPass`'s single pure virtual is `ExecuteImpl`; `Execute` is non-virtual; `FRDGParameterStruct` is `{const uint8*, const FRHIUniformBufferLayout*}` from macro-generated static metadata | **HARD**, `RenderGraphPass.h:110-340`, `:380-428`; `RenderGraphParameter.h:186-205` |
| An externally registered texture gets `SetRHI` immediately and must be a real `FPooledRenderTarget` | **HARD**, `RenderGraphBuilder.cpp:502-532` (the `static_cast` at `:516`) |
| A `CreateTexture` handle acquires an RHI resource only when a pass references it | **HARD**, `RenderGraphBuilder.cpp:1220-1233`, `:2053-2084` |
| `ISceneViewExtension`'s in-`Execute()` callbacks are `PostRenderViewFamily_RenderThread` / `PostRenderView_RenderThread`, and they are the LAST passes of the scene renderer | **HARD**, `SceneRendering.cpp:3181-3200`; the "too late for an upscale" reading is **[derived]** from the pass order in `PostProcessing.cpp` |
| `EPostProcessingPass` offers no location at or near the temporal upscale | **HARD**, `SceneViewExtension.h:101-108`; `SubscribeToPostProcessingPass` is called at `PostProcessing.cpp:465`, before the upscale |
| Present and mid-frame `ExecuteCommandLists` are both refused for SR | **HARD** / **[derived]**, inherited from `docs/RESEARCH-RESHADE-SHAPE-SWEEP.md` §13.4 |
| `SetTemporalUpscalerInterface` is FORCEINLINE with a `checkf` compiled out in Shipping; the four interface pointers are the last members of `FSceneViewFamily` | **HARD**, `SceneView.h:1807-1855` |
| The view family destructor deletes `ScreenPercentageInterface` and the two spatial upscalers but **not** the temporal one | **HARD**, `SceneView.cpp:2666-2685` — so a persistent object is the correct shape, as NVIDIA's is |
| `GameViewportClient.cpp:1400` gates on `VMI_Lit` and runs before the legacy screen-percentage driver is installed at `:1420` | **HARD** |
| `GetDefaultTemporalUpscaler` returns a function-local static, so delegation is reachable by scanning for its (already-known) vtable | **HARD** that it is a local static (`TemporalAA.cpp:1577-1581`); **[derived]** that the scan finds it, and unbuilt |
| A declining registered upscaler leaves the post chain at the render rect, breaking the contract `PostProcessing.cpp:1058` states | **HARD** for the initialisation and the `check`; **[derived]** for what the resulting frame looks like — §9's experiment is what would settle it |
| Line counts and the 2.913 ms / 1.694 ms / 0.681 ms split | **HARD** — `wc -l` at `1aa3986`; costs from the live `[perf]` line quoted in the sweep §1.2 |
| **Correction to `docs/RESEARCH-OFFICIAL-DLSS-UE-PLUGIN.md` §A.4:** the **4.2x** plugin defaults `r.NGX.DLSS.AutoExposure` to **0** ("Use the engine-computed exposure value"), not 1. The `1` default is the UE5-era plugin | **HARD** on both files; flagged here rather than edited, since that document is owned elsewhere |

---

## 11. Cross-references

* **`docs/RESEARCH-ENGINE-TAA-HOOK.md`** — §4.3 declined this rung on two grounds. **The `FViewInfo`
  ground is retired** (§19 of that document reads `FViewInfo+5768` every frame, HARD). **The
  RDG-authoring ground stands but was under-stated**: §4 here shows the template is not the binding
  constraint, and §4.3 adds a blocker that document does not have. Its §9 rules, §14.2's window
  enumeration and §16.2's "check which quantity the authority covers" are what this assessment
  applies. **Not edited — owned elsewhere.**
* **`docs/RESEARCH-U0-IDENTITY.md`** — §6's reading of NVIDIA's plugin (*"their solution is precisely
  the one thing unavailable to us"*) is confirmed here from the plugin's own source at a second
  mirror, and §10's RHI route is what §7.2 shows owning the pass would destroy. **Not edited.**
* **`docs/RESEARCH-U0-EXTERNAL-PRIOR-ART.md`** — §6 already states *"authoring an RDG pass is still
  impossible from an injected DLL … UEVR does not do it"*. §4 here is the detailed version, and §5.2
  reuses its pool-hook technique for the one place it would genuinely help.
* **`docs/RESEARCH-ENGINE-AWARE-REPLAN.md`** — its items 1, 2, 4 and 5 are the routes that already
  deliver §7.1's deletions without any of this. **Not edited — owned elsewhere.**
* **`docs/RESEARCH-RESHADE-SHAPE-SWEEP.md`** §13.3-§13.4 — the "no SR at Present, no mid-frame
  submit" ordering argument this document leans on in §5.3.
* **CLAUDE.md** §2.3 (the seam and `EngineSeam=3`), §2.9 (`u0` is both scene colour and next-frame
  history), §5 (compounding motion errors; the descriptor hazards). **Deliberately not edited:** this
  document records no measurement, and §2.3's block is being written by other work in flight.
