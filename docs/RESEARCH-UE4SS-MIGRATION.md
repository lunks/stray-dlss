# Migrating off ReShade: owning the D3D12 interception, with UE4SS as the engine-side half

Assessment written 2026-09-01. Read-only investigation; nothing in `src/` was changed.

> **DATED 2026-09-03.** Written before NR moved to a present stage. Every reference below to
> `src/nr_history.cpp`, `src/nr_codec_pass.cpp`, `src/core/nr_codec.*` or `[STRAYDLSS] NgxNRHook`
> names something that no longer exists — the codec, the history restore and the site choice were
> deleted with the TAA hook site. The migration reasoning is unaffected (if anything it gets
> easier: NR no longer needs ReShade's `reshade_begin_effects`, an effect file loaded, or the
> `_has_commands` workaround at all), but the file inventory and the line counts are stale.

Provenance labels follow CLAUDE.md §0.5: **HARD** = read from source, measured, or read out of this
repo's own code; **SOFT** = reasoned from HARD facts; **UNCONFIRMED** = not established, do not
build on it.

---

## Verdict

**Feasible with caveats — no blocker found — but the shape of the answer is not the one the framing
implies.** The D3D12 interception has to be a plain Win32/D3D12 hook layer patching vtables on
vkd3d's device, command list, queue and swapchain: UE4SS contains no D3D12 or DXGI code whatsoever
(verified in its source *and* its shipped binary) and cannot contribute a line of it, so it is the
loading vehicle and the engine-side half, not the interception. Its real value is elsewhere and
larger than expected — cvars, camera-cut and resolution signals, and above all **deterministic
camera control**, which attacks the noisy-measurement problem that has caused most of this
project's wrong conclusions.

