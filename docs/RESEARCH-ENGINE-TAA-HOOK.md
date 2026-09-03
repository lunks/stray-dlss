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
| **L0 ORACLE** | Stand in for `FDefaultTemporalUpscaler::AddPasses` via its vtable; read the engine's own output rect and input textures; cross-check the heuristic matcher against it | **Yes, and cheaply.** One string anchor, one qword patch, three exact self-checks, no code signature, no engine headers | **IMPLEMENTED** in this change, default OFF, two levels (`EngineSeam=1` scan-only, `=2` stand-in + cross-check) |
| **L1 IDENTIFY** | Map the `FRDGTextureRef`s L0 hands us to `ID3D12Resource*` so interception targets the engine's resources by identity | **Probably, and the last hop is trivial** — `FRHITexture::GetNativeResource()` is a virtual returning `ID3D12Resource*`. Two derived offsets stand in the way, and the RDG resource is null at `AddPasses` time for graph-allocated textures | **Not built.** Needs L0's log first: the same log line that proves L0 also says whether the RHI pointers are non-null there |
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
| `Stray-Win64-Shipping.exe` | **NOT CONSULTED.** The box was unreachable from this session (`ssh` to it: *Network is unreachable*), and the task forbids launching the game. **Every claim about the shipped binary's byte-level shape is UNCONFIRMED**, and the implementation is built so that a wrong guess refuses instead of hooking |

**That last row is the single most important caveat in this document.** It is why L0 ships with a
level 1 that installs nothing, why discovery is *static* (it decodes candidate functions rather
than calling them), and why the default is 0.

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
| 1 | The literal is not in the image, or `GetDebugName` is not lea+ret | `ENGINE SEAM: NOT FOUND` with the stage it reached. Nothing installed. **This is the expected first outcome to rule out** |
| 2 | The vtable slot order is not declaration order | slots 3/4 do not decode to 0.5/2.0 → `fraction_mismatch`, refused |
| 3 | MSVC emitted a fourth shape for `return 0.5f` | `fraction_shape`, refused, with 16 bytes of hex logged |
| 4 | `sizeof(EPixelFormat) != 4` | the three `FPassInputs` pointers are read 4 bytes off. They are only compared, so the visible effect is that they look like garbage in the first log line — and the rect, which is the part we correlate on, is unaffected |
| 5 | `FIntRect` is not four `int32` | the announced rect is nonsense; `rect_mismatch` climbs to 100% and `unreadableRect` counts the degenerate ones |
| 6 | The thunk's ABI is wrong | a crash on the first frame with TAA. Bounded by level 2 being opt-in |
| 7 | `r.TemporalAA.HistoryScreenPercentage > 100` | the config becomes `MainSuperSampling` and `TemporalAA.cpp:1466-1474` overrides `OutputViewRect` *after* `SecondaryViewRect` was captured, so the announced rect stops matching the dispatch. CLAUDE.md §4 already requires this to stay 100; now there is a counter that would say so |

Row 4 deserves its label: `EPixelFormat` is an unscoped `enum` with no fixed underlying type whose
enumerators run 0..~80 (`Core/Public/PixelFormat.h:12`), so MSVC gives it `int`. **HARD from the
declaration**, but it is a derivation about a compiler, not a measurement.

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
| Anything about `Stray-Win64-Shipping.exe`'s actual bytes | **UNCONFIRMED.** The box was unreachable this session |

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

**Ship state: `EngineSeam=0`.** The working SR path is untouched and the heuristic matcher is still
the mechanism.

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
