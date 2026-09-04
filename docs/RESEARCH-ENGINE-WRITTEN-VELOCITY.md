# Can Stray's motion vectors be ENGINE-WRITTEN instead of reconstructed? The ceiling, and why it is low

Research spike, 2026-09-04, branch `nr-dlaa-engine-mvecs`. The brief was the user's own question,
and its precision is what makes it answerable: **not** "are the motion vectors dense" — they are,
`mv_resolve.hlsl` fills every pixel — but *"can they stop being reconstructed"*.

**Nothing was built and nothing has run on the box.** This document is a verdict, the source that
forces it, two corrections to CLAUDE.md, and one launch that would settle the residual doubt.

Provenance labels follow CLAUDE.md §0.5. UE 4.27.2 citations were read this session from the
mirror `AlexMercer-MA/UnrealEngine-4.27` @ `306a7e9` — the ref CLAUDE.md already cites — and are
labelled **HARD** where the line was fetched and quoted, **HARD-via-our-own-doc** where it is
carried second-hand through `docs/RESEARCH-RR-GBUFFER.md`.

---

## 0. Verdict, up front

**Not worth doing. `r.BasePassForceOutputsVelocity=1` cannot make Stray's motion-vector field
engine-written, and the reason is a compile-time exclusion the cvar structurally cannot reach.**

The argument in one line: the cvar bypasses two *per-primitive uniform* tests that live **inside**
`#if WRITES_VELOCITY_TO_GBUFFER`, and for this title's statically-lit world that macro is **0**,
so the permutation contains neither the tests, the branch, nor an `SV_Target4` to write to.

**Worth MEASURING first, though — one launch, no rebuild, no new code**, because the cvar is live
and `[STRAYDLSS] MvStats=1` already counts exactly the right thing. §5 is the experiment and its
pre-registered prediction. It is cheap enough that running it is a better use of a launch than
arguing further, and its failure mode is informative: a coverage jump toward 90% would refute this
whole document.

**And the residual is smaller than even that suggests.** `bOutputVelocity` — one of the two gates
the cvar bypasses — tests whether the scene holds a *previous transform entry* for the primitive,
not whether it moved; and the base pass's own per-frame read refreshes that entry's
`LastFrameUsed`, so it is never trimmed. **A primitive that has moved even once passes that gate
forever.** What is left for the cvar is Static-mobility non-lightmapped props and movables that
have never once moved — i.e. **exactly the pixels for which the camera reconstruction is already
correct.** The prize is concentrated where it is worth least. §3.

**And a separate finding, which is the more valuable half of the spike.** The value of engine
velocity is far smaller than "96% of the field is reconstructed" suggests, because **94% of what
the engine already writes is static geometry for which the reconstruction is exactly correct**
(facts §37). The population that gets a *wrong* vector today is bounded above by ~0.1% of the
frame, and the reflection artefact — the thing that actually looks wrong — is not in it and is not
fixable from the velocity side at all (CLAUDE.md §5, four blocking reasons, two of which are
arithmetic).

---

## 1. What the measurement actually says, restated so it is not over-read

`[STRAYDLSS] MvStats=1`, 4500 gameplay frames, 9.33e9 px (CLAUDE.md §2.5, facts §37) — **HARD**:

```
engine-written 3.91%   reconstructed 96.09%
of engine-written: genuinely moving 2.152%  still 94.04%  UNORM-clamped 0.0129%
of reconstructed: w<=0 rejected (emitted ZERO) 0.0000%   non-finite 0
```

Read it as three populations rather than two, because the middle one is where the value is:

| population | share of frame | is its vector correct today? |
|---|---|---|
| engine-written, genuinely moving | `3.91% x 2.152%` = **0.084%** | yes — the engine wrote it |
| engine-written, still | `3.91% x 94.04%` = **3.68%** | yes, and the reconstruction would also have been right |
| reconstructed | **96.09%** | right for static geometry; **wrong for any mover that wrote nothing** |

