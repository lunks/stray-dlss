# Streamline internals, read for what our own present-owner/pacer/tagging code should learn

We deliberately do not use Streamline. `src/ngx_fg.cpp` drives NGX feature 11
(`NVSDK_NGX_Feature_FrameGeneration`) directly through the NGX core; `src/backend_native/present_owner.*`
and `fg_present.cpp` are our own swapchain hook, present-twice path, phase-locked pacer and
resource-tagging equivalent — everything Streamline would otherwise provide, hand-built. That was
not a default; it was forced. `CLAUDE.md`'s OptiScaler section records a measured result:
Streamline's own swapchain path, loaded as `sl.interposer.dll`, killed frame generation on this
target deterministically — FG frame 11 (FSR-FG output), 32 (DLSSG output), 36 (DLSSG + Reflex,
and again identically with our own NR path disabled) — across six launches, always at a fixed
point, never past it. So the question this document answers is not "should we adopt Streamline" —
that has a measured answer, and it is no, on this driver stack. The question is: **Streamline is
the reference implementation of the thing we built ourselves. Read its source. Where is ours
wrong or weaker, and where does its own design explain the crash we already measured?**

Companion documents, not repeated: `docs/RESEARCH-DLSS-UE5-PLUGIN.md` (the UE5 plugin wrapper
around Streamline — this document goes one layer deeper, into the SDK the plugin calls),
`docs/STRAY-RENDERING-FACTS.md` §32 (the raw `nvngx_dlssg.dll` parameter contract, reverse
engineered from strings in the same Streamline release read here), `CLAUDE.md`'s OptiScaler
section (the six-launch death, the ext-vtable-patch mechanism it is measured to share a crash
signature with).

**Provenance labels**, per `CLAUDE.md` §0.5: **HARD** — read directly from the source or docs
quoted; **SOFT** — a claim resting on documentation prose rather than the code that implements
it, or on a different version than the one examined; **UNCONFIRMED** — inferred without a
citation, with a note on what would confirm it.

