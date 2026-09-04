# The engine-aware replan: everything we reconstruct that the engine would tell us, re-ranked by what it deletes

**The user's brief:** *"Assess what other things could be improved with similar techniques — look at
what we do and what we need to do, and anything we could simplify as well."*

**The lens, stated once.** `docs/RESEARCH-U0-EXTERNAL-PRIOR-ART.md` found that this project has been
reasoning like a graphics-API interposer — ReShade, Special K, OptiScaler — when it is an
**engine-aware injector** with UE4SS's scanner, UObject reflection and the engine's own objects in
hand, in the same process. The one project that shares our vantage point, `praydog/UEVR`, does no
descriptor tracking at all. So the question asked of every part of the plugin below is the same one:
**does this exist only because we could not ask the engine, and can the engine now be asked?**

**What this document is.** A re-rank of `docs/RESEARCH-RESHADE-SHAPE-SWEEP.md` — our own inventory
of the same question, written before UEVR was known — with the currency changed from "measured
cost" to **machinery removed**, because the sweep's own correction (§0 there) records that a
measured cost attributed to the wrong consumer shaped decisions for a day. Line counts are `wc -l`
on this tree at `a9c35e4` unless marked "~" (estimated from cited regions). Nothing here was built or
run; the `u0` agent has the box.

**Sources.** Epic `EpicGames/UnrealEngine` @ `3abfe77d0b24a6d8bacebd27766912e5a5fa6f02` (4.27),
read directly. `praydog/UEVR` @ `master` and @ `0587a468` (pre-submodule). DeepWiki over
`UE4SS-RE/RE-UE4SS` for the console-manager claim, labelled SOFT.

---

## 0. The re-ranked table

Ranked by **what it deletes**. "Route" is the engine mechanism; "validator" is how a wrong answer is
refused, which every row must have or it does not ship.

