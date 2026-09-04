# Has anyone solved `u0` from an external DLL? Prior art, tested against our "no engine route" verdict

**Scope.** `docs/RESEARCH-U0-IDENTITY.md` closes with *"There is no engine route to `u0`'s identity
for the frame in which we need it. The descriptor shadow stays"*, on the strength of seven
mechanisms each refused for its own reason. That verdict was reached against Epic's 4.27 source
alone. This document tests it against **what other people have built**, and against a re-read of
the same Epic tree prompted by what they built.

**Verdict: the conclusion is WRONG, and it is wrong in two specific, independently-confirmed
places.** Both were found by looking at one project — `praydog/UEVR`, an injected DLL that adds VR
to shipping UE4/UE5 games without engine source — which already does, in production, across
hundreds of titles, the thing §4.1/§4.2 and §14.2 say cannot be done: **name a UE render target
from outside the engine, and get a callback on the render thread inside `FRDGBuilder::Execute()`.**

Nothing here has run. There was no box; the other agent has it.

**Sources.** Epic's official `EpicGames/UnrealEngine` @ **`3abfe77d0b24a6d8bacebd27766912e5a5fa6f02`**
(branch `4.27`) — same tree `RESEARCH-U0-IDENTITY.md` cites, read directly via the GitHub API, not
from a mirror. `praydog/UEVR` @ `master` and @ `0587a468602aef102b55741bd7931a93ac34d483` (the last
commit before its SDK moved to a private submodule — that commit is why the locator source below
is quotable at all). Plus DeepWiki interrogation of `crosire/reshade`, `cdozdil/OptiScaler`,
`SpecialKO/SpecialK`, `NVIDIAGameWorks/dxvk-remix`, `UE4SS-RE/RE-UE4SS`, `chongdashu/unreal-mcp`,
`mikeroyal/Unreal-Engine-Guide`.

---

## 0. Verdict, up front

| Refusal in `RESEARCH-U0-IDENTITY.md` | Status after this review |
|---|---|
| §4.1 "name the resource — no names in a Shipping build" | **HALF WRONG.** The claim about *D3D12 objects* is right. But the name is a **live function argument** at the pool call, in Shipping, and that is where UEVR reads it (§2) |
| §4.2 "hook `FindFreeElementForRDG` — no self-validating constant, no shipping-surviving string anchor" | **WRONG on both halves.** The anchor is a *caller's* literal, not one inside the function (UEVR's technique, field-proven); and the self-validating constant is the `InDebugName` argument itself, checked on **every call** rather than once at install (§2.2) |
| §14.2 "no hookable point in the `Execute()` window" (transferred to the output by §2) | **WRONG.** `ISceneViewExtension::PostRenderViewFamily_RenderThread` and `PostRenderView_RenderThread` are invoked *from inside RDG pass lambdas* (`SceneRendering.cpp:3182-3200 @ 4.27/3abfe77`), i.e. on the render thread during `Execute()`. UEVR installs a fake `ISceneViewExtension` into shipping UE binaries to get exactly this (§3) |
| §4.2's other half — authoring an RDG pass needs no ABI | **STANDS.** Nobody does it. UEVR does not; NVIDIA can only because they compile in |
| `r.RHICmdBypass=1` | **STANDS** (§4.4 adds that it does not even skip the virtual — it only moves the thread) |
| `PrevFrameViewInfo` at the seam is null | **STANDS**, and §3 does not need it |
| The descriptor-shadow's ~1.7 ms is irreducible | **NO.** Two routes (§2, §3) and the coordinator's third (§4) each name `u0` without it |

**The prior-art picture, stated as a rule, because it is the useful part.** Every project that
identifies resources **at the graphics API** — ReShade, Special K, 3Dmigoto-class tools,
dxvk-remix, OptiScaler — uses descriptor/binding tracking plus heuristics plus, eventually,
user-authored per-game rules. Not one of them has an engine route, and dxvk-remix's answer to
"which texture is this" is literally *a hash list the user maintains*. **That is a genuine,
strong confirmed negative — for API-level tools.** UEVR is the counterexample, and the reason it
is a counterexample is precisely that it is **not** an API-level tool: it is an *engine-aware*
injector that reads UE's own objects. **We are an engine-aware injector too, and we have been
reasoning like an API-level one.**

---

## 1. Layer classification (the no-translation-layers constraint)

The user's constraint: *"avoid translation layers — so no DXVK etc. Only DX12 or UE if possible,
so it works on Windows."* Everything below is classified:

| Technique | Layer | Windows-native? |
|---|---|---|
| UEVR pool hook (§2) | **engine** (UE objects, non-virtual function hook) | yes |
| Fake `ISceneViewExtension` (§3) | **engine** (UE vtable + engine array) | yes |
| `IRHIComputeContext::RHISetUAVParameter` vtable hook (§4) | **engine** (UE RHI vtable) | yes |
| `ID3D12Device::CreateUnorderedAccessView` hook (§4.5) | **D3D12** (real API) | yes |
| ReShade / Special K descriptor + binding tracking (§5) | **D3D12** | yes (but heuristic) |
| dxvk-remix draw-call categorisation (§5) | **translation layer** (D3D9→Vulkan) | **disqualified** |
| Anything reading vkd3d-proton / DXVK internals | translation layer | **disqualified** |

No candidate route in this document depends on a translation layer. Our existing `ext_unhook`
(CLAUDE.md §1) does — but that is a *coexistence repair*, not an identification route, and is out
of scope here.

---

## 2. THE FIND: UEVR names UE render targets from outside, in Shipping, by hooking the pool

### 2.1 What it does

`praydog/UEVR` hooks `FRenderTargetPool::FindFreeElement` and keys every pooled render target it
sees by the **debug name argument**, then hands out the native D3D11/D3D12 resource on request.
Verbatim, from `src/mods/vr/RenderTargetPoolHook.cpp` @ `praydog/UEVR` master:

```cpp
void RenderTargetPoolHook::on_post_find_free_element(
    sdk::FRenderTargetPool* pool,
    sdk::FPooledRenderTargetDesc* desc,
    TRefCountPtr<IPooledRenderTarget>* out,
    const wchar_t* name)
{
    if (name != nullptr) {
        std::scoped_lock _{g_hook->m_mutex};
        if (out != nullptr) {
            g_hook->m_render_targets[name] = out->reference;
        } else {
            g_hook->m_render_targets.erase(name);
        }
        if (!g_hook->m_seen_names.contains(name)) {
            g_hook->m_seen_names.insert(name);
            SPDLOG_INFO("FRenderTargetPool::FindFreeElement called with name {}", utility::narrow(name));
        }
    }
}
```