**The prize is not 96%.** It is the unknown subset of that 96% which is a *moving* surface handed
the motion of a static world point at its depth. Nothing measures that subset directly, and its
upper bound is not the 96% — it is bounded by how much of the frame moves relative to the world at
all, which the engine-written half puts at **~0.084%** of pixels for the movers the engine *does*
draw. **SOFT**: that is an estimate by analogy, not a measurement of the missing set, and the
whole point of the missing set is that we cannot see it.

**Two consequences, both stated in CLAUDE.md §2.5 already and both worth repeating here:** the
reconstruction carries essentially the whole field, so its own correctness (the `ClipToPrevClip`
transposition class of bug) dominates everything on this page; and the `w<=0` zero-emission
hypothesis measured **0.0000%** and is closed — do not build that fix.

---

## 2. The three exclusions, and only one of them is per-primitive

`WRITES_VELOCITY_TO_GBUFFER` is the whole gate, and it is a *preprocessor* macro
(`Engine/Shaders/Private/BasePassCommon.ush:53-54`, **HARD**, verbatim):

```c
#define SUPPORTS_WRITING_VELOCITY_TO_BASE_PASS ((FEATURE_LEVEL >= FEATURE_LEVEL_SM4) && (MATERIALBLENDING_SOLID || MATERIALBLENDING_MASKED) && !SIMPLE_FORWARD_SHADING)
#define WRITES_VELOCITY_TO_GBUFFER ((SUPPORTS_WRITING_VELOCITY_TO_BASE_PASS || USES_GBUFFER) && GBUFFER_HAS_VELOCITY && (!SELECTIVE_BASEPASS_OUTPUTS || !(STATICLIGHTING_TEXTUREMASK || STATICLIGHTING_SIGNEDDISTANCEFIELD || HQ_TEXTURE_LIGHTMAP || LQ_TEXTURE_LIGHTMAP || WATER_MESH_FACTORY)))
```

and `USES_GBUFFER` (`:41`) independently requires `MATERIALBLENDING_SOLID || MATERIALBLENDING_MASKED`
— so **both** operands of the `||` carry the blend-mode term and there is no path through the
macro for a translucent permutation. **HARD.**

The three exclusions, in the order of how much of Stray's frame each removes:

### 2.1 The lightmap term — the biggest, and the one this project had wrong

`r.SelectiveBasePassOutputs` **defaults to 0** (`BasePassRendering.cpp:23`, HARD) and **Stray
ships it `True`** — `docs/game-config/Hk_project_Config_DefaultEngine.ini:85`, inside
`[/Script/Engine.RendererSettings]`, the game's own cooked ini. **HARD.** Alongside
`r.AllowStaticLighting=True` (`:38`), `r.VirtualTexturedLightmaps=True` (`:81`) and a world whose
lighting is baked (CLAUDE.md §5).

With `SELECTIVE_BASEPASS_OUTPUTS == 1`, the macro additionally requires **no lightmap define**,
and those defines are emitted by the vertex factory's lightmap policy
(`LightMapRendering.h:110-118, 152-157`; `LightMapRendering.cpp:15-19`, **HARD**). So for every
HQ-lightmapped permutation the macro is **0**, and:

* `PIXELSHADEROUTPUT_MRT4` is literally `WRITES_VELOCITY_TO_GBUFFER`
  (`BasePassPixelShader.usf:1563`) and `PixelShaderOutputCommon.ush:84-86` declares
  `out float4 OutTarget4 : SV_Target4` only under it — **the permutation has no velocity output at
  all**, not a zeroed one. **HARD.**

And those same primitives are Static mobility, so they are refused by the *separate* velocity pass
too: `FOpaqueVelocityMeshProcessor::PrimitiveCanHaveVelocity` opens
`if (!PrimitiveSceneProxy->IsMovable()) return false;` (`VelocityRendering.cpp:424-427`, **HARD**).
**They write velocity nowhere, and no cvar changes it** — `r.SelectiveBasePassOutputs` is
`ECVF_ReadOnly | ECVF_RenderThreadSafe` and carries the engine's own comment *"Changing this
causes a full shader recompile"* (`BasePassRendering.cpp:21-27`, **HARD**), so in a cooked game
the permutations that exist are the permutations that exist.