The one genuine technical risk is the **descriptor and root-argument shadow** (~1,000 lines
replacing ReShade's `descriptor_tracking` + `state_tracking`), and it is de-riskable to near zero
by running both backends side by side in the live game and diffing them before switching over
(§9 stage 2). **57.5% of the codebase ports with zero edits.** Two things to go in with eyes open:
the UE4SS build is gated on Epic Games GitHub org membership and has no stable ABI (§1.7), and
**frame generation is not an outcome of this work** — the best-supported cause of the FG crash is
NVIDIA-documented fullscreen-transition non-compliance, testable today for free, and a second
blocker requires a swapchain wrapper regardless of ReShade (§7).

---

## 0. Scope: what "the migration" actually is

Three separable pieces. Conflating them is the main way this goes wrong.

| Piece | What it is | Where it must live |
|---|---|---|
| **A. D3D12 interception** | Pipeline/descriptor/dispatch/present hooks; the `dispatch` suppression that is the whole technique | A plain Win32 + D3D12 static library. **Not** in UE4SS glue |
| **B. Loading vehicle** | Whatever gets (A) into the process early enough | UE4SS C++ mod, and/or a thin proxy DLL |
| **C. Engine-side access** | UE reflection: cvars, camera pose, camera cuts, resolution | UE4SS C++ mod glue, ~150 lines |

**The house pattern for (C) already exists in this repo and should be copied verbatim.**
`mods/StrayDualSense/src/Runtime.hpp:1-6` states it: *"the UE4SS glue in Mod.cpp does nothing but
translate UFunction callbacks into these calls, so the part of this mod that depends on an SDK we
cannot build against locally is as thin as it can be."* (HARD — this repo.) Applied here: the hook
layer must be buildable and testable with no UE4SS headers at all, exactly as `src/core/` is
buildable today with no ReShade headers.

**A material fact that de-risks (B) and (C) entirely: UE4SS already runs in Stray, under this
Proton build, in the same process as ReShade.** `docs/STRAY-DUALSENSE.md:162-168` hooks
`COMP_CatScratchableComponent_C:SetPS5TriggerActivated` in the live game; `:310` names `UE4SS.log`
and `stray-dlss.log` as things that failed together, i.e. both were expected in the same session.
`tools/dualsense/README.md:32` gives the install path `ue4ss/Mods/StrayTriggers/Scripts/main.lua`.
(All HARD, from this repo.) So UE4SS-on-Stray-on-Proton, coexisting with a `dxgi.dll` proxy, is
**measured, not assumed**.

Corroborated externally: UE4SS's `docs/patternsleuth-games.md:844` lists `427D_Stray` under *"Games
tested with all AOBs found by Patternsleuth"*, there is no entry for Stray in
`assets/CustomGameConfigs/` (i.e. it needs no per-game signature overrides), and UE4SS PR #1160 —
which fixed console text under Proton/Wine — was tested by its author on Stray specifically. (HARD,
read from the UE4SS repo.)

**Coexistence is structural, not lucky.** UE4SS proxies `dwmapi.dll` by default
(`cmake/modules/ProjectConfig.cmake:37`) and **contains no D3D12 or DXGI code whatsoever** — the
shipped `UE4SS.dll` imports `d3d11.dll` and `D3DCOMPILER_47.dll` and nothing else graphics-related;
a string scan finds zero occurrences of `dxgi`, `Present` or `SwapChain` outside its own
`D3D11CreateDeviceAndSwapChain` import name. (HARD, binary-level.) There is nothing for it to
contend with ReShade — or with us — over.

Operational note: the Wine builtin `dwmapi` wins unless overridden, so the launch options must
**merge** rather than replace — `WINEDLLOVERRIDES="dwmapi,dxgi=n,b"`, and `n,b` not bare `n`.
CLAUDE.md §5 already warns that clobbering Stray's launch options silently removes the add-on.

---

## 1. What must be built, ordered by risk

Difficulty is calibrated against this codebase: **S** ≈ a day, **M** ≈ a few days, **L** ≈ a week
or more of careful work with a real feedback loop.

### 1.1 Descriptor + root-argument shadow — **L, and the only item with real technical risk**

This replaces `third_party/reshade_utils/descriptor_tracking.{cpp,hpp}` (303 lines) +
`state_tracking.{cpp,hpp}` (230 lines) and the half of `src/frame_state.cpp` that consumes them
(956 lines across `.cpp`/`.hpp`, 110 `reshade::` references — **the deepest coupling in the
project**). (HARD, measured.)

**What ReShade's `descriptor_tracking` actually gives us**, read from
`third_party/reshade_utils/descriptor_tracking.hpp:30-45` (HARD):

* `get_resource_view(heap, offset)` / `get_buffer_range(heap, offset)` / `get_sampler(...)` — the
  contents of a descriptor slot, shadowed rather than read back.
* `get_pipeline_layout_param(layout, param)` — the root-signature parameter description, so a
  bound table can be walked into shader registers.

It builds that shadow from `update_descriptor_tables` / `copy_descriptor_tables`
(`descriptor_tracking.hpp:57-58`), i.e. from ReShade's *translation* of the game's
`CreateShaderResourceView` / `CopyDescriptors` calls on its proxy device. **Nothing ever reads the
game's heap.** That is the design constraint, and it is why CLAUDE.md §5 hazard 1 exists: a
shader-visible heap is an illegal copy *source* (`D3D12 ERROR #654`, reproduced in this project's
own CI — HARD).

**Our equivalent, and it is cleaner than ReShade's.** Hook on the native `ID3D12Device`:

| Method | Purpose |
|---|---|
| `CreateShaderResourceView` / `CreateUnorderedAccessView` / `CreateConstantBufferView` | record `(resource, view desc, resource desc)` |
| `CreateRenderTargetView` / `CreateDepthStencilView` | same, for the pass/G-buffer finders |
| `CopyDescriptors` / `CopyDescriptorsSimple` | propagate shadow entries |
| `CreateRootSignature` | deserialize with `D3D12CreateVersionedRootSignatureDeserializer`, keep `D3D12_ROOT_SIGNATURE_DESC1` verbatim |
| `CreateComputePipelineState` / `CreateGraphicsPipelineState` / `CreatePipelineState` | hash `CS.pShaderBytecode`, then call the original **with the caller's unmodified desc** |

and on `ID3D12GraphicsCommandList`: `SetPipelineState`, `SetComputeRootSignature`,
`SetComputeRootDescriptorTable`, `SetComputeRoot{ConstantBufferView,ShaderResourceView,
UnorderedAccessView}`, `SetComputeRoot32BitConstant(s)`, the four graphics equivalents,
`SetDescriptorHeaps`, `OMSetRenderTargets`, `RSSetViewports`, `RSSetScissorRects`, `BeginRenderPass`,
`CopyResource`, `CopyTextureRegion`, `ResolveSubresource`, `Reset`, `Close`, `Dispatch`,
`DrawInstanced`, `DrawIndexedInstanced`. Plus `ID3D12CommandQueue::ExecuteCommandLists` and
`IDXGISwapChain::{Present,Present1,ResizeBuffers,GetBuffer}`.

That is roughly 30 methods across 4 interfaces — large but fully enumerable, and it is a superset
of the 33 ReShade event types currently in use (26 ours + 15 from the vendored trackers, with
overlap; HARD, counted).

**Two design decisions that make this simpler than ReShade's version, not harder:**

1. **Key the shadow by the raw `D3D12_CPU_DESCRIPTOR_HANDLE.ptr`, not by `(heap, index)`.** (SOFT.)
   Then `CopyDescriptorsSimple(dst, src, …)` is a shadow lookup at `src.ptr` and a store at
   `dst.ptr`, and **offline heaps never need to be identified at all** — which matters, because an
   offline heap is never bound and so could not otherwise be discovered. Only the *online* heaps
   need base handles, and those arrive for free at `SetDescriptorHeaps`
   (`GetCPUDescriptorHandleForHeapStart` / `GetGPUDescriptorHandleForHeapStart` +
   `GetDescriptorHandleIncrementSize`), which is what turns the GPU handle in
   `SetComputeRootDescriptorTable` back into a CPU handle. This removes the need to hook
   `CreateDescriptorHeap` for correctness.

2. **Store the resource desc and view desc at *creation* time, and never call `GetDesc()` at
   dispatch time.** This is the direct fix for CLAUDE.md §5 hazard 2, and this repo has the
   receipt: `src/frame_state.cpp:79-86` shows the liveness check had to be hoisted *above*
   `get_resource_view_desc` / `get_resource_desc` because *"the access violation happens here,
   during binding capture, which is why the game still died on a frame the resolve correctly
   skipped."* (HARD.) If we never interrogate the object, that class of fault cannot occur.

**Why this is still the riskiest item:** it is the one part with no offline oracle. Matrix math,
velocity decode and the codec are pure functions with golden values; "did we correctly reconstruct
what UE4 had bound at register t3" can only be judged against the running game. §7 proposes a
differential oracle that removes almost all of this risk.

### 1.2 Hook installation, lifetime and coexistence — **M–L**

**Mechanism: vtable patching, not object wrapping.** This should be treated as a hard design
invariant, because it is the root cause of the entire §1 hazard class:

> **The interception layer observes and may suppress. It never substitutes an object and never
> mints a handle.**

ReShade violates that (it hands the game a proxy device and mints synthetic descriptor handles
whose encoding is `heap_index << 28 | offset & 0x0FFFFFF8` — `docs/UPSTREAM-RESHADE-EXTHOOK.md:72-75,
258`, HARD), and every §1 symptom follows from it. A pure vtable patch hands every other component
in the process — vkd3d, `nvngx_dlss.dll`, DXVK-NVAPI, OptiScaler, Streamline — the *real*
interfaces and the *real* handles. There is then no conversion anywhere, so there is nothing to get
wrong.

**Feasibility of vtable patching against vkd3d-proton:** vkd3d uses a **single static vtable** for
`ID3D12DeviceExt`, so one patch reaches every holder of that interface process-wide — measured on
the target RTX 4090 by this project's own `test_vkd3d_ext_hook_reachability`
(`tests/warp/warp_mv_resolve.cpp:1347-1400`; `before=…C645AD0 after=…D5680D0`). (HARD.) That the
*main* interfaces are likewise static-vtabled is **SOFT** — strongly indicated, since vkd3d-proton
is C with `CONST_VTBL` structs — and is settled by a three-line check: compare
`*(void**)cmdListA` against `*(void**)cmdListB`. Write that check first; the entire approach
depends on it. `src/ext_unhook.cpp:105-118` already demonstrates the `VirtualProtect`
write-to-read-only-vtable dance working on this exact target (HARD).

**Three things this creates that ReShade handled for us:**

* **Re-entrancy.** Our own NGX evaluate and resolve passes record onto the game's command list, so
  our `Dispatch` / `SetComputeRoot*` hooks will fire for our own calls. Needs a thread-local
  in-hook guard. (S, but forgetting it is a hang.)
* **Foreign command lists.** A process-wide patch fires for lists created by OptiScaler, Streamline
  or anything else. Filter by "have we seen a PSO bound on this list that we hashed". (S.)
* **Contested patch sites.** If another component patches the same slots, chain correctly (save
  original, always call it) and **never restore**. Note the irony: `ext_unhook` exists precisely
  because ReShade does not follow this rule.

**Timing — see §6.2. This is the item that decides whether §1.1 can be complete.**

### 1.3 Frame ownership at present — **M**

Replaces `addon_event::present` (`src/addon.cpp:937-1000`), which is almost entirely our own logic
already — the only ReShade touches are the `command_queue` and `swapchain->get_current_back_buffer()`
arguments. (HARD.)

Needs: our own `ID3D12CommandAllocator` + `ID3D12GraphicsCommandList` + `ID3D12Fence` for
present-time work, submitted on the swapchain's own queue *before* forwarding to the original
`Present`. This **removes** a documented trap rather than adding one: `src/nr_history.cpp:560-580`
currently routes barriers through `cmd_list->barrier()` purely because ReShade's
`flush_immediate_command_list()` early-outs on `!_has_commands`, which only ReShade's own API sets
(CLAUDE.md §5, HARD). Owning the submission makes that constraint evaporate.

### 1.4 Resource liveness — **S, and it gets strictly better**

Today: a global address-keyed live set fed by `init_resource`/`destroy_resource`
(`src/frame_state.cpp:795-801`, HARD). It has a residual hole that CLAUDE.md §5 itself names:
*"D3D12 reuses addresses, so a freed pointer can come back as a different valid resource."* The set
says "live" for the recycled address, so a stale view→resource mapping can resolve to a *different,
live* resource and pass the check. (SOFT — the hole follows directly from the documented behaviour
plus the implementation.)

Replacement, without any ReShade event: attach a small `IUnknown` to each resource via
`ID3D12Object::SetPrivateDataInterface` when we first see it (at view creation — the pointer is
right there in `CreateShaderResourceView`). D3D12 releases private-data interfaces when the object
is destroyed, so its final `Release()` is a destruction callback. On that callback, **erase every
shadow slot that references the resource.** Per-slot invalidation closes the address-reuse hole
that a global live set cannot. This is the same mechanism ReShade uses internally. (SOFT on the
mechanism being what ReShade uses; the `SetPrivateDataInterface` release semantics are documented
D3D12 behaviour.)

Bonus: it also removes the need to hook `CreateCommittedResource` / `CreatePlacedResource` /
`CreateReservedResource` at all.

### 1.5 Config and logging — **S, and both already exist in this repo**

* **Logging.** `src/log.cpp` has exactly two ReShade references (`log::message`, `log::level`, at
  `:72`). Everything else already goes through `STRAY_LOG_*` over `stray_dlss::log`. And
  `mods/StrayDualSense/src/Log.{hpp,cpp}` (203 lines) is already a mutex-guarded,
  flush-every-line, UE4SS-type-free file sink written by the same author for the same box, with a
  mirror queue for forwarding into UE4SS's log from the game thread. **Port it.** (HARD, this repo.)
* **Config.** 55 `[STRAYDLSS]` keys across 79 call sites, 74 of them in `addon.cpp` (HARD).
  `mods/StrayDualSense/src/Config.{hpp,cpp}` (266 lines) is already a **hot-reloading INI** whose
  header explains the exact reason it exists: *"this mod cannot be built or tested locally — the
  only tuning loop is the user editing a value on the box, and making that not require a relaunch is
  worth the code."* (HARD.) That is a better fit for this project than ReShade's ini ever was —
  see §4.

### 1.6 Visual verification channel — **M, and it becomes a better instrument**

See §5.3. This must be built **before** ReShade is removed, not after.

### 1.7 UE4SS glue + build/packaging — **M for the code, L for the build**

The glue itself is tiny: a `CppUserModBase` subclass plus two exported C functions,
`start_mod()` / `uninstall_mod(CppUserModBase*)`, resolved by `GetProcAddress`
(`UE4SS/src/Mod/CppMod.cpp:52-53`), shipped as `Mods/StrayDLSS/dlls/main.dll`. (HARD.)

**The build is the expensive part, and it is worse than it looks.** All HARD, from the UE4SS repo
and the shipped release archives:

* **There is no headers-only SDK.** `UE4SS/include/Common.hpp:5` makes the API `__declspec(dllimport)`;
  mods link `UE4SS.lib`, which exists only inside a UE4SS build tree. Both shipped release zips
  contain **zero** `.hpp`, `.h` or `.lib` files.
* **`git submodule update --init --recursive` requires a GitHub account linked to Epic Games**
  (`README.md:80-82`) — the `UEPseudo` submodule is gated. That is a hard prerequisite on whoever
  runs the build, including CI.
* **C++23, MSVC ≥ 19.43, VS ≥ 17.13, Rust ≥ 1.73** (patternsleuth is Rust). CI currently pins
  MSVC v143 14.44, so the toolchain is close but the Rust dependency is new.
* **The ABI is not stable.** `CppUserModBase.hpp:29-30` verbatim: *"they will break if UE4SS and the
  mod don't use the same C Runtime library version. This includes them being compiled in different
  configurations."* `UE4SS.dll` exports 4,239 C++ symbols with `std::function`, `std::vector` and
  `std::string` across the DLL boundary. A mod is bound to one exact UE4SS build. Note this
  collides with `CMakeLists.txt`'s current `/MT` static-CRT choice for the add-on — the glue DLL
  will have to match UE4SS's CRT, whatever that is.
* Releases are effectively rolling: the last tag is `v3.0.1` (2024), and everyone uses the
  `experimental-latest` rolling pre-release.

`mods/StrayDualSense/src/Runtime.hpp:1-6` already records the consequence — an SDK *"we cannot
build against locally"* — and there is still no build file under `mods/` (HARD, checked).

**Design consequence, and it is the important one.** Because the glue is the only part with a
heavy, gated, ABI-fragile build, **the D3D12 hook layer must not live behind it.** Keep (A) free of
UE4SS types and CI keeps compiling and testing 100% of the interesting code exactly as it does
today; the glue becomes a ~100-line file compiled on the box. If the Epic-membership gate or the
ABI churn proves intolerable, (A) moves to a thin proxy DLL and **nothing else changes** — which is
§6.2's fallback, and the reason the split is worth enforcing from day one.

### 1.8 Overlay — **L if reproduced faithfully; recommend deferring**

See §5.1.

---

## 2. What is already solved

**Measured across `src/` + `tests/` + `shaders/` — 24,623 lines, 83 files:**

| Category | Files | Lines | Share |
|---|---:|---:|---:|
| PURE (no ReShade, no D3D12) | 47 | 7,654 | 31.1% |
| D3D12-ONLY (raw `ID3D12*`, no ReShade) | 18 | 6,503 | 26.4% |
| RESHADE-LIGHT (log/config only) | 3 | 235 | **1.0%** |
| RESHADE-COUPLED | 15 | 10,231 | 41.6% |

**57.5% of the codebase ports with zero edits.** (HARD, counted.)

The whole DLSS stack is already ReShade-free raw D3D12: `ngx_backend.cpp` (996),
`ngx_nr.cpp` (1,513), `ngx_snippet.cpp` (486), `nr_codec_pass.cpp` (697), `mv_resolve.cpp` (680),
`gbuffer_resolve.cpp` (713), `input_dump.cpp` (261), `ext_unhook.cpp` (125),
`d3d12_restore.cpp` (48) — plus every shader and all 23 files in `src/core/`. (HARD.)

And the 41.6% "coupled" figure badly overstates the work, because coupling is not uniform:

* `src/taa_hook.cpp` — **2,288 lines, 17 `reshade::` references.** Ten of them are `get_native()`;
  the rest are `map_buffer_region` / `unmap_buffer_region` (the View CB read at `:892-898`) and one
  `get_resource_desc`. Everything below that is raw D3D12. This file is ~99% portable. (HARD.)
* `src/addon.cpp` — 2,043 lines and 271 references, but **all 26 event registrations sit in one
  function** (`register_events()`, `:1714-1914`) with a symmetric unregister block. This is the
  file the migration rewrites; it is one file. (HARD.)
* `pass_finder.cpp` (431/19) and `gbuffer_finder.cpp` (760/8) need only
  `command_list` / `device` / `resource_view` handles — near-trivial to re-type. (HARD.)

**Also already solved, and worth noting explicitly:**

* `src/d3d12_restore.{hpp,cpp}` already separates *observing* state (needs a framework) from
  *replaying* it (pure D3D12), with a comment saying exactly that, and the replay half is already
  WARP-tested in CI. The migration only has to rewrite the observing half. (HARD.)
* `src/reshade_all.hpp` is a 13-line single front door for the entire dependency. There is exactly
  one place to cut. (HARD.)
* The NGX handle discipline — "pass plain `ID3D12Resource*` and nothing else", CLAUDE.md §1 — is
  already the rule, so nothing in the NGX path assumes a framework.

---

## 3. What gets better

This is the strongest part of the case, and most of it is HARD rather than speculative.

### 3.1 The entire §1 device/vtable hazard class disappears — HARD

`docs/UPSTREAM-RESHADE-EXTHOOK.md:26-53` gives two independent reasons a real vkd3d handle reaches
`convert_to_original_cpu_descriptor_handle`, and **both are properties of ReShade being in the
process at all**, not of us using its API:

1. vkd3d's `ID3D12DeviceExt` vtable is static, so ReShade's patch — installed from *any*
   `QueryInterface` on its proxy, including the game's own (measured in the live game: clean at
   `init_device`, `HOOKED BY RESHADE` by frame 300, CLAUDE.md §1) — reaches interfaces taken
   straight from the original device.