and `RenderTargetPoolHook.hpp`:

```cpp
template<typename T>
Microsoft::WRL::ComPtr<T> get_texture(const std::wstring& name) {
    if (auto it = m_render_targets.find(name); it != m_render_targets.end()) {
        const auto& rt = it->second;
        const auto& tex = rt->item.texture.texture;      // IPooledRenderTarget + 8
        if (tex == nullptr) return nullptr;
        auto native_resource = (T*)tex->get_native_resource();   // FRHITexture vtable
        ...
    }
}
```

It is **exposed as a public plugin API** — `UEVR_FRenderTargetPoolHookFunctions { activate;
get_render_target(const wchar_t* name); }` and `UEVR_FRHITexture2DFunctions {
get_native_resource(...); }` in `include/uevr/API.h` — i.e. this is a supported, shipped
mechanism, not an experiment. **HARD** (read from the repository).

### 2.2 How it FINDS the function, and this is the part that refutes §4.2

Our §4.2 refused the pool because `FindFreeElementForRDG` / `FindFreeElementInternal` have no
string literal *inside them* that survives Shipping. **UEVR does not look inside the function.**
It anchors on a **caller's argument literal** and disassembles forward to the CALL.
`shared/sdk/FRenderTargetPool.cpp` @ `praydog/UEVR` `0587a46`, verbatim structure:

1. scan the module for the wide string `L"SceneDepthZ"` (`utility::scan_strings(module, L"SceneDepthZ", true)`);
2. find the displacement reference to it (`scan_displacement_reference`) — the `lea` that loads the
   name argument;
3. **linearly decode forward up to 20 instructions** for the first `CALL`
   (`utility::exhaustive_decode`), handling both `E8` direct calls and import-table
   pointer calls;
4. that call target **is** `FRenderTargetPool::FindFreeElement`.

Epic's 4.27 confirms the anchor exists and is unique enough:
`GRenderTargetPool.FindFreeElement(RHICmdList, Desc, SceneDepthZ, TEXT("SceneDepthZ"), ...)` at
`SceneRenderTargets.cpp:1543 @ 4.27/3abfe77` — one of **25** `FindFreeElement` call sites in that
file alone, each passing a distinct literal (`TEXT("GBufferA")`, `TEXT("GBufferVelocity")`,
`TEXT("ScreenSpaceAO")`, …). **HARD.**

**And the name argument is live in Shipping.** Our §4.1 established that no *D3D12 object* carries
a name, which is true (`D3D12RHIPrivate.h:94`, `RenderTargetPool.cpp:612-618`) — but it then read
that as "the engine cannot tell us which texture this is", and that does not follow. The name is a
`const TCHAR*` **parameter**, and `FindFreeElementInternal` uses it unconditionally:

```cpp
// RenderTargetPool.cpp:403, 411 @ 4.27/3abfe77   (both outside any #if)
UE_LOG(LogRenderTargetPool, Display, TEXT("%d MB, NewRT %s %s"), ..., InDebugName);
...
FRHIResourceCreateInfo CreateInfo(Desc.ClearValue);
CreateInfo.DebugName = InDebugName;
```

**HARD.** UEVR's own log line printing real names in shipping games is the field confirmation.

**The self-validating constant §4.2 said did not exist is the argument itself.** It is *stronger*
than the `ITemporalUpscaler` scan's `0.5`/`2.0` pair, because that pair is checked once at install
while this is checked on every call: a hook on the wrong function would receive a garbage
`const TCHAR*` that never reads as a plausible wide string and never equals the name we want.

### 2.3 What the pool call gives us for `u0`, exactly

Chain, all from `4.27/3abfe77`:

```
FRDGBuilder::BeginResourceRHI                                     RenderGraphBuilder.cpp:2080
  -> GRenderTargetPool.FindFreeElementForRDG(RHICmdList, Texture->Desc, Texture->Name)
       -> FindFreeElementInternal(RHICmdList, Translate(Desc), Name, false, false)
                                                                   RenderTargetPool.cpp:320-328
          returns TRefCountPtr<FPooledRenderTarget>
  -> Texture->SetRHI(PooledRenderTarget, PreviousOwner)             RenderGraphBuilder.cpp:2084
       -> ResourceRHI = PooledTexture->GetRHI()                     RenderGraphResources.cpp:208
```