> **This is the second time a shipped `[/Script/Engine.RendererSettings]` line has beaten a
> "stock default is N" argument in this project**, and the first time was documented as a rule
> three sections from the paragraph that then broke it (CLAUDE.md §5, `r.SSR.Temporal`). CLAUDE.md
> is corrected in the same commit as this file, in both places. The habit the rule asks for is
> one grep of `docs/game-config/` before the argument, not after it.

### 2.2 The blend mode

Translucent, additive and modulate materials are excluded at preprocessor level, from both
operands. **HARD**, above. This is the exclusion CLAUDE.md already documented correctly.

### 2.3 The two per-primitive tests — the ONLY thing the cvar reaches

`r.BasePassForceOutputsVelocity`, `SceneRendering.cpp:330-335`, **HARD**, verbatim:

```cpp
static TAutoConsoleVariable<int32> CVarBasePassForceOutputsVelocity(
	TEXT("r.BasePassForceOutputsVelocity"), 0,
	TEXT("Force the base pass to compute motion vector, regardless of FPrimitiveUniformShaderParameters.")
	TEXT("0: Disabled (default)")
	TEXT("1: Enabled"),
	ECVF_RenderThreadSafe);
```

CLAUDE.md's claim is **verified**: default 0, `ECVF_RenderThreadSafe` **only**, so it is
live-changeable. One assignment (`SceneRendering.cpp:1524` → `View.ForceDrawAllVelocities`,
`SceneView.h:667`) and **four total references repo-wide** — the declaration, that assignment, and
the two shader reads. It appears in no C++ branch. **HARD**, by exhaustive search.

It bypasses exactly:

* `OutputVelocity > 0` — `BasePassVertexShader.usf:225`, `BasePassPixelShader.usf:985`; fed by
  `bOutputVelocity || AlwaysHasVelocity()` (`PrimitiveSceneProxy.cpp:385`), where `bOutputVelocity`
  is *"the scene has a previous transform recorded for this component"* (`RendererScene.cpp:2631`);
* `DrawsVelocity != 0` — `BasePassPixelShader.usf:997`, which is `return IsMovable();`
  (`PrimitiveSceneProxy.h:572-575`).

Both sites are **inside** `#if WRITES_VELOCITY_TO_GBUFFER` (VS block `:221`-`:250`, PS block
`:983`-`:1009`). **HARD.** That containment is the whole verdict.

---

### 2.4 The separate velocity pass does not rescue the static world, and nothing forces it to

