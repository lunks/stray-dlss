# Streamline internals, read for what our own present-owner/pacer/tagging code should learn

> ## RE-REVIEWED 2026-09-03 — read this box first
>
> The first pass (commit `dd38dd5`) read only the **public** `NVIDIA-RTX/Streamline` SDK. Access
> to **`NvRTX/UnrealEngine`**, NVIDIA's private UE fork, has since been obtained, and with it
> NVIDIA's own *caller* of that SDK on both UE 4.27 (our engine) and UE 5.8. Sources with branch
> and SHA are in §0.
>
> **Where the first pass was right, and it was right about the things it built its argument on.**
> Its two adoptable recommendations both stand: `NvAPI_D3D12_NotifyOutOfBandCommandQueue` is
> genuinely the one Reflex-surface call Streamline makes that we do not (§4), and the
> orthonormality check on `camera_basis()` remains a cheap guard on a silent failure (§6). Its
> §2 cross-validation of our tagging discipline stands, and is now corroborated by the plugin
> itself, which uses `eOnlyValidNow` — the stricter mode — exactly as we do. Its §5.1/§5.2
> negative result (Streamline's proxies are transparent to `IID_ID3D12DeviceExt`) stands.
>
> **What changed materially:**
>
> 1. **§5's premise is stated too absolutely, and its own §5.3 already knows it.** The document
>    opens by calling the OptiScaler death *"a measured answer, and it is no, on this driver
>    stack"* — then argues 300 lines later that the mechanism was probably **ReShade's** vtable
>    patch, which is not in the process any more. Those two cannot both be load-bearing. Worse,
>    the conclusion has since been **overtaken by measurement**: our own present-twice path has
>    cleared the exact wall OptiScaler died at. Corrected in §5.0 and §5.3.
> 2. **§5.4 contains a feasibility claim that has flipped.** "Forcing the engine to render
>    off-screen" is listed as *not reachable from outside the engine*. **We do it today**, by
>    returning replacement textures from a hooked `GetBuffer`. Corrected in §5.4.
> 3. **§3's hudless contract is right but incomplete in a way that matters** — the UI half
>    depends on an engine patch, and NVIDIA tells HDR titles (which we are) not to use it at all.
> 4. **§1's pacing section gains a first-party fact**: NVIDIA's plugin never presents. The extra
>    frames come from inside the interposed swapchain. Our present-twice design is our own.
>
> Superseded reasoning is preserved inline rather than deleted, per `CLAUDE.md`'s house style.

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

> **CORRECTED 2026-09-03 — this paragraph overstates its own evidence, and §5.3 below already
> knows why.** Calling the OptiScaler result *"a measured answer, and it is no"* treats a
> confounded measurement as settled. Three things are now on the record against it:
>
> 1. **The measurement included ReShade by construction** (OptiScaler as `dxgi.dll`, ReShade
>    loaded behind it as `ReShade64.dll`) — a configuration this project no longer runs. §5.3
>    argues at length that ReShade's process-wide ext-vtable patch is the most plausible
>    mechanism, and explicitly predicts that **removing ReShade should remove the crash**. If
>    that argument is right, the opening sentence is wrong. The document cannot have both.
> 2. **It has been overtaken by our own measurement.** `docs/STRAY-RENDERING-FACTS.md`
>    §32.9-§32.11 record DLSS-G evaluating at **1.91x through three checkpoint reloads**, and
>    `CLAUDE.md` records that *"the fullscreen transition — where OptiScaler's FG died — is
>    survived"*. The wall that stopped OptiScaler is one our own present-twice path has since
>    walked through. Whatever killed OptiScaler, "frame generation cannot work on this driver
>    stack" is not it.
> 3. **The project's own diagnosis is internally unreconciled**: `CLAUDE.md` attributes the death
>    to a bit-packed handle (the ReShade descriptor-conversion family) in one place and to the
>    fullscreen transition in another, and never reconciles them.
>
> **Restated honestly: Streamline's interposer is off the table by USER CONSTRAINT** (recorded in
> `docs/STRAY-RENDERING-FACTS.md` §32.5) **and was measured to fail once in a confounded
> configuration.** That is a good enough reason not to adopt it — we have a working alternative
> — but it is **not** the settled technical finding this sentence claims, and no design decision
> should rest on it as though it were. **SOFT**, and the falsifiable test is in §5.3.

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

