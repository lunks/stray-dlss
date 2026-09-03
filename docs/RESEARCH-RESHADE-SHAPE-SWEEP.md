# The ReShade shape, swept: what this plugin still reconstructs that the engine already knows

**The user's question, verbatim:** *"so this is another case where we were using the reshade shape,
anything else left we have to fix similar to this? do a sweep."*

**The occasion.** Two ReShade-era inferences have just been replaced by asking UE 4.27 directly, and
each replacement removed a real defect:

1. **TAA pass identity** — a DXBC hash whitelist, a depth+stencil-SRV-over-one-resource signature,
   dispatch-rect arithmetic, an aspect-ratio band and a permutation pin. Replaced by
   `ITemporalUpscaler::AddPasses`, found by static vtable scan. **Done and live on hardware**
   (`EngineSeam=3` is the default; facts §36, `docs/RESEARCH-ENGINE-TAA-HOOK.md` §10): `orphans=0`
   over 8570 announcements, and the structural signature was measured accepting **two** dispatches
   per frame that the engine never announced.
2. **TAA input identity and liveness** — register-role extraction from bound descriptors plus a
   resource-liveness set. Being replaced right now on branch `engine-seam-l1` by the `FPassInputs`
   textures the engine hands `AddPasses`. **Treated here as done**; every item below says
   explicitly whether that work subsumes it.

This document sweeps everything else. It is a survey, not a plan of record: nothing here is built,
and several items conclude "keep as-is" on purpose.

**Scope.** Every source file under `src/` and `mods/StrayDLSS/`, read at `d0a0a5e`.

**Provenance rules (CLAUDE.md §0.5).** Every engine claim below carries a file:line into the UE
4.27.2 mirror `AlexMercer-MA/UnrealEngine-4.27`, whose `Build.version` reads 4.27.2 /
`++UE4+Release-4.27` — **HARD** for anything quoted from it. Claims about *our* code are HARD
(read at the cited line). Layout claims about Renderer-private types are **[derived]** and are
labelled as such every time, because a derived offset that is wrong dereferences engine memory.

---

## 0. The ranked table

Ranked by **measured cost or bug risk**, biggest first. "After item 2" means: assuming the
`engine-seam-l1` work lands and colour/depth/velocity identity comes from `FPassInputs`.

| # | What is inferred | Where | Measured cost / risk | Engine truth, and the route | After item 2 | Verdict |
|---|---|---|---|---|---|---|
| 1 | **Which resource every descriptor slot views**, and every root argument, rebuilt from `Create*View` / `CopyDescriptors` / `SetComputeRoot*` hooks | `src/backend_native/descriptor_shadow.cpp`, `root_shadow.cpp`, `resource_registry.cpp`, `d3d12_hooks.cpp` | **2.913 ms/frame, 14% of a 20.2 ms frame**; 33 826 436 shadow slots at frame 7800; the Config-B flicker (1 frame in 5 undriven, facts §29) and the 52→104 fps hitch recovery (facts §30) both came from this machinery | The TAA hook's share is replaced by `FPassInputs` (`TemporalAA.h:150-157`); the rest has no engine route | **Partly.** Three consumers keep it alive: the View-CB search (#3), the `u0` identity (#2b), the state restore | **Shrink, do not delete.** §1 |
| 2 | **The output rect DLSS is created for** is the matcher's `group_count × 8`, not the engine's announced rect — *while the log says the engine's is used* | `src/taa_hook.cpp:1916-1917` against `:1396-1401` | **A false diagnostic on the shipping path.** They agree today; the log would not say so if they stopped | `*OutSceneColorViewRect`, already captured in `seam_verdict.out_width/height` and in scope at the call site | No — this is orthogonal | **Fix.** ~4 lines. §2 |
| 3 | **Which bound constant buffer is `View`** — "try every one, keep the first that parses plausibly" | `src/taa_hook.cpp:1268-1290`, `:895-922` | The last hard dependency the TAA hook has on the descriptor shadow's *write* side | `FSceneView::ViewUniformBuffer` (`SceneView.h:901`) — but that is a [derived] offset into an ENGINE_API class and then an RHI-buffer chain | No | **Keep. Revisit only as part of §1.** §3 |
| 4 | **The cooked-hash table still decides which colour-identification path runs**, though it was demoted from a gate to an assertion | `src/taa_hook.cpp:1817-1818` | An engine-announced pass whose hash is not cooked silently falls back to the weaker heuristics | `FPassInputs::SceneColorTexture` (`TemporalAA.h:153`) | **Yes, wholly** | **Subsumed by item 2.** §4 |
| 5 | **The camera-cut OR's third signal** — "the history or velocity SRV is a 1×1 texture" standing in for `!InputHistory.IsValid()` | `src/core/view_params.cpp:179-190`, `taa_signature.cpp:284-312` | Every missed cut is a temporal-history error that compounds; every false cut is a discarded accumulation | `bCameraCut = !InputHistory.IsValid() \|\| View.bCameraCut` — **HARD**, `TemporalAA.cpp:644`. Half of it (`View.bCameraCut`) we already read exactly, as View row 145.x | Partly — the dummy test moves onto the engine's own velocity texture identity | **Keep, assert against the engine.** §5 |
| 6 | **The pin, the history round-trip, the aspect-ratio band and the 3.5× upscale ceiling** | `src/taa_hook.cpp:179-192, 1691-1722, 1942-1967, 2358-2382`; `core/taa_signature.cpp:251-282` | Dead under `EngineSeam=3` but **still executed every dispatch**, and still the whole gate under `EngineSeamFallback=1` | The announcement, already live | Independent of item 2 | **Delete after one clean level-3 session.** §6 |
| 7 | **"Are we in gameplay"** — a distinct-shader census with a threshold, then `taa_pipelines>=1 && dispatches advancing` | `src/app/dlss_app.cpp:1253-1268` | Already wrong once (400 → 300 → structurally false under this host, facts §22); currently a self-described non-classifier | `UGameplayStatics::IsGamePaused`, the world name and `BP_CatPawn_C` — **already implemented and running** in `mods/StrayProbe/Scripts/main.lua:104-150` | No | **Delete the duplicate or read the probe's file.** §7 |
| 8 | **The base-pass G-buffer set**, rebuilt from render-target binds by format+extent+slot order, with a 30-frame arming gate and an unresolvable B/C ambiguity | `src/gbuffer_finder.cpp`, `core/gbuffer_classify.cpp` | ~1 900 lines serving a feature that is **off by default** (`NgxRR=0`, `GBufferFinder=0`), and whose events this host does not even deliver | `FSceneTextureParameters` names GBufferA-F and velocity explicitly (`SceneTextureParameters.h:12-22`) — but it is a struct built by a `RENDERER_API` free function, not a readable field | No | **Keep as-is; no cheap route.** §8 |
| 9 | **Engine cvars baked into code comments as HARD facts** — `r.SelectiveBasePassOutputs`, `r.AllowStaticLighting`, `r.SceneColorFormat`, `ScreenPercentage` | `src/core/gbuffer_classify.hpp:14, 29, 31`; `taa_hook.cpp` render-rect comments | A user's `Engine.ini` edit silently falsifies a HARD-labelled assumption, with nothing asserting it | `IConsoleManager::Get().FindConsoleVariable(...)->GetInt()` (`IConsoleManager.h:833, 914, 414`) — but `Get()` is `FORCEINLINE` over a **private static** `Singleton` (`:929`), unexported in a monolithic build | No | **Assert, do not fetch.** §9 |
| — | Velocity decode constants, the dense-MV resolve, FG's `GameIndexMirror`, NR's guide contract, the present owner, `ext_unhook`, the vtable slot table | various | — | — | — | **Legitimately D3D12/RHI-level. Keep.** §10 |

