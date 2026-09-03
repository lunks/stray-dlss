# The engine's own upscaler seam: replacing behavioural TAA identification with UE 4.27's

**The question.** Every part of how this project decides *which compute dispatch is Stray's TAA
pass* is behavioural — a DXBC FNV1a hash, a "depth+stencil SRV over one resource" signature,
dispatch-rect-versus-view-rect arithmetic, an aspect-ratio gate, a permutation-hash pin with a
300-present staleness release, and six named refusal reasons for when all of that still says no
(`src/core/taa_signature.cpp`, `src/taa_hook.cpp`). **All of it exists because the project began
as an out-of-process ReShade add-on that could see nothing but D3D12 descriptors.** We are now a
UE4SS C++ plugin running inside the engine's own address space, and UE 4.27 has a first-class
extension point for exactly this. This document assesses whether we can use it.

**The user's framing, verbatim:** *"we've been using characteristics of the shader because this
started as a reshade plugin. Now that we are a UE4SS plugin, we should be able to better figure
out and hook on the right shader, like the DLSS plugin does."*

**The live symptom this is about:** *"I sometimes see the issue where the whole screen appears
suddenly which in the past have got to do with the wrong shader being picked by the dlss sr
override."*

**Companion documents, both load-bearing and neither duplicated here.**
`docs/RESEARCH-OFFICIAL-DLSS-UE-PLUGIN.md` establishes what NVIDIA's own plugin does and closes
with the observation this document takes up: *"Using the real extension point from an injected DLL
would require actual code execution inside the engine's own address space … not something
decidable from ReShade alone."* `docs/RESEARCH-UE4SS-MIGRATION.md` §6.1 establishes, HARD, that
UE4SS's UObject reflection cannot reach any renderer type — so reflection is not a route and is
not re-litigated below.

---

## 0. Verdict, up front

| Rung | What it is | Feasible? | Status |
|---|---|---|---|
| **L0 ORACLE** | Stand in for `FDefaultTemporalUpscaler::AddPasses` via its vtable; read the engine's own output rect and input textures; cross-check the heuristic matcher against it | **Yes, and cheaply.** One string anchor, one qword patch, three exact self-checks, no code signature, no engine headers | **MEASURED ON THE BOX 2026-09-03** (§10, facts §36): found at every stage with one candidate, installed, forwarding, `orphans=0` over 8570 announcements, two look-alikes caught. **Now THE GATE**: `EngineSeam=3` is the default and DLSS SR runs only on the announced dispatch |
| **L1 IDENTIFY** | Map the `FRDGTextureRef`s L0 hands us to `ID3D12Resource*` so interception targets the engine's resources by identity | **Yes** — `FRHITexture::GetNativeResource()` is a virtual returning `ID3D12Resource*`; two derived offsets, and the null-at-`AddPasses` problem is solved by resolving at CLAIM time instead | **BUILT** (§11), `EngineSeamInputs=1`. The offsets are still [derived]; a failure falls back and is counted |
| **L2 OWN IT** | Register our own `ITemporalUpscaler` on the view family and implement `AddPasses` | **The registration is feasible and is NOT the hard part.** Implementing `AddPasses` from an injected DLL means authoring RDG passes and reading `FViewInfo`, and `FViewInfo` is a Renderer-private class of ~180 heavily `#if`-conditioned members. **Do not attempt the full replacement.** The *useful half of L2 is already what L0 does* | **Subsumed.** See §4.3 |

**The load-bearing discovery, and it is the reason L0 is worth doing at all:**
`ITemporalUpscaler::AddPasses` is called from **exactly two** places in the whole engine
(`PostProcessing.cpp:559` desktop, `:2005` mobile), and **every documented FTAAStandaloneCS
look-alike reaches the shader by a different route that never touches the interface.** Being
called through it *is* the identification. There is no archaeology left to do.

---

## 1. Sources, and how far each can be trusted

| Source | Trust |
|---|---|
| `AlexMercer-MA/UnrealEngine-4.27`, whose `Engine/Build/Build.version` reads 4.27.2 / `++UE4+Release-4.27` — **our exact engine version** | **HARD** for anything quoted |
| `Engine/Source/Programs/UnrealBuildTool/**` in the same mirror | **HARD** for what UBT does to `*_API` macros |
| GitHub code search over that mirror (used for "is this the only caller") | **SOFT** — the index is not provably complete for a fork of a repo this size. Every claim below that rests on it is labelled |
| `Stray-Win64-Shipping.exe` | **Not consulted when this was written** (the box was unreachable), then **run against on 2026-09-03** with the scan at both levels (§10, facts §36). Every byte-level claim §4.1 makes about GetDebugName's shape, the vtable's slot order and the fraction accessors is HARD on that executable now |

**That last row was the single most important caveat in this document** when it was written; it
is why level 1 installs nothing and why discovery is *static*. The design held: the first run
found one candidate at every stage and validated all three constants, and the second installed the
stand-in with no visible change. §10 has the lines.

---

## 2. The fact that governs everything: there are no exports

**HARD**, `UnrealBuildTool/Configuration/UEBuildModule.cs:545-554`:

```csharp
if(Rules.Target.LinkType == TargetLinkType.Monolithic)
{
    if (Rules.Target.bShouldCompileAsDLL && (...))
        Definitions.Add(ModuleApiDefine + "=DLLEXPORT");
    else
        Definitions.Add(ModuleApiDefine + "=");          // <-- a game .exe lands here
}
```

with `ModuleApiDefine = Name.ToUpperInvariant() + "_API"` (`:180`). A monolithic game executable
is not compiled as a DLL, so **`RENDERER_API`, `ENGINE_API`, `RHI_API` and `D3D12RHI_API` all
expand to nothing.** Consequently `Stray-Win64-Shipping.exe` exports none of:

* `AddTemporalAAPass` (`TemporalAA.h:139`, declared `extern RENDERER_API`)
* `ITemporalUpscaler::GetDefaultTemporalUpscaler` / `GetTemporalUpscalerMode`
* `GCustomStaticScreenPercentage` (`CustomStaticScreenPercentage.h:29`, `extern ENGINE_API`)
* `FSceneViewExtensions::RegisterExtension`

The task asked for this to be verified rather than assumed, and it is — from the build system's
own source, which does not need the binary. **A `dumpbin /EXPORTS` on the box remains worth one
minute** as an independent confirmation, and if by some accident the renderer were a separate DLL
with exports the whole problem would collapse to `GetProcAddress`; nothing in UBT suggests it is.

Two more build-configuration facts that change layouts and must be assumed for Stray (Shipping):

* **`RDG_ENABLE_DEBUG` is 0** — `RenderGraphDefinitions.h:13`,
  `#define RDG_ENABLE_DEBUG (!UE_BUILD_SHIPPING && !UE_BUILD_TEST)`. This removes
  `FRDGResource::FDebugData` and reduces `FRDGResource::GetRHI()` to a plain load.
* **`ENABLE_RHI_VALIDATION` is 0** — `Core/Public/Misc/Build.h:427-429`. This removes a *second
  base class* from `FRHITexture` (`RHIResources.h:965`), i.e. a second vptr, so `FRHITexture` has
  exactly one.

---

## 3. What the engine's seam actually is

### 3.1 One interface, one call site

**HARD.** `Renderer/Private/PostProcess/TemporalAA.h:147-186` declares `ITemporalUpscaler` with
five virtuals in this declaration order — which for MSVC is slot order, since there is no virtual
base and no other virtual:

| slot | member |
|---|---|
| 0 | `~ITemporalUpscaler` (compiler-generated deleting destructor in the derived class) |
| 1 | `const TCHAR* GetDebugName() const` |
| 2 | `void AddPasses(FRDGBuilder&, const FViewInfo&, const FPassInputs&, FRDGTextureRef*, FIntRect*, FRDGTextureRef*, FIntRect*) const` |
| 3 | `float GetMinUpsampleResolutionFraction() const` |
| 4 | `float GetMaxUpsampleResolutionFraction() const` |

`GetDefaultTemporalUpscaler()` and `GetTemporalUpscalerMode()` are **statics** and not in the
vtable.

`FPassInputs` (`TemporalAA.h:150-157`) is five members: `bool bAllowDownsampleSceneColor`,
`EPixelFormat DownsampleOverrideFormat`, then `FRDGTextureRef SceneColorTexture`,
`SceneDepthTexture`, `SceneVelocityTexture`.

The consumer, `PostProcessing.cpp:535-566`:

```cpp
int32 UpscaleMode = ITemporalUpscaler::GetTemporalUpscalerMode();
const ITemporalUpscaler* DefaultTemporalUpscaler = ITemporalUpscaler::GetDefaultTemporalUpscaler();
const ITemporalUpscaler* UpscalerToUse = ( UpscaleMode == 0 || !View.Family->GetTemporalUpscalerInterface())? DefaultTemporalUpscaler : View.Family->GetTemporalUpscalerInterface();
...
UpscalerToUse->AddPasses(GraphBuilder, View, UpscalerPassInputs,
    &SceneColor.Texture, &SecondaryViewRect,
    &HalfResolutionSceneColor.Texture, &HalfResolutionSceneColor.ViewRect);
```

`UpscalerToUse`'s static type is `const ITemporalUpscaler*`, so **the call cannot be
devirtualised** even though `FDefaultTemporalUpscaler::AddPasses` is declared `final` — the
compiler does not know the dynamic type at that site. A vtable patch is therefore honoured.

### 3.2 The look-alike census — the finding that makes L0 decisive

**Every pass this project has had to exclude by binding archaeology calls `AddTemporalAAPass`
DIRECTLY and never goes through `ITemporalUpscaler`.** From code search over the mirror (**SOFT**
that the list is exhaustive; **HARD** for each entry, read at its call site):

| caller | `ETAAPassConfig` | today excluded by |
|---|---|---|
| `TemporalAA.cpp` `AddGen4MainTemporalAAPasses` | `Main` / `MainUpsampling` / `MainSuperSampling` | **this is the one we want** — and the only one reached through the interface |
| `DiaphragmDOF.cpp` | `DiaphragmDOF`, `DiaphragmDOFUpsampling` | the depth+stencil signature, by luck |
| `LightShaftRendering.cpp` | `LightShaft` | ditto |
| `IndirectLightRendering.cpp` | `ScreenSpaceReflections` | the `FSSDTemporalAccumulationCS` exclusion list and `cb1[145]` reasoning |
| `SingleLayerWaterRendering.cpp` | (water) | not previously enumerated at all |
| `PostProcessing.cpp:2327` `FPostProcessing::ProcessPlanarReflection` | **`Main` or `MainUpsampling`** — a full main-config TAA on a planar reflection, with its OWN `FRDGBuilder` and `Execute()` | the aspect-ratio / upscale-factor gate (`kGateNotPrimaryView`), added 2026-09-01 after DLSS features were being created 11 times a session |
| `PostProcessing.cpp:1224` `AddDebugViewPostProcessingPasses`, `DVSM_RayTracingDebug` | `Main`-family | nothing; it does not run in normal gameplay |