Stray's `r.SelectiveBasePassOutputs=True` makes the separate opaque velocity pass run
(`DeferredShadingRenderer.cpp:2078-2083`, quoted verbatim and verified: *"If
bBasePassCanOutputVelocity is set, basepass fully writes the velocity buffer unless
bUseSelectiveBasePassOutputs is enabled"*). It is the obvious place to look for the missing
static world. It is not there, for two independent and individually sufficient reasons, both
**HARD**:

* **`FOpaqueVelocityMeshProcessor::PrimitiveCanHaveVelocity` opens
  `if (!PrimitiveSceneProxy->IsMovable()) return false;`** (`VelocityRendering.cpp:424-427`).
  Lightmapped world geometry is Static mobility. It is refused before anything else is asked.
* **The per-FRAME test refuses anything that did not move.**
  `FOpaqueVelocityMeshProcessor::PrimitiveHasVelocityForFrame` (`VelocityRendering.cpp:445-464`),
  verbatim in the load-bearing part:

  ```cpp
  if (!PrimitiveSceneProxy->AlwaysHasVelocity())
  {
      // Check if the primitive has moved.
      ...
      Scene->VelocityData.GetComponentPreviousLocalToWorld(PrimitiveSceneInfo->PrimitiveComponentId, PreviousLocalToWorld);
      if (LocalToWorld.Equals(PreviousLocalToWorld, 0.0001f))
      {
          // Hasn't moved (treat as background by not rendering any special velocities)
          return false;
      }
  }
  ```

**And there is no "force everything" switch for this pass.** The parameter that looks like one is
not: `RenderVelocities(..., EVelocityPass::Opaque, bHairEnable)` passes **`bHairEnable`** into
`bForceVelocity` (`DeferredShadingRenderer.cpp:1894`, `:2083`; the translucent call passes a
literal `false` at `:2451`), and at `VelocityRendering.cpp:275-301` `bForceVelocity` only
guarantees the texture **exists and is cleared** when there are zero draws — `if (!bHasAnyDraw)
{ continue; }` still runs at `:298-301`. **It adds no draw.** Exhaustively: `VelocityRendering.cpp`
declares four cvars (`r.BasePassOutputsVelocity`, `r.ParallelVelocity`,
`r.VertexDeformationOutputsVelocity`, `r.RHICmdFlushRenderThreadTasksVelocityPass`), and
`bForceVelocity` appears in exactly two files repo-wide. **HARD.**

So the separate pass is *a supplement for lightmapped movers*, never a route to the static world.

---

## 3. What the cvar would actually buy — and it is smaller than it first looks

**Only primitives whose permutation ALREADY has `WRITES_VELOCITY_TO_GBUFFER == 1`** — opaque or
masked **and** non-lightmapped — **that are failing one of the two per-primitive tests.** The
entire statically-lit world is untouched (§2.1, §2.4).

**And that residual set is smaller still, because `bOutputVelocity` is effectively permanent once
earned.** This is the sharpest fact in the whole spike and it was not obvious. `OutputVelocity` is
`bOutputVelocity || AlwaysHasVelocity()` (`PrimitiveSceneProxy.cpp:385`), and `bOutputVelocity` is
`VelocityData.GetComponentPreviousLocalToWorld(...)` (`RendererScene.cpp:2631`) — which is true
**if an ENTRY EXISTS**, not if the primitive moved this frame. The entry is then immortal in
practice: the lookup itself does `VelocityData->LastFrameUsed = InternalFrameIndex;` on every
successful read (`ScenePrivate.h:2198-2211`), trimming only removes entries older than 10 frames
(`RendererScene.cpp:2620`), and the base pass performs that read every frame for every such
primitive. **So a primitive that has moved even once keeps passing `OutputVelocity` forever.**
**HARD.**

Which leaves the cvar's real beneficiaries as just two narrow classes:

* **Static-mobility, non-lightmapped** primitives — they fail `DrawsVelocity() == IsMovable()`;
* **movable primitives that have never once moved** since entering the scene.

**HARD** on the mechanism, **SOFT** on the size — but both classes are, by construction, things
that do not move, which is precisely the population for which the camera reconstruction is
already exactly right. **The cvar's prize is concentrated in the pixels that need it least.**

### 3.0 Where the measured "still 94%" comes from, and why it corroborates all of this

Facts §37 measures that 94.04% of engine-written pixels are *still* — their vector matches what
the camera reconstruction would have produced. The source says exactly which pass wrote them, and
the two passes point in opposite directions, so the attribution is not ambiguous:

* the **separate** pass explicitly refuses a still primitive (`PrimitiveHasVelocityForFrame`, §2.4);
* the **base** pass writes it every frame regardless, because `bOutputVelocity` is "an entry
  exists" and the entry never expires (above). With a still camera the value is ~0, and per §2.5's
  encode `V*0.2495 + 0.49999` a zero encodes to `0.49999`, which **passes** the strict
  `EncodedVelocity.x > 0` validity test and reads as engine-written.

**HARD** on the mechanism and the arithmetic; **SOFT** only in apportioning Stray's primitives
between them, which nobody has counted. The 94% is therefore *movable, non-lightmapped primitives
standing still, written by the base pass* — which is both a coherent picture of a Stray street
and an independent confirmation that the lightmapped world contributes nothing at all to the
engine-written 3.91%.

### 3.1 Translucency has a route, and it is unreachable

`UMaterial::IsTranslucencyWritingVelocity()` (`Material.cpp:5723-5726`, **HARD**) is
`bOutputTranslucentVelocity && IsTranslucentBlendMode(GetBlendMode())`, and it is real: it feeds
`FVelocityVS::ShouldCompilePermutation` (`VelocityRendering.cpp:94`) and
`FTranslucentVelocityMeshProcessor::TryAddMeshBatch` (`:574`), i.e. the **separate** velocity pass,
not `WRITES_VELOCITY_TO_GBUFFER`. **HARD.**

**But `bOutputTranslucentVelocity` is a per-material-asset `UPROPERTY`** — `Material.h:885-887`,
*"When true, translucent materials will output motion vectors and write to depth buffer in
velocity pass"*, an editor checkbox serialised into the cooked asset. **There is no cvar.**
Reaching it means editing and recooking every affected material. **HARD, and it closes the
question**: this is not per-material at runtime, it is per-asset at cook time.

Worth keeping for a different reason: the separate pass writes into the **same** texture our hook
reads (`VelocityRendering.cpp:251-259, 313, 346-351` — when the base pass already produced
velocity the pass loads and adds into it). So if those flags *were* set, we would see the result
for free. They are not, and we cannot set them.

### 3.2 What it cannot fix, and this is not negotiable

**Screen-space reflections.** A reflective pixel is `(1-w)·diffuse + w·reflection` with **one**
motion-vector slot, so no single vector is correct for it — that is arithmetic, not a UE 4.27
limitation, and no engine input touches it. Nor does a rough reflection have a single reflected
point. Both are **HARD** and both survive any improvement in coverage. CLAUDE.md §5 carries the
full four-reason argument and NVIDIA's own concession (a *separate*
`pInMotionVectorsReflections` texture, not a corrected single field). **Do not propose an
engine-velocity fix that implies otherwise.** The honest treatment is
`pInBiasCurrentColorMask` — marking the pixels rather than claiming to know where they went —
and facts §53 already measured that this runtime **ignores** the mask at `R8_UNORM`.