2. ReShade registers its proxy on the **original** device via
   `SetPrivateData(__uuidof(D3D12Device), …)` (`d3d12_device.cpp:34`), so the hook's proxy lookup
   succeeds even for a caller that never touched the proxy.

Remove ReShade and neither exists. That deletes, in one move:

* `src/ext_unhook.{hpp,cpp}` (164 lines) and its 8 `repair()` call sites across `ngx_backend.cpp`
  and `ngx_nr.cpp`;
* the **race** inherent in that repair — a single `QueryInterface` on any thread reinstalls the
  patch between our repair and NGX's use, and nothing can close that window;
* the practice of mutating a process-global vtable that other components may depend on;
* CLAUDE.md §1's whole three-row truth table, and with it the `NgxDevice` knob, the frame-120
  decision logic, and `kNgxDecisionDeadline`. **There is only one device.**

**Caveat, stated plainly: this win is only realised if ReShade actually leaves the process.** If
the user keeps ReShade loaded for its effects, the patch returns and `ext_unhook` must stay.

### 3.2 The PSO-cache side effect disappears, so shader hashing becomes free — HARD

`docs/RESEARCH.md:246-250`: merely registering the pipeline events makes ReShade rebuild the
subobject stream and **deliberately drop the `CachedPSO` blob**. The add-on currently warns
*"EXPECT A VERY SLOW FIRST LOAD"* (`src/addon.cpp:1770`) and gates hashing behind `HashShaders`.

Our own `CreateComputePipelineState` hook reads `pDesc->CS.pShaderBytecode` and forwards the
**caller's unmodified desc**. Zero side effect, by construction. Consequences: hashing is always on;
the `HashShaders` / `PassFinder` / `GBufferFinder` registration gymnastics in `register_events()`
collapse; and the standing tension between "identify by hash" and "identify structurally because
hashing is expensive" simply ends.

### 3.3 Root-argument capture becomes complete instead of reconstructed — HARD

`src/frame_state.hpp:100-113` is explicit that ReShade's `state_tracking` registers no
`push_descriptors` and no `push_constants` handler, so *"apply() can never replay the game's ROOT
descriptors, and UE4's D3D12 RHI binds its uniform buffers as root CBVs. Restoring with apply()
alone leaves the game's own TAA dispatch running with undefined root arguments every frame we
inject, which is the visible corruption."* The project had to write its own capture and its own
native replay to work around it.

Hooking `SetComputeRoot*` directly gives the exact argument list the game set, in order. The
restore stops being a reconstruction and becomes a replay of observed calls. `src/frame_state.hpp:79-86`
records a second workaround of the same shape (ReShade's `descriptor_tracking` keeps a dangling
pointer for `descriptor_table_with_flags`, *"which is the variant UE4 uses"*, so its descriptor walk
*"silently visits nothing"*). Owning the root-signature deserialization removes that too.

### 3.4 Ordering is ours — HARD

Three ordering traps in CLAUDE.md §5 exist only because ReShade owns the frame:

* `reshade_begin_effects` **never fires with an empty preset** (`runtime.cpp:737`), so
  `NgxNRHook=present` is silently inert unless an effect file is loaded.
* `flush_immediate_command_list()` early-outs on `!_has_commands`, which only ReShade's own
  `command_list` API sets — the sole reason `nr_history.cpp` routes barriers through the proxy.
* The `preui` hook site has to guess a back-buffer bind ordinal (`NgxNRPreUiBind`, default 2)
  because the frame boundary is not ours to define.

Owning `Present` and `OMSetRenderTargets` makes all three questions of our own scheduling.

### 3.5 The build-compatibility gate class disappears — HARD

Gone: `RESHADE_API_VERSION == 20` pinning; the `IMGUI_VERSION_NUM != 19250` `#error` and the
"include `imgui.h` before `reshade.hpp` or the whole TU silently breaks" trap
(`src/reshade_all.hpp:1-9`); the `RESHADE_ADDON >= 2` full-build gate and the startup assertion
written specifically because *"wrong ReShade build" and "UE4 binds differently" are otherwise
indistinguishable from a log* (`docs/RESEARCH.md:173-185`); `register_addon` returning `false`
silently; and 8,322 lines of vendored SDK headers plus 4,594 lines of vendored imgui.

Also removed as a crash suspect: ReShade's effect runtime, which CLAUDE.md §1 still lists under
*"Still untried: whether ReShade's own effect runtime or another add-on is what dies."*

### 3.6 Measurement gets dramatically sharper — SOFT, and this may be the biggest win

Read CLAUDE.md's measurement history as a single story and one thing dominates it: **almost every
wrong conclusion this project has drawn came from a noisy instrument, not from a wrong theory.**
The `0x8978e4e6431cacb3` negative (n=44, medians within 1%) could only resolve a ~27% difference.
The candidate-set result needed n=32 and three separate confound removals to move a signal from 47%
to 111%. The cyan-pixel test spanned 0→2134 *in the control alone*.

Two capabilities the migration hands us attack that directly:

* **Deterministic capture (from owning `Present`).** Capture frame N and frame N+1 back to back, on
  command, at a known frame index. The project's own best metric is *"comparing a frame against
  ITSELF a moment later, camera still"* — that is exactly what consecutive-frame capture is, and it
  is currently approximated by injecting `KEY_SYSRQ` and hoping (`tools/screenshot-stray.sh`,
  CLAUDE.md §2.11). It also removes the ALT-PHASE timestamp-correlation machinery: we can capture
  *at* the phase transition instead of inferring which phase a PNG belongs to.
* **Deterministic camera (from UE4SS).** See §6.1.

Together these convert the noisiest experiments in the project into repeatable ones. That is worth
more than any single rendering fix on the list.

### 3.7 Config stops fighting us — HARD

Two documented ReShade-ini defects go away: the parser **cannot deliver a comma-separated value to
an add-on** (returns element 0 only), which forced the `stray-dlss-dryrun.txt` /
`stray-dlss-hashes.txt` sidecar files; and rewriting `ReShade.ini` races ReShade's exit flush, which
silently reverts fresh edits. (CLAUDE.md §5, HARD.) A hot-reloading INI we own — which
`mods/StrayDualSense/src/Config.cpp` already implements — fixes both and adds live re-tuning without
a relaunch, which partly compensates for losing the overlay (§5.1).

---

## 4. What is lost, and the replacement for each

| Lost | Replacement | Verdict |
|---|---|---|
| **ImGui overlay** (`register_overlay` ×2, ~195 lines, 65 `ImGui::` calls) | Hot-reload INI + the existing `stray-dlss-status.txt` heartbeat (`addon.cpp:990-1000`) | **Downgrade, partly compensated.** See §5.1 |
| **`get/set_config_value`** (55 keys, 79 sites) | `mods/StrayDualSense/src/Config.{hpp,cpp}`, already written | **Upgrade** (§3.7) |
| **Screenshot channel** (`KEY_SYSRQ` → ReShade PNG) | Our own back-buffer readback at `Present` | **Upgrade** (§5.3), but sequencing-critical |
| **`descriptor_tracking` + `state_tracking`** (533 lines, free) | §1.1, ~1,000 lines of ours | **Cost.** The real work |
| **Real-ReShade CI lane** | Hook-layer-on-WARP lane | **Sideways**, arguably an upgrade (§5.2) |
| **`reshade::log::message`** | `mods/StrayDualSense/src/Log.{hpp,cpp}`, already written | Neutral |
| **Cross-API abstraction** (D3D11/OpenGL/Vulkan) | None | **No loss.** Target is D3D12-only, forever |
| **ReShade's effects for the user** | Run ReShade alongside if wanted | **Real loss** — and doing so reinstates §3.1's hazard |

## 5. The three regressions, in detail

### 5.1 The overlay is the one genuine regression

`register_overlay(nullptr, draw_status)` + `register_overlay("OSD", draw_osd)`
(`src/addon.cpp:1912-1913`) are load-bearing in a way the file list understates. CLAUDE.md's
`NgxNRRestoreHistory` section says it explicitly: *"the checkbox is live so the A/B happens inside
one session"* — and given §3.6, within-session A/B is the project's most reliable experimental
form.

Three options, in increasing cost:

1. **Hot-reload INI + status file.** The A/B still happens within one session (edit a value over
   ssh, it takes effect in ≤1 s), and it is *scriptable*, which the checkbox is not — an automated
   alternating harness can drive it. Loses the at-a-glance OSD. **Recommended for stages 1–4.**
2. **UE4SS's own ImGui GUI.** It does expose mod tabs — `register_tab(name, RenderFunctionType)`
   where `RenderFunctionType` is a **raw function pointer** (`GUI/GUITab.hpp:20`, so no capturing
   lambdas), plus the `UE4SS_ENABLE_IMGUI()` macro that rebinds the mod's statically-linked ImGui
   to UE4SS's context (`UE4SSProgram.hpp:27-42` — note it is a **busy-spin with no sleep and no
   timeout**). **But it renders into UE4SS's own separate OS window, on its own D3D11 or OpenGL
   swapchain** (`GUI/DX11.cpp:73-117`) — not into the game's frame. (HARD.) Under gamescope's
   nested compositor, driving the game full-screen on the DRM backend, a second toplevel window is
   at best awkward and quite possibly invisible; CLAUDE.md §6 records the analogous trap where a
   Wine window stole gamescope's focus and looked exactly like a game hang. **Do not count on this
   as the overlay replacement.**
3. **Our own ImGui on D3D12** at `Present`. Entirely doable once we own `Present`, but it is a font
   atlas, an SRV heap, a render pass and input plumbing — ~600 lines and a new class of state bugs
   in the one place we most need to not have them. **Defer.**

### 5.2 The CI lanes

Today (HARD, `.github/workflows/ci.yml`): Unit (Linux, doctest, `stray_dlss_core` only) · WARP
(Windows, real D3D12 + debug layer + GPU-based validation) · **Real ReShade** (downloads
`ReShade_Setup_6.8.0_Addon.exe`, extracts it as `d3d12.dll`, runs `stray_dlss_warp.exe
--expect-reshade` so every harness test executes through ReShade's proxies) · Hardware (by hand,
`tools/run-harness-proton.sh`).

Note there is **no separate "real ReShade" CMake target** — it is the WARP harness binary with a
flag (`warp_mv_resolve.cpp:1511`). (HARD.)

After the migration:

* **Unit and WARP lanes are unaffected** — neither links ReShade for anything load-bearing
  (`stray_dlss_tests` links only `stray_dlss_core` + doctest; the WARP harness's ReShade usage is
  the `fake_reshade_command_list.hpp` shim, 141 lines, which disappears along with the abstraction
  it fakes).
* **The real-ReShade lane is replaced by a strictly more relevant one:** install our hooks on a
  WARP device, run a synthetic "UE4-shaped" frame (create a root signature with tables, create
  views into an offline heap, `CopyDescriptors` into an online heap, bind, dispatch) and assert
  that the shadow reconstructs the exact registers, that `Dispatch` suppression works, and that the
  debug layer reports nothing. **That tests the code we are actually writing**, where the current
  lane tests that we load into someone else's DLL.
* **What is genuinely lost:** the "our D3D12 usage survives a real third-party proxy" evidence, and
  the `test_vkd3d_ext_hook_reachability` test (`warp_mv_resolve.cpp:1347`), which becomes moot.
  Keep the ReShade lane running for as long as the ReShade backend exists (§9 stage 1–3) so the
  loss is deferred, not immediate.

