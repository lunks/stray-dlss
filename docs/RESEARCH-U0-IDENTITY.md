# Is there an engine route to the TAA output UAV (`u0`)? Verified against Epic's 4.27 — and no

**Scope.** Two questions, both narrow, both left open by
`docs/RESEARCH-RESHADE-SHAPE-SWEEP.md` §13 (commit `7fad976`):

1. §13.3 transfers `docs/RESEARCH-ENGINE-TAA-HOOK.md` §14.2's "no hookable point in the
   `Execute()` window" from the colour **input** to the **output**, on the grounds that §14.2
   enumerates hook points rather than textures. That is an inference. **Verify it against the
   engine.**
2. Nobody has examined the **history extraction**. `TemporalAA.cpp` queues the same texture into
   `PrevFrameViewInfo` / `FTemporalAAHistory`, and an extracted texture becomes an
   `IPooledRenderTarget` holding a real RHI texture. **Is that the same allocation as `u0`, and is
   it reachable, on the render thread, at a point we can hook, with an RHI resource assigned?**

**Sources.** Epic's official **`EpicGames/UnrealEngine`, branch `4.27`, commit
`3abfe77d0b24a6d8bacebd27766912e5a5fa6f02`** — every engine citation below is
`path:line @ 4.27/3abfe77`, read from that tree, not from a mirror. Plus
`NvRTX/UnrealEngine` @ `dlss3/sl2-4.27-dlss-plugin`, commit
`32c3e4d5e0ee9ea7b792d1d09bcc2549917ad252`. Nothing here was run; there was no box.

---

## 0. Verdict, up front

**There is no engine route to `u0`'s identity for the frame in which we need it. The descriptor
shadow stays.** §13.3's transfer holds — but not for the reason it gives, and the correct reason
is stronger.

Three findings, in order of how much they change the picture:

1. **"Graph-allocated transient" is the wrong description of `u0` in UE 4.27, and it was doing
   load-bearing work in the argument.** 4.27's RDG has **no transient allocator**: every
   `FRDGTexture` is backed by an `IPooledRenderTarget` taken from `GRenderTargetPool`
   (`RenderGraphBuilder.cpp:2080 @ 4.27/3abfe77`). `u0`'s `ID3D12Resource` is an ordinary,
   long-lived, refcounted pool element that exists before the frame starts and survives it. What
   dies with the graph is the `FRDGTexture` *wrapper*, not the resource. So the "no" cannot rest
   on the resource being ephemeral — it does not exist yet only in the sense that RDG has not yet
   *chosen* which pool element to use.
2. **The extraction is real, it is the same allocation, and it lands in a persistent slot — but
   the slot is empty at every render-thread point we can hook.** `ViewState->PrevFrameViewInfo` is
   **cleared at `InitViews`** (`SceneVisibility.cpp:3412-3416`), which runs *before*
   `AddPasses`, and is written only at the **tail of `FRDGBuilder::Execute()`**
   (`RenderGraphBuilder.cpp:1336-1339`), after which `Clear()` runs and the frame is over. The
   window in which that slot names this frame's `u0` opens after our last render-thread hook and
   closes at the next frame's `InitViews`. It does not overlap the moment we need it.
3. **Something genuinely new and free is reachable at the seam: the PREVIOUS frame's `u0`.**
   `View.PrevViewInfo.TemporalAAHistory.RT[0]` (`SceneRendering.h:1275`, `:929`, `:814`) is, at
   `AddPasses` on frame N, the pooled render target that was `u0` on frame N−1 — with a live
   `FRHITexture` and therefore a live `ID3D12Resource*`. That is **not** an identity for frame N
   (§5.1 proves frame N's `u0` is provably a *different* allocation), but it is a **free, exact,
   one-frame-late oracle for the descriptor walk**, in the same idiom as L1's velocity assertion
   that caught a real defect on its first frame. **That is the deliverable here**, and it deletes
   nothing.

**What this closes.** The `u0` question. Not "not yet" — the enumeration below is of every
mechanism, and each fails for its own reason.

---

## 1. What `u0` is, established from Epic's source

### 1.1 `*OutSceneColorTexture` IS `u0` — exactly, not "the post-chain colour downstream of it"

Sweep §13.3 calls `*OutSceneColorTexture` "the graph-allocated post-chain scene colour". It is
more precise than that, and the precision matters because it removes a possible escape:

```cpp
// TemporalAA.cpp:688-696 @ 4.27/3abfe77
for (int32 i = 0; i < FTemporalAAHistory::kRenderTargetCount; i++)
{
    NewHistoryTexture[i] = GraphBuilder.CreateTexture(
        SceneColorDesc, OutputName, ERDGTextureFlags::MultiFrame);
}
NewHistoryTexture[0] = Outputs.SceneColor = NewHistoryTexture[0];
```

```cpp
// TemporalAA.cpp:919-922 @ 4.27/3abfe77
for (int32 i = 0; i < FTemporalAAHistory::kRenderTargetCount; i++)
{
    PassParameters->OutComputeTex[i] = GraphBuilder.CreateUAV(NewHistoryTexture[i]);
}
```

`OutComputeTex[0]` is the shader's `u0` (§2.3's `OutComputeTex_0`). And it comes straight back
out:

```cpp
// TemporalAA.cpp:1497, 1515 @ 4.27/3abfe77
FRDGTextureRef SceneColorTexture = TAAOutputs.SceneColor;
...
*OutSceneColorTexture = SceneColorTexture;
```

The only thing between them is the `HistoryUpscaleFactor > 1.0f` branch
(`TemporalAA.cpp:1499-1512`), which inserts a Mitchell-Netravali downsample and is reached only
when `r.TemporalAA.HistoryScreenPercentage > 100` — which CLAUDE.md §4 already forbids and Stray
does not set. **So on this title `*OutSceneColorTexture == u0`, identically.** **HARD.**

`PostProcessing.cpp:559-566` passes `&SceneColor.Texture` as that out-parameter, so it is *also*
the post chain's colour from then on — both readings are true, and it is the same object.

### 1.2 Every RDG texture in 4.27 is a pooled render target

```cpp
// RenderGraphBuilder.cpp:2053-2084 @ 4.27/3abfe77
void FRDGBuilder::BeginResourceRHI(FRDGPassHandle PassHandle, FRDGTextureRef Texture)
{
    ...
    TRefCountPtr<FPooledRenderTarget> PooledRenderTarget =
        GRenderTargetPool.FindFreeElementForRDG(RHICmdList, Texture->Desc, Texture->Name);
    const bool bTransient = PooledRenderTarget->IsTransient();
    FRDGTextureRef PreviousOwner = nullptr;
    Texture->SetRHI(PooledRenderTarget, PreviousOwner);
```

and

```cpp
// RenderGraphResources.cpp:182-209 @ 4.27/3abfe77
void FRDGTexture::SetRHI(FPooledRenderTarget* InPooledRenderTarget, FRDGTextureRef& OutPreviousOwner)
{
    ...
    Allocation = InPooledRenderTarget;
    PooledRenderTarget = InPooledRenderTarget;
    ResourceRHI = PooledTexture->GetRHI();
```

**There is no transient allocator in 4.27** — `IsTransient()` is
`!!(Desc.Flags & TexCreate_Transient)` (`RenderTargetPool.h:87-90`), a platform aliasing hint on
an already-allocated pool element, not a separate arena. The `ResourceRHI` we read at
`kRdgResourceRhiOffset` is a pointer *into* a pool element that predates the frame.

**Consequence, and it is the one that reframes the whole question.** The reason we cannot get
`u0` from the engine is **not** that the resource is ephemeral. It is that the *binding* —
"which pool element did RDG pick for this `FRDGTexture` this frame" — is made inside
`FRDGBuilder::Execute()` and is published nowhere we can read in time. The problem is a
**schedule**, not a lifetime.

### 1.3 The one-frame timeline, from the source

Everything below is on the **render thread**, sequentially, in one frame:

| # | Site | What happens to the `u0` identity |
|---|---|---|
| 1 | `SceneVisibility.cpp:3408` | `View.PrevViewInfo = ViewState->PrevFrameViewInfo;` — frame N−1's histories are handed to the view |
| 2 | `SceneVisibility.cpp:3412-3416` | `ViewState->PrevFrameViewInfo = NewPrevViewInfo;` — **the extraction slot is CLEARED** |
| 3 | `PostProcessing.cpp:559` | **our seam.** `AddPasses` — `*OutSceneColorTexture` is an `FRDGTexture*` with `ResourceRHI == nullptr`; `ViewState->PrevFrameViewInfo.TemporalAAHistory.RT[0] == nullptr` |
| 4 | `TemporalAA.cpp:961-972` | `OutputHistory->SafeRelease()` (already null), then `QueueTextureExtraction(NewHistoryTexture[0], &OutputHistory->RT[0])` — the *promise* is recorded, nothing is resolved |
| 5 | `RenderGraphBuilder.cpp:1220-1232` | `Execute()` → `CollectPassResources` → `BeginResourceRHI` → `FindFreeElementForRDG` → `SetRHI`. **`u0`'s identity now exists**, for the first time |
| 6 | `RenderGraphBuilder.cpp:1300-1311` | the pass loop; `FComputeShaderUtils::AddPass`'s lambda `ALLOC_COMMAND`s the dispatch onto the RHI command list |
| 7 | `RenderGraphBuilder.cpp:1336-1339` | `for (const auto& Query : ExtractedTextures) *Query.Value = Query.Key->PooledRenderTarget;` — **`ViewState->PrevFrameViewInfo.TemporalAAHistory.RT[0]` now holds this frame's `u0`** |
| 8 | `RenderGraphBuilder.cpp:1360`, `:1362-1377` | `Clear()` → `Allocator.ReleaseAll()`; every `FRDGTexture` is freed |
| 9 | *later, RHI thread* | the D3D12 `Dispatch` our hook intercepts |
| 10 | frame N+1, step 2 | the slot is cleared again |