**The one-line summary.** After items 1 and 2, the project's remaining ReShade shape is not the
*identification* any more — that is solved — it is the **descriptor shadow that exists to answer
questions the engine could answer**, and it survives item 2 for exactly three reasons (§1.3). Every
other finding is small, and two of them (§2, §4) are bugs rather than architecture.

---

## 1. The descriptor shadow, the root shadow and the resource registry

### 1.1 What is inferred, and how

`src/backend_native/descriptor_shadow.hpp:31-60` defines a `ViewEntry` — resource id, view format,
extent, buffer offset/size, a liveness generation — and the shadow keeps one per **descriptor
handle**. It is fed by hooks on nine device methods (`d3d12_hooks.cpp:380-540`):
`CreateConstantBufferView`, `CreateShaderResourceView`, `CreateUnorderedAccessView`,
`CreateRenderTargetView`, `CreateDepthStencilView`, `CopyDescriptors`, `CopyDescriptorsSimple`, plus
`CreateDescriptorHeap` for the flat arrays. `root_shadow.hpp:18-33` keeps, per command list, the
compute root signature, the table handles by root parameter, root CBV/SRV/UAV addresses and 32-bit
constants. `resource_registry.cpp:129-220` snapshots `GetDesc`, the heap properties and the GPU
virtual address at creation, and attaches a hand-laid COM sentinel through
`SetPrivateDataInterface` so the runtime's final `Release` tells us the resource died.

`NativeBackend::resolve_compute_bindings` (`native_backend.cpp:174-308`) then walks the bound root
signature's tables, converts each GPU handle back to a CPU handle
(`shadow::gpu_to_cpu`), looks the slot up, and produces `DispatchBindings` — the SRVs, UAVs and
constant buffers of one dispatch, **by shader register**.

**All of it exists to answer one question the ReShade add-on could not ask any other way: *which
engine resource is bound where*.**

### 1.2 What it costs, measured

From the plugin's own periodic line (`src/perf.cpp:373-374`), a live session:

```
[perf] native hooks/frame: shadow-write 0.050ms (44 views), shadow-copy 1.644ms (734 calls, 4059
descs), heap-bind 0.038ms (18), root-bind 0.643ms (559), resolve 0.539ms (7.0) - total 2.913ms
(14% of 20.2ms; summed over threads)
NATIVE SHADOW GROWTH [frame 7800] slots=33826436
```

**HARD.** Two thirds of that total (`shadow-copy` + `root-bind` = 2.287 ms) is the **write** side —
work done on every descriptor copy and every `SetComputeRoot*` the game makes, whether or not any
dispatch ever asks about it. `resolve` — the part the TAA hook actually consumes — is 0.539 ms over
7 dispatches a frame.

Its bug history is the other half of the cost, and it is not small:

* **facts §29**, the Config-B flicker: under ReShade the game's online descriptor heap is created
  through ReShade's proxy device, which the real-device hook never saw, so ~1 frame in 5 resolved
  the pinned TAA pass to unknown, the gate refused, and the engine's own TAA ran. The user saw the
  image and the DLSS indicator flicker in sync.