### 5.3 The screenshot channel — replace it BEFORE removing ReShade

CLAUDE.md §2.11 and §5 both make this the project's primary visual-verification tool, and §5
records it saving the project twice in one session. Losing it mid-migration would be the single
most costly sequencing mistake available.

Replacement, once we own `Present`: on a trigger, copy the back buffer to a `READBACK` heap, fence,
map, write. The existing `src/input_dump.cpp` (261 lines) already does exactly this readback
pattern for NGX inputs — *"copy on the game's list, map 5 presents later"*, measured working under
vkd3d (CLAUDE.md §5, HARD) — so this is mostly a re-point, not new machinery. Trigger by a command
file in the game directory, which is the IPC pattern `docs/STRAY-DUALSENSE.md:426-435` already uses
on this box, and which removes the `KEY_SYSRQ` injection and its "find the keyboard node whose
`Handlers` include `sysrq`" fragility entirely.

Encoding: write raw + a `.json` sidecar and convert host-side with the existing
`tools/rawdump2png.py`, rather than linking a PNG encoder into the game process.

---

## 6. UE4SS: where it genuinely helps, and where it does not

### 6.0 What UE4SS does not do — settled

**UE4SS hooks no renderer, at all.** Its GUI backends are exactly `DX11` and `GLFW3_OpenGL3`
(`UE4SS/include/GUI/GUI.hpp:19-23`); it creates its **own window and its own D3D11 swapchain**
(`GUI/DX11.cpp:73-117`, `sd.OutputWindow = hWnd`) and presents to that. Repo-wide grep finds zero
hits for `kiero` or `CreateSwapChain`; the shipped `UE4SS.dll` imports no `dxgi.dll` and no
`d3d12.dll`, and contains zero `dxgi` / `Present` strings and zero D3D12-related exports among its
4,239. It exposes no `ID3D12Device`, no command queue and no swapchain to mods. (HARD, verified at
both source and binary level.)

So **every line of piece (A) is new work.** That was assumed at the outset; it is now established.

### 6.1 Does UE4SS help the DLSS work? Yes — but not where CLAUDE.md §2.6 suggests

**It does NOT replace the View constant-buffer read, and this is now HARD rather than reasoned.**
A grep of the whole UE4SS tree returns **zero** hits for `FViewUniformShaderParameters`, `FViewInfo`,
`FSceneRenderer`, `FRDGBuilder`, `RHICmdList`, `ViewMatrices` and `TemporalAA`; the only
`FSceneView` hits are `;`-comments inside vtable-layout `.ini` files, and the only tree-wide
non-comment hit is `FSceneViewStateReference` appearing in a *dump-exclusion* list. The shipped
binary contains none of those strings and exports none of those types. The renderer is plain
non-`UObject` C++, so `FindObject` / `GetValuePtrByPropertyName` / UFunction hooking are
structurally incapable of reaching it.

There is no reflective path to row 122 `ClipToPrevClip` or row 152 `TemporalAAParams`. **The
byte-offset read at `src/taa_hook.cpp:892` stays exactly as it is.**

That is fine, because §2.6 is already the *strongest*-evidenced part of the project: row 135
validates itself from a single read (`y*z == 1.0` by construction, `x` a denormal int32, `w` exactly
0.0 — measured 2026-09-01, HARD). It does not need help.

**Where UE4SS is genuinely valuable, in descending order:**

1. **Deterministic camera pose for measurement — the big one.** (SOFT.) `APlayerController` /
   `APlayerCameraManager` are `UObject`s with reflected properties and callable `UFunction`s
   (`SetControlRotation`, `SetActorLocation`). CLAUDE.md's measurement sections repeatedly name the
   camera as the dominant confound: *"a moving camera makes runs incomparable"*, *"a pair caught
   while the camera is still settling differs enormously whatever the TAA is doing"*, and the
   3-state lighting cycle that *"turned out to be the camera rotating between captures."* Setting an
   exact pose, waiting K frames for convergence, then capturing frames N and N+1 turns the project's
   noisiest measurement into a repeatable benchmark. Combined with §3.6's deterministic capture,
   this is the highest-value item in the whole migration.
2. **Live cvars — write-only, and that is enough.** `UObject::ProcessConsoleExec` is exported
   (`bool ProcessConsoleExec(const wchar_t*, FOutputDevice&, UObject*)`), as are
   `ULocalPlayer::ExecInternal` and the ordinary `UKismetSystemLibrary:ExecuteConsoleCommand` /
   `APlayerController:ConsoleCommand` UFunctions. (HARD.) So `r.ScreenPercentage`,
   `r.TemporalAA.Upsampling`, `r.RayTracing`, `r.SSR.Quality`, `r.EyeAdaptationQuality` become
   **within-session A/B variables** instead of `Engine.ini` edits plus a relaunch (CLAUDE.md §2.2:
   *"`Engine.ini` settings take effect. Command-line arguments do not."*). Given that
   `r.RayTracing=False` nearly tripled the frame rate and that "RR without RT" is an open
   experiment, that is directly useful.

   **Two caveats.** UE4SS has **no CVar API at all** — zero hits for `IConsoleManager`,
   `IConsoleVariable` or `CVar` in the source, zero matching exports in the binary, and its
   `ConsoleManagerSingleton` scan is documented as *"currently unused and a WIP"*
   (`assets/Changelog.md:68-69`). So you can *set* by command string but cannot *read* a cvar's
   value back typed; verify by observing the effect, not by querying. (HARD.) And `ECVF_ReadOnly`
   cvars such as `r.UsePreExposure` still cannot be changed at runtime. (SOFT.)
3. **An authoritative camera-cut signal.** §2.8 currently ORs three heuristics because
   *"`bCameraCut = !InputHistory.IsValid() || View.bCameraCut`, and the history-invalid case never
   reaches the View buffer."* `APlayerCameraManager::bGameCameraCutThisFrame` is a reflected
   transient property in UE4. Reading it would add a fourth, independent, non-heuristic signal.
   (SOFT — I have not verified it is a `UPROPERTY` in 4.27.2 specifically; cheap to check with a
   UE4SS object dump.)
4. **Authoritative resolution.** `UGameUserSettings` is a `UObject`; `ScreenPercentage`,
   `ResolutionSizeX/Y` are reflected. Today these are inferred from dispatch sizes and read out of
   `GameUserSettings.ini` host-side (§2.3.1). Reading them in-process removes a whole inference
   step from the DLSS create decision, and would have caught the *"DLSS pin could outlive its pass"*
   bug (CLAUDE.md §5) directly rather than via a 300-present timeout. (SOFT.)
5. **Frame-accurate game-state markers** — menu vs loading vs gameplay, currently inferred from the
   depth histogram (§2.4). A `UWorld`/`GameMode` check is exact. Would have prevented the
   `NR CODEC LUMINANCE` measurement that fired on a black loading frame (CLAUDE.md §5). (SOFT.)

6. **A real game-thread, per-frame callback.**
   `Hook::RegisterEngineTickPreCallback` / `PostCallback` hooks `UEngine::Tick` and is on by default
   (`UE4SS-settings.ini:217`); `RegisterGameViewportClientTickPostCallback` is the same on
   `UGameViewportClient::Tick`. (HARD, both exported.) The add-on has no clean equivalent today —
   its frame boundary is `Present`, which is a *render*-side event.

**Net: real, and concentrated in measurement and automation rather than in the rendering path.**
None of it makes the DLSS injection itself work better; all of it makes finding out whether it
works cheaper and less error-prone. Given that this project's stated bottleneck is *"a round-trip is
expensive"* (§0.1), that is the right place for a win.

> **Defect found in the sibling mod while researching this — worth fixing there.**
> `mods/StrayDualSense/src/Runtime.hpp:31-32` says `Tick()` is *"Called from UE4SS's on_update, i.e.
> the game thread."* **`on_update` is not the game thread.** It runs on UE4SS's own dedicated
> `std::jthread` (named `UE4SS-UpdateThread`) in a loop ending
> `std::this_thread::sleep_for(5ms)` — so ~200 Hz, unsynchronised with frames or the game thread
> (`UE4SSProgram.cpp:1205, 1322-1341`, HARD). `Log.hpp:8-10`'s caution that forwarding to UE4SS's
> log *"on the thread UE4SS itself calls us on"* is therefore also resting on a wrong premise.
> For game-thread per-frame work, use `Hook::RegisterEngineTickPreCallback`. The same mistake in
> the DLSS glue would be much more dangerous, since anything touching D3D12 state off the render
> thread is a data race.