`ExtractedTextures` is keyed by the `FRDGTexture*` and stores `Query.Key->PooledRenderTarget` —
the very field `SetRHI` assigned at step 5. **So the extracted `IPooledRenderTarget` IS the same
allocation as `u0`. HARD.** That half of the coordinator's question is a clean yes.

---

## 2. Question 1 — does the OUTPUT differ from the INPUT in a way that opens a window?

The transfer in sweep §13.3 is **correct**, but the two textures are *not* identical in lifetime
and the difference is worth stating so nobody re-opens this on the grounds that it was glossed.

| | colour **input** (`PassInputs.SceneColorTexture`) | colour **output** (`*OutSceneColorTexture` = `u0`) |
|---|---|---|
| `ResourceRHI` at `AddPasses` | **null** — graph-allocated, `SetRHI` at step 5 (this is why L1 reports colour `rhi_null`, facts §36.13) | **null**, same reason, same step |
| RDG flags | none special | **`ERDGTextureFlags::MultiFrame`** (`TemporalAA.cpp:690-692`) |
| extracted? | no | **yes** (`TemporalAA.cpp:969`) |
| pool element's fate after the graph | returned to the free list | **retained** by a `TRefCountPtr` in the view state |
| `FRDGTexture*` wrapper's fate | freed by `Allocator.ReleaseAll()` | **identically freed** |

**The difference is real and it is entirely downstream of the moment we need the answer.** The
output's *pool element* outlives the graph; its *identity binding* is created at exactly the same
step 5 as the input's, and is published at step 7 — one step after the dispatch has already been
recorded, and on the wrong thread relative to where the dispatch is *executed*.