* **facts §30**, the SR-only deficit: the shadow's own data structures cost 52 fps against the
  add-on's 113, with 6-10 hitch buckets out of 57. Three rewrites (drop the reverse index, shard by
  address, then flat lock-free arrays) recovered it to ~104 with 0 hitches. The reverse index that
  was dropped had grown to 15 M entries and cost up to 63 ms in a frame.
* **facts §15**, the observer runs: 39 384 of 39 505 dispatches disagreed with ReShade's tracker,
  bounded to two UE4 constant ring buffers whose GPU-VA map the registry did not cover.

### 1.3 What survives item 2, and why

Item 2 gives the TAA hook the engine's own colour, depth and velocity textures. That is the
majority of what `resolve_compute_bindings` was consulted for. **Three consumers keep the shadow
alive on the shipping path:**

1. **The View constant buffer.** `taa_hook.cpp:1273-1285` iterates `b.constant_buffers` — which the
   native backend fills from the root-CBV addresses via `registry::buffer_for_va`
   (`native_backend.cpp:289-305`) — and keeps the first that parses as a plausible `View`. Without
   the shadow there is no list to iterate. This is §3, and it is the load-bearing one.
2. **The output UAV `u0`.** `taa_hook.cpp:1791-1796` and `:1906-1911` find the output by register
   from `b.uavs`. **The engine states this too** and item 2 may not cover it: `AddPasses`'s
   `FRDGTextureRef* OutSceneColorTexture` (`TemporalAA.h:174`) is written by
   `AddGen4MainTemporalAAPasses` at `TemporalAA.cpp:1514` — but note it is written **after** the
   forwarded call, and the RDG texture's RHI resource is assigned during `FRDGBuilder::Execute`,
   not at setup. Whether that pointer is dereferenceable at the seam is the same open question
   `docs/RESEARCH-ENGINE-TAA-HOOK.md` §4.2 raises for the inputs, and the `engine-seam-l1` work
   will answer it for free. **Flagging it because the brief's item 2 names only `FPassInputs`, and
   the output is one field further along the same call.**
3. **`restore_game_compute_state`** (`native_backend.cpp:370-389`). We clobber the game's root
   signature, PSO and heaps on the game's own list, and D3D12 has no state getters. This is
   **legitimately D3D12-level** and cannot move to the engine.

### 1.4 Effort and risk

**Not a single change.** The honest shape is: item 2 removes the *consumer* pressure, §3 would
remove the last one, and only then does the write side become deletable — which is where the
2.287 ms/frame lives. Anything short of that shrinks nothing measurable, because the write side runs
per descriptor-copy regardless of who reads it.

**Recommendation: SHRINK, staged, and only after §3 is settled.** The intermediate state (item 2
landed, shadow still fully written) buys correctness and no performance, and that is fine — but do
not expect the perf line to move until the write side can be gated.

---

## 2. The engine's output rect is announced, logged as used, and not used

**This is a bug, and it is four lines to fix.**

Under `EngineSeam=3` the seam captures the engine's own `*OutSceneColorViewRect` into
`seam_verdict.out_width/out_height` (`engine_seam_hook.cpp:110-121`, `taa_hook.cpp:1329`). When the
matcher's rect disagrees, `taa_hook.cpp:1396-1401` warns, **verbatim**:

> `ENGINE SEAM ASSERTION: pass 0x... claimed the engine's %ux%u announcement but the matcher reads
> its output rect as %ux%u. **The engine's rect is used.** Once per pass.`

The rect that actually reaches DLSS is the matcher's (`taa_hook.cpp:1916-1917`):

```cpp
fd.output_width  = m.output_width  ? m.output_width  : render_w;
fd.output_height = m.output_height ? m.output_height : render_h;
```

`m.output_width` is `group_count_x * 8`, clamped to the output UAV's own extent
(`core/taa_signature.cpp:239-242`) — a reconstruction. `seam_verdict.out_width` is the engine's
`FIntRect`, read out of the out-parameter after the forwarded call. **They are different
quantities**, and `seam_verdict` is in scope at line 1916 (declared at `:1325`).

**Why they agree today**, and why that is not a defence: `AddGen4MainTemporalAAPasses` sets
`*OutSceneColorViewRect = SecondaryViewRect` where `SecondaryViewRect = TAAParameters.OutputViewRect`
(**HARD**, `TemporalAA.cpp:1456`, `:1516`), and the dispatch is
`GetGroupCount(PracticableDestRect.Size(), GTemporalAATileSizeX)` with
`PracticableDestRect = DivideAndRoundUp(OutputViewRect, ResolutionDivisor)` and
`ResolutionDivisor == 1` for every `Main*` config (**HARD**, `TemporalAA.cpp:958`, `:656`,
`GTemporalAATileSizeX = 8` at `:16`). And `QuantizeSceneBufferSize` keeps the output rect a multiple
of 8 in practice. So `ceil(rect/8)*8 == rect` and the two agree — **until they do not**, at which
point the ledger's claim rule still matches (it compares group counts, `engine_seam.hpp:294`) and
DLSS is silently created for a rect up to seven pixels short of the one the engine wrote.

**The deeper point, and it is the reason this ranks second rather than ninth:** the log asserting
one thing while the code does another is precisely the class of failure CLAUDE.md §0.2 exists to
prevent. A future session reading that WARN would conclude the engine's answer is authoritative for
the rect. It is not.