---

## 4. Cost, and whether it can be scoped

**No extra render target, and that is worth knowing because it is the intuition most people
have.** The velocity target's existence is decided by `r.BasePassOutputsVelocity` alone, at
pass-setup time, with no reference to the force cvar:
`SceneRenderTargets.cpp:1022-1025` → `FVelocityRendering::BasePassCanOutputVelocity`
(`VelocityRendering.cpp:371-374`) → `IsUsingBasePassVelocity` (`RenderUtils.h:552-556`) →
`GBasePassVelocityPlatformMask` (`RenderUtils.cpp:1200-1204`). The RT is bound for the whole base
pass whenever allocated (`SceneRenderTargets.cpp:751-756, 950-953`). **HARD.**

So the cost is per newly-un-gated primitive only:

* **VS** — a second `GetMaterialVertexParameters`, `VertexFactoryGetPreviousWorldPosition`,
  `GetMaterialPreviousWorldPositionOffset` and a `mul` by `PrevTranslatedWorldToClip`
  (`BasePassVertexShader.usf:227-233`). For skinned and gFur factories that is **a second
  skinning evaluation against the previous-frame bone matrices** — with `LAYERS=48` shells on the
  cat, not cheap. **HARD** on the work; **UNCONFIRMED** on the frame cost, which nobody has
  measured.
* **PS** — `Calculate3DVelocity` + `EncodeVelocityToTexture` and one more MRT write
  (`:989-1006`). The `VelocityPrevScreenPosition` interpolant already exists in the permutation,
  so there is no new interpolator. **HARD.**
* For a `WRITES_VELOCITY_TO_GBUFFER == 0` permutation the cost is **exactly zero** — and so is
  the benefit. The code is not compiled.