* `IPooledRenderTarget` has **one vptr and no data member before `RenderTargetItem`**
  (`RendererInterface.h:477-524`), whose first member is `FTextureRHIRef TargetableTexture`
  (`:428-465`). So `*(FRHITexture**)((char*)pooled + 8)`. **HARD from the header**, and
  **independently corroborated** by UEVR's field-tested `struct IPooledRenderTarget :
  IRefCountedObject { virtual bool IsFree(); virtual void* GetDesc(); FSceneRenderTargetItem item; }`
  in `shared/sdk/StereoStuff.hpp`. This upgrades `RESEARCH-U0-IDENTITY.md` §3.1's `+8` from
  **[derived]** to **[derived, corroborated by a shipping implementation across many UE builds]**.
* `SetRHI` takes `InPooledRenderTarget->GetRDG(RenderTargetTexture)`, and for a
  `GraphBuilder.CreateTexture` the `RenderTargetTexture` is `Targetable` — so `u0`'s
  `ResourceRHI` **is** that `+8` pointer. **HARD.**
* `FRHITexture::GetNativeResource` at vtable slot 7 is already HARD on this exe (facts §36.13).

### 2.4 What the name will be, and a correction to `RESEARCH-U0-IDENTITY.md`

§4.1 writes the pool name as `TEXT("TemporalAAHistory")`. **It is not.**

```cpp
// TemporalAA.cpp:554-562 @ 4.27/3abfe77
const TCHAR* const kTAAOutputNames[] = {
    TEXT("TemporalAA"),          // Main
    TEXT("TemporalAA"),          // MainUpsampling      <- Stray
    TEXT("TemporalAA"),          // MainSuperSampling
    TEXT("SSRTemporalAA"),
    TEXT("LightShaftTemporalAA"),
    TEXT("DOFTemporalAA"),
    TEXT("DOFTemporalAA"),
};
```

So `u0`'s `FRDGTexture::Name` — and therefore the pool call's `InDebugName` — is
**`L"TemporalAA"`**. **HARD.** The other TAA-family passes carry different names, which is
convenient: the SSR denoiser and DOF paths self-separate.

**A free corollary, and it costs nothing to build.** `FRDGResource::Name` is
`const TCHAR* const Name = nullptr;` — **unconditional, not under `RDG_ENABLE_DEBUG`**
(`RenderGraphResources.h:61 @ 4.27/3abfe77`). So at our existing `AddPasses` seam,
`*OutSceneColorTexture`'s `Name` should read `L"TemporalAA"` **right now**, with the RDG offsets we
already have. That is a one-pointer, zero-risk, read-only self-check on `kRdgResourceRhiOffset`'s
neighbourhood that can ship before any hook — and if it does *not* read `L"TemporalAA"`, something
about our RDG layout assumptions is wrong and we would want to know that first. **Recommend
building this regardless of which route wins.**

### 2.5 The honest residual: UEVR's anchor lands one function short of ours

This is the real remaining cost and it must not be glossed.

`FindFreeElement` (the one UEVR finds, `RenderTargetPool.cpp:623`) and `FindFreeElementForRDG`
(`:320`) are **two different functions** that both forward to `FindFreeElementInternal` (`:330`).
The RDG path — and therefore `u0` — goes through `FindFreeElementForRDG`, never through
`FindFreeElement`. **A hook placed exactly where UEVR places it would never see `u0`.** (This is
almost certainly why UEVR's UE5 support for this hook was commented out at one point: in UE5
nearly everything is RDG.)

So one extra step is needed, and there are three ways to take it, in descending order of
attractiveness:

1. **Direct anchor on `FindFreeElementInternal` via the `UE_LOG` format string.**
   `RESEARCH-U0-IDENTITY.md` §4.2 already named the falsifiable test: search
   `Stray-Win64-Shipping.exe` for `%d MB, NewRT %s %s`. That literal is **inside
   `FindFreeElementInternal` itself** (`RenderTargetPool.cpp:403`). If Stray ships with
   `USE_LOGGING_IN_SHIPPING=1` — not unusual for UE4 titles — this is a *unique, in-function*
   anchor and the whole problem collapses to one string scan. **This test costs one grep of a
   file the project already has and should be run before anything else in this section.**
   **UNCONFIRMED.**
2. **Two-step from `FindFreeElement`.** Anchor as UEVR does, then enumerate call targets inside
   `FindFreeElement` and pick the one with exactly two module-wide xrefs, the other being a
   ~10-instruction function (`FindFreeElementForRDG`). Checkable, but fiddly.
3. **Install-then-observe.** Hook the candidate in a mode that reads nothing and does nothing but
   count; require it to be called at least once with `arg3` reading as the wide string
   `L"TemporalAA"` within N frames; unhook and refuse otherwise. This is self-validating in the
   house idiom — `FindFreeElement` can never produce that name, only `FindFreeElementInternal`
   can — but it means installing an inline hook on a not-yet-proven function, which is a crash if
   wrong.

### 2.6 Two implementation traps, both from the ABI

* **`FindFreeElementInternal` returns `TRefCountPtr<FPooledRenderTarget>` by value.** `TRefCountPtr`
  has a non-trivial destructor, so under the MSVC x64 ABI it is returned via a **hidden pointer in
  RCX** and every declared argument shifts one register right: `(retptr, this, RHICmdList, Desc,
  InDebugName, bDeferTextureAllocation, bDoAcquireTransientTexture)` — the name lands in the
  **stack** slot, not `R9`. UEVR's hook does not hit this because `FindFreeElement` returns `bool`
  and takes `Out` as an explicit reference. **[derived] from the ABI**, but a wrong signature here
  reads garbage and is exactly the kind of thing that "works" until it crashes.
* **`FindFreeElementInternal` is `private`** (`RenderTargetPool.h:331`) — irrelevant to a
  monolithic shipping build (nothing is exported anyway), noted only so nobody looks for an export.

### 2.7 Cost, and how a wrong answer is refused

**Cost.** One inline trampoline on a render-thread function called on the order of tens of times
per frame; a wide-string compare and a desc compare per call. Against the 1.694 ms
`shadow-copy` this replaces, the cost is noise. **It writes to the game's code section** — this
project has patched vtables (`ext_unhook`) but never engine code, and that is a real escalation to
state plainly.

**How a wrong answer is refused.** Four independent predictions, all checkable per frame:

1. `InDebugName` must read as a null-terminated wide string equal to `L"TemporalAA"`;
2. the `FPooledRenderTargetDesc` extent/format must equal the announcement's output rect and the
   `R16G16B16A16_FLOAT` history format (§2.3);
3. the `+8` walk must yield an `FRHITexture*` whose `GetNativeResource()` lands in **our own
   resource registry** — the same warrant L1 uses for depth and velocity;
4. it must agree with the descriptor shadow's answer while the shadow is still running.

Prediction 4 is the whole deployment plan: ship it as an **oracle first** (`observe`), let it
disagree-count for a session, and only then let it become authoritative and delete the walk. That
is the ladder `EngineSeam` already proved.

**How it could go silently wrong, stated so it is designed against.**
* `kRenderTargetCount == 4` (`SceneRendering.h:810`) and **all four** `NewHistoryTexture[i]` are
  created with the *same name and the same desc* (`TemporalAA.cpp:690-694`). Only index 0 (and
  index 1 in the DOF-alpha config) survives `ClearUnusedGraphResources`
  (`TemporalAA.cpp:944-948`), so only those get a pool element — but that is an inference about
  RDG culling, not a guarantee. **If two `L"TemporalAA"` allocations of identical desc appear in
  one frame, the mechanism must decline, not pick.**
* Several views per frame (scene captures, cinematics — `RESEARCH-ENGINE-TAA-HOOK.md` §17) each run
  a main TAA, each allocating `L"TemporalAA"`. The capture must be bound to a specific
  announcement, not to the frame.

---

## 3. SECOND FIND: there ARE hookable points inside `FRDGBuilder::Execute()`

`RESEARCH-ENGINE-TAA-HOOK.md` §14.2 enumerates five candidates for a hook in the `Execute()` window
and refuses all five, concluding *"this is a 'no' rather than a 'not yet'"*. The enumeration is of
**RDG's own internals**. It missed that the engine itself calls out of that window, into a virtual
interface, on the render thread — and that the interface is one an injected DLL can implement.

### 3.1 The engine calls `ISceneViewExtension` from inside RDG pass lambdas

```cpp
// SceneRendering.cpp:3181-3200 @ 4.27/3abfe77   (FSceneRenderer::RenderFinish)
for (int32 ViewExt = 0; ViewExt < ViewFamily.ViewExtensions.Num(); ++ViewExt)
{
    RDG_EVENT_SCOPE(GraphBuilder, "ViewFamilyExtension(%d)", ViewExt);
    ISceneViewExtension& ViewExtension = *ViewFamily.ViewExtensions[ViewExt];

    AddUntrackedAccessPass(GraphBuilder, [this, &ViewExtension](FRHICommandListImmediate& RHICmdList)
    {
        ViewExtension.PostRenderViewFamily_RenderThread(RHICmdList, ViewFamily);
    });

    for (int32 ViewIndex = 0; ViewIndex < ViewFamily.Views.Num(); ++ViewIndex)
    {
        AddUntrackedAccessPass(GraphBuilder, RDG_EVENT_NAME("ViewExtension(%d)", ViewIndex),
            [this, &ViewExtension, ViewIndex](FRHICommandListImmediate& RHICmdList)
        {
            ViewExtension.PostRenderView_RenderThread(RHICmdList, Views[ViewIndex]);
        });
    }
}
```

`AddUntrackedAccessPass` is `GraphBuilder.AddPass(MoveTemp(Name), ERDGPassFlags::UntrackedAccess,
MoveTemp(ExecuteLambda))` (`RenderGraphUtils.h:505-508`) — an ordinary RDG pass. **So these two
virtuals run on the render thread, during `FRDGBuilder::Execute()`'s pass loop, between step 5 and
step 8 of `RESEARCH-U0-IDENTITY.md` §1.3. HARD.**

### 3.2 And RDG assigns *every* resource before *any* pass runs

`Execute()` runs `CollectPassResources` for **all** passes in one loop
(`RenderGraphBuilder.cpp:1228-1233`) and only then enters the execute loop (`:1304-1310`). So by
the time any pass lambda runs, `u0`'s `SetRHI` has already happened. And `EndResourceRHI`
(`:2263-2290`) never clears `ResourceRHI` — it only drops `Allocation`. **`u0Texture->ResourceRHI`
is non-null from `CollectPassResources` until `Clear()`. HARD.**

This also sharpens `RESEARCH-U0-IDENTITY.md` §1.3, which presents step 5 (`BeginResourceRHI`) and
step 6 (the pass loop) as interleaved per pass. They are two separate whole-graph phases.

### 3.3 What that buys, and why it is the cheapest of the three routes

We already hold `FRDGTexture* u0` from `AddPasses`. Inside a `PostRenderViewFamily_RenderThread`
callback we are on the **same thread, in the same frame**, with that wrapper still alive. So:

```
read *(FRHITexture**)((char*)u0Texture + kRdgResourceRhiOffset)   // the offset we ALREADY validate
     -> GetNativeResource()  (vtable slot 7, already HARD)        // -> ID3D12Resource*