So §14.2's enumeration does transfer, and now for a checked reason rather than a structural
analogy: **the output shares the input's `SetRHI` moment.** Every hook point §14.2 refuses
(`TRDGLambdaPass<>::Execute` — a template instantiation per lambda; `FRDGResource::GetRHI` and
`MarkResourceAsUsed` — inline, no symbol; `FRDGBuilder::ExecutePass` — no self-validating
constant; `Execute()`'s entry and exit — outside the window at both ends) refuses it for the
output too, unchanged.

**One detail that does NOT open anything.** NVIDIA calls `MarkResourceAsUsed()` on the *output*
as well as the inputs (`DLSSUpscaler.cpp:730` @ NvRTX), so there is no special output accessor to
look for — and `MarkResourceAsUsed()` is `inline void MarkResourceAsUsed() {}` in Shipping
anyway (§14.1), so it is not a hookable point for either.

**Verdict on question 1: the transfer holds. HARD, verified against Epic's tree rather than
inferred.**

---

## 3. Question 2 — the history extraction, traced to the end

### 3.1 Yes, it is the same allocation, and yes, it has a real RHI resource

Established in §1.3 step 7, and the chain from there to a native pointer is unusually clean —
cleaner than any offset this project currently derives:

```cpp
// RendererInterface.h:477-524 @ 4.27/3abfe77
struct IPooledRenderTarget
{
    virtual ~IPooledRenderTarget() {}
    ...
    inline FSceneRenderTargetItem& GetRenderTargetItem() { return RenderTargetItem; }
    ...
protected:
    FSceneRenderTargetItem RenderTargetItem;   // :524
};

// RendererInterface.h:428-465
struct FSceneRenderTargetItem
{
    ...
    FTextureRHIRef TargetableTexture;      // :462  <- first data member
    FTextureRHIRef ShaderResourceTexture;  // :465
```

`IPooledRenderTarget` has a vptr, then `RenderTargetItem`, whose first member is
`TargetableTexture` (a `TRefCountPtr<FRHITexture>`, one pointer). So
`*(FRHITexture**)((char*)pooled + 8)` is the RHI texture — **an offset derivable from a public
header**, not a layout guess — and then `FRHITexture::GetNativeResource` at vtable slot 7, already
**HARD** on this exe (facts §36.13). This route's *arithmetic* is sound. It is the *schedule*
that kills it.

### 3.2 The slot is empty at every render-thread point we can hook

```cpp
// ScenePrivate.h:889-897 @ 4.27/3abfe77
/** Informations of to persist for the next frame's FViewInfo::PrevViewInfo.
 *
 * Under normal use case (temporal histories are not frozen), this gets cleared after setting
 * FViewInfo::PrevViewInfo after being copied to FViewInfo::PrevViewInfo. New temporal histories
 * get directly written to it.
 * ...
 */
FPreviousViewInfo PrevFrameViewInfo;
```

and the clear itself:

```cpp
// SceneVisibility.cpp:3407-3416 @ 4.27/3abfe77
else
{
    View.PrevViewInfo = ViewState->PrevFrameViewInfo;
}

// Replace previous view info of the view state with this frame, clearing out references over render target.
if (!View.bStatePrevViewInfoIsReadOnly)
{
    ViewState->PrevFrameViewInfo = NewPrevViewInfo;
}
```

`NewPrevViewInfo` is a fresh `FPreviousViewInfo`, so every `TRefCountPtr<IPooledRenderTarget>` in
it — `TemporalAAHistory.RT[0]` included — is null. **This runs in `InitViews`, i.e. before
`AddPasses`.** **HARD.**

**Therefore at our seam the extraction slot reads `nullptr`, every frame, by construction.** Not
"stale" — empty. There is nothing to read one frame late, either, because the write and the next
clear bracket a window that contains no hook of ours.

### 3.3 Reading it from the RHI thread at claim time — why this is the trap, not the fix

The slot lives in `FSceneViewState`, which is persistent (owned by the local player, not by any
allocator), so dereferencing it from the RHI thread is memory-safe — none of §12.3's or §12.9's
use-after-free applies. That makes it superficially attractive, and it is worth writing down why
it still fails, because the failure is silent.

At the instant the RHI thread executes frame N's TAA dispatch, the render thread is somewhere in
frame N+1. The slot then holds one of four things:

| render thread is at | slot holds | detectable? |
|---|---|---|
| still inside frame N's `Execute()`, before step 7 | **null** | yes |
| between frame N step 7 and frame N+1 `InitViews` | **frame N's `u0` — correct** | — |
| past frame N+1's `InitViews`, before its step 7 | **null** | yes |
| past frame N+1's step 7 | **frame N+1's `u0` — WRONG** | **no** |

The last row is the whole problem. It is not a rare edge: a render thread one frame ahead of the
RHI thread is the *normal* pipelined state, and it is the state facts §36.11 measured directly
(`stale` growing every frame, `seq 715 vs newest 716`). And nothing distinguishes the two values:
they are pool elements with **byte-identical descriptors** (same extent, same format, same
flags — that is why they are interchangeable in the pool at all), so neither the resource
registry's desc check nor `ViewportRect` / `ReferenceBufferSize` (both written at *setup* time, so
already frame N+1's, and equal anyway at a fixed resolution) can tell them apart.

Writing DLSS's upscale into the wrong one of the two history buffers is a silent wrong image fed
straight into the engine's temporal state (§2.9) — precisely the class prime directive 2 exists
to refuse. **And the project has already paid twice for exactly this shape of reasoning**:
§12.8's "thread identity is not lifetime" and §12.9's "no gate on the claim side could ever have
worked". A guard here would be the third.

**One idea a future reader will have, refused in advance.** `FTemporalAAHistory::RT` has four
slots (`SceneRendering.h:809-814`) and only `RT[0]` is ever used at this call site, so `RT[1]`
looks like a free place to stamp a frame tag from `AddPasses` and read back on the RHI thread.
It is not: `SafeRelease()` (`SceneRendering.h:823-829`) calls `Release()` on every slot, so
anything we put there must be a real refcounted `IPooledRenderTarget`, and we would be **writing
into the engine's render state** to buy a frame counter. This project has never written engine
memory; the failure mode is a crash inside the renderer, and there is no validator. Recorded so
it is not rediscovered as clever.

**Verdict on question 2: no. The extraction produces the right object, and produces it in a
window that contains none of our hooks and cannot be read safely from the one thread that is
awake when we need the answer.**

---

## 4. Two more mechanisms checked and closed while in the source

### 4.1 Naming the pool element — dead in Shipping, twice

`FindFreeElementForRDG` carries the engine's own debug name (`TEXT("TemporalAAHistory")`,
`kTAAOutputNames`) all the way down, so "let the resource registry recognise `u0` by the name the
engine gave it" is an obvious idea. Both places it could surface are compiled out:

```cpp
// RenderTargetPool.cpp:612-618 @ 4.27/3abfe77   (the REUSE path — the steady state)
#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
    if (Found->GetRenderTargetItem().TargetableTexture)
    {
        RHIBindDebugLabelName(Found->GetRenderTargetItem().TargetableTexture, InDebugName);
    }
#endif
```

```cpp
// D3D12RHIPrivate.h:94 @ 4.27/3abfe77
#define NAME_OBJECTS !(UE_BUILD_SHIPPING || UE_BUILD_TEST)  // Name objects in all builds except shipping
```

The game is `Stray-Win64-Shipping.exe` (§2.1), so **no D3D12 object carries an engine name**.
**HARD.** Even had it survived, it would have been a candidate *set*, not an identity: the pool
matches on `GetTypeHash(Desc)` and `IsFree()` only (`RenderTargetPool.cpp:360-395`), never on the
name, so an element first created for one texture is freely reused for another.

### 4.2 Hooking `FRenderTargetPool::FindFreeElementForRDG` — no self-validating constant

This is the *right* site in principle: it runs on the render thread inside `Execute()`, in the
exact window, and it returns the `FPooledRenderTarget*` with the name in hand. But it is a
non-virtual member with, in Shipping, no anchor: `FindFreeElementForRDG` is a three-line forward
to `FindFreeElementInternal` (`RenderTargetPool.cpp:320-328`), whose only string literals are a
`UE_LOG(..., Display, ...)` (`:403`) and two `checkf`s (`:370`, `:586`) — all removed by
`NO_LOGGING` / `DO_CHECK` in a Shipping build. That puts it in exactly the bucket §14.2 assigns
`FRDGBuilder::ExecutePass`: plausibly signature-scannable, **but a wrong answer could not be
refused**, and §9's rule forbids installing on a guess. The `ITemporalUpscaler` scan is safe
because a candidate must reproduce a name literal *and* `0.5` *and* `2.0`; nothing here offers an
equivalent.

**[derived]** — the shipping-build stripping of `UE_LOG` / `check` is standard UBT behaviour, not
something read out of Stray's binary. If someone wants to reopen this, the falsifiable step is a
literal search of `Stray-Win64-Shipping.exe` for `%d MB, NewRT %s %s`; its presence would make
this route worth a second look, and its absence closes it.

---

## 5. What the engine DOES guarantee, and the one thing worth building

### 5.1 Frame N's `u0` is provably a different allocation from frame N−1's

```cpp
// RenderTargetPool.cpp:1492-1499 @ 4.27/3abfe77
bool FPooledRenderTarget::IsFree() const
{
    uint32 RefCount = GetRefCount();
    check(RefCount >= 1);
    // If the only reference to the pooled render target is from the pool, then it's unused.
    return !bSnapshot && RefCount == 1;
}
```

and the pool skips anything not free (`RenderTargetPool.cpp:372-375`). At step 5 of frame N,
frame N−1's history is still referenced by `View.PrevViewInfo.TemporalAAHistory.RT[0]` (copied at
`SceneVisibility.cpp:3408` and held for the whole frame) *and* by the RDG external registration
that binds it as the `HistoryBuffer_0` SRV at `t5`. Its refcount is ≥ 2, so it is not free, so the
pool cannot hand it back. **Frame N's `u0` ≠ frame N−1's `u0`. HARD.**

This is a genuine constraint, and it is worth being clear about what it is not: it **excludes one
candidate**, it does not select one. Turning "not that one" into "this one" needs the pool to hold
exactly two elements of that descriptor — which is a property of the whole post chain's
allocation pattern, unmeasured, resolution-dependent, and with no runtime check. Relying on it
would be the same trade as §3.3.

### 5.2 The previous frame's `u0` IS at the seam, and it is a free oracle