> **ADDED 2026-09-03 — first-party callers of this SDK, now obtainable.** The first pass read
> the SDK alone. These are NVIDIA's own integrations of it, and several claims below are now
> cross-checked against them rather than against the SDK's prose:
>
> | Source | Branch | Tip SHA | Relevance |
> |---|---|---|---|
> | `NvRTX/UnrealEngine` | `dlss3/sl2-4.27-dlss-plugin` | `32c3e4d5e0ee9ea7b792d1d09bcc2549917ad252` | DLSS-FG/Streamline **1.3.0-SL2.4.0 on UE 4.27** — our exact engine version |
> | `NvRTX/UnrealEngine` | `nvrtx-5.8_prerelease` | `5b89940f9436ab7475d547e322443efe6d875ac2` | Current: plugin **8.7.2**, Streamline SDK **2.11.1**, NGX **310.6.0** (July 2026) |
> | `NvRTX/UnrealEngine` | `dlss-streamline-4.27-engine-changes` | `e1ee6efc0d4efda533fb71060b2387a698073dba` | The **engine-side** requirements for hosting Streamline on 4.27 |
> | `EpicGames/UnrealEngine` | tags `4.27`, `5.4`, `5.6`, `5.8` | (by ref) | Stock engine, to establish what Stray already has |
>
> Note the SDK version read by the first pass (**public v2.12.0**) is *newer* than the one the
> current plugin vendors (**2.11.1**). Where the two could differ, the SDK reading is retained
> and flagged. `docs/RESEARCH-DLSS-UE5-PLUGIN.md` §7 covers the engine-changes branch in full and
> is not duplicated here.

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

> **CONFIRMED 2026-09-03, and the reason no algorithm is visible is now structural rather than
> incidental.** The first pass inferred that SL's pacing lives in the closed DLL. First-party
> reading of NVIDIA's own UE plugin shows *why nothing else could*: **the plugin never presents,
> and never even evaluates DLSS-G.** There is no `evaluateFeature` call for `kFeatureDLSS_G`
> anywhere in it; DLSS-G is driven entirely by `slDLSSGSetOptions` + resource tags + constants,
> and the extra frames are issued from **inside the interposed swapchain**. The custom present
> the plugin installs is a deliberate no-op that exists only to release back-buffer references
> before a viewport resize:
>
> ```cpp
> virtual bool NeedsNativePresent()    override final { return true; }   // engine presents normally
> virtual bool NeedsAdvanceBackbuffer() override final { return true; }  // engine advances normally
> virtual bool Present(FRHIViewport*, IRHICommandContext&, int32&) override final { return true; };
> ```
>
> **HARD.** So the pacer is not merely closed — it is on the far side of an architectural
> boundary NVIDIA's own engine integration never crosses. **The first pass's "no action" verdict
> stands and is now better founded**: there is no reference implementation to compare our
> `core::fg::Schedule` against, at any level of access short of the DLL itself.
>
> One consequence for our own pacer's *correctness argument*, worth recording: our phase-locked
> scheduler and the Wine `wait_for` spin-window workaround (`fg_present.cpp:721-751`) are
> **unreviewable against a reference**, so their only evidence is our own measurement. That is a
> reason to keep the FG validation gate (the per-frame crop readback that revokes a generated
> frame and counts the refusal by reason) rather than to trust the schedule.

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