The planar-reflection row is the important one. It is a genuine `Main`-config
`FTAAStandaloneCS` dispatch, structurally indistinguishable from the real thing except by
*resolution*, and today it is excluded by a heuristic aspect-ratio band (0.96–1.04) and an
upscale-factor ceiling (3.5) in `taa_signature.cpp:259-281`. **Through the interface it is
excluded by construction: it never appears.**

### 3.3 Registration, and the global that does it

**HARD.** `FSceneViewFamily::TemporalUpscalerInterface` is private (`SceneView.h:1847`); the
setter (`:1807`) and getter (`:1814`) are both `FORCEINLINE`, so **there is no callable function
to hook** — a registration must write the field, or be handed a `FSceneViewFamily&`.

The engine provides exactly that, and it is a single writable global:

```cpp
// Engine/Source/Runtime/Engine/Public/CustomStaticScreenPercentage.h:14-29
class ENGINE_API ICustomStaticScreenPercentage
{
public:
    virtual ~ICustomStaticScreenPercentage() {};
    virtual void SetupMainGameViewFamily(FSceneViewFamily& ViewFamily) = 0;
    virtual void SetupViewFamily(FSceneViewFamily&, TSharedPtr<ICustomStaticScreenPercentageData>) {};
    virtual float GetMinUpsampleResolutionFraction() const = 0;
    virtual float GetMaxUpsampleResolutionFraction() const = 0;
};
extern ENGINE_API ICustomStaticScreenPercentage* GCustomStaticScreenPercentage;
```

read once, on the game thread, at `GameViewportClient.cpp:1400-1402`:

```cpp
if (GCustomStaticScreenPercentage && ViewFamily.ViewMode == EViewModeIndex::VMI_Lit)
{
    GCustomStaticScreenPercentage->SetupMainGameViewFamily(ViewFamily);
}
```

**This is exactly where NVIDIA's plugin registers** (`docs/RESEARCH-OFFICIAL-DLSS-UE-PLUGIN.md`
§A.1: `FDLSSUpscaler::SetupMainGameViewFamily` is an override of `ICustomStaticScreenPercentage`).
It sits at line 1400, *before* `CalcSceneView` at `:1450`, which matters because two side effects
in `FSceneView`'s own constructor fire only if the interface is already set:

* `SceneView.cpp:837-841` — a non-null temporal upscaler forces
  `PrimaryScreenPercentageMethod = TemporalUpscale` **without** needing `r.TemporalAA.Upsampling`.
* `SceneView.cpp:937-941` — inside `SetupAntiAliasingMethod`, it forces
  `AntiAliasingMethod = AAM_TemporalAA`.

So a registered upscaler gets TAAU unconditionally. **For Stray this buys nothing** — the game
already ships `r.TemporalAA.Upsampling=True` and `r.DefaultFeature.AntiAliasing=2` (CLAUDE.md
§2.3.1) — but it is worth recording, because it is the one thing a real registration has that a
vtable patch does not.

**Two things NOT to build on.** `GTemporalUpscaler` (`TemporalAA.h:186`, `TemporalAA.cpp:1521`,
assigned at `Renderer.cpp:56`) is **written and never read** — `r.TemporalAA.Upscaler`'s help text
still advertises it and the help text is stale. And `GEngine->ViewExtensions` is **not a
`UPROPERTY`** (`Engine.h:1768-1769`), so the scene-view-extension route is invisible to reflection
and sits among three identically sized `TSharedPtr` neighbours — strictly worse than the global
above.

### 3.4 `r.TemporalAA.Upscaler` is already 1

**HARD**, `TemporalAA.cpp:70-77`: `CVarUseTemporalAAUpscaler` defaults to **1**, and mode 0 is what
forces the engine's own. So no config change is needed for the engine to honour an installed
upscaler. Worth one grep of the box's `Engine.ini` / the pak's `DefaultEngine.ini` to confirm Stray
does not ship a 0; nothing in `docs/game-config/` sets it.

---

## 4. The ladder

### 4.1 L0 — ORACLE. Built, and this is what it does

**The mechanism, and why it needs no code signature.** `FDefaultTemporalUpscaler::GetDebugName`
is one line (`TemporalAA.cpp:1527-1530`):

```cpp
virtual const TCHAR* GetDebugName() const { return TEXT("FDefaultTemporalUpscaler"); }
```

At `/O2` that is `lea rax, [rip+disp]; ret` — eight bytes. So:

1. Find the UTF-16LE literal `"FDefaultTemporalUpscaler\0"` in any readable section.
2. Find `48 8D 05 <int32> C3` in an executable section whose rip-relative target is that literal.
   That address is `GetDebugName`. If the rip-relative form finds nothing, `48 B8 <uint64> C3`
   (`movabs rax, imm64; ret`) is tried — the other way x86-64 can materialise an absolute
   address. Covering a second shape costs 25 lines; a missed shape costs a whole round trip.
3. Find any 8-byte-aligned qword in a non-executable section equal to it. That is vtable slot 1;
   the vtable base is 8 bytes below. Require all five slots to point into executable memory.
4. **Validate against three exact predictions, statically — without executing anything.** Decode
   slots 3 and 4, which are `return <compile-time constant>;`
   (`FSceneViewScreenPercentageConfig::kMin/kMaxTAAUpsampleResolutionFraction`, `SceneView.h:1438-1439`):
   they must be **exactly 0.5 and 2.0**, both exactly representable in binary32. Together with
   slot 1 returning the literal we already found, that is three independent constants of three
   different kinds reproduced from one scan.

This is deliberately the same shape as the row-135 pre-exposure self-check (CLAUDE.md §2.6): a
candidate that is wrong cannot reproduce all three. Nothing is executed at level 1, so a wrong
candidate costs a refusal and a log line, never a call into arbitrary code.

`src/core/engine_seam.{hpp,cpp}` is that scan, pure — bytes in, verdict out — and
`tests/test_engine_seam.cpp` builds a synthetic module and drives it, including the near-misses:
a wrong literal, a blanked `GetDebugName`, a blanked vtable, a slot pointing at data, an
undecodable accessor (whose bytes are then logged verbatim, so an unfamiliar MSVC codegen costs
one round trip rather than a guess), constants of 0.5/4.0, and a decoy five-pointer run that must
lose to the real one. `src/engine_seam_hook.cpp` is the live half: it walks the PE section headers
of `GetModuleHandleW(nullptr)`, runs the scan, logs, and at level 2 patches slot 2.

**What the stand-in reads, and what it deliberately does not.** Our thunk is a free function whose
first parameter is `this` — on x86-64 MSVC there is one calling convention and a `const` member
function of a class with no virtual bases passes `this` in RCX and each remaining pointer or
reference in RDX/R8/R9/stack, so the two are ABI-identical. It reads, per call:

* the three `FRDGTexture*` from `FPassInputs` at offsets 8 / 16 / 24 — **for identity only, never
  dereferenced** (see L1 for why);
* then forwards to the engine's own `AddPasses`;
* then reads `*OutSceneColorViewRect` (`FIntRect` = four `int32`), which the engine has just
  written.

**Why that rect is the whole prize.** `AddGen4MainTemporalAAPasses` sets
`*OutSceneColorViewRect = SecondaryViewRect` where `SecondaryViewRect = TAAParameters.OutputViewRect`
(`TemporalAA.cpp:1456`, `:1516`), and the dispatch is
`GetGroupCount(PracticableDestRect.Size(), GTemporalAATileSizeX)` (`:958`) with
`PracticableDestRect = DivideAndRoundUp(Inputs.OutputViewRect, Inputs.ResolutionDivisor)`
(`:654-656`) and `ResolutionDivisor == 1` for every `Main*` config. **So the group counts the game
is about to issue are exactly `ceil(the announced rect / 8)`, and we know them before the dispatch
happens.** All HARD.

**The correlation rule** (`seam::Ledger`, pure and tested): announcements go into a small ring;
each candidate dispatch the existing matcher accepts asks to claim one whose expected group counts
equal its own; each announcement can be claimed once. Three outcomes, three counters:

* `claimed` — the two answers agree.
* `rect_mismatch` — an announcement is pending and this dispatch is not it. The matcher matched a
  look-alike.
* `orphans` — **no announcement at all this frame, and the matcher still called a dispatch the TAA
  pass.** This is the wrong-pass class, named, per frame. It is the number the whole exercise is
  for, and it is what "the whole screen appears suddenly" has been until now.

The first disagreement per pass hash also emits one `ENGINE SEAM DISAGREES` WARN naming the hash
and the group counts, in the same discipline as `log_gate_refusal`.

**Cost.** One read-only scan of our own module at device init (a few tens of MB of `memcmp`; the
elapsed ms is logged), one qword write, one forwarding call per view per frame, and a ring of
eight structs. No GPU work, no allocation on the render thread.

**What fails first if a convention is wrong**, in order:

| # | If this is wrong | The symptom |
|---|---|---|
| 1 | The literal is not in the image, or `GetDebugName` is not lea+ret | `ENGINE SEAM: NOT FOUND` with the stage it reached. Nothing installed. **RULED OUT 2026-09-03**: found, one candidate each |
| 2 | The vtable slot order is not declaration order | slots 3/4 do not decode to 0.5/2.0 → `fraction_mismatch`, refused. **RULED OUT**: decoded to exactly 0.5 and 2.0 |
| 3 | MSVC emitted a fourth shape for `return 0.5f` | `fraction_shape`, refused, with 16 bytes of hex logged. **RULED OUT** |
| 4 | `sizeof(EPixelFormat) != 4` | the three `FPassInputs` pointers are read 4 bytes off. They are only compared, so the visible effect is that they look like garbage in the first log line — and the rect, which is the part we correlate on, is unaffected. **Not falsified**: the three pointers read as plausible heap addresses on the first call |
| 5 | `FIntRect` is not four `int32` | the announced rect is nonsense; `rect_mismatch` climbs to 100% and `unreadableRect` counts the degenerate ones. **RULED OUT**: 3840x2160 on frame 0, `unreadableRect=0` across 8570 announcements |
| 6 | The thunk's ABI is wrong | a crash on the first frame with TAA. **RULED OUT**: 8570 forwarded calls, image unchanged, SR/FG/NR all ran |
| 7 | `r.TemporalAA.HistoryScreenPercentage > 100` | the config becomes `MainSuperSampling` and `TemporalAA.cpp:1466-1474` overrides `OutputViewRect` *after* `SecondaryViewRect` was captured, so the announced rect stops matching the dispatch. CLAUDE.md §4 already requires this to stay 100; now there is a counter that would say so |