### 6.2 The one hard timing question — better than expected, but not settled

**Correction to the obvious assumption: UE4SS loads C++ mods VERY early — before its own AOB
scanning and before `on_unreal_init`.** The chain, all HARD from the UE4SS source:

`dwmapi` proxy `DllMain` → `QueueUserAPC(process_initialized, main_thread)`
(`main_ue4ss_rewritten.cpp:118-132`, deliberately avoiding the loader lock) → on the **game's main
thread**, the `UE4SSProgram` constructor runs `install_cpp_mods()` → `start_cpp_mods()`, which
`LoadLibraryExW`s our DLL (so **our `DllMain` runs**) and then calls **`start_mod()`** — all at
`UE4SSProgram.cpp:386-420`, **before `setup_unreal()`**. `on_unreal_init` fires later, after the
scan, on a different thread.

So a C++ mod gets a main-thread entry point at roughly the same stage a proxy DLL would. Plain
Win32 and D3D12 work is legal there; only the `Unreal` namespace is off-limits.

**What is NOT settled, and it is the crux: `QueueUserAPC` only runs when the target thread enters an
alertable wait.** Nothing guarantees that happens before the game creates its D3D12 device. So the
mod's entry point is *early*, but its position relative to `D3D12CreateDevice` is **UNCONFIRMED**
and is a property of Stray's startup, not of UE4SS.

Two smaller facts that bear on it (HARD): `on_dll_load` is an IAT hook on the main executable's
`LoadLibrary*` thunks, so it **never fires for DLLs already loaded when UE4SS attaches** — which for
a UE4 monolithic shipping build very likely includes `d3d12.dll` and `dxgi.dll` as static imports.
And UE4SS's main-thread stall (`s_wait_for_ue4ss`) is inert by default, because `DoEarlyScan`
defaults to 0.

Consequences, split:

* **Dispatch interception is unaffected.** A vtable patch on a *shared static* vtable takes effect
  immediately for every existing and future command list. Late attach costs nothing here. (SOFT,
  contingent on the static-vtable check in §1.2.)
* **The descriptor shadow is affected.** Any `CreateShaderResourceView` that happened before we
  attached is invisible, and a later `CopyDescriptors` from that offline slot resolves to nothing.

**Why this is probably survivable, and how to find out in one run.** UE4's scene render targets and
their views are reallocated on resolution change and across the menu→gameplay transition, so the
views we actually care about (SceneColor, SceneDepth, Velocity, History) are very likely created
*after* a mod that attached during the menu. The add-on already defers NGX init to frame 120 and
tolerates a warmup, so late attach is consistent with existing behaviour. **The instrument that
settles it is one counter:** at each matched TAA dispatch, log how many table slots resolved as
`unknown`. Zero unknowns over a gameplay session = the shadow is complete and the timing question
is closed. (SOFT.)

**If it turns out not to be survivable**, the fallback is not exotic: install the hook layer from a
thin proxy DLL that loads at process start (taking the `dxgi.dll` override slot ReShade currently
occupies, or `d3d12.dll`), and keep the UE4SS mod for §6.1 only. That is precisely the two-component
architecture `docs/STRAY-DUALSENSE.md:426-435` already ships on this box, so it is a known-good
shape rather than a new risk. It costs one extra artifact and an IPC channel; it does not cost any
of §3's wins, because those come from *removing ReShade*, not from being a UE4SS mod.

---

## 7. Frame generation

The instinct that this route "would work better for FG as well" is **supported — but not by the
argument that looks strongest.** The crash-signature resemblance does not survive arithmetic. The
proxy-stacking argument does, and it is the one to build on.

### 7.1 Is the crash signature genuinely §1's class? Test it, don't eyeball it

CLAUDE.md §1 records the vkd3d crash registers as `rbx=rcx=8146001500000004` and
`rdx=rsi=0048002500000001`; the FG crash is `EXCEPTION_ACCESS_VIOLATION writing address
0x000000020000000d`. All three "look bit-packed". That is where the similarity ends.

