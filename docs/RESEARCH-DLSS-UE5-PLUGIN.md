# Auditing stray-dlss against NVIDIA's current UE5 DLSS / Streamline plugin

> ## RE-REVIEWED 2026-09-03 AGAINST FIRST-PARTY NVIDIA ENGINE SOURCE — read this box first
>
> The first pass (2026-09-03, commit `dd38dd5`) recorded, in its own "What could not be
> obtained" section, that NVIDIA's real distribution channel **`NvRTX/UnrealEngine` returned
> HTTP 404** and that every claim therefore rested on `XPOL555/unreal-engine-5.7-dlss4`, a
> **third-party mirror of an NVIDIA sample project**. That access has since been obtained.
> **This document now rests on first-party NVIDIA and Epic source.** Sources, with the exact
> branch and commit SHA read, are in §0.
>
> **Where the first pass was right, this re-review says so** — and it was right more often than
> not. Its two headline findings (the `CameraFar` default, and that our reset detection is
> ahead of NVIDIA's own FG code) are both **confirmed against first-party source on our exact
> engine version**, which is a stronger footing than they had. Its motion-vector,
> dilation-default and `DoSharpening` findings also hold.
>
> **What changed materially:**
>
> 1. **§0 — provenance.** The mirror is superseded throughout. Where the mirror and first-party
>    source disagree, first-party wins and the correction is marked inline.
> 2. **§1 and §5.2 — the reachability question was answered with the wrong route, and the
>    conclusion was too pessimistic.** Stock UE **4.27**'s `ISceneViewExtension` already
>    carries **both** `BeginRenderViewFamily(FSceneViewFamily&)` (where the temporal-upscaler
>    slot is claimed) **and** `SubscribeToPostProcessingPass` with
>    `EPostProcessingPass::VisualizeDepthOfField` (the exact hudless capture point NVIDIA
>    uses). One mechanism unlocks both, and it exists in the engine Stray actually runs. The
>    first pass named a materially harder route (AOB-scanning `AddPostProcessingPasses`) and
>    graded the difficulty from that. See the new §1.1.
> 3. **§4 — the preset table is stale again.** The first pass read plugin **8.3.0**, where
>    preset E is deprecated. NVIDIA's own release notes record E **restored in 8.7.0**. See §4.
> 4. **§5.2 — the "NVIDIA also just uses an alpha heuristic" reading was wrong in a way that
>    matters.** That heuristic is backed by a mandatory **engine patch** (`DrawClearQuadAlpha`,
>    absent from stock 4.27; the plugin `#error`s without it) which *guarantees* the backbuffer
>    alpha is a meaningful UI mask. It is not a post-hoc trick we could copy from outside.
> 5. **New §7 — what NVIDIA changed in the UE 4.27 engine, and what we need instead.** The
>    single most useful artifact obtained: the engine diff is a concrete, itemised statement of
>    what an engine must provide to host DLSS/Streamline, and for each item the answer to
>    "do we need the equivalent?" is now recorded. Two of NVIDIA's 4.27 patches were later
>    **upstreamed by Epic into UE5**, which is the clearest available evidence of what NVIDIA
>    learned and the engine adopted.
>
> Superseded reasoning is preserved inline rather than deleted, per `CLAUDE.md`'s house style.

`docs/RESEARCH-OFFICIAL-DLSS-UE-PLUGIN.md` (828 lines, 2026-08-31) already established the
architecture question — hook vs. stage, `ITemporalUpscaler`, `ISceneViewExtension`, Streamline's
present-time model — against a UE5-era mirror of a DLSS2/3-vintage plugin and the generic
Streamline 2.12.0 SDK guides. That document is not repeated here. This one exists because NVIDIA
kept shipping: DLSS4, Multi Frame Generation, new presets, a real DLSS4 UE5.6 sample project with
a bundled Streamline Core plugin. The question is narrower and more concrete: **now that we can
read the actual current source, where does our hand-built SR/FG implementation diverge from what
NVIDIA does today, and is any divergence worth fixing?**

Companion documents, not repeated: `docs/RESEARCH-OFFICIAL-DLSS-UE-PLUGIN.md` (architecture),
`docs/RESEARCH.md` (NGX D3D12 SDK / UE 4.27 internals, 228 claims), `docs/STRAY-RENDERING-FACTS.md`
§32 (the raw `nvngx_dlssg.dll` parameter contract, reverse-engineered from Streamline 2.12.0's
snippet binary — cited extensively below because it turns out to cross-validate against the real
UE5 source read here), and the `dlss-exposure` branch (auditing our exposure handling against the
UE4 plugin — not duplicated; §3 below covers only what changed for UE5/DLSS4).

**Architecture note, since the companion document is ReShade-era and this one must not silently
inherit that framing.** `docs/RESEARCH-OFFICIAL-DLSS-UE-PLUGIN.md` (2026-08-31) describes "our"
side as a ReShade add-on intercepting the TAA compute dispatch through ReShade's device/
command-list event API. **That is no longer how this project loads or hooks.** As of this
writing we are a **UE4SS C++ plugin** (`mods/StrayDLSS`, built by
`.github/workflows/dlss-plugin.yml` against RE-UE4SS) loaded in-process on the game's main
thread, ahead of `D3D12CreateDevice`; the actual D3D12 interception — device/command-queue/
command-list/swapchain vtable patching, our own descriptor and root-argument shadow
(`src/backend_native/descriptor_shadow.cpp`, `root_shadow.cpp`), the TAA-dispatch capture, the
present-twice frame-generation path (`src/backend_native/present_owner.*`, `fg_present.*`) — is
a plain native hook layer (`src/backend_native/`) that does not depend on ReShade's add-on
events at all. A `src/backend_reshade/` backend still exists in the tree; it is not how the
project is run today. `docs/RESEARCH-UE4SS-MIGRATION.md` (2026-09-01, HARD, this repo) is the
assessment that led to this, and its verdict matters directly for §1 below: **UE4SS itself
contributes UObject/Blueprint reflection, cvars, and deterministic camera control — it
"contains no D3D12 or DXGI code whatsoever... and cannot contribute a line of it"** to the
render-level interception, which remains a plain Win32/D3D12 hook exactly as it was under
ReShade. Where a claim below depends on this distinction, it says so explicitly; the
TAA-dispatch interception *technique* itself (identify by hash, capture by register, replace
the dispatch) is unchanged — only the mechanism that installs the hooks and the process
that hosts them changed.

**Provenance labels**, per `CLAUDE.md` §0.5: **HARD** — read directly from the source quoted;
**SOFT** — documentation, release notes, a vendor blog, or a claim whose engine/SDK version
differs from what we read; **UNCONFIRMED** — searched for and not found, or inferred without a
citation, with a note on what would confirm it.

---

## 0. Sources examined, and how far each can be trusted

> **REVISED 2026-09-03.** The first pass could not reach NVIDIA's own repository and said so
> plainly — the honest call, and it is what made this re-review worth doing. Access has since
> been obtained. The table below now leads with first-party source; the mirror row is retained
> at the bottom, demoted, so that any claim still resting on it is visible.

**Primary sources, all first-party, all read directly over the GitHub API on 2026-09-03:**

| Source | Branch / ref | Tip SHA read | What it is | Trust |
|---|---|---|---|---|
| `NvRTX/UnrealEngine` | `nvrtx-5.8_prerelease` | `5b89940f9436ab7475d547e322443efe6d875ac2` (2026-07-24) | NVIDIA's UE fork carrying the **current** DLSS4 / Streamline plugins under `Engine/Plugins/Runtime/Nvidia/` (`DLSS`, `Streamline`, `StreamlineCore`, `StreamlineDLSSG`, `Reflex`, `NIS`, `NRD`). Its own `PublicReleaseNotes.md` dates the build: **plugin 8.7.2, July 2026, Streamline SDK 2.11.1, NGX SDK 310.6.0**, UE 5.8→5.6 supported | **HARD** — first-party, versioned by NVIDIA's own release notes |
| `NvRTX/UnrealEngine` | `dlss3/sl2-4.27-dlss-plugin` | `32c3e4d5e0ee9ea7b792d1d09bcc2549917ad252` (2024-03-26) | **NVIDIA's DLSS plugin for UE 4.27 — our exact engine version.** 14 commits on top of the `4.27.2 release` commit `d94b38ae3446`. Ships DLSS-SR 3.7.0 and **DLSS-FG/Streamline 1.3.0-SL2.4.0**, plus the engine patches they require | **HARD**, and the most directly applicable source in this document |
| `NvRTX/UnrealEngine` | `dlss-streamline-4.27-engine-changes` | `e1ee6efc0d4efda533fb71060b2387a698073dba` (2022-09-24) | The **engine-side** changes for Streamline on 4.27: 5 commits, 42 files, on top of the same `4.27.2 release` base. Read as a cumulative diff `d94b38ae3446...e1ee6efc0d4e`. See §7 | **HARD** |
| `EpicGames/UnrealEngine` | tags `4.27`, `5.0`, `5.4`, `5.6`, `5.8` | (read by ref) | **Stock, unmodified Unreal Engine** — used to establish what Stray's own engine does and does not already contain, and which of NVIDIA's patches Epic later adopted upstream | **HARD** |

**Secondary / supporting:**

| Source | What it is | Trust |
|---|---|---|
| `NVIDIA-RTX/Streamline` `changelog.txt`, `docs/ProgrammingGuideDLSS_G.md` | NVIDIA's public SDK repo | **HARD** for doc text; the doc is prose, not the code that implements it |
| Web search (DLSS 4.5 / multi-frame-gen announcements) | NVIDIA/press, January 2026 CES and later | **SOFT** — used only to date-stamp; no claim below rests on it |
| `XPOL555/unreal-engine-5.7-dlss4` | The third-party mirror of an NVIDIA UE 5.6 sample project that the **first pass** relied on for everything. Plugin `8.3.0-NGX310.4.0`, bundled Streamline `2.9.0` | **SUPERSEDED.** Retained only to explain where a first-pass claim came from. Where it and first-party source disagree, first-party wins and the correction is marked inline. Its age is now itself a finding: 8.3.0 predates the preset-E restoration in 8.7.0 (§4) |

**A note on line numbers.** First-pass citations of the form `DLSSUpscaler.cpp:598-628` are line
numbers *in the mirror*, not in any NVIDIA release. Where this re-review re-verified a claim, it
cites the first-party file and branch instead. Where a first-pass line-numbered citation is left
standing, it is because the *substance* was re-verified first-party and only the line number is
mirror-relative — those are marked. **Do not treat an unmarked mirror line number as a
first-party citation.**
> **The first pass's own source table, superseded 2026-09-03 and preserved for the lesson.**
> The rows it carried listed `Plugins/DLSS/DLSS.uplugin` at `8.3.0-NGX310.4.0-EXPERIMENTAL_FORUM`
> and `StreamlineCore.uplugin` at `8.3.0-SL2.9.0`, and recorded the NvRTX branches as
> "**Could not obtain — 404 over the API, gated.**" All three are now superseded by the
> first-party table above. **The lesson is worth keeping**: the first pass established the
> mirror's authenticity by intact NVIDIA copyright headers, which was a reasonable test and gave
> the right answer about *authorship* — but authorship is not *currency*. The mirror was a
> genuine NVIDIA 8.3.0 tree, and 8.3.0 was already four plugin releases behind by the time it was
> read, which is exactly how the stale preset table in §4 survived the first pass. **An intact
> copyright proves who wrote it, never when.**

---

## 1. Where DLSS is placed in the frame

`docs/RESEARCH-OFFICIAL-DLSS-UE-PLUGIN.md` §A.1–A.2 and §D.2 established that `ITemporalUpscaler`
is a genuine engine interface, unreachable from ReShade's D3D12 event model. **That framing is now
stale in one specific way and needs re-examining, not re-argument-by-inertia, because we are no
longer a ReShade add-on.** We are a UE4SS C++ plugin: a native DLL loaded in-process, ahead of
device creation, with UE4SS's UObject/Blueprint reflection and pattern-scan/hook infrastructure
available as tooling. The honest question is whether that changes the answer, and the honest
answer has two parts.

**What genuinely changed: nothing about `ITemporalUpscaler` itself is UObject-reflected, so UE4SS's
own reflection does not reach it — and this project's own prior assessment already establishes
that precisely, for the adjacent D3D12 problem.** `FSceneViewFamily`, `ITemporalUpscaler` and
`ISceneViewExtension` are plain internal renderer C++ classes with no `UCLASS`/`UFUNCTION`
metadata — UE4SS's reflection targets UObjects (as `mods/StrayDualSense` already exercises,
hooking `COMP_CatScratchableComponent_C:SetPS5TriggerActivated`, a genuine `UFUNCTION`), not
private renderer-thread C++. `docs/RESEARCH-UE4SS-MIGRATION.md`'s own verdict, reached from
inside this project for a closely related question, says the same thing in stronger terms: UE4SS
"contains no D3D12 or DXGI code whatsoever... and cannot contribute a line of it" to the
render-level interception, which the migration built as "a plain Win32/D3D12 hook layer" instead
(`src/backend_native/`) — precisely because UE4SS's tooling does not extend to this class of
internal engine object. `SetTemporalUpscalerInterface` sits in exactly that same class: reaching
it would need pattern-scanning and hooking a specific, private, renderer-thread function
(something like `AddPostProcessingPasses` or the game's analogue of
`SetupMainGameViewFamily`/`BeginRenderViewFamily`), with real UE 4.27.2 struct-layout knowledge of
`FSceneViewFamily` — not a UFunction call, not a UObject property read, and not anything UE4SS's
SDK-dumping tooling generates automatically.