Row 4 deserves its label: `EPixelFormat` is an unscoped `enum` with no fixed underlying type whose
enumerators run 0..~80 (`Core/Public/PixelFormat.h:12`), so MSVC gives it `int`. **HARD from the
declaration**, and consistent with what the first call read; a wrong offset here would have
shown as two of the three pointers sharing a value or one being tiny, and neither happened.

### 4.2 L1 — IDENTIFY. Not built; the last hop is trivial and the first two are not

The prize is to stop matching on *shape* and match on *identity*: the engine hands us its scene
colour, depth and velocity, and our D3D12 interception recognises those exact `ID3D12Resource*`.

**The chain, all HARD from source:**

```
FRDGTextureRef                      = FRDGTexture*                      RenderGraphDefinitions.h:555
FRDGResource::ResourceRHI           = FRHIResource*                     RenderGraphResources.h:121
FRDGTexture::GetRHI()               = static_cast<FRHITexture*>(...)    RenderGraphResources.h:646-649
FRHITexture::GetNativeResource()    virtual, returns void*              RHIResources.h:997
TD3D12Texture2D::GetNativeResource() override final -> Resource->GetResource()
                                                                        D3D12Texture.h:311-331
FD3D12Resource::GetResource()       = Resource.GetReference()           D3D12Resources.h:171
                                    -> ID3D12Resource*
```

**`FRHITexture::GetNativeResource()` is the single best fact in this whole document.** It is a
*virtual member function returning the `ID3D12Resource*` directly*, so it needs no export and no
symbol — only a correct `this` and a slot index. In a Shipping build (`ENABLE_RHI_VALIDATION == 0`,
so one vptr) `FRHIResource` declares exactly one virtual (its destructor) and `FRHITexture`
declares, in order, `GetTexture2D`, `GetTexture2DArray`, `GetTexture3D`, `GetTextureCube`,
`GetTextureReference`, `GetSizeXYZ`, **`GetNativeResource`**, … — putting it at **slot 7**.
`FRHITexture2D` adds no new virtuals. Slot 7 is **[derived]** from MSVC vtable rules, not measured.

**Two obstacles, and the second is the real one.**

1. **`FRDGResource`'s layout must be derived.** In Shipping (`RDG_ENABLE_DEBUG == 0`) it is
   `+0` vptr, `+8` `const TCHAR* const Name`, `+16` `FRHIResource* ResourceRHI` — three members,
   shallow, and `GetRHI()` reduces to that single load with no `checkf`. Low risk, but derived.

2. **`ResourceRHI` is NULL at `AddPasses` time for a graph-allocated texture.** `FRDGBuilder::Execute()`
   assigns it in the `CollectPassResources` loop (`RenderGraphBuilder.cpp:1222-1233` →
   `BeginResourceRHI` → `Texture->SetRHI(...)` at `:2084`), and the pass lambdas only run at
   `:1300-1312`. `AddPasses` is graph *setup*. **Externally registered textures are the exception**
   — `RegisterExternalTexture` calls `SetRHI` immediately (`:516`) — and Stray's scene depth and
   velocity come from `GetSceneTextureParameters`, which registers `FSceneRenderTargets`' own
   targets externally. So depth and velocity are **plausibly** non-null at `AddPasses`; scene
   colour has passed through DOF and post-process material passes by then and is **plausibly**
   transient and null. **SOFT, and cheaply settled:** log the three `GetRHI()` values at L0 level 2
   and see. That costs one dereference of a derived offset, which is why it is not in this change.

**If colour is null**, L1 still works for depth and velocity — which is enough, because those two
are what the current matcher's `kGateDeadInputs` and render-extent logic hang off, and the colour
input is already resolved by register (§2.3). If more is needed, the honest answer is not to guess
harder: it is to record our own RDG pass and read the pointers inside its lambda, which is L2 work.

**Cost estimate:** ~150 lines plus tests for the offset decode. **Validate the ANSWER, not the
slot index** — slot 7 is a derivation, and §9's rule against calling a discovered address applies
to checking it by calling slot 6 as much as to anything else. The cheap, safe check is that the
`ID3D12Resource*` slot 7 returns is one our own resource registry already knows is live
(`is_resource_live`), and that it is the same pointer the descriptor walk resolves for that
register on the same frame. A wrong slot index returns something the registry has never seen, and
that is a refusal, not a fault. **Risk:** moderate — three derived offsets, all dereferencing
engine memory, where L0 dereferences only its own parameters.

### 4.3 L2 — OWN IT. Registration is easy; `AddPasses` is not; and L0 already ate the useful half

**Registration is genuinely feasible.** Write `GCustomStaticScreenPercentage` with a pointer to a
four-slot vtable we author, and `UGameViewportClient::Draw` calls
`SetupMainGameViewFamily(ViewFamily)` on the game thread, once per frame, before the views are
constructed. We would then write `TemporalUpscalerInterface` on the family — a private field, but
the accessors are `FORCEINLINE` so writing the field is *exactly* what the engine does. That needs
its offset, which agent-derived at **≈184** in a Shipping build and which **hinges entirely on
`sizeof(FEngineShowFlags)`** (a bitfield whose width changes with `UE_BUILD_OPTIMIZED_SHOWFLAGS`,
`ShowFlags.h:39-42`, 61 always-accessible flags versus 171 in Development). **Do not hardcode 184.**
The robust locator is structural: it is the **second of four consecutive pointers at the very end
of the object** (`ScreenPercentageInterface`, `TemporalUpscalerInterface`,
`PrimarySpatialUpscalerInterface`, `SecondarySpatialUpscalerInterface`, `SceneView.h:1845-1849`,
nothing after them).

**The blocker is not registration. It is `AddPasses`.** Implementing it means:

* authoring RDG passes — `FRDGBuilder::AddPass` with a shader parameter struct, an
  `FShaderParametersMetadata`, a `TShaderMapRef` — from a DLL with no engine headers and no
  linkage. This is not an offsets problem, it is a "reimplement RDG's template machinery against
  a private ABI" problem;
* reading `const FViewInfo&`. **`FViewInfo` is a Renderer-`Private` class** (`SceneRendering.h:1032`)
  of ~180 members with `#if WITH_EDITOR` and `#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)` blocks
  throughout, and it is where `TemporalJitterPixels` (`:1220`), `PreExposure` (`:1281`),
  `ViewRect` (`:1038`) and `PrevViewInfo` (`:1275`) live. **Reproducing that layout is not
  defensible**, and our View-constant-buffer read is by a wide margin the better source for those
  four — CLAUDE.md §2.6 is the strongest-evidenced part of the project, and row 135 validates
  itself from a single read. `docs/RESEARCH-UE4SS-MIGRATION.md` §6.1 reached the same conclusion
  from the reflection side.

**The intermediate the task asked about — "can `AddPasses` delegate to the default upscaler's
`AddPasses` and let us observe rather than replace?" — is exactly what L0 does**, and by patching
the default's own vtable slot it gets there without needing `GCustomStaticScreenPercentage`,
without needing `FSceneViewFamily`'s layout, and without authoring a single RDG pass. The only
things a real registration adds are the two `FSceneView`-constructor side effects (which Stray
does not need, §3.3) and the ability to declare a resolution-fraction range — and **that range is
never read in a Shipping build**: its only consumer is `FScreenPercentageHellDriver`, inside
`#if !UE_BUILD_SHIPPING` at `SceneRendering.cpp:444`. So L2's registration buys, for Stray,
precisely nothing that L0 does not already have.

**Verdict: do not build L2.** Not because it is impossible, but because its feasible half is
already built and its infeasible half is not on the path to anything the project needs.

---

## 5. The demotion map: what each rung retires

Nothing below is removed in this change. This is the plan the cross-check would license.

| Today | Why it exists | Retired by |
|---|---|---|
| `kTaaMainHash` + `taa_hashes.hpp`'s 27 cooked permutations + `stray-dlss-hashes.txt` | a permutation fingerprint standing in for identity | **L0.** Keep as a *confirmation*, demoted from a gate. A game update recooking shaders stops mattering |
| the depth+stencil-SRV-over-one-resource signature | the strongest runtime discriminator available to a descriptor-only observer | **L0** (as the gate) / **L1** (as identity) |
| dispatch-rect vs output-UAV vs view-rect arithmetic, and its DLAA/200% special cases | reconstructing `OutputViewRect` from what the GPU was asked to do | **L0** — the engine states the rect |
| the aspect-ratio band and 3.5x upscale ceiling (`kGateNotPrimaryView`) | excluding cubemap faces and planar reflections | **L0** — those never reach the interface |
| `g_ngx_pass_hash`, the 300-present staleness release, `kGatePinnedElsewhere` | "the matcher matches several passes, so pin to the first that evaluates"; and the pin then outliving its permutation | **L0** — with one announcement per view per frame there is nothing to pin |
| `kGateNoRoundTrip` / `owns_temporal_history` | the history round-trip as a proxy for "this pass owns the TAA history" | **L0** |
| `is_known_taa_hash` gating view creation (the loading-screen `vkCreateImageView` crash guard) | look-alikes over short-lived resources | **L1**, properly: liveness plus engine-stated identity. **Until L1, keep it** |

**The order matters and it is not negotiable:** the cross-check must come back from the box saying
`orphans` and `rect_mismatch` are zero in steady-state gameplay *before* any of the above is
demoted. Replacing a working mechanism with an unvalidated one is the failure this project's own
history warns about most loudly.

---

## 6. Does this subsume the separate menu / gameplay-gate problem?

**Yes, and that is worth stating plainly, because a previous attempt was building a gameplay gate
and a pin policy — more heuristics — and the user stopped it as the wrong direction.**

Today "are we in gameplay" is inferred from a depth histogram (§2.4) and a shader census whose
threshold has already been wrong once (400 → 300, CLAUDE.md §2.3.1). Through the seam it is not
inferred at all:

* A frame in which the engine runs **no primary temporal upscale** — a loading screen, most menu
  frames — produces **no announcement**, so every candidate dispatch is an `orphan` and can simply
  be refused. No histogram, no census, no threshold.
* The main menu *does* run the TAA pass (CLAUDE.md §5), and through the seam that is not a problem
  to detect but a fact to read: it announces, with its own rect, and its `R11G11B10_FLOAT` scene
  colour stops being something a format check has to be taught not to trip over.
* `GameViewportClient.cpp:1400`'s own guard is `ViewFamily.ViewMode == VMI_Lit`, so the engine has
  already excluded the debug view modes for us.

So the gate becomes a consequence of the identification rather than a second mechanism beside it.

---

## 7. Provenance ledger