ReShade's CPU descriptor handle encoding is documented and exact —
`heap_index = (ptr >> 28) & 0xFFFFFFF`, `offset = ptr & 0x0FFFFFF8`
(`docs/UPSTREAM-RESHADE-EXTHOOK.md:72-75`; `heap_index_start = 28` on 64-bit confirmed by that
document's own CI at `:258`). Decoding all three:

| Value | `heap_index` | `offset` | Plausible? |
|---|---:|---|---|
| FG crash `0x000000020000000d` | **32** | `0x00000008` | **yes** |
| §1 `0x8146001500000004` | 73,400,656 | `0x00000000` | no |
| §1 `0x0048002500000001` | 75,498,064 | `0x00000000` | no |

**So the two do NOT share an encoding, and "same class as §1" is not established.** (HARD — this is
arithmetic on numbers both documents record.) Two corrections follow, and the second matters more:

* CLAUDE.md §1's own characterisation of its registers as "a descriptor handle reaching vkd3d
  unconverted" is an *interpretation*. The values are handle-*shaped* — small index in the low
  dword, tag-like bits high — but they are not ReShade CPU descriptor handles in the documented
  format. They may be GPU descriptor handles (a different encoding), a register pair, or something
  else. The measured fact is the crash site and the registers; the identification is inference.
* The FG address independently **does** fit ReShade's format, as heap 32 / descriptor byte-offset 8.
  That is a *separate* argument pointing at ReShade, and it does not depend on §1 at all.

**Do not over-read that fit.** A 28-bit field will "accept" almost any small value; heap 32 and
offset 8 are unremarkable numbers. And the fault is a **write**, whereas §1's mechanism is a
*dereference for read* of a bit-packed integer. Grade: **suggestive, not diagnostic.** Worth one
launch to test (§7.5), not worth a redesign.

### 7.2 The mechanistic link that IS strong: DLSS-G travels the same cubin path — CONFIRMED

**`nvngx_dlssg.dll` 310.x calls `CreateCubinComputeShaderExV2`,
`GetCudaMergedTextureSamplerObject`, `GetCudaIndependentDescriptorObject` and `CaptureUAVInfo`** —
verified in DXVK-NVAPI issue logs, including NVIDIA's own Liam Middlebrook debugging an
`nvngx_dlssg.dll` stack through that exact path. (HARD, external.) Two of those are precisely the
`ID3D12DeviceExt` entry points ReShade patches and CLAUDE.md §1 already caught failing for us with
`nvapi status -5`.

**So the §1 hazard applies to frame generation identically, and this is no longer an inference.**
Same slots, same conversion, same silent-corruption failure mode. ReShade's mere presence is a
first-order hazard for DLSS-G, independent of anything about swapchains.

Also confirmed from NVIDIA's own header (`nvapi_lite_common.h`): **`-5` is
`NVAPI_INVALID_ARGUMENT`**, which corroborates §1's mangled-descriptor-handle diagnosis from the
vendor's enum rather than from our inference. And **`-3` is `NVAPI_NO_IMPLEMENTATION`**, which
reframes the Reflex finding: `setReflexTiming` failing meant `VK_NV_low_latency2` was reported
unavailable, **not** that the call was malformed — so `fakenvapi` was substituting for a failed
capability query. On a 4090 with driver 610 that query should succeed, which makes the `fakenvapi`
dependency itself suspicious and worth re-testing rather than inheriting.

**A correction CLAUDE.md should absorb: "DLSS-G needs optical flow" is true only for DLSS 3.**
DLSS 4 FG (310.x) is a transformer model on CUDA cubins and uses no optical flow at all — hence the
different prerequisites (vkd3d-proton ≥ 3.0.1 for `supports_cubin_64bit` /
`vkGetImageViewHandle64NVX`, DXVK-NVAPI ≥ 0.9.2 for 64-bit cubin call passthrough, driver R570+).
**The target box meets every one of them, several at exactly the minimum**: vkd3d-proton 3.1.0,
DXVK-NVAPI 0.9.2, driver 610.43.02, Ada. (HARD.)

**That matters more than it first appears: it removes "the platform cannot do this" from the
hypothesis list.** If FG fails here it fails for an integration reason.

**What this does NOT settle** is the direction of the effect. `ext_unhook` un-patches those slots
from eight call sites before every NGX call we make (`ngx_backend.cpp:317, 619, 716, 801, 904`;
`ngx_nr.cpp:416, 999`), so the vtable is *toggling* during FG sessions at our frame cadence. If
DLSS-G runs on the native device it wants them unpatched, as we do; if Streamline was initialised
through ReShade's proxy — and CLAUDE.md's own OptiScaler log says
**`hkD3D12CreateDevice Caller: ReShade64.dll`** — it wants them patched, and our repair is actively
breaking it. Either way, **a vtable whose state depends on another component's frame cadence is not
a sound foundation**, and only removing ReShade makes the question disappear rather than merely
picking a side.

### 7.3 The best-supported explanation is NOT ReShade — and it is testable today, for free

**DLSS-G must own the swapchain, and this is structural.** NVIDIA's `ProgrammingGuideDLSS_G.md`
§18.0, verbatim: *"When DLSS-G is loaded it will create an extra graphics command queue used to
present frames asynchronously and in addition it will force the host application to render
off-screen (host has no access to the swap-chain buffers directly)."* And §12.0: *"DLSS-G takes over
frame presenting … When turned off DLSS-G will call SwapChain::Present on the same thread as the
host application which is not the case when DLSS-G is turned on."* (HARD, NVIDIA's own guide.)
There is no mode in which FG interpolates on a swapchain it does not own.

**And NVIDIA documents the exact operation that is crashing.** §12.0, verbatim: *"when host is
modifying resolution, full-screen vs windowed mode or performing any other operation that could
cause SwapChain::Present call to generate a deadlock DLSS-G must be turned off by the host using the
`sl::DLSSGConsts::mode` field."* Restated in §17: *"Turn DLSS-G off before any window manipulation
(resize, maximize/minimize, full-screen transition etc.) to avoid potential deadlocks or
instability."* (HARD.)

Nothing in the current setup turns FG off across Stray's `SetFullscreenState(TRUE)`. That
reconfigures the swapchain underneath Streamline's asynchronous present thread and its extra command
queue, both of which still hold back-buffer references — and OptiScaler's own
`wrapped_swapchain.cpp:675-692` carries the comment *"dlssg calls this from present it seems / don't
try to get a mutex when present owns it while dlssg mod is enabled"*, i.e. DLSS-G really does drive
`SetFullscreenState` from inside its present path. (HARD.)

**This fits the evidence better than any proxy-stacking or descriptor story:**

* It explains the **determinism** and the ~1 s delay after the transition.
* It explains why the death frame **varies with configuration** (11 FSR-FG / 32 DLSSG / 36 with
  Reflex) — different pipeline depths reaching the same wall — where a descriptor bug would fire on
  the *first* evaluate, not a fixed number of frames after a mode change.
* The adjacent known failure is the same shape: NVIDIA's dev forum has UE 5.6 D3D12 crashing in
  `ResizeBuffers` with `E_PENDING` when the DLSS-G swapchain provider is installed, and OptiScaler
  issue #898 shows *"Back buffers have outstanding references"* with `Caller: sl.interposer.dll`.
  Our `ResizeBuffers` *succeeded* — so we are past that gate, and `SetFullscreenState` is the very
  next reconfiguration.

**Correspondingly, §7.2's ext-vtable route is demoted to a contributing hazard, not the cause.**
That fault is a **read**-path failure on the CUDA texture/surface entry points returning
`NVAPI_INVALID_ARGUMENT (-5)`; ours is a **write** AV inside the FG path. Same class, different
instance. `ext_unhook` must nevertheless stay **ON** for any FG attempt, because DLSS 4 FG travels
those same entry points (§7.2).

**The cheapest disproof costs one launch and no code: force borderless/windowed so
`SetFullscreenState(TRUE)` never happens.** If the deterministic death disappears, §12.0
non-compliance is confirmed and the fix is to toggle `DLSSGConsts::mode` around window changes —
which requires owning the FG integration, not removing ReShade. **Run this before writing any
Streamline code and before using FG to justify anything.**

### 7.4 A hard blocker for FG that has nothing to do with ReShade

`sl_dlss_g.h` defines `eFailGetCurrentBackBufferIndexNotCalled = 1<<4` — *"D3D integrations must use
SwapChain::GetCurrentBackBufferIndex API"*. **UE 4.27's D3D12 RHI never calls it.**
`D3D12Viewport.cpp:872-875` increments its own counter (`CurrentBackBufferIndex_RHIThread++; … %
NumBackBuffers;`), and a search across the UE 4.27 tree returns **zero** hits for
`GetCurrentBackBufferIndex`. (HARD.) This is consistent with NVIDIA shipping a *separate UE 4.27
branch* requiring engine source modifications for the DLSS-G plugin; native support begins at UE 5.2
via `IDXGISwapChainProvider`.

**Consequence: an injector must supply that patch's effect itself — own an `IDXGISwapChain4` wrapper
and drive `GetCurrentBackBufferIndex`.** Which is precisely why OptiScaler has
`wrapped_swapchain.{h,cpp}` at all.

So the swapchain wrapper is **not optional and not removable**. FG for this title requires exactly
one component to substitute the swapchain, and that component is either OptiScaler or us. This is
the single place where §1.2's *never substitute an object* invariant has to be broken — and the
right conclusion is that it should be broken **once, deliberately, by a component that knows it is
doing it**, rather than accidentally by three stacked proxies that do not know about each other.

Also confirmed while establishing the above (HARD, from the Streamline SDK):

* **Reflex is mandatory** — §8.0: *"It is required for sl.reflex to be integrated in the host
  application… any existing regular Reflex SDK integration (not using Streamline) cannot be used by
  DLSS-G."* But it **fails loudly, never crashes**: `eFailReflexNotDetectedAtRuntime`, and any
  non-`eOk` status makes DLSS-G *"add pink overlay to the final color image"*. So `slDLSSGGetState`
  polling is a cheap decisive diagnostic and a pink screen is a free indicator. Reflex is therefore
  **not** a crash suspect, which retires it properly rather than by the observed 4-frame shift.
* **DLSS-G needs no `slEvaluateFeature`** — *"DLSS-G is a unique case since it already has the
  marker provided by the existing API (SwapChain::Present)"*. Present **is** the evaluate.
* **HUD-less colour is strongly recommended, not required.** Tagging the post-UI back buffer is not
  an error; it degrades quality (wobbling HUD text) rather than failing. So it is not a blocker,
  and `nr_hook.cpp`'s existing back-buffer bind-ordinal machinery is the right groundwork for it.
* **`slProxyToNative` does not exist** — the SDK has `slUpgradeInterface` (native → proxy, in place,
  *"NOT thread safe and should be called IMMEDIATELY after base interface is created"*) and
  `slGetNativeInterface` (the inverse). Upgrading an already-created swapchain is documented only
  for multi-swapchain disambiguation, **not** as a way to retro-attach FG.
* **Streamline supports exactly one swapchain**, and logs when it skips hooks on a second.
* `PreferenceFlags::eUseDXGIFactoryProxy` exists specifically for *"3rd party overlays which are NOT
  integrated with the host application but rather operate via injection"* — i.e. NVIDIA anticipates
  our situation and provides a knob for it.

**What we already have for a direct integration:** every constant and both hard-required tags.
`kBufferTypeDepth` and `kBufferTypeMotionVectors` are required; `sl::Constants` wants
`clipToPrevClip` (row-major, jitter-free), pixel-space `jitterOffset`, `mvecScale`, near/far,
`depthInverted` (eTrue for UE4 reversed-Z) and `reset` — **all of which the TAA hook already
computes and validates.** That is the expensive half of an FG integration and it is done.

### 7.5 The silent-substitution class, and what the migration really buys FG

**OptiScaler as an upscaler middleman is removable and should be.** CLAUDE.md records the cost
precisely: *"while OptiScaler owns the upscaler, our carefully built NGX parameter block can be
replaced by FSR2 without anything failing"* — and it was, measured
(`FSR2FeatureDx12_212::EvaluateInternal Dispatch!!` with `Dx12Upscaler=dlss` set). Owning the stack
removes that entire class: nothing sits between our parameter block and NGX.

But note the asymmetry §7.4 forces: we can remove OptiScaler *as an upscaler* while still needing
*something* to wrap the swapchain. Driving Streamline directly means taking on the wrapper,
`GetCurrentBackBufferIndex` synthesis, PCL/Reflex markers and the §12.0 mode toggling ourselves.
That is a real project, not a configuration change.

### 7.6 The experiments, ordered

**Runnable today, zero code — do these first:**

1. **Borderless/windowed, FG on.** Tests §7.3, the best-supported hypothesis. Highest value per
   minute of anything in this document.
2. **`ExtUnhook=0` with FG on.** Tests §7.2's residual. Untried — the ruled-out list covers load
   order, the Vulkan overlay, Reflex and `NgxNR=0`, but not this.
3. **Poll `slDLSSGGetState` / look for the pink overlay.** Turns "did DLSS-G accept our
   configuration" from an inference into a read.
4. **Pin the Proton version.** Proton issue #8692 (open) reports the FG option *vanishing* on
   Proton 10 in several titles while working on 9.0-4. A version drift would look exactly like a
   regression in our own work.

**Runnable only after stage 4:** FG with ReShade absent, OptiScaler still present — the clean
control. It is not runnable today because FG's input comes from OptiScaler intercepting *our* NGX
calls, which come from the add-on, which requires ReShade. The stack is circular, and the migration
is what breaks the circle.

**Revised verdict on FG — and it is a downgrade from where this section started.** The platform is
*not* the problem: vkd3d-proton 3.1.0, DXVK-NVAPI 0.9.2, driver 610.43.02 and Ada meet every DLSS 4
FG prerequisite, several at exactly the minimum (HARD). But **removing ReShade is not the unlock.**
The two things standing between this project and frame generation are §7.3's fullscreen-transition
non-compliance — fixable today, config-only — and §7.4's `GetCurrentBackBufferIndex` gap, which
requires *someone* to wrap the swapchain no matter what happens to ReShade.

What the migration genuinely buys FG is narrower and should be stated as such: **one fewer proxy
holding back-buffer references across reconfiguration** (the exact failure class in §7.3), an end to
a vtable whose patch state toggles at our frame cadence (§7.2), and removal of the silent-FSR2
substitution class (§7.5). Real, but supporting evidence for a migration justified elsewhere —
**not a reason to migrate.** The user's instinct that this route "would work better for FG" is
directionally right and materially overstated; the borderless-window test may well deliver more FG
progress in one launch than the entire migration does.

---

## 8. The biggest problems, ranked

### 8.1 The descriptor/root shadow has no offline oracle

Everything else in this project is either a pure function with golden values or a D3D12 usage
question the debug layer can answer. "Did we correctly reconstruct that UE4 bound the depth SRV at
t2 and the same resource's stencil view at t4" is neither. It can be wrong in a way that produces a
*plausible* wrong answer — the exact failure mode CLAUDE.md §0.2 exists to prevent — and the only
judge is the running game, at one expensive round-trip per attempt.

**This is why §9 stage 2 exists.** Running the new shadow *beside* ReShade's and diffing them per
dispatch turns an unbounded correctness question into a bounded one, on real data, with the
known-good implementation as the oracle. Build the differential harness before trusting the shadow.

### 8.2 Hook-installation timing decides whether 8.1 is even solvable

§6.2. Ranked second only because the fallback (proxy DLL) is known-good on this box, so the
downside is an extra artifact rather than a dead end. **Answer this before writing the shadow**, not
after — it changes where the code has to live.

### 8.3 A process-wide vtable patch is a shared, contested resource

We become the thing `ext_unhook` was written to defend against. Three specific hazards: our own
NGX/resolve passes re-entering our own hooks; foreign command lists (OptiScaler, Streamline)
triggering our filters; and another component patching the same slots. Each has a standard answer
(thread-local guard, PSO-based filtering, never-restore chaining), but they must all be in place
from the first build, because the failure modes are hangs and heisenbugs rather than errors.

There is also a subtler version: **vkd3d's `ID3D12GraphicsCommandList` vtable may not be a single
static one.** If it is per-object or per-version, the whole approach needs a different installation
strategy. Three lines of code answer it; write them first (§1.2).

### 8.4 Sequencing: do not lose the visual channel

§5.3. ReShade currently owns the screenshot path, and the project's own history says a session
without visual verification produces confident wrong conclusions. **Our capture channel must be
working and validated while ReShade is still loaded.**

### 8.5 Do not let frame generation motivate this migration

§7. The FG hypothesis that motivated including this section — proxy stacking causing a
handle-translation fault — **does not survive contact with the sources.** The best-supported cause
is NVIDIA-documented non-compliance at the fullscreen transition (§7.3), testable today for free,
and there is a second blocker (§7.4, `GetCurrentBackBufferIndex`) that requires a swapchain wrapper
regardless of what happens to ReShade.

The risk this poses to the *project* is a planning one: if FG is counted as a deliverable of the
migration, stage 4 will arrive and FG will still be broken, and the migration will read as a
failure when it was not. Book FG's upside as small and indirect, and run §7.6's four zero-code
experiments **before** the migration rather than after.

### 8.6 The UE4SS SDK is not buildable in this development environment

`Runtime.hpp:1-6`, HARD. Mitigated by construction if (A) contains no UE4SS types — but it means
the glue file itself is only compiled on the box or in a CI job that has to fetch the UE4SS tree,
and it will be the one file with no local feedback loop. Keep it under ~150 lines and free of logic.

### 8.7 Loss of the "runs through a real third-party proxy" evidence

§5.2. Small, and deferrable by keeping the ReShade backend alive through stages 1–3.

---

## 9. A staged plan that keeps the project working

The migration does **not** have to be a flag day. The seam already exists in the right place: 16
`get_native()` call sites and one 13-line include header.

**Stage 0 — cheap answers, ~1 day, no architecture.**
Write and run: (a) the static-vtable check (`*(void**)listA == *(void**)listB`) — decides §1.2;
(b) a UE4SS C++ mod that does nothing but log a timestamp and whether an `ID3D12Device` already
exists when `start_mod()` runs — decides §6.2, and doubles as the end-to-end proof that a C++ mod
builds and loads at all; (c) confirm the `SetPrivateDataInterface` destruction callback fires under
vkd3d-proton — decides §1.4. All three are load-bearing and answerable in one or two launches.

**Run §7.6's four zero-code FG experiments in the same sitting.** They are independent of the
migration, they cost nothing, and the borderless-window test may settle FG outright.

**Stage 1 — introduce the seam, ship no behaviour change.**
Define a narrow interception interface (roughly: `on_pipeline_created(hash, handle)`,
`on_pipeline_bound(list, handle)`, `resolve_bindings(list, out)`, `on_dispatch(list,x,y,z) -> bool`,
`on_present(queue, swapchain, backbuffer)`, `restore_state(list)`) and put the ReShade backend
behind it. `taa_hook.cpp`, `nr_hook.cpp`, `pass_finder.cpp`, `gbuffer_finder.cpp` and
`nr_history.cpp` change types, not logic. `addon.cpp` splits into `backend_reshade.cpp` + a
framework-free core. CI unchanged, all four lanes still green, artifact behaviour identical.
**Verification: the user runs it and the log is indistinguishable from the previous build.**

**Stage 2 — build the native backend and run it as a DIFFERENTIAL OBSERVER.**
Install the hooks *while ReShade is still driving*. Both backends resolve every dispatch; only
ReShade's answer is used. Log every disagreement — missing slot, different resource, different
format, unknown descriptor. Add the `unknown-slot` counter from §6.2. This is the highest-value
step in the plan: it validates ~1,000 lines of new code against a known-good oracle, on the real
game, without ever risking the image. Run it until disagreements are zero over a full gameplay
session.

**Stage 3 — flip the TAA hook to the native backend; keep ReShade loaded.**
`ext_unhook` stays on (ReShade is still there). The screenshot channel still works. If anything
regresses, the config flips back within one launch. **Verification: DLSS indicator present, image
matches, `dmesg` clean, frame rate unchanged.**

**Stage 4 — build our capture channel, validate it against ReShade's, THEN remove ReShade.**
Both channels shoot the same frame; compare. Only once ours is trusted, drop ReShade from the DLL
overrides. Delete `ext_unhook`, the `NgxDevice` knob and the frame-120 decision logic. The clean FG
control (§7.6) becomes runnable here — as a diagnostic, not as a deliverable.

**Stage 5 — UE4SS glue and the measurement wins.**
Deterministic camera pose, live cvars, camera-cut and resolution signals (§6.1). Rebuild
`tools/measure-alternating.py` on top of pose-locked, frame-indexed capture. This is where the
round-trip cost that §0.1 calls the project's core constraint actually comes down.

**Stage 6 — overlay, if still wanted.** By this point the hot-reload INI will have been in use for
weeks and the answer will be obvious.

**Rollback exists at every stage through stage 3**, and stage 2 costs nothing but CPU. The only
irreversible step is stage 4, and it is gated on a validated replacement for the one tool the
project cannot work without.