> **CORRECTED AND NARROWED 2026-09-03.** The contract above is accurate and worth keeping. Two
> things change the conclusion, and they pull in opposite directions.
>
> **The UI half is not merely unreachable — it is the WRONG TECHNIQUE for this title, twice
> over. HARD, first-party.**
>
> 1. **NVIDIA's alpha mask is manufactured by an engine patch, not discovered.** The view
>    extension clears scene-colour alpha to zero at the end of the post-process chain
>    (`r.Streamline.ClearSceneColorAlpha`, default **true**) so that whatever Slate draws
>    afterwards is the only thing with non-zero alpha. It calls `DrawClearQuadAlpha`, which
>    **does not exist in stock UE 4.27** (verified absent from `EpicGames/UnrealEngine` @ `4.27`,
>    `RenderCore/Public/ClearQuad.h`) and which the plugin hard-`#error`s without. Without that
>    step the back-buffer alpha is simply whatever the game left there.
> 2. **NVIDIA tells HDR titles not to use it.** The 8.7.2 plugin's own `README.md` records that
>    in HDR the back-buffer alpha is not meaningful and recommends `r.Streamline.TagUIColorAlpha 0`.
>    Stray presents `R10G10B10A2_UNORM` under gamescope's HDR path. **So the technique is
>    switched off for titles like ours even when it is fully available.**
>
> **The hudless half is more reachable than this section says**, and that is the more useful
> correction. `SubscribeToPostProcessingPass(EPostProcessingPass::VisualizeDepthOfField, ...)`
> is a method on **stock UE 4.27's** `ISceneViewExtension`, with the enum value present — not a
> UE5 addition. It is not reachable from our *DXGI-level* hook, which is what this section
> correctly says; it is reachable in principle by registering a view extension, which is a
> different and better-specified problem. `docs/RESEARCH-DLSS-UE5-PLUGIN.md` §1.1 and §5.2 carry
> the full grading and the obstacles (`GEngine->ViewExtensions` is not a `UPROPERTY`;
> `FSceneViewExtensions::NewExtension` is a template and cannot be hooked). **UNCONFIRMED** that
> it is achievable; **HARD** that the interface exists on our engine.
>
> **Net for this section: the target contract narrows to Hudless alone.** User Interface
> Recomposition (§6.6) needs *both* buffers and is therefore doubly out of reach — worth knowing
> before anyone budgets for it.

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

> **CONFIRMED AND EXTENDED 2026-09-03, first-party.** Reflex is not merely recommended for
> DLSS-G, it is a **hard SDK-level requirement**: `sl_dlss_g.h` defines
> `DLSSGStatus::eFailReflexNotDetectedAtRuntime` — *"Reflex is not active while DLSS-G is
> running, Reflex must be turned on when DLSS-G is on"* — and the 8.7.2 plugin `checkf`s on a
> non-`eOk` status **every frame** (`r.Streamline.DLSSG.CheckStatusPerFrame`, default true). The
> plugin force-enables Reflex whenever DLSS-G is active, via a function whose entire body is
> `return IsDLSSGActive();`. **HARD.** This corroborates, from the vendor side, why this project
> needed `fakenvapi` + `force_reflex=2` before FG behaved.
>
> **A refinement to the recommendation below.** NVIDIA's markers go through `kFeaturePCL`
> (`slPCLSetMarker`) with a 1:1 mapping onto `sl::PCLMarker` — `eSimulationStart/End`,
> `eRenderSubmitStart/End`, `ePresentStart/End`, `eTriggerFlash` — and
> `SetInputSampleLatencyMarker` is a **deliberate no-op** ("no longer supported"). Our
> `fg_reflex.cpp` set is equivalent. **`NvAPI_D3D12_NotifyOutOfBandCommandQueue` remains the one
> call we do not make**, and the first pass's reasoning for it is unchanged and still
> **UNCONFIRMED** as *necessary*: no source or doc read on either pass states a consequence of
> omitting it. Keep it ranked as a small, well-targeted addition, not a fix for a known symptom.

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