**Fix:** use `seam_verdict.out_width/out_height` for `fd` when `seam_gate == seam::Gate::engine`,
keep the matcher's as the fallback, and leave the assertion as-is. **Effort: trivial. Risk: low** —
the values are equal on every measured configuration, so the change is provably inert today and
correct tomorrow.

---

## 3. Finding the View constant buffer by searching every bound CB

### 3.1 What is inferred

`taa_hook.cpp:1273-1285`:

```cpp
for (const auto &cb : b.constant_buffers) {
    ue4::ViewParams candidate{};
    if (read_view_cb(cb.second, candidate) && ue4::view_params_plausible(candidate)) { ... break; }
}
```

`read_view_cb` (`:895-922`) then maps the buffer and copies a fixed 2448-byte prefix, and
`view_params_plausible` (`core/view_params.cpp:85-109`) decides whether it looks like `View` — a
render-target-sized `ViewSizeAndInvSize` whose `.zw` are the reciprocals of `.xy`, a sub-pixel
jitter, a small positive sequence length. The register is not fixed: `intercept/types.hpp:65-68`
records that b3, b4 and b5 have all been observed carrying it, and CLAUDE.md §2.3 records b1 → b4
between two resolutions.

**Two separable things are happening here**, and conflating them is what makes this look worse than
it is:

* **The LAYOUT** — which row holds what. This is the **strongest-evidenced part of the whole
  project** and must not be touched. Row 135 validates itself from a single read: the UE 4.27
  member table has `int32 NumSceneColorMSAASamples`, `float PreExposure`, `float OneOverPreExposure`
  consecutively (**HARD**, `SceneView.h:635-637`), which is exactly the measured
  `(denormal, P, 1/P, 0.0)` (CLAUDE.md §2.6, measured 2026-09-01). Three data types landing where
  predicted, from one `memcpy`.
* **The SEARCH** — which bound buffer it is. *This* is the ReShade shape: it needs the full
  `constant_buffers` list, which needs the root-CBV GPU-VA map, which needs the registry and the
  root shadow. It is §1's last consumer.

### 3.2 The engine truth, and why the route is not cheap

`FSceneView::ViewUniformBuffer` is a `TUniformBufferRef<FViewUniformShaderParameters>` (**HARD**,
`SceneView.h:901`), and `FViewInfo` inherits it (`SceneRendering.h:1033`). The seam hands us
`const FViewInfo&`. So the *identity* of the View uniform buffer is one field read away —

— except that reaching it needs `FSceneView`'s member offset, and `FSceneView` is an `ENGINE_API`
class of ~280 members with `#if WITH_EDITOR` blocks. **[derived], and this project has a written
rule against exactly this** (`docs/RESEARCH-ENGINE-TAA-HOOK.md` §9: *"Do not read `FViewInfo`"*).
And even with the offset, a `TUniformBufferRef` is an `FRHIUniformBuffer*`, from which the D3D12
upload allocation is another undocumented hop.

**There is no cheap engine route to the View CB, and that is a real finding rather than a gap.**

### 3.3 Recommendation

**KEEP the row parsing (it self-validates). KEEP the search.** Revisit only if §1's write side is
being deleted, at which point the search is the blocker and the cost/benefit changes. Rank it third
because it is the *reason* §1 cannot shrink, not because it is itself wrong.

---

## 4. The cooked-hash table still decides the colour path

**Subsumed by item 2. Recorded because it is invisible from the outside.**

`EngineSeam=3` demoted the hash table from a gate to an assertion — `taa_hook.cpp:1389-1394` warns
once per pass when an announced pass's hash is not cooked and proceeds on the engine's word. But
**one consumer still branches on it** (`taa_hook.cpp:1817-1818`):

```cpp
const bool trust_registers = m.verdict == MatchVerdict::hash_and_structural;
```

and `hash_and_structural` is exactly `is_known_taa_hash(sig.shader_hash)`
(`core/taa_signature.cpp:316`). When it is false, `reg_colour` stays 0 and the colour input is
chosen by the fallback chain at `:1879-1902` — "whichever live colour slot is not last frame's
`u0`", the heuristic the code's own comment (`:1806-1809`) says *"has picked the wrong resource at
least once"*.

**So a pass the engine announced, whose hash a game update recooked, would run DLSS on a colour
input chosen by the weakest identification in the file** — and the only signal would be one
`ENGINE SEAM ASSERTION` line whose text says the opposite ("Proceeding on the engine's word").

Engine truth: `FPassInputs::SceneColorTexture` (**HARD**, `TemporalAA.h:153`), assigned at the call
site from `SceneColor.Texture` (`PostProcessing.cpp:553`). Item 2 removes the whole chain — the
register map, the fallback heuristics, `g_prev_output`, and `trust_registers` with them.

**Recommendation: no separate work. Verify when `engine-seam-l1` lands that `trust_registers`,
`kSceneColourReg`/`kHistoryReg` and the `slot_a`/`slot_b`/`history` fallback are all gone.** If any
survives, this is the line that makes it dangerous.

---

## 5. The camera-cut OR

### 5.1 What is inferred

`core/view_params.cpp:179-190` ORs three signals:

1. `p.camera_cut != 0` — View row 145.x;
2. `TemporalAAJitter.zw == .xy` — a measured heuristic (CLAUDE.md §2.8);
3. `history_or_velocity_is_1x1` — passed in from `MatchResult::camera_cut_dummies`, computed at
   `core/taa_signature.cpp:284-312` by testing whether the velocity SRV is absent or a colour SRV is
   1×1.

### 5.2 The engine truth

**HARD, and it is one line.** `TemporalAA.cpp:644`:

```cpp
const bool bCameraCut = !InputHistory.IsValid() || View.bCameraCut;
```

(the Gen5 path repeats it at `:998`). So the engine's answer has exactly **two** terms, and our
three-signal OR is a faithful reconstruction of it:

* `View.bCameraCut` (**HARD**, `SceneView.h:1048`) is our signal 1 — we read it exactly, out of the
  View CB, at row 145.x. **This half is not an inference at all.**
* `!InputHistory.IsValid()` is what signals 2 and 3 stand in for. `InputHistory` is
  `View.PrevViewInfo.TemporalAAHistory` (**HARD**, `TemporalAA.cpp:1484`; the member is at
  `SceneRendering.h:929`), and the engine substitutes the 1×1 black dummy for velocity and history
  precisely when it is invalid — which is why signal 3 works.

### 5.3 After item 2, and the recommendation

Signal 3 currently tests *bound descriptors*. After item 2 it can test the **engine's own**
velocity texture instead — a null or dummy `FPassInputs::SceneVelocityTexture` is the same
information from the same source the engine used. That is a small improvement in provenance and no
change in behaviour.

Signal 2 (`jitter.zw == .xy`) is the one genuinely heuristic term and it has a **known false
positive**: two identical consecutive jitter samples are legal in any finite sequence. It is
harmless (a spurious reset costs one frame of accumulation) and it was measured firing 3 times in
the menu and 5 in gameplay (CLAUDE.md §2.8) — i.e. rarely.

**Recommendation: KEEP, and add an assertion rather than a replacement.** Log, once, when signal 2
fires while signals 1 and 3 are both false — that is the only configuration in which the heuristic
is deciding anything on its own, and nobody has ever counted it. **Effort: ~10 lines. Risk: none
(log-only).**

---

## 6. The pin, the round-trip, and the shape gates: dead under the seam, still executed

### 6.1 What is bypassed today

Under `seam::Gate::engine` (`core/engine_seam.hpp:113`), `taa_hook.cpp` bypasses:

| Mechanism | Where | How it is bypassed |
|---|---|---|
| the cooked-hash gate | `:1587-1591` | `worth_resolving = true` |
| the pin, and its 300-present staleness release | `:1691-1722` | the release is guarded by `seam_gate != seam::Gate::engine`; `eligible = true` at `:1719` |
| the history round-trip proof (`owns_temporal_history`) | `:1719-1722`, `:984-989` | same `eligible = true` |
| the aspect-ratio band and the 3.5× ceiling | `:1942-1959` | `shape_ok` is only computed when `seam_gate != seam::Gate::engine` |

The engine's own answer makes every one of them redundant, and the reason is structural rather than
empirical: `AddPasses` is reached from exactly two call sites (`PostProcessing.cpp:559` desktop,
`:2005` mobile) and **every documented look-alike calls `AddTemporalAAPass` directly** — the
`ETAAPassConfig` enum itself enumerates them (`TemporalAA.h:22-27`: `LightShaft`, `DiaphragmDOF`,
`DiaphragmDOFUpsampling`, plus SSR and water elsewhere). A planar reflection runs a full
`Main`-config TAA and is what the aspect-ratio band existed to exclude; through the interface it
never appears.

### 6.2 What still runs, and what it costs

**They are bypassed, not removed.** Every one is still compiled, still branched on, and two still
do per-dispatch work:

* the **history round-trip recorder** (`:2358-2382`) takes `g_mutex` and scans every SRV of every
  matched dispatch, every frame, to maintain `g_prev_output` and `g_roundtrip_seen` — data nothing
  on the level-3 path reads;
* the **structural matcher** still runs in full (`:1301`), including the depth+stencil search, the
  rect arithmetic and the aspect band, because its `MatchResult` is still how the register roles are
  extracted. That is the correct design *today* and stops being necessary after item 2.

They are also the **entire** gate under `EngineSeamFallback=1` with a dead seam, which is why
deleting them needs the level-3 session first.

### 6.3 Recommendation and effort