```

**No new offset. No new vtable index. No code patching.** It reuses, verbatim, the two hops L1
already performs on depth and velocity every frame — the only thing that changes is *when* they
are performed. That is a materially smaller correctness surface than §2 or §4.

### 3.4 Installing it: proven, but it writes engine data

UEVR does this in shipping UE binaries. Its method, from praydog's own write-up
(<https://praydog.com/reverse-engineering/2023/07/03/uevr.html>) and DeepWiki over the repo:

* find `GEngine` by string anchor (`"CALIBRATEMOTION"`), then the stereo-device offset, then
  `FSceneViewExtensions` at `stereo_device_offset + 2 * sizeof(TWeakPtr)`;
* build a static `g_view_extension_vtable` array and install an object using it;
* **discover the vtable indices at runtime** rather than assuming them — first fill the vtable with
  "analysis dummy" thunks (*"a dummy vtable that has many variants of the same function using
  template metaprogramming"*), identify `IsActiveThisFrame` as the most-called slot, then
  `BeginRenderViewFamily` / `PreRenderViewFamily_RenderThread` by watching which argument offsets
  hold *"integer-like values that are increasing by 1 every call"*;
* only then swap in the real implementations.

**HARD** that UEVR does this and ships it. **UNCONFIRMED** for our target, and three honest costs:

1. **It writes into engine state** — appending to (or swapping a vtable inside) `GEngine`'s view
   extension array. This project has never written engine memory, and
   `RESEARCH-U0-IDENTITY.md` §3.3 refuses a much smaller such write (`FTemporalAAHistory::RT[1]`
   as a frame tag) on exactly that ground. Consistency demands the same scepticism here; the
   difference is that this one is a mechanism a shipping tool exercises across hundreds of titles,
   and the other was invented on the spot.
2. **The vtable-index discovery is a whole sub-project.** UEVR needed emulation, string-presence
   analysis, and per-version heuristics because *"the relevant vtable indices … [have] changed a
   lot over the years of Unreal Engine development"*. We need it for exactly one engine version,
   which is far easier — but it is not free, and a wrong index is a call into the wrong function.
3. **The engine must run at least one view extension.** If `ViewFamily.ViewExtensions` is empty the
   `AddUntrackedAccessPass` block adds nothing; ours must be in the array *and*
   `IsActiveThisFrame` must return true.

**Two smaller variants worth considering before the full UEVR treatment:**
* If Stray already registers any `ISceneViewExtension` (VR plugins, media framework, some
  platform plugins do), hooking that *existing* object's vtable slot is a smaller write than
  inserting a new object — though a class vtable in `.rdata` is process-wide and needs
  `VirtualProtect`.
* `IPersistentViewUniformBufferExtension` is a second global extension list
  (`SceneRendering.cpp:3761-3765`) with the same insertion shape, but its `EndFrame()` runs
  *after* `Render()` returns, i.e. outside `Execute()`. **Not useful. Recorded so it is not
  rediscovered.**

---

## 4. The `RHISetUAVParameter` route (the coordinator's addition), tested against the outside world

### 4.1 Prior art: none found for hooking the RHI *context* vtable

Searched: DeepWiki over `praydog/UEVR` and `UE4SS-RE/RE-UE4SS`; web searches for
`"RHISetUAVParameter"`/`"IRHIComputeContext"` + hook/mod/inject, and `FD3D12CommandContext` +
vtable + injected DLL. **No project was found that hooks `IRHIComputeContext`,
`IRHICommandContext` or `FD3D12CommandContext`.** State that as *no prior art found with these
search terms*, not as *nobody has done it* — mod source often lives in Discord attachments and
Patreon posts that no search reaches.

### 4.2 The closest prior art is one level up, and it is encouraging

UEVR hooks the **RHI command object** vtable rather than the context: from
`PreRenderViewFamily_RenderThread` it takes `FRHICommandListBase::root`, decides whether it is a
"new" (vtable-based `FRHICommandBase`) or "old" (function-pointer) command, and for the new shape
**installs a replacement vtable filled with `hooked_command_fn<0..6>`, then determines which index
is really `ExecuteAndDestruct` by observing which one gets called and on which thread**. It also
maintains an `RHIThreadWorker` specifically to marshal work onto the RHI thread, and enqueues its
own render poses from inside the hooked command.

Three things this settles for the coordinator's route:

* **Hooking a UE RHI-layer vtable from an injected DLL is field-proven in shipping UE builds.**
* **The slot index must be DISCOVERED, not derived** — UEVR brute-forces it rather than trusting a
  count, and it does so for a class with far fewer virtuals than `IRHIComputeContext`.
* **The RHI thread being separate did not defeat them**; it forced an explicit worker and explicit
  thread-identity checks. Expect the same.

### 4.3 The decisive engine question: does the TAA pass bind `u0` through `RHISetUAVParameter`? **Yes.**

The coordinator flagged this as the thing that would make the approach fail silently. Chain,
all `@ 4.27/3abfe77`:

```cpp
// TemporalAA.cpp:950  — the TAA pass is an ordinary FComputeShaderUtils::AddPass
FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("TAA %s%s %dx%d -> %dx%d", ...),
                             ComputeShader, PassParameters, GroupCount);