**Architecture note.** This project is a **UE4SS C++ plugin** (`mods/StrayDLSS`), loaded
in-process ahead of device creation; `src/backend_native/` — including `present_owner.*` and
`fg_present.cpp`, both cited throughout this document — is a plain native D3D12 hook layer
(vtable patches on the game's real device/queue/command-list/swapchain objects) that does not
depend on ReShade's add-on events. A `src/backend_reshade/` backend still exists in the tree but
is not how the project runs today. **This matters specifically for §5 below**: the OptiScaler/
Streamline coexistence experiment `CLAUDE.md`'s OptiScaler section records — the six-launch,
deterministic FG death this document spends most of its diagnostic effort on — was measured
*before* this migration, under the then-standard configuration (OptiScaler as `dxgi.dll`,
ReShade loaded behind it as `ReShade64.dll`; `CLAUDE.md`: "Status: dropped by the user
2026-09-01, box fully reverted to ReShade-as-`dxgi.dll`"). §5.3 reasons about a mechanism
specific to that configuration and says explicitly where its conclusions do and do not carry
forward to the current, ReShade-free, UE4SS-native setup.

---

## 0. Sources examined, and — critically — what is and is not open

**`NVIDIA-RTX/Streamline`, tag `v2.12.0`** (GitHub, public, MIT-style NVIDIA copyright
2022-2023, retrieved 2026-09-03). This is the **same release** `docs/STRAY-RENDERING-FACTS.md`
§32 already used to extract `nvngx_dlssg.dll`'s parameter names — so everything below is directly
cross-checkable against that work, and several places below do exactly that. **HARD** throughout
unless marked.

**What is genuinely open source in this repo**, confirmed by browsing `source/`:

| Component | Open? | Relevance here |
|---|---|---|
| `source/core/sl.interposer/` (DXGI factory/swapchain, D3D12 device/queue/command-list proxies, the export/inline hook engine) | **Yes, full source** | §1, §5 |
| `source/plugins/sl.common/` (resource tagging, DRS, the shared per-frame constants entry point) | **Yes, full source** | §2 |
| `source/plugins/sl.reflex/`, `source/plugins/sl.pcl/` | **Yes, full source** | §4 |
| `source/plugins/sl.dlss/` | **Yes** — but this is a thin wrapper that forwards to the closed `nvngx_dlss.dll` snippet; no SR neural-network or evaluate-decision code here | context only |
| `source/platforms/sl.chi/` (the D3D11/D3D12/Vulkan compute abstraction every plugin calls through, including the raw `NvAPI_D3D*` calls) | **Yes, full source** | §4, §5 |
| **`sl.dlss_g` (Frame Generation itself)** | **No.** Not present anywhere in `source/plugins/`. Distributed only as the closed `sl.dlss_g.dll` / `nvngx_dlssg.dll` binaries | The actual interpolation algorithm, the exact Auto Scene Change Detection math, and the frame-pacer's scheduling formula are **not readable** — everything said about them below comes from `docs/ProgrammingGuideDLSS_G.md`'s prose (SOFT unless independently corroborated) or from `docs/STRAY-RENDERING-FACTS.md` §32's binary string extraction (HARD for names, UNCONFIRMED for the algorithm behind them) |
| `sl.dlss_d` (Ray Reconstruction) | **No**, same situation as `sl.dlss_g` |

So this document can speak with source-level confidence about the swapchain/device/queue/
command-list proxy layer, the resource-tagging lifecycle machinery, and the Reflex/PCL NVAPI
plumbing — and only with documentation-level confidence about DLSS-G's own scheduling and
scene-change logic. That split is maintained explicitly below.

---

## 1. Present pacing

**What Streamline's docs say, and what they do not.** `docs/ProgrammingGuideDLSS_G.md` §13.1,
"Understanding Frame Pacing Behavior" (**HARD**, doc text):

> "DLSS-G uses an asynchronous presentation mechanism called the 'SL pacer' to manage frame
> delivery... DLSS-G uses specialized hardware to delay the image after Present() has been
> called. This delay ensures the image is shown to the end user at precisely the right time, but
> `MsBetweenPresents` does not account for this delay."

and §13.0: *"the call to `Swapchain::Present` is no longer a blocking call for the host and can
be up to 1ms faster."* **This is prose, not code** — "the SL pacer" and "specialized hardware"
are named but not shown; `sl.dlss_g.dll` is closed. Two things are nonetheless established at
HARD confidence: (1) the host's call to `Present` is **non-blocking** — SL hands the frame off
and returns quickly, doing its own scheduling elsewhere; (2) the actual display-timing mechanism
for the generated frame is described as hardware-level, not a CPU-timed wait — which, if
accurate, is not a mechanism a userspace add-on like ours can reach at all, whatever we
implement.

**What we built, independently, before this document existed.** `src/backend_native/fg_present.hpp:14`
and `:54`: the default pacing mode is `Pacing::thread` — both the generated and the real present
run on a **dedicated worker thread** (`g_worker`, `fg_present.cpp:305,848`), and the game's own
call into our `hk_Present` (`present_owner.cpp:440-453`) hands a `Pair` to that worker and returns
essentially immediately (`fg_present.cpp:1281-1298`: queue the pair, `notify_all`, return the
*previous* frame's cached present `HRESULT`) — blocking only if the worker is still finishing the
prior pair. **This already matches Streamline's stated "Present is no longer blocking" principle,
independently arrived at.** No change indicated.

**The actual scheduling algorithm is ours, not modeled on Streamline (we had not read it before
now), and it is already tuned past a real measured jitter source Streamline's docs do not
mention.** `core::fg::Schedule` (`src/core/fg_plan.hpp:74-104`) computes a phase-locked schedule
from a 16-sample median interval, presenting the generated frame at the midpoint after the
previous real present and the real one a full interval later, with hitch detection and re-anchor.
The comment at `fg_present.cpp:721-726` records a real bug this project found and fixed that has
no analogue in Streamline's documentation: *"`condition_variable::wait_for` under Wine wakes ~1 ms
LATE (a 6.1 ms hold came out as 7.15 ms every frame...) so the last 1.5 ms are spun on the
dedicated worker thread"* (`wait_until`, `fg_present.cpp:722-751`, `kSpinWindow = 1'500'000` ns).
**This is a Proton/Wine-specific timer-precision defect our own measurement found and worked
around; nothing in Streamline's public documentation or source addresses it, because Streamline
was never run under Wine in the material read here.** Worth keeping as a genuine, independent
finding of this project's own, not something to second-guess against an unread reference.

**No specific algorithm to adopt.** Streamline's actual generated-frame timing decision (how far
before/after the real present to schedule it, how it reacts to a hitch, whether it uses vsync
timing feedback) lives entirely inside the closed `sl.dlss_g.dll`. What is adoptable is only the
*principle* already matched (§ above) — **no action**.

---

## 2. Resource tagging: lifetimes and states

This is where the open `sl.common` source pays off directly, and where the strongest
cross-validation against our own already-shipped design lives.

**The lifecycle contract, from the guide (HARD, `docs/ProgrammingGuideDLSS_G.md` §5.2) and
confirmed in the code that enforces it:**

* `sl::ResourceLifecycle::eValidUntilPresent` — the recommended default. SL does **not** copy the
  resource; it holds a pointer and an `AddRef`-equivalent reference until a null tag is set for
  that slot, and the guide is explicit that the app therefore will not crash even if it releases
  the resource early ("This does not apply to Vulkan"). Use this first; only downgrade if content
  observably goes stale by present time (over-copying wastes VRAM, per the guide's own warning).
* `sl::ResourceLifecycle::eOnlyValidNow` / `eValidUntilEvaluate` — SL makes an **immediate GPU-side
  copy** on the command list it was handed, transitioning the source to `COPY_SOURCE` and the
  clone to `COPY_DEST`. **HARD, verified in the code, not just the prose**:
  `source/plugins/sl.common/resourceTaggingForFrame.cpp:152-186` (`setTag`) — the copy path is
  gated on `lifecycle != ResourceLifecycle::eValidUntilPresent`, allocates
  `frameTag.clone = m_pPool->allocate(...)`, and requires a valid `cmdBuffer` argument or returns
  `Result::eErrorMissingInputParameter`. Output-shaped tags (`kBufferTypeBackbuffer`,
  `kBufferTypeScalingOutputColor`, the SSD-denoised outputs) are explicitly excluded from ever
  being copied (`writeTag` check, `:148-150`, comment: *"tagging outputs as volatile is ignored,
  we need to write output into the engine's resource"*).
* **The command-list-ordering rule for volatile resources, stated as a hard requirement** (§5.2):
  *"GPU payload that generates content for any volatile resource MUST be either already submitted
  to the provided command list or some other command list which is guaranteed to be executed
  BEFORE."*

**Cross-check against our own design: we already independently chose the stricter option, for
the same reason the guide gives as the *fallback* case.** `src/ngx_fg.cpp`'s `GuideSet g_guides[2]`
(`ngx_fg.cpp:33-40`) is exactly SL's `eOnlyValidNow` behaviour applied unconditionally: on every
TAA-hook dispatch we make our own owned copies of depth and motion vectors into resources under
our control, alternating two sets so a publish for frame N+1 can record while the evaluate for
frame N (on the later, present-time command list) still reads the other. **We never rely on
`eValidUntilPresent`-style "just hold a pointer and hope the game doesn't touch it before
present"** — correctly, because unlike Streamline (which is called from inside the engine's own
render graph, with full knowledge of resource lifetimes), our TAA-hook capture point has no such
guarantee: UE4 recycles descriptor-heap-backed resources within seconds
(`CLAUDE.md` §5, "ReShade's view → resource map outlives the resource on D3D12"), so treating
every guide as needing an immediate GPU copy is the only safe choice for us specifically. **No
action — already correct, and for a reason Streamline's own guide states explicitly as the
justification for the stricter mode.**

**The command-list-ordering rule is respected by construction, not by an explicit check.** Our
guide copies are recorded on the TAA hook's own command list, which UE 4.27 submits to its single
graphics queue before our present-time evaluate consumes them later on the same queue
(`present_owner.cpp`'s "the REAL queue we execute on"); one queue executes in submission order, so
"already submitted to a command list guaranteed to execute before" holds without our needing to
track it separately. Worth stating plainly since it is easy to assume this needs an explicit
fence — on a single shared queue, it does not.

**The other half of §5.2 independently validates our biggest, hardest-won architectural
decision.** *"If validity of tagged resources cannot be guaranteed (for example game is loading,
paused, in menu, playing a video cut scene etc.) all tags should be set to null pointers to avoid
stability or IQ issues."* Our mechanism is different in shape (we never call an `slSetTagForFrame`-
style API to null; we simply do not publish a `GuideSet` when the TAA hook did not fire —
`ngx_fg.cpp:332`'s comment: *"a loading screen, a menu without the pass) has no guides and gets no
generated frame"*) but is the same policy, arrived at independently before this document was
written. **No action — confirms, does not change, the existing design.**

---

## 3. HUD-less / UI handling — the contract, precisely, for future work

We implement no HUD-less or UI-alpha tagging at all today; `DLSSG.HUDLess`/`DLSSG.UI`/
`DLSSG.UIAlpha` are confirmed present, optional parameter names in the raw snippet
(`docs/STRAY-RENDERING-FACTS.md` §32.2) that we simply never set. This is a known gap the
`NgxNRHook=preui` work already reasons toward for an unrelated problem (the NR feedback loop);
this section exists to record the *exact* Streamline contract in one place so that if a hudless
capture is ever built, it does not need to be re-derived from scratch. **HARD**, `docs/
ProgrammingGuideDLSS_G.md` §5.1-5.2:

* **Hudless**: "the scene color *before* any UI/HUD elements are drawn... Same color space and
  post-processing effects (e.g. tonemapping, blur etc.) as color backbuffer." Captured, per the
  code sample, "after post-processing pass but before UI/HUD is added."
* **UI**: either a single-channel **UI Alpha** (preferred, most performant — "if both are tagged,
  DLSS-FG today prefers `kBufferTypeUIAlpha`") or full **UI Color and Alpha**, pre-multiplied.
  Must be exactly `0.0` where there is no UI, non-zero where there is, and must satisfy the
  literal blending identity `Final_Color.RGB = UI.RGB + (1 - UI.Alpha) x Hudless.RGB`.
* **User Interface Recomposition** (`DLSSGOptions::enableUserInterfaceRecomposition`, §6.6):
  requires *both* Hudless and a UI buffer tagged; interpolates HUD and scene as two separate
  streams for "significantly-improved UI interpolation quality," at "a slight performance and
  memory cost."
* **The documented cost of not doing this at all** (§5.1, the Hudless row): "If some HUD/UI
  elements are unavoidably included, expect some image quality degradation on those elements" —
  stated as the ordinary consequence of omission, not a failure mode. This project currently pays
  exactly this cost, knowingly.

**Not adoptable today.** `docs/RESEARCH-DLSS-UE5-PLUGIN.md` §5.2 already traced the actual UE5
mechanism NVIDIA uses to produce these two buffers — one engine-internal post-process callback for
Hudless, one Slate `OnBackBufferReadyToPresent` hook plus an alpha-threshold compute pass for UI —
and found that our DXGI-level hook structurally cannot reach the first of those two (we only ever
see the *final*, already-composited backbuffer). Nothing in Streamline's own source changes that
conclusion; it only sharpens the target contract for whenever engine-level access exists.

---

## 4. Reflex

`CLAUDE.md` and this project's own `fg_reflex.cpp` already establish that DLSS-G needs Reflex and
that DXVK-NVAPI's implementation alone was insufficient (`fakenvapi` + `force_reflex=2` was
required). Streamline's own Reflex plugin is fully open (`source/plugins/sl.reflex/reflexEntry.cpp`,
849 lines) and its actual NVAPI calls live one layer down, in the shared compute abstraction
(`source/platforms/sl.chi/d3d12.cpp`) — confirmed by grep, not by reading every line:

```cpp
// d3d12.cpp:2845-2869
ComputeStatus D3D12::notifyOutOfBandCommandQueue(ChiCommandQueue* queue, OutOfBandCommandQueueType type)
{
    NVAPI_CHECK(NvAPI_D3D12_NotifyOutOfBandCommandQueue((ID3D12CommandQueue*)queue, (NV_OUT_OF_BAND_CQ_TYPE) type));
    return ComputeStatus::eOk;
}
ComputeStatus D3D12::setAsyncFrameMarker(CommandQueue queue, PCLMarker marker, uint64_t frameId)
{ ... NvAPI_D3D12_SetAsyncFrameMarker(...) ... }
ComputeStatus D3D12::setLatencyMarker(CommandQueue queue, PCLMarker marker, uint64_t frameId)
{ ... NvAPI_D3D_SetLatencyMarker(...) ... }
```

plus `setSleepMode` (`reflexEntry.cpp:434`, `ctx.compute->setSleepMode(ctx.constants)`, same
abstraction layer). **HARD.** Our own `src/backend_native/fg_reflex.cpp` already calls the
NVAPI-equivalent set: `Initialize`, `SetSleepMode`, `Sleep`, `SetLatencyMarker`, and
`SetAsyncFrameMarker` (grep confirms all present). **One call Streamline makes that we do not:
`NvAPI_D3D12_NotifyOutOfBandCommandQueue`.** This marks a command queue as carrying work *outside*
the normal simulate→render→present chain Reflex/PCL measures — exactly the shape of our own
present-owner's extra queue submissions for the generated frame, which by construction sit
outside the game's own Reflex-instrumented frame boundary (we already mark this at the *frame*
level with `reflex::Marker::out_of_band_present_start/end`, `fg_present.cpp:706-718`, but never at
the *queue* level). **Investigate/adopt, concrete and cheap**: call
`NvAPI_D3D12_NotifyOutOfBandCommandQueue` once on the queue our present owner submits the
generated-frame present through. Confidence this is *necessary* is **UNCONFIRMED** — Reflex has
been working well enough to be worth keeping in this project's own measurements without it — but
it is a small, well-targeted addition with a plausible latency-accounting benefit (Reflex/PCL
correctly excluding our extra present from its own timing model) and no plausible downside.

**Streamline needs the identical NVAPI surface we already use; there is no lighter-weight
Reflex path available that we are missing.** No further action beyond the one call above.

---

## 5. Swapchain ownership and present-twice mechanics — diagnostic, not adoptable

This is the section CLAUDE.md's OptiScaler note flags as unfinished: *"the hook-up works, the FG
swapchain does not"* (commit `ff39c61`), with the crash `EXCEPTION_ACCESS_VIOLATION writing
address 0x000000020000000d` already noted as sharing its bit-packed-handle-shaped signature with
the ext-vtable-patch bug `CLAUDE.md` §1 fully diagnoses for a *different* proxy (ReShade's). What
follows either strengthens or weakens that suspicion, using Streamline's own open source, read for
the first time for this purpose.

### 5.1 — Architecture: full COM object wrapping, not vtable patching

Streamline's swapchain, device, command queue and command list are each a genuine **wrapper
object** — a new C++ class implementing the full COM interface, holding the real object as
`m_base`, forwarding by default (`source/core/sl.interposer/dxgi/dxgiSwapchain.cpp`,
`d3d12/d3d12Device.cpp`, `d3d12/d3d12CommandQueue.cpp`, `d3d12/d3d12CommandList.cpp`) — the same
general shape as ReShade's own device/command-list proxying, and **architecturally different from
our own `present_owner.cpp`**, which patches specific vtable slots on the game's real swapchain
object in place and never creates a new object identity. Separately, `source/core/sl.interposer/hook.cpp`
implements a raw inline-trampoline code patcher (`VirtualProtect` + a written jump,
`kCodePatchSize`) for hooking **exported functions** like `D3D12CreateDevice`/`CreateDXGIFactory` —
this is how the wrapper objects get substituted in the first place, not how per-call interception
happens afterward. **HARD**, all four proxy classes confirmed present and reachable via grep and
direct reading of the D3D12 device and command-queue files.

### 5.2 — What was checked, specifically, against the ReShade ext-vtable-patch hazard, and found clean

`CLAUDE.md` §1's diagnosed bug is precise: ReShade's `IID_ID3D12DeviceExt` `QueryInterface`
handler patches vtable slots 7/8 (and 14/15 for `Ext2`) so that `GetCudaTextureObject`/
`GetCudaSurfaceObject` descriptor handles are run through `convert_to_original_cpu_descriptor_handle`
— correct only for descriptors ReShade itself minted, and an out-of-bounds read on any other
handle, including a **real** vkd3d-proton descriptor. Two specific things were checked in
Streamline's device and command-list proxies for the same class of hazard, and both came back
clean:

* **`QueryInterface` for an unrecognized IID passes straight through to the native object.**
  `D3D12Device::QueryInterface` (`d3d12Device.cpp:353-375`) and
  `D3D12GraphicsCommandList::QueryInterface` (`d3d12CommandList.cpp:99-121`) both check a small,
  explicit set of IIDs they wrap (`checkAndUpgradeInterface`), and for anything else —
  including `IID_ID3D12DeviceExt`/`ID3D12GraphicsCommandListExt`, the vkd3d-proton extension
  interfaces this whole hazard is about — `return m_base->QueryInterface(riid, ppvObj);`,
  unmodified. Neither proxy intercepts or wraps the extension interface at all. **HARD.**
* **Command lists are explicitly unwrapped to their native pointer before the real queue ever
  sees them.** `D3D12CommandQueue::ExecuteCommandLists` (`d3d12CommandQueue.cpp:159-181`):
  for every list the app submits, it `QueryInterface`s for Streamline's own wrapper type and, if
  found, substitutes `cmdListProxy->m_base` — the **native** pointer — before calling
  `m_base->ExecuteCommandLists(...)`. The real driver never receives a Streamline-proxy command
  list pointer through this path. **HARD.**

**So the specific mechanism this project's own history would make you suspect first —
Streamline's own object wrapping leaking a proxy identity into a path that expects the native
vkd3d object — is not present in the parts of Streamline read here.** This is a genuine negative
result, not a shrug: it narrows, rather than confirms, the hypothesis space, which is the honest
outcome CLAUDE.md's own verification discipline asks for.

### 5.3 — The stronger remaining hypothesis: the *already-diagnosed* ReShade bug, reached by DLSS-G's own calls, which `ext_unhook` cannot protect

`CLAUDE.md` §1 (unrelated to Streamline, established from this project's own earlier debugging)
measured that ReShade's ext-vtable patch is a **single process-wide static table** — one
`QueryInterface` against ReShade's proxy, from *anywhere* in the process, patches the vtable every
other caller's `ID3D12DeviceExt` interface pointer also uses, and that it stays patched from the
moment it first happens (measured: clean at device creation, hooked by frame 300) for the rest of
the session. This project's own fix, `src/ext_unhook.cpp`, works by **re-installing the original
vtable pointers immediately before every one of *our own* NGX calls** (init, feature creation,
every evaluate) — it is a repair scoped to our own call sites, applied fresh each time because one
`QueryInterface` anywhere can re-break it.

**That scoping is exactly the gap.** DLSS-G's own internal NVAPI cubin calls — inside the closed
`sl.dlss_g.dll`/`nvngx_dlssg.dll`, invoked from Streamline's own present hook at a point in the
frame our `ext_unhook` has no visibility into — run against whatever the shared static vtable
currently holds, and nothing repairs it for them. If ReShade's proxy (loaded as `ReShade64.dll`
behind OptiScaler's `dxgi.dll` per the documented coexistence setup) patches that table at any
point in the session — which `CLAUDE.md` §1 already measured happens reliably, driven by nothing
more than Stray's own normal D3D12 usage — DLSS-G's own evaluate calls inherit the corruption with
no repair mechanism watching for them, on every frame from that point on.

**This fits every measured fact without requiring a new mechanism:**

* The death is **deterministic and frame-count-specific** (11 / 32 / 36 depending on config) —
  consistent with "the vtable gets patched once, early, then every subsequent DLSS-G evaluate
  that touches the corrupted entries is broken from that point," not with a random race.
  `CLAUDE.md`'s own earlier measurement puts ReShade's patch as observed by frame 300 in one
  session and unmeasured-but-presumably-similar in others — a config that runs FG's first
  real evaluate near frame 11-36 would be well within the window where this could already have
  happened, especially if OptiScaler's own additional `QueryInterface` traffic at startup
  triggers it earlier than Stray's own D3D12 usage alone would.
* **`NgxNR=0` died at the identical frame** (`CLAUDE.md`'s OptiScaler section) — ruling out our
  own feature 18 code as the cause, consistent with the fault being in DLSS-G's own calls,
  entirely independent of what our add-on does.
* **The crash signature already matches**: `0x000000020000000d`, a bit-packed handle-shaped
  value — `CLAUDE.md`'s own text draws this exact parallel to §1's descriptor-handle-dereferenced-
  as-pointer signature, without concluding a mechanism. This document supplies the missing half:
  *why* a fix already shipped in this project (`ext_unhook`) does not help a third party's
  (DLSS-G's) own separate calls into the same shared, process-wide, persistently-corrupted table.
* **"With FG off this configuration reaches gameplay and is stable"** (`CLAUDE.md`) — consistent,
  because with FG off, DLSS-G's own cubin calls never happen at all; only our own NGX calls do,
  and those are protected by `ext_unhook` on every call.

**This is SOFT — a reasoned hypothesis, not a proof.** `sl.dlss_g.dll` itself was not, and cannot
be, read (closed source); this document did not re-run anything on the box (report-only task).
What it changes is the *shape* of the next test, if this combination is ever revisited: confirm
whether `ext_unhook`'s repair, run continuously (or specifically right before every Present, where
Streamline's own evaluate fires) rather than only before our own NGX calls, changes the outcome.
That is a testable, falsifiable next step this document did not have before reading Streamline's
source, and it is squarely a fix to *our own* resilience (a repair that currently only guards our
own call sites could guard the process more broadly) rather than anything requiring Streamline's
cooperation.

**What carries forward to the current, UE4SS-native, ReShade-free architecture, and what does
not.** `src/ext_unhook.cpp` still exists and is still referenced from the current native backend
(`src/backend_native/vtable_patch.hpp`, `src/ngx_backend.cpp`, `src/ngx_fg.cpp`) — its header
comment is unchanged, still specifically about repairing *ReShade's* patch of vkd3d-proton's
`ID3D12DeviceExt` vtable. Its continued presence is defensive, not evidence that ReShade is part
of the standard run configuration today: a user is free to run ReShade alongside this plugin for
unrelated visual effects, and if they do, the same process-wide static-vtable hazard `CLAUDE.md`
§1 diagnoses is live again regardless of how *our* code loads. But the OptiScaler/Streamline
death itself was measured specifically with `ReShade64.dll` loaded as part of that experiment's
own coexistence setup (per the architecture note above) — a configuration this project does not
run by default. So: the hypothesis above explains a **historical measurement**, taken under a
configuration that included ReShade by construction. Whether a *fresh* Streamline/OptiScaler
coexistence test, run today with no ReShade in the process at all, reproduces the same
deterministic death is genuinely **UNCONFIRMED** — if the mechanism above is right, removing
ReShade from the picture should remove the crash too, which is itself a cheap, falsifiable
prediction this document did not have before being written, and a cleaner test than re-adding
`ext_unhook` protection to a configuration nobody runs anymore.

### 5.4 — The architecture Streamline actually wants, and why it does not fit us regardless of the above

Independent of the crash hypothesis, §18.0 of the DLSS-G guide states plainly that Streamline's
intended integration shape is heavier than anything a swapchain-level hook can provide:

> "When DLSS-G is loaded it will create an **extra graphics command queue** used to present
> frames asynchronously and in addition it will force the host application to **render off-screen**
> (host has no access to the swap-chain buffers directly)... To avoid [overhead when DLSS-G is
> off], swap-chain must be **torn down and re-created** every time DLSS-G is switched on or off."

**HARD**, and it names three things our own design deliberately does not do: a dedicated second
D3D12 command queue (we reuse "the REAL queue we execute on," `present_owner.cpp`; no
`CreateCommandQueue` call anywhere in our present path), forcing the whole engine to render
off-screen (not reachable from outside the engine — the same wall
`docs/RESEARCH-DLSS-UE5-PLUGIN.md` names for `ITemporalUpscaler`), and a full swapchain
teardown/recreate on every FG on/off toggle (our design patches the existing swapchain in place
and simply starts or stops presenting extra frames through it — lighter-weight, but only possible
*because* we are not attempting the off-screen-render model). **Not applicable — these are
consequences of Streamline owning the whole presentation pipeline, which is precisely the
posture measured to break under this target's vkd3d-proton, and precisely what our own design
exists to avoid.**

---

## 6. Anything Streamline does that we simply do not

* **Auto Scene Change Detection (ASCD)** — described (`docs/ProgrammingGuideDLSS_G.md` §20,
  SOFT/prose, `sl.dlss_g.dll` itself not read) as running on *every* frame pair inside the closed
  snippet, independently of whatever reset flag the caller supplies, built from the **same**
  camera right/up/forward vectors we already send (`DLSSG.CameraRightX/Y/Z` etc.,
  `ngx_fg.cpp:458-466`). It stitches them into a 3×3 rotation matrix and **requires that matrix
  to be orthonormal** — "If the orthonormal check fails, ASCD is entirely disabled," logged, but
  silently as far as our own add-on (which does not read Streamline's — or the snippet's — debug
  log stream) is concerned. **Investigate, concrete and cheap**: `core::fg::camera_basis`
  (`src/core/fg_plan.cpp:375-384`) extracts right/up/forward as columns of
  `TranslatedWorldToView` and is unit-tested only against synthetic identity/rotation matrices
  (`tests/test_fg_plan.cpp:239-246`) — never checked for orthonormality against a matrix captured
  from the live View constant buffer. This project has already shipped one live bug from exactly
  this class of error (`shaders/mv_resolve.hlsl`'s `LegacyTransposedClip` note: a missing
  `row_major` qualifier on `ClipToPrevClip` produced correct output on a *stationary* camera and
  silently wrong output only once the camera moved). A convention mistake here would not crash or
  visibly corrupt the image — it would only and silently disable NVIDIA's own extra safety net
  for scene cuts, which is exactly the "quiet wrong image" class of failure `CLAUDE.md` §0.2
  exists to prevent. Add a CI check that `camera_basis()` applied to a real captured
  `TranslatedWorldToView` produces three mutually perpendicular, unit-length vectors.
* **`NvAPI_D3D12_NotifyOutOfBandCommandQueue`** — covered in §4; the one concrete Reflex-adjacent
  call we do not make.
* **VSync-with-Frame-Generation** (§22.0, `sl::DLSSGState::bIsVsyncSupportAvailable`) — a
  supported combination in Streamline's own design. **Not applicable**: this project's target
  runs through gamescope's own compositor and VRR path (`CLAUDE.md` §1, DRM backend), not an
  application-level DXGI vsync toggle Streamline would need to coordinate with.
* **The API-error-callback pattern** (§6.7, `onErrorCallback` on `DLSSGOptions`, fired
  synchronously on the present thread with an explicit "must not block" warning) — a reasonable
  diagnostic pattern; **not applicable** in the sense that we drive the NGX core directly and
  already get a return code from every call inline rather than through an async present-thread
  callback, so there is no equivalent gap to fill.
* **Multi-viewport support** (§5.3) — not applicable, Stray is a single-viewport title.
* **`eRetainResourcesWhenOff`** — already flagged as low-priority/UNCONFIRMED in
  `docs/RESEARCH-DLSS-UE5-PLUGIN.md` §6; not re-derived here.

---

## What could not be obtained, stated plainly

* **`sl.dlss_g.dll` / `sl.dlss_d`'s actual source.** Not present in the open repository at any
  tag, including `v2.12.0`. Every claim above about the *interpolation algorithm*, the *exact*
  ASCD math, and the *precise* pacer scheduling formula rests on `docs/ProgrammingGuideDLSS_G.md`'s
  prose (SOFT) or on `docs/STRAY-RENDERING-FACTS.md` §32's binary-string extraction (HARD for
  parameter names, UNCONFIRMED for the logic behind them) — never on code read for this document.
* **Confirmation of the §5.3 crash hypothesis.** This is a reasoned diagnostic argument built
  from source facts that were genuinely verified (the proxy-transparency and command-list-unwrap
  behaviour) plus facts already established elsewhere in this project (`CLAUDE.md` §1's
  process-wide static vtable, the OptiScaler death's exact frame numbers and crash signature). It
  was not, and could not be, tested against the box in this report-only task. The next concrete
  step is named in §5.3.
* **Whether `NvAPI_D3D12_NotifyOutOfBandCommandQueue` is required or merely correct-in-principle.**
  No source or doc read here states a consequence of omitting it; the recommendation rests on it
  being the one Reflex-surface call Streamline makes that we do not, not on a measured symptom.

---

## Recommendations, ranked

### Adopt into our own code (independent of Streamline's swapchain — safe under vkd3d-proton)

1. **Call `NvAPI_D3D12_NotifyOutOfBandCommandQueue` on the present owner's queue for the
   generated-frame submissions.** Small, concrete, matches Streamline's own Reflex plumbing
   exactly; we already have the conceptual equivalent at the marker level
   (`out_of_band_present_start/end`) and are missing the queue-level counterpart. §4.
2. **Add a CI/runtime check that `core::fg::camera_basis()` produces an orthonormal matrix from a
   real captured `TranslatedWorldToView`, not just from synthetic test matrices.** Cheap, and
   protects against a repeat of exactly the kind of stationary-camera-hides-everything convention
   bug this project has already shipped once (`ClipToPrevClip`'s missing `row_major`). Protects a
   safety net (NVIDIA's own ASCD) whose loss would be silent. §6.
3. **No code change, but worth stating with confidence**: our async worker-thread present
   (`Pacing::thread`), our per-guide GPU-side copy discipline (`GuideSet[2]`), and our
   guide-nulling-by-omission on invalid frames are all independently confirmed to already match
   the principles Streamline's own documentation and source state as correct. §1, §2.

### Explains a bug we have measured

1. **The OptiScaler FG death — measured under the ReShade-based configuration this project no
   longer runs by default — is most plausibly the already-diagnosed ReShade ext-vtable-patch bug
   (`CLAUDE.md` §1), reached by DLSS-G's own internal NVAPI calls at present time, a path our
   `ext_unhook` did not protect because it only repairs the vtable immediately before *our own*
   NGX calls.** Streamline's own device/command-queue/command-list proxies were checked
   specifically for the naive version of this hazard (a leaked proxy pointer reaching a subsystem
   that expects the native vkd3d object) and found clean — so the fault was not Streamline's own
   object-wrapping design. Two falsifiable next steps, in order of cost: (a) cheapest — re-run the
   same OptiScaler/Streamline coexistence test under the *current*, ReShade-free, UE4SS-native
   configuration; if the hypothesis is right, the deterministic death should simply not occur,
   since nothing installs the ext-vtable patch in the first place. (b) if ReShade is ever
   reintroduced into any configuration (e.g. a user layering it for visual effects) — run
   `ext_unhook`'s repair continuously, or specifically before every `Present`, rather than only
   before our own NGX calls, and see whether that changes the outcome. §5.3.

### Not applicable under vkd3d-proton / UE 4.27

1. **Streamline's full off-screen-render + dedicated-queue + swapchain-teardown-on-toggle
   architecture (§18.0).** Requires forcing UE 4.27's entire renderer to draw off-screen — not
   reachable from a swapchain or D3D12-event hook, and a heavier commitment than this project's
   present-in-place design needs for what it does today. §5.4.
2. **Streamline's swapchain proxy itself, deployed as `sl.interposer.dll`.** Measured to break
   frame generation on this exact target, deterministically, independent of how clean its
   component-level code reads — an interaction with this specific driver stack, not a defect
   fixable by reading more source. §5, `CLAUDE.md` OptiScaler section.
3. **VSync-with-Frame-Generation, multi-viewport support, dynamic-resolution+FG interplay.** None
   apply to Stray's fixed single-viewport, fixed-screen-percentage, gamescope-composited
   configuration. §6.
4. **User Interface Recomposition (§6.6).** Depends on Hudless+UI tagging this project cannot
   produce without engine-level access — recorded precisely in §3 for if that ever changes, not
   actionable today.