> ### CORRECTED 2026-09-03 — one of those three is something WE ALREADY DO, from outside
>
> **The middle claim is wrong, and it is exactly the class of ReShade-era feasibility judgement
> this re-review exists to catch.** "Forcing the whole engine to render off-screen" is filed
> above as *not reachable from outside the engine*, by analogy to `ITemporalUpscaler`. **We do it
> today.**
>
> `src/backend_native/fg_present.cpp` hooks `IDXGISwapChain::GetBuffer` and returns **our own
> replacement textures** (`replacement[kMaxReplacements]`, created with the same desc as the real
> back buffer), copying into the real back buffer at each of our presents. The game renders every
> frame into a texture of ours and **never touches a real swap-chain buffer** — which is
> precisely the guide's *"host has no access to the swap-chain buffers directly"*. We achieved it
> not by changing the engine but by lying in one COM method. **HARD**, read from our own source,
> and measured working (`docs/STRAY-RENDERING-FACTS.md` §32.7-§32.11).
>
> The analogy to `ITemporalUpscaler` is what misled the paragraph: that is engine-internal C++
> with no D3D12 marshalling, whereas off-screen rendering is expressed **entirely through the
> DXGI interface we already own**. Same sentence, two completely different kinds of wall.
>
> **The other two claims stand, and are now first-party confirmed.** We use no dedicated second
> command queue, and we do not tear down the swapchain on toggle. Both remain deliberate.
>
> **What this changes in practice: less than it sounds, and that is the honest part.** We are not
> "avoiding Streamline's model" as thoroughly as this section claims — we have independently
> reimplemented one of its three defining properties. The genuine architectural difference is
> narrower and is stated in `docs/RESEARCH-DLSS-UE5-PLUGIN.md` §7.6: **NVIDIA presents once and
> lets a proxy swapchain inject frames; we present twice from a private ring.** The plugin's own
> custom present is a no-op with `NeedsNativePresent() { return true; }` — the engine presents
> normally and `sl.dlss_g` adds frames invisibly inside the interposed swapchain. **Present-twice
> is our invention**, forced by not owning the swapchain end to end, not a reproduction of
> NVIDIA's design.

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
  `docs/RESEARCH-DLSS-UE5-PLUGIN.md` §6; not re-derived here. **CONFIRMED 2026-09-03**:
  `r.Streamline.DLSSG.RetainResourcesWhenOff` defaults to **false** in the 8.7.2 plugin, so
  NVIDIA does not take it by default either. **HARD.** Closes the first pass's open question in
  the "do nothing" direction.

---

## Recommendations, ranked — RE-RANKED 2026-09-03

> The first pass's ranking is largely retained: its two adoptable items both survive first-party
> checking. What changed is the confidence attached to the diagnostic section, and the removal of
> one feasibility claim that had flipped.

### Adopt into our own code (independent of Streamline's swapchain — safe under vkd3d-proton)

1. **Call `NvAPI_D3D12_NotifyOutOfBandCommandQueue` on the present owner's queue for the
   generated-frame submissions.** Small, concrete, matches Streamline's own Reflex plumbing; we
   already have the marker-level equivalent (`out_of_band_present_start/end`) and are missing the
   queue-level counterpart. **HARD** that Streamline makes the call and we do not.
   **UNCONFIRMED** that omitting it has any consequence — no source read on either pass states
   one. Adopt as tidiness with a plausible latency-accounting benefit, not as a fix. §4.
2. **Add a CI/runtime orthonormality check on `core::fg::camera_basis()` against a real captured
   `TranslatedWorldToView`**, not just synthetic matrices. Cheap; protects NVIDIA's own ASCD
   safety net, whose loss would be **silent**. Unchanged from the first pass and still right —
   note it is the same failure class as the newly-found half-pixel motion-vector question in
   `docs/RESEARCH-DLSS-UE5-PLUGIN.md` §2.4, and the two would be sensibly done together. §6.
3. **No code change, stated with more confidence than before.** Our async worker-thread present,
   our per-guide GPU-side copy discipline (`GuideSet[2]`), and our guide-nulling-by-omission on
   invalid frames all match the principles Streamline's source and docs state as correct — and
   the tagging discipline is now **corroborated by NVIDIA's own plugin**, which uses
   `eOnlyValidNow` (the stricter mode) with a TODO noting `eValidUntilPresent` would be more
   efficient. We chose the same trade-off independently, for a reason the guide states. §1, §2.

### Explains a bug we have measured — DOWNGRADED, and partly overtaken