// RenderGraphUtils.h:246-253
static void Dispatch(FRHIComputeCommandList& RHICmdList, ..., FIntVector GroupCount) {
    RHICmdList.SetComputeShader(ShaderRHI);
    SetShaderParameters(RHICmdList, ComputeShader, ShaderRHI, Parameters);
    RHICmdList.DispatchComputeShader(GroupCount.X, GroupCount.Y, GroupCount.Z);
    UnsetShaderUAVs(RHICmdList, ComputeShader, ShaderRHI);
}

// ShaderParameterStruct.h:244-250  -> SetShaderUAV -> :151-157
case UBMT_RDG_TEXTURE_UAV:
    auto GraphUAV = *reinterpret_cast<FRDGUnorderedAccessView* const*>(Base + ParameterBinding.ByteOffset);
    GraphUAV->MarkResourceAsUsed();
    RHICmdList.SetUAVParameter(ShadeRHI, ParameterBinding.BaseIndex, GraphUAV->GetRHI());

// RHICommandList.h:2390-2399
FORCEINLINE_DEBUGGABLE void SetUAVParameter(FRHIComputeShader* Shader, uint32 UAVIndex, FRHIUnorderedAccessView* UAV) {
    if (Bypass()) { GetComputeContext().RHISetUAVParameter(Shader, UAVIndex, UAV); return; }
    ALLOC_COMMAND(FRHICommandSetUAVParameter<FRHIComputeShader>)(Shader, UAVIndex, UAV);
}