| # | What we reconstruct today | Lines held | Route the engine offers | What it deletes | Validator | Verdict |
|---|---|---|---|---|---|---|
| **1** | **Which resource is bound at each compute register** — the descriptor shadow, fed by 5 `Create*View` + 2 `CopyDescriptors` + heap hooks, walked per dispatch | `descriptor_shadow.{cpp,hpp}` **827**, `heap_math` **59**, the table walk `native_backend.cpp:174-308` **~135**, 8 of the device hooks in `d3d12_hooks.cpp` (**~350** of 1161), `va_map` **81**; **1.694 ms/frame** | **The engine's own bind stream**: `IRHIComputeContext::RHISetShaderTexture` / `RHISetShaderResourceViewParameter` / `RHISetUAVParameter` / `RHISetShaderUniformBuffer`, each carrying the **register index** and the RHI view object (`RHIContext.h:184-215`), issued in a fixed order per pass (`ShaderParameterStruct.h:198-283`) and bracketed by `RHISetComputeShader` / `RHIDispatchComputeShader` (`RenderGraphUtils.h:246-253`) | **All of the above.** Every SRV `t0-t5` and UAV `u0` of §2.3's register map, by the engine's own hand, no copy tracking | The `FRHIUnorderedAccessView*`/`FRHIShaderResourceView*` resolves to a resource in **our registry** via the `CreateUnorderedAccessView`/`CreateShaderResourceView` descriptor-handle cross-match (prior-art §4.5); agreement with the shadow while both run; the `UnsetShaderUAVs` nulls ignored | **Build. This is the dominant simplification** (§1) |
| **2** | **Which bound CB is `View`** — search every root CBV, keep the first plausible; measured taking a stale ring copy on 0.33% of claimed dispatches (facts §36.20) | `taa_hook.cpp` `read_view_cb` + search + `view_fits_dispatch` + the `[view]` counters **~200**; 1 of 9 root hooks; `registry::buffer_for_va`; the never-built two-offset design of TAA-hook §15 | **`FViewInfo::CachedViewUniformShaderParameters`** (`SceneRendering.h:1047`) — a **CPU copy of the exact struct** TAA's `View.ViewUniformBuffer` is created from (`SceneRendering.cpp:1859-1869`, bound at `TemporalAA.cpp:767`), reachable from the `const FViewInfo&` `AddPasses` already hands us | The search, the CB mapping, the ring-copy hazard, the ambiguity instrument, one root hook, `va_map` | The pointee must pass `view_params_plausible` **and** row 135 **and** its rows 129/130 must equal the extents L1 already resolves — then be **byte-identical** to `read_view_cb`'s 2448-byte prefix while both run | **BUILT AND MEASURED 2026-09-04** — `[STRAYDLSS] EngineSeamViewParams`, `src/core/view_cached.hpp`, `src/view_params_hook.cpp`. Level 1 ran on the box: **`FViewInfo+5768`**, one survivor, byte-IDENTICAL to the bound buffer, latched, `faults=0`, and `disagree=4` == the search's own `ambClaimed=4` **event for event** (facts §36.22). **Level 2 is now the DEFAULT**; level 3 (delete the search) declared, not built. Level 2's substitution is **UNCONFIRMED live and needs GAMEPLAY** (§2.4, TAA-hook report §19) |
| **3** | **`u0`** — the SRV/UAV table walk's sole irreducible job | (counted in #1) | Three routes: the RHI bind stream (#1), the pool hook by name `L"TemporalAA"`, a fake `ISceneViewExtension` reading `ResourceRHI` inside `Execute()` | The reason #1 could not be deleted | Per route, in `docs/RESEARCH-U0-EXTERNAL-PRIOR-ART.md` | **In progress** on the box |
| **4** | **The structural matcher's register roles, depth+stencil signature, rect arithmetic, cooked-hash table, pin, round-trip, shape gates** — bypassed under `EngineSeam=3`, still executed | sweep §11 steps 2+3: **~550** (`taa_hook.cpp` ~230, `taa_signature.cpp` ~160, `taa_hashes.hpp` 51, tests ~60) | Already the seam; #1 removes the last reason to run the matcher at all (register roles) | ~550 lines and the `MatchResult` dependency | The seam's own `unclaimed=0`/`orphans=0` session, already the licence condition | **Delete, after #1** — the sweep already said so; #1 removes its last excuse (§4) |
| **5** | **RR's G-buffer guides** — the heuristic finder was deleted (~1 900 lines) and RR now refuses; nothing replaces it | 0 today; **~1 900 prevented** from regrowing | **The render-target pool's name argument.** In 4.27 `GBufferA-E`, `SceneDepthZ`, `GBufferVelocity`, `SceneColorDeferred` are allocated through the *outer* `FRenderTargetPool::FindFreeElement` with literal names (`SceneRenderTargets.cpp:1005-1741`) — **the exact function UEVR hooks**, with `Out` as an explicit parameter and no ABI trap | Any future finder. Replaces 1 900 lines with ~150 | Name equality + `FPooledRenderTargetDesc` format/extent + the `+8` `FRHITexture` → registry; and `FSceneRenderTargets`'s public members (`SceneRenderTargets.h:459-476`) as an independent oracle | **Build when RR is wired; do not rebuild a finder** (§5) |
| **6** | **Camera-cut OR, jitter, `ClipToPrevClip`, `PreExposure`** — row reads from the searched CB | `core/view_params`, `ue4_view` (~400, all kept) | Same rows, from #2's CPU struct; the cut's second term from `View.PrevViewInfo.TemporalAAHistory.IsValid()` | Nothing — **the row reads are the safer thing** and stay; only their *source* changes | Row 135 as today | **Keep the reads; retarget the source** (§6) |
| **7** | **"Are we in gameplay"** | `dlss_app.cpp:1253-1268` **~16** + the hash census | UE4SS reflection from C++ (`UObjectGlobals::FindFirstOf`, the probe's exact route) | ~16 + the census set | The probe file already agrees | **Delete** (sweep §7, unchanged) |
| **8** | **Engine cvars as HARD comments** (`r.RayTracing`, `r.SceneColorFormat`, …) | comments + a planned ~30-line assertion | **UE4SS locates `ConsoleManagerSingleton` by AOB** — SOFT (DeepWiki over the repo) — so `FindConsoleVariable` is callable from a C++ mod | The planned assertion; turns four premises into reads | The returned `IConsoleVariable*` must carry the literal name back | **Fetch, do not assert** — overturns sweep §9 (§8) |
| **9** | **The seam's own locator** — literal + `lea/ret` + vtable qword + two decoded constants | `core/engine_seam` **1 198**, live half **1 143** | UEVR's caller-literal → xref → forward-decode-to-CALL | Nothing — ours is **more** self-validating for an interface whose slots return constants | (already three constants) | **Keep; add UEVR's method as a second locator KIND** for functions without constant-returning neighbours (§9) |
| **10** | **FG's `GameIndexMirror`, present-twice, replacement back buffers; NR's guide contract** | `fg_plan` 628, `fg_present` 1 608, `present_owner` 782 | `FRHIViewport::GetNativeBackBufferTexture()` (`RHIResources.h:1472`), `FRHICustomPresent` (`:1493`); `ISceneViewExtension::PostRenderViewFamily_RenderThread` as a pre-UI seam | **Nothing worth its risk** — the mirror is exact by construction and measured at 2.00x | — | **Keep. Record the engine's present seam for the HUD-less stage** (§10) |
| **11** | The state restore (`restore_game_compute_state`) and the root shadow that feeds it | `root_shadow` **199**, 9 list hooks (**~400** of `d3d12_hooks.cpp`), `d3d12_restore` ; **0.681 ms** | None — D3D12 has no state getters, and NVIDIA solves it *inside* `FD3D12StateCache` (prior-art §6) | Nothing | — | **Keep.** Stays even after #1 (§3) |

**Two cross-cutting adoptions from UEVR that touch every row** (§11): install hooks *disabled* and
enable only after an observe-only validation window; validate every discovered pointer with "does its
vtable live in the game module" before use; marshal RHI-thread work through an explicit worker.

---

## 1. Item 1 — the descriptor shadow is a copy of a shadow the engine already keeps

### 1.1 The observation

`FD3D12StateCache` is the engine's own per-context record of every bound SRV, UAV, CBV and sampler by
register. Its **write side** is the `IRHIComputeContext` virtuals: every compute pass in 4.27 binds
through exactly four of them, in a fixed order, from one function:

```cpp
// ShaderParameterStruct.h:198-283 @ 4.27/3abfe77, SetShaderParameters()
RHICmdList.SetShaderParameter(...)                         // loose params, rare for RDG passes
RHICmdList.SetShaderTexture(ShadeRHI, BaseIndex, Tex)      // UBMT_TEXTURE, UBMT_RDG_TEXTURE
RHICmdList.SetShaderSampler(...)
SetShaderUAV(...)  -> RHICmdList.SetUAVParameter(ShadeRHI, BaseIndex, UAV)      // UBMT_*_UAV
// then, deferred ("HACKHACK ... bind SRVs after UAVs"):
RHICmdList.SetShaderResourceViewParameter(ShadeRHI, BaseIndex, SRV)             // UBMT_RDG_*_SRV
RHICmdList.SetShaderUniformBuffer(ShadeRHI, BufferIndex, UB)                     // View at b1/b4
```

each of which is a pure virtual on `IRHIComputeContext` with the **register index as an explicit
argument** (`RHIContext.h:184`, `:192`, `:200`, `:211`, `:213`), overridden by `FD3D12CommandContext`
(`D3D12Commands.cpp:680` for the UAV; the SRV and texture overloads adjacent). The pass is bracketed
by `RHISetComputeShader` (`:134`) before and `RHIDispatchComputeShader` (`:145`) after
(`RenderGraphUtils.h:246-253`). **HARD.**

**So `descriptor_shadow.cpp` reconstructs, from 4 059 descriptor copies a frame, a mapping the
engine hands its RHI context ~15 times per pass with the register number attached.** The whole
`CopyDescriptors` write side, `gpu_to_cpu`, `heap_math`, the flat slot arrays and the 33 M-slot
growth line exist because a ReShade add-on could only see the D3D12 side of that transaction.

### 1.2 What a hook on those virtuals gives, per TAA dispatch

Exactly `docs/STRAY-RENDERING-FACTS.md`'s §2.3 register map, from the engine: `t0` eye-adaptation
texture, `t1` colour, `t2` depth, `t3` velocity, `t4` stencil, `t5` history — **each as the engine's
own RHI view object, at the register the shader declares** — and `u0`. Colour "by register" (which
`RESEARCH-ENGINE-TAA-HOOK.md` §14.4 makes the intended end state) becomes the engine's answer, not a
shadow's. And it fires on the **RHI thread, immediately before the `Dispatch` our D3D12 hook already
intercepts**, on the same thread, so the pairing is "the binds recorded since the last
`RHISetComputeShader` on this context". No cross-thread ledger, no announcement ring for these.

### 1.3 What it deletes

| Component | Lines | Fate |
|---|---|---|
| `src/backend_native/descriptor_shadow.{cpp,hpp}` | 685 + 142 | **deleted** |
| `src/core/heap_math.{cpp,hpp}` | 59 | **deleted** |
| `resolve_compute_bindings`'s table walk, `native_backend.cpp:174-308` | ~135 | **deleted**; `DispatchBindings` is filled from the RHI stream instead |
| `Create{ShaderResource,UnorderedAccess,RenderTarget,DepthStencil,ConstantBuffer}View`, `CopyDescriptors`, `CopyDescriptorsSimple`, `CreateDescriptorHeap` hooks in `d3d12_hooks.cpp` | ~350 of 1161 | **6 deleted**; `CreateShaderResourceView` and `CreateUnorderedAccessView` **kept, repurposed** as the handle→resource cross-match (prior-art §4.5) — creation-time only, not per copy |
| `src/core/va_map.{cpp,hpp}` | 81 | **deleted** with item 2 |
| `shadow-write` + `shadow-copy` | 1.694 ms/frame | gone |
| `NATIVE SHADOW GROWTH` and its three rewrites' worth of lock-free arrays | — | gone |

**~1 450 lines and 58% of the measured native-hook cost**, against **~250 lines** for the vtable
hook, the slot discovery and the cross-match. This is the largest deletion available anywhere in the
plugin and it is why it ranks first.

### 1.4 Cost and failure modes

* **Slot discovery.** `IRHIComputeContext` has ~30 virtuals with several overload groups, and MSVC
  emits overloads in reverse declaration order (prior-art §4.6). UEVR's answer is the model: install
  a **probe vtable** whose every slot is a distinct thunk, observe which slot fires with which
  argument shape on the RHI thread, latch, then install the real hooks (`FFakeStereoRenderingHook.cpp:2873-2893`
  @ UEVR master does this for `FRHICommandBase` with `hooked_command_fn<0..6>`). A slot is accepted
  only when its second argument reads as a small integer and its third resolves through the
  cross-match — a wrong slot cannot pass both.
* **MGPU redirector.** `FD3D12CommandContextRedirector` is a different class; not used on one GPU;
  **count** dispatches that arrive at our D3D12 hook with no preceding `RHISetComputeShader` on that
  thread — a non-zero count is the silent-miss detector.
* **`UnsetShaderUAVs`** fires `SetUAVParameter(..., nullptr)` after every dispatch — ignore nulls.
* **A licensee moving `FTAAStandaloneCS` to a root-parameter struct** bypasses the whole stream —
  same counter as above catches it.
* **Writing the game's vtable page** (`.rdata`, `VirtualProtect`) — the same class of write as
  `ext_unhook`. Restore on detach, as the native backend already does for its own slots (CLAUDE.md
  "UE4SS migration", the address-0 crash lesson).
* **Five of the six TAA inputs need no cross-match at all.** `EyeAdaptationTexture`,
  `InputSceneColor`, `HistoryBuffer[]`, `SceneDepthTexture` and `GBufferVelocityTexture` are all
  `SHADER_PARAMETER_RDG_TEXTURE` (`TemporalAA.cpp:188-203`), so they arrive through
  `RHISetShaderTexture` as a bare **`FRHITexture*`** — whose `GetNativeResource()` slot 7 is already
  HARD on this exe. Only `StencilTexture` is an `_SRV` (`:206`) and only `OutComputeTex` is a UAV,
  and those two share one cross-match design: `FD3D12ShaderResourceView` has `FRHIShaderResourceView`
  as a base and `FD3D12View<D3D12_SHADER_RESOURCE_VIEW_DESC>` behind it (`D3D12View.h:797`), the same
  shape as the UAV (prior-art §4.5).

**What it does NOT delete: the root shadow and the restore.** See §3.

---

## 2. Item 2 — the View CB has a CPU copy sitting in the object we already hold

### 2.1 The fact

```cpp
// SceneRendering.h:1047 @ 4.27/3abfe77   (FViewInfo, right after ViewRect at :1038)
TUniquePtr<FViewUniformShaderParameters> CachedViewUniformShaderParameters;

// SceneRendering.cpp:1853-1869, FViewInfo::InitRHIResources()
CachedViewUniformShaderParameters = MakeUnique<FViewUniformShaderParameters>();
SetupUniformBufferParameters(SceneContext, VolumeBounds, TVC_MAX, *CachedViewUniformShaderParameters);
ViewUniformBuffer = TUniformBufferRef<FViewUniformShaderParameters>::CreateUniformBufferImmediate(
    *CachedViewUniformShaderParameters, UniformBuffer_SingleFrame);

// TemporalAA.cpp:767
PassParameters->ViewUniformBuffer = View.ViewUniformBuffer;
```

**The 2448-byte prefix `read_view_cb` maps out of the D3D12 upload ring is a copy of this struct, made
by the engine from this struct.** It is a heap block owned by the `FViewInfo` the seam hands us, on
the render thread, at `AddPasses`. **HARD.** `SceneRendering.cpp:1838-1843` shows it is also updated
in place later in the frame (`UpdateUniformBufferImmediate(*CachedViewUniformShaderParameters)`),
which is fine — the pointer is stable, and at `AddPasses` it holds the values TAA will bind.

### 2.2 Why the offset can be discovered safely, and why this beats TAA-hook §15

§15's route needed two offsets into RHI-private types, one gated on an unobservable build define,
and its strongest validator was a 64-bit equality between two bookkeepers. **This route needs one
offset into `FViewInfo`, and the pointee is the most self-validating object this project owns**:

1. it is a readable heap block of ≥ 2448 bytes (`VirtualQuery` + SEH, L1's guards);
2. `view_params_plausible` passes — reciprocals at row 130, sub-pixel jitter, sane sequence length;
3. **row 135 passes**: `y*z == 1.0`, `x` denormal, `w == 0` (CLAUDE.md §2.6, three data types);
4. **rows 129/130 equal the extents L1 already resolves** for depth and velocity on the same
   announcement — which is the thing the *search* could never check, because the search had no
   announcement to compare against;
5. while both mechanisms run, the 2448-byte prefix is **byte-identical** to what `read_view_cb`
   produced for the claimed dispatch.

A scan of `FViewInfo`'s first ~4 KB for a pointer satisfying 1-4, latched after N agreeing
announcements and re-validated per frame, cannot latch on the wrong field: nothing else in
`FViewInfo` points at a block that passes row 135. And `FSceneView::ViewMatrices`, `bCameraCut`
(`SceneView.h:936`, `:1048`) sit *inline* in the same object — but they are not needed, because the
cached struct carries `ClipToPrevClip`, `TemporalAAJitter`, `CameraCut`, `PreExposure` at the rows
we already parse.

### 2.3 What it deletes, and what it fixes

* `read_view_cb` (`taa_hook.cpp:384-~440`), the search (`:783-~850`), `view_fits_dispatch` and its
  from-above-only limitation, the `[view]` counters (`wrongView`, `suspectSmall`, `ambClaimed`),
  the mapping of an upload heap from a foreign thread (TAA-hook §15.3's SOFT item) — **~200 lines**.
* The root-CBV hook (1 of 9), `registry::buffer_for_va`, `core/va_map` (81) — the root shadow keeps
  its CBV *replay* for the restore but no longer needs the VA→buffer map.
* **The 0.33% stale-ring residue** (facts §36.20) is not a search bug to be tuned; it is a
  consequence of reading a ring slot a frame after it was written. The CPU struct is read at
  announce, on the thread that owns it, and carried in the announcement as a decoded `ViewParams` —
  exactly the shape §15.3 already recommended.
* **The never-built §15.2 design** (two RHI-private offsets) is not needed. Retire it.

**Failure mode.** The block is `TUniquePtr`-owned by the `FViewInfo`; `FViewInfo` lives in the scene
renderer, which is destroyed after the frame. Reading at `AddPasses` is inside its lifetime (§12.9's
rule); reading later is not. Carry values, never the pointer.

### 2.4 Status: BUILT 2026-09-04; level 1 MEASURED, level 2 shipped as the default

`src/core/view_cached.{hpp,cpp}` (pure: scan, six predictions, byte diff, latch, decision),
`src/view_params_hook.{hpp,cpp}` (live: guarded readers, the announce-time scan, the claim-time
comparison and substitution), `[STRAYDLSS] EngineSeamViewParams` (default 2 since 2026-09-04;
it shipped at 1 and the level-1 launch below is what moved it). Two refinements
against the design above, both from reading the source rather than assuming it:

* **Prediction 4 uses row 132 against L1's depth extent, not rows 129/130.** `ViewSizeAndInvSize`
  is the VIEW rect, which is smaller than the depth texture whenever the scene buffer is left
  oversized (§2.3 of CLAUDE.md measured 2560x1440 depth under a 1280x720 rect); what equals the
  depth extent is `BufferSizeAndInvSize` = `SceneContext.GetBufferSizeXY()` (HARD,
  `SetupUniformBufferParameters`). Rows 129/130 are tested against the announcement's own output
  rect instead, from above and below, with the two predicates the search already uses.
* **Byte-equality is judged at CLAIM, not at announce**, because the search's bytes only exist
  there; the announcement carries the decoded `ViewParams` and the raw prefix. That is also what
  makes the latch honest about the 0.33%: a disagreement BEFORE the latch resets the run (a wrong
  offset never latches), and one AFTER it is counted as the search's error, which is the only
  reading left once the offset has matched the bound buffer eight announcements running.

Level 1 discovers, compares and reports and changes nothing; level 2 makes the struct the source
once latched and gates `ambClaimed` off for those dispatches; level 3 deletes the search and is
declared, not built. Full design, the launch checklist and the provenance ledger:
`docs/RESEARCH-ENGINE-TAA-HOOK.md` §19.

**MEASURED THE SAME DAY, level 1, main menu, one launch (facts §36.22).** Every prediction the
design made held:

* **`FViewInfo+5768`**, with **`survivors=1` at every stage** — and the scan does not stop at the
  first hit, so that is a statement about everything it judged (with the caveat in §19.6: the
  probe budget truncated the last 3% of the window, and has been raised to cover it).
* **Prediction 6 held on the first comparison and then eight announcements running**,
  `preDisagree=0`. Two routes, two threads, one exact 2448-byte agreement.
* **`disagree=4` matched the search's own `ambClaimed=4` event for event**, every WARN naming
  jitter / `PreExposure` / `ClipToPrevClip` at row 0. So facts §36.20's residue really is a stale
  ring copy of the same view, confirmed rather than inferred — which is the claim this whole row
  rests on.
* `faults=0 off=0 ambiguous=0 empty=0 uncompared=0 unverified=0`. Every guard silent.

**Level 2 is therefore the default from 2026-09-04**, and it is safe by construction rather than
by optimism: substitution requires the latch, the latch requires eight byte-exact agreements with
a buffer the engine bound, and where no latch forms the search supplies the View exactly as at
level 0. **What is still UNCONFIRMED is the substitution itself, and it must be judged in
GAMEPLAY** — the menu offers no shadow, capture or planar-reflection view, and §36.21 records a
sibling counter going 0 → 171 across that boundary. The criterion is `ambClaimed=0` on the
`[view]` line with the stale events appearing under `disagree=` on `[viewParams]`.

---

## 3. Item 11 — what stays: the restore and the root shadow

Stated so the deletion in §1 is not over-read.

`restore_game_compute_state` (`native_backend.cpp:370-389`) replays root signature, opaque table
handles, root CBV/SRV/UAV addresses, 32-bit constants, PSO and heaps from `root::snapshot`. D3D12 has
no getters for any of it; NVIDIA's own plugin solves the same problem by calling
`StateCache.ForceSetComputeRootSignature()` *inside* the engine (prior-art §6) — an RHI-private
method behind an RHI-private object, not a route for us. **The root shadow (199 lines + ~400 of hooks,
0.681 ms) stays**, and it is the *only* thing on the shipping path that remains ReShade-shaped after
items 1-4. That is the honest floor: ~0.7 ms and ~600 lines, not 2.9 ms and ~2 000.

One thing the engine *does* offer here, recorded and refused: the RHI stream of §1 also carries
`RHISetComputeShader` and `RHISetShaderUniformBuffer`, so our hook *sees* the shader and buffers to
restore — but replaying through the RHI context would go through `FD3D12StateCache`'s dirty tracking
and the D3D12 list would not be touched until the engine's next flush, which is after our NGX
clobber. The restore must be at the D3D12 level. Keep it.

---

## 4. Item 4 — the matcher loses its last job

Sweep §11 steps 2 and 3 already schedule ~550 lines for deletion "after item 2 lands" and "after one
clean level-3 session". The one reason the structural matcher still *runs* — not gates, runs — is
that its `MatchResult` is how register roles are extracted for the shadow walk (sweep §6.2). **Item 1
supplies register roles from the engine**, so after it the matcher has no consumer at all. Delete
in the same change. The seam's `unclaimed=0` / `orphans=0` condition is unchanged and already met
(`RESEARCH-ENGINE-TAA-HOOK.md` §10).

---

## 5. Item 5 — RR's guides have names, and the hook that reads them is UEVR's verbatim

The G-buffer finder is gone (~1 900 lines, `RESEARCH-ENGINE-TAA-HOOK.md` §13) and RR refuses. Sweep
§8.2 concluded there was "no cheap route" because `FSceneTextureParameters` is a value struct from a
free function. It missed that **every one of those textures is allocated through the pool, with a
literal name, by the outer `FindFreeElement`** — the function whose locator and hook are quoted in
prior-art §2:

| Name (4.27) | Call site | RDG? |
|---|---|---|
| `TEXT("GBufferA")` … `TEXT("GBufferE")`, `TEXT("SceneDepthAux")` | `SceneRenderTargets.cpp:1125-1161` | no — outer `FindFreeElement` |
| `TEXT("GBufferVelocity")` | `:1172`, `:1722` | no |
| `TEXT("SceneDepthZ")` | `:1543` | no — **UEVR's anchor** |
| `GetSceneColorTargetName()` → `TEXT("SceneColorDeferred")` | `:1005`, `:320` | no |
| `TEXT("GBufferF")` | `:1741` | no |

**HARD.** That is the entire RR guide set, plus depth and velocity, from one hook UEVR ships to
hundreds of titles, with `Out` as an explicit `TRefCountPtr<IPooledRenderTarget>&` (no hidden-return
ABI), and — because `FindFreeElement` early-returns "keep the current one" through the same entry
(`RenderTargetPool.cpp:660-672`) — it fires **every frame** the target is re-requested, so the map
is never stale.

**Validator, three-fold:** name equality; the `FPooledRenderTargetDesc` format matches what the RR
path expects (`RESEARCH-RR-GBUFFER.md`'s table); the `+8` `FRHITexture` → `GetNativeResource()`
lands in our registry. And an **independent oracle**: `FSceneRenderTargets` is a singleton
(`SceneRenderTargets.h:157`, `:160` `GetGlobalUnsafe`) whose `SceneDepthZ`, `SceneVelocity`,
`GBufferA…` are **public** `TRefCountPtr<IPooledRenderTarget>` members (`:459-476`) — findable by the
same pointer-topology scan, and they must equal what the hook recorded.

**What it costs:** one inline hook (UEVR's `safetyhook::create_inline`), a `wstring → IPooledRenderTarget*`
map under a mutex, ~150 lines. **What it prevents:** rebuilding a 1 900-line format-and-slot-order
classifier with an unresolvable B/C ambiguity. **Failure mode:** several views per frame allocate the
same names — key the map by `(name, FSceneRenderTargets instance)` or accept only the primary view's
(the one whose desc extent matches the announcement).

**Not for depth and velocity on the SR path.** L1 already has those from `FPassInputs` with the same
`+16`/slot-7 hops and a registry check; the pool hook is an oracle for L1, not a replacement.

---

## 6. Item 6 — the rows stay; the source moves

Camera cut, jitter, `ClipToPrevClip`, `PreExposure` are read by row offset from a buffer we search
for. The brief asks whether a route avoiding a derived `FViewInfo` offset exists, or whether the
row read is the safer thing. **The row read is the safer thing, and item 2 keeps it while removing
the search.** The rows are validated by row 135 from a single read; `FSceneView`'s inline fields
(`ViewMatrices` at `SceneView.h:936`, `bCameraCut` at `:1048`) would each be one more unvalidated
offset for information the struct already carries at a validated one.

The camera-cut OR's second term (`!InputHistory.IsValid()`, sweep §5) is
`View.PrevViewInfo.TemporalAAHistory.RT[0] == nullptr` (`SceneRendering.h:1275`, `:929`, `:814`) —
the same field prior-art/`RESEARCH-U0-IDENTITY.md` §5.2 proposes scanning for as the one-frame-late
`u0` oracle. One scan, two consumers.

---

## 7. Item 7 — gameplay gate

Unchanged from sweep §7: the Lua probe's route (`FindFirstOf("BP_CatPawn_C")`, `IsGamePaused`) is
available to the C++ plugin through `Unreal::UObjectGlobals` (UE4SS, **HARD** that the API exists —
it is what `mods/StrayProbe` calls). Delete the census.

---

## 8. Item 8 — cvars: fetch, do not assert

Sweep §9 kept four engine cvars as HARD-labelled comments because `IConsoleManager::Get()` is
`FORCEINLINE` over an unexported singleton, adding that "UE4SS offers no help". Two corrections:

* **Writing** cvars from a mod is already HARD — `UObject::ProcessConsoleExec` and
  `UKismetSystemLibrary::ExecuteConsoleCommand` are reachable (`docs/RESEARCH-UE4SS-MIGRATION.md:587`).
  A premise can therefore be *set* rather than asserted, which is what an A/B wants anyway.
* **Reading** them: UE4SS's own scanner locates `ConsoleManagerSingleton` by AOB (patternsleuth's
  optional scan targets; overridable via `UE4SS_Signatures/ConsoleManager.lua`) — **SOFT**, from
  DeepWiki over `UE4SS-RE/RE-UE4SS`, not read in source here — and whether that address is exposed
  to C++ mods is **UNCONFIRMED**. If it is, `FindConsoleVariable(TEXT("r.RayTracing"))->GetInt()` is
  one call from `on_unreal_init`, validated by the returned variable handing the literal name back.
  If it is not, the same caller-literal technique as §9 finds the `FAutoConsoleVariableRef`
  registration site for any `r.*` name — the literal is right there — and the storage it binds.

Either way the four premises become four startup log lines rather than comments. A ten-minute
read of UE4SS's `patternsleuth_bind` settles which.

---

## 9. Item 9 — the seam's locator is already better than UEVR's, for this interface

Ours: find the wide literal, find `lea rax,[rip+d]; ret` targeting it, find the vtable qword, decode
two neighbouring slots for `0.5` and `2.0` (`RESEARCH-ENGINE-TAA-HOOK.md` §4.1). UEVR's: find a
caller's literal, xref, decode forward to the CALL (`shared/sdk/FRenderTargetPool.cpp` @ `0587a46`).

**Ours is stronger where it applies** — three exact constants of three kinds, statically, nothing
executed — and UEVR's is the tool for the case ours cannot handle: a non-virtual function whose
neighbours return nothing checkable. Adopt it as a **second locator kind** in `core/engine_seam`
(pure, testable against a synthetic module like the existing scan), used by items 1 (the context
object, via `FRDGBuilder::RHICmdList` at offset 0 → `FRHICommandListBase::ComputeContext`,
`RenderGraphBuilder.h:205`, `RHICommandList.h:579`) and 5. Robustness across patches is the same for
both: both rediscover per binary and refuse on ambiguity; neither hardcodes.

---

## 10. Item 10 — FG and NR: the engine has a present seam, and we should know it exists

`FRHIViewport` (`RHIResources.h:1456-1499`) is the engine's own answer to two things we do by hand:

* `virtual void* GetNativeBackBufferTexture() const` (`:1472`) — the **current** back buffer's
  `ID3D12Resource*`, from `FD3D12Viewport::BackBuffer_RHIThread`, advanced at
  `D3D12Viewport.cpp:872-875` exactly as `GameIndexMirror` mirrors it.
* `virtual void SetCustomPresent(FRHICustomPresent*)` (`:1493`) — the interface VR runtimes use to
  **take over presentation**: `FD3D12Viewport::Present` consults `CustomPresent->NeedsNativePresent()`
  (`D3D12Viewport.cpp:589-590`, `:644`).

**Recorded, not recommended.** `GameIndexMirror` is 14 lines and exact by construction (facts
§32.4); reaching an `FRHIViewport` needs `GEngine → GameViewport → FViewport → ViewportRHI`, two of
which are not `UPROPERTY`s, and `FRHICustomPresent` would change *who presents*, which is the one
thing the present-twice design has proven stable across `SetFullscreenState` and three reloads.
The validator would be strong — `GetNativeSwapChain()` (`:1465`) must return the swapchain we
already hold, a 64-bit equality — so if the mirror ever misbehaves, this is the replacement.

**The pre-UI seam, which CLAUDE.md names as the follow-up FG and NR both want**, is engine-level and
cheap once a fake `ISceneViewExtension` exists (prior-art §3): `PostRenderViewFamily_RenderThread`
runs as the *last* RDG pass of the scene renderer (`SceneRendering.cpp:3181-3200`), after the
tonemapper has written the view family target and before Slate draws the HUD — **[derived]** on the
Slate ordering, HARD on the pass ordering. A copy of the back buffer there is the HUD-less frame,
with no "first back-buffer RTV bind" heuristic. And `SubscribeToPostProcessingPass` (`SceneViewExtension.h:161`)
lets the extension be handed the post-tonemap `FScreenPassTexture` at setup, whose `ResourceRHI` is
then readable inside `Execute()` by the same trick as `u0`.

NR's guide contract (`nr_hook.cpp:142`) is a cross-thread handoff of values the TAA site holds; the
engine has nothing to add.

---

## 11. What UEVR does by habit that we do by hand — adopt these regardless of the rows

From `praydog/UEVR` (`FFakeStereoRenderingHook.cpp`, `RenderTargetPoolHook.cpp`, `shared/sdk/*`,
`shared/utility/*` @ master / `0587a46`) and praydog's write-up
(<https://praydog.com/reverse-engineering/2023/07/03/uevr.html>):

1. **Install disabled, enable after proof.** Five of its inline hooks are created with
   `safetyhook::InlineHook::StartDisabled` and enabled only once a later check passes. Our
   `EngineSeam` ladder (0/1/2/3) is the same idea for one hook; make it the *only* way any hook goes
   live.
2. **"Does this pointer belong to a loaded module?" as the universal first validator.**
   `utility::get_module_within(ptr)` is called 31 times in one file — on every vtable, every
   function pointer, every candidate before it is trusted. L1's `plausible_heap_ptr` is the
   heap-side half; this is the code-side half, and item 1's slot discovery needs it.
3. **Discover slots by observing, not counting.** `hooked_command_fn<0..6>` thunks, then
   `correct_execute_index` chosen by which fires on the DXGI present thread. Every vtable in items
   1, 3 and 10 should be found this way; MSVC's overload ordering makes counting a trap.
4. **Anchor on callers' literals; forward-decode to the CALL; never plain AOB.** The write-up:
   *"tries to minimize use of plain AOBs … only uses them … in a localized scenario (such as within
   the bounds of a function or near an anchor point)."* Our `is_known_taa_hash` table was the AOB
   habit applied to shaders; the seam retired it. Apply the same rule to code.
5. **Emulate before executing.** `utility::ShemuContext` (bdshemu) runs a candidate function's
   prologue to classify it (`is_stub_code`, string-in-path) without calling it. TAA-hook §9 already
   says "do not call a discovered function to validate it — decode it"; emulation is the stronger
   form and is a library away.
6. **An explicit RHI-thread worker.** `RHIThreadWorker::get().enqueue(...)` / `.execute()`
   (`shared/sdk/threading/ThreadWorker.hpp`): work destined for the RHI thread is queued and drained
   from a hook that provably runs there. Our announcement ring is a specialised version for one
   producer/consumer pair; item 1's binds and the pool hook's records will want the general one.
7. **Log every seen name once** (`m_seen_names` + `SPDLOG_INFO_ONCE`). The pool hook's first
   session should produce the complete name census of this title for free; that census is the
   RR guide table, measured rather than assumed.
8. **Per-game persisted discovery.** UEVR caches nothing across versions and rediscovers per binary,
   but persists *per-game* config. We already re-scan at every launch; keep it that way — a game
   patch must cost a rescan, never a stale offset.

---

## 12. What was checked and found already right

* **The seam locator** (`core/engine_seam`) — three constants, nothing executed. Better than UEVR's
  method for a constant-returning vtable. Keep (§9).
* **L1's `ResourceRHI@16` + `GetNativeResource` slot 7** for depth and velocity — this *is* the
  engine's accessor inlined away (`RESEARCH-ENGINE-TAA-HOOK.md` §14.1); the pool hook is an oracle
  for it, not a replacement (§5).
* **The row-offset reads of the View struct** — row 135 self-validation is the strongest check in
  the project; item 2 changes where the bytes come from, not how they are read (§6).
* **The camera-cut OR's structure** — a faithful two-term reconstruction of `TemporalAA.cpp:644`
  (sweep §5); only its second term's *source* improves.
* **The restore and the root shadow** — D3D12-level by necessity; NVIDIA solves it inside the RHI
  and we cannot (§3).
* **`GameIndexMirror`, present-twice, the present owner** — exact, measured, and the engine's
  alternative would change who presents (§10).
* **The velocity decode, the dense-MV resolve, `srv_format_for`, NVIDIA's synthetic near/far, the
  COM vtable slot table** — sweep §10's list, unchanged.
* **`ext_unhook`** — a translation-layer coexistence repair, out of scope for the Windows-portable
  rule and correctly isolated behind its own key.
* **The sweep's §13.4 refusal of evaluating SR at Present** — still right (submission order).
* **The sweep's §13.5 refusal of lazy dst→src descriptor resolution** — still right, and now moot.

---

## 13. Provenance ledger

| Claim | Status |
|---|---|
| `SetShaderParameters` binds every RDG compute pass's textures/SRVs/UAVs/uniform buffers through the four `IRHIComputeContext` virtuals, with register indices, bracketed by `RHISetComputeShader`/`RHIDispatchComputeShader` | **HARD** — `ShaderParameterStruct.h:198-283`, `RHIContext.h:134-215`, `RenderGraphUtils.h:246-253` @ 4.27/3abfe77 |
| `RHISetShaderTexture` receives an `FRHITexture*`; eye-adaptation, colour, history, depth and velocity are `SHADER_PARAMETER_RDG_TEXTURE` and arrive through it; only stencil is an `_SRV` | **HARD** — `ShaderParameterStruct.h:227-233`, `TemporalAA.cpp:188-206` |
| UE4SS can *write* cvars from a mod (`ProcessConsoleExec`) | **HARD** — `docs/RESEARCH-UE4SS-MIGRATION.md:587` |
| `FD3D12ShaderResourceView` has `FRHIShaderResourceView` as a base and `FD3D12View` behind it, like the UAV | **HARD** — `D3D12View.h:797` |
| `FViewInfo::CachedViewUniformShaderParameters` is the struct `ViewUniformBuffer` is created from, and TAA binds `View.ViewUniformBuffer` | **HARD** — `SceneRendering.h:1047`, `SceneRendering.cpp:1853-1869`, `TemporalAA.cpp:767` |
| It is also updated in place later in the frame | **HARD** — `SceneRendering.cpp:1838-1843`; the function name at that site was not read |
| `FSceneView::ViewMatrices` and `bCameraCut` are inline fields | **HARD** — `SceneView.h:936`, `:1048` |
| GBufferA-F, SceneDepthAux, GBufferVelocity, SceneDepthZ, SceneColorDeferred are allocated via the outer `FindFreeElement` with literal names in 4.27 | **HARD** — `SceneRenderTargets.cpp:1005`, `:319-320`, `:1125-1172`, `:1543`, `:1722`, `:1741` |
| `FindFreeElement`'s keep-current path goes through the same entry, so the hook fires per request | **HARD** — `RenderTargetPool.cpp:660-672` |
| `FSceneRenderTargets` is a singleton with public `IPooledRenderTarget` members | **HARD** — `SceneRenderTargets.h:157`, `:160`, `:459-476` |
| `FRHIViewport` has `GetNativeSwapChain`, `GetNativeBackBufferTexture`, `SetCustomPresent` virtuals; `FD3D12Viewport::Present` consults `CustomPresent` | **HARD** — `RHIResources.h:1465`, `:1472`, `:1493`; `D3D12Viewport.cpp:589-590`, `:644` |
| `CurrentBackBufferIndex_RHIThread` advances `(i+1) % NumBackBuffers` after present, reset to 0 on resize | **HARD** — `D3D12Viewport.cpp:872-875`, `WindowsD3D12Viewport.cpp:387` |
| `PostRenderViewFamily_RenderThread` is the last RDG pass of the scene renderer | **HARD** on ordering within `RenderFinish` (`SceneRendering.cpp:3181-3200`); **[derived]** that Slate's HUD draw follows the scene renderer's `Execute()` |
| `SubscribeToPostProcessingPass` exists on `ISceneViewExtension` in 4.27 | **HARD** — `SceneViewExtension.h:161` |
| `FRDGBuilder::RHICmdList` is the first data member; `FRHICommandListBase::ComputeContext` exists | **HARD** that they exist and their declaration order (`RenderGraphBuilder.h:205-207`, `RHICommandList.h:578-579`); **[derived]** for any byte offset |
| UEVR installs hooks `StartDisabled`, validates pointers with `get_module_within`, brute-forces slots with templated thunks, emulates with `ShemuContext`, marshals RHI work through `RHIThreadWorker` | **HARD** — `FFakeStereoRenderingHook.cpp` (5× `StartDisabled`, 31× `get_module_within`, 4× `ShemuContext`, `:2873-2893`), `shared/sdk/threading/ThreadWorker.hpp` @ UEVR |
| UE4SS locates `ConsoleManagerSingleton` by AOB | **SOFT** — DeepWiki over `UE4SS-RE/RE-UE4SS` citing `patternsleuth_bind` and `UE4SS_Signatures/ConsoleManager.lua`; not read in source here. That it is exposed to C++ mods is **UNCONFIRMED** |
| Line counts | **HARD** — `wc -l` at `a9c35e4`; regions marked "~" are estimates from cited line ranges |
| The 1.694 ms / 0.681 ms split | **HARD** — the live `[perf]` line quoted in sweep §1.2 |
| Any of items 1, 2, 5, 8 working on Stray's executable | **UNCONFIRMED.** Nothing built, nothing run |

---

## 14. Cross-references

* `docs/RESEARCH-RESHADE-SHAPE-SWEEP.md` — the inventory this re-ranks. Its §0 table now carries a
  pointer here; its §1.3/§13 "the shadow stays" conclusion, §3.2 "no cheap engine route to the View
  CB", §8.2 "no cheap route" for the G-buffer, and §9 "assert, do not fetch" are **superseded** by
  §1, §2, §5 and §8 here respectively. Its §2, §5, §6, §7, §10, §13.4, §13.5 stand.
* `docs/RESEARCH-U0-EXTERNAL-PRIOR-ART.md` — the three `u0` routes and the UEVR techniques this
  document applies more widely.
* `docs/RESEARCH-ENGINE-TAA-HOOK.md` §15 — the View-CB identity design that §2 here retires; §14.4
  (colour by register as the end state, now the engine's own); §9 (the rules every row above obeys).
* `docs/RESEARCH-U0-IDENTITY.md` §5.2 — the `PrevViewInfo` scan that §6 here shares.