**What genuinely did not change: this is now a substantially more *plausible*, still entirely
*unbuilt*, engineering problem — not a solved one.** Being a native in-process DLL loaded early is
necessary for this kind of hook and was, in fact, equally true of a ReShade add-on (also a native
in-process DLL) — so "in-process" alone was never the blocker the earlier framing implied, and is
not one now either. What UE4SS adds is tooling that lowers the *cost* of the remaining problem:
AOB/pattern-scan infrastructure, a live SDK dump for this exact game build, and (per the migration
assessment) a track record of this project already doing the harder, adjacent version of this work
— its own hand-built descriptor/root-argument shadow replacing ReShade's equivalent
(`docs/RESEARCH-UE4SS-MIGRATION.md` §1.1, "L, and the only item with real technical risk"). No
part of that shadow work, and nothing in the current `mods/StrayDLSS/src/Mod.cpp` (94 lines, the
*only* file that includes a UE4SS header at all — its `on_unreal_init` is empty, and none of the
render-path code touches a UObject), currently reaches `FSceneViewFamily` or
`ITemporalUpscaler`. The View constant buffer is still read by raw byte offset
(`CLAUDE.md` §2.6), and the TAA pass is still identified by DXBC hash and intercepted as a compute
dispatch (`src/taa_hook.cpp`) — exactly the ReShade-era technique, now installed through our own
vtable patches instead of ReShade's `addon_event::dispatch`.