4. **The OptiScaler FG death is most plausibly ReShade's ext-vtable patch reached by DLSS-G's own
   NVAPI calls — but this now explains a historical measurement of diminishing relevance.**
   §5.1/§5.2's negative result stands (Streamline's own proxies are transparent to
   `IID_ID3D12DeviceExt` and unwrap command lists before submission, so its object-wrapping design
   was not the fault). The §5.3 hypothesis remains **SOFT** and untested.
   **What changed: the question matters less.** Our own present-twice path has since carried
   DLSS-G past the exact wall OptiScaler died at — 1.91x through three checkpoint reloads,
   surviving the fullscreen transition (`docs/STRAY-RENDERING-FACTS.md` §32.9-§32.11). The
   cheapest test named by the first pass — re-run the coexistence experiment with no ReShade in
   the process — is still valid and still falsifiable, but it now buys a tidy explanation of a
   dead configuration rather than unblocking anything. **Rank accordingly: interesting, not
   urgent.** §5.0, §5.3.
5. **The one live item that falls out of §5.3.** `ext_unhook` repairs the vtable only immediately
   before *our own* NGX calls. With the present owner we now own the frame's end-of-submission
   chokepoint, so running the repair there as well is nearly free. Only worth doing if ReShade is
   ever reintroduced alongside the plugin (a supported configuration — `mods/StrayDLSS/README.md`
   documents Config B); **inert otherwise**, as measured. §5.3.

### Not applicable under vkd3d-proton / UE 4.27

6. **Streamline's dedicated-queue and swapchain-teardown-on-toggle architecture.** Both remain
   deliberate divergences. **NOTE the third item in the original list was WITHDRAWN**: "forcing
   the engine to render off-screen" is *not* out of reach — our `GetBuffer` replacement ring does
   exactly that, from outside, today. The real difference is present-once-with-injection versus
   present-twice-from-a-private-ring. §5.4.
7. **Streamline's interposer deployed as `sl.interposer.dll`.** Off the table by **user
   constraint** (`docs/STRAY-RENDERING-FACTS.md` §32.5), and separately measured to fail once in
   a configuration that included ReShade. **Restated as SOFT** rather than as the settled
   technical finding the document's opening claimed. §5.0.
8. **Hudless + UI tagging, and User Interface Recomposition.** Narrowed: the **UI half is dead
   for this title regardless of reachability** — NVIDIA's alpha mask depends on the
   `DrawClearQuadAlpha` engine patch (absent from stock 4.27), and NVIDIA's own README tells HDR
   titles, which we are, to disable UI tagging. Recomposition needs both buffers and is therefore
   doubly out. The **hudless half alone** is the remaining prize, and it is a view-extension
   problem rather than a DXGI-hook one. §3.
9. **VSync-with-FG, multi-viewport, dynamic-resolution+FG.** Unchanged: none apply to Stray's
   fixed single-viewport, fixed-screen-percentage, gamescope-composited configuration. §6.

---

## What could not be obtained, stated plainly — REVISED 2026-09-03

* **`sl.dlss_g.dll` / `sl.dlss_d`'s source.** Still closed, at every tag. Everything about the
  interpolation algorithm, the exact ASCD math and the pacer's scheduling formula rests on
  `ProgrammingGuideDLSS_G.md` prose (**SOFT**) or on `docs/STRAY-RENDERING-FACTS.md` §32's binary
  string extraction (**HARD** for names, **UNCONFIRMED** for the logic). First-party access to
  NVIDIA's *engine* fork did not change this — the plugin never calls into DLSS-G's evaluate at
  all, so there was never a caller-side view of it to obtain.
* **Confirmation of the §5.3 crash hypothesis.** Unchanged: a reasoned argument, never tested.
  This was a report-only task and the box is owned by another agent this session.
* **Whether `NvAPI_D3D12_NotifyOutOfBandCommandQueue` is required or merely correct.** Unchanged
  and still **UNCONFIRMED** after reading both NVIDIA integrations.
* **Streamline SDK 2.12.0's source versus the 2.11.1 the current plugin vendors.** The public SDK
  read by the first pass is *newer* than the vendored one; no diff between them was performed, so
  a 2.12.0-only behaviour could be attributed here to Streamline generally. **SOFT** wherever the
  distinction could matter.