**Scoping: there is no per-primitive opt-in this project can reach.** `bAlwaysHasVelocity` is a
render-thread C++ proxy bitfield, not a `UPROPERTY`, so UE4SS reflection cannot see it
(`PrimitiveSceneProxy.h:936, 556`; `PrimitiveSceneProxy.cpp:138`). **HARD.** `Mobility` *is* a
`UPROPERTY` and is reachable — but flipping a lightmapped Static primitive to Movable invalidates
its baked lighting **and still leaves it in a permutation with no velocity code**, so it buys
nothing. `r.VertexDeformationOutputsVelocity` (`VelocityRendering.cpp:41-45`, no ECVF flags, so
runtime-changeable) only affects primitives that are already movable and already in a
velocity-writing permutation. **HARD.**

---

## 5. The one experiment, with its prediction pre-registered

It costs one launch, no rebuild and no new code: the cvar is `ECVF_RenderThreadSafe`, and
`[STRAYDLSS] MvStats=1` already counts exactly the right thing (CLAUDE.md §5, `src/core/mv_census.hpp`).

**Two arms, one session each, same scene, same route** — use `tools/stray-bench.sh`'s recorded
scenario so the content is identical, and read the **`MV CENSUS TOTAL`** line at frame 3600+ (a
menu or a loading screen has no movers and would answer the wrong question — the §2.4 menu/load
trap, which has now caught this project four times):

| arm | `Engine.ini [SystemSettings]` | read |
|---|---|---|
| A (control) | `MvStats=1`, nothing else changed | `engine-written`, `genuinely moving`, `non-finite` |
| B | `+ r.BasePassForceOutputsVelocity=1` | the same three |

`r.BasePassForceOutputsVelocity` is not `ECVF_ReadOnly`, so arm B can also be reached live from
the debug menu inside one session, which makes it an A/B rather than two sessions — **preferable**,
since CLAUDE.md's own alternating-harness lesson is that between-session comparisons of a moving
scene are not comparable.

**Pre-registered prediction, from §2:** `engine-written` moves from 3.91% to somewhere in the high
single digits, and `genuinely moving` as a share of the frame barely changes. **`non-finite` must
stay 0**; if it does not, nothing else on the line is worth reading.

**What each outcome decides:**

* **Coverage barely moves (predicted).** The compile-time lightmap exclusion is confirmed
  dominant, the reconstruction is permanent for this title, and this line of work is closed. Spend
  the effort on the reconstruction's own correctness instead — it carries 96% of the field.
* **Coverage jumps toward 90%+.** This document is **wrong**: the exclusion is per-primitive after
  all, the lightmap reading is mistaken, and forcing the cvar is a real and cheap win. Then the
  next question is the frame cost of §4's second skinning evaluation, measured with
  `tools/stray-bench.sh` against the ~140 fps no-host baseline.
* **Coverage rises but `genuinely moving` does not.** The worst outcome to misread: more
  *static* geometry became engine-written, which the reconstruction was already getting right, at
  the cost of §4's extra VS work. That is a pure loss and the cvar should be left off.

**Do not run this until someone has read this section.** A coverage number with no `genuinely
moving` beside it is the metric this project has been fooled by before.

---

## 6. What was not done, and why

* **Nothing was built.** The verdict is negative and the experiment needs no code.
* **No fix was proposed for the reflection artefact**, deliberately. §3.2 is why, and the
  refusal is the finding.
* **Stray's primitives were not counted.** §3.0 explains the measured 94% by mechanism and the
  mechanism is HARD, but the apportionment between primitive classes is **SOFT** and would need
  either the dispatch census or a UE4SS component walk. It cannot change the verdict — every
  candidate class is a thing that does not move.
* **`docs/RESEARCH-RR-GBUFFER.md` §1.4's unresolved anomaly is untouched** — the live game shows a
  6-RTV base-pass bind with no velocity-format target where stock source predicts 7-with-velocity,
  and its leading hypothesis is a licensee delta disabling base-pass velocity outright. If that
  hypothesis is true the ceiling here is **lower still**, not higher, so it does not change the
  verdict; but it is the one thing that could make even arm B's small delta vanish. The
  discriminator is already recorded there and needs the same census run.