`View.PrevViewInfo.TemporalAAHistory.RT[0]` — `FViewInfo::PrevViewInfo`
(`SceneRendering.h:1275`) → `FPreviousViewInfo::TemporalAAHistory` (`:929`) →
`FTemporalAAHistory::RT` (`:814`) — is, at `AddPasses` on frame N, the live pooled render target
that was `u0` on frame N−1. It has a real `FRHITexture`, hence a real `ID3D12Resource*` through
the §3.1 chain, and it is alive by construction (§5.1's refcount is what keeps it alive).

**This is what the seam already gives us that nobody has used** — the fourth avenue in the brief.
And it comes with a self-validating locator in the house idiom, which is why it can be built to
the standard the vtable scan set:

`FTemporalAAHistory` is 56 bytes — `{ 4 × TRefCountPtr, FIntPoint ReferenceBufferSize, FIntRect
ViewportRect }` (`SceneRendering.h:807-816`) — and at our seam it must satisfy **four independent
predictions we already know from the announcement**:

* `RT[0]` non-null **and present in our own resource registry** after the pooled → RHI → native
  walk (the same warrant L1 uses);
* `RT[1..3]` all null — only index 0 is ever extracted at this call site, because
  `bUseHistoryTexture[i]` is taken from `PassParameters->HistoryBuffer[i]` *after*
  `ClearUnusedGraphResources` (`TemporalAA.cpp:944-948`, `:961-972`) and the TAA shader declares
  one history slot (§2.3);
* `ReferenceBufferSize == OutputExtent * ResolutionDivisor` of the **previous** announcement
  (`TemporalAA.cpp:974`);
* `ViewportRect == DestRect` of the previous announcement (`TemporalAA.cpp:973`).

`FPreviousViewInfo` contains three other `FTemporalAAHistory` (`DOFSetupHistory`, `SSRHistory`,
`WaterSSRHistory`, `SceneRendering.h:938-942`), and they carry different rects, so the four
predictions discriminate. The offset can therefore be **found by a bounded scan and latched**,
then re-validated every frame — never assumed. **[derived]** that a scan is necessary at all:
`FViewInfo`'s layout is private and this project has no build of the engine to measure it
against.

**What it buys.** One `ENGINE SEAM` assertion: *the descriptor walk said `u0` was X on frame N;
the engine says the texture it extracted as frame N's history was Y*. A disagreement is a wrong
`u0`, which is DLSS writing the upscale into a texture the engine does not carry forward — a
silent wrong image, and exactly the class L1 caught on velocity the day it shipped
(§36.13.1: *"the engine's velocity is …5323DD00 and the heuristic's register walk says
…52FB62F0"*). It costs one pointer read plus one vtable call per frame and **deletes nothing**.

**What it does not buy.** Anything about the shadow. It is one frame late by construction (§1.3),
and §5.1 forbids using it as a prediction.

---

## 6. NVIDIA's plugin: what it implies, since it faces the identical problem

Read at `NvRTX/UnrealEngine` @ `dlss3/sl2-4.27-dlss-plugin`, `32c3e4d`. On UE 4.27 their
`DLSS_ENGINE_ADDPASSES_RETURN_THROUGH_PARAMS` is 1 (`DLSS.Build.cs:99-101`), so they implement
the same out-parameter signature we intercept. Their sequence, end to end:

```
render thread, AddPasses:
  CreateTexture("DLSSOutputSceneColor")                       DLSSUpscaler.cpp:590
      -> FRDGTexture* handle, NO RHI resource
  AddPass(..., RDG_TEXTURE_ACCESS(SceneColorOutput,
               ERHIAccess::UAVCompute), Compute|Raster|SkipRenderPass, lambda)   :156, :649-658
  QueueTextureExtraction(Outputs.SceneColor,
        &View.ViewState->PrevFrameViewInfo.TemporalAAHistory.RT[0])              :752
  *OutSceneColorTexture = that handle   <-- handed back still rhi_null           :527

RDG execution, inside their own pass lambda:
  PassParameters->SceneColorOutput->MarkResourceAsUsed();
  DLSSArguments.OutputColor = PassParameters->SceneColorOutput->GetRHI();        :730-731
  RHICmdList.TransitionResource(ERHIAccess::UAVMask, ...); EnqueueLambda(...)    :733-734

RHI thread, ExecuteDLSS:
  GetD3D12TextureFromRHITexture(...)->GetResource()->GetResource()   <- ID3D12Resource*
                                                          NGXD3D12RHI.cpp:328
```

Four things this settles, and none of them is a route:

1. **They hit the same wall and get past it only by owning a pass.** Their output is `rhi_null`
   at `AddPasses` too (`:527` hands back a bare `CreateTexture` handle). The `FRHITexture*` is
   obtained at `:731`, *inside a lambda they authored*, which is the window §14.2 enumerates and
   §4.3 establishes we cannot enter — `AddPass` is a template over `FShaderParametersMetadata`
   instantiated at engine compile time, with no ABI from an injected DLL. **Their solution is
   precisely the one thing unavailable to us.** That is the strongest available confirmation that
   there is no *other* way in: the vendor, with full source access, did not find one either.
2. **They carry a plain pointer across the thread boundary, exactly as our announcement now
   does.** `EnqueueLambda` (`:734`) ships a POD `FRHIDLSSArguments` of `FRHITexture*` to the RHI
   thread. That is the same shape as §12.9's fix — resolve where it is alive, hand across a plain
   pointer — and independent validation of the architecture we already ship for depth and
   velocity. The only difference is that they can resolve *four* inputs plus the output at that
   point and we can resolve two.
3. **They extract the output into `PrevFrameViewInfo.TemporalAAHistory.RT[0]` too** (`:752`,
   preceded by `SafeRelease()` at `:750`), with `ViewportRect` / `ReferenceBufferSize` set at
   `:754-755` — the same protocol as `TemporalAA.cpp:961-975`. So §5.2's oracle is *the engine's
   own contract for the primary upscaler*, not an artefact of the stock TAA path, and it would
   survive us ever becoming a real `ITemporalUpscaler`.
4. **They never set `ERDGTextureFlags::MultiFrame` and never touch `TexCreate_*` beyond
   `ShaderResource | UAV`** (`:582-592`). The extraction *is* their lifetime extension. Worth
   knowing: `MultiFrame` is not what keeps `u0` alive across the frame — the `TRefCountPtr` in
   the view state is.

One incidental fact, better filed with the NGX work than here: after the evaluate they do **not**
save and restore command-list state; they call `StateCache.ForceSetComputeRootSignature()` and
re-point the descriptor cache (`NGXD3D12RHI.cpp:369-370`). That is the vendor's own
acknowledgement that NGX clobbers root signature and heaps (§5's number-one corruption risk),
fixed from inside the engine in a way we cannot copy.

---

## 7. Consequence for the descriptor shadow

Sweep §13's ranking stands, and this document removes the last "unverified" from it:

* **The descriptor shadow proper (1.694 ms, 58%) has one shipping-path reader — the SRV/UAV table
  walk — and its irreducible job is naming `u0`.** There is no engine route to that identity.
  Confirmed against Epic's tree, not transferred by analogy.
* **The line of enquiry is closed, not paused.** The mechanisms are: read it at `AddPasses` (null
  by `SceneVisibility.cpp:3412`); read it from the RHI thread (silently wrong one frame in a
  pipelined steady state, no validator); hook inside `Execute()` (§14.2, extended here to the
  output); hook the pool (no self-validating constant in a Shipping build); name the resource (no
  names in a Shipping build); author a pass (no ABI); `r.RHICmdBypass=1` (kills the RHI thread
  process-wide). Seven, each failing for its own reason.
* **So the shadow's future is narrowing what it RECORDS, never who reads it.** Sweep §13.5 owns
  that question; nothing here changes its answer, and its read-bit instrument is still the right
  next move, because a filter has to be chosen at write time.
* **What this document adds is not a saving but an assertion** (§5.2), and it is worth building on
  the same grounds L1 was: the walk is currently unchecked against the engine, and the one time
  this project checked a heuristic against the engine it found a real defect on the first frame.

---

## 8. Provenance ledger

| Claim | Status |
|---|---|
| `*OutSceneColorTexture == u0` exactly, on this title | **HARD** — `TemporalAA.cpp:688-696`, `:919-922`, `:1497`, `:1515` @ 4.27/3abfe77; the only divergence is the `HistoryUpscaleFactor > 1` branch, which needs `r.TemporalAA.HistoryScreenPercentage > 100` |
| Every RDG texture in 4.27 is an `IPooledRenderTarget` from `GRenderTargetPool`; there is no transient allocator | **HARD** — `RenderGraphBuilder.cpp:2080-2084`, `RenderGraphResources.cpp:182-209`, `RenderTargetPool.h:87-90` |
| `u0`'s `ResourceRHI` is assigned in `BeginResourceRHI`, inside `Execute()`, at the same step as the colour input's | **HARD** — `RenderGraphBuilder.cpp:1220-1232`, `:2053-2084` |
| The extracted `IPooledRenderTarget` is the same allocation as `u0` | **HARD** — `RenderGraphBuilder.cpp:1336-1339` writes `Query.Key->PooledRenderTarget`, the field `SetRHI` assigned |
| `ViewState->PrevFrameViewInfo` is cleared at `InitViews`, before `AddPasses` | **HARD** — `SceneVisibility.cpp:3407-3416`, and the comment at `ScenePrivate.h:889-896` says so in words |
| The extraction slot therefore reads null at our seam, every frame | **HARD**, from the two above |
| The slot is written at the tail of `Execute()`, after the dispatch is recorded and before `Clear()` | **HARD** — `RenderGraphBuilder.cpp:1300-1311`, then `:1336-1339`, then `:1360` |
| §14.2's "no hookable point in the `Execute()` window" covers the output | **HARD now**, upgraded from sweep §13.3's **[derived]**: the output shares the input's `SetRHI` moment, so the same enumeration applies to the same window with no new object |
| Reading the slot from the RHI thread is memory-safe but silently wrong when the render thread is one frame ahead | **[derived]** — memory-safety is HARD (`FSceneViewState` is persistent); the wrongness follows from §1.3's ordering plus facts §36.11's measured pipelining. Not measured on the box |
| Frame N's `u0` ≠ frame N−1's `u0` | **HARD** — `RenderTargetPool.cpp:1492-1499` (`IsFree()` is refcount 1) + `:372-375` (unfree elements skipped) + `SceneVisibility.cpp:3408` (the view holds a reference all frame) |
| The pool holds exactly two elements of that descriptor, so "the other one" identifies frame N's `u0` | **UNCONFIRMED and refused** — a property of the whole post chain's allocation pattern, resolution-dependent, with no runtime check |
| `View.PrevViewInfo.TemporalAAHistory.RT[0]` is frame N−1's `u0`, live, with an RHI resource, at `AddPasses` | **HARD** — `SceneVisibility.cpp:3408`, `SceneRendering.h:1275`, `:929`, `:814` |
| `IPooledRenderTarget` → `FRHITexture*` is `+8` (vptr, then `RenderTargetItem.TargetableTexture`) | **[derived] from a PUBLIC header** — `RendererInterface.h:477-524`, `:428-465`. Stronger than a layout guess, weaker than a measurement; validate by the returned pointer being in our registry |
| The four predictions that locate `FViewInfo::PrevViewInfo` by scan | **[derived]** — `SceneRendering.h:807-816`, `:938-942`, `TemporalAA.cpp:944-948`, `:973-974`. Unbuilt and unmeasured |
| No D3D12 object carries an engine debug name in a Shipping build | **HARD** — `D3D12RHIPrivate.h:94`; and the pool's reuse-path rebind is `#if !(UE_BUILD_SHIPPING \|\| UE_BUILD_TEST)`, `RenderTargetPool.cpp:612-618` |
| `FindFreeElementForRDG` / `FindFreeElementInternal` have no shipping-surviving string anchor | **[derived]** — `RenderTargetPool.cpp:320-328`, `:370`, `:403`, `:586`; rests on standard `NO_LOGGING` / `DO_CHECK` stripping, not on a scan of Stray's exe. Falsifiable by searching the exe for `%d MB, NewRT %s %s` |
| RDG never dispatches the command list to the RHI thread inside `Execute()` | **[derived]** — no `ImmediateFlush` / `WaitForTasks` anywhere in `RenderGraphBuilder.{cpp,h}`; the sole submit is under `GRDGDebugFlushGPU`, default 0 (`RenderGraphPrivate.cpp:26`). It does not account for some other pass lambda in the chain flushing, so it is not HARD — and nothing in this document's verdict depends on it |
| NVIDIA obtains the output's `FRHITexture*` only inside their own pass lambda, and the `ID3D12Resource*` only on the RHI thread | **HARD** — `DLSSUpscaler.cpp:590`, `:730-731`, `NGXD3D12RHI.cpp:328` @ NvRTX `32c3e4d` |
| NVIDIA extracts their output into `PrevFrameViewInfo.TemporalAAHistory.RT[0]` on the same protocol | **HARD** — `DLSSUpscaler.cpp:748-756` @ NvRTX `32c3e4d` |
| UE5 differs | **SOFT, not investigated** — UE5 adds `FRDGTransientResourceAllocator` (so "pooled" is no longer universal) and UE5.1+ returns `ITemporalUpscaler::FOutputs` by value (`DLSS.Build.cs:99-101` @ NvRTX gates on exactly this). 4.27 decides for this project; do not carry §1.2 to a UE5 title |

---

## 9. Cross-references

* `docs/RESEARCH-RESHADE-SHAPE-SWEEP.md` §13 — the consumer census whose open half this answers.
  §13.3's `u0` verdict is confirmed; its "graph-allocated transient" wording is refined by §1.2
  here, and its **[derived]** transfer is upgraded to **HARD** by §2.
* `docs/RESEARCH-ENGINE-TAA-HOOK.md` §14.2 (the window enumeration), §14.1
  (`MarkResourceAsUsed` is inline in Shipping), §14.3 (`r.RHICmdBypass`), §4.3 (why we cannot
  author a pass), §12.9 (resolve where it is alive, hand across a plain pointer).
* `CLAUDE.md` §2.9 (`u0` is both scene colour and next-frame history), §2.3 (the seam, and
  `EngineSeam=3`).