| Claim | Status |
|---|---|
| `RENDERER_API`/`ENGINE_API` are empty in a monolithic target; nothing here is exported | **HARD**, `UEBuildModule.cs:545-554` |
| `ITemporalUpscaler`'s five virtuals and their declaration order | **HARD**, `TemporalAA.h:160-180` |
| `FDefaultTemporalUpscaler::GetDebugName` returns `TEXT("FDefaultTemporalUpscaler")` and nothing else | **HARD**, `TemporalAA.cpp:1527-1530` |
| The two fraction accessors return exactly `0.5f` and `2.0f` | **HARD**, `TemporalAA.cpp:1566-1573` + `SceneView.h:1438-1439` |
| `AddPasses` is called only from `PostProcessing.cpp:559` and `:2005` | **HARD** at both sites; **SOFT** that there is no third (code search) |
| Every look-alike calls `AddTemporalAAPass` directly | **HARD** per caller; **SOFT** that the list of six is complete |
| `FDefaultTemporalUpscaler` is the only `ITemporalUpscaler` in the tree | **SOFT** (code search over a large fork) |
| `*OutSceneColorViewRect == OutputViewRect`, and the dispatch is `ceil(that / 8)` for every `Main*` config | **HARD**, `TemporalAA.cpp:654-656`, `:958`, `:1456`, `:1516` |
| `FPassInputs`'s three pointers sit at 8 / 16 / 24 | **[derived]** — HARD declaration order, plus `sizeof(EPixelFormat) == 4` from `PixelFormat.h:12` and the MSVC x64 ABI |
| `FIntRect` is four `int32` at 0 / 4 / 8 / 12 | **[derived]**, from the declaration; universally relied on in UE code |
| MSVC slot order == declaration order; a virtual destructor takes one slot | **[derived]** from the MSVC ABI. This is what the 0.5/2.0 check tests |
| A `const` member function is ABI-identical to a free function taking `this` first, on x64 | **[derived]** from there being one x64 calling convention |
| `GCustomStaticScreenPercentage` is read at `GameViewportClient.cpp:1400`, before `CalcSceneView` | **HARD** |
| Registering an upscaler forces `AAM_TemporalAA` + `TemporalUpscale` | **HARD**, `SceneView.cpp:837-841`, `:937-941` |
| `GTemporalUpscaler` is written and never read | **HARD** (three files, one writer, no readers) |
| `GEngine->ViewExtensions` is not a `UPROPERTY` | **HARD**, `Engine.h:1768-1769` |
| Min/Max upsample fractions are never read in Shipping | **HARD**, `SceneRendering.cpp:444` guards their only caller |
| `FRHITexture::GetNativeResource()` is a virtual returning `ID3D12Resource*` under D3D12 | **HARD**, `RHIResources.h:997`, `D3D12Texture.h:311-331` |
| It is vtable slot 7 in a Shipping build | **[derived]** |
| `FRDGResource` is vptr / `Name` / `ResourceRHI` at 0 / 8 / 16 in Shipping | **[derived]**, from `RDG_ENABLE_DEBUG == 0` |
| `ResourceRHI` is null at `AddPasses` time for graph-allocated textures, non-null for externally registered ones | **HARD** for the assignment sites (`RenderGraphBuilder.cpp:516`, `:2084`, `:1222-1233`, `:1300-1312`); **SOFT** for which of Stray's three inputs is which |
| `FSceneViewFamily::TemporalUpscalerInterface` is the second of four trailing pointers | **HARD**, `SceneView.h:1845-1849` |
| Its byte offset is ~184 in Shipping | **[derived], and fragile.** Depends on `sizeof(FEngineShowFlags)`. **Do not hardcode it** |
| `FTAAPassParameters` is 80 bytes with `InputViewRect` at 12 | **[derived], and contested.** A naive walk inserts a spurious 4-byte pad before `InputViewRect` and gets 88 with it at 16. `FIntRect` aligns to 4, so 12/28/44/48/56/64/72 and size 80 is right — **and the fact that two careful derivations disagree is the argument for the interface route, which needs none of it** |
| The literal, the lea+ret `GetDebugName`, the five-slot vtable in declaration order, 0.5/2.0 by static decode, the `FPassInputs` and `FIntRect` reads, the thunk's ABI | **HARD on `Stray-Win64-Shipping.exe`, 2026-09-03** (§10, facts §36). Was UNCONFIRMED when this document was written |

---

## 8. What is implemented, and the one launch that settles it

**Code.**

* `src/core/engine_seam.{hpp,cpp}` — the pure scan, the layout constants, and the correlation
  ledger. In `stray_dlss_core`, so it builds and runs on the Linux unit lane.
* `tests/test_engine_seam.cpp` — 14 cases, 103 assertions: discovery, both float codegen shapes,
  every refusal, the movabs fallback, the decoy vtable, `Image::read` bounds, the offsets as pinned constants, and the
  ledger's claim/orphan/mismatch/retire/overflow behaviour.
* `src/engine_seam_hook.{hpp,cpp}` — the Windows half: PE section walk, discovery logging, the
  vtable patch, the thunk, `shutdown()`. Added to the mingw `-fsyntax-only` lane, which is a real
  compiler pass over Windows code on the fast CI job.
* `src/taa_hook.cpp` — forwards the present boundary and performs the cross-check on every
  positive match. **No behavioural change:** it counts and it warns.
* `src/app/dlss_app.cpp` — the `EngineSeam` key, the status-file line, and `seamhook::shutdown()`
  at device teardown, ahead of everything else, because the patched slot lives in the *game's*
  module and outlives both this device and this DLL (the address-0 crash of facts §14).

**Ship state as first written: `EngineSeam=0`.** Superseded by §10 — the default is now `3`.

**`docs/STRAY-RENDERING-FACTS.md` is deliberately NOT updated by this change**, and that is the
house rule applied rather than skipped. Its own scope rule is that every line was *"observed on
hardware, read out of the game's own shader bytecode, or read from the game's own files"*, and
that *"anything inferred, assumed, or believed-but-unconfirmed has been left out rather than
hedged."* Nothing in this document is any of those things: it is UE 4.27.2 source plus derivation,
with zero contact with Stray's binary. It belongs here until a run on the box produces a measured
line, at which point that line — the vtable address, the validated constants, the orphan rate —
goes into the facts document and this one gets its status column updated.

**The one launch.** `EngineSeam=1`, reach gameplay, paste the log. Two outcomes, both decisive:

```
ENGINE SEAM FOUND: ITemporalUpscaler vtable at 0x… (base=0x…, NN MB in N.N ms). Validated by
three independent constants from one scan: GetDebugName returns the literal at 0x…,
GetMin/MaxUpsampleResolutionFraction decode to 0.500000 and 2.000000 …
```

or a `NOT FOUND` naming the exact stage it reached and, for an unfamiliar codegen, the 16 bytes it
could not decode. **Then** `EngineSeam=2` for one walked session, and read `seam_orphans` and
`seam_rectMismatch` in `stray-dlss-status.txt`. Zero of both in steady-state gameplay licenses §5.

**Do not skip to level 2.** Level 1 cannot change a pixel; level 2 writes a pointer into the
engine's read-only data on the strength of a derivation about MSVC's ABI. One log line separates
the two risks, and this project's whole method is to spend a round trip on that separation rather
than on a guess (the `NgxPaint` / `NgxDumpInputs` ladder, CLAUDE.md §5).

---

## 9. Things not to do, collected

* **Do not read `FViewInfo`.** Renderer-private, ~180 conditional members, and the four quantities
  worth having are already read better from the View constant buffer.
* **Do not write `GTemporalUpscaler`.** Nothing reads it.
* **Do not hardcode `FSceneViewFamily::TemporalUpscalerInterface`'s offset.** Locate it as the
  second of the four trailing pointers.
* **Do not hook `AddTemporalAAPass` by code signature.** It has no string anchor, its
  `FTAAPassParameters` argument needs an 80-vs-88-byte derivation nobody has measured, and the
  interface gives strictly better information for strictly less risk. The one thing it would add
  is visibility into the look-alikes themselves — and those are precisely what we want to stop
  looking at.
* **Do not call a discovered function to validate it.** Decode it. Executing a wrongly identified
  address is the one failure mode with no diagnostic.
* **Do not let level 2 gate DLSS** until the cross-check has come back clean. An unvalidated
  identification that refuses everything is DLSS silently off, which is the same class of quiet
  failure as a wrong pass.


---

## 10. Measured on the box, 2026-09-03 — and the switch

Both levels ran. The lines are verbatim in `docs/STRAY-RENDERING-FACTS.md` §36; this section is
what they mean.

### 10.1 Level 1 found it, with one candidate at every stage

```
ENGINE SEAM FOUND: ITemporalUpscaler vtable at 0x6ffffae71730 (base=0x6ffff7140000, 82 MB in 178.3 ms).
Validated by three independent constants from one scan: GetDebugName returns the literal at
0x6ffffae71758, GetMin/MaxUpsampleResolutionFraction decode to 0.500000 and 2.000000 ...
ENGINE SEAM slots: dtor=... GetDebugName=... AddPasses=... GetMin=... GetMax=... (candidates: name=1 getDebugName=1 vtable=1)
```

Every row of §4.1's "what fails first" table is now answered, and every UNCONFIRMED in §7 about
the binary is HARD. `name=1 getDebugName=1 vtable=1` is the part worth noticing: the scan was
built to tolerate ambiguity and never had to.

### 10.2 Level 2 installed, forwarded, changed nothing — and caught two look-alikes

The engine reached the stand-in on **frame 0** with `output rect 3840x2160`, i.e. from the main
menu before any input. DLSS SR, frame generation and neural rendering ran exactly as before. And
the cross-check did what it was built for:

```
ENGINE SEAM DISAGREES about pass 0xe3ddca4be9830076: the matcher calls this dispatch (240x135 groups) the TAA pass ...
ENGINE SEAM DISAGREES about pass 0x42af595f8ff91038: the matcher calls this dispatch (120x68 groups) the TAA pass ...
```

**The structural signature accepted both.** Only the cooked-hash whitelist stopped DLSS from
running on them — one line of defence, and one whose table a game update invalidates. The
engine's answer excluded both, independently, by construction. Over 8570 announcements
`orphans=0`: the heuristic never once accepted a dispatch in a frame the engine had not run its
primary upscale.

### 10.3 `rectMismatch` at 45%: root cause, with the evidence first

The counters read `announced=8570 claimed=8496 orphans=0 rectMismatch=3821 unclaimed=73`.

**Hypothesis offered: frame generation presents twice per game frame, the ledger keys on the
present count, so announcements pair against the wrong frame.** Three independent observations
kill it:

1. **The mismatches are the two look-alikes.** The `DISAGREES` lines above name them with their
   group counts, 240x135 and 120x68 — half and quarter of the 480x270 primary. The ledger's
   `claim()` was called for *every* positive structural match (deliberately: that was the
   cross-check), and a look-alike asking while the frame's one announcement is pending is, in
   the counting rule that shipped, exactly one `rect_mismatch`. Two look-alikes per gameplay
   frame is the growth curve observed (12% in the menu-heavy first snapshot, 45% once the SSD
   passes were running every frame).