// RHIContext.h:200 (pure virtual)  ->  D3D12Commands.cpp:680 (FD3D12CommandContext override)
```

**`FTAAStandaloneCS` uses `SHADER_USE_PARAMETER_STRUCT`** (`TemporalAA.cpp:149`), **not**
`SHADER_USE_ROOT_PARAMETER_STRUCT` — which matters, because `SetShaderParameters` opens with
`checkf(Bindings.RootParameterBufferIndex == kInvalidBufferIndex, TEXT("Can't use
SetShaderParameters() for root parameter buffer index."))` (`ShaderParameterStruct.h:195`). The
root-parameter path exists and would bypass the per-resource RHI calls entirely; this shader does
not take it. **HARD.**

**And 4.27 has no batched bind.** `IRHIComputeContext` (`RHIContext.h:124-260`) has no
`RHISetShaderParameters`; the batched `FRHIBatchedShaderParameters` design arrives in UE5.3+.
`RHISetUAVParameter` is the only route by which a compute UAV is bound. **HARD for 4.27.**

### 4.4 Ways it could still be bypassed, enumerated

| Path | Effect on the hook |
|---|---|
| `r.RHICmdBypass=1` | **does NOT skip the virtual** — `Bypass()` calls `GetComputeContext().RHISetUAVParameter(...)` directly. It moves the call to the render thread. A hook that assumes "I am on the RHI thread" breaks; the hook itself still fires |
| `UnsetShaderUAVs` after the dispatch | fires `SetUAVParameter(shader, 0, nullptr)`. **Must be ignored**, or the published `u0` is nulled immediately after every dispatch |
| The SRV "HACKHACK" reorder (`ShaderParameterStruct.h:258-266`) | SRVs are bound **after** UAVs. Irrelevant for `u0`, load-bearing if the same hook is extended to name colour/depth/velocity by register |
| MGPU `FD3D12CommandContextRedirector` | a **different class with a different vtable**. Not used on a single GPU; would silently miss every call if it were |
| Async compute | a different `FD3D12CommandContext` *instance*, same class vtable — so the hook fires, but "last UAV0 before the dispatch" must be tracked per context/thread, not globally |
| Parallel command-list translation | still `IRHICommandContext` subclasses; in D3D12 still `FD3D12CommandContext`. Fires |
| A licensee switching the shader to a root-parameter struct | **silent total miss**. Detectable only by the hook never firing for the TAA dispatch — so *count it* |

### 4.5 The `FRHIUnorderedAccessView*` → `ID3D12Resource*` hop, and a self-validating way to do it

`FD3D12CommandContext::RHISetUAVParameter` receives the `FRHIUnorderedAccessView*` and does
`RetrieveObject<FD3D12UnorderedAccessView>(UAVRHI)` (`D3D12Commands.cpp:684`). The class is:

```cpp
// D3D12View.h:909 @ 4.27/3abfe77
class FD3D12UnorderedAccessView : public FRHIUnorderedAccessView,
                                  public FD3D12View<D3D12_UNORDERED_ACCESS_VIEW_DESC>,
                                  public FD3D12LinkedAdapterObject<FD3D12UnorderedAccessView>

// D3D12View.h:701-716
template <typename TDesc> class FD3D12View {
private:
    TD3D12ViewDescriptorHandle<TDesc> Descriptor;   // { FD3D12Device* Parent; CD3DX12_CPU_DESCRIPTOR_HANDLE Handle; uint32 Index; }
protected:
    ViewSubresourceSubsetFlags Flags;
    FD3D12ResourceLocation*    ResourceLocation;
    FD3D12ResidencyHandle*     ResidencyHandle;
    FD3D12Resource*            Resource;
    CViewSubresourceSubset     ViewSubresourceSubset;
    TDesc                      Desc;
```

Two things follow, and the second is the useful one:

1. **`FRHIUnorderedAccessView` is the FIRST base**, so the pointer the hook receives is the object
   base — good. But `FD3D12View` is a **second base at a non-zero offset**, so reaching `Resource`
   is *two* unknown offsets (base offset + member offset), not one. `Resource` is also an
   `FD3D12Resource*`, not an `ID3D12Resource*` — a third hop. Doing this as three derived
   constants is exactly what `RESEARCH-ENGINE-TAA-HOOK.md` §15.1 refuses for the View CB.
2. **There is a self-validating alternative that needs no layout knowledge at all.**
   `TD3D12ViewDescriptorHandle::CreateView` calls
   `(GetParentDevice()->GetDevice()->*&ID3D12Device::CreateUnorderedAccessView)(Resource, &Desc, Handle)`
   (`D3D12View.h:668-679`) — a **real `ID3D12Device` method, on the device we already hook**, with
   the `ID3D12Resource*` and the CPU descriptor handle both as explicit arguments. So:
   * hook `ID3D12Device::CreateUnorderedAccessView` (view creation only — rare, not per frame) and
     record `Handle.ptr -> ID3D12Resource*`;
   * on the first `RHISetUAVParameter`, **scan the first ~40 qwords of the
     `FRHIUnorderedAccessView*` for a value present in that map.** `Handle` is inside `Descriptor`,
     the *first* member of `FD3D12View`, so it is close to the base-class boundary;
   * a hit **simultaneously locates the offset and yields the resource**, by two independent
     bookkeepers agreeing on a 64-bit value — the same warrant `RESEARCH-ENGINE-TAA-HOOK.md`
     §15.2's prediction 3 rests on;
   * latch the offset, re-validate every call, and **decline on zero hits or more than one**.

   This is **engine-level plus real-D3D12-level, Windows-native, no translation layer**, and it
   needs no `FD3D12Resource` or `FD3D12ResourceLocation` layout at all.

### 4.6 Two traps for the vtable index

* **MSVC emits overloaded virtuals in REVERSE declaration order within a group.**
  `IRHIComputeContext` declares `RHISetUAVParameter(FRHIComputeShader*, uint32,
  FRHIUnorderedAccessView*)` at `RHIContext.h:200` immediately followed by the 4-argument
  `InitialCount` overload at `:209`; `IRHICommandContext` re-declares three overloads at
  `:516`/`:525`/`:534`. **Do not count declarations to get a slot.** Discover it, as UEVR does.
  **[derived] from the MSVC ABI**, and the reason it is worth writing down is that a
  count-the-declarations derivation would look right and land on the `InitialCount` overload.
* **`IRHICommandContext : public IRHIComputeContext`** (`RHIContext.h:361`) — single inheritance,
  one vptr, and the compute-shader overload is an *override*, so it occupies the base's slot.
  **HARD**, and it means one patch covers both interfaces.

### 4.7 Getting the `IRHIComputeContext*` in the first place

Not a blocker, and it starts from something we already have:

* `FRDGBuilder::RHICmdList` is `FRHICommandListImmediate&` and is the **first data member** of
  `FRDGBuilder` (`RenderGraphBuilder.h:205`, with `private:` at `:207` and no data member before
  it) — and we are handed `FRDGBuilder&` at the `AddPasses` seam.
* `FRHICommandListBase::Context` / `ComputeContext` are `IRHICommandContext*` /
  `IRHIComputeContext*` at `RHICommandList.h:578-579`.
* Both offsets are **[derived] and must be discovered**: validate by the candidate's `*(void**)`
  landing in the main module's read-only section with executable entries — the pointer-topology
  check L1 already implements.

### 4.8 One anchor idea that is DEAD, recorded so it is not tried

`FRHICommandSetUAVParameterString::TStr()` returns `TEXT("FRHICommandSetUAVParameter")`
(`RHICommandList.h:961-964`) and looks like a perfect anchor for the command's `Execute`. It is
not: its only use is
`TRACE_CPUPROFILER_EVENT_SCOPE_ON_CHANNEL_STR(NameType::TStr(), RHICommandsChannel)` inside
`FRHICommand::ExecuteAndDestruct` (`:757-759`), and in Shipping `CPUPROFILERTRACE_ENABLED` is `0`
(`CpuProfilerTrace.h:10-16`) so the macro expands to nothing and the literal is never referenced.
**HARD: that string is stripped in a Shipping build.**

---

## 5. The confirmed negatives — worth recording so nobody looks again

Every one of these is a project that solves "identify the engine's buffers from outside" and none
has an engine route. Together they are the reason the original verdict *felt* right.

| Project | Layer | How it identifies resources | Engine route? |
|---|---|---|---|
| **ReShade** (`crosire/reshade`) | D3D12 API | Intercepts API calls only; `generic_depth` add-on tracks per-draw-call and per-resource state and picks the depth buffer heuristically. Tracks D3D12 descriptor-table copies by converting every `CopyDescriptors`/`CopyDescriptorsSimple` into `reshade::api::descriptor_table_copy` and firing an event — with an explicit per-copy conversion + temp-allocation + event cost | **No.** *"ReShade operates by intercepting API calls rather than having engine-level access"* |
| **Special K** (`SpecialKO/SpecialK`) | D3D11/12 API | Hooks `CreateTexture2D` / `Create*View` / `OMSetRenderTargets`; keeps `SK_D3D11_RenderTargets` binding tables; stashes the RTV descriptor in command-list private data (`SKID_D3D12RenderTarget0`). Where heuristics fail it ships **per-game plugins** (`src/plugins/nier.cpp` hard-codes NieR:Automata's bloom/AO render-target shapes) | **No** — and the per-game plugin is the tell |
| **dxvk-remix** (`NVIDIAGameWorks/dxvk-remix`) | translation layer (disqualified) | `DrawCallState` per draw call, plus **texture hashing against user-maintained lists** — `rtx.skyBoxTextures`, `rtx.terrainTextures`, `rtx.uiTextures`, `rtx.decalTextures` — plus geometric heuristics (orthographic + no Z-write ⇒ UI) | **No.** The final answer is a hash list a human curates |
| **OptiScaler** (`cdozdil/OptiScaler`) | D3D12 API | Does not identify anything: it lifts `color/depth/motionVectors/output` straight out of the game's own FSR2/FSR3/XeSS `dispatchDescription`. It knows `NVSDK_NGX_ENGINE_TYPE_UNREAL` only to pick resource barriers | **No**, and not applicable — it presupposes a game that already names them |
| **UE4SS** (`UE4SS-RE/RE-UE4SS`) | engine | AOB/pattern scanning (`patternsleuth`, Lua signature overrides), vtable layout tables from `.ini`. Its `SinglePassSigScanner` lists `D3D11RHI`/`D3D12RHI` as scan *targets* but there is no RHI/RDG API surface | **No API**, but the scanning + vtable-override infrastructure is exactly what §2/§3/§4 need |
| **`chongdashu/unreal-mcp`** | — | **Editor-only plugin**, `Type = "Editor"`, a TCP/JSON bridge to `UUnrealMCPBridge` (an `EditorSubsystem`). No `RenderCore`/`RHI`/`D3D12RHI` dependency; its closest rendering touch is `HighResScreenshot`. **Does not inject into shipping games** | **Not relevant** |
| **`mikeroyal/Unreal-Engine-Guide`** | — | A curated list of official UE features and tools (RenderDoc, Nsight, RGP). Nothing on render-target identification from outside, nothing on runtime hooking of shipping builds | **Not relevant** |
| **NVIDIA's own UE4 DLSS plugin** | in-engine | Authors an RDG pass and reads `PassParameters->SceneColorOutput->GetRHI()` inside its own lambda (`RESEARCH-U0-IDENTITY.md` §6) | Yes — **by compiling in.** Unavailable to us, and that remains true |

**The rule this yields.** *If your only vantage point is the graphics API, descriptor and binding
tracking plus heuristics is the state of the art and there is no better answer — the industry's
fallback is a human-curated list.* **We are not at that vantage point.** We are inside the process
with the engine's own objects reachable, and the one project that shares that vantage point (UEVR)
does not do descriptor tracking at all.

---

## 6. What this does NOT change

* **Authoring an RDG pass is still impossible from an injected DLL.** `AddPass` is a template over
  `FShaderParametersMetadata` instantiated at engine compile time. UEVR does not do it; NVIDIA can
  only because they are compiled in. `RESEARCH-U0-IDENTITY.md` §6.1's reading — *"the vendor, with
  full source access, did not find [another way] either"* — was over-read: NVIDIA had no *reason*
  to look for one. Their not looking is not evidence of absence.
* **`r.RHICmdBypass=1` is still the wrong trade** (§14.3), and §4.4 adds that it would not even be
  necessary: the virtual fires in both modes.
* **Reading `PrevFrameViewInfo` from the RHI thread is still silently wrong one frame in a
  pipelined steady state** (§3.3). None of the three routes here needs it.
* **`View.PrevViewInfo.TemporalAAHistory.RT[0]` is still a free one-frame-late oracle** (§5.2), and
  is now *more* attractive: it validates whichever of §2/§3/§4 gets built, from a completely
  independent direction, at the cost of one pointer read.
* **Frame N's `u0` ≠ frame N−1's `u0`** (§5.1) stands and is unaffected.

---

## 7. Recommendation

In order, cheapest and safest first:

1. **Read `(*OutSceneColorTexture)->Name` at the existing seam and log it.** Zero risk, no hook,
   no write; it must be `L"TemporalAA"` (§2.4). If it is not, stop and find out why before
   building anything else.
2. **Grep `Stray-Win64-Shipping.exe` for `%d MB, NewRT %s %s`** (§2.5). One command. Its presence
   turns the pool route into a single in-function string anchor and makes §2 the obvious winner;
   its absence costs nothing and closes route 1 of 3 in §2.5.
3. **Let the RHI-context ladder the other agent is building run to a verdict** (§4). It has the
   best cost profile — no engine-memory write, no code patch if the vtable page is restored — and
   §4.3 now says HARD that the call it depends on really does fire for this shader.
   §4.5's descriptor-handle cross-match is the recommended way to do its resource hop.
4. **Hold §3 (fake `ISceneViewExtension`) in reserve.** It reuses offsets we already validate and
   is therefore the *smallest correctness surface* of the three — but it is the *largest*
   engineering surface (vtable-index discovery) and the only one that writes engine data
   structures. It is the right answer if §4 fails on a binding path we have not foreseen.
5. **Whichever wins, ship it as an oracle against the shadow first**, and delete the SRV/UAV table
   walk only after a session of zero disagreements. The shadow is currently the only thing that
   has ever named `u0`; nothing here has run.

---

## 8. Provenance ledger

| Claim | Status |
|---|---|
| UEVR hooks `FRenderTargetPool::FindFreeElement`, keys pooled RTs by the `const wchar_t*` name argument, and exposes it as a public plugin API | **HARD** — `src/mods/vr/RenderTargetPoolHook.{cpp,hpp}` and `include/uevr/API.h` @ `praydog/UEVR` master, read directly |
| UEVR locates that function by scanning for `L"SceneDepthZ"`, resolving the displacement reference, and linearly decoding ≤20 instructions forward to the first CALL | **HARD** — `shared/sdk/FRenderTargetPool.cpp` @ `praydog/UEVR` `0587a46`, read directly |
| `TEXT("SceneDepthZ")` is passed to `FindFreeElement` in 4.27, one of 25 such literal call sites in one file | **HARD** — `SceneRenderTargets.cpp:1543` and `:1005-1741` @ 4.27/3abfe77 |
| `FindFreeElementForRDG` and `FindFreeElement` are distinct, both forwarding to `FindFreeElementInternal`; the RDG path (hence `u0`) uses the former | **HARD** — `RenderTargetPool.h:268-334`, `RenderTargetPool.cpp:320-328`, `:623`, `:703` @ 4.27/3abfe77 |
| The debug name is live in a Shipping build as a function argument (`CreateInfo.DebugName = InDebugName`, outside any `#if`) | **HARD** — `RenderTargetPool.cpp:403-411` @ 4.27/3abfe77 |
| `u0`'s pool debug name is `L"TemporalAA"`, not `L"TemporalAAHistory"` | **HARD** — `TemporalAA.cpp:554-562`, `:686-694` @ 4.27/3abfe77. Corrects `RESEARCH-U0-IDENTITY.md` §4.1 |
| `FRDGResource::Name` is unconditional (`const TCHAR* const Name`), not gated on `RDG_ENABLE_DEBUG` | **HARD** — `RenderGraphResources.h:61` @ 4.27/3abfe77 |
| `IPooledRenderTarget` has one vptr and no data member before `RenderTargetItem`, whose first member is `TargetableTexture` ⇒ `+8` | **HARD from the header** (`RendererInterface.h:428-465`, `:477-524`) and **corroborated in the field** by UEVR's `StereoStuff.hpp` layout |
| `FRDGTexture::SetRHI` assigns `ResourceRHI` from the *Targetable* pooled texture, i.e. the same `+8` pointer | **HARD** — `RenderGraphResources.cpp:182-209` @ 4.27/3abfe77 |
| `FindFreeElementInternal` returns `TRefCountPtr` by value ⇒ hidden return pointer in RCX, all arguments shift | **[derived] from the MSVC x64 ABI.** Not verified against Stray's code |
| `%d MB, NewRT %s %s` is inside `FindFreeElementInternal` and would be a unique in-function anchor if Stray ships with logging | **HARD** that the literal is there (`RenderTargetPool.cpp:403`); **UNCONFIRMED** whether it survives in Stray's exe |
| `ISceneViewExtension::PostRenderViewFamily_RenderThread` / `PostRenderView_RenderThread` run inside RDG pass lambdas, on the render thread, during `Execute()` | **HARD** — `SceneRendering.cpp:3181-3200` + `RenderGraphUtils.h:505-508` @ 4.27/3abfe77. **Refutes `RESEARCH-ENGINE-TAA-HOOK.md` §14.2's enumeration** |
| RDG collects resources for ALL passes before executing ANY pass; `EndResourceRHI` never clears `ResourceRHI` | **HARD** — `RenderGraphBuilder.cpp:1228-1233`, `:1304-1310`, `:2263-2290` @ 4.27/3abfe77 |
| ⇒ `u0Texture->ResourceRHI` is readable from any render-thread callback inside `Execute()`, using offsets we already validate | **HARD** from the two above |
| UEVR installs a fake `ISceneViewExtension` into shipping UE binaries and brute-forces its vtable indices at runtime | **HARD** that it does (repo + praydog's write-up, <https://praydog.com/reverse-engineering/2023/07/03/uevr.html>); **UNCONFIRMED** for Stray |
| `IPersistentViewUniformBufferExtension::EndFrame()` runs after `Render()`, outside `Execute()` | **HARD** — `SceneRendering.cpp:3761-3765` @ 4.27/3abfe77 |
| `FTAAStandaloneCS` uses `SHADER_USE_PARAMETER_STRUCT`, so `SetShaderParameters` binds `u0` via `RHICmdList.SetUAVParameter(shader, 0, UAV)` | **HARD** — `TemporalAA.cpp:149`, `:950`; `RenderGraphUtils.h:246-253`; `ShaderParameterStruct.h:151-157`, `:195`, `:244-250` @ 4.27/3abfe77 |
| 4.27 has no batched `RHISetShaderParameters`; `RHISetUAVParameter` is the only compute-UAV bind | **HARD** — `RHIContext.h:124-260` @ 4.27/3abfe77 |
| `r.RHICmdBypass=1` does not skip the virtual, only moves the thread | **HARD** — `RHICommandList.h:2390-2399` @ 4.27/3abfe77 |
| `UnsetShaderUAVs` fires `SetUAVParameter(..., nullptr)` after every dispatch | **HARD** — `ShaderParameterStruct.h:164-178`, `RenderGraphUtils.h:252` @ 4.27/3abfe77 |
| `IRHICommandContext : public IRHIComputeContext`, single inheritance, compute overload is an override sharing the base slot | **HARD** — `RHIContext.h:200`, `:361`, `:525` @ 4.27/3abfe77 |
| MSVC emits overloaded virtuals in reverse declaration order ⇒ the slot must be discovered | **[derived] from the MSVC ABI**, not measured here |
| `FD3D12UnorderedAccessView` has `FRHIUnorderedAccessView` as its FIRST base and `FD3D12View` as a second base at non-zero offset; `Resource` is behind `Descriptor`, `Flags`, `ResourceLocation`, `ResidencyHandle` | **HARD** — `D3D12View.h:701-716`, `:909` @ 4.27/3abfe77 |
| `TD3D12ViewDescriptorHandle::CreateView` calls `ID3D12Device::CreateUnorderedAccessView` with the resource and the CPU handle as explicit arguments ⇒ a hookable, self-validating cross-check | **HARD** — `D3D12View.h:636-679` @ 4.27/3abfe77. The *scan-for-the-handle* design built on it is **UNCONFIRMED**, proposed here |
| `FRDGBuilder::RHICmdList` is the first data member; `FRHICommandListBase::Context`/`ComputeContext` at `:578-579` | **HARD** that the members exist and their order (`RenderGraphBuilder.h:205-207`, `RHICommandList.h:578-579`); **[derived] and unmeasured** that any particular byte offset reaches them |
| `TEXT("FRHICommandSetUAVParameter")` is stripped in Shipping (its only use is a CPU-profiler trace macro that compiles away) | **HARD** — `RHICommandList.h:757-759` + `CpuProfilerTrace.h:10-16`, `:178-183` @ 4.27/3abfe77 |
| ReShade, Special K, dxvk-remix and OptiScaler all identify resources at the API layer, with heuristics and user-curated lists, and none has an engine route | **SOFT-to-HARD** — DeepWiki over each repository with source citations; not independently read line by line here |
| No project was found that hooks `IRHIComputeContext` / `FD3D12CommandContext` from an injected DLL | **Absence of evidence.** Search terms named in §4.1; mod source frequently lives where search does not reach |
| Anything in §2, §3 or §4 working on Stray's executable | **UNCONFIRMED.** Nothing was built, nothing was run |

---

## 9. Cross-references

* **`docs/RESEARCH-U0-IDENTITY.md`** — the verdict this document tests. **§0's conclusion, §4.1 and
  §4.2 are superseded by §2 here; §1.3's step 5/6 ordering is sharpened by §3.2; §3.1's `+8` is
  corroborated by §2.3; §4.1's `TEXT("TemporalAAHistory")` is corrected to `TEXT("TemporalAA")` by
  §2.4.** Its §3.3, §5.1, §5.2 and §6's factual content stand.
* **`docs/RESEARCH-ENGINE-TAA-HOOK.md`** §14.2 — the "no hookable point in the `Execute()` window"
  table. **Refuted by §3.1 here**: the table enumerates RDG internals and misses that the engine
  calls out of the window into `ISceneViewExtension`. §14.1, §14.3, §14.4 and §15's analysis stand;
  §15.2's "two bookkeepers agreeing on a 64-bit value" is the pattern §4.5 reuses.
* **`docs/RESEARCH-RESHADE-SHAPE-SWEEP.md`** §13 — the consumer census. Its ranking is unchanged,
  but the conclusion that the SRV/UAV table walk's job is irreducible no longer holds.
* **CLAUDE.md** §2.3 (the seam), §2.9 (`u0` is both scene colour and next-frame history), §5 (the
  descriptor hazards the shadow exists to survive), §0 prime directive 2 (a wrong answer must be
  refused, never used — which is why every route above ships as an oracle first).