**DELETE, after one clean `EngineSeam=3` session with `unclaimed=0`** — the condition
`docs/RESEARCH-ENGINE-TAA-HOOK.md` §5 already sets ("the cross-check must come back … *before* any
of the above is demoted"), and facts §36.5 records that a level-3 session had not yet run when that
was written. **The `EngineSeamFallback` question must be answered first**: deleting the heuristic
deletes the fallback, so either the fallback goes too (and `EngineSeamFallback=0` becomes the only
behaviour) or the heuristic stays. **That is a decision, not an implementation detail, and it
belongs to the user.**

---

## 7. "Are we in gameplay"

### 7.1 Three generations of the same inference

* **The depth histogram** (CLAUDE.md §2.4: *"menu and load frames carry no usable depth range"*) —
  **documented but never implemented.** A search of `src/` finds no histogram and no depth-range
  sampling. Recorded here so a future session does not go looking for it.
* **The shader census threshold** — `distinct_shader_hashes.size() >= 400`, then `>= 300`. Measured
  structurally false under this host (facts §22): the plugin's `on_pipeline` receives **compute**
  pipelines only, ~34 in gameplay against the add-on's ~390.
* **What ships now** (`src/app/dlss_app.cpp:1263-1268`): `taa_pipelines >= 1 && dispatches
  advancing`, whose own comment says it *"is NOT a strict menu-vs-gameplay classifier"*.

### 7.2 The engine truth is already implemented, in another mod

`mods/StrayProbe/Scripts/main.lua` answers this from inside the engine and has been running on the
box since 2026-09-02:

* `FindFirstOf("BP_CatPawn_C")` for the player pawn (`:110`),
* `UEHelpers.GetPlayerController()` (`:133`),
* the world's object name, treating any name containing "menu" as not gameplay (`:95-100`, `:165`),
* `UGameplayStatics::IsGamePaused(world)` (`:142-150`).

All on the game thread through `ExecuteInGameThread`, with file I/O on the async thread — the
threading discipline five mods now share. **This route is HARD and proven**, and it is available to
the C++ plugin for the same reason it is available to the Lua one (UE4SS loads via `dwmapi` in every
configuration).

### 7.3 Recommendation

**DELETE the plugin's own `in_game` field**, or have it read `stray-game-state.txt` when present.
Its only consumer is the automation harness, which already reads the probe's file directly
(`tools/launch-stray-safe.sh`). Keeping a second, weaker classifier in the render host is how the
400-vs-300 threshold survived being wrong for a session. **Effort: small. Risk: none** — nothing on
the render path reads it.

---

## 8. The G-buffer finder

### 8.1 What is inferred

`src/gbuffer_finder.cpp` (759 lines) watches every `OMSetRenderTargets`, classifies sets of ≥4 RTVs
with `core/gbuffer_classify.cpp` (283 lines) by format and extent, requires a shape to hold 30
consecutive frames before arming, serves the freshest accepted bind if it is ≤2 presents old, and
assigns GBufferB/C **by slot order alone** — an ambiguity the header itself says nothing measurable
on this machine can resolve (`gbuffer_classify.hpp:41-44`).

### 8.2 The engine truth, and why the route is not cheap

The engine does not infer any of this. `FSceneTextureParameters` (**HARD**,
`SceneTextureParameters.h:12-22`) names them:

```cpp
SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneDepthTexture)
SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<uint2>, SceneStencilTexture)
SHADER_PARAMETER_RDG_TEXTURE(Texture2D, GBufferATexture)  // ... B, C, D, E, F
SHADER_PARAMETER_RDG_TEXTURE(Texture2D, GBufferVelocityTexture)
```

built by `GetSceneTextureParameters(FRDGBuilder&)` (`:26`). **But there is no field to read and no
vtable to stand in for**: it is a value struct returned by a `RENDERER_API` free function (empty in
a monolithic target), consumed by passes that are not virtual. The seam's `FPassInputs` carries
colour, depth and velocity and **nothing else** (`TemporalAA.h:150-157`) — the G-buffer is not on
that path.

**UNCONFIRMED whether any second seam exists** for this. The obvious candidate — an
`IScreenSpaceDenoiser` vtable, the same trick that worked for `ITemporalUpscaler` — is not
investigated here and should not be assumed; the brief's rule against speculating about
Renderer-private structure applies.

### 8.3 Recommendation

**KEEP as-is.** It is off by default (`GBufferFinder=0`, `NgxRR=0` in `StrayDLSS.ini:29, 36`), and
the plugin host does not even deliver the render-target and draw events it needs — `Host.cpp:420`
logs exactly that: *"the finders' render-target/draw/copy taps are NOT delivered by this host"*.
**So under the shipping host this ~1 900 lines is inert code, and that is worth knowing before
anyone spends a session on RR.** The one thing worth doing is cheap: make that inertness loud at
startup when `NgxRR=2` is set under this host, rather than a line in a different log.

---

## 9. Engine cvars baked into comments as HARD facts

`src/core/gbuffer_classify.hpp` carries three settings as premises of its classification logic:

* `:14` — *"Stray ships `r.SelectiveBasePassOutputs=True` (DefaultEngine.ini:85, HARD)"*
* `:29` — *"Stray ships `r.AllowStaticLighting=True`"*
* `:31` — *"Stray ships `r.SceneColorFormat=3`"*

They were read from the game's own pak, which makes them HARD **about the shipped defaults** — and
`Engine.ini` overrides in the Proton prefix take effect (CLAUDE.md §2.2), so a user's edit
silently falsifies a HARD-labelled premise with nothing asserting it. The same shape applies to
`ScreenPercentage=50` and `r.TemporalAA.Upsampling=True`, which several comments reason from.

**The engine route exists and is not free.** `IConsoleManager::FindConsoleVariable` (**HARD**,
`IConsoleManager.h:833`) plus `IConsoleVariable::GetInt/GetFloat` (`:414-416`) is the read. But
`IConsoleManager::Get()` is `FORCEINLINE` over a **private static** `Singleton`
(`IConsoleManager.h:914-922, 929`), and in a monolithic game executable `CORE_API` expands to
nothing (`docs/RESEARCH-ENGINE-TAA-HOOK.md` §2, HARD from `UEBuildModule.cs:545-554`) — so from an
injected DLL there is no linkage and the singleton must be located by scan. UE4SS offers no help:
`docs/RESEARCH-UE4SS-MIGRATION.md:587` records that it has **no CVar API at all**.

**One cheaper route exists for one of them.** `HKGameUserSettings.ScreenPercentage` is a
`UInt32Property` on a `UGameUserSettings` subclass (`docs/RESEARCH-STRAY-MENU-OPTIONS.md:96-98`) —
a plain UObject field, reachable by the reflection route five mods already use.

**Recommendation: ASSERT, do not fetch.** The values the code depends on are all observable in the
frame it already sees — the base-pass MRT count, the scene-colour format, the render/output ratio.
Where a comment says "Stray ships X", the code should say so out loud when what it observes
contradicts it. **A cvar-singleton scan is a whole seam's worth of risk for a premise a format
check already tests.**

---

## 10. Legitimately D3D12/RHI-level — keep, and here is why for each

Stated explicitly so a future sweep does not re-open them.

| Thing | Where | Why there is no engine answer |
|---|---|---|
| **The velocity decode constants** | `core/ue4_view.hpp:74-100` | Not an inference at all: the encode is quoted from `Common.ush:1537-1570` and **both constants were located in Stray's own DXBC** (CLAUDE.md §2.5). The validity test `EncodedVelocity.x > 0` is the shader's own. |
| **The dense-MV resolve pass** | `src/mv_resolve.cpp` | Mandatory by format: DLSS accepts `RG16_FLOAT`/`RG32_FLOAT` and UE4's velocity is `R16G16B16A16_UNORM`. Its *inputs* are §1/§3's problem; the pass itself is ours by necessity. |
| **`srv_format_for`'s typeless→typed map** | `mv_resolve.cpp:321-332` | We create our own SRVs, so we must name a plane. The engine does the same thing at `TemporalAA.cpp:777`: `CreateSRV(FRDGTextureSRVDesc::CreateWithPixelFormat(Inputs.SceneDepthTexture, PF_X24_G8))` — **which is also the source of the depth+stencil-over-one-resource signature** that identified the pass for a year (`:771` binds the depth, `:777` the stencil view of the same texture). |
| **FG's `GameIndexMirror`** | `core/fg_plan.hpp:27-41` | It mirrors `FD3D12Viewport::CurrentBackBufferIndex_RHIThread`, an RHI-private field, and UE4 never calls `GetCurrentBackBufferIndex` (facts §32.4, HARD). There is nothing to read from either side; the mirror is exact by construction. |
| **FG's camera near/far** | `ngx_fg.hpp:36-49` | Deliberately **NVIDIA's synthetic pair** (0.01 / 75000), not the engine's — their own plugin documents them as not needing to match (CLAUDE.md §5, corrected 2026-09-03). Using `View.NearPlane` here would be *less* right. |
| **NR's guide publication** | `src/nr_hook.cpp:142-177` | A cross-thread handoff of values the TAA site already holds, not a reconstruction. Its one assumption, `NgxNRStageBackBufferState`, is a D3D12 resource-state question with a knob and an honest comment. |
| **The present owner, the swapchain hooks, the queue pick** | `src/backend_native/present_owner.cpp` | The frame boundary and a command list that runs after the game's last submission. Purely a D3D12/DXGI concern. |
| **`ext_unhook`** | `src/ext_unhook.cpp` | Repairs ReShade's patch of vkd3d's `ID3D12DeviceExt` vtable. Interop, nothing engine-side. |
| **The vtable slot table** | `backend_native/vtable_slots.hpp` | COM slot indices, compile-time-checked against the Windows SDK's own `Vtbl` structs (`vtable_slots_check.c`). |

---

## 11. Suggested order of work, and what each step deletes

Line counts are **estimates from the cited regions**, not a compiler's answer.

### Step 0 — one clean `EngineSeam=3` session (no code)

The precondition for steps 2 and 3. Read the periodic `[seam]` line: `unclaimed` must be 0 and
`orphans` must stay 0 through a walked session. facts §36.5 records this had not run when the seam
work was written. **Deletes nothing; licenses everything below.**

### Step 1 — use the engine's rect (§2)

`src/taa_hook.cpp:1916-1917`. **~4 lines changed, 0 deleted.** Do this first because it is the one
item that is wrong today, it is independent of `engine-seam-l1`, and it makes an existing log line
true.

### Step 2 — delete the demoted heuristics (§6), after step 0

| File | Region | ~lines |
|---|---|---|
| `src/taa_hook.cpp` | pin state + release + publish (`:179-192`, `:1691-1722`, `:2240-2246`) | ~55 |
| `src/taa_hook.cpp` | round-trip recorder + `owns_temporal_history` (`:2358-2382`, `:984-989`, `g_prev_output`) | ~35 |
| `src/taa_hook.cpp` | the create-site shape gate (`:1942-1967`) | ~26 |
| `src/taa_hook.cpp` | `kGatePinnedElsewhere`, `kGateNoRoundTrip`, `kGateNotPrimaryView` + texts | ~12 |
| `src/core/taa_signature.cpp` | aspect band + upscale ceiling (`:251-282`) | ~32 |
| `src/taa_hook.hpp` | `owns_temporal_history`, `set_ngx_pass_hash` declarations | ~8 |
| `tests/` | the corresponding cases | ~60 |
| **total** | | **~230** |

**Blocked on a decision:** these are also the `EngineSeamFallback=1` path. Deleting them means
`EngineSeamFallback` stops meaning "run the old heuristic" and starts meaning "refuse".

### Step 3 — after `engine-seam-l1` lands, delete what item 2 orphans (§4)

| File | Region | ~lines |
|---|---|---|
| `src/taa_hook.cpp` | `trust_registers`, `kSceneColourReg`/`kHistoryReg`, the colour fallback chain (`:1791-1902`) | ~110 |
| `src/core/taa_signature.cpp` | depth+stencil search, colour/velocity slot extraction, dummy detection (`:134-161`, `:284-312`) | ~57 |
| `src/core/taa_signature.cpp` | dispatch-rect arithmetic (`:181-249`) | ~68 |
| `src/core/taa_hashes.hpp` + `is_known_taa_hash` + the override file loader | (`taa_signature.cpp:27-40, 93-100`; `dlss_app.cpp:1705-1735`) | ~85 |
| **total** | | **~320** |

Keep `is_ssd_temporal_hash` — it is the RR guide trigger and unrelated.

### Step 4 — the gameplay gate (§7)

`src/app/dlss_app.cpp:1253-1268`, **~16 lines**, plus the `distinct_shader_hashes` set if nothing
else wants it. Independent of everything above.

### Step 5 — assertions instead of premises (§5, §9)

**Adds ~30 lines, deletes none.** One counter for the jitter-only camera cut; one startup line when
the observed base-pass shape contradicts the baked `r.*` premises. Both log-only.

### Step 6 — the descriptor shadow (§1), and only if §3 is solved first

The 2.287 ms/frame write side. **Do not start this before the View-CB search has an answer** —
without it the shadow cannot be gated and nothing measurable changes. If §3 stays unsolved, the
correct outcome of this sweep is that the shadow **stays**, and that is not a failure: it is the
price of reading a constant buffer the engine will not hand us.

---

## 12. Provenance ledger

| Claim | Status |
|---|---|
| `ITemporalUpscaler::FPassInputs` is `{bool, EPixelFormat, 3× FRDGTextureRef}` | **HARD**, `TemporalAA.h:150-157` |
| `AddPasses` writes `*OutSceneColorViewRect = SecondaryViewRect = TAAParameters.OutputViewRect` | **HARD**, `TemporalAA.cpp:1456`, `:1516` |
| The dispatch is `GetGroupCount(PracticableDestRect.Size(), 8)`, `ResolutionDivisor == 1` for `Main*` | **HARD**, `TemporalAA.cpp:958`, `:656`, `:16` |
| `bCameraCut = !InputHistory.IsValid() \|\| View.bCameraCut` | **HARD**, `TemporalAA.cpp:644` (and `:998`) |
| `FSceneView::bCameraCut` exists and is the second term | **HARD**, `SceneView.h:1048` |
| `View` uniform buffer has `NumSceneColorMSAASamples`, `PreExposure`, `OneOverPreExposure` consecutively | **HARD**, `SceneView.h:635-637` — this is what makes row 135 self-validating |
| `FViewInfo::TemporalJitterPixels`, `PreExposure`, `ViewRect`, `PrevViewInfo` exist | **HARD**, `SceneRendering.h:1220`, `:1281`, `:1038`, `:1275` |
| …and reading any of them needs `FViewInfo`'s layout | **[derived], and refused** — `SceneRendering.h:1033`, ~180 conditional members |
| `FSceneView::ViewUniformBuffer` is the View uniform buffer's identity | **HARD** that the field exists, `SceneView.h:901`; **[derived]** and unmeasured that its offset is reachable |
| `FSceneTextureParameters` names GBufferA-F + velocity | **HARD**, `SceneTextureParameters.h:12-22` |
| …and there is no field or vtable to read it from | **[derived]** from it being a value struct from a `RENDERER_API` free function (`:26`) |
| `IConsoleManager::Get()` is FORCEINLINE over a private unexported static | **HARD**, `IConsoleManager.h:914-922, 929` |
| The engine binds depth and its stencil view over one texture | **HARD**, `TemporalAA.cpp:771`, `:777` |
| `FTAAPassParameters::GetOutputExtent()`'s `Max()` degenerates at 1:1 | **HARD**, `TemporalAA.cpp:596-614` |
| The call site is `PostProcessing.cpp:535-566`, inputs assigned at `:551-557` | **HARD** |
| Our perf line, 2.913 ms / 14% / 33.8 M slots | **HARD**, a live session's own output |
| Config-B flicker, the 52→104 fps recovery, the compute-only census | **HARD**, facts §29, §30, §22 |
| `taa_hook.cpp:1916` uses the matcher's rect while `:1398` says the engine's is used | **HARD**, read at both lines |
| `trust_registers` gates the colour path on the cooked hash | **HARD**, `taa_hook.cpp:1817-1818` |
| The plugin host does not deliver the finders' RT/draw events | **HARD**, `mods/StrayDLSS/src/Host.cpp:419-421` |
| The depth-histogram gameplay gate is documented but not implemented | **HARD** (absence, searched `src/`) |
| `mods/StrayProbe` already answers "in gameplay" from the engine | **HARD**, `main.lua:104-150` |
| A second seam for the G-buffer (e.g. `IScreenSpaceDenoiser`) | **UNCONFIRMED** — not investigated, do not assume |
| Whether `*OutSceneColorTexture`'s RHI resource is readable at the seam | **UNCONFIRMED** — `engine-seam-l1` answers it for the inputs at the same time |
| Line-count estimates in §11 | **[derived]** from the cited regions; not a compiler's answer |