2. **The claim rate is 99.1%.** Pairing against the wrong frame would *lose* claims, not add
   mismatches while keeping claims at 99%.
3. **The present counter is per game present anyway.** `present_owner.cpp`'s `before_present`
   returns early under `in_own_code()`, and frame generation's own two presents are issued under
   `OwnCodeScope`; `pc.frame` never doubled.

So `rectMismatch` was a **naming error, not a correlation error**: it counted correct refusals.
It is now reported as `lookalikesRefused`, and is expected to grow. **`unclaimed` is the error
metric** — an announced primary upscale we never intercepted, i.e. a frame that ran the engine's
TAA — and it was 0.85%.

**The ledger was changed regardless**, because the coordinator's instinct was right about the
design even though the symptom had another cause: correlation must never depend on how presents
relate to frames. Announcements now retire when four *newer announcements* exist — the engine's
own `AddPasses` count is the clock — with the present counter only as an eight-present backstop
for a session that stops announcing altogether. `tests/test_engine_seam.cpp` pins both, plus the
exact session shape (one 4K announcement, two look-alikes asking first, then the real 480x270
dispatch, 100 frames: 100 claimed, 200 refused, 0 orphans, 0 unclaimed).

The 73 `unclaimed` are not yet explained (facts §36.5). The old two-present retire window is the
leading candidate and is gone; the periodic `[seam]` line now reports the number in the log, so
the next session answers it without the status file.

### 10.4 The switch: `EngineSeam=3`, the engine's announcement gates DLSS SR

*"Can we just switch to the new hook?"* — yes, and it is the default now.

| level | `seam::Mode` | what gates DLSS |
|---|---|---|
| 0 | `off` | the heuristic: cooked-hash table + structural signature + pin |
| 1 | `discover` | the heuristic; the seam is scanned, validated and logged, nothing installed |
| 2 | `observe` | the heuristic; the stand-in is installed and every announcement is cross-checked (`ENGINE SEAM DISAGREES`) |
| **3** | **`authoritative`** | **the announcement.** A structural candidate that claims the frame's announcement runs; one that does not is refused as `not-announced`, once per pass in the log. The cooked-hash table and the matcher's rect become **assertions** (`ENGINE SEAM ASSERTION`, once per pass), never gates. The pin, the round-trip proof and the aspect-ratio/upscale-factor shape gate are bypassed — an announced pass needs none of them |

`seam::decide()` is the whole rule, pure and tested: below `authoritative` → heuristic;
`authoritative` with the seam live → announced ? engine : refuse; `authoritative` with the seam
**not** live → `EngineSeamFallback=1` (default) runs the heuristic and says so at **ERROR** level
in the startup line, `=0` refuses every dispatch as `no-seam`. A silent downgrade is not one of
the outcomes.

**What the structural matcher still does:** it *extracts* the register roles (depth, stencil,
velocity, colour, output) that the resolve and the evaluate need. That is a binding walk, not a
judgement, and the announcement does not replace it. It no longer decides anything.

**What did not change:** FG and NR consume what the TAA hook publishes (`nrhook::note_guides`,
`ngxfg::publish`), and the hook publishes after a successful evaluate exactly as before.

### 10.5 Reading the verdict from the main menu

No launcher, no injected input. A Steam launch that sits in the main menu produces, in order:

1. `ENGINE SEAM MODE: authoritative ([STRAYDLSS] EngineSeam=3, EngineSeamFallback=1)` — the
   configuration, before the scan.
2. `ENGINE SEAM FOUND ...` and `ENGINE SEAM INSTALLED ... MODE=AUTHORITATIVE`.
3. `ENGINE SEAM MODE: authoritative is ACTIVE - the engine's ITemporalUpscaler::AddPasses
   announcement gates DLSS SR.` **This is the one line that says what is in charge.** Its
   failure forms are ERROR-level and name the fallback that applies.
4. `ENGINE SEAM: ITemporalUpscaler::AddPasses reached us on frame 0 ... Mode=authoritative: DLSS
   SR will run ONLY on the dispatch this announcement fits.`
5. `ENGINE SEAM AUTHORITATIVE: first announced pass claimed - 0x..., 480x270 groups, engine rect
   3840x2160, matcher rect 3840x2160, render 1920x1080, cooked-hash=yes.`
6. `DLSS feature created: ...` as before.
7. Every 600 presents: `[seam] frame N: seam=found mode=authoritative hooked=1 announced=...
   claimed=... unclaimed=... orphans=... lookalikesRefused=... (unclaimed must stay 0;
   lookalikesRefused is expected to grow)`.

A `DLSS did not run for pass 0x...: the engine's ITemporalUpscaler::AddPasses announced no primary
temporal upscale this dispatch fits` line for `0xe3ddca4be9830076` or `0x42af595f8ff91038` is the
expected, correct outcome for those two, and the only line each will ever produce.

### 10.6 What §5's demotion map looks like now

Nothing is deleted yet; everything in §5 is *bypassed* under level 3 and still runs under 0–2.
Deleting the pin, the round-trip proof, the shape gate and the hash whitelist's gating role is a
follow-up once a level-3 session has come back with `unclaimed=0` and DLSS on the announced pass.
The one row that stays regardless is the liveness check on the announced pass's own resources:
the announcement says *which* dispatch, not that its resources are alive under ReShade's stale
view map.


---

## 11. L1, and what level 3 exposed (2026-09-03)

`EngineSeam=3` ran on the box from the main menu. It is authoritative and correct — and it
showed that **identity was only half the problem**. Full lines in facts §36.6-36.8.

### 11.1 The measurement

`orphans=0` throughout, both look-alikes refused by the engine's answer rather than by the hash
list, the claimed pass exactly `0x901e041a7cadc9db` at 480x270 groups with the engine's rect and
the matcher's rect agreeing. And yet:

```
[seam] frame  600: announced= 603 claimed= 601 unclaimed= 0  ...
[seam] frame 3600: announced=3603 claimed=3553 unclaimed=47 ...

[frame 1200] NR STAGE: ... guides-stale=6   NR RESETS: total=8  from: frame-gap=7
[frame 3600] NR STAGE: ... guides-stale=50  NR RESETS: total=47 from: frame-gap=45
```

**`unclaimed` ~ `guides-stale` ~ NR `frame-gap` at every checkpoint, accelerating.** The engine
announced its primary upscale, we did not intercept it, SR skipped the frame, the TAA hook
published no guides, NR declined and its next evaluate carried a `DLSSNR.Reset` — a whole-screen
discontinuity. That is the flip the user reports, and it is now a counter rather than a story.

### 11.2 The cause: the heuristic still gated the INPUTS

Level 3 replaced the heuristic as the authority on *which dispatch*. It did not touch the two
things that decide whether that dispatch is usable:

* **the register walk** that assigns depth / velocity / colour / output roles, and
* **`is_resource_live`**, i.e. ReShade's `view->resource` map, as the liveness authority.

The only per-pass refusal logged for the real pass all session was *"its depth or velocity SRV is
missing or not known live"* — printed **once, by design**, which is exactly why the rate stayed
invisible. The menu is the worst case for it: scene colour is `R11G11B10` there and the animated
CRT/video surfaces churn resources every frame.

### 11.3 Fix one: every unclaimed frame explains itself

The `[seam]` line now carries continuous per-reason counters, split by how far the frame got:

```
| notClaimed: noDispatch=N | claimedButNoSR: viewUnreadable=N deadInputs=N roleUnresolved=N
  mvFailed=N createFailed=N evalFailed=N | evaluated=N | l1: resolved=N partial=N fellBack=N
```

`noDispatch` is the ledger's `unclaimed` — the matcher rejected the real dispatch, so it never
reached `claim()`. The `claimedButNoSR` group is a claimed engine dispatch failing downstream.
**Their sum is the number of frames that published no guides**, which is what NR reports as
`guides-stale`. The once-per-pass WARN stays: it answers "which gate", the counters answer "how
often", and conflating them is what cost this round trip.

### 11.4 Fix two: L1 — the engine's own textures

`AddPasses` hands us `FPassInputs{SceneColorTexture, SceneDepthTexture, SceneVelocityTexture}`.
Those are now resolved **at claim time** — during graph execution, which is where a
graph-allocated texture finally has an RHI resource (§4.2's obstacle 2, solved by moving the read
rather than by guessing):

```
FRDGTexture* --(+16)--> FRHITexture* --(vptr, slot 7)--> GetNativeResource() --> ID3D12Resource*
```

When they resolve they **replace** the register-role guesses and the liveness verdict for that
frame. The justification is not convenience: *a resource the engine is about to bind is alive by
construction*, and no `view->resource` map can be more authoritative than the engine naming it.
The heuristic's answer is kept as an **assertion** — one WARN per session when the two differ,
naming both pointers — and as a **counted fallback**.

**Both offsets are [derived] and UNCONFIRMED on this executable**, so the live path is guarded in
depth and the pure part is fully testable:

* `seam::resolve_rhi_fn` walks the chain through caller-supplied `read_u64` / `is_code`
  callbacks and returns a **status**, never dereferencing anything itself. All seven outcomes are
  exercised against a synthetic object graph in `tests/test_engine_seam.cpp` — including
  `rhi_null`, which is the AddPasses-time case the design exists to avoid.
* Live, a wrong offset must pass **five** filters to do harm: alignment and canonical range on
  every pointer; the vtable inside the game's own module; the function pointer inside an
  executable section; and finally the returned `ID3D12Resource*` must be one **our own resource
  registry already knows is live**. Anything else falls back and increments `l1: fellBack`.

`[STRAYDLSS] EngineSeamInputs` (default 1) turns L1 off without leaving level 3, so the two
changes can be separated on the box.

### 11.5 The test, and it needs no input

**Target: `unclaimed=0` in the main menu**, with `guides-stale` and NR `frame-gap` following it
to 0. A Steam launch that sits in the menu is the whole test — the seam fires on frame 0 and the
menu runs the TAA pass. Read, in order:

1. `ENGINE SEAM L1: first resolve of the engine's own FPassInputs ...` — the three pointers with
   their chain status and whether the registry recognised each. **This line is what confirms the
   two [derived] offsets.** Its absence, or `registered=0`, means L1 never engaged and the
   `l1: fellBack=` counter will say so.
2. `[seam] frame N: ... unclaimed=0 ... | claimedButNoSR: all zero | l1: resolved=N`.
3. `NR STAGE: ... guides-stale=0` and `NR RESETS: ... frame-gap=0`.

If `unclaimed` is still non-zero, the breakdown now says which of the six gates to fix, which is
the thing this round trip bought whatever else it settles.

---

## 12. L1 CRASHED THE GAME, and the reason is the difference between IDENTITY and POINTERS (2026-09-03)

