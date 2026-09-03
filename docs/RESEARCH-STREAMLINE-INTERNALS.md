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
>
> ## SECOND PASS, same day: §7 answers "should we REPLACE ours with Streamline?"
>
> Because the downgrade above reopens a question this project had treated as closed, **§7 is a
> new dated section that answers it directly.** Verdict: **KEEP OURS — and the reasoning we had
> for keeping it was wrong.** "Streamline was measured to break FG under vkd3d-proton" was never
> established; the measurement contained ReShade by construction and the crash signature is
> ReShade's. The real reasons are (a) **the pacer is closed and this platform breaks pacers** —
> we already found and fixed a ~1 ms Wine timer skew in code we own, and could not have fixed it
> in `sl.dlss_g.dll`; (b) we would be replacing a system measured at **2.00× steady state** with
> an untested one; (c) the gains are all worth zero on a single-viewport UE 4.27 title on Ada.
> **§7.6 names a one-launch experiment that could still overturn it, and it should be run.**
> **§7.2b is the honest counterweight**: a line-by-line inventory of our own FG path found
> twelve structural defects — VSync unhandled, device removal unhandled, the mirror
> desynchronising on a fullscreen toggle without a resize, and two outright bugs. The verdict
> survives, but only on the narrower claim that ours is correct *for the user's configuration*
> and that its defects are in code we can reach.
>
> **§7.2c locates Streamline's platform gates; §7.2d corrects what they mean.** The pacer really
> is built on `NvAPI_SetFlipConfig`, `GetFrameStatistics`, `WaitForVBlank` and Independent Flip.
> But **§7.2c's prediction that Streamline would refuse to run here was WRONG**: Proton
> implements HWS via an NVIDIA-authored spoof, NVIDIA's maintainer states *"Currently Proton is
> supported"*, and DLSS-G runs under Proton in the wild. The verdict survives on **measured**
> grounds instead — DXVK's presentation-timing APIs are approximate, and
> **[vkd3d-proton#2325], "DLSS FG causes stuttering and breaks VRR", has been open since
> 2025-02-01** in exactly the 165 Hz VRR + gamescope configuration the user runs.
> **§7.2d also retires the OptiScaler mystery**: "DLSSG as FG Output" is documented **unsupported
> by OptiScaler's own maintainers**, which is a third and more parsimonious explanation of the
> six-launch death than either ReShade or the fullscreen transition.

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

## 7. NEW 2026-09-03 — Should we REPLACE our present owner with Streamline? A decision, argued

The re-review above downgraded "Streamline breaks frame generation under vkd3d-proton" from a
settled finding to **SOFT**, and §5.1/§5.2 examined Streamline's own proxy objects for the
obvious hazard and found them **clean**. That reopens a question this project had treated as
closed. This section answers it.

**Verdict, up front: KEEP OURS — and the reasoning we had for keeping it was wrong.** The
right reasons are different from the one that was being repeated, one of them is strong enough
to be nearly decisive on its own, and none of them is "Streamline was measured to fail."
Section 7.6 names a cheap experiment that could still overturn this, and it should be run.

### 7.0 The reasoning that was wrong, stated plainly

The claim in circulation was: *Streamline's swapchain path was measured to break frame
generation on this target, deterministically, across six launches.* **Streamline was never
convicted, and the evidence does not support that sentence.**

* **The configuration contained ReShade by construction.** OptiScaler was installed as
  `dxgi.dll` with `LoadReshade=true`, because at that time ReShade was *how this project
  loaded at all*. Every one of those six launches had ReShade's D3D12 proxy in the process.
* **The crash signature is ReShade's, not Streamline's.**
  `EXCEPTION_ACCESS_VIOLATION **writing** address `0x000000020000000d`` — a bit-packed,
  handle-shaped value used as a destination pointer. `CLAUDE.md` §1 documents exactly this
  family: ReShade's `IID_ID3D12DeviceExt` handler patches vtable slots 7/8 so descriptor
  handles run through `convert_to_original_cpu_descriptor_handle`, which has no release-build
  validation, and the patch is a **process-wide static vtable** that one `QueryInterface` from
  anywhere installs for every caller. A ReShade-minted synthetic handle reaching vkd3d
  unconverted, and being written to, produces precisely this. **SOFT** (a signature match and a
  mechanism, not a proof) — but it is a far better fit than anything in Streamline's own code,
  which §5.2 checked directly.
* **`ext_unhook` could not have protected it.** Our repair runs immediately before *our own*
  NGX calls. DLSS-G's internal NVAPI calls, made from inside the closed snippet at present
  time, were never covered.
* **And we no longer run behind ReShade.** The plugin loads through UE4SS; `mods/StrayDLSS/
  README.md:33` documents **Config A (no ReShade)** as a supported, ordinary configuration.

**There is a second, independent confound that is NOT ReShade, and it must not be dropped.**
`CLAUDE.md` records that the OptiScaler death came *"about a second after `SetFullscreenState
Fullscreen: 1`"*. So the deaths correlate with **the fullscreen transition** as well as with
ReShade's presence. Two confounds, not one. That matters in both directions: it further
weakens the case against Streamline (the transition is a hard moment for anyone), and it
weakens any confident claim that removing ReShade alone would fix it.

**What this means for the decision.** "We cannot use Streamline" was doing work it had not
earned. The honest position is: **Streamline's viability on this stack is UNCONFIRMED — never
tested without ReShade in the process.** The rest of this section therefore argues the merits
rather than leaning on a measurement that does not exist.

**One constraint is real but is not a technical finding.** `docs/STRAY-RENDERING-FACTS.md`
§32.5 records the interposer as off the table as a **hard constraint from the user**
(2026-09-02). That is a legitimate reason not to build on it and it stands until the user says
otherwise — but it is **a decision, not evidence**, and it should never again be cited as
though the SDK had been shown to fail.

### 7.1 Feature by feature: Streamline against what we built

Sources: `NVIDIA-RTX/Streamline` @ **v2.12.0** (`e8aaa6eaac`, public) for the SDK;
`NvRTX/UnrealEngine` @ `dlss3/sl2-4.27-dlss-plugin` (`32c3e4d5`) and `nvrtx-5.8_prerelease`
(`5b89940f`) for NVIDIA's own use of it; our own tree at `3224c46`.

| Capability | Streamline | Ours | Honest assessment |
|---|---|---|---|
| **Hooking model** | Full COM **wrapper objects** for device/queue/list/swapchain, substituted via inline trampolines on the exports (`sl.interposer/hook.cpp`). By default it also **modifies the DXGI factory v-table** | **V-table slot patches** on the game's real objects (`vtable_patch.cpp`, 108 lines) + MinHook on exports. No new object identity is ever created | **Ours is the smaller surface** and cannot leak a proxy pointer into a driver path, because there is no proxy. Streamline's is more general (it can intercept calls we never see). Roughly equivalent for our needs; ours is safer on a translation layer specifically because a wrapper that escapes is the classic vkd3d hazard |
| **Swapchain ownership** | Owns it end to end; host renders off-screen; **teardown + recreate on every FG on/off toggle** | Game keeps its real swapchain object; we substitute the **back buffers** via a hooked `GetBuffer` (`fg_present.cpp`, `replacement[kMaxReplacements]`) | Both achieve "host never touches a real swap-chain buffer" (§5.4 correction). Ours does it without a teardown path. **Equivalent in effect, lighter in mechanism** |
| **Present model** | Presents **once**; extra frames injected inside the interposed swapchain by the closed `sl.dlss_g.dll` | Presents **twice** from a dedicated worker thread | Genuinely different. Ours is measured at exactly `issued = 2 × game_presents − 2` (facts §32.7). Neither is obviously better; theirs needs the interposer |
| **Frame pacing** | Closed. Guide §13.1 describes an async "SL pacer" using **"specialized hardware to delay the image after Present()"** | `core::fg::Schedule` — phase-locked to the previous real present, 16-sample median interval, hitch re-anchor, **and a 1.5 ms spin window because `condition_variable::wait_for` wakes ~1 ms late under Wine** | **This is the decisive row. See 7.2.** |
| **Resource tagging** | `setTag` with a lifecycle enum; `eValidUntilPresent` (pointer only) or `eOnlyValidNow` (SL makes a GPU copy) | `GuideSet g_guides[2]` — we always make our own owned copies, double-buffered | **Equivalent, and ours is the stricter mode** — the same one NVIDIA's own plugin chose. Confirmed in §2 |
| **Reflex** | **Mandatory.** `eFailReflexNotDetectedAtRuntime`; the 8.7.2 plugin `checkf`s the status **every frame** | Best-effort and **non-gating**: we call the five NVAPI entry points, log the result, and proceed regardless (`fg_reflex.cpp`, 215 lines) | **Ours is weaker in principle and more robust here.** Facts §32.7: the calls return `NVAPI_OK` through DXVK-NVAPI 0.9.2, but *what they do* under vkd3d/gamescope is **UNCONFIRMED**. Streamline would gate on a subsystem whose real behaviour on this stack is unknown |
| **Camera-cut / reset** | UE plugin: `bReset = View.bCameraCut`, with an open `// TODO STREAMLINE check for other conditions` on **both** branches | 3-signal OR (cut flag ∥ jitter `zw==xy` ∥ 1×1 history/velocity) | **Ours is ahead of NVIDIA's shipped code.** Unchanged from the first pass |
| **Output validation** | Auto Scene Change Detection inside the closed snippet (input-side) | **Crop readback gate**: 3 consecutive good looks to validate, and it **revokes** on a bad one, with per-reason refusal counters (`fg_present.cpp:642-662`) | **We have something Streamline does not** at the integration level — but it is a 64×64 centre crop, and a generated frame byte-identical to the real one (a pure copy, not an interpolation) is weighted *good* and validates the feature. It proves "something plausible reached the screen", not "a frame was interpolated". **See §7.2b** |
| **Back-buffer index** | **Requires** `GetCurrentBackBufferIndex`; `eFailGetCurrentBackBufferIndexNotCalled` is a documented failure bit | `GameIndexMirror` models the engine's `++ % N` counter | Ours is correct **only against an engine that manually increments** — true for stock 4.27, false for UE 5.6+. A stated precondition, not a bug |
| **Resolution / fullscreen** | Teardown + recreate | Drain the worker, idle the GPU, bump an **epoch**, drop and re-arm replacements | Ours is measured through `SetFullscreenState(TRUE)` + `ResizeBuffers` and three checkpoint reloads (facts §32.7, §32.10) — **deliberately, because that is where OptiScaler died** |
| **Multi-viewport / multi-swapchain** | Multi-**viewport** is view-id keyed. Multi-**swapchain** is **also one only** — `dxgiSwapchain.cpp:152`: *"Streamline supports only one DXGISwapChain"* | **Assumes one** (`fg_present.cpp:301`; a second logs an error and is passed through untouched) | **Corrected in §7.2c.** The one-swapchain limit is **shared with the reference**, not a place we fall short. Only multi-viewport is genuinely theirs |
| **VSync** | Handled; `bIsVsyncSupportAvailable` is part of the API | **Unhandled.** Both presents reuse the game's sync interval verbatim; nothing forces `sync=0` on the generated one | **A genuine weakness that would bite on a config change.** Validated only at VSync off on a VRR panel. §7.2b |
| **Device removal / TDR** | Handled | **Zero handling** — no `DXGI_ERROR_DEVICE_REMOVED` anywhere in the native backend, and an internal failure returns `E_FAIL`, which UE4's `PresentChecked` does not treat as fatal | §7.2b |
| **MFG (>2×)** | Supported | `MultiFrameCount=1` | Not applicable — Ada's ceiling is 2×. Same outcome |
| **Diagnostics / knobs** | Debug overlay in non-shipping builds; `onErrorCallback` | 12 `NgxFG*` knobs including `NgxFGPacing`, `NgxFGTrace` (per-present timestamp trace), `NgxFGValidate`, plus per-reason refusal counters | **Ours is substantially better for this project's actual workflow** — a round trip to the user's box is expensive, and these are what make one launch diagnostic |

### 7.2 The decisive row: the pacer, and why it is not a fair fight on this platform

**Frame generation is a pacing problem.** A generated frame that is correct but delivered at
the wrong instant is worse than no frame at all — the user's own verdict on the first working
build was *"frame pacing is visibly wrong with FG on"*, with the image itself fine.

This project then **found and fixed a Wine-specific timing defect** (facts §32.11, HARD, from a
per-present timestamp trace):

```
[fg-trace] gaps: 7.1 5.0 7.1 5.3 7.1 4.8 7.1 5.1 ...     <- before
[fg-trace] gaps: 6.1 6.2 6.1 6.1 6.1 6.1 6.1 6.1 ...     <- after (165 Hz panel period = 6.06 ms)
```

The cause was that `std::condition_variable::wait_for` **wakes about 1 ms late under Wine**, so
a 6.1 ms hold came out at 7.15 ms, every frame. The fix was to spin the last 1.5 ms on the
worker thread and to anchor the schedule on the previous *real* present.

**Now apply that to Streamline.** Its pacer is inside `sl.dlss_g.dll`, which is closed and is
not present in the open repository at any tag (§0). It was written for Windows and its
documented mechanism is *"specialized hardware to delay the image after `Present()`"* — a
display-stack facility, not a portable CPU wait.

Two possibilities, and **both are bad for us**:

1. **It uses the same class of OS wait primitive.** Then it inherits the same ~1 ms Wine skew,
   and produces the same 7.1/5.0 alternation we just spent a session removing — **and we cannot
   fix it**, because the code is closed. We would be trading a defect we diagnosed and repaired
   for the identical defect with no recourse.
2. **It really does use a hardware present-timing path.** Then it depends on the Windows
   display stack (WDDM present statistics / HWS-era scheduling), and there is no reason to
   expect that to exist through vkd3d-proton, DXVK's DXGI, and gamescope's Wayland compositor.
   It would degrade to something unspecified, again unfixable.

**This is the single strongest argument in the section, and it is specific to our platform
rather than a general preference.** **SOFT** — it rests on the guide's prose plus our own
measurement of Wine's timer behaviour, not on reading Streamline's pacer, which cannot be read.
But the asymmetry is real and it does not require the hypothesis to be exactly right: *any*
pacing defect in a closed component is unfixable, and we have already demonstrated that this
platform produces pacing defects that a Windows-developed component would not have anticipated.



### 7.2b Where ours is genuinely weaker — the honest defect list

**This subsection exists because the first draft of §7 was too kind to our own code.** It called
our implementation "a working, measured system" and left it there. It *is* that — but a
line-by-line inventory of the FG path (this tree, `3224c46`) turns up a specific defect list,
and an argument for keeping our own code is worthless if it will not look at it. Everything
below is **HARD**, read from our own source. None of it is flagged in-code: a scan for
`TODO`/`FIXME`/`XXX`/`HACK` across the whole FG and present path returns **zero hits**, so these
are structural, not known-and-deferred.

**Would bite on a configuration change — the class the question asks about:**

| # | Defect | What triggers it |
|---|---|---|
| 1 | **VSync is unhandled.** Both presents reuse the game's sync interval verbatim (`fg_present.cpp:1239`, `p.sync = args.sync`). Nothing forces `sync=0` for the generated present. The entire pacer was validated only at `bUseVSync=False` on a 165 Hz VRR panel | **Turning VSync on.** Both presents block on vblank, the phase-locked schedule becomes meaningless, and the game is throttled toward half rate |
| 2 | **`SetFullscreenState` without a following `ResizeBuffers` desynchronises the mirror silently.** `fg_plan.hpp:22` asserts a Resize follows every fullscreen change, but `after_reconfigure(drop_replacements=false)` does not reset the mirror (`fg_present.cpp:1092-1095`). The WARP test states the gap explicitly (`warp_fg_present.inc:385-386`) | A fullscreen toggle that does not resize. **Failure mode is a stale presented frame, with no detector** |
| 3 | **Device removal is unhandled and can be masked.** Grep for `DXGI_ERROR_DEVICE_REMOVED` across the whole native backend: **zero hits.** Worse, `present_one` returns `E_FAIL` without calling the original `Present` when `GetBuffer` or the list ring fails (`fg_present.cpp:693-701`) — and `E_FAIL` is *not* one of the three codes UE4's `PresentChecked` treats as fatal | A device reset / TDR. UE4 ignores the code and continues against a dead device |
| 4 | **The HRESULT the game receives is one frame stale** in the default threaded mode (`fg_present.cpp:1299` returns `g_last_present_hr`, the *previous* frame's result; on the first present it is the `S_OK` initialiser) | Any fatal present error is reported at least one frame late |
| 5 | **A reconfiguration silently drops the game's frame.** `run_pair` (`:763-769`) presents the real swapchain without copying the replacement the game just rendered into — the displayed frame is whatever an earlier copy left there, and `note_issued_present()` is skipped so the counters under-report | Every resize/fullscreen transition drops one frame |
| 6 | **`GetCurrentBackBufferIndex` (slot 36) is not hooked.** Anything that queries it sees the *real* ring, which advances **twice** per game frame | Any code — engine, overlay, or a future UE patch — that asks DXGI instead of counting |
| 7 | **Exactly one swapchain.** `fg_present.cpp:301`, `Chain g_chain;`. A second logs an error and is passed through untouched | A video player, a second window, an overlay with its own chain |

**Latent bugs, independent of configuration:**

| # | Defect |
|---|---|
| 8 | **The FG worker thread bypasses the own-code guard.** `in_own_code()` is `thread_local` (`native_backend.cpp:25,71-73`) and `fg_present.cpp` never establishes an `OwnCodeScope` on the worker. The worker calls `ListRing::begin()` → `list->Reset(...)`, and `Reset` **is** vtable-patched (`d3d12_hooks.cpp:1087`), so `hk_List_Reset` delivers **our own FG present list into the root-signature shadow and the application sink as if it were the game's**. Unguarded, untested |
| 9 | **`ReleaseFeature` runs without a GPU idle.** `NgxGenerator::g_queue` (`ngx_fg.cpp:519`) is **never assigned**; `release_feature(queue)` → `wait_idle(nullptr)` returns immediately (`:167-168`). So `ngx_fg.hpp:70`'s documented contract — "Releases the feature and every texture. GPU idle first." — **is not met**, on every size change and at shutdown |
| 10 | **`g_cfg.enabled` is mutated from inside hooks without synchronisation** (`fg_present.cpp:899,1046,1052`) and read lock-free from six places. One failure permanently disables FG for the session with **no re-arm path** |
| 11 | **Every barrier `StateBefore` in the FG path is an assumed constant**, never queried. `PRESENT` and `COMMON` are both `0`, so the real-back-buffer assumption is invisible until it is wrong. This is the same gap `docs/RESEARCH-DLSS-UE5-PLUGIN.md` §7.4 identifies — NVIDIA asks the engine's resource-state tracker; we cannot |
| 12 | **Config is startup-only.** Every `NgxFG*` knob is read once, in `install()` (`present_owner.cpp:578`), despite `configure()` containing live worker start/stop logic that only the WARP test exercises |

**And the crop gate is narrower than §7.1 gives it credit for.** It is a **64×64 centre crop**, so
a generated frame that is black everywhere except the centre passes. More pointedly,
`CropVerdict::identical` — the generated crop being *byte-identical* to the real one, i.e. a pure
copy rather than an interpolation — is weighted **good** (`fg_plan.cpp:269`) and will **validate
the feature**. So the gate proves "something plausible reached the screen", not "a frame was
interpolated". That is still more than Streamline offers at the integration level, but the §7.1
row overstated it.

**Costs Streamline's design would not pay:** a **1.5 ms busy-spin on a dedicated core, every
present** (`fg_present.cpp:731` — the Wine workaround), **two full back-buffer `CopyResource`s per
game frame**, three separate command-list rings plus three fences, and a `Map` + reduce of two
64×64 readbacks **on the game thread** every present.

**Our own docs already record the deepest uncertainty.** `docs/STRAY-RENDERING-FACTS.md:1383`,
verbatim: *"HARD that the names are read; the **VALUES' conventions (MvecScale, matrices'
handedness, CameraFar=0) are UNCONFIRMED until an interpolated frame is judged on the box**"* —
and `:1330` calls the sign convention *"the design bet of this branch"*. So the parameter
correctness that §7.4 says Streamline would not improve is, strictly, **not yet established for
ours either**; what is established is that the pipeline runs and the output passes a coarse gate.

**Does this change the verdict? No — and the reason is the point.** Every defect above is in code
**we own and can fix**, most of them cheaply (force `sync=0` on the generated present; reset the
mirror on any reconfigure; propagate the real HRESULT; add an `OwnCodeScope` on the worker; assign
`g_queue`). Item 8 and item 9 are real bugs that should be filed regardless of this decision.
The argument for keeping our implementation was never that it is flawless — it is that **its
flaws are reachable**. §7.2's pacer argument is the same argument in its strongest form.

**What this does change: the confidence with which §7.5 can say "a working, measured system."**
It works, and is measured, *in one configuration* — windowed-fullscreen, VSync off, VRR, one
swapchain, three back buffers, no device resets. That is exactly the configuration the user
runs, which is why it has never bitten. It is not a general implementation and should not be
described as one.

### 7.2c PARTLY SUPERSEDED — the platform gates, located in Streamline's own source

> **Read §7.2d immediately after this section.** The source reading below is accurate and worth
> keeping; **its headline conclusion — that HWS is the likeliest blocker — is WRONG**, because
> Proton implements HWS. The section is preserved because locating the gates is useful and
> because the error is instructive.

§7.2 offered two possibilities for the pacer and §7.6 left HWS as **UNCONFIRMED**. Both are now
answered from `NVIDIA-RTX/Streamline` @ **v2.12.0** (`e8aaa6eaac`), plus string/import analysis of
the shipped `sl.dlss_g.dll` (**the FG plugin is closed — `source/plugins/sl.dlss_g` does not exist
in the repository**; findings from the binary are marked **HARD (binary)**).

**§7.2's possibility (2) is CONFIRMED: the pacer really is a display-stack mechanism, and it is
the part least likely to survive translation.** DLSS-FG's pacer ("RSync") drives itself from:

* **`NvAPI_SetFlipConfig`** — a *private* NVAPI flip-metering entry point, V1 and V2, with
  fallback logging (`Driver reports SetFlipConfig V2 support`, `Failed to set flip config V2,
  falling back to V1`). **HARD (binary).**
* **`IDXGISwapChain::GetFrameStatistics`** in a warm-up loop (`RSYNC: getFrameStats not working
  during warmup (initial call), will keep retrying.`, `RSYNC: getFrameStats failed, invalidating
  warmup.`), **`IDXGIOutput::WaitForVBlank`** on a dedicated `thread.rsync-vblank`, and
  **`GetFrameLatencyWaitableObject`** flip-queue throttling. **HARD (binary).**
* **Independent Flip (IFLIP)**, a DWM-bypass presentation mode, for the VSync+FG path
  (`ProgrammingGuideDLSS_G.md` §22).

DXGI presentation timing is historically the weakest area of any translation layer, and gamescope
is a Wayland compositor with no DWM and no IFLIP. **So the pacer would not merely inherit Wine's
timer skew — its inputs largely do not exist here.** This closes §7.2's argument at HARD rather
than SOFT: we would be handing pacing to a closed component whose timing sources are absent.

**The HWS gate: located, and it fails CLOSED with no bypass.** `sl.common`'s
`commonInterface.cpp:150-256` queries it entirely through **WDDM kernel-mode thunks in
`gdi32.dll`** — `D3DKMTEnumAdapters2` + `D3DKMTQueryAdapterInfo` with
`KMTQAITYPE_WDDM_2_7_CAPS`, reading `D3DKMT_WDDM_2_7_CAPS::HwSchEnabled`, matched against the
DXGI adapter LUID. Then `sl.cpp:792-801`:

```cpp
bool required = cfg["hws"]["required"];
bool detected = cfg["hws"]["detected"];
if (required && !detected) {
    SL_LOG_ERROR("Feature '%s' requires GPU hardware scheduling to be enabled in the OS", ...);
    return Result::eErrorOSDisabledHWS;
}
```

Three properties make this the likeliest single blocker, and all three are **HARD**:

1. **It is a WDDM concept, not a D3D12 one.** There is no WDDM on Linux.
2. **Enumeration failure is indistinguishable from HWS being off.** If `D3DKMTEnumAdapters2`
   fails, SL logs a warning, zeroes the struct, skips the loop, and leaves `hwsSupported =
   false`. There is no fallback and no NVAPI alternative.
3. **`eBypassOSVersionCheck` does not help.** It gates the *OS version* comparison only. **No
   flag bypasses HWS.** And `hws.required` is set by the closed `sl.dlss_g` itself (the string
   table at `0x74d28` carries `hws`, `detected`, `required`, `supported`), so it cannot be
   configured away either.

**A second fail-closed gate the first draft did not know about: Authenticode.**
`security::loadLibrary` (`secureLoadLibrary.cpp:31-41`) gates every `LoadLibraryW` of an SL plugin
behind `verifyEmbeddedSignature`, which needs `wintrust.dll!WinVerifyTrust` with
`WSS_GET_SECONDARY_SIG_COUNT | WSS_VERIFY_SPECIFIC`, requires **exactly one secondary signature**,
and walks the nested-signature OID `1.3.6.1.4.1.311.2.4.1` through nine `crypt32.dll` entry points
to confirm NVIDIA is the second signer. Failure message: *"Streamline will not load unsecured
modules."* **HARD.** This depends on the OS trust store and Wine's `wintrust`/`crypt32`
completeness — another gate that reports unavailable rather than degrading.

**Corrections to §7.1 this forces:**

* **"Multi-swapchain: Streamline supported, ours assumes one" was WRONG.** Streamline supports
  exactly one too — `dxgiSwapchain.cpp:152`: *"Streamline supports only one DXGISwapChain
  (currently it is 0x%p). Skipping some Present() hooks…"*. **HARD.** Our one-swapchain
  assumption (§7.2b item 7) is a limitation we *share with the reference*, not a place we fall
  short. Multi-**viewport** is view-id keyed and genuinely theirs; multi-**swapchain** is neither's.
* **Our replacement-back-buffer trick is what DLSS-FG does too.** It creates its own internal
  swapchain and hands the application **cloned fake back buffers** — binary strings
  `cloneFakeBuffers: ENTRY - width=%u…`, `tex2d.fake-swapchain-buffer`, and `GetBuffer` /
  `GetCurrentBackBufferIndex` are declared `"exclusive_hooks"` in its plugin JSON. **HARD
  (binary).** §5.4's correction was right and is now doubly so: we independently reinvented the
  reference's own mechanism.
* **Reflex's mandatory status is structural, not stylistic.** `sl.dlss_g`'s plugin JSON lists
  `"required_plugins": ["sl.common", "sl.reflex"]`, and the guide states that *"any existing
  regular Reflex SDK integration (not using Streamline) cannot be used by DLSS-G"* — so our
  working DXVK-NVAPI Reflex would not satisfy it; SL's own would have to work. Note also that SL
  calls `NvAPI_D3D_GetLatency` with a **newer-than-public struct layout**
  (`NV_LATENCY_RESULT_PARAMS_V1_BFM_36495674`, `chi/generic.cpp:1252-1276`), which DXVK-NVAPI has
  no reason to match.

**One finding that cuts the other way, and it deserves recording.** The *only* mention of
vkd3d/Proton/Wine/Linux in Streamline's entire source tree is a deliberate accommodation for our
stack (`chi/d3d12.cpp:425-428`):

```cpp
// Shared fences are needed for DX11 interop (DX11-on-DX12 scenario).
// For pure DX12 titles, non-shared fences avoid inefficient shared semaphore
// GPU waits in vkd3d-proton (Linux DX12-to-Vulkan translation layer).
```

**HARD.** NVIDIA knows vkd3d-proton exists and has tuned at least one thing for it. That is weak
evidence of viability — one comment is not a support commitment — but it is honest to note that
the SDK is not oblivious to the platform.

**Two operational notes for anyone running §7.6.** OTA is **on by default**
(`Preferences::flags` defaults to `eAllowOTA | eLoadDownloadedPlugins`) and uses `wininet` plus
`CreateProcessW` on `nvngx_update.exe` — clear those flags before testing anything. And SL raises
the **global** timer resolution to 0.5 ms via `NtSetTimerResolution`
(`slPreferredTimerRes = 5000UL`), which is a system-wide side effect worth knowing about under
Wine.


### 7.2d CORRECTION 2026-09-03, hours later — §7.2c's headline prediction is WRONG

**§7.2c predicted that Streamline would most likely refuse to run here on the HWS gate. That
prediction does not survive the ecosystem evidence, and it should not have been made with the
confidence it was.** The source reading behind it is accurate — the gate exists, it is a WDDM
kernel-thunk query, it fails closed, no flag bypasses it. What was wrong was the unstated
assumption that **Wine does not implement those thunks.** It does.

**Wine/Proton implements Hardware-accelerated GPU Scheduling. HARD.**

* Proton exposes it as a compat option, `nohardwarescheduling`, and as
  `WINE_DISABLE_HARDWARE_SCHEDULING=1` — i.e. HWS is **on by default** and there is a switch to
  turn it *off*.
* The Wine HWS patch was authored by **Liam Middlebrook of NVIDIA**, the same engineer behind
  vkd3d-proton's `DXVKInteropDevice1` and its `VK_NV_optical_flow` queue — both commits dated
  2024-06-23, the latter's message reading *"Needed for implementation of nvofapi64 DLL D3D12
  functionality in dxvk-nvapi."*
* dxvk-nvapi **v0.8.0**'s release notes state the chain outright: the `nvofapi64` pass-through
  for *"DLSS3 / Frame Generation"* **"requires VKD3D-Proton 2.14 and a Wine implementation with
  support for hardware scheduling (HAGS)."**
* It shipped in **Proton 9.0-4, 2024-12-11**.
* **Important qualifier:** the HWS implementation is a spoof that lives **only in Proton's Wine
  fork** — upstream Wine does not implement it, and there is no spoofing anywhere in
  vkd3d-proton, DXVK or dxvk-nvapi. Our target runs GE-Proton, which is Proton-derived, so it
  should carry it; that is **UNCONFIRMED** for this specific build and is one thing the §7.6 run
  would establish for free.

**So the enabling stack for Streamline DLSS-G under Proton was built deliberately, largely by
NVIDIA, and our own Proton 11.0-2 / vkd3d-proton master / dxvk-nvapi 0.9.2 is exactly the
supported configuration.**

**And NVIDIA says so.** On the official repo, [NVIDIA-RTX/Streamline#105 "Linux support"], the
maintainer `jake-nv` wrote on 2026-04-09: **"Currently Proton is supported."** **SOFT** — a
maintainer comment on an issue, not documentation — but it reverses NVIDIA's earlier posture and
is corroborated by the code above.

**Two more §7.2c claims that need softening:**

* **`NvAPI_SetFlipConfig` is not simply absent.** dxvk-nvapi **v0.9.2** — our exact version —
  ships a `SET_FLIP_CONFIG_V2` implementation, its release note reading: *"Implement several
  NVAPI entrypoints needed for **limited/incomplete support for Dynamic Multi Frame
  Generation.** This requires updated DLSS snippets and Streamline."* **HARD.** What
  "limited/incomplete" excludes is not enumerated anywhere, so the pacer's inputs are **partial,
  not missing** — a materially weaker claim than §7.2c made.
  **But partial is the operative word, and it still favours §7.2's conclusion.** DXVK provides
  the `IDXGISwapChain4` façade and vkd3d-proton the presenter backend, so every interface
  Streamline hooks exists — yet **`GetFrameStatistics` and `WaitForVBlank` are approximate and
  log warnings**, and on the kernel-thunk side `D3DKMTWaitForVerticalBlankEvent`,
  `GetPresentHistory`, `Present` and `GetDeviceState` are **absent from `gdi32`'s export table
  entirely** (`GetProcAddress` returns NULL), with `QueryStatistics` a stub returning
  `STATUS_SUCCESS` and writing nothing. **HARD.** So a pacer built on presentation timing runs
  here on approximations — which is exactly the shape of the open VRR/stutter bug below.
* **Authenticode verification evidently passes**, since Streamline DLSS-G demonstrably runs
  under Proton in the wild (below). §7.2c was right that it is a fail-closed gate and wrong to
  imply it is an obstacle.

**The strongest correction of all: the OptiScaler dead end was neither ReShade nor Streamline.**
OptiScaler's own wiki lists **"DLSSG as FG Output"** among options that *"aren't supported and
may be implemented down the line."* A `framegen/dlssg/DLSSG_Dx12.cpp` exists on master but is not
a shipped feature. **HARD.** `CLAUDE.md` already recorded that no published build would accept
`FGOutput=dlssg` and that it *"exists only on master"* — what is new is that it is **unfinished
upstream, by upstream's own account**, not merely unreleased.

**So the six-launch death now has THREE candidate explanations, not two**: ReShade's ext-vtable
patch (§7.0), the fullscreen transition (§7.0), and **an OptiScaler code path its own maintainers
document as unsupported**. The third is the most parsimonious, and it further weakens any claim
against Streamline itself — which is the direction §7.0 was already pointing, now with a better
reason.

**What actually works on Linux today, and it is the mirror image of what we tried.** DLSSG via
**Streamline as FG *input*** with **FSR-FG/XeFG as output** is reported working — OptiScaler
issue #1100, 2026-08-16, Alan Wake 2 on Arch + GE-Proton 11.5: *"Frame Generation set to 'DLSSG
via Streamline' — works correctly and stably, including through loading screens and cutscene
transitions."* The failing arm in that same report was *"DLSSG via NvNGX"* → black-screen hang.
**SOFT** (a user report), but it is the only positive datapoint anyone has.

#### Does the verdict change? No — and one piece of evidence now supports §7.2 better than my own reasoning did

**Keep ours still stands, but the load-bearing argument shifts from structure to measurement.**

* **§7.2's pacer conclusion survives, on better evidence than the argument I gave it.**
  [vkd3d-proton#2325], **open since 2025-02-01**: *"DLSS Frame Generation causes stuttering and
  breaks VRR"* — five titles, RTX 4070 Super; Cyberpunk 2077 retained VRR but showed a double
  image. And vkd3d-proton `c8c8ab50` (2026-04-09) fixed *"DLSS Frame Generation causing Reflex
  desync and game freezes"*, where DLSS-G presenting interpolated frames without new latency
  markers made Streamline conclude `eDLSSGStatusFailReflexNotDetectedAtRuntime` and freeze the
  game — *"particularly when switching workspaces on Wayland compositors."* **HARD.** We have
  that fix, but the pattern is the point: **frame pacing for Streamline DLSS-G under
  Proton/Wayland is a known, open, actively-worked problem in someone else's tree.** Our own
  pacer, whatever its defects (§7.2b), produces a measured 6.0-6.2 ms cadence on this exact
  panel today.
* **The cost/benefit is unchanged.** Everything in §7.3-§7.5 stands: ~3,100 lines and 803 test
  lines deleted, everything re-measured, gains worth ~zero on a single-viewport UE 4.27 title on
  Ada.

**But my reasoning was wrong twice in one section, and the pattern is worth naming.** §7.2c
argued from *structure* — "this is a WDDM concept, Linux has no WDDM, therefore it cannot work" —
without checking whether the compatibility layer implements it. That is the same error class as
§7.0's original sin (concluding from a confounded measurement) and as §5.4's flipped feasibility
claim ("cannot render off-screen from outside" — we already do). **Three times in this document a
confident structural argument has been overturned by looking at what actually exists.** The rule
this earns: *when the argument is "platform X cannot do Y", go and check whether someone has
already made it do Y — especially when the vendor has an incentive to.*

### 7.3 What replacing it would actually cost

**Deleted or reduced to a shim** (exact line counts, tree at `3224c46`):

| File | Lines | Fate |
|---|---|---|
| `fg_present.cpp` + `.hpp` | 1516 | **Deleted** — replacement ring, pacer, worker, crop gate, epochs |
| `ngx_fg.cpp` + `.hpp` | 679 | **Rewritten** — `DLSSG.*` becomes `slDLSSGSetOptions` + tags |
| `core/fg_plan.cpp` + `.hpp` | 628 | **Deleted** — `Schedule`, `GameIndexMirror`, `CropJudge`, `IntervalHistogram`, `Epoch` |
| `fg_reflex.cpp` + `.hpp` | 278 | Deleted; SL drives Reflex |
| `present_owner.cpp` + `.hpp` | 757 | **Partially** — it is *not* FG-only; its header calls it "the frame boundary the seam's `on_present` needs — without ReShade", which the SR path also uses. Only the FG branches go |
| `core/present_plan.cpp` + `.hpp` | 42 | Deleted |
| `tests/test_fg_plan.cpp` | 310 (8 cases, 107 checks) | **Deleted** — the entire CI-testable surface of our FG work |
| `tests/warp/warp_fg_present.inc` | 493 | **Deleted** — the WARP present-twice harness |

**Honest figure: ~3,100 production lines are FG-only** (`fg_present` + `ngx_fg` + `fg_plan` +
`fg_reflex` + `present_plan`), plus the FG branches inside `present_owner`, plus **803 lines of
tests**. The commonly-quoted "3,500" over-counts by treating `present_owner` as FG-only; it is
not, and the SR path needs it either way.

**Added:**

* Ship and load `sl.interposer.dll`, `sl.common.dll`, `sl.dlss_g.dll`, `sl.pcl.dll`,
  `sl.reflex.dll` — NVIDIA binaries with a signature-checking loader (`sl_security.h`).
* Rework our hook layer so SL, not us, wraps device/queue/swapchain — and reconcile that with
  the SR path, the TAA dispatch interception, and the descriptor/root shadow, which all assume
  they are looking at the game's real objects.
* Convert guide publication to `setTag` with view ids and frame tokens.

**Re-measured from scratch** — this is the part that dominates the cost, because every one of
these is a box round trip and the box is a shared, contended resource:
DLSS-G creation and evaluate; the 2.00× steady-state ratio; **pacing on the 165 Hz VRR panel**;
the fullscreen transition; three checkpoint reloads; Reflex behaviour; interaction with
gamescope; and the SR path, which currently shares the device and command lists.

### 7.4 What we would gain — specifically

1. **We stop owning the pacer, the ring and the epoch machinery.** ~3,500 lines and a whole
   class of bug (present ordering, resource lifetime across a resize) become NVIDIA's problem.
   This is the only *large* gain and it is real.
2. **Multi-swapchain / multi-viewport correctness** we currently do not have. **Worth nothing
   for Stray**, which has one of each.
3. **Forward compatibility**: MFG beyond 2× (Blackwell only — worthless on a 4090), Latewarp,
   Dynamic FG, and whatever comes next, without us implementing each.
4. **`eUseFrameBasedResourceTagging`** and the frame-token model, which is a cleaner solution to
   guide freshness than our consumption-sequence counter. Marginal.
5. **Reflex done properly**, including `NvAPI_D3D12_NotifyOutOfBandCommandQueue` — the one call
   §4 identifies as missing. **But we can simply make that call ourselves**, which is exactly
   what recommendation #1 already says.

**Note what is NOT on this list: image quality.** Both paths drive the same closed
`nvngx_dlssg.dll` with the same guides. Streamline would not generate a better frame; it would
schedule and present it differently.

### 7.5 What we would lose

1. **A working, measured system — in one configuration.** 2.00× steady-state, correct
   6.0-6.2 ms cadence on the user's panel, surviving the fullscreen transition and three
   checkpoint reloads, zero `[ERROR]` lines, no Xid. **HARD** (facts §32.7-§32.11). Replacing
   this with something untested on this stack is the core of the trade — **but read §7.2b before
   weighting this too heavily**: "working and measured" holds for windowed-fullscreen, VSync
   off, VRR, one swapchain, three back buffers and no device resets, which is precisely the
   user's configuration and precisely why the defect list has never bitten.
2. **The ability to fix pacing at all** (7.2).
3. **The validation gate and the per-reason refusal counters.** These are how a single launch
   tells us *why* a frame was not generated (`source-missing`, `not-validated`,
   `no-previous-frame`). Streamline offers a debug overlay in non-shipping builds and an error
   callback. Given that every diagnosis costs a round trip to the user's machine, this is a
   larger loss than it looks.
4. **Our 12 `NgxFG*` knobs**, including `NgxFGTrace`, which is what diagnosed the pacing fault.
5. **Independence from Reflex's real behaviour on this stack** (7.1).
6. **The user's stated constraint** would have to be revisited — which is their call, not ours.

### 7.6 REDESIGNED 2026-09-03 — the experiments, cheapest first

> **The original §7.6 proposed re-running the OptiScaler + Streamline configuration with
> `LoadReshade=false`. That experiment is now largely pointless and is withdrawn as the headline
> recommendation.** §7.2d establishes that OptiScaler's own maintainers document **"DLSSG as FG
> Output" as unsupported** — so the original six launches were driving an unfinished upstream
> code path, and re-running it without ReShade would most likely reproduce the death for that
> reason and teach us nothing about Streamline. The confound the first draft was proud of
> removing was not the only one, and probably not the important one.

Three experiments, in ascending cost. **Only the first is recommended now.**

**A. `NVPRESENT_ENABLE_SMOOTH_MOTION=1` — one environment variable, zero integration.**
NVIDIA's Linux driver exposes driver-level frame generation that is, in NVIDIA's own words,
*"transparent to the game engine and gets applied automatically"* (driver installation guide,
gaming section; driver README). No Streamline, no NGX feature, no swapchain proxy, nothing to
hook. **SOFT** that it works under gamescope specifically — that is exactly what the run would
establish.

Worth trying **before** any further Streamline investigation, with two caveats stated up front:
NVIDIA warns that *"Native DLSS Frame Generation and Smooth Motion are competing technologies
and should not be used together"*, so it must be tested with our own FG **off**; and if it works
it is a different product from ours — no guide control, no validation gate, no per-reason
refusals, and no way to reason about what it does on a camera cut. **It would not replace our
implementation so much as make the question moot for a user who just wants more frames.** That
is a legitimate outcome and the user should get to choose it.

**B. Streamline as FG *input*, FSR-FG/XeFG as output, through OptiScaler.** This is the mirror
image of what was tried, and the only configuration with a positive Linux report (§7.2d,
OptiScaler #1100 — Alan Wake 2, GE-Proton 11.5, *"works correctly and stably"*). It would tell us
whether Streamline's DLSS-G **input** path functions on this box without requiring its present
path to work. Moderate cost, genuine information, **but it does not test what §7 is about** —
it routes around the swapchain question rather than answering it.

**C. Native Streamline DLSS-G, via `eUseManualHooking`.** The real test, and now clearly
possible: the host loads `sl.interposer.dll` explicitly and keeps its own hooks, routing a
mandatory list (`CreateSwapChain*`, `Present`, `Present1`, `GetBuffer`, `ResizeBuffers`,
`ResizeBuffers1`, `GetCurrentBackBufferIndex`, `SetFullscreenState`, `CreateCommandQueue`)
through SL's proxies (`docs/ProgrammingGuideManualHooking.md`, **HARD**). No OptiScaler, no
`dxgi.dll` substitution. **This is real integration work, not a config flip**, and §7.7 says not
to do it. Recorded so that if it is ever wanted, the shape is known.

**If any of these runs, what to look for**, in order — this list is the durable part of the
original §7.6 and survives its redesign:

| Log line | Means |
|---|---|
| `eErrorOSDisabledHWS`, `requires GPU hardware scheduling`, `Adapter enumeration has failed` | The Proton HWS spoof is absent from this GE-Proton build (§7.2d). Structural, nothing to do with our code |
| `Streamline will not load unsecured modules` | Authenticode verification failed under Wine |
| `eDLSSGStatusFailReflexNotDetectedAtRuntime` | The Reflex presentID desync class — vkd3d-proton `c8c8ab50` fixed one instance of this; our build has the fix |
| `eFailGetCurrentBackBufferIndexNotCalled` | The §7.1 back-buffer contract |
| `eFailHDRFormatNotSupported` | Stray presents `R10G10B10A2`, which should be fine, but SL rejects FP16/scRGB |

**Set before any run:** clear the default OTA flags (`eAllowOTA | eLoadDownloadedPlugins`, on by
default) so nothing tries `wininet` or spawns `nvngx_update.exe`.

**And a standing hazard to weigh against all three:** [vkd3d-proton#2325] — *"DLSS Frame
Generation causes stuttering and breaks VRR"* — has been **open since 2025-02-01** across five
titles. The user runs a 165 Hz VRR panel through gamescope. Any Streamline-based FG path is
walking into a known-unfixed bug in precisely our display configuration.

### 7.7 Verdict: KEEP OURS, and adopt the pieces — but for the right reasons

**Not replace. Not a hybrid in the architectural sense** — the pacer is inside `sl.dlss_g.dll`,
driven by the interposed present, and cannot be taken without the swapchain. **Hybrid only in
the sense our existing recommendations already mean: adopt the specific, small things Streamline
does that we do not.**

> **One correction to this paragraph, 2026-09-03.** The first draft said Streamline must own the
> DLL chain. **It need not** — `eUseManualHooking` is a documented mode in which the host loads
> `sl.interposer.dll` explicitly and keeps its own hooks (§7.6). But the mandatory hook list
> still includes `CreateSwapChain*`, `Present`, `GetBuffer`, `GetCurrentBackBufferIndex` and
> `SetFullscreenState`, so SL still ends up owning the swapchain and the pacer still arrives
> with it. **The conclusion is unchanged; the reason "there is no way" was overstated.**

**The argument, in order of weight:**

1. **The pacer is closed, and its timing sources largely do not exist here** (7.2, **now
   confirmed at HARD in 7.2c**). This was a hypothesis in the first draft; Streamline's own
   source and binary settle it. RSync paces from `NvAPI_SetFlipConfig` (a *private* NVAPI
   entry), `IDXGISwapChain::GetFrameStatistics`, `IDXGIOutput::WaitForVBlank`, waitable-object
   flip-queue throttling and Independent Flip — DXGI presentation timing and a DWM-bypass mode,
   under a Wayland compositor that has neither. We already found and fixed one Wine timer defect
   in code we own; here we would be handing pacing to a closed component whose *inputs* are
   missing, with a failure mode that is visible to the user and unfixable by us.

   **1b. RETRACTED — see §7.2d.** This slot previously read *"Streamline may simply refuse to
   run"* on the HWS and Authenticode gates. **Proton implements HWS** (an NVIDIA-authored spoof,
   shipped since Proton 9.0-4), NVIDIA's own maintainer states *"Currently Proton is
   supported"*, and Streamline DLSS-G demonstrably runs under Proton in the wild. **That
   argument is withdrawn.**

   **What replaces it is better, because it is measured rather than deduced.**
   [vkd3d-proton#2325], *"DLSS Frame Generation causes stuttering and breaks VRR"*, has been
   **open since 2025-02-01** across five titles — and the user runs a 165 Hz VRR panel through
   gamescope. Meanwhile DXVK's `GetFrameStatistics` and `WaitForVBlank` are **approximate and
   log warnings**, and `D3DKMTWaitForVerticalBlankEvent` / `GetPresentHistory` / `GetDeviceState`
   are absent from `gdi32` entirely. So Streamline's pacer would run on approximations, into a
   known-unfixed bug, in exactly our display configuration — while ours produces a measured
   6.0-6.2 ms cadence on that panel today.
2. **We would be replacing a measured system with an unmeasured one**, and the measured one
   already hits the theoretical maximum (2.00× steady state; the 1.91× headline is startup and
   reload frames that are legitimately not generatable).
3. **The gains do not land on this title.** Multi-swapchain, multi-viewport, MFG beyond 2× and
   dynamic FG are all worth zero on a single-viewport UE 4.27 game on Ada. The one gain that is
   real — deleting 3,500 lines — is a maintenance argument, and those lines are currently
   working, unit-tested where testable, and instrumented.
4. **The one concrete capability Streamline has that we lack is one call**, not an architecture:
   `NvAPI_D3D12_NotifyOutOfBandCommandQueue`. We can make it ourselves.

**And the part that matters even though the conclusion is unchanged: the reason we had was
wrong.** "Streamline was measured to break FG under vkd3d-proton" was never established — the
measurement contained ReShade by construction, the crash signature is ReShade's, and
Streamline's own proxy code was checked and found clean. **That sentence should stop being
repeated.** The correct statement is:

> *Streamline's viability on this stack is untested. We keep our own implementation because it
> works, is measured, is instrumented, and — critically — because frame pacing on Wine is a
> problem we must be able to fix ourselves.*

**What would change the verdict:** our pacer proving unfixable for some case we cannot handle;
a second swapchain or viewport appearing; a move to Blackwell where MFG >2× has value; or the
7.6 experiment showing Streamline working *and* pacing correctly under gamescope. The last of
those is the only one that is cheap to check, which is why it is worth running anyway.

**One caveat on how this verdict was reached, stated because it nearly went the other way by
default.** The first draft of §7 argued "keep ours" while describing our implementation only in
terms of what it achieves. §7.2b was added after a line-by-line inventory turned up twelve
structural defects, two of them real bugs, none flagged in-code. **The verdict survives that —
but the argument had to be rebuilt to survive it**, and it now rests on a narrower claim: not
that our implementation is good in general, but that it is correct *for this configuration* and
that its defects are in code we can reach. An argument for keeping your own code that has not
looked at your own code is not an argument.

---

## Recommendations, ranked — RE-RANKED 2026-09-03

> The first pass's ranking is largely retained: its two adoptable items both survive first-party
> checking. What changed is the confidence attached to the diagnostic section, and the removal of
> one feasibility claim that had flipped.
>
> **Updated again for §7's replace-or-keep decision.** The verdict is **keep ours**, so nothing
> below becomes obsolete — but item 0 is new and outranks everything else on cost-to-value, and
> item 1 (`NvAPI_D3D12_NotifyOutOfBandCommandQueue`) is now doing more work than before: §7.4
> identifies it as **the only concrete capability Streamline has that we lack**, which makes it
> the whole of the "hybrid" option rather than a nicety.

### Adopt into our own code (independent of Streamline's swapchain — safe under vkd3d-proton)

1. **Call `NvAPI_D3D12_NotifyOutOfBandCommandQueue` on the present owner's queue for the
   generated-frame submissions.** Small, concrete, matches Streamline's own Reflex plumbing; we
   already have the marker-level equivalent (`out_of_band_present_start/end`) and are missing the
   queue-level counterpart. **HARD** that Streamline makes the call and we do not.
   **UNCONFIRMED** that omitting it has any consequence — no source read on either pass states
   one. Adopt as tidiness with a plausible latency-accounting benefit, not as a fix. §4.

   > **PROMOTED 2026-09-03, and now verified available on our exact stack.** §7.4 finds this is
   > **the only concrete capability Streamline has that we lack** — everything else it offers is
   > either architectural (and comes bundled with its swapchain) or worth zero on this title. So
   > this one call *is* the entire "hybrid" option, which raises its value considerably.
   >
   > And it is genuinely reachable, which was not previously checked: **DXVK-NVAPI implements
   > it** — `src/nvapi_d3d12.cpp:1088`, `NVAPI_FUNCTION
   > NvAPI_D3D12_NotifyOutOfBandCommandQueue(ID3D12CommandQueue*, NV_OUT_OF_BAND_CQ_TYPE)` —
   > and it is **not a stub**: it forwards to vkd3d-proton's own
   > `device->NotifyOutOfBandCommandQueue(...)` through the vendor interface declared in
   > `src/interfaces/vkd3d-proton_interfaces.h`, with distinct handling for
   > `OUT_OF_BAND_IGNORE` and `OUT_OF_BAND_RENDER_PRESENT`. **HARD**, read from
   > `jp7677/dxvk-nvapi` @ `master`.
   >
   > `NV_OUT_OF_BAND_CQ_TYPE_OUT_OF_BAND_PRESENT` is the value that matches our generated-frame
   > present. Still **UNCONFIRMED** what vkd3d-proton does with it at the driver level, and still
   > not a fix for any observed symptom — but the "we could not use it anyway" objection is now
   > closed.
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

### Try this first — one environment variable

0. **`NVPRESENT_ENABLE_SMOOTH_MOTION=1`, with our own FG off.** NVIDIA's Linux driver does
   frame generation at the driver level, *"transparent to the game engine"*, with no Streamline,
   no NGX feature, no swapchain proxy and nothing to hook. **SOFT** that it works under
   gamescope — that is what the run establishes. If it works it does not replace our
   implementation (no guide control, no validation gate, no per-reason refusals) but it **may
   moot the whole question for a user who simply wants more frames**, and they should get to
   choose that. NVIDIA warns it must not be combined with native DLSS-FG. §7.6-A.

   > **This replaces the previous item 0**, which proposed re-running OptiScaler with
   > `LoadReshade=false`. **Withdrawn**: §7.2d found OptiScaler documents *"DLSSG as FG Output"*
   > as **unsupported by its own maintainers**, so those six launches were driving an unfinished
   > upstream path and re-running them would most likely reproduce the death for that reason and
   > teach us nothing about Streamline. The confound the first draft removed was real but was
   > probably not the important one.

### Fix these regardless of the Streamline question — found while writing §7.2b

0a. **Two real bugs, neither flagged in-code, both cheap.** (i) The FG **worker thread never
   establishes an `OwnCodeScope`** (`in_own_code()` is `thread_local`), so its
   `ID3D12GraphicsCommandList::Reset` is delivered into the root-signature shadow and the
   application sink **as if it were the game's** (`fg_present.cpp` ↔ `d3d12_hooks.cpp:1087`).
   (ii) **`NgxGenerator::g_queue` is never assigned**, so `release_feature()` → `wait_idle(nullptr)`
   returns immediately and `NVSDK_NGX_D3D12_ReleaseFeature` runs **without the GPU idle its own
   header promises** (`ngx_fg.cpp:167-168, 519`; `ngx_fg.hpp:70`), on every size change and at
   shutdown. **HARD**, both read from source. §7.2b.

0b. **Three cheap robustness fixes** that close the configuration-change gaps: force `sync=0`
   on the generated present (VSync is currently unhandled and would throttle the game); reset
   the `GameIndexMirror` on **any** reconfigure, not only one that drops replacements (a
   `SetFullscreenState` without a following `ResizeBuffers` desynchronises it silently, and the
   failure mode is a stale frame with no detector); and propagate the real present `HRESULT`
   rather than the previous frame's, so a device-removed code is not delayed or masked as
   `E_FAIL`. §7.2b.

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
   constraint** (`docs/STRAY-RENDERING-FACTS.md` §32.5) — **a decision, not evidence**. It was
   separately measured to fail once, in a configuration that contained ReShade by construction
   and whose crash signature is ReShade's. **Restated as UNCONFIRMED**, not as the settled
   technical finding the document's opening claimed. **§7 now argues the full replace/keep
   decision on the merits**, and the answer is still keep — but for reasons that survive the
   suspect being acquitted. §5.0, §7.
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
