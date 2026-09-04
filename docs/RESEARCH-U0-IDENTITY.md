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

> **CORRECTED 2026-09-04 — see §10.** This verdict enumerated every way of reading RDG's own
> state and found none in time. It never examined the RHI: between RDG assigning the resource and
> the D3D12 dispatch we intercept, the pass lambda binds `u0` through
> `IRHIComputeContext::RHISetUAVParameter(shader, 0, uav)` on the RHI thread — a virtual with the
> register as an argument, immediately before the dispatch, on the same thread. The route exists,
> is built on branch `u0-rhi-uav`, and its measurements are in facts §37. Also corrected below:
> the pool name is `TEXT("TemporalAA")`, not `TEXT("TemporalAAHistory")` (§4.1), and RDG binds
> every resource before executing any pass (§1.3's step 5/6 are two phases, not interleaved).
> The text of §0-§9 is left as written, because the reasoning error — enumerating producers and
> forgetting the consumer — is the instructive part.
>
> **AND SO IS §4.1/§4.2's, corrected in place the same day.** Naming a pooled render target from
> outside the engine is not only possible, it is a shipping mechanism: the debug name is a live
> `const TCHAR*` ARGUMENT to `FRenderTargetPool::FindFreeElement`, used unconditionally in
> Shipping, and `praydog/UEVR` hooks it across hundreds of UE titles. §4.1 asked what is STORED
> when the question was what is PASSED; §4.2 looked for an anchor inside the function when the
> anchor is in its 25 callers. Built as `[STRAYDLSS] PoolNames`
> (`docs/RESEARCH-ENGINE-TAA-HOOK.md` §20), and what it buys is not `u0` — it is **Ray
> Reconstruction's entire G-buffer guide set**, `docs/RESEARCH-ENGINE-AWARE-REPLAN.md` §5.

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

> **CORRECTED 2026-09-04, AND THE ROUTE IS BUILT.** The heading and the two `#if` quotes below
> are right about *D3D12 objects* and wrong about the engine. **The name is a LIVE FUNCTION
> ARGUMENT.** `FRenderTargetPool::FindFreeElement` takes `const TCHAR* InDebugName`
> (`RenderTargetPool.h:268-274`) and `FindFreeElementInternal` uses it unconditionally —
> `CreateInfo.DebugName = InDebugName` at `RenderTargetPool.cpp:415`, outside any `#if`. A hook
> on that function is handed the name on every call, in a Shipping build, and
> **`praydog/UEVR` ships exactly this across hundreds of shipping UE titles**
> (`src/mods/vr/RenderTargetPoolHook.cpp`). The reasoning error is worth naming because it is
> the same one §10 records for the whole document: *"no D3D12 object carries a name"* was read
> as *"the engine cannot tell us which texture this is"*, and that does not follow — it asks
> what is stored, when the question is what is passed.
>
> The paragraph's second half — *"even had it survived, it would have been a candidate SET,
> not an identity: the pool matches on `GetTypeHash(Desc)` and `IsFree()` only, never on the
> name"* — is **true and irrelevant to the route**. We do not ask the pool to FIND an element by
> name; we watch the engine ASK for one by name and record what it got back. Reuse across names
> is invisible to that, because the name is the caller's word for what the element is being used
> as this frame.
>
> Built as `[STRAYDLSS] PoolNames` (`src/core/pool_locator.hpp`, `src/pool_name_hook.hpp`);
> the design and the ladder are `docs/RESEARCH-ENGINE-TAA-HOOK.md` §20, and the prize is
> `docs/RESEARCH-ENGINE-AWARE-REPLAN.md` §5 — **DLSS Ray Reconstruction's entire guide set**,
> without rebuilding the ~3 970-line heuristic G-buffer finder. Also corrected here by
> `docs/RESEARCH-U0-EXTERNAL-PRIOR-ART.md` §2.4: `u0`'s pool name is `TEXT("TemporalAA")`, not
> `TEXT("TemporalAAHistory")`.


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

> **CORRECTED 2026-09-04. BOTH HALVES ARE WRONG, and the second one is the transferable
> mistake.**
>
> **"No shipping-surviving string anchor"** — the section looks for a literal INSIDE the
> function. UEVR anchors on a **CALLER's** argument literal, resolves the `lea reg,[rip+d]` that
> loads it, and decodes forward to the CALL (`shared/sdk/FRenderTargetPool.cpp` @ `0587a46`).
> 4.27's `SceneRenderTargets.cpp` alone has **25 such call sites**, each with a distinct literal,
> spread over **six distinct enclosing functions** (`AllocGBufferTargets`,
> `AllocateCommonDepthTargets`, `AllocateDeferredShadingPathRenderTargets`,
> `AllocateReflectionTargets`, `AllocateDebugViewModeTargets`, `AllocateAnisotropyTarget`).
> **The anchor is not in the function, it is in its callers** — and a route refused for lacking
> something it never needed is the shape to watch for.
>
> **"No self-validating constant"** — the `InDebugName` argument IS one, and a *stronger* one
> than the `ITemporalUpscaler` scan's `0.5` / `2.0` pair, because that pair is checked once at
> install while this is checked on **every call**: a hook on the wrong function is handed a
> `const TCHAR*` that does not read as a wide string and never equals a name we know. The static
> side gets its own bar in the same spirit — agreement across **≥ 3 distinct enclosing
> functions** (derived from the exe's own `.pdata`, not from a source reading) and **≥ 4 distinct
> name literals** on one address that is a `RUNTIME_FUNCTION::BeginAddress`, with the runner-up
> beaten outright and a tie refused. `docs/RESEARCH-ENGINE-TAA-HOOK.md` §20.
>
> **What STANDS: `FindFreeElement` and `FindFreeElementForRDG` are different functions**
> (`RenderTargetPool.cpp:623` and `:320`), and `u0` goes through the latter. **That residual is
> not this route's problem** — every RR guide (`GBufferA-E`, `SceneDepthZ`, `GBufferVelocity`,
> `SceneColorDeferred`) is allocated through the OUTER `FindFreeElement`, which is what the scan
> finds. The falsifiable test this section named — is `%d MB, NewRT %s %s` still in the exe? — is
> now RUN AT STARTUP and reported either way, so the RDG half is settled by a log line rather
> than by a grep, and it costs nothing. The refusal to install on a guess (§9's rule) also
> stands, and is why level 1 installs nothing at all.


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

---

## 10. THE VERDICT IS CORRECTED: the window has a MIDDLE, and the engine names `u0` there (2026-09-04)

§0 concluded *"there is no engine route to `u0`'s identity for the frame in which we need it"*, and
§1.3 laid out the timeline it rests on. Both ENDS of that timeline were checked and both readings
stand: at `AddPasses` (step 3) `ResourceRHI` is null, and by the time our D3D12 `Dispatch` hook runs
(step 9) the `FRDGTexture` is freed. **What §1.3 never examined is what happens between steps 6 and
9** — how the pass lambda's recorded RHI commands turn into the D3D12 calls we intercept. That is
where the route is, and it is not subtle once looked at.

### 10.1 What lives in the middle, verified against 4.27.2

Read from `AlexMercer-MA/UnrealEngine-4.27` @ `306a7e9` (`Build.version` 4.27.2), the mirror
CLAUDE.md already cites; the line numbers below are that tree's.

1. **The TAA pass is an ordinary compute pass, and its shader takes the per-parameter path.**
   `FTAAStandaloneCS` is `SHADER_USE_PARAMETER_STRUCT` (`TemporalAA.cpp:149`) — not
   `SHADER_USE_ROOT_PARAMETER_STRUCT`, which would bypass everything below — and the pass is
   `FComputeShaderUtils::AddPass(GraphBuilder, ..., ComputeShader, PassParameters, GroupCount)`
   (`:950`). Its output is `SHADER_PARAMETER_RDG_TEXTURE_UAV_ARRAY(... OutComputeTex ...)` (`:217`),
   `OutComputeTex[0]` being `u0` (`:921`, CLAUDE.md §2.3).
2. **The pass lambda is `FComputeShaderUtils::Dispatch`** (`RenderGraphUtils.h:305-308`, the lambda
   `AddPass` records), which is four calls (`:247-254`): `RHICmdList.SetComputeShader(ShaderRHI)`,
   `SetShaderParameters(...)`, `RHICmdList.DispatchComputeShader(...)`, `UnsetShaderUAVs(...)`.
3. **`SetShaderParameters` binds every resource through a per-parameter RHI call with the register
   as an argument** (`ShaderParameterStruct.h:185-283`). For `UBMT_RDG_TEXTURE_UAV` it is
   `SetShaderUAV` → `RHICmdList.SetUAVParameter(ShadeRHI, ParameterBinding.BaseIndex,
   GraphUAV->GetRHI())` (`:145-158`, the call at `:157`). `BaseIndex` is the shader's register —
   0 for `OutComputeTex_0`. For the five `SHADER_PARAMETER_RDG_TEXTURE` inputs (`:188-203`) it is
   `RHICmdList.SetShaderTexture(ShadeRHI, BaseIndex, GraphTexture->GetRHI())` (`:239-245`); for the
   stencil `_SRV` (`:206`) `SetShaderResourceViewParameter` (`:258-266`, deferred after the UAVs);
   for the one `SHADER_PARAMETER_STRUCT_REF` (`ViewUniformBuffer`, `:207`)
   `SetShaderUniformBuffer(ShadeRHI, BufferIndex, ...)` (`:277-282`).
4. **`SetUAVParameter` is `FORCEINLINE` and queues a command or calls the context directly**
   (`RHICommandList.h:2390-2398`): `if (Bypass()) { GetComputeContext().RHISetUAVParameter(Shader,
   UAVIndex, UAV); return; } ALLOC_COMMAND(FRHICommandSetUAVParameter<FRHIComputeShader>)(...)`.
   The command's `Execute` is `CmdList.GetComputeContext().RHISetUAVParameter(Shader, UAVIndex,
   UAV)` (`RHICommandListCommandExecutes.inl:175-179`, `INTERNAL_DECORATOR_COMPUTE` at `:13`).
   **So `r.RHICmdBypass` moves the thread; it never skips the virtual.**
5. **`RHISetUAVParameter(FRHIComputeShader*, uint32, FRHIUnorderedAccessView*)` is a pure
   virtual of `IRHIComputeContext`** (`RHIContext.h:200`), overridden `final` by
   `FD3D12CommandContext` (`D3D12CommandContext.h:269`; body `D3D12Commands.cpp:680-695`:
   `RetrieveObject`, `ConditionalClearShaderResource`, `StateCache.SetUAVs<SF_Compute>`).
6. **On one GPU the context IS `FD3D12CommandContext`.** `FD3D12DynamicRHI::RHIGetDefaultContext()`
   (`D3D12RHI.cpp:282-298`) returns `&Device->GetDefaultCommandContext()` unless
   `GNumExplicitGPUsForRendering > 1`; the `FD3D12CommandContextRedirector` (whose FORCEINLINE
   `ContextRedirect` calls would devirtualise past our slot) is not in the path.
7. **The dispatch we intercept is the very next thing the same thread does.**
   `FD3D12CommandContext::RHIDispatchComputeShader` (`D3D12Commands.cpp:106-125`) runs
   `StateCache.ApplyState<D3D12PT_Compute>()` and then `CommandListHandle->Dispatch(...)` (`:120`)
   — the `ID3D12GraphicsCommandList::Dispatch` our native hook stands in for — followed by
   `ConditionalFlushCommandList()`, so it is never a tail call and our hook's return address lies
   inside that function.

So between §1.3's step 6 and step 9, on the RHI thread, **the engine calls a virtual we can stand in
for, with the register number and the live `FRHIUnorderedAccessView*` as arguments, immediately
before the dispatch we already intercept, on the same thread.** The `FRDGTexture` wrapper being
freed is irrelevant: nothing here touches it.

### 10.2 What §0's enumeration missed, and why

§7 lists seven mechanisms, "each failing for its own reason": read at `AddPasses`, read from the
RHI thread (the `PrevFrameViewInfo` slot), hook inside `Execute()`, hook the pool, name the
resource, author a pass, `r.RHICmdBypass`. Every one of them is a way of reading **RDG's** state —
the `FRDGTexture`, the pool element, the extraction slot. **None of them is the RHI's state.** The
enumeration was of places RDG publishes the binding, and RDG publishes it nowhere in time; but RDG
is not the last thing that knows. The pass lambda hands the binding to the RHI, and the RHI is a
virtual interface with the register as an argument. §14.2 of the TAA-hook report made the same
omission for the input textures, and `docs/RESEARCH-U0-EXTERNAL-PRIOR-ART.md` §3 found a third
route the enumeration missed (`ISceneViewExtension` callbacks inside `Execute()`).

The rule this earns: **when an enumeration of routes comes up empty, ask who the last consumer of
the value is, not only who the producer is.** The producer (RDG) had no hookable point; the
consumer (the RHI context) is an abstract interface.

### 10.3 Two factual errors in this document, corrected

* **§4.1's `TEXT("TemporalAAHistory")` is wrong.** The pool name is `kTAAOutputNames[Pass]`, and
  for every `Main*` config that is `TEXT("TemporalAA")` (`TemporalAA.cpp:554-562`, `:686`). It
  matters because the name is now READ at the seam (§10.5).
* **§1.3 presents `BeginResourceRHI` (step 5) and the pass loop (step 6) as interleaved.** They are
  two whole-graph phases: `CollectPassResources` runs for every pass before any pass executes
  (`RenderGraphBuilder.cpp:1228-1233`, then `:1304-1310`), and `EndResourceRHI` never clears
  `ResourceRHI`. Not load-bearing for this route, but it is why the prior-art document's
  `ISceneViewExtension` route works at all.

### 10.4 What is built (branch `u0-rhi-uav`), and how a wrong answer is refused

`src/core/u0_rhi_uav.hpp` (pure, CI-tested) and `src/u0_rhi_hook.cpp` (live), behind
`[STRAYDLSS] U0Hook`, the seam's ladder verbatim: 1 discovers and installs nothing; 2 installs
FORWARDING thunks and asserts; 3 is declared and not built.

**Discovery of the vtable has no name literal to anchor on, so the seed is a return address.** Our
`Dispatch` hook's `_ReturnAddress()` is inside `RHIDispatchComputeShader` (§10.1 item 7); the exe's
own exception directory (`.pdata`, `RUNTIME_FUNCTION` entries, which an x64 image cannot be
unwound without and a Shipping build cannot strip; chained entries followed) turns it into the
function's start; the read-only qword equal to that start is `IRHIComputeContext` slot 3
(destructor, `RHISetComputeShader`, `RHISetComputePipelineState`, then `RHIDispatchComputeShader`,
`RHIContext.h:127-145`). The candidate is then held to predictions of the same kind the seam's
`0.5` / `2.0` are: all 38 slots up to `RHIPostExternalCommandsReset` inside the module's code, and
the six virtuals whose only body on this class is empty — `RHISetAsyncComputeBudget` (the base's
`{}` and `FD3D12CommandContextBase`'s `{}`, `D3D12CommandContext.h:53`), the four `UAVOverlap`
(`RHIContext.h:177-181`, not overridden by D3D12), `RHIInvalidateCachedState` (`:235`) — each
beginning with `ret`. Five more are reported and never gated (`RHISetGPUMask`'s `ensure`,
`RHITransferTextures`, `RHIBuildAccelerationStructure(Geometry)`'s stripped `checkNoEntry`,
`RHIGetNativeCommandBuffer`'s `xor eax,eax; ret`, `RHIPostExternalCommandsReset`). The seed needs
eight consecutive agreeing dispatches; one survivor installs, two refuse.

**The slot is discovered, never counted.** MSVC lays out consecutively-declared overloads in
reverse declaration order, so the 3-argument `RHISetUAVParameter` (`:200`) and the `InitialCount`
overload (`:209`) sit at 16 and 17 in an order this file does not assume; both are hooked with a
thunk that forwards all five arguments (correct for either), and each slot is CLASSIFIED by the
objects it is handed. Slots 14 (`RHISetShaderTexture`), 15, 18 (`RHISetShaderResourceViewParameter`)
and 19 (`RHISetShaderUniformBuffer`) are pinned by the validated layout — no overload pair precedes
them except the four `{}` `UAVOverlap` slots, which are required `ret` predictions.

**The resource hop needs no D3D12RHI layout.** `TD3D12ViewDescriptorHandle::CreateViewWithCounter`
calls `ID3D12Device::CreateUnorderedAccessView(resource, counter, &desc, Handle)` (`D3D12View.h:668-679`)
— a real D3D12 call on the device we already hook, and the descriptor shadow records exactly that
`(Handle.ptr → resource)` pair. The `FRHIUnorderedAccessView*` handed to the thunk is scanned
(qwords 1..39, every read `VirtualQuery`'d and under SEH) for a value the shadow knows as a UAV's
CPU handle: **one hit locates the offset and yields the resource by two bookkeepers agreeing on a
64-bit value; zero hits or two hits is a refusal.** The offset is latched after three agreeing scans
and read alone thereafter. The prior-art document's §4.5 proposed this; §15.1 of the TAA-hook
report is why the alternative (`FD3D12View::Resource` behind a second base) was not built.

**Correlation is thread-local and consumed by the dispatch.** `RHISetComputeShader` opens a
bracket on the calling thread; the binds fill it by register (`t0..t7` as the engine's RHI
objects, `u0..u3` resolved at the bind, the `b` registers as a mask); our `Dispatch` hook closes it
as "the bracket for this dispatch" before the sink is asked anything. The engine-announced claim
(`taa_hook.cpp`, still inside the game's `Dispatch` call, so every object in the bracket is alive)
resolves the texture registers through `FRHITexture::GetNativeResource` (vtable slot 7, HARD on
this exe since L1; the call is refused unless the object's vptr is in the module and its first
eight slots are all code, which a sampler's one-entry vtable cannot pass) and the SRV register
through the same handle scan with its own latch, and judges every register against the descriptor
walk's answer. `UnsetShaderUAVs`' null binds are ignored. The `FRHIUnorderedAccessView*` is never
carried past its call; the `ID3D12Resource*` is, and is checked against the registry at the claim.

**What the assertion covers** — and this is wider than `u0`, per
`docs/RESEARCH-ENGINE-AWARE-REPLAN.md` §1: `u0` (with a desc check: live, 2D, UAV-capable, HDR
float, at least the announced rect); `t0..t5` by register against the walk's SRVs; and the View
constant buffer's `b` register — `FTAAStandaloneCS` references exactly one uniform-buffer struct,
so a bracket with one `RHISetShaderUniformBuffer` names the View register outright, which the
slot-order search of CLAUDE.md §2.6 has been guessing at (facts §36.18).

### 10.5 A free self-check that ships regardless

`FRDGResource::Name` is `const TCHAR* const Name` at +8, **unconditional in Shipping**
(`RenderGraphResources.h:61`; it is not under `RDG_ENABLE_DEBUG`). At the seam, after the
forwarded `AddPasses` has written `*OutSceneColorTexture`, that texture is `u0` and alive, so its
`Name` is read (guarded) and must be `L"TemporalAA"`. One pointer read per announcement that
validates the RDG layout L1's `ResourceRHI @16` sits in; `u0name: ok= bad= unreadable=` on the
`[seam]` line.

### 10.6 Windows-portability, stated per prediction

| What | Depends on |
|---|---|
| The `.pdata` lookup | the PE format — every x64 image |
| The vtable and its `ret` predictions | the game's own `.rdata` and MSVC codegen |
| The CPU-handle cross-match | an opaque 64-bit key our shadow recorded from a real `ID3D12Device::CreateUnorderedAccessView` — on Windows a `heap_base + index × increment` value, on vkd3d a pointer; both are just keys |
| The resource registry and the desc check | real `ID3D12Device` creation hooks |
| `FRHITexture::GetNativeResource` slot 7 | the engine's vtable, HARD on this exe |
| The thread-local bracket | the engine executing one context on one thread |

Nothing consults vkd3d-proton, DXVK or Wine. The one thing the box's configuration DOES affect is
the seed: under a proxied command list (ReShade as `dxgi.dll`, Config B) every dispatch reaches our
hook from ReShade's module, the return address is foreign, and discovery refuses loudly
(`seed foreign=` on the `[u0]` line, one WARN naming the module). Config A is what this was measured
on.

### 10.7 What this changes about §7's consequence for the descriptor shadow

§7 says the shadow's expensive half has one shipping-path reader, the SRV/UAV table walk, and that
its irreducible job is naming `u0`. **The reader still exists; its job is no longer irreducible.**
If the level-2 assertion runs clean across gameplay — `assert: disagree=0`, `regs: disagree=0`,
`viewReg: disagree=0`, `noBind=0` — the walk's every answer for the TAA pass has an engine-sourced
twin, and level 3 (not built) could feed `DispatchBindings` from the bracket instead. What that
would delete is the replan's list (~1 450 lines, `shadow-copy`'s 1.644 ms); what it would not
delete is the root shadow, which the state restore needs and D3D12 offers no getter for.

**Measured results are in `docs/STRAY-RENDERING-FACTS.md` §37.** Nothing in this section is
evidence that it works; it is the design and the reasons a wrong answer cannot be used.