**Measured on the box.** Branch `engine-seam-l1` tip `3365f02`, plugin DLL md5 `bc1fa257…`
(CI run 33790794760), `EngineSeam=3`, `EngineSeamInputs=1`, FG on, NR on, launched into the main
menu with **no input**. The game died at `SecondsSinceStart 24`, about frame 600:

```
Unhandled Exception: EXCEPTION_ACCESS_VIOLATION reading address 0x0000021c000003c0
```

### 12.1 The stack, symbolized — and it dissolves both apparent tensions

The crash context carried seven `main` offsets and no names. They were resolved against the PDB
from that exact CI artifact (`llvm-symbolizer --obj=main.dll --relative-address`, the PDB
renamed to the basename recorded in the DLL's debug directory), innermost first:

| frame | symbol | source |
|---|---|---|
| `VCRUNTIME140+0x562` | `memcpy` | — |
| `main+0x771f6` | `seamhook::l1_read_u64` | `src/engine_seam_hook.cpp:434` |
| `main+0xad4e7` | `seam::resolve_rhi_fn` | `src/core/engine_seam.cpp` |
| `main+0x772ba` | `seamhook::resolve_one` | `src/engine_seam_hook.cpp:455` |
| `main+0x75520` | `seamhook::resolve_inputs` | `src/engine_seam_hook.cpp:659` |
| `main+0x6be7f` | `taa_hook::intercept_dispatch` | `src/taa_hook.cpp:1337` |
| `main+0x7abf` | `app::DlssApp::on_dispatch` | `src/app/dlss_app.cpp:1001` |
| `main+0x7bddb` | `native::hooks::hk_List_Dispatch` | `src/backend_native/d3d12_hooks.cpp:779` |

Line 434 is `l1_read_u64`'s `std::memcpy`. So:

* **"a `memcpy` in VCRUNTIME, but L1 does pointer-chain walks" was a false tension.** L1's chain
  walk *is* `std::memcpy`, one qword at a time, and MSVC emitted a CRT call rather than an inline
  load. The two descriptions are the same code.
* **"the fault is on the GameThread, and L1 runs on the render thread" was also false.** The
  bottom frame is the game calling `ID3D12GraphicsCommandList::Dispatch` through our own vtable
  hook, so L1 ran on whichever thread recorded that dispatch. The crash context's `ThreadName`
  describes that thread; it does not place our code anywhere we did not already know it was.
* **"the address is handle-shaped, like the descriptor-handle faults in §1" was a coincidence of
  shape, and the shape is still the clue.** `0x0000021c000003c0` is two int32s — `0x3c0` = **960**
  and `0x21c` = **540** — i.e. an `FIntPoint(960, 540)` read as one pointer. 960x540 is exactly a
  half-resolution buffer at this session's 1920x1080 render extent. It is not a descriptor handle;
  it is a texture extent, sitting where L1 expected `FRDGResource::ResourceRHI`.

### 12.2 The bisect

Same DLL, same everything, one ini key:

| `EngineSeamInputs` | outcome |
|---|---|
| `0` | **no crash**, frame 16200, ~16 minutes. `l1: resolved=0 partial=0 fellBack=0` (the switch really was off), `unclaimed=159` of 16203 announcements — all `noDispatch` |
| `1` | **crash at ~frame 600**, after `ENGINE SEAM L1: first resolve …` reported colour, depth AND velocity all `(ok, registered=1)`, and `announced=603 claimed=603 unclaimed=0` |

Two things fall out of this and both matter.

**The offsets are not simply wrong.** The first resolve returned three distinct pointers that our
own resource registry already knew were live `ID3D12Resource`s. Three independent hits by chance is
not a thing that happens, so `FRDGResource::ResourceRHI @16` and `GetNativeResource` at vtable slot
7 are **right when the object is right**. Changing the constants would not have fixed this.

**L1 must not be abandoned.** `unclaimed=159` returns the moment L1 is switched off, and
`unclaimed` is the user's "DLSS flip" (§11). The feature was doing its job.

### 12.3 The root cause: the ledger claims IDENTITY with slack, and L1 read POINTERS through it

`Ledger::claim` returns the **first unconsumed announcement whose rect matches**, and
`retire_stale` keeps an announcement alive for `kRetireAfterAnnouncements` (4) newer announcements
or `kRetireAfterFrames` (8) presents. That slack is deliberate and correct **for correlation**: a
dispatch that arrives late still names the right pass, and `unclaimed` stays honest rather than
counting a timing wobble as a missed frame.

It is fatal for **pointers**. `FRDGTexture` is allocated from the frame's `FRDGAllocator`, which
`FRDGBuilder` resets when `Execute` finishes. The instant a newer graph exists, the pointer
`FPassInputs` gave us addresses **recycled arena memory** — where `+16` is no longer `ResourceRHI`
but whatever the next allocation put there. An `FIntPoint(960, 540)`, for instance.

The sequence, and note that every step is invisible in the counters:

1. Frame N: `AddPasses` announces. The real dispatch does **not** reach `claim()` — either the
   structural matcher rejected it (**which is the exact failure L1 exists to work around**) or it
   never arrived. The announcement stays pending; nothing is counted, because `unclaimed` only
   increments at *retire*.
2. Frame N+1: `AddPasses` announces again, same rect. Two slots pending.
3. Frame N+1's dispatch calls `claim()`, which scans in order and returns the **older** slot.
   Rect-identical, so `claimed` increments and `unclaimed` stays 0. Correlation is still right.
4. L1 dereferences that slot's `FRDGTexture*`. Frame N's builder is gone.

From then on the ledger runs one graph behind until a retire breaks the chain — and every claim
in between hands L1 a dead pointer. **The `unclaimed=159` measured with L1 off is the frequency of
step 1: roughly 1 announcement per 100 frames misses its own dispatch.** At ~600 frames that is
around six opportunities, which is exactly where the crash landed.

Most dead reads are harmless: the qword is null, or unaligned, or outside the canonical range, so
`plausible_heap_ptr` refuses and L1 falls back. The crash needs one qword that *looks* like a
pointer. `(960, 540)` is 8-aligned, above `0x10000` and canonical, so it passed every guard L1
had — and the next read faulted.

### 12.4 The second defect: `plausible_heap_ptr` is a RANGE test, not a readability test

L1's own comment admitted there was "no portable way to make the read itself fault-proof" and
argued the defence was in depth — short chain, vtable inside the module, function in executable
memory, answer recognised by our registry. **Every one of those checks is on a RESULT, and the
first bad read faults before any of them can run.** Shipping a dereference of engine memory whose
layout *and* lifetime are both `[derived]`, behind a numeric range check, is prime directive 2
inverted: it converts a fallback into a crash.

### 12.5 The fix

**1. Freshness, `seam::announcement_is_fresh` (pure, `tests/test_engine_seam.cpp`).** L1 may
dereference an announcement's `FPassInputs` only when it is the **newest** announcement (`sequence
== Ledger::sequence()`) and the **frame has not turned over** (`announce_frame == current_frame`).
Anything else is declined and counted as `l1: stale=`.

> **This first shipped with a THIRD condition — same thread — and that was wrong. See §12.8**,
> which is the correction and the measurement that forced it. `Announcement` still carries the
> announcing OS thread id, but it is **reported, never tested**.

**`claim()` itself is untouched.** Correlation keeps its slack, so `announced`, `claimed`,
`unclaimed`, `orphans` and `lookalikesRefused` are byte-identical to `3365f02` and the
`unclaimed=0` L1 bought is preserved. A stale claim is still a claim; it just supplies no engine
inputs, and that frame uses the heuristic exactly as `EngineSeamInputs=0` would. A test pins that
the ledger still hands back the older announcement in the step-1-to-4 sequence above, and that the
freshness verdict on it is false.

**2. The read cannot kill the process, and this is deliberate.**
`VirtualQuery` first — the page must be `MEM_COMMIT`, readable, not `PAGE_GUARD`/`PAGE_NOACCESS`,
and hold all 8 bytes — which is the check `plausible_heap_ptr` never was. Then **SEH**
(`__try`/`__except`) around the `memcpy` *and* around the `GetNativeResource` call, MSVC-only
(`#if defined(_MSC_VER)`; the mingw fast lane has no `__try` and keeps the `VirtualQuery` guard).
Both guarded functions are leaves holding no object that needs unwinding, which is what MSVC
requires of `__try`.

**This is an explicit, named exception to "never swallow a fault".** These two sites dereference
and *call through* offsets that are `[derived]` from engine source and cannot be proven from
outside the engine. The alternative to catching the fault is not correctness, it is a dead game
with a stack trace that has to be symbolized by hand — which is what this section is.

**3. A fault latches L1 off for the session, loudly.** The first fault logs at ERROR with the
faulting address and both `[derived]` constants by name, and `l1_gate` returns `faulted` for the
rest of the run. A guard that fires every frame is a diagnosis; a guard that fires every frame and
keeps going is a crash report waiting to be re-filed.

**4. `l1_gate` is pure, so `EngineSeamInputs=0` is TESTED rather than asserted.** It checks the
off-switch first and unconditionally, so no fault latch, freshness verdict or seam state can route
around it, and an exhaustive sweep over the other five inputs pins that.

### 12.6 What to read on the next launch

The `[seam]` line's `l1:` group now reads
`resolved= partial= fellBack= stale= faults= off=`.

* **`stale=` is the number this whole section is about.** Zero means the ledger never slipped a
  graph behind. Non-zero and climbing means it does, routinely — L1 is safe but declining, and the
  next step is to stop `claim()` serving a previous frame's announcement at all rather than to
  refuse the pointer afterwards. It is accompanied by one WARN, once per session, printing the
  announcement's sequence against the ledger's newest, and both frames and both threads. **That is
  the single line that would have identified this crash without a symbolization round trip.**
* **`faults=` must be 0, and `off=0`.** Non-zero means one of `kRdgResourceRhiOffset` /
  `kRhiGetNativeResourceSlot` is wrong on this executable; the ERROR line carries the address.
* `ENGINE SEAM L1: first resolve …` now also prints the three raw `FRDGTexture*` and the
  announcement's sequence, frame and thread, so a fault line has its inputs beside it.

### 12.7 What is still open

* **Whether the stale claim is the *only* way L1 saw a dead pointer.** The mechanism is derived
  from `Ledger`'s own retire rules and fits every measurement (crash only with `EngineSeamInputs=1`,
  not on the first resolve, hundreds of frames in, a value shaped like a half-res extent, and an
  `unclaimed` rate with L1 off that supplies the trigger at the right frequency) — but it was not
  observed directly, because nothing was counting. `stale=` counts it now, and one launch settles
  it: `stale=0` with no crash would mean the mechanism is something else and the SEH guard is what
  is holding the line.