**So: reclassify, do not repeat verbatim.** The old framing ("unreachable from ReShade's D3D12
event model") is not wrong, it is simply the wrong *reason* now — the real constraint was never
ReShade specifically, it is that `ITemporalUpscaler` is non-reflected internal engine state
requiring ABI-level knowledge to reach from outside, which is exactly as true for a UE4SS native
plugin as it was for a ReShade add-on. What UE4SS changes is that the *next* step toward reaching
it — AOB-scanning and hooking the specific renderer-thread function that registers it — is now a
scoped, tooled, and precedented kind of work for this project, rather than a wall. **Investigate,
long-horizon, not now**: this is squarely a candidate for a future `docs/RESEARCH-UE4SS-MIGRATION.md`-
style assessment of its own, not something to attempt as a side effect of this audit.

### 1.1 CORRECTION 2026-09-03: the route above is the hard one, and it is not the route NVIDIA uses

**The paragraph above is right that this needs ABI knowledge and wrong about how much.** It
names "AOB-scanning and hooking the specific renderer-thread function that registers it
(something like `AddPostProcessingPasses` or the game's analogue of `BeginRenderViewFamily`)"
and grades the difficulty from that. But `BeginRenderViewFamily` is not a private function to be
scanned for — **it is a pure-virtual method on `ISceneViewExtension`, a public engine interface
that a plugin is *supposed* to implement**, and it is the mechanism NVIDIA's own plugin uses.
The question is therefore not "can we find and hook a private function" but "can we register an
object with the engine's view-extension list", which is a materially different and smaller
problem.

**And the interface is already in the engine Stray runs. HARD**, read from stock
`EpicGames/UnrealEngine` @ tag `4.27`,
`Engine/Source/Runtime/Engine/Public/SceneViewExtension.h`:

```cpp
class ISceneViewExtension {
public:
    enum class EPostProcessingPass : uint32 {
        MotionBlur, Tonemap, FXAA, VisualizeDepthOfField, MAX          // :99-106
    };
    virtual void BeginRenderViewFamily(FSceneViewFamily& InViewFamily) = 0;              // :136
    virtual void PrePostProcessPass_RenderThread(FRDGBuilder&, const FSceneView&,
                                                 const FPostProcessingInputs&) {}        // :156
    virtual void SubscribeToPostProcessingPass(EPostProcessingPass Pass,
                     FAfterPassCallbackDelegateArray& InOutPassCallbacks,
                     bool bIsPassEnabled) {}                                             // :161
};
```

and, in the same stock 4.27 tree, `Engine/Source/Runtime/Engine/Public/SceneView.h:1807-1847`
carries `FSceneViewFamily::SetTemporalUpscalerInterface(const ITemporalUpscaler*)` and the
`TemporalUpscalerInterface` member itself. **`ITemporalUpscaler` is a stock UE 4.27 interface,
not a UE5 one.**

**And on 4.27 the registration is simpler still — it is a GLOBAL POINTER, not a view
extension.** This is the part that most changes the grading, and it comes from NVIDIA's own
4.27 plugin. `EpicGames/UnrealEngine` @ `4.27`,
`Engine/Source/Runtime/Renderer/Private/PostProcess/TemporalAA.h`, Epic-authored (commit
`22839a01`, guillaume abadie, "Makes TAAU plugable", 2020-07-31):

```cpp
class RENDERER_API ITemporalUpscaler { /* 5 pure virtuals: GetDebugName, AddPasses,
                                          GetMin/MaxUpsampleResolutionFraction */ };
extern RENDERER_API const ITemporalUpscaler* GTemporalUpscaler;
```

and NVIDIA's 4.27 plugin registers itself by **assigning three engine globals**
(`NvRTX/UnrealEngine` @ `dlss3/sl2-4.27-dlss-plugin`, `DLSS/Private/DLSS.cpp`):

```cpp
checkf(GTemporalUpscaler == ITemporalUpscaler::GetDefaultTemporalUpscaler(), ...);
GTemporalUpscaler           = DLSSUpscaler.Get();
GCustomStaticScreenPercentage = DLSSUpscaler.Get();
GCustomResourcePool          = DLSSUpscaler.Get();
```

with `class FDLSSUpscaler final : public ITemporalUpscaler, public ICustomStaticScreenPercentage,
public ICustomResourcePool`. **HARD.** On 4.27 the upscaler is **one process-wide singleton
selected by a writable global pointer** — `FSceneViewFamily::SetTemporalUpscalerInterface` and
the per-view-family object are the **UE5** design (`FDLSSSceneViewFamilyUpscaler`,
`FDLSSUpscalerViewExtension`), and on UE5 `GTemporalUpscaler` and
`GCustomStaticScreenPercentage` are gone entirely.

**Four consequences, revised:**

1. **On our engine, claiming the upscaler slot means writing one pointer.** Not hooking a
   function, not registering a view extension — locating `GTemporalUpscaler` in the shipped
   binary and storing a pointer to an object of ours that implements five virtuals. That is a
   materially smaller problem than anything either pass had previously described, and it is
   *the* mechanism on 4.27. **The first pass's difficulty grading was wrong, and so was the
   first draft of this section**, which reached for the UE5-era view-extension route because it
   was reading UE5-era source.
2. **The hudless capture (§5.2) is still the view-extension mechanism**, and that is separate.
   `SubscribeToPostProcessingPass(EPostProcessingPass::VisualizeDepthOfField, ...)` is on stock
   4.27's `ISceneViewExtension` with the enum value present, so it is available on our engine —
   but it needs `GEngine->ViewExtensions` (`Engine.h:1769`, a `TSharedPtr<FSceneViewExtensions>`
   and **not** a `UPROPERTY`, so UE4SS reflection will not yield its offset) and a correctly
   shaped vtable. `FSceneViewExtensions::NewExtension` is a **template**, so it is inlined into
   every caller and cannot be hooked. So §1 and §5.2 are *not* one problem after all — §1 is a
   global write, §5.2 is a view-extension registration. §1 is much the cheaper of the two.
3. **NVIDIA needed private engine headers, and so would we.** `DLSS.Build.cs` on 4.27 adds
   `Runtime/Renderer/Private` to `PrivateIncludePaths` because `ITemporalUpscaler` lives in a
   private renderer header; `NGXD3D12RHI.Build.cs` additionally reaches into
   `Runtime/D3D12RHI/Private` to get at `FD3D12Device`/`FD3D12CommandContext`. On UE5 the
   latter is gone — the RHI work moved to the public `ID3D12DynamicRHI` API. **We have neither,
   and would be reconstructing `ITemporalUpscaler`'s ABI (a 5-entry vtable plus the `FPassInputs`
   and `FRDGBuilder` layouts) by hand.** That is the real cost, and it is not small: `AddPasses`
   takes an `FRDGBuilder&` and an `FViewInfo&`, neither of which we can construct or safely
   traverse without 4.27.2 struct layouts.
4. **Stray is a monolithic shipping build**, so `RENDERER_API`/`ENGINE_API` buy us nothing —
   there are no exports, only addresses to be found.

**Provenance, stated exactly.** That `ITemporalUpscaler`, `GTemporalUpscaler`,
`ISceneViewExtension`, `SubscribeToPostProcessingPass` and
`EPostProcessingPass::VisualizeDepthOfField` all exist in **stock UE 4.27** is **HARD** (read
from Epic's own tag). That NVIDIA's 4.27 plugin registers via those three globals is **HARD**
(read from NVIDIA's own branch). That either route is *achievable from an external DLL against
a monolithic shipping build* is **UNCONFIRMED** — nobody in this project has attempted it, no
address for `GTemporalUpscaler` has been located, and a wrong vtable or struct layout is a
crash rather than a compile error.

**What is now HARD is that the earlier grading was wrong.** This was filed as "harder than a
hooked function call, needs the shipped game's engine source." The interfaces are public
stock-4.27 headers, the 4.27 registration is a single global assignment, and the RDG-side ABI
is the actual difficulty — a different and better-specified problem than the one the first pass
described. **It is still not cheap and still should not be started as a side effect of this
audit**; the first pass's scheduling advice ("its own scoped assessment") stands, and §7 below
adds a reason to be cautious about the whole direction.

**This does not make it cheap, and it should not be started as a side effect of this audit** —
the first pass's scheduling advice ("its own scoped assessment") stands. What changes is the
expected value of that assessment, and the fact that it now covers §1 and §5.2 together.

**What changed since that read (registration mechanism, UE5.4+):** DLSS SR now registers through
a real `ISceneViewExtension`, not only through `ICustomStaticScreenPercentage`.
`Plugins/DLSS/Source/DLSS/Private/DLSSUpscaler.cpp:598-628`,
`FDLSSUpscalerViewExtension::BeginRenderViewFamily`:

```cpp
if (!ViewFamily.GetTemporalUpscalerInterface())
{
    GetGlobalDLSSUpscaler()->SetupViewFamily(ViewFamily);
}
else
{
    UE_LOG(LogDLSS, Error, TEXT("Another plugin already set FSceneViewFamily::SetTemporalUpscalerInterface()"));
    return;
}
```

**HARD.** `SetupViewFamily` (`:1265-1341`) is what actually calls
`ViewFamily.SetTemporalUpscalerInterface(new FDLSSSceneViewFamilyUpscaler(...))` — same
mechanism the companion doc found, reached one layer further out. This is a UE-version-driven
refactor (view extensions are the modern, ordered way plugins hook a view family), not a change
in what the interface hands DLSS: still exactly the three textures plus two downsample hints
per `AddPasses`, nothing more.

**New in this refactor: a "Modular Feature" indirection.** `DLSSUpscaler.cpp:17`
(`#include "DLSSUpscalerModularFeature.h"`), gated `#if ENGINE_SUPPORTS_UPSCALER_MODULAR_FEATURE`
(`:506-513`, `:1286-1294`): another plugin can register a functor that overrides, per view,
whether DLSS's view extension is active this frame and which quality mode it should use. **HARD**
that this exists; **not applicable to us** — it is a plugin-to-plugin extension point inside the
engine's own C++ object graph, reachable only from inside the process's UE code, which is exactly
the "would need real engine execution, not a D3D12 hook" wall the companion doc already names.

**Ray Reconstruction: refined, not new.** The companion doc's §C read RR's placement from the
*generic* Streamline guide ("Default up-scaling pass like TAAU goes here"). The actual UE plugin
source is more precise, and worth stating exactly because it resolves an ambiguity the generic
doc left open — RR does **not** run as a second, independent evaluate layered after a denoiser
callback. It is the *same* `ITemporalUpscaler` object, parameterized:

```cpp
// DLSSUpscaler.cpp:415-421 (GetDebugName)
ENGXDLSSDenoiserMode DenoiserMode = GetDenoiserMode(Upscaler);
return (DenoiserMode == ENGXDLSSDenoiserMode::DLSSRR) ? GDLSSRRSceneViewFamilyUpscalerDebugName : GDLSSSceneViewFamilyUpscalerDebugName;
```

`FDLSSDenoiser` (`DLSSDenoiser.h:20`, `class DLSS_API FDLSSDenoiser final : public IScreenSpaceDenoiser`)
is registered too, but only to **skip** the engine's own screen-space denoiser passes when RR is
active (`DLSSDenoiser.cpp:44-49`, `SkipDenoiser` returns true whenever `r.NGX.DLSS.DenoiserMode`
says DLSSRR) — it performs no denoising work itself; the real RR evaluate happens inside
`FDLSSSceneViewFamilyUpscaler::AddPasses`, the identical call site SR uses. **HARD.** So: one
temporal-upscaler slot handles SR and RR both, plus a companion no-op-shaped `IScreenSpaceDenoiser`
registration that exists purely to stop the built-in SSD from doing redundant work. This
refines, and does not contradict, the companion doc's conclusion that RR occupies SR's slot.

**Quality modes: six now, not five.** `DLSSUpscaler.cpp:454` asserts
`static_assert(int32(EDLSSQualityMode::NumValues) == 6, ...)`; the new value is `UltraQuality`,
sitting between `Quality` and `DLAA` (`:657-663`, `ToNGXQuality` maps it to
`NVSDK_NGX_PerfQuality_Value_UltraQuality`). **HARD.** Our own `DlssQuality` enum
(`src/ngx_backend.hpp`) has `dlaa`, `max_quality`, `balanced`, `max_performance`,
`max_performance` (ultra-performance folded into max_performance's branch) — no `ultra_quality`.
**Investigate:** a render/output ratio that would select `UltraQuality` on the real plugin
currently falls into whichever of our five buckets its ratio threshold lands in; low priority,
since Stray only ever runs at the game's two shipped screen percentages (50%/70%, `CLAUDE.md`
§2.3.1) and neither is close to `UltraQuality`'s band.

---

## 2. Motion vectors

DLSS SR's own `VelocityCombine.usf` (`Plugins/DLSS/Shaders/Private/VelocityCombine.usf`) is
**unchanged** from what the companion doc already quoted verbatim (same decode, same
`View.ClipToPrevClip` row-vector `mul`, same `(0.5,-0.5)` scale-and-negate, same
`MotionVectorScale = (1,1)`, same dilate-by-default cvar). Not re-quoted.

**New: Frame Generation has its own, separate velocity-combine pass**, and it differs from DLSS
SR's in three ways worth recording.

**2.1 — An "alternate motion vectors" extension point exists, for exactly the problem
`CLAUDE.md` already named as unfixable from our hook.** `CLAUDE.md`'s NR section concludes,
after ruling out every other explanation for a persistent reflection-tracking artefact:

> "A reflection does not move with its surface — it moves with the reflected geometry... Treat
> 'temporal network + screen-space reflections' as a structural mismatch, not an open bug."

NVIDIA's own reference has a compile-time extension point for precisely this, on **both** the
SR combine and the FG combine, gated by the same flag:

```cpp
// VelocityCombine.usf (StreamlineCore), inside the non-dilated branch
#if SUPPORT_ALTERNATE_MOTION_VECTOR
    const float2 EncodedAltVelocity = AlternateMotionVectorsTexture[PixelPos];
    if (EncodedAltVelocity.x > 0.0f)
    {
        float2 DecodedVelocity = DecodeVelocityFromTexture(float4(EncodedAltVelocity, 0.0f, 0.0f)).xy;
        // we encode in the orientation DLSS expects, so the extra negate it to make them
        // consistent with the ones generated above
        OutVelocity = -1.0f * DecodedVelocity * CombinedVelocity_ViewportSize;
    }
#endif
```

with the comment at the call site (`DLSSUpscaler.cpp:797`, `AlternateMotionVectorTexture`):
"replacement motion vectors for items like reflections that DLSS might prefer to track." **HARD**
that this exists in both the DLSS SR path (`DLSSUpscaler.cpp:797-807`, `SUPPORT_GUIDE_GBUFFER`)
and the FG path (`StreamlineShaders/Private/VelocityCombinePass.cpp`,
`FSupportAlternateMotionVectorDim`). **HARD, and important**: `SUPPORT_GUIDE_GBUFFER` is `#define`d
`0` in this build (`StreamlineViewExtension.cpp:44-46`) — the extension point exists but is
**compiled out** in the shipped plugin. A title gets reflection-aware motion vectors only by
authoring its own "guide G-buffer" (`SceneTextures.AlternateMotionVector`) and flipping that
define — engine-level work, not a config flag. **Investigate, long-horizon, not now, and harder
than §1's `ITemporalUpscaler` question**: this is the one piece of evidence that NVIDIA's own
engineers consider the reflection-motion problem real enough to reserve plumbing for, but the
plumbing here is a *compile-time* `#define` inside the engine plugin's own source — it needs a
new member on `FSceneTextures` and a base-pass write into it, i.e. changes to Stray's actual
engine build, not merely a hooked function call at runtime. That is a materially harder wall than
§1's (reaching a runtime-registered interface via a pattern-scanned hook, from our own DLL) — it
needs the shipped game's engine source or bytecode-level patching of the compiled shader/engine
binary, neither of which the UE4SS migration (`docs/RESEARCH-UE4SS-MIGRATION.md`) provides or
was ever scoped to provide. Not reachable by this project's current architecture, UE4SS-based or
not.

**2.2 — FG's own dilation default is off, opposite of SR's.**
`r.Streamline.DilateMotionVectors` defaults to **0** (`StreamlineViewExtension.cpp:76-81`,
comment: "pass low resolution motion vectors into DLSS Frame Generation (default)") — versus
`r.NGX.DLSS.DilateMotionVectors` defaulting to **1** for SR. **HARD.** Consistent with our own
choice not to dilate (`shaders/mv_resolve.hlsl` produces one vector per render pixel, no
dilation, for either SR or FG): **do not adopt** dilation for FG guides, we already match
NVIDIA's own FG-side default; the earlier "we don't dilate, unlike SR's own default-1" note from
the companion doc applies to SR only, and does not apply to FG at all.

> **EXTENDED 2026-09-03 — NVIDIA changed its mind about SR dilation too, and we now match on
> BOTH paths.** The first pass's FG finding holds and is confirmed first-party:
> `r.Streamline.DilateMotionVectors` defaults to **0** on both `dlss3/sl2-4.27-dlss-plugin`
> (`StreamlineViewExtension.cpp:54-58`) and `nvrtx-5.8_prerelease`. **HARD.** What it could not
> see is that the **SR** side flipped between the two engine generations:
>
> | | UE 4.27 (DLSS-SR 3.7.0) | UE5 (8.7.2) |
> |---|---|---|
> | SR motion vectors | `r.NGX.DLSS.DilateMotionVectors` **default 1** (dilated, output-res) | **forced off — the cvar is gone** |
> | `MVLowRes` feature flag | conditional on `!bHighResolutionMotionVectors` | **unconditional** |
>
> 8.7.2's `DLSS/Source/DLSSUtility/Private/VelocityCombinePass.cpp` states it outright:
>
> ```cpp
> // neither current DLSS-SR, nor current DLSS-SR models/preset use dilated motion vectors.
> // Eventually we should remove the shader permutation but for now, we keep things the way they are in here
> const bool bDilateMotionVectors = false;
> ```
>
> **HARD.** The combined-velocity texture is consequently allocated at `InputViewRect.Size()`
> (render res) on UE5, where 4.27 allocated it at `GetSecondaryViewRectSize()` (output res).
> The shipped Programming Guide agrees: *"This is not the preferred option though, as the DLSS
> dll will typically apply its own optimized and targeted dilation which might result in higher
> quality."*
>
> **Consequence: our non-dilated, render-resolution, `MVLowRes`-flagged motion vectors match
> current NVIDIA practice on the SR path as well as the FG path.** The companion doc's "we don't
> dilate, unlike SR's own default-1" caveat is not merely FG-inapplicable — **it is obsolete
> outright**, because SR's default-1 no longer exists. No action; recorded because it retires a
> standing caveat rather than adding one.

**2.3 — FG's motion-vector scale convention, cross-validated from two independent readings.**
`docs/STRAY-RENDERING-FACTS.md` §32.3 reverse-engineered the raw `nvngx_dlssg.dll` snippet's
expectation from the Streamline *programming guide* (§7.0, HARD there): `mvecScale = {1,1}` if
the buffer is already `[-1,1]`, `{1/renderWidth, 1/renderHeight}` if it is pixel-space, and
concluded (since our resolve emits render-resolution pixels) that `DLSSG.MvecScaleX/Y` should be
`1/renderW, 1/renderH` — which is exactly what `src/ngx_fg.cpp:433-434` sends. The real UE5
plugin source computes the *identical* quantity independently:

```cpp
// StreamlineViewExtension.cpp:914-923
const float MotionVectorScale = CVarStreamlineMotionVectorScale.GetValueOnRenderThread(); // default 1.0f
if (bDilateMotionVectors)
    StreamlineArguments.MotionVectorScale = { MotionVectorScale / ViewInfo.GetSecondaryViewRectSize().X, MotionVectorScale / ViewInfo.GetSecondaryViewRectSize().Y };
else
    StreamlineArguments.MotionVectorScale = { MotionVectorScale / ViewInfo.ViewRect.Width(), MotionVectorScale / ViewInfo.ViewRect.Height() };
```

Non-dilated (our case): `1.0 / ViewRect.Width()`, `1.0 / ViewRect.Height()` — the same
`1/renderWidth, 1/renderHeight` §32.3 derived from the guide text alone. **HARD, two
independent sources agree. Do not adopt anything — this confirms our existing value is right**,
and retires the "UNCONFIRMED until an interpolated frame is judged" caveat §32.3 attached to
this specific convention (the *values* are now cross-validated from source; whether the
resulting interpolated frame looks correct on the box is still a separate, unconfirmed question).

**2.4 — The camera-cut/validity test differs by one detail: `.x` vs. `all(.xy)`.**
Both NVIDIA combine shaders (SR's and FG's) test `all(EncodedVelocity.xy > 0)` to decide
"is this pixel's velocity encoded, or do I need to reconstruct it from depth+camera". Our own
resolve shader, and Stray's own DXBC (`CLAUDE.md` §2.5, HARD from the shipped bytecode), test
`.x` alone: *"Validity test is `EncodedVelocity.x > 0.0` — strict, red channel only... reproduce
it exactly; never use a magnitude threshold."* These are two different shaders (the engine's own
TAA vs. NVIDIA's velocity-combine) checking the same encoded texture for different purposes, so
this is not necessarily a contradiction — but it is a real, citable divergence in how
conservative the two conventions are: `all(xy>0)` rejects strictly more pixels back to the
camera-reconstruction branch than `x>0` does. **Investigate, low priority**: Stray's own TAA
shader is the ground truth for what encodes here (HARD, DXBC-derived), so our `.x`-only test is
correct *for matching the engine's own convention* — but NVIDIA's own reference being more
conservative is worth a note in case a future title's velocity encode ever produces `x>0, y<=0`
for a genuinely moving pixel (would decode a wrong `y` under our test, would fall through to
camera reconstruction under NVIDIA's). Not actionable for Stray specifically without evidence
such a pixel exists in practice.

> ### RESOLVED 2026-09-03 — NVIDIA moved to OUR convention. The divergence no longer exists.
>
> The first pass filed this as an open, low-priority divergence: NVIDIA tests
> `all(EncodedVelocity.xy > 0)`, we and Stray's own DXBC test `.x` alone. **First-party source
> closes it, and in our favour.** `VelocityCombine.usf` changed between the two integrations:
>
> | | UE 4.27 (DLSS-SR 3.7.0) | UE5 8.7.2 |
> |---|---|---|
> | dynamic-velocity test | `all(EncodedVelocity.xy > 0)` | **`EncodedVelocity.x > 0.0f`** |
> | pixel centre | `SvPositionToScreenPosition(float4(PixelPos.xy, 0, 1))` | `... (PixelPos.xy + 0.5f, 0.0f, 1.0f)` |
> | viewport scale | `View.ViewSizeAndInvSize.xy` | `CombinedVelocity_ViewportSize` |
> | `PrevClipPos.w <= 0` fallback | `Velocity = EncodedVelocity.xy` (raw encoded — a bug) | leaves `Velocity = 0` |
>
> **HARD**, both files read first-party. **Our `.x > 0` rule matches the current plugin exactly**,
> and it always matched Stray's own shipped bytecode (`CLAUDE.md` §2.5, HARD from DXBC). The
> first pass's caution was reasonable given what it could read, but the concern is now retired:
> there is no convention to reconcile. **No action.**
>
> **Two smaller things fall out, one of which is worth a look.**
>
> * **The half-pixel offset.** UE5 samples the pixel *centre* (`PixelPos.xy + 0.5f`) in the
>   camera-reconstruction branch where 4.27 sampled the corner. Our `shaders/mv_resolve.hlsl`
>   should be checked against this — a uniform half-pixel bias in the reconstructed (static
>   geometry) motion vectors is exactly the "compounds through the accumulation, never looks
>   like a motion-vector bug" failure `CLAUDE.md` warns about after the `ClipToPrevClip`
>   transposition incident. **Investigate, cheap, and this is a genuinely new finding neither
>   pass had.** UNCONFIRMED whether our shader already does this.
> * **`PrevClipPos.w <= 0`.** 4.27 fell back to the *raw encoded* velocity, which is
>   dimensionally wrong; UE5 leaves it zero. Our own guard (`CLAUDE.md` §5, "Guard with
>   `PrevClipPos.w > 0`") matches the newer, correct behaviour. No action.
>
> **The Programming Guide's own reference shader is STALE**, incidentally: §3.6.1.1 still ships
> the 4.27-era `all(Velocity.xy > 0)` form. A vendor doc's code snippet is not the vendor's code
> — the same lesson as the preset table in §4.

---

## 3. Exposure

**Unchanged from the UE4 plugin, at the source level.** `DLSSUpscaler.cpp:1111-1115`:

```cpp
check(PassParameters->EyeAdaptation);
PassParameters->EyeAdaptation->MarkResourceAsUsed();
DLSSArguments.InputExposure = PassParameters->EyeAdaptation->GetRHI();
DLSSArguments.PreExposure = Inputs.PreExposure;
DLSSArguments.bUseAutoExposure = bUseAutoExposure;
```

is byte-for-byte the same call shape the companion doc already quoted from the UE4/5.0-era
mirror, with `r.NGX.DLSS.AutoExposure` still defaulting to **1** (`:97-98`,
"Enable DLSS internal auto-exposure instead of the application provided one (default)"). **HARD,
nothing to report.** This project already implements both the texture path and the auto-exposure
path with the identical flag pairing (`src/ngx_backend.cpp:867-877`, cited against
`NGXRHI.cpp:537-546` in our own comments). Full audit of exposure is the `dlss-exposure` branch's
job — this section exists only to confirm there is nothing UE5/DLSS4-specific to add to it.

> ### CONFIRMED AND EXTENDED 2026-09-03 — plus the first-party text behind the preset gate
>
> **The first pass's conclusion is right and now rests on first-party source.**
> `r.NGX.DLSS.AutoExposure` defaults to **1** on *both* branches — `dlss3/sl2-4.27-dlss-plugin`
> and `nvrtx-5.8_prerelease` — with identical help text: *"1: Enable DLSS internal auto-exposure
> instead of the application provided one (default)"*. **HARD.**
>
> Three details the first pass did not have, all first-party, all relevant to the
> `dlss-exposure` audit rather than to this document:
>
> 1. **`InPreExposure` is sent unconditionally**, auto-exposure on or off. Only the *texture* is
>    gated: `EvalParams.pInExposureTexture = InArguments.bUseAutoExposure ? nullptr : ...`
>    (`NGXD3D12RHI.cpp`, both branches). And `nvsdk_ngx_helpers.h:538` substitutes 1.0 for a zero
>    pre-exposure. **HARD.**
> 2. **The preset gate the `dlss-exposure` branch found is confirmed in NVIDIA's own shipped
>    Guide, verbatim.** `DLSS_Programming_Guide_Release.pdf` §3.9 (dated 31 March 2026, shipped
>    inside the plugin folder on `nvrtx-5.8_prerelease`):
>
>    > **3.9 Exposure Parameter** — Only supported by Presets J and K. Preset L always uses
>    > AutoExposure.
>
>    and the Guide's own changelog carries the line *"Updated Section 3.9 Exposure input is only
>    supported by Presets J"*. **HARD.** Cross-referenced, not duplicated — the `dlss-exposure`
>    branch owns this.
>
>    **The finding that belongs to THIS document is the inconsistency**: the plugin's code says
>    nothing about the gate. No comment, no branch, no preset-conditional anywhere in either
>    tree. So **NVIDIA's own plugin will hand an exposure texture to Preset L or M, where the
>    runtime ignores it** — an inconsistency by silence, not a contradiction, and a reminder
>    that the plugin is not a specification. Note the interaction with §4: at Stray's 50% the
>    runtime's default preset is **M**, which is *not* one of the two presets that accept an
>    exposure input.
> 3. **UE5 adds `r.NGX.DLSS.ExposureScale`** (default `1.0f`, clamped `[0,1]`) →
>    `InExposureScale`, on the **SR path only**. 4.27 has no such cvar. We do not set it;
>    **UNCONFIRMED** whether it matters at the default. Low priority.
>
> **Doc drift worth knowing**: the plugin's own `README.md` still documents the cvar as
> `r.NGX.DLSS.EnableAutoExposure`, which does not exist. The code name is
> `r.NGX.DLSS.AutoExposure`.

---

## 4. Feature flags and creation

**`DoSharpening` is gone, not merely deprecated.** A search of `DLSSUpscaler.cpp` for
`Sharpening` returns nothing — no flag, no cvar, no reference at all. **HARD** (absence). This
retires any lingering doubt about `CLAUDE.md`'s "Never set `DoSharpening` (deprecated, does
nothing)" — it is not even wired up anymore on NVIDIA's own side. **Do not adopt anything; we
already never set it.**

**Presets: the set has grown, and the deprecation boundary moved.** `DLSSUpscaler.cpp:44-86`:

```cpp
// r.NGX.DLSS.Preset (SR/DLAA)
//  0: Use default preset or ini value
//  1-6: Force preset A..F      (A-E deprecated per ToNGXDLSSPreset's ensureMsgf, F is NOT)
//  7: Force preset G
// 10-15: Force preset J..O     (10-13 = J,K,L,M — what CLAUDE.md documents — plus 14=N, 15=O)

// r.NGX.DLSSRR.Preset (Ray Reconstruction)
//  1-3: Force preset A..C (deprecated)
//  4-15: Force preset D..O (not deprecated)
```

```cpp
// DLSSUpscaler.cpp:267-278, ToNGXDLSSPreset
case EDLSSPreset::A: case EDLSSPreset::B: case EDLSSPreset::C: case EDLSSPreset::D: case EDLSSPreset::E:
    ensureMsgf(false, TEXT("ToNGXDLSSPreset should not be called with a deprecated value"));
case EDLSSPreset::Default: return NVSDK_NGX_DLSS_Hint_Render_Preset_Default;
case EDLSSPreset::F: return NVSDK_NGX_DLSS_Hint_Render_Preset_F;   // <-- F is a live, non-deprecated preset now
```

**HARD.** `CLAUDE.md` §5 currently reads *"Presets: only `0, J=10, K=11, L=12, M=13` are valid
(A–D removed, E/F deprecated)."* Against this source: **A–E are deprecated for SR (not A–D), F is
a live preset, and N (14) and O (15) exist and are not deprecated.** For Ray Reconstruction the
deprecation boundary is A–C, not A–D. This is worth a `CLAUDE.md` correction independent of
anything else in this report — the exact wording is now stale, not merely incomplete. **Adopt the
correction**; whether to also change our pinned preset from K to something in the newer range is
a separate, lower-confidence question below.

> ### RETRACTED 2026-09-03 — the paragraph above is WRONG, and `CLAUDE.md` was RIGHT
>
> **This is the most important correction in the re-review, because the first pass did not merely
> miss something — it talked a correct statement out of the manual.** The recommendation
> "**Adopt** the correction" above would have introduced two errors into `CLAUDE.md` §5.
>
> **Ground truth, read first-party from the NGX 310.6.0 header shipped inside plugin 8.7.2**
> (`nvrtx-5.8_prerelease`, `DLSS/Source/ThirdParty/NGX/Include/nvsdk_ngx_defs.h`):
>
> ```c
> NVSDK_NGX_DLSS_Hint_Render_Preset_Default = 0,   // A/B/C/D removed, use preset J or K
> NVSDK_NGX_DLSS_Hint_Render_Preset_E = 5,         // Deprecated
> NVSDK_NGX_DLSS_Hint_Render_Preset_F = 6,         // Deprecated
> NVSDK_NGX_DLSS_Hint_Render_Preset_G = 7,         // Do not use, reverts to default
> NVSDK_NGX_DLSS_Hint_Render_Preset_H_Reserved = 8, I_Reserved = 9,
> J = 10, K = 11, L = 12, M = 13,
> N = 14, O = 15,                                  // Do not use, reverts to default
> ```
>
> | First-pass claim | Truth (HARD, 310.6.0 header) |
> |---|---|
> | "F is a live, non-deprecated preset now" | **False.** F is marked `Deprecated`. |
> | "N (14) and O (15) exist and are not deprecated" | **False.** Both are `Do not use, reverts to default` — they are *reserved*, not new models. |
> | "A–E are deprecated for SR (not A–D)" | Cosmetic at best: A–D are **removed**, E is **deprecated**. `CLAUDE.md`'s "A–D removed, E/F deprecated" is the more accurate of the two. |
>
> **`CLAUDE.md` §5's "only `0, J=10, K=11, L=12, M=13` are valid" is CORRECT and should be left
> exactly as it is.** Recommendation #2 of the first pass is **withdrawn**, and its
> recommendation #3 — "re-run the J/K/L/M A/B with N and O included" — is **void**: N and O
> revert to default, so the experiment would have compared K against K and produced a confident
> null result from a test that could not have shown anything.
>
> **How the first pass got here, and it is the lesson.** It read plugin **8.3.0** through a
> mirror and quoted the plugin's own `EDLSSPreset` *UENUM* — which enumerates every value the
> editor dropdown can offer, including reserved ones — rather than the **NGX header**, which is
> the actual contract. A UENUM entry existing is not a preset existing. **Read the SDK header,
> never the wrapper's enum.**
>
> **On preset E specifically, both are half right.** NVIDIA's release notes
> (`PublicReleaseNotes.md`, 8.7.0, July 2026) do say *"Restored DLSS-SR legacy preset E"* — but
> the 310.6.0 header still marks E `Deprecated` and the plugin hides it
> (`E=5 UMETA(Hidden, ToolTip = "Deprecated, use preset J or K")`). So E is *forceable* again via
> `r.NGX.DLSS.Preset=5`, not *recommended*. **SOFT→HARD split**: the restoration is release-note
> prose; the deprecation is header text. Do not use E.
>
> ### The genuinely actionable preset finding the first pass missed
>
> **The plugin sets no per-quality-mode preset at all** — every field of `UDLSSSettings`
> (`DLAAPreset`, `DLSSQualityPreset`, `DLSSBalancedPreset`, `DLSSPerformancePreset`,
> `DLSSUltraPerformancePreset`, `DLSSUltraQualityPreset`) defaults to `EDLSSPreset::Default`,
> and `ApplyCommonNGXParameterSettings` writes that one value into **all six** hint keys. NVIDIA
> defers entirely to the runtime. **HARD.**
>
> The runtime's own per-mode defaults are documented first-party in
> `DLSS_Programming_Guide_Release.pdf` §3.12 (dated **31 March 2026**, shipped inside the plugin
> folder on `nvrtx-5.8_prerelease`):
>
> | Quality mode | Runtime default preset |
> |---|---|
> | DLAA, Quality, Balanced | **K** |
> | Performance | **M** |
> | UltraPerformance | **L** |
>
> and the Guide adds that J is *"similar to K, slightly less ghosting at the cost of extra
> flickering; K is generally recommended over J."*
>
> **This matters for us concretely.** `src/ngx_backend.cpp` pins **K** on all five hint keys
> unconditionally. Stray runs 50% screen percentage — a **2.0x ratio, i.e. the Performance
> band** — where NVIDIA's runtime would choose **M**, not K. At 70% (1.43x, Quality/Balanced
> band) K is what the runtime would pick anyway.
>
> **Investigate, cheap, and now with a specific hypothesis**: A/B `NgxPreset=13` (M) against the
> current K **at 50% only**. The project's existing user-judged A/B found J/K/M
> indistinguishable (`CLAUDE.md` §5) — so the prior is that this changes nothing, and that prior
> is now better founded than before, since we know M is merely the vendor's default for this
> ratio rather than a different model generation. **Low urgency, genuinely low expected value,
> but it is a config flip and the reasoning is no longer guesswork.** Alternatively, and closer
> to NVIDIA's own posture: pass `Default` (0) and let the runtime pick per mode.

**Default behaviour differs: NVIDIA lets the driver choose per quality mode unless the project
overrides it; we hardcode K everywhere.** `GetNGXDLSSPresetFromQualityMode`
(`DLSSUpscaler.cpp:369-406`) starts from `EDLSSPreset::Default` and only overrides it from a
per-quality-mode project setting (`UDLSSSettings::DLSSPerformancePreset`,
`DLSSBalancedPreset`, ... one field per `EDLSSQualityMode`) or from the global
`r.NGX.DLSS.Preset` cvar if it is non-zero. **HARD.** `src/ngx_backend.cpp:71-81` instead pins a
single preset (K, `g_preset`) unconditionally for every quality mode via
`NgxPreset`/`NVSDK_NGX_DLSS_Hint_Render_Preset_K` on all five hint keys at once
(`ngx_backend.cpp:855-858`). **Investigate, not adopt outright**: our own user-judged A/B already
found J/K/M visually indistinguishable on this content (`CLAUDE.md` §5, "Gotchas ledger"), so
there is no measured reason to prefer the driver default over K specifically — but that
comparison predates presets N and O existing at all. Re-running that same A/B with N and O
added is cheap (a config flip, no code change) and would either confirm K still holds or find a
newer model that is visibly better; low urgency, non-zero expected value.

**Dynamic-resolution bookkeeping: not applicable.** `FDLSSUpscaler`'s constructor
(`DLSSUpscaler.cpp:629-671`) tracks `MinDynamicResolutionFraction`/`MaxDynamicResolutionFraction`
across all six quality modes' `FDLSSOptimalSettings`, feeding UE5's dynamic-resolution system.
Stray runs a fixed screen percentage (`CLAUDE.md` §2.3.1: 50% or 70%, chosen by the shipped
config, never adjusted at runtime) and this project does not implement dynamic resolution at
all — **not applicable**, nothing to adopt.

---

## 5. Frame generation

This is the section where the real UE5 source adds the most over the companion doc's generic-SDK
reading, because `docs/STRAY-RENDERING-FACTS.md` §32 already reverse-engineered the raw
`nvngx_dlssg.dll` parameter *names* from a 2.12.0 binary, and what follows either confirms those
names' *values and conventions* from real engine source, or finds a genuine value mismatch.

**5.1 — Guide capture is ONE engine hook, not several, and it sits after the full post-process
chain.** `FStreamlineViewExtension::SubscribeToPostProcessingPass` registers a single callback,
at `EPostProcessingPass::VisualizeDepthOfField` (`StreamlineViewExtension.cpp:639-649`) — despite
the name, this is Epic's designated "last callback in the post-process chain" slot, confirmed by
what the callback actually reads: `PostProcessPassAtEnd_RenderThread`
(`StreamlineViewExtension.cpp:702` onward) names its own local `SceneColorAfterTonemap =
SceneColor.Texture` (`:777-778`) and pulls Depth and Velocity straight from `FSceneTextures`
(`:794-799`), not from any DOF-specific resource. **HARD.** From this single hook, in one RDG
pass, NVIDIA builds and tags: `Depth` (raw `SceneTextures.Depth.Resolve`), `Velocity` (through
its own combine pass, §2 above), `SceneColorWithoutHUD` (a plain copy of the post-tonemap scene
colour, `AddDrawTexturePass(..., SceneColor.Texture, SLSceneColorWithoutHUD, ...)`,
`:836-841`), and `NoWarpMask` (custom depth, for the Latewarp feature, not applicable to us).
**This is materially simpler than what we built**: one engine callback with `FRDGBuilder` and
`FSceneTextures` access replaces our TAA-dispatch interception, per-register descriptor capture,
and separate `mv_resolve` compute dispatch — because NVIDIA is running *inside* the engine and we
are not. Nothing here is adoptable directly (same wall as §1's `ITemporalUpscaler` finding), but
it is worth stating plainly: **our architecture's extra complexity is a direct, measured cost of
staying outside the engine, not an implementation gap** — useful context for any future decision
about the UE4SS migration's expected value.

**5.2 — UI/HUD-less capture is a SECOND, separate hook, at actual present time, on the game
thread.** Not part of the RDG view extension at all: `RegisterStreamlineDLSSGHooks`
(`StreamlineDLSSG.cpp:366-395`) binds `DLSSGOnBackBufferReadyToPresent`
(`OnBackBufferReadyToPresentHandle`) to Slate's `SlateRenderer::OnBackBufferReadyToPresent`
delegate — this fires once per window, holding the actual composited backbuffer (UI included),
right before it presents. From there, when the engine has no dedicated UI-alpha buffer, NVIDIA
runs a compute pass (`AddStreamlineUIHintExtractionPass`,
`StreamlineShaders/Private/UIHintExtractionPass.cpp`) that thresholds the **backbuffer's own
alpha channel** against `r.Streamline.TagUIColorAlphaThreshold` to synthesize a UI mask —
i.e. even NVIDIA's reference falls back to an alpha heuristic rather than true UI/scene
separation when a title's UI doesn't carry a clean alpha. **HARD**, and genuinely useful to
record: **this exact two-hook shape (a pre-UI colour snapshot from inside the engine's
post-process chain, plus a present-time alpha threshold on the final composited image) is
precisely the design our own `NgxNRHook=preui` work already reasons toward for the NR
feedback-loop problem** (`CLAUDE.md`, "The hook site is now a choice"). We still cannot reach the
first half (`SceneColorWithoutHUD`) today — our present hook is at the swapchain/D3D12 level (now
UE4SS-loaded native code, `src/backend_native/present_owner.*`, not a ReShade proxy, but still
operating at the same point in the pipeline) and only ever sees the *final* composited backbuffer,
the same thing Slate's `OnBackBufferReadyToPresent` sees, never the pre-UI intermediate. Reaching
that intermediate needs exactly the same technique §1 already reclassifies for `ITemporalUpscaler`
— pattern-scan and hook the specific renderer-thread post-process callback point (Epic's
`SubscribeToPostProcessingPass`/`EPostProcessingPass::VisualizeDepthOfField` slot, per §5.1 above)
— which UE4SS's own reflection does not provide automatically, but which our now-native,
in-process, already-vtable-patching codebase is at least the right *kind* of program to attempt.
**Investigate, long-horizon, tied to §1's reclassification, not separately actionable today**: our
own `preui` render-target-identity heuristic (`NgxNRPreUiBind`) remains the closest available
substitute in the meantime, and there is no cheaper alternative to adopt right now.
`DLSSG.HUDLess`/`DLSSG.UI`/`DLSSG.UIAlpha` (confirmed present, optional, in
`docs/STRAY-RENDERING-FACTS.md` §32.2) remain unset by us — correctly, since we have no source
for them.

> ### CORRECTED 2026-09-03: the alpha threshold is NOT a heuristic we could copy, and HDR kills it anyway
>
> The paragraph above says NVIDIA *"falls back to an alpha heuristic rather than true UI/scene
> separation when a title's UI doesn't carry a clean alpha."* **That reading is wrong in the way
> that matters, and it made the capability look more reachable than it is.**
>
> **The alpha channel is not found meaningful — it is MADE meaningful, by an engine patch.**
> The same view extension that captures hudless also *clears scene-colour alpha to zero* at the
> end of the post-process chain, so that whatever Slate subsequently draws is the only thing
> with non-zero alpha. `r.Streamline.ClearSceneColorAlpha`, default **true**
> (`nvrtx-5.8_prerelease`, `StreamlineViewExtension.cpp:862-880`; same mechanism on
> `dlss3/sl2-4.27-dlss-plugin` at `:716`). It calls `DrawClearQuadAlpha(RHICmdList, 0.0f)` — and
> the file **hard-`#error`s** without it:
>
> ```cpp
> #error "Engine missing DrawClearQuadAlpha support. Apply latest custom engine patch using
>          instructions from DLSS-FG plugin quick start guide or README.md"
> ```
>
> **`DrawClearQuadAlpha` does not exist in stock UE 4.27.** Verified against
> `EpicGames/UnrealEngine` @ tag `4.27`,
> `Engine/Source/Runtime/RenderCore/Public/ClearQuad.h` — zero occurrences. It is added by
> NVIDIA's own engine patch (`NvRTX/UnrealEngine` commit `8b8facd1a8`, "Implement
> DrawClearQuadAlpha", which also templates `ClearQuadSetup` on `EColorWriteMask` to make a
> `CW_ALPHA`-only clear possible). **HARD.**
>
> So the pipeline is: *engine patch zeroes scene-colour alpha → Slate draws UI into alpha →
> present-time compute pass thresholds alpha*. The threshold is the last and least interesting
> step. Without the first step the alpha channel of Stray's back buffer is whatever the game
> happened to leave there, and thresholding it is not an approximation of NVIDIA's approach —
> it is unrelated to it.
>
> **And NVIDIA says not to use it in HDR at all, which is our configuration.** The 8.7.2
> plugin's own `StreamlineCore/README.md:433-447` records that in HDR the back-buffer alpha is
> not meaningful and recommends setting `r.Streamline.TagUIColorAlpha 0`. Stray presents
> `R10G10B10A2_UNORM` under gamescope's HDR path (`CLAUDE.md` §1, §2.1). **So even a perfect
> reproduction of the alpha trick would be the configuration NVIDIA tells titles to switch
> off.** UI tagging via alpha is a dead end for this target specifically, independent of
> reachability. **HARD** (doc text, first-party README).
>
> **What this does NOT retract, and what §1.1 adds.** The *hudless colour* half is a different
> story and is now more reachable than the first pass concluded, not less: it comes from
> `SubscribeToPostProcessingPass(EPostProcessingPass::VisualizeDepthOfField, ...)`, which is on
> stock 4.27's `ISceneViewExtension` (§1.1). The first pass was right that our present-time hook
> only ever sees the final composited back buffer and can never produce hudless; it was wrong to
> file the fix as a separate, harder problem from §1's. **They are the same problem: register a
> view extension.** Hudless is the valuable half in any case — the UI half is what HDR rules out.
>
> **Also corrected: `OnBackBufferReadyToPresent` is not an NVIDIA addition.** It is stock UE
> 4.27 (`EpicGames/UnrealEngine` @ `4.27`, `SlateCore/Public/Rendering/SlateRenderer.h:262-263`).
> **HARD.** But reaching it buys us nothing: it delivers the *final composited back buffer*,
> which is precisely what our own present hook already has. Noted so nobody spends effort
> reaching a delegate that hands over an image we already hold.

**5.3 — `DLSSG.CameraFar`: a concrete, actionable value mismatch.** `docs/STRAY-RENDERING-FACTS.md`
§32.3 already flagged this as open: *"`sl_consts.h:248-249`... `cameraNear/cameraFar` default
`INVALID_FLOAT` (must be set)."* `src/ngx_fg.hpp:36` currently defaults our own
`camera_far` to **0.0f**, documented as *"0 = pass 0 (infinite, what a reversed-Z infinite
projection means)"* — i.e. we treat 0 as a stand-in for "no far plane." The real UE5 plugin does
the opposite: it never tries to express "infinite" at all. `StreamlineViewExtension.cpp:90-95`:

```cpp
static TAutoConsoleVariable<float> CVarStreamlineCustomCameraFarPlane(
    TEXT("r.Streamline.CustomCameraFarPlane"),
    75000.0f,
    TEXT("Custom distance to camera far plane. Used for internal DLSS Frame Generation purposes, ")
    TEXT("does not need to match corresponding value used by engine. (default = 75000.0f)"),
    ECVF_RenderThreadSafe);
```

and this literal cvar value is what feeds `StreamlineArguments.CameraFar` (`:833`), unconditionally,
for every title the plugin ships in — including UE5 titles that, like Stray, may use an infinite
far plane in their actual projection matrix. **HARD.** So NVIDIA's own reference, when faced with
"the engine's real far plane may be infinite," does not pass 0 or attempt to signal infinity — it
substitutes an arbitrary large finite constant and says explicitly this need not match the
engine. Passing **0.0** is a materially different choice: `sl_consts.h`'s own default is a
sentinel (`INVALID_FLOAT`, i.e. "not set," not "infinite"), and there is no source read in this
project confirming that the closed `nvngx_dlssg.dll` snippet's internal camera-motion
reconstruction treats a literal `0.0` far plane as "infinite" rather than as a degenerate
near-zero-depth-range input. **Adopt/investigate — the highest-confidence, most concrete
finding in this report**: change `NgxFGCameraFar`'s default from `0.0f` to a large finite
constant (NVIDIA's own `75000.0f` is a reasonable starting point, or derive one from Stray's own
scene scale) and re-test. This is a one-line config change with a plausible, specific failure
mode behind it (the snippet's `1/depth`-style linearization, `docs/STRAY-RENDERING-FACTS.md`
§32.3, dividing by a degenerate range) and costs nothing to try.

**Our `CameraNear` choice is, if anything, more correct than NVIDIA's own default — no
change.** `CVarStreamlineCustomCameraNearPlane` defaults to a hardcoded `0.01f`
(`StreamlineViewExtension.cpp:89`, same "does not need to match" caveat). We instead pass
`c.near_plane` — the real, measured `View.NearPlane` read from row 142.x of the View constant
buffer (`ngx_fg.cpp:448`, `CLAUDE.md` §2.6). **Do not adopt** NVIDIA's hardcoded default; ours is
already the more faithful value and there is no evidence it needs to change.

> ### CONFIRMED AND SHARPENED 2026-09-03, first-party, on BOTH engine versions
>
> **The first pass's headline recommendation is right, and now rests on first-party source
> rather than a mirror.** The `75000.0f` constant is byte-identical in both NVIDIA integrations:
>
> * **UE 4.27 — our exact engine version.** `NvRTX/UnrealEngine` @ `dlss3/sl2-4.27-dlss-plugin`
>   (`32c3e4d5`), `Engine/Plugins/Runtime/Nvidia/Streamline/Source/StreamlineCore/Private/
>   StreamlineViewExtension.cpp:66-76`, consumed unconditionally at `:561-562`.
> * **UE 5.8 — current, plugin 8.7.2.** `nvrtx-5.8_prerelease` (`5b89940f`), same file
>   `:84-94`, consumed at `:693-696`.
>
> Both carry the identical help text — *"Used for internal DLSS Frame Generation purposes, does
> not need to match corresponding value used by engine"* — and both pass `bIsDepthInverted =
> true` unconditionally alongside. **HARD.**
>
> **New, and it is the part that actually settles the argument.** The first pass could only say
> "there is no source confirming the snippet treats `0.0` as infinite". The vendored SDK header
> now answers it directly. `StreamlineCore/Source/ThirdParty/Streamline/include/sl_consts.h`
> (SL **2.11.1**) states at `:174` that *"Common constants, all parameters must be provided
> unless they are marked as optional"*; `cameraNear`/`cameraFar` (`:210-213`) **carry no
> Optional annotation**, and the struct's own unset value is the sentinel `INVALID_FLOAT =
> 3.402823466e38f` (`:90`). Only `clipToLensClip`, `cameraPinholeOffset` and
> `motionVectorsInvalidValue` are marked Optional. **So an unset/degenerate far plane is a
> detectable error condition in Streamline's own model, not a way of signalling infinity** —
> and `sl_dlss_g.h` has a status bit for exactly this class of fault,
> `DLSSGStatus::eFailCommonConstantsInvalid`. Our `0.0f` is not "infinite"; it is the
> degenerate case.
>
> **Adopt. This is now the highest-confidence recommendation in the document.** Change
> `NgxFGCameraFar`'s default from `0.0f`.
>
> **REVISED — take the PAIR, not just the far plane.** The first pass recommended keeping our
> real `View.NearPlane` while changing only the far plane, on the reasoning that ours is "more
> faithful". **That reasoning does not survive first-party reading and is withdrawn.** NVIDIA
> passes near and far as a *matched, deliberately synthetic pair* (`0.01`, `75000`) whose whole
> point is that it is decoupled from the engine's projection — the header says so twice. If the
> snippet uses them together to define an internal depth range (a `near*far/(far - z*(far-near))`
> style linearisation is the obvious candidate, though the snippet is closed and this is
> **UNCONFIRMED**), then mixing our measured near with NVIDIA's synthetic far produces a pair
> NVIDIA never tests. **Recommended: pass NVIDIA's pair — near `0.01`, far `75000.0` — as the
> new defaults, keeping both as knobs.** "More faithful to the engine" is not a virtue for a
> value the vendor explicitly documents as not needing to match the engine.
>
> **Still UNCONFIRMED, and it is the honest limit**: whether either value visibly changes an
> interpolated frame on the box. `nvngx_dlssg.dll` is closed; nothing here is a measurement of
> its behaviour, only of what NVIDIA's own callers send it.

**5.4 — Reset detection: NVIDIA's own FG-side code is simpler than SR's, and admits it. Ours is
already the more complete OR.** `StreamlineViewExtension.cpp:915`:

```cpp
// TODO STREAMLINE check for other conditions, similar to DLSS
StreamlineArguments.bReset = View.bCameraCut;
```

**HARD**, including the comment — a single-signal reset test with an open TODO acknowledging
DLSS SR's own reset logic (the multi-signal check the companion doc's §A quotes, and
`CLAUDE.md` §2.8 documents independently: `CameraCut` flag OR jitter `zw==xy` OR a 1×1
history/velocity texture) is more complete. Our project already propagates the same
multi-signal OR to both SR and FG (`CLAUDE.md` §2.8's "OR three signals for `InReset`", and
`ngx_fg.cpp:158`'s `g_pending_reset` comment citing exactly this). **Do not adopt anything —
this is a case where our implementation is already ahead of NVIDIA's own shipped FG code**,
worth recording for confidence rather than action.

**5.5 — Multi Frame Generation: real, but not applicable to our target hardware.**
`sl_dlss_g.h` (this build, SL 2.9.0) defines `DLSSGOptions::numFramesToGenerate` (default `1`,
i.e. classic 2x) with the comment *"For 2x only supporting devices, numFramesToGenerateMax is 1.
For 3x and 4x supporting devices, numFramesToGenerateMax is 3."* **HARD** that this ceiling is
read from the SDK itself, not inferred. `docs/STRAY-RENDERING-FACTS.md` §32.2 already confirms
the underlying raw NGX parameter names exist in the snippet (`DLSSG.MultiFrameCount`,
`DLSSG.MultiFrameIndex`, `DLSSG.MultiFrameCountMax`) and that **we already set them**
(`src/ngx_fg.cpp:245-246`: `MultiFrameCount=1, MultiFrameIndex=0`), reading `MultiFrameCountMax`
back after create and logging it (`:275-296`). Multi-frame generation beyond 2x is an Ada
(RTX 40-series, our target — `CLAUDE.md` §1: RTX 4090) hardware ceiling, not a software gap on
either NVIDIA's side or ours: **not applicable, no action** — this was already the right call,
made independently, before this report existed. `sl::DLSSGMode` in this exact SDK snapshot
(2.9.0) has only `eOff/eOn/eAuto`, no `eDynamic` — Dynamic Multi Frame Generation (`SOFT`, web
search: introduced in the Streamline 2.11.0 changelog with `dynamicTargetFrameRate`) postdates
this source and was not read; moot regardless, since it only selects *how many* extra frames to
generate on hardware that can generate more than one, which Ada cannot.

**5.6 — Fullscreen-menu detection: a different mechanism, same intent, no action needed.**
NVIDIA gates FG per-frame on `DLSSGFlags::eEnableFullscreenMenuDetection`
(`CVarStreamlineFullScreenMenuDetection`, `StreamlineDLSSG.cpp:94-98`) — the snippet itself
inspects the tagged buffers to guess "is this a fullscreen menu" and suppresses generation. We
instead set `DLSSG.MenuDetectionEnabled=0` and rely on our own guide-publish gate: no TAA-hook
dispatch (a loading screen, most menu states) means no published guides, means no generated
frame that presentation cycle (`ngx_fg.cpp:332`'s comment). **Do not adopt** — different
mechanism, same effect, and ours is grounded in something we can observe directly (whether our
own hook fired) rather than a heuristic over tagged content.

**5.7 — Reflex, present pacing, and swapchain ownership: intentionally out of scope here.**
`StreamlineReflex.cpp` confirms DLSS-G forces Reflex on automatically when active
(`DoActiveStreamlineFeaturesRequireReflex()`, `:195` and its use at `:269`) — consistent with,
and no update to, what `CLAUDE.md` already establishes about needing `fakenvapi`
`force_reflex=2`. A full comparison of Streamline's present-pacing algorithm, its resource-tag
lifetime rules, and its swapchain-proxy mechanics (the thing that was measured to break frame
generation for us under vkd3d-proton, `docs/STRAY-RENDERING-FACTS.md` §32) against our own
present-twice path and phase-locked scheduler is the subject of a **separate** document,
`docs/RESEARCH-STREAMLINE-INTERNALS.md`, read directly from NVIDIA's open-source Streamline SDK
rather than from this UE plugin wrapper. Not duplicated here.

---

## 6. Anything NVIDIA now does that we do not do at all

* **`DLSSGFlags::eRetainResourcesWhenOff`** (`sl_dlss_g.h:44`) — keep FG's internal resources
  allocated across an off→on toggle instead of tearing them down. **UNCONFIRMED** whether this
  would meaningfully change our own create/release cadence (`src/ngx_fg.cpp`'s
  `g_create_latched` teardown-on-resize path) — plausible minor win (avoids a
  `ReleaseFeature`+`CreateFeature` round trip if FG is toggled at runtime), not evaluated in
  this pass. **Investigate, low priority**: only matters if FG is toggled frequently at runtime,
  which is not this project's current usage pattern (set once via `Engine.ini`/launch config).
* **`DLSSGOptions::numBackBuffers`, `colorBufferFormat`, `mvecBufferFormat`, `depthBufferFormat`,
  `hudLessBufferFormat`** — creation-time format/count hints so the snippet can pre-validate or
  pre-allocate. We already pass the equivalent information at create time
  (`DLSSG.Width/Height/BackbufferFormat/ColorBuffersHDR`, `ngx_fg.cpp:238-244`,
  `docs/STRAY-RENDERING-FACTS.md` §32.2 confirms these names are read by the snippet) —
  **no gap**, already covered, different name surface (Streamline's `DLSSGOptions` vs. the raw
  `DLSSG.*` parameter names we write directly).
* **`DLSSGFlags::eShowOnlyInterpolatedFrame`** — a debug/visualization mode. Not applicable to a
  shipping configuration; no action.
* **`DLSSGQueueParallelismMode::eBlockNoClientQueues`** (`sl_dlss_g.h:47-56`) — Vulkan-only queue
  parallelism mode. Not applicable — this project is D3D12-only by the vkd3d-proton constraint
  (`CLAUDE.md` §0).
* **Dynamic-resolution-aware FG** (`DLSSGOptions::dynamicResWidth/Height`,
  `eDynamicResolutionEnabled`) — not applicable; Stray does not use engine dynamic resolution
  (`CLAUDE.md` §2.3.1, fixed screen percentage).

---

## 7. NEW 2026-09-03 — what NVIDIA had to change in the UE 4.27 ENGINE, and what we need instead

This section did not exist in the first pass, which could not reach the source. It is the most
useful artifact obtained, because **a change NVIDIA made to the engine is a concrete, itemised
statement of a requirement**: everything in the diff is something an engine must provide to host
DLSS/Streamline and that a stock 4.27 game does not have. For each item the question is the one
the brief asks — do we need the equivalent, do we already achieve it another way, or is it not
needed for our narrower scope?

**Sources**, both first-party, both diffed against the same `4.27.2 release` base commit
`d94b38ae3446`:

* `dlss-streamline-4.27-engine-changes` @ `e1ee6efc0d4e` — 5 commits, **42 files** (2022).
* `dlss3/sl2-4.27-dlss-plugin` @ `32c3e4d5` — the engine patches carried alongside the shipping
  plugin (2023-2024), which **supersede** the 2022 approach for the same problem.

### 7.0 The headline, in NVIDIA's own words

`Streamline/README.md` on the plugin branch:

> "The DLSS Frame Generation plugin is supported **"out of the box" by Unreal Engine 5.2 and
> later**, including packaged engine releases from Epic. For **5.1 and earlier** engine releases,
> **additional source changes to the engine itself must be made**."

**HARD.** Two consequences, and the second is the one worth internalising:

* **NVIDIA's own frame-generation plugin cannot run on stock UE 4.27.2.** Stray is stock UE
  4.27.2. So on this title our external-hook architecture is not a *compromise* relative to
  NVIDIA's approach — **NVIDIA's approach is simply unavailable without rebuilding the game.**
* **DLSS-SR is different and needs no engine change on 4.27.** Its quickstart has no patch step,
  `ITemporalUpscaler` is stock 4.27 (§1.1), and a grep of the whole 4.27 DLSS-SR plugin finds
  **zero** references to the patched `D3D12RHI_API` symbols (7.4 below). **HARD.** So the
  engine-patch requirement is specific to **frame generation**, which is exactly the feature we
  hand-built.

### 7.1 The itemised requirement list

| # | What NVIDIA changed in the 4.27 engine | Why | Do we need it? |
|---|---|---|---|
| 1 | **`DXBridge`** — a whole new runtime module (10 new files) routing every `CreateDXGIFactory*`, `D3D12CreateDevice`, `D3D11CreateDevice` and `DXGIGetDebugInterface1` through an overridable indirection (`IDXGIFunctions::SetCustomDXGIFunctions`) | UE 4.27 loads these by `LoadLibrary`+`GetProcAddress`, which **bypasses an import-table interposer** | **No — we are immune by construction.** 7.2 |
| 2 | **Back-buffer index from the swapchain** rather than a manual `++ % NumBackBuffers` | An interposer that presents extra frames desynchronises the engine's own counter | **No — our rings are decoupled.** 7.3 |
| 3 | **`IDXGISwapchainProvider`** modular feature (the 2023 replacement for #1) | A sanctioned seam for substituting the swapchain | **No, and it does not exist on 4.27 anyway.** 7.5 |
| 4 | **`D3D12RHI_API` exports** on `CResourceState`, `AddTransitionBarrier`, `GetResourceState`, `FlushResourceBarriers`, `UpdateResidency` | The plugin needs the engine's **authoritative resource-state tracker** | **We have no equivalent, and it is a real gap.** 7.4 |
| 5 | **`DrawClearQuadAlpha`** + templating `ClearQuadSetup` on `EColorWriteMask` | Zero scene-colour alpha so the UI mask is meaningful | **Cannot have it; and HDR rules the technique out anyway.** §5.2 |
| 6 | **Slate `OnPreRHIViewportCreate` / `OnPostRHIViewportCreate`**, and `SWindow*` added to `OnSlateWindowDestroyed` | Associate a swapchain with the `SWindow` that owns it | **No — single window, and we hook swapchain creation directly.** |
| 7 | **`ENGINE_STREAMLINE_VERSION` in `Version.h`** | Let the plugin detect whether the engine is patched | Not applicable. |

### 7.2 Requirement #1: we are immune to the problem that cost NVIDIA 42 files

The bulk of the 2022 diff is one mechanical change. UE 4.27's `FD3D12Adapter::CreateDXGIFactory`
originally did this (removed lines in the diff):

```cpp
HMODULE DxgiDLL = (HMODULE)FPlatformProcess::GetDllHandle(TEXT("dxgi.dll"));
CreateDXGIFactory2FnPtr = (FCreateDXGIFactory2)(GetProcAddress(DxgiDLL, "CreateDXGIFactory2"));
VERIFYD3D12RESULT(CreateDXGIFactory2FnPtr(Flags, IID_PPV_ARGS(DxgiFactory.GetInitReference())));
```

**An interposer that works by DLL substitution or import-table patching never sees this call** —
the engine explicitly opens the real system `dxgi.dll` by name and takes its address directly.
Hence `DXBridge`.

**We hook the export's own code, so the `GetProcAddress` path lands on us regardless.**
`mods/StrayDLSS/src/Host.cpp:353` takes `GetProcAddress(d3d12, "D3D12CreateDevice")` and installs
a **MinHook trampoline over the function's prologue**; `:208-234` does the same for all three
`CreateDXGIFactory*` entry points. Anyone who later resolves the same export — by IAT, by
`GetProcAddress`, from any module — executes our patched bytes. **HARD**, read from our own
source. The host even logs whether the prologue was already detoured (`:361-366`).

**This is the cleanest single result in the re-review**: NVIDIA needed a 42-file engine
refactor to solve a problem our hooking mechanism does not have. Recorded because it is easy to
read an engine diff as a list of things we are *missing*.

**One real caveat, and it is ours to keep true**: the immunity depends entirely on our hook being
installed *before* the engine's call. `Host.cpp:335-343` deliberately waits for the game to load
`d3d12.dll` rather than forcing it early (an earlier build that forced it crashed ~11 s in). So
the ordering is a live invariant, not a structural guarantee. It is currently checked only by the
"pristine / ALREADY DETOURED" log line.

### 7.3 Requirement #2: back-buffer indexing — and why facts §32.4 is right

This is the change the branch tip is named for, and it is directly on our path. **The 2023 form**
(`dlss3/sl2-4.27-dlss-plugin`, `D3D12Viewport.cpp`):

```cpp
if (bNativelyPresented && SwapChain4)
    CurrentBackBufferIndex_RHIThread = SwapChain4->GetCurrentBackBufferIndex();
else
    { CurrentBackBufferIndex_RHIThread++; CurrentBackBufferIndex_RHIThread %= NumBackBuffers; }
```

**Why it is needed**: Streamline's swapchain presents *more* frames than the engine submits, and
the engine and Streamline **share one back-buffer ring**, so a manual `++` drifts from the real
DXGI index. Streamline treats this as a hard contract — `sl_dlss_g.h` defines a status bit for
it, and the plugin `checkf`s on it every frame:

```c
//! D3D12 integrations must use SwapChain::GetCurrentBackBufferIndex API
eFailGetCurrentBackBufferIndexNotCalled = 1 << 4,
```

**HARD.** This retroactively confirms the reasoning in `docs/STRAY-RENDERING-FACTS.md` §32.5,
which classified `eFailGetCurrentBackBufferIndexNotCalled` as *"a property of SL's swapchain
proxy; ours mirrors UE4's counter (32.4)"* — from binary strings alone, before this source was
available. That call was correct.

**We do not have the problem, because our rings are decoupled.** We hook
`IDXGISwapChain::GetBuffer` and hand the game **our own replacement textures**
(`src/backend_native/fg_present.cpp:179`, `replacement[kMaxReplacements]`), copying into the real
back buffer at each of our presents. The game therefore indexes **our** ring, which advances
exactly once per Present that reaches our hook — precisely what `core::fg::GameIndexMirror`
models. Streamline shares the real ring and must therefore read it; we own a private ring and
must therefore model the game's counter. **Two coherent designs; the requirement is a property
of Streamline's, not of frame generation in general.**

**And §32.4's mirror is correct *because* Stray's engine is unpatched.** It models the stock
`++ % NumBackBuffers` behaviour, which is exactly what stock 4.27.2 does. NVIDIA's patch changes
that behaviour — so if this project ever ran against an engine carrying the Streamline patches,
or a UE 5.6+ title, **the mirror would be wrong**. Worth writing down as a stated precondition
rather than an assumption: *the `GameIndexMirror` is valid only against an engine that manually
increments.* **HARD.**

**Epic adopted the fix upstream.** `EpicGames/UnrealEngine` @ `5.6`, `D3D12Viewport.cpp:608-618`:

```cpp
#if DXGI_MAX_SWAPCHAIN_INTERFACE >= 3
    if (bNativelyPresented && SwapChain3)
        SetBackBufferIndex_RHIThread(SwapChain3->GetCurrentBackBufferIndex());
    else
#endif
        SetBackBufferIndex_RHIThread(CurrentBackBufferIndex_RHIThread + 1);
```

— NVIDIA's 2023 form almost verbatim, in stock Epic UE5, with no NVIDIA markers in the file.
**HARD.** The manual increment survives only as the fallback.

### 7.4 Requirement #4: the one place NVIDIA has a capability we genuinely lack

`c0b55ad106`, "make various D3D12RHI symbols externally accessible", adds `D3D12RHI_API` to
`FD3D12CommandListHandle::FlushResourceBarriers`, `::GetResourceState`, `::AddTransitionBarrier`,
`FD3D12Resource::UpdateResidency`, and `class CResourceState` itself. **HARD.**

**What it buys NVIDIA: the authoritative answer to "what state is this resource in right now?"**
UE4 tracks per-resource, per-subresource state on the CPU; the plugin can ask, transition
correctly, and leave the engine's bookkeeping consistent.

**We have to derive it, and this project already knows the cost.** `CLAUDE.md`'s
`NgxNRRestoreHistory` ledger records the resulting state as *"**[derived], four source anchors,
ZERO measurements.** This is the risk"*, and notes that a wrong constant *"desynchronises the
ENGINE's bookkeeping, not just ours"*, softened on this target only because vkd3d-proton keeps
UAV-capable images in `VK_IMAGE_LAYOUT_GENERAL`. **That is exactly the gap this export list
closes for NVIDIA and leaves open for us.** Not fixable from outside without reconstructing
`FD3D12CommandListHandle`'s layout (Tier 2 ABI work); **STILL UNREACHABLE** in practice, and
worth naming as a permanent, known weakness rather than an oversight.

**A related one, also from the 4.27 plugin, which validates our own approach.** `CLAUDE.md` §5
warns that "NGX clobbers D3D12 command-list state… ReShade does not do this for us." NVIDIA's
4.27 plugin has the identical problem and solves it by talking to the engine:

```cpp
Device->GetCommandContext().StateCache.ForceSetComputeRootSignature();
Device->GetCommandContext().StateCache.GetDescriptorCache()->SetCurrentCommandList(...);
```

**HARD.** We do the same repair by hand against our own root/descriptor shadow
(`src/backend_native/root_shadow.cpp`, `descriptor_shadow.cpp`) because we have no
`FD3D12CommandContext`. **Our approach is the correct one for our position, and the hand-built
shadow is doing a job NVIDIA delegates to the engine** — useful context for the UE4SS
migration's expected value, and a reason the shadow is worth its complexity.

### 7.5 What NVIDIA learned later: 42 files became 4, and Epic took it

The single clearest instance of the question this document exists to answer.

**2022 — `dlss-streamline-4.27-engine-changes`:** the `DXBridge` approach. 42 files, a new
runtime module, every DXGI/D3D entry point in the engine rerouted, and a `TODO STREAMLINE ...
causes a circular reference..` left in `WindowsPlatformApplicationMisc.cpp` where the indirection
could not be applied. Broad, invasive, and it intercepts far more than it needs.

**2023 — `22bcaebe0b`, "Engine changes to support Streamline plugin":** **4 files.** One new
22-line header:

```cpp
class IDXGISwapchainProvider : public IModularFeature
{
public:
    static FName GetModularFeatureName() { static FName FeatureName(TEXT("DXGISwapchainProvider")); return FeatureName; }
    virtual bool SupportsRHI(const TCHAR* RHIName) const = 0;
    virtual TCHAR* GetName() const = 0;
    virtual HRESULT CreateSwapChainForHwnd(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
        const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**) = 0;
    virtual HRESULT CreateSwapChain(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**) = 0;
};
```

`FD3D12Viewport::Init` looks up implementations of the modular feature and, if one is present,
calls it instead of the factory. **Intercept the one thing that actually matters — swapchain
creation — instead of every DXGI entry point in the engine.**

**And Epic adopted it.** The header carries `// Copyright Epic Games, Inc. All Rights Reserved.`
and is present in **stock Epic UE5**: verified at `EpicGames/UnrealEngine` tags **5.4, 5.6 and
5.8**, and **absent at 5.0**. By 5.8 it has been refined further (`SupportsRHI(ERHIInterfaceType)`
instead of a string; `GetProviderName()`). **HARD.**

The 8.7.2 plugin uses it, and then wraps the result in its own thin `IDXGISwapChain4` proxy
(`StreamlineDXGISwapchainProxy`) whose `GetCurrentBackBufferIndex` forwards straight through —
registration gated on `FeaturesThatNeedSwapchainProvider = { kFeatureImGUI, kFeatureDLSS_G }`.

**What this means for us — and it is a caution, not an opportunity.** `IDXGISwapchainProvider`
does **not** exist in stock UE 4.27 (absent from Epic's `4.27` tag; NVIDIA adds it by patch), so
there is no sanctioned seam on our engine and our external swapchain hook is the only route.
More importantly: **the direction of NVIDIA's travel was to intercept LESS, more precisely.** Our
own present owner patches four swapchain methods plus `GetBuffer` and substitutes the entire
back-buffer ring — closer in spirit to the 2022 design than the 2023 one. That is forced (we have
no cooperating engine) and it is measured working (`docs/STRAY-RENDERING-FACTS.md` §32.7-32.11),
so this is **not** a recommendation to change anything. It is a reason to treat every widening of
our interception surface as a cost, and to prefer the narrowest hook that does the job — the
lesson NVIDIA's own two attempts encode.

### 7.6 The FG present model: NVIDIA never presents twice, and it is not available to us

Worth stating because it is the sharpest architectural contrast with our own design.
**The UE5 plugin never presents at all.** There is no `evaluateFeature` call for `kFeatureDLSS_G`
anywhere in the plugin; DLSS-G is driven purely by `slDLSSGSetOptions` + tags + constants, and
the extra frames are issued by `sl.dlss_g` **inside the interposed swapchain**, invisibly to UE.
The custom present it installs is a deliberate no-op:

```cpp
class FStreamlineDLSSGCustomPresent final : public FRHICustomPresent {
    virtual bool NeedsNativePresent() override final { return true; }      // engine presents normally
    virtual bool NeedsAdvanceBackbuffer() override final { return true; }  // engine advances normally
    virtual bool Present(FRHIViewport*, IRHICommandContext&, int32&) override final { return true; };
```

— existing only to release back-buffer references before a viewport resize. **HARD.**

**So "present twice" is our invention, not a reproduction of NVIDIA's design.** NVIDIA presents
once and lets a swapchain proxy inject frames; we present twice from a private ring. Their model
requires owning the swapchain end to end — which is `sl.interposer.dll`, the component this
project has a measured reason to avoid on this driver stack. **Not adoptable. Recorded so the
difference is a documented choice rather than an unexamined divergence**, and because it explains
why NVIDIA needs requirement #2 (7.3) and we do not.

One thing NVIDIA does that we do not, and it is small: the engine compensates for the extra
frames by scaling motion blur (`BeginRenderViewFamilyDLSSG` divides `MotionBlurTimeScale` and
`MotionBlurTargetDeltaTime` by the number of frames actually presented). **Not applicable** —
that is a cvar-level engine adjustment we would make through `Engine.ini` if motion blur were
ever observed to be wrong under FG, and it has not been.

---

## What could not be obtained, stated plainly

> **REWRITTEN 2026-09-03.** The first pass's headline limitation — "a pristine NVIDIA checkout;
> `NvRTX/UnrealEngine` returned HTTP 404" — **no longer applies**. That access was obtained and
> is the basis of this revision. What follows is the honest limit of *this* pass.

* **`nvngx_dlssg.dll` and `nvngx_dlss.dll` themselves.** Closed. Everything about what the
  snippets *do* with `CameraFar`, the presets, or the exposure texture remains inference from
  what NVIDIA's own callers send them. **No claim in §5.3 or §4 is a measurement of runtime
  behaviour**, and the difference matters: we now know precisely what NVIDIA sends and still do
  not know what happens if you send something else.
* **Nothing was run on the box.** This is a report-only, documentation-only task and the box is
  owned by another agent this session. Every "adopt" below is a proposal with a rationale, not a
  result. In particular the `CameraFar` change (§5.3) has never been tried.
* **A pristine Epic UE 5.8 diff of `D3D12Viewport.cpp`.** The `GetCurrentBackBufferIndex` block
  was read at `EpicGames/UnrealEngine` tag `5.6` and confirmed present with no NVIDIA markers;
  `IDXGISwapchainProvider` was confirmed present at tags `5.4`, `5.6`, `5.8` and absent at `5.0`.
  That is strong, but it is presence-checking, not a full diff — **SOFT** that no NVIDIA-specific
  modification hides elsewhere in those files.
* **UE 5.3-5.5 era plugins.** Only `dlss3/sl2-{4.27,5.0,5.1,5.2}-dlss-plugin` and
  `nvrtx-5.{0..8}` exist on the branch list; the jump from the 4.27 plugin (2024) to 8.7.2 (2026)
  skips intermediate versions, so a change attributed here to "between 4.27 and UE5" could have
  landed anywhere in between. Where this document says NVIDIA "changed its mind", that is
  **HARD** about the endpoints and **UNCONFIRMED** about when.
* **Whether any of §1.1's routes actually work.** No address for `GTemporalUpscaler` was located,
  no view extension was registered, no ABI was reconstructed. §1.1 argues the problem is smaller
  and better specified than the first pass thought; it does **not** demonstrate that it is
  solvable, and `ITemporalUpscaler::AddPasses` receiving an `FRDGBuilder&` is a real and
  unaddressed obstacle.
* **DLSS 4.5 / Dynamic Multi Frame Generation.** The newest plugin read is 8.7.2 (Streamline
  2.11.1). Anything later is **SOFT**, from release notes and press coverage only.
* **RTX Remix / DLSS-RR's per-guide contract** beyond what the plugin shows. This project's
  `docs/RESEARCH-RR-GBUFFER.md` covers it in more depth; not re-verified here. **Note it now
  needs its own re-review**: the ReShade-era sweep run for this task found that the PassFinder
  and GBufferFinder taps that document depends on are **inert under the current native host**
  (`src/backend_native/native_backend.cpp:316-317`, "the pass finder needs the ReShade
  backend"). That is a live gap, not a documentation one, and is out of scope here.

---

## Recommendations, ranked by expected value — RE-RANKED 2026-09-03

> The first pass's ranking is superseded. Two of its ten entries were **withdrawn as wrong**
> (#2, #3 — the preset table); several were confirmed and promoted. Provenance label on every
> line.

### Adopt

1. **`NgxFGCameraFar` — and take the whole PAIR.** Change from `0.0f` to NVIDIA's own
   `75000.0f`, and `NgxFGCameraNear` from the measured `View.NearPlane` to `0.01f`. **HARD** that
   both constants are what NVIDIA ships on *both* engine generations, and **HARD** from
   `sl_consts.h` that these parameters are non-optional with an `INVALID_FLOAT` sentinel — so
   `0.0` is the degenerate case, not a way of expressing "infinite". One config change.
   **UNCONFIRMED** that it changes an interpolated frame. §5.3.
   *(The first pass ranked this #1 on weaker evidence and was right; the near-plane half is new.)*
2. **Leave `CLAUDE.md` §5's preset table exactly as it is.** It is correct. The first pass's
   recommendation to "correct" it would have introduced two errors. **HARD**, NGX 310.6.0 header.
   §4. *Action: none — but the withdrawal needs recording so the bad edit is not made later.*

### Investigate — cheap, specific hypothesis

3. **The half-pixel offset in `shaders/mv_resolve.hlsl`.** UE5's `VelocityCombine.usf` samples
   the pixel **centre** (`PixelPos.xy + 0.5f`) in the camera-reconstruction branch; 4.27 sampled
   the corner. **UNCONFIRMED** which ours does. A uniform half-pixel bias in static-geometry
   motion vectors is precisely the silent, compounds-through-the-accumulation failure this
   project has already shipped once. Reading one shader settles it. §2.4. **New — neither pass
   had this.**
4. **Preset M at 50%.** NVIDIA's runtime default for the Performance band is **M**, not the K we
   pin everywhere; at 70% K is already what it would pick. **HARD** (Programming Guide §3.12,
   first-party, 31 March 2026). Prior expectation: no visible change, since J/K/M already tested
   indistinguishable here. A config flip. §4.
5. **Note the `dlss-exposure` interaction.** Exposure input is supported by presets **J and K
   only** (**HARD**, shipped Guide §3.9) — and at 50% the runtime default is **M**. Cross-
   reference for that branch, not this one; recorded because #4 and the exposure audit touch the
   same setting from opposite directions. §3.

### Record — no action, but the reasoning is now settled

6. **Motion-vector scale, reset detection, `MVLowRes`, non-dilated render-res guides,
   `.x > 0` velocity validity, `DoSharpening`-never-set, `MultiFrameCount=1`.** All confirmed
   correct against first-party source on both engine generations. Two are cases where **we are
   ahead of NVIDIA's own shipped code** (the 3-signal reset OR against their
   `bReset = View.bCameraCut` with its open TODO — present identically on *both* branches), and
   two are standing caveats now **retired** (SR's "default-1 dilation" no longer exists; the
   `all(.xy>0)` divergence is gone — NVIDIA moved to our convention). §2.2, §2.3, §2.4, §4, §5.4,
   §5.5.
7. **The engine-requirement list, and the two things it says about us.** §7. Requirement #1
   (`DXBridge`) is a 42-file problem we are immune to because we detour the export's prologue
   rather than its import table — **HARD**, and worth knowing before reading any engine diff as a
   list of gaps. Requirement #4 (the `D3D12RHI_API` resource-state exports) is a genuine
   capability we lack and cannot obtain: our restore-state constant stays **[derived] with zero
   measurements**, a named permanent weakness. §7.2, §7.4.
8. **`GameIndexMirror` has a stated precondition now.** It is correct **because Stray's engine is
   unpatched** and manually increments. It would be wrong against a Streamline-patched 4.27 or a
   UE 5.6+ title, where the engine reads `GetCurrentBackBufferIndex()`. **HARD.** Nothing to
   change; write the precondition down. §7.3.

### Long-horizon — re-graded, still not now

9. **`ITemporalUpscaler` (§1.1) — re-graded EASIER, and it is now the cheaper of the two engine
   routes.** On 4.27 NVIDIA registers by assigning the global `GTemporalUpscaler`; the interface
   is stock 4.27. So the task is "locate one global, supply a 5-entry vtable", not "AOB-scan a
   private renderer function" as the first pass had it. **The real obstacle is `AddPasses`
   receiving `FRDGBuilder&`** — emitting RDG passes from an injected DLL is the hard part and is
   unaddressed. **UNCONFIRMED**; still deserves its own scoped assessment, still not a side
   effect of this audit.
10. **Hudless (§5.2) — re-graded, and now clearly SEPARATE from #9.** The capture point
    (`SubscribeToPostProcessingPass(VisualizeDepthOfField)`) is on stock 4.27's
    `ISceneViewExtension`, but registering a view extension needs `GEngine->ViewExtensions` (not
    a `UPROPERTY`) and a correct vtable, and `NewExtension` is a template that cannot be hooked.
    **The UI half is dead regardless**: NVIDIA's alpha mask depends on the `DrawClearQuadAlpha`
    engine patch, and NVIDIA's own README tells HDR titles — which we are — to switch UI tagging
    off entirely. **HARD.** So the prize is hudless colour alone. §5.2.
11. **The alternate-motion-vector extension point for reflections.** Confirmed **absent from the
    4.27 plugin entirely** — it is a UE5-era addition, compiled out even there
    (`SUPPORT_GUIDE_GBUFFER 0`). **HARD.** It needs a new `FSceneTextures` member and a base-pass
    write, i.e. Stray's engine source. Unchanged verdict: not reachable. §2.1.

### Not applicable

12. **Streamline's present model, swapchain ownership, MFG beyond 2x, dynamic resolution, Vulkan
    queue parallelism, VSync-with-FG, UI recomposition.** Ruled out by Ada's 2x ceiling, Stray's
    fixed screen percentage, the D3D12-only vkd3d constraint, gamescope's compositor, or (for the
    present model, §7.6) by requiring `sl.interposer.dll`. **NVIDIA never presents twice** —
    that is our invention, forced by not owning the swapchain end to end, and it is measured
    working. §5.5, §5.7, §6, §7.6.

---

## Stale-elsewhere note — EXPANDED 2026-09-03 from a repo-wide sweep

The first pass flagged two documents as still describing this project as a ReShade add-on. A
systematic sweep for **ReShade-era *feasibility* claims** (statements of the form "we cannot
reach X", "not available to an add-on", "that would need the engine") was run for this
re-review, because several such claims were true only of the old architecture. Findings that
belong to other files are listed here rather than fixed, since this is a docs-only task scoped
to two documents.

**The framing that decides most of it.** "ReShade" and "outside the engine" were treated as one
wall. They are two, and only one fell:

* **Tier 1 — UObject-reflected game state: REACHABLE, and already precedented in this repo.**
  `mods/StrayDualSense` hooks a real UFunction and reads `HKGameUserSettings` properties;
  `mods/StrayProbe` does the same from Lua. Live cvars via `UObject::ProcessConsoleExec` would
  make `r.RayTracing`, `r.SSR.Quality` and `r.ScreenPercentage` **within-session A/B variables** —
  the single highest value-to-cost item the sweep found, and it touches several experiments this
  project has run the expensive way.
* **Tier 2 — renderer-internal C++ (`FViewInfo`, `FSceneViewFamily`, `ITemporalUpscaler`,
  `FRenderTargetPool`): NOT reachable by reflection, and that has not changed.** What changed is
  that AOB-scan + struct offsets + MinHook is now available in-process. These move to
  **UNTESTED**, never straight to reachable. §1.1 is the worked example.
* **Tier 3 — genuinely unchanged**: stripped DXBC reflection (`D3DReflect` is still impossible),
  absent `NAME_OBJECTS` debug names, hardware-level FG present pacing, and correct motion
  vectors for screen-space reflections.

**Flagged for whoever owns these files** (all verified file:line by the sweep):

* **`CLAUDE.md:3`** — the manual's one-line self-definition still reads *"A ReShade **add-on**…"*.
  Highest-priority single fix in the repo. Likewise **`docs/STRAY-RENDERING-FACTS.md:3-4`**
  ("gathered while building a ReShade D3D12 add-on") and **`:14`**, which still lists ReShade in
  the environment baseline.
* **`CLAUDE.md` §3 and §5** — "How the add-on works" (the ASCII diagram is framed
  `┌── ReShade add-on events ──┐`) and the whole "ReShade 6.8 add-on API" section. The first pass
  already noted these; the sweep confirms they are describing a code path that is not how the
  project runs.
* **`docs/RESEARCH-OFFICIAL-DLSS-UE-PLUGIN.md` §D.2** — needs exactly the reclassification §1.1
  applies here. Its scoping predicate ("not reachable *while staying inside ReShade's
  device/command-list hooking model*") is now simply false, and the document has no architecture
  note at all.
* **`docs/RESEARCH-RENODX-DLSS5.md` contradicts itself** — §9 (*"the largest single difference is
  the hook point, and it is not available to us"*) is refuted 75 lines later by its own §10
  (*"Can we run NR as a STAGE rather than a HOOK? — verdict: YES, at present"*). §9 is a
  pre-`present_owner` judgement that survived into a document that overturns it.
* **`docs/RESEARCH-RR-GBUFFER.md`** — cites `src/addon.cpp:1082-1084`, **a file that no longer
  exists**, and its §1.4 negative result (the 6-vs-7 MRT anomaly) may be a ReShade
  observation artefact: our own `src/frame_state.hpp:79-86` records that ReShade's
  `descriptor_tracking` keeps a dangling pointer for `descriptor_table_with_flags`, the binding
  variant UE4 uses, so its walk "silently visits nothing". **That negative should be re-run on the
  native backend before being treated as an engine fact** — but note it cannot be, yet: the
  graphics taps it needs are not implemented under the current host.
* **Live runtime strings that are wrong in the user's log** — `src/shader_dump.cpp:36` tells the
  user to edit **`ReShade.ini`** (the plugin reads `StrayDLSS.ini`); `src/gbuffer_finder.cpp:734`
  blames *"this ReShade build"*; `src/app/dlss_app.cpp:1440` warns that *"ReShade will drop the
  D3D12 PSO cache"*. These are user-facing and misleading.
* **`src/ngx_backend.hpp:5-6`** states the NGX thread-safety invariant in terms of *"the thread
  that owns the ReShade device/command-list callbacks"* — a thread that no longer exists. The
  invariant is presumably still satisfied by the present owner, but it is now stated against a
  nonexistent referent, which for a thread-safety rule is worth fixing.
* **`CLAUDE.md`'s ReShade-prerequisite framing** — *"ReShade ≥ 6.8.0 is a hard prerequisite"* is
  now a **Config-B-only** constraint (the plugin supports running with or without ReShade;
  `ext_unhook` is retained and is **inert without it**, measured). And *"the shipped
  configuration is the broken row"* refers to a configuration that no longer ships.
* **One live code consequence, reported and deliberately NOT fixed here** (docs-only task):
  `src/app/dlss_app.cpp:1226-1237` gates NGX initialisation on `hooked || frame >= 900`, where
  `hooked` means *ReShade owns the ext vtable slot*. With no ReShade in the process it is always
  false, so **NGX now waits the full 900 presents instead of initialising at 120** — roughly 780
  presents of avoidable startup delay, purely from a ReShade-era decision. **Worth fixing; not
  ours to fix in this task.**
* **The build and the docs disagree about what ships.** `CMakeLists.txt:11` defaults
  `STRAY_DLSS_BUILD_ADDON` **ON**, and `.github/workflows/ci.yml` attaches the ReShade add-on as
  the **GitHub Release** artifact while telling users to attach `ReShade.log`; the UE4SS plugin
  workflow uploads an artifact with **no release step**. So the add-on is the only artefact
  reaching a tagged release, while the plugin is the only thing the documentation describes as
  how the project runs. That is a factual inconsistency, not stale prose.