* **Whether the dispatch-recording thread is ever a different thread from the announcing one.**
  The freshness gate would catch it and count it as `stale`, and the WARN prints both thread ids,
  so the same launch answers this too.

### 12.8 The fix worked and the guard was still wrong: THREAD IDENTITY IS NOT LIFETIME (2026-09-03)

**Measured on the box.** DLL md5 `83628ea2…` (`13250b0`), menu, no input, same config.

**The crash is gone.** `faults=0 off=0`, no crash, 4200+ frames — so the `VirtualQuery` + SEH
guards and the fault latch did their job, and this section exists as a data point rather than as
another symbolization exercise. That part stands unchanged.

**And L1 was completely inert.** Its own WARN named the reason, which is the one redeeming thing
about the mistake:

```
ENGINE SEAM L1: declining to dereference a STALE announcement - seq 1 against the ledger's
newest 1, announced on frame 0 / thread 1400, claimed on frame 0 / thread 1152.
```

* `seq 1` against newest `1` — it **is** the newest.
* `frame 0` against `frame 0` — the frame had **not** turned over.
* thread **1400** announcing against **1152** claiming — the third condition, and the only one
  that failed. It failed on **every single claim**:

```
[seam] frame 4200: announced=4203 claimed=4147 unclaimed=53 orphans=0 lookalikesRefused=1747
  | l1: resolved=0 partial=0 fellBack=0 stale=4147 faults=0 off=0
[frame 3600] NR STAGE: … guides-absent=121 guides-stale=26   NR RESETS: total=25 from: frame-gap=24
```

`stale=4147` is every claim; `resolved=0` is not one resolve in the session. The build was
behaving exactly as `EngineSeamInputs=0` while every other counter reported L1 switched on, and
`unclaimed=53` / `guides-stale=26` / `frame-gap=24` moved together again — the blips were back.

**Why the condition was wrong, and it is structural rather than marginal.** UE 4.27 calls
`ITemporalUpscaler::AddPasses` during RDG graph **setup**; the D3D12 `Dispatch` our hook sees is
recorded during graph **execution**. Those are not required to be the same thread, and on this
build they are not: the pair was stable for the whole session, so this is the engine's design and
not a race.

**The reasoning error, which is the part worth keeping.** The argument for the condition was
"`FRDGBuilder` is a stack object, so only the announcing thread may dereference what it holds".
The premise is true and the conclusion does not follow. **Thread identity governs OWNERSHIP, not
VALIDITY.** A stack object being alive is a statement about whether its frame has returned, not
about which thread may read it; memory held by a live stack frame is readable from any thread, and
the engine itself reads `ResourceRHI` on the recording thread in order to bind the texture. The
lifetime argument in §12.3 never needed the thread at all: a **newer announcement** means a newer
graph, and **a present between announce and claim** means the announcing graph has completed.
Those two are the lifetime. Each is independently sufficient against the crash sequence, which is
now pinned as its own test so that dropping the third condition cannot have left the guard resting
on one leg.

**The wider lesson, and this project has now paid for it twice in one day.** A guard that is
"obviously safe" and costs nothing to add is not free: this one cost the entire feature, silently,
and would have read as "L1 doesn't help" rather than as "L1 never ran" if the decline had not been
counted and named. **A defensive condition needs the same provenance discipline as a functional
one** — HARD, SOFT or UNCONFIRMED — because a wrong one does not fail loudly, it just never fires
the thing it guards. The same-thread test was UNCONFIRMED, presented as reasoning, and shipped as
a gate.

**What changed.**

* `announcement_is_fresh` tests **newest + same frame only**. The thread ids stay in `Freshness`
  and in the stale WARN, which now also **names which condition failed** in words rather than
  printing six numbers and leaving the reader to spot the differing pair — that is what cost this
  round trip to read.
* The announce/claim **thread pair is latched and reported**: one INFO on the first claim saying
  what the pair is and that differing threads are normal and untested, and one WARN if the pair
  ever changes mid-session. The identity stays visible as a measurement without being a gate.
* The regression test for the ordinary frame now uses the box's **measured pair (1400 announcing,
  1152 recording)** rather than a tidy same-thread one, so a re-introduced thread test fails in CI
  instead of on the box.

**Still UNCONFIRMED, and worth naming rather than assuming:** *what* thread 1152 is. The NGX cubin
lines in the same session carry `tid:1152` and the earlier crash context's `ThreadName` read
`GameThread`, which do not obviously agree with each other or with "the RHI thread". Nothing
depends on the answer now, which is the point — but the latch above will say if the pair ever
moves.

**Also open:** whether newest + same frame is sufficient in practice, which only the box can
answer. The residual hole is a second graph inside one present interval whose dispatch claims an
announcement the first graph's dispatch never consumed. It needs an unclaimed announcement AND a
rect-matching look-alike in the same frame, and if it happens the `VirtualQuery` + SEH guards make
it a counted fault rather than a crash — which is exactly why those stay whatever else changes.

### 12.9 THE SAFE POINT IS `AddPasses`, NOT THE CLAIM — and this is why (2026-09-03)

`e421e14` on the box: **no crash, `faults=0 off=0`, 4200+ frames** — the guards hold, and this
session is a measurement rather than a dump. And L1 was inert after startup:

```
[seam] frame 1800: … | l1: resolved=164 partial=0 fellBack=550 stale=1080 faults=0 off=0
[seam] frame 2400: … | l1: resolved=164 partial=0 fellBack=550 stale=1676 faults=0 off=0
[seam] frame 3000: … | l1: resolved=164 partial=0 fellBack=550 stale=2270 faults=0 off=0
```

`resolved` and `fellBack` **frozen**; `stale` growing ~596 per 600 frames, i.e. every frame. The
WARN said exactly why, and both lifetime conditions failed together, by one, every time:

```
seq 715 vs newest 716, frame 711 vs 712; threads 1408 -> 1160
```

The 164 early resolves are the shallow-pipeline window during load.

#### The root cause, and it retires §12.3's

Two facts from UE 4.27.2 (`AlexMercer-MA/UnrealEngine-4.27` @ `306a7e9`) settle it:

1. **`FRDGBuilder::Execute()` frees every `FRDGTexture` before it returns.** Its tail runs the
   extraction loops and then `Clear()`, which contains `Allocator.ReleaseAll();`. `Execute()`
   calls no `ImmediateFlush`, no `FlushRHIThread`, no `WaitForRHIThread`. `RenderGraphBuilder.h`
   states outright: *"The builder should be created on the stack and executed prior to
   destruction"*, and holds `FRDGAllocator Allocator;` by value.
2. **The dispatch we intercept has not happened yet at that point.** `RHICommandList.h` opens
   *"RHI Command List definitions for queueing up & executing later"*, and every entry point is
   `if (Bypass()) { GetContext().RHI…; return; } ALLOC_COMMAND(…)`. With the RHI thread on, a
   pass lambda's `DispatchComputeShader` **allocates a command**; the D3D12 call happens when the
   RHI thread drains it.

So the order is: **pass lambda enqueues → `Allocator.ReleaseAll()` frees the `FRDGTexture` →
later, another thread makes the D3D12 call our hook sees.** Resolving at claim time reads freed
memory **by construction**, and the measured thread pair (1408 render, 1160 RHI) with a one-frame
lag is that mechanism's signature.

**§12.3 named the danger correctly and put the safe point in the wrong place.** Ledger slack was
real but secondary; the race is structural, and no gate on the claim side could have fixed it —
which is what the two inert builds were empirically demonstrating.

#### One thing the ledger was doing RIGHT all along

With the RHI thread exactly one frame behind, dispatch(N−1) arrives while `{N−1, N}` are pending
and `claim()` returns the OLDEST rect match — **N−1, the announcement that dispatch actually
belongs to**. The correlation was never wrong; only the pointer was dead. `claim()` stays
untouched on the merits, not out of caution.

#### The three candidate fixes, assessed against the source

**Resolve at announce — YES, and it is the fix.** `GetSceneTextureParameters` builds both guides
externally:

```cpp
Parameters.SceneDepthTexture      = GraphBuilder.RegisterExternalTexture(SceneContext.SceneDepthZ, ERenderTargetTexture::ShaderResource);
Parameters.GBufferVelocityTexture = TryRegisterExternalTexture(GraphBuilder, SceneContext.SceneVelocity);
```

and `RegisterExternalTexture` calls `Texture->SetRHI(...)` immediately. `PostProcessing.cpp` passes
them straight through (`UpscalerPassInputs.SceneDepthTexture = SceneDepth.Texture;`,
`.SceneVelocityTexture = Velocity.Texture;`) before
`UpscalerToUse->AddPasses(GraphBuilder, View, UpscalerPassInputs, …)`. **So depth and velocity have
a non-null `ResourceRHI` inside `AddPasses`, on the render thread, inside the builder's own
setup — provably alive**, and `TD3D12Texture2D::GetNativeResource` is `return Resource->GetResource();`,
a pure getter with no locks or side effects. Colour is the post-chain `SceneColor.Texture` and is
expected to be graph-allocated, hence `rhi_null` — **expected, not a failure**, and enough, because
depth and velocity are what `deadInputs` and the render-extent logic hang on while colour is
already resolved by register. **Assert none of it: try all three and let the `l1:` counters say.**

**A later render-thread point inside `Execute()` — possible, strictly worse.** An RDG pass lambda
runs after `SetRHI` and before `ReleaseAll`, so all three would be non-null there. But it needs
the RDG parameter-struct machinery (§4.3's L2) and still ends by handing a plain
`ID3D12Resource*` across to the RHI thread — the same hand-off the announce-time resolve gets for
a few lines. Only worth building if colour specifically turns out to matter.

**A generation/serial — impossible, and worth recording why.** The memory is *freed*, so any
serial stored in the object lives in freed memory and reading it to validate is the very read we
are trying to make safe. `FRDGResource` has no generation counter. A serial in our own ledger only
tells us the announcement is stale, which we already do, and the steady state is "always stale".

#### What shipped

* **The chain walk moved into `add_passes_thunk`.** The `Announcement` now carries three resolved
  `ID3D12Resource*` and their `RhiChain` statuses; the `FRDGTexture*` are kept for the
  first-resolve line and are **never dereferenced anywhere else**.
* **`resolve_inputs` no longer dereferences anything.** It checks each resolved pointer against
  our own resource registry — liveness at claim is the only check that still means anything once
  the `FRDGTexture` is gone — and hands them on.
* **`announcement_is_fresh` is demoted to a pipeline-depth diagnostic.** `L1Gate` loses `stale`
  and `L1GateInputs` loses `fresh`; `stale=` keeps counting, and its once-per-session line is now
  INFO explaining that the lag is normal, not a WARN explaining a refusal.
* **The guards moved with the read** — `VirtualQuery` + SEH around the `memcpy` and the
  `GetNativeResource` call, now on the render thread — and the fault latch fires there, before the
  next `AddPasses`.
* **`l1_gate` is asked at BOTH ends**, so `EngineSeamInputs=0` still means zero dereferences of
  engine memory anywhere, and the exhaustive off-switch test covers the new shape.
* **`claim()` is byte-identical**, so `announced` / `claimed` / `unclaimed` / `orphans` /
  `lookalikesRefused` stay comparable across all five builds.

#### What to read

`l1: resolved=` should now track `claimed` instead of freezing, with `partial=` counting frames
where depth+velocity resolved and colour did not (**the expected steady state**), `fellBack=`
small, `faults=0 off=0`. `stale=` will still grow every frame and that is now *information*, not
a fault — it is the RHI thread's lag. `unclaimed` must stay 0.

**Still UNCONFIRMED:** whether colour ever resolves, and whether the resolved depth/velocity
survive the registry's liveness check a frame later. Both are answered by the same `l1:` line.

---

## 13. The heuristic G-buffer finder is DELETED, and Ray Reconstruction now refuses loudly (2026-09-03)

**~3 970 lines removed, 106 added.** Whole files: `src/gbuffer_finder.{cpp,hpp}`,
`src/gbuffer_resolve.{cpp,hpp}`, `src/core/gbuffer_classify.{cpp,hpp}`,
`src/core/envbrdf.{cpp,hpp}`, `tests/test_gbuffer_classify.cpp`, `tests/test_envbrdf.cpp`,
`shaders/gbuffer_resolve.hlsl`. In place: the RR half of `taa_hook.{cpp,hpp}`, its wiring in
`dlss_app.cpp`, one perf bucket, the CMake entries and four ini keys.

### 13.1 Why, in one sentence

**The finder identified GBufferA-E by descriptor SHAPE, which is the same class of guessing this
whole document exists to retire.** It scored render-target sets by format, extent and draw
pattern — exactly the reasoning the engine seam replaced for the TAA pass, where the answer had
been wrong often enough to cost sessions. Keeping a second heuristic identifier alive, unused,
next to a working engine-sourced one is how a project ends up debugging the wrong oracle again.

Two supporting facts made it a clean cut rather than a judgement call:

* **Nothing on the SR, NR or FG path referenced any of it.** `gbr::` appears in no SR, NR or FG
  code; the finder's only consumers were `try_evaluate_rr` and its own event taps.
* **RR had not run in any measured session.** `NgxRR` ships 0, and the guide path had never
  produced a confirmed image.

### 13.2 What was KEPT, byte-identical, and why that is the point

**`src/ngx_backend.{hpp,cpp}` needed zero changes.** `ensure_feature_rr`, `evaluate_rr`,
`release_feature_rr`, `RRStatus`, the `SuperSamplingDenoising.*` availability query, the
row-major matrix plumbing and the `nvsdk_ngx_helpers_dlssd.h` include all survive untouched,
because **`EvaluateInputsRR` takes raw `ID3D12Resource*` and never named the finder in code.**

That separation is the whole reason this deletion is cheap: what was deleted is a *guide source*,
not Ray Reconstruction. **What RR needs to come back is guides, and the intended source is the
engine's own named RDG G-buffer textures, reachable from the `const FViewInfo&` that
`ITemporalUpscaler::AddPasses` already hands us** — identity from the engine, exactly as L1 now
does for depth and velocity (§12.9). That is a much shorter path than the finder ever was, and it
is written down here so nobody resurrects the heuristic instead.

### 13.3 `NgxRR` refuses loudly — it does not silently no-op

`dlss_app.cpp` reads the key and, for any non-zero value, logs at **ERROR** naming the reason, the
fact that the NGX side is intact and waiting for guides, and that DLSS SR is unaffected this
session. Prime directive 2: a feature that cannot work must say so, not quietly do nothing.

`perf::kNgxRr` survives but **its only timing scope lived inside `try_evaluate_rr`**, so the
bucket now reads **always zero**. Both the enum comment and the `[perf]` line say so — a
permanently-zero bucket with no explanation is a thing for someone to chase in six months.
`perf::kGBufferResolve` is gone entirely.

### 13.4 Two real bugs fixed while in the file

Both were reported in `docs/RESEARCH-RESHADE-SHAPE-SWEEP.md` and both check out:

1. **DLSS was created for the matcher's rect, not the engine's.** `fd.output_width/height` took
   `m.output_width`, which is `group count x 8` clamped to the UAV — i.e. **rounded up to a
   multiple of 8**. At 3840x2160 that is 480 groups exactly and the two agree, which is why it
   never showed; at any output rect not divisible by 8 the feature would be created a few pixels
   too large, silently. When the engine announced the dispatch, its own `OutputViewRect` is now
   used. The `rect_agrees` assertion already compared the two once per pass — this applies that
   assertion's conclusion.
2. **The colour path was still gated on the cooked-hash table.** `trust_registers` required
   `hash_and_structural` before it would believe §2.3's register map (t1 colour, t5 history) —
   but under `EngineSeam=3` the hash is demoted to an assertion everywhere else, and **being
   called through `ITemporalUpscaler::AddPasses` is a STRONGER warrant for the register map than
   a hash match**: it proves the dispatch is the primary temporal upscale, which is what the
   cooked table only ever approximated and which it misses outright after a game update recooks
   the shaders. An engine-announced pass with an unlisted hash was silently falling back to the
   weaker "which buffer looks like colour" heuristics. It now trusts the registers on either
   warrant.

This one matters more than it looks: **L1 resolves colour as `rhi_null`** (it is the
graph-allocated post-chain texture, §12.9), so the register map *is* the colour source on the
live path.

---

## 14. Can we read SceneColorTexture the way NVIDIA does? Assessed: NO, and the current shape is the answer (2026-09-03)

### 14.1 First, a correction that improves how our offsets should be described

`RenderGraphResources.h` (UE 4.27):

```cpp
FRHIResource* GetRHI() const  { ValidateRHIAccess(); return ResourceRHI; }
FRHIResource* GetRHIUnchecked() const { return ResourceRHI; }
```

`GetRHI()` is **an inline accessor over exactly the field we read**, and in Shipping
(`RDG_ENABLE_DEBUG == 0`) `ValidateRHIAccess()` is a no-op, so it compiles to a single load of
`ResourceRHI`. There is no out-of-line symbol: the only `RENDERCORE_API` function in the whole
resource class family is `FRDGTexture::GetPassthrough`.

**So `kRdgResourceRhiOffset` should stop being described as a layout guess.** For depth and
velocity we are doing *precisely* what NVIDIA's lambda does — same field, same value, same moment
in the frame — and the offset is only how we spell an accessor the compiler has already inlined
away. `kRhiGetNativeResourceSlot` is the same shape one level down: a virtual we cannot name, so
we index its vtable. Both are confirmed on this exe and both are validated at runtime by our own
registry recognising the returned pointer (facts §36.13).

Also settled, and it removes a tempting anchor: **`MarkResourceAsUsed()` is
`inline void MarkResourceAsUsed() {}` in Shipping** — NVIDIA's call to it compiles to nothing, so
it is not a hookable point.

### 14.2 The question, and why the answer is no

NVIDIA reads all four inputs — colour included — inside a lambda **they authored** as an RDG pass:

```cpp
GraphBuilder.AddPass(..., [PassParameters, ...](FRHICommandListImmediate& RHICmdList) {
    DLSSArguments.InputColor = PassParameters->SceneColorInput->GetRHI();
});
```

That lambda runs during graph *execution*, after RDG has assigned the transient texture's memory
and before `Execute()` reaches `Allocator.ReleaseAll()` — the window §12.9 identifies. **Authoring
such a pass is exactly the thing an injected DLL cannot do**: `AddPass` is a template over a
parameter struct with shader-parameter metadata, instantiated at engine compile time, with no ABI
to call from outside.

So the question reduces to: is there some *other* hookable point in that window? Every candidate
fails, and they fail for different reasons, which is what makes this a "no" rather than a "not
yet":

| Candidate | Why not |
|---|---|
| `TRDGLambdaPass<>::Execute` (the virtual that runs each pass) | a **template instantiation per lambda type** — one vtable per pass, not one anchor. There is nothing to scan for that identifies the TAA pass's instantiation specifically |
| `FRDGResource::GetRHI` / `MarkResourceAsUsed` | **inline**, no symbol, nothing to hook |
| `FRDGBuilder::ExecutePass` | private, non-template, plausibly signature-scannable — but **no self-validating constant**. The seam is safe because a candidate must reproduce a name literal *and* 0.5 *and* 2.0 before anything is installed (§3). Nothing here offers that, so a wrong answer could not be refused, and §9's rule forbids installing on a guess |
| `FRDGBuilder::Execute` entry / exit | outside the window at both ends: at entry transient textures have no RHI, at exit the allocator is gone |
| `RHICmdList.SetGlobalUniformBuffers({})`, after the pass loop | inline, called from many places — **and by then the TAA pass has already run**, so its transient colour may already be back in the pool |

### 14.3 The one thing that WOULD work, and why we must not do it

`r.RHICmdBypass=1` makes `FRHICommandList` execute inline instead of queueing, so the pass
lambda's `Dispatch` would reach D3D12 **on the render thread, inside `Execute()`, with the
allocator alive** — and our existing claim-time L1 code would have worked unmodified.

This also retroactively explains the **164 early resolves** of §36.11: during load the pipeline is
shallow, so claim and announce briefly coincide.

**It is still the wrong trade.** Bypass disables the RHI thread process-wide — the engine's main
rendering-throughput mechanism — to buy one texture we already have by another route. Recording it
here so the idea is not rediscovered as clever.

### 14.4 Why colour-by-register is a good end state, not a compromise

* **Colour was never the ambiguous input.** L1's own cross-check has flagged a disagreement
  exactly once, and it was **velocity** (§36.13.1) — the heuristic naming a resource the engine
  never bound. Nothing has ever suggested colour was misidentified.
* **Its warrant improved anyway.** `trust_registers` used to require a cooked-hash match before
  believing §2.3's register map; it now also accepts the engine's announcement (§13.4), which is
  a *stronger* warrant — being called through `ITemporalUpscaler::AddPasses` proves the dispatch
  is the primary temporal upscale, which the hash table only ever approximated and which it
  misses outright after a game update.
* **The register map is the engine's own binding**, read from the descriptor the game actually
  set, not an inference about what a colour buffer looks like.

### 14.5 The useful half of the reference

NVIDIA implements **velocity combine and the G-buffer resolve as RDG passes of their own** — the
same two pieces of work we built by hand as D3D12 compute. That is independent validation of the
architecture, and it says where the deleted finder's replacement belongs: **not a descriptor-shape
search, but the engine's own named G-buffer textures**, taken from the `const FViewInfo&` that
`AddPasses` already hands us (§13.2). If RR is ever rewired, that is the shape to copy.
