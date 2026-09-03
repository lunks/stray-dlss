# stray-dlss — agent operating manual

A **UE4SS C++ plugin** (`mods/StrayDLSS`) that injects **NVIDIA DLSS** — Super Resolution,
Frame Generation and Neural Rendering — into **Stray** (Unreal Engine 4.27.2, **D3D12**). SR
replaces the engine's own temporal AA compute dispatch with an NGX evaluation; FG and NR run at
present time. It loads inside the game process and installs its own D3D12 hooks
(`src/backend_native/`); it needs no ReShade.

> **CORRECTED 2026-09-03.** This line read "A ReShade **add-on**" until today, long after the
> plugin became how the project runs. The ReShade add-on still exists as a second host
> (`src/backend_reshade/`, built as `stray-dlss.addon64`) and is still built and tested, but it
> is not the shipping configuration and the two must never be run together — that is two drivers
> of one TAA pass. **Do not read "Config A/B" as "plugin/add-on":** both are *plugin*
> configurations, and they differ only in whether ReShade is also loaded as `dxgi.dll`
> (`mods/StrayDLSS/README.md`). ReShade being present under the plugin is supported, so "the
> plugin host" never implies "no ReShade in the process".
>
> The stale self-definition was load-bearing: two research documents inherited ReShade-era
> FEASIBILITY judgements from it — "we cannot reach X", "that would need the engine" — which were
> true of an out-of-process add-on and are not true of an in-process plugin with UObject
> reflection. When reading anything below that reasons from "we are outside the engine", check it
> against this. Sections 3 and 5 still describe the add-on's event model in places.

Read this file completely before touching anything. It is the contract for how this project is
built, what is known versus assumed, and what will silently produce a wrong image.

Depth lives in two companion documents, both of which are load-bearing:

* **`docs/STRAY-RENDERING-FACTS.md`** — what was measured against the running game. Verbatim.
* **`docs/RESEARCH.md`** — verified external research: the ReShade 6.8 API, the NGX D3D12 SDK, UE
  4.27 internals, the Proton/vkd3d chain, and CI. 228 claims, each adversarially verified. **When
  this file and `docs/RESEARCH.md` disagree, RESEARCH.md wins** — it carries the citations.

---

## 0. Prime directives

1. **You cannot test this yourself.** No game, no Windows box, no GPU. Every build happens in
   GitHub Actions with MSVC. The only feedback loop is the human running the artifact on their
   Linux/Stray machine and pasting back a log. A round-trip is expensive, so each one must carry
   the maximum diagnostic payload.
2. **Prefer a loud failure to a quiet wrong image.** A crash gets a stack trace. A wrong
   motion-vector sign gets "it looks smeary" three days later and costs a week. Every convention
   must be asserted and logged, never assumed. NVIDIA's guide is explicit that wrong resource usage
   flags can produce *"the output may be black without further indication"* — that class of failure
   is the enemy.
3. **Never claim something works because it compiled.** The only evidence DLSS is engaged is a log
   line from the user's machine, a screenshot, or the DLSS on-screen indicator. Follow
   `superpowers:verification-before-completion`.
4. **Everything provable in CI must be tested in CI.** DXBC hashing, matrix math, jitter
   conversion, velocity decode, quality-mode selection — all pure functions. Keep the untestable
   NGX surface as thin as possible.
5. **HARD / SOFT / UNCONFIRMED.** Label the provenance of every technical claim you write down.
   "Read it in the header" is HARD. A forum post is SOFT. Everything else is UNCONFIRMED and must
   be verified before code depends on it.

---

## 1. Target environment

The human runs Stray on Linux. Proton is the target, not an afterthought.

| | |
|---|---|
| GPU | NVIDIA RTX 4090 |
| Driver | 610.43.02, open kernel modules |
| Host | Linux 6.17.13, SteamOS guest |
| Proton | `GE-Proton-dxvk301-ds5-clean-nowl` — the `dxvk301` names **DXVK** 3.0.1, not vkd3d |
| vkd3d-proton | **3.1.0** (measured 2026-08-31 from the prefix's own `d3d12core.dll`) |
| DXVK-NVAPI | **v0.9.2** (measured from the prefix's `nvapi64.dll`; `PROTON_NVIDIA_LIBS=1` selects the `nvidia-libs` build, so the one under `files/lib/wine/nvapi/` is **not** the one loaded) |
| D3D12 layer | **vkd3d-proton** |
| Compositor | gamescope, DRM backend, `--hdr-enabled --hdr-itm-enabled` |
| ReShade | **6.8.0.2155**, add-on support enabled |
| Game | Stray, Steam AppID **1332010** |

### Feasibility: resolved

**D3D12 NGX DLSS works under vkd3d-proton.** It is an NVIDIA-authored path present since
vkd3d-proton 2.5, routed through `NvAPI_D3D12_*` cubin entry points in DXVK-NVAPI into
`ID3D12DeviceExt` and `VK_NVX_binary_import`. `nvngx_dlss.dll` never touches native handles — it
makes ordinary D3D12 descriptors — so `ID3D12Resource` being a `VkImage` underneath is a non-issue.

**Consequences, and they are absolute:**

* **Pass plain `ID3D12Resource*` to NGX and nothing else.** Never touch `ID3D12DXVKInteropDevice`,
  `GetVulkanResourceInfo`, or any vkd3d interop API. Never write a D3D12→Vulkan bridge. Never use
  the Vulkan NGX path.
* **Initialise NGX with `device::get_native()`**, which is the *original* vkd3d `ID3D12Device`, not
  ReShade's proxy. This bypasses ReShade's descriptor remapping, which is exactly what we want —
  ReShade's `convert_to_original_cpu_descriptor_handle` has no release-build validation and would
  corrupt any handle it did not mint. Never fetch ReShade's proxy device via `GetPrivateData`.
* **ReShade ≥ 6.8.0 is a hard prerequisite.** ReShade-addon + D3D12-DLSS under vkd3d was a known
  crash; crosire fixed it in two commits, both in tag v6.8.0. 6.7.3 is still broken. Log the
  ReShade version at startup and warn below 6.8.0.
* **`PROTON_ENABLE_NVAPI` does not exist.** NVAPI is on by default. Only `PROTON_DISABLE_NVAPI` and
  `PROTON_FORCE_NVAPI` exist. Never set `PROTON_HIDE_NVIDIA_GPU=1`.
* **Do not gate availability on `NVSDK_NGX_D3D12_GetFeatureRequirements`** — Proton's `_nvngx.dll`
  does not implement it and reports unsupported even when DLSS works. Gate on `Init` succeeding
  plus `NVSDK_NGX_Parameter_SuperSampling_Available`.
* **Do not set `VKD3D_CONFIG=descriptor_heap`.** Leave `PROTON_ENABLE_NGX_UPDATER` off.
* Detect vkd3d for free: `QueryInterface(IID_ID3D12GraphicsCommandListExt,
  77a86b09-2bea-4801-b89a-37648e104af1)` on the native command list. ReShade uses this itself.

### 0xd2e4d8c23c362ed1 IS a TAA permutation — and the "it writes depth+stencil" claim was a capture artifact

> **CORRECTED 2026-08-31 from the game's own shipped bytecode.** The offline extractor
> (`tools/shaderlib_extract.py`, cache-only mode) finds `0xd2e4d8c23c362ed1` INSIDE the
> FTAAStandaloneCS section of `GlobalShaderCache-PCD3D_SM5.bin` (27 cs permutations, all
> enumerable offline), and a declaration scan of its DXBC shows **SRVs t0-t5 and exactly ONE
> UAV, u0, plus `dcl_tgsm_structured`** — the identical shape to `0x901e041a7cadc9db`. It is
> the `AA_UPSAMPLE` permutation selected at 2560x1440 output / 50% screen percentage.

The original section below is preserved as a lesson, because its reasoning error is the
instructive part. The live capture reported this pass writing u2 (colour), u3 (depth) and u5
(stencil). The shader declares none of those. **Our binding capture enumerates bound
descriptor-TABLE slots, not shader-declared registers** — UE4 binds tables wider than any one
shader's declarations, so "the table holds a depth UAV at u3" never meant "the shader writes
depth". The 1x1 dummies at u1/u4/u6 in the same capture were the tell.

The observed frame corruption when replacing it ("frozen like we dropped TAA, and the cat
sideways") had nothing to do with unreproduced outputs: it was the ext-vtable no-write bug
(§1 resolution) — NGX was writing nothing, into any pass, at the time.

**What remains true and load-bearing:** "suppressing it freezes the picture" proves a pass is
ON THE DISPLAY CHAIN, not that it is the TAA — motion blur or any composite pass freezes it
identically. The suppression test is necessary, never sufficient. And before trusting any
claim about what a pass writes, check the shader's own `dcl_uav` declarations — extractable
offline for every global shader via `tools/shaderlib_extract.py --dump-dir`.

### The candidate set DOES contain a pass that drives the image (measured 2026-08-31)

Suppressing **every** relaxed candidate (`NgxDryRun=3`: any dispatch with a depth SRV, a
velocity SRV and an HDR colour UAV), alternating within one session, fixed viewpoint, n=32:

```
SUPPRESSED: n=16  mean=15263  sd=23448  median=3107  min=569  max=71461
normal    : n=16  mean= 2431  sd= 3258  median=1474  min=620  max=14787
```

Median frame-to-frame shimmer is **2.1× higher** when suppressed, and the suppressed group has
a tail the normal group has nothing like (71461 against 14787). Use the medians: the means are
outlier-driven.

**The effect grew monotonically as each confound was removed** — 47% → 71% → 111% median gap —
which is what a real signal emerging from noise looks like, and is more persuasive than any one
run. The three fixes, in order of how much they mattered: comparing a frame against itself
rather than measuring absolute sharpness; cropping away the animated CRT wall; and finally
**not panning at all during measurement**, since a pair caught while the camera is still
settling differs enormously whatever the TAA is doing.

**This revises the earlier "DLSS's output is invisible" reading.** At least one pass in the
candidate set does drive the picture; the strict §2.3 signature simply is not selecting it. The
next step is a bisection over the candidate list — suppress half, measure, halve again — which
this instrument is finally sharp enough to run.

### Temporal stability is the right metric, and the CRT wall is the confound

Absolute sharpness could not separate the groups (sd 7810 against a 1633 difference). Comparing
a frame against ITSELF a moment later, camera still, is far better: content cancels, and working
temporal AA converges so consecutive still frames are nearly identical, while suppressing it
lets jitter reshuffle sub-pixel sampling every frame.

Re-measuring `0x8978e4e6431cacb3` that way, n=32:

```
SUPPRESSED: n=17  mean=3898  sd=2250  median=3851
normal    : n=15  mean=3140  sd=1825  median=2617
```

Suppressed frames shimmer **more**, which is the physically correct direction, and the medians
differ by 47%. But t ≈ 1.05 on the means — **not significant**, so this neither confirms nor
overturns the n=44 negative above.

**The confound is the scene.** Stray's starting apartment is full of animated CRT screens that
change every frame no matter what the TAA does, and they contribute to the frame difference in
both groups equally. That is almost certainly what is holding the spread at ~2000. Two ways to
fix it, in order of cheapness: crop the measurement to a region with no screens in it, or drive
the cat somewhere without them. Either should shrink the variance far more than adding samples
will.

### Candidate 0x8978e4e6431cacb3 is NOT the pass that drives the image (n=44)

Measured with the alternating harness, `tools/measure-alternating.py`, 44 captures with a panning
camera inside one session:

```
SUPPRESSED: n=21  mean=16464  sd=7810  median=12204  min=8138  max=36163
normal    : n=23  mean=14831  sd=6545  median=12316  min=6451  max=30125
```

The means differ by 1633 against a standard error of the difference near 2183 (t ≈ 0.75), and
the **medians are within 1%**. The mean gap is entirely a few outliers where the pan crossed
bright detail. Suppressing this pass does not change the image.

That matters because it was the strongest candidate: the only 1:1 pass with both the
depth+stencil pair and full-resolution velocity, and the only one the strict matcher called
"structure matches".

**Know the test's power before trusting a negative.** With this spread it can only resolve a
difference of roughly 2×SE ≈ 4400, about 27% of the mean. A subtle temporal difference would
hide inside that. A sharper instrument would compare *consecutive* frames for temporal
stability rather than absolute high-frequency energy — aliasing that shimmers frame to frame is
what TAA removes, and that is a much larger signal than the absolute sharpness of any one frame.

Remaining full-res-velocity candidates: `0xee4b6c0ca521851f`, `0xf102e72bdc0355be`,
`0xd2b4d12546bf6dd5`, `0x52b3e5af4f54ff47`.

### How to test a temporal pass honestly (the method, after three false starts)

Three things each defeated an earlier attempt, and all three must be handled together:

1. **A stationary camera hides everything.** With nothing moving, TAA's history is converged and
   suppressing the pass looks identical to leaving it in. `tools/screenshot-stray.sh` now pans
   the right stick (`ABS_RX` on "Microsoft X-Box 360 pad 0") before capturing; `PAN=0` restores
   the old behaviour.
2. **A moving camera makes runs incomparable**, because the scene changes too. So compare
   *within* one session: `DryRunAlternate=<frames>` flips the named pass between suppressed and
   normal, logging `ALT PHASE SUPPRESSING/normal at frame N` so a screenshot's timestamp
   identifies its state.
3. **Mean RGB and cyan counts are the wrong metrics** for this. Use high-frequency energy —
   `magick shot.png -colorspace gray -morphology Convolve Laplacian:0 -format
   "%[fx:standard_deviation]"` — since aliasing raises it and temporal accumulation lowers it.

**First measurement with all three in place, and it is INCONCLUSIVE.** Suppressing
`0x8978e4e6431cacb3`, HF energy ×10⁵: suppressed `9044, 10490, 11252, 12567, 10722` (mean 10815)
versus normal `12087, 21619, 12939` (mean 15548). The suppressed frames score *lower*, which is
backwards for removing an anti-aliasing pass — but one normal sample (21619) dominates its group
and the within-group spread is wider than the difference. **n = 5 and 3 is not enough. Take 20+
per phase before drawing any conclusion.**

### A STATIC SCENE INVALIDATES EVERY TEMPORAL TEST — read this before trusting the two below

The launch script drives the cat to gameplay and then stops, so every screenshot taken this
session was of a **stationary camera**. With nothing moving, TAA's history is already converged:
suppressing the pass produces very nearly the same image as leaving it in place. So
"suppressing it changed nothing" does **not** establish "that pass does not matter" — it may only
mean nothing was moving.

That undermines the two sections below, which were written from static captures. Treat their
conclusions as unproven, not as facts.

`tools/screenshot-stray.sh` now pans the right stick (`ABS_RX` on the
"Microsoft X-Box 360 pad 0" node) for about a second before capturing; `PAN=0` restores the old
behaviour. The remaining difficulty is that a moving camera also changes the scene, so frames
from different runs are no longer directly comparable — a content-independent metric, or a
toggle within a single run, is needed to compare them honestly.

### The §2.3 signature does not match this configuration (measured 2026-08-31)

`NgxDryRun=2` suppresses **every** structurally matched pass and writes nothing. The image is
unchanged — mean RGB `R=44 G=49 B=35`, identical to the control, and the cyan count sits inside
the control's own 0–2134 noise band.

**So none of the passes our matcher finds draws the picture.** That answers the identification
question in one run instead of one run per candidate, and it rules out bisection as a next step.

TAA is definitely running: `sg.AntiAliasingQuality` is absent from `GameUserSettings.ini`, so the
scalability group falls back to the pak's own `r.DefaultFeature.AntiAliasing=2` (§2.3.1), and
`sg.ResolutionQuality=100` with `SteamDeckScreenPercentage=100` confirms 1:1.

**And 1:1 is the point.** Everything in §2.3 — the depth+stencil SRV pair over one resource, the
dispatch over an output rect larger than the inputs — was measured at 3840×2160 with 50% screen
percentage, i.e. `ETAAPassConfig::MainUpsampling`.

> **CORRECTED 2026-08-31.** This section went on to claim that 1:1 selects
> `ETAAPassConfig::Main`, "a different permutation whose bindings we never characterised". Both
> halves are wrong: the config stays `MainUpsampling` at every screen percentage, and all 27
> cooked permutations declare the same registers. What genuinely does not survive 1:1 is "the
> dispatch covers an output rect LARGER than the inputs" — at 1:1 `SetupViewRect` makes
> `OutputViewRect == InputViewRect`, so that test is a *ratio* detector, not a TAA detector.
> See the DLAA section in §5 for the source quotes and the fix.

**Consequence:** §2.3's signature is configuration-specific and must be treated as such. Either
characterise the `Main` permutation at 1:1, or drive the game back to the configuration §2.3 was
measured in (`r.ScreenPercentage` below 100 with `r.TemporalAA.Upsampling=1`, §4) so the
documented shape reappears. The second is cheaper and is also where DLSS SR wants to be anyway.

### DLSS runs, but into a pass that does not drive the image (measured 2026-08-31)

`NgxDryRun` suppresses the pinned pass and writes **nothing** in its place. If that pass drove
the picture, the image would visibly break. It does not:

```
DRY RUN: suppressing pass 0x2a7ec4fd7daced09 and writing NOTHING
mean RGB  control R=44 G=49 B=35 | DLSS R=44 G=49 B=35 | dry run R=44 G=49 B=35
```

**So every "DLSS evaluate OK" so far has written into a texture the frame never displays.** The
API side is correct — feature created, evaluated, no NGX error, no crash — and the result is
invisible. An unchanged image was never evidence of success.

Two further facts from the same runs:

* The pass chosen **varies between runs** (`0xda289b0ddfa934c6`, then `0x2a7ec4fd7daced09`), so
  the history round-trip test matches SEVERAL passes and pins to whichever proves itself first.
  §2.9 calls the round-trip decisive; measured here it is necessary but not sufficient.
* Neither hash is one of the structural TAA candidates measured at this resolution
  (`0xd2e4d8c23c362ed1` / `0xe14e7fc8d0db9b0f`).

**The identification problem is therefore open, and it is now the whole problem.** The way to
close it is the dry run itself, applied per candidate: suppress exactly one pass, screenshot,
and compare. The pass that drives the image is the one whose suppression changes it. That is a
bisection over a handful of candidates and needs no new theory.

### The device choice, settled by experiment — and §1's prediction was wrong

**Measured 2026-08-31.** The `NgxEvaluate` crash was caused by giving NGX **ReShade's proxy
device**. Forcing it onto the **native** device (`[STRAYDLSS] NgxDevice=native`) reaches gameplay
and keeps running: DLSS feature created 2560×1440 DLAA preset K, pinned to one pass, repeated
`DLSS evaluate OK`, game healthy at 40% GPU.

The crash was inside **vkd3d-proton itself** — `d3d12core.dll +0x3A4AB0`, called from
`+0x395BB0`, with `rbx=rcx=8146001500000004` and `rdx=rsi=0048002500000001`. Those are not
pointers but packed handle-shaped values: a descriptor handle reaching vkd3d unconverted. Give
NGX the proxy and its descriptors are ReShade-synthetic; whatever path then carries them into
vkd3d does not convert them, and vkd3d dereferences a bit-packed integer.

**SUPERSEDED 2026-08-31, and the supersession is instructive.** The "correct image" measured
for the native+patched row was an artifact: DLSS was pinned to a wrong pass AND its output was
never being written (see below), so the screen simply showed the engine's own image. The truth
table's third row IS broken as originally predicted — but the failure mode on vkd3d-proton 3.1
is a loud rejection rather than a silent wrong texture: the mangled handle makes
`NvAPI_D3D12_GetCudaMergedTextureSamplerObject` / `GetCudaIndependentDescriptorObject` fail
with `nvapi status -5` on every evaluate (visible in our log via the NGX LoggingCallback),
NGX reports evaluate success anyway, and the output resource stays untouched. Proven by
`NgxDumpInputs=1`: changing colour/depth inputs, byte-identical all-zero 66 MB output across
300 evaluates. `NgxPaint=1` (magenta clear of the captured `u0`) separately proved the output
handle, the suppression and the downstream chain correct.

**Resolution: native device + `src/ext_unhook.{hpp,cpp}` (default ON, `ExtUnhook=0` opts out).**
Capture the pristine slot pointers (7/8, and 14/15 for Ext2) at `init_device`, before the game's
stack can have routed a query through ReShade's proxy; write them back immediately before NGX
init, feature creation and every evaluate — one QueryInterface re-installs the patch at any
time, so the repair is re-applied, never applied once. Safe in this process because the only
caller of the CUDA entry points is `nvngx_dlss.dll` itself: Stray never calls them, DXVK-NVAPI
only queries the interface. If capture finds the slots already ReShade-owned, the originals are
unrecoverable and the repair disables itself loudly.

### The NgxEvaluate crash: what has been ruled OUT

DLSS evaluates correctly in Stray — `DLSS evaluate OK: 2560x1440 -> 2560x1440
jitter=-0.3750,-0.0556 reset=0 preExposure=0.056`, no NGX error — and the game then dies about
a second later at the loading→gameplay transition. **CPU access violation `0xc0000005`, no Xid,
UE4's dump carries an empty callstack, and ReShade's log simply stops mid-frame.**

A step trace that opens only after the first successful evaluate shows our entire cycle
completing, repeatedly, right up to the end:

```
ngx-trace: barrier mv -> SRV → evaluate returned 1 → barrier mv -> UAV done
ngx-trace: restore begin → restore done          (and again, and again)
```

**So the fault is downstream of our code.** Five hypotheses have been tried and none fixed it —
do not spend the round trip re-testing them:

1. **Resource lifetime.** NGX holds no references to what we pass it, so we AddRef colour, depth,
   motion vectors and output at Evaluate and release them six frames later. Correct, and kept —
   but not the cause.
2. **Write-after-write on `u0`.** We now suppress the engine's TAA dispatch when DLSS produced
   the output. Correct, and kept — but not the cause.
3. **Wrong colour input.** NGX rejected it once — *"input Color parameter needs to be Tex2D
   resource"* — so candidates are now filtered to live 2D textures of at least the render rect,
   which excludes buffers and the 1×1 `BlackDummy`. Fixed the NGX error; the crash remained.
4. **Replacing several passes.** The structural matcher matches many passes (numerous distinct
   hashes each report a history round-trip), so suppression was hitting more than the TAA. DLSS
   is now pinned to the first pass it successfully evaluates. Correct, and kept — but not the
   cause.
5. **Feature recreation mid-flight** was suspected from the timing but only one feature is ever
   created in these runs, so it is not being torn down under the GPU.

**Still untried:** whether ReShade's own effect runtime or another add-on is what dies (the other
two add-ons are renamed `.disabled`, but ReShade still compiles and runs its own effects);
evaluating only every Nth frame to see whether it is cumulative; and running with `MvDispatch=1`
to shrink the GPU work while keeping the evaluate.

### CONFIRMED WORKING ON THE TARGET, 2026-08-31

NGX initialises and reports DLSS available, in Stray, under vkd3d-proton, through ReShade:

```
[pre-NGX] vkd3d ID3D12DeviceExt slot 8 is HOOKED BY RESHADE (dxgi.dll)
NGX will use ReShade's PROXY device (…6968B50): the ext hook is installed, so
  descriptors must be ReShade-minted for its conversion to be correct.
Initialising NGX (frame 120)...
NVSDK_NGX_D3D12_Init_with_ProjectID succeeded
DLSS SR available=1 needs_updated_driver=0 min_driver=470.0 feature_init=0x00000001 (Success)
[DLSSCubinKernelMap::InitCubins:303] Setting DLTSS Engine Cubins
Add-on event check OK: bind_pipeline and push_descriptors both observed
```

Three things this settles, each previously inference:

* **The cubin path works.** `nvngx_dlss.dll` loaded its DLTSS engine cubins through DXVK-NVAPI
  into vkd3d — the mechanism §1 describes, now observed rather than argued.
* **`Init_with_ProjectID` needs no whitelist**, and `Available=1` comes back without ever calling
  `GetFeatureRequirements` (which Proton does not implement).
* **The ext hook is installed by frame 120**, earlier than the frame-300 figure measured before.
  The device choice must therefore be made from the live vtable state, never from a fixed rule —
  which is exactly what the add-on now does, and it picked the proxy.

Still not done: `CreateFeature` and `EvaluateFeature` do not exist yet. NGX is initialised and
interrogated, nothing more, so none of this yet proves an image.

### The native-device rule has a trap, measured 2026-08-31

**Two configurations are self-consistent. The one we currently ship is only safe by luck.**

ReShade 6.8.0 added `source/d3d12/d3d12_extensions.cpp`. Inside its proxy's `QueryInterface`
for `IID_ID3D12DeviceExt`/`Ext1`/`Ext2` it patches vtable slots **7/8** (and 14/15 for `Ext2`)
of vkd3d's extension interface, so that `GetCudaTextureObject` / `GetCudaSurfaceObject` run
their descriptor handles through `convert_to_original_cpu_descriptor_handle`. Those are the
**CUDA texture and surface entry points — the path `nvngx_dlss.dll` takes under vkd3d.**

The conversion has no release-build guard:

```cpp
const size_t heap_index = (handle.ptr >> heap_index_start) & 0xFFFFFFF;
assert(heap_index < _descriptor_heaps.size() && _descriptor_heaps[heap_index] != nullptr);
return { _descriptor_heaps[heap_index]->_orig_base_cpu_handle.ptr + ... };
```

A **real vkd3d handle** yields a garbage `heap_index`, an out-of-bounds read, and a handle
pointing anywhere. DLSS then samples the wrong texture, silently. **That is what a colour cast
looks like.**

| NGX device | ext vtable patched? | Result |
|---|---|---|
| native (`get_native()`) | no | **works** — handles are never converted |
| ReShade proxy | yes | **works** — ReShade minted them, so conversion is correct |
| **native** | **yes** | **BROKEN** — real handles run through the conversion |

**Measured on the RTX 4090, 2026-08-31** (`test_vkd3d_ext_hook_reachability`): vkd3d-proton uses
a **single static vtable** for this interface, so one query against ReShade's proxy — *from
anywhere in the process* — patches the vtable that interfaces taken straight from the **original**
device also use. `before=…C645AD0 after=…D5680D0`, and the patched entry belongs to ReShade's
DLL. Reachability is **HARD**.

**CONFIRMED IN THE LIVE GAME, 2026-08-31.** Stray itself installs the patch. Measured with our
add-on in observation-only mode (`EnableNGX=0 MvDispatch=0 MvResolve=0`) and with the other two
ReShade add-ons disabled, so nothing of ours and no third-party add-on is responsible:

```
[init_device] vkd3d ID3D12DeviceExt present, slot 8 unhooked (owner=d3d12core.dll) - safe.
[frame 300]   vkd3d ID3D12DeviceExt slot 8 is HOOKED BY RESHADE (dxgi.dll).
[frame 1200]  ... HOOKED BY RESHADE.
[frame 3600]  ... HOOKED BY RESHADE.
```

The state **changes during the session**: clean at device creation, hooked by frame 300. A
startup-only check reports "safe" and is wrong, which is why the check is repeated.

**Therefore the shipped configuration is the broken row.** Initialising NGX with
`device::get_native()` while the patch is installed means our real vkd3d descriptor handles are
run through `convert_to_original_cpu_descriptor_handle`, producing a garbage heap index, an
out-of-bounds read, and a handle pointing anywhere — a wrong texture sampled with no error.

**Consequence for §1: the native-device rule is necessary but not sufficient.** Proxy-with-patch
is not an option — measured, it crashes vkd3d on the synthetic packed handles that reach it
unconverted through the non-hooked entry points. Native-with-repair is the working combination:
`ext_unhook` restores the patched slots before every NGX call (see the resolution above).

Full chain, prerequisites and diagnostics: `docs/RESEARCH.md` §1.

---

## 2. Everything we know about Stray

Measured on hardware, read out of the game's own DXBC, or read from the game's own files — not
inferred. Verbatim source: `docs/STRAY-RENDERING-FACTS.md`. Facts *derived* from these by verified
research are marked **[derived]**.

### 2.1 Engine and process

| Fact | Value |
|---|---|
| Engine | **Unreal Engine 4.27.2** |
| Executable | `Stray-Win64-Shipping.exe`, PE32+ (x86-64) |
| Graphics API | **D3D12** |
| UE project name | `Hk_project` |
| Version string | `v1.54368 (Revision 26632)` |

Observed swapchains from `IDXGISwapChain::ResizeBuffers`:
`BufferCount = 3` at `3840×2160` and `2560×1440`, `NewFormat = 24` = `DXGI_FORMAT_R10G10B10A2_UNORM`.

**[derived]** Each output-resolution change requires a full NGX `ReleaseFeature` + `CreateFeature`.
Guard Evaluate to no-op when observed sizes differ from creation sizes.

### 2.2 Filesystem layout

```
<SteamLibrary>/steamapps/common/Stray/Hk_project/Binaries/Win64/
```

Config and saves live in the **Proton prefix**:

```
<compatdata>/1332010/pfx/drive_c/users/steamuser/AppData/Local/Hk_project/Saved/
    Config/WindowsNoEditor/Engine.ini
    SaveGames/
    Crashes/UE4CC-Windows-<GUID>_0000/     CrashContext.runtime-xml, UE4Minidump.dmp
    Logs/                                   (observed empty)
```

* **`Engine.ini` settings take effect. Command-line arguments do not.** Everything we need the
  engine to do goes through `[SystemSettings]` in that file — which **[derived]** is also the only
  way to set `ECVF_ReadOnly` cvars like `r.UsePreExposure`.
* Two `compatdata` trees exist on this machine. The live one is
  `/home/deck/.local/share/Steam/steamapps/compatdata/1332010`. The one on `GamesLinux` is a
  ~6.1 MB skeleton that is **not read**.

### 2.3 The TAA pass — our interception point

Stray uses UE 4.27's `FTAAStandaloneCS`. **[derived]** that is
`/Engine/Private/TemporalAA/TAAStandalone.usf`, entry `MainCS` — **`PostProcessTemporalAA.usf` does
not exist in 4.27**. Threadgroup 8×8, dispatch `ceil(W/8) × ceil(H/8)`.

> **CORRECTED 2026-08-30, against the live game.** The original measurement had the shader
> identities **backwards**, and everything below supersedes it. The add-on now captures real
> bindings per dispatch, and a separate bytecode analysis confirmed the correction
> independently. `docs/STRAY-RENDERING-FACTS.md` is left as the historical record.

**The TAA pass is:**

```
0x901e041a7cadc9db          <-- at 3840x2160 output / 1920x1080 render
```

> **MEASURED 2026-08-31 AT A DIFFERENT RESOLUTION.** Running at **2560×1440 output / 1280×720
> render** (still 50% screen percentage), the structural matcher finds the pass but the hash is
> **not** `0x901e…`. Two candidates come back `structural_only`, both dispatching 320×180 = the
> **output** rect:
>
> ```
> 0xd2e4d8c23c362ed1        0xe14e7fc8d0db9b0f
> ```
>
> The two known look-alikes were correctly **excluded** in the same run, so the exclusion rules
> hold. **The hash is a PERMUTATION fingerprint, not the shader's identity — and the permutation
> is chosen by the upsampling ratio, not by the pixel count.** 1440p and 4K at the same screen
> percentage share a hash and differ only in dispatch size; going from 50% to 100% changes the
> code that is compiled. Never gate on the hash alone; the depth+stencil-over-one-resource
> signature is what actually identifies the pass. Also measured in the
> same frames: the View CB appeared at **b4**, not b1, so that register is not invariant either.
>
> **CORRECTED 2026-08-31 from UE 4.27.2 source.** The dimension that flips is
> **`FTAAScreenPercentageDim`** (`TemporalAA.cpp:726-750`) — 1 below 71% of the output, 0 at 1:1
> — *not* `AA_UPSAMPLE`. `ETAAPassConfig` stays `MainUpsampling` at every screen percentage
> because `r.TemporalAA.Upsampling=True` and nothing consults the resolution fraction (see the
> DLAA section in §5). `ShouldCompilePermutation` (`TemporalAA.cpp:228`) confirms the direction:
> a non-zero `TAA_SCREEN_PERCENTAGE_RANGE` is only compiled *for* an upsampling config, so a
> range-1 shader is by construction `MainUpsampling`.

Captured live at 3840×2160 output. Note every input is **1920×1080**: the game already runs
**temporal upsampling at 50% screen percentage**, so the pass is `ETAAPassConfig::MainUpsampling`,
not `Main`.

| Register | Role | Format | Size |
|---|---|---|---|
| `t0` | `EyeAdaptationTexture` | `R32G32B32A32_FLOAT` | 1×1 |
| `t1` | `InputSceneColor` | `R16G16B16A16_FLOAT` | 1920×1080 |
| `t2` | `SceneDepthTexture` | `R32_FLOAT_X8X24` | 1920×1080 |
| `t3` | `GBufferVelocityTexture` | `R16G16B16A16_UNORM` | 1920×1080 |
| `t4` | `StencilTexture` | `X32_G8X24_UINT` | **same resource as t2** |
| `t5` | `HistoryBuffer_0` | `R16G16B16A16_FLOAT` | 1920×1080 |
| `u0` | `OutComputeTex_0` | `R16G16B16A16_FLOAT` | **3840×2160** |

Dispatch is **480×270** = `ceil(3840/8) × ceil(2160/8)` — over the **output** rect, not the
render rect. Matching against the render rect rejects the real pass.

**Why the original identification failed, and the lesson.** The old heuristic scored this
shader `colour=1 depth=2 velocity=0` and rejected it. Both halves of that score are actually
the TAA's *signature*: `velocity=0` because the frame it sampled was a **camera cut**, where
UE4 substitutes the 1×1 `GSystemTextures::BlackDummy` for velocity and history; `depth=2`
because the depth SRV and the stencil SRV are **two views of one resource**. The gate rejected
the pass for exactly the properties that identify it. **`EyeAdaptationTexture` is also 1×1 and
present every frame, so "any 1×1 SRV" is not a cut signal** — only the velocity and colour
slots count.

**The strongest runtime discriminator** is the depth+stencil pair over a shared resource. No
other pass in this title binds one resource as both `R32_FLOAT_X8X24` and `X32_G8X24_UINT`.

**Two convincing look-alikes, both excluded:**

* `0x1708ec956099e259` — previously believed to be the primary TAA. It is
  **`FSSDTemporalAccumulationCS`** (named offline 2026-08-31 via `shaderlib_extract.py
  --find-hash` + CUE4Parse's name table): UE 4.27's Screen-Space Denoiser temporal
  accumulation, permutation #5 of a 9-permutation family that ALSO contains
  `0x52101a15e1a0c5cc` (#0), `0xee4b6c0ca521851f` and `0xf102e72bdc0355be` — retroactively
  closing several unresolved bisection-era candidates. It reads depth and velocity and
  reprojects with `ClipToPrevClip`, which is why its bindings look right. It declares `cb1[126]` and so never
  indexes View row 144 (`StateFrameIndexMod8`), which **every** `FTAAStandaloneCS` permutation
  reads — therefore it cannot be this shader. Dispatches 240×135, output 1920×1080.
* `0x52101a15e1a0c5cc` — `FSSDTemporalAccumulationCS` permutation #0 (see above). Eleven
  SRVs, two UAVs, `cb1[131]`. Not TAA.

**Bytecode evidence for the correction** (from the shipped DXBC, `cs_5_0`, `numthreads(8,8,1)`):

* Every `FTAAStandaloneCS` permutation must declare `cb1[145]`; only five dumped shaders do,
  and `0x901e` is one.
* `0x901e` declares `dcl_tgsm_structured` stride 16 count 64 — `float4[64]`, which compiles
  only under `AA_UPSAMPLE == 1` with `TAA_SCREEN_PERCENTAGE_RANGE == 1` (input under ~71% of
  output).
* The upsample-only kernel constants `0.905` and `-1.9` appear 7× in `0x901e` and in no other
  shader except its sibling permutation.

**Consequence for the DLSS work.** The game is *already* upsampling 1080p→4K. DLSS should be
created as **1920×1080 → 3840×2160** (a Performance-ratio feature), **not DLAA** — which
inverts the plan's staging, since there is no 1:1 pass to replace.

### 2.3.1 Confirmed from the game's own shipped configuration

Extracted from `Hk_project-WindowsNoEditor.pak` (`tools/pakextract.py`) and from the live prefix
config. Copies are kept in `docs/game-config/`. These retire several previously *derived* facts
to HARD — they are the developers' own settings, not inference:

| Setting | Value | Source | What it settles |
|---|---|---|---|
| `r.DefaultFeature.AntiAliasing` | **2** (TAA) | `DefaultEngine.ini:54` | TAA is the AA method |
| `r.TemporalAA.Upsampling` | **True** | `DefaultEngine.ini:69` | **TAAU ships enabled.** The upsampling TAA pass is the game's own default, not a user setting |
| `r.BasePassOutputsVelocity` | **True** | `DefaultEngine.ini:59` | Velocity is written in the base pass, so coverage is broader than stock UE4 — helpful for DLSS, but the resolve pass is still required |
| `r.PostProcessAAQuality` | **3** (Medium) | `WindowsEngine.ini:18` | Confirms the earlier derivation exactly |
| `r.DOF.TemporalAAQuality` | 0 | `WindowsEngine.ini:58` | |
| `ScreenPercentage` | **50** | live `GameUserSettings.ini` | Renders 1920×1080 |
| `ResolutionSizeX/Y` | **3840×2160** | live `GameUserSettings.ini` | Output is 4K |

So the render/output relationship measured at runtime — 1920×1080 in, 3840×2160 out — is
exactly what the shipped configuration asks for. **`ETAAPassConfig::MainUpsampling` is
confirmed by the game's own settings**, independently of the bytecode and binding evidence.

The only user-added override in the live `Engine.ini` is `r.BasePassOutputsVelocity=1`, which is
redundant since the game already ships it as True.

**The cat's fur material, read the same way 2026-09-03.** `M_Fur_2sidedshading_backpackON` is
a `MaterialInstanceConstant` over `M_Base_GFur_2sidedshading`; both are **`BLEND_Masked`**, the
instance overrides the shading model to `MSM_SubsurfaceProfile` (parent `MSM_TwoSidedFoliage`),
and the base sets `bUsedWithSkeletalMesh`. Masked is what puts the ~48 gFur shell layers in the
**opaque base pass**, which is what makes them write `GBufferVelocity` — see "The cat is unchanged
under NR, and the fur is NOT a motion-vector hole" in §5 for the full chain and for the extraction
procedure (these assets are Oodle-compressed, so `pakextract.py --raw` + `oodle_unblock.py` is
required).

**Reading the pak.** It is version 11, **unencrypted**, 5.3 GiB, 55,120 entries.
`tools/paklist.py` lists it and `tools/pakextract.py` extracts by regex, both reading only the
index blobs rather than the archive — worth knowing, because a bulk copy is neither necessary
nor kind to a machine someone is playing on.

**Shader census:** 728 distinct PS/CS shaders in gameplay, `not_dxbc=0`, `dxil=0` — every one is
**DXBC**. ~150 in the main menu, rising to ~728 on entering gameplay.
> **MEASURED 2026-09-01:** that 728 was the apartment. The main menu reads **110** and gameplay in
> **The Slums (save slot 1) reads 388-392**; the add-on's `in_game` threshold is now **300**, not
> 400, which never fired there.

### 2.4 Depth

* `t0` resource is `r32_g8_typeless`; the SRV is `r32_float_x8_uint`.
* UE 4.27 uses **reversed-Z** → `NVSDK_NGX_DLSS_Feature_Flags_DepthInverted`. **[derived]** near =
  1.0, far = 0.0, **infinite far plane**, `SceneDepth = Near / DeviceZ`. The TAA shader
  hard-`#error`s on non-inverted Z. Read `View.NearPlane` (row 142.x) rather than assuming 10 uu.

Depth over menu and loading screens reads `below 0.25: 3456000, above 0.75: 0, mean 0.00000` —
**menu and load frames carry no usable depth range; gameplay frames do.** Use this as a cheap
"are we in gameplay" gate.

### 2.5 Velocity — and the sparse-velocity problem

`t2` is `r16g16b16a16_unorm` and **sparse**: UE 4.27 writes it only for **moving objects**. Static
geometry carries nothing and its motion must be reconstructed from depth and the camera matrices.

**[derived] A resolve pass is mandatory regardless**, because DLSS only accepts `RG16_FLOAT` or
`RG32_FLOAT` motion vectors and `r16g16b16a16_unorm` is not an accepted format.

**[derived]** The velocity texture is at the full **scene-buffer** extent
(`BufferSizeAndInvSize`, row 132), *not* the view size — index it with absolute buffer coords.

**Encoding, from UE 4.27 `Common.ush:1537-1570`:**

```
EncodedV.xy = V.xy * (0.499f * 0.5f) + 32767.0f / 65535.0f          // encode
V.xy        = EncodedV.xy * InvDiv - 32767.0f / 65535.0f * InvDiv   // decode
InvDiv      = 1.0f / (0.499f * 0.5f)
```

**Both constants located in Stray's own DXBC:**

| Constant | Value | Bit pattern | Notes |
|---|---|---|---|
| `InvDiv` | `4.00801611f` | `0x408041AB` | bytes `AB 41 80 40` |
| Folded MAD bias | `2.00397754f` | `0x4000412B` | appears **negated** as `0xC000412B` |
| Bias term | `32767/65535 = 0.49999237f` | `0x3EFFFF00` | **not** 0.5 |

**[derived] Compute the bias in float as `(32767.0f/65535.0f) * InvDiv`; do not hardcode a rounded
constant.** `DecodeVelocityFromTexture` takes a `float4` and returns a `float3`.

**[derived] Validity test is `EncodedVelocity.x > 0.0`** — strict, red channel only, because the
target is cleared to 0. Reproduce it exactly; never use a magnitude threshold.

**[derived]** The stored value is `ScreenPos - PrevScreenPos` in NDC with **both** frames' jitter
removed — directly comparable to the `ClipToPrevClip`-derived camera motion. Both branches are
jitter-free and both are current-minus-previous.

### 2.6 The View constant buffer

Stray's TAA shader carries the stock UE 4.27.2 `View` uniform buffer at register **`b1`**. Observed
total sizes vary (126 / 131 / 145 float4s) but **row offsets do not**. The layout was established
twice independently from `VIEW_UNIFORM_BUFFER_MEMBER_TABLE` (`SceneView.h:582-774`), and **[derived]**
a third time by a recomputation that reproduced all seven measured anchors exactly.

| Field | Row | Byte | |
|---|---|---|---|
| `ViewToClip` | 28 | 448 | measured |
| `ViewToClipNoAA` | 32 | 512 | measured |
| `InvDeviceZToWorldZTransform` | 65 | 1040 | [derived] |
| `ClipToPrevClip` | 122 | 1952 | measured |
| `TemporalAAJitter` | 126 | 2016 | measured |
| `ViewRectMin` | 129 | 2064 | measured |
| `ViewSizeAndInvSize` | 130 | 2080 | measured |
| `LightProbeSizeRatioAndInvSizeRatio` | 131 | 2096 | measured — **decoy**, reads `(1,1,1,1)` |
| `BufferSizeAndInvSize` | 132 | 2112 | [derived] |
| `PreExposure` | 135.y | 2164 | **MEASURED 2026-09-01** — see the row-135 self-check below |
| `OneOverPreExposure` | 135.z | 2168 | **MEASURED 2026-09-01**; also assigned as `1.f / PreExposure` on the adjacent source line |
| `NearPlane` | 142.x | 2272 | [derived] |
| `DeltaTime` | 143.x | 2288 | [derived] |
| `CameraCut` | 145.x | 2320 | [derived] |
| `TemporalAAParams` | 152 | 2432 | measured |

Everything we need is in a single **2448-byte prefix**. **Rows beyond 152 were not verified — do
not use them.**

> **RETRACTED 2026-09-01, same day, and the retraction is the lesson.** This block asserted that
> row 135.z is not `OneOverPreExposure`, on the strength of one frame where 135.y read 0.456 and
> 135.z read 6.6794 — a product of 3.05 rather than 1.
>
> **The row mapping was right and the MEASUREMENT was wrong.** `SceneRendering.cpp:1563-1564`
> assigns the pair on adjacent lines from the same float:
> ```cpp
> ViewUniformShaderParameters.PreExposure        = PreExposure;
> ViewUniformShaderParameters.OneOverPreExposure = 1.f / PreExposure;
> ```
> Their product is 1.0 **by construction**. The two numbers I compared came from two different
> reads at two different times — 6.6794 sits inside the sequence of exposure factors logged that
> session while 1/0.456 = 2.193 does not.
>
> **Two costs, and the second is worse.** The "fix" derived the reciprocal instead of reading it,
> which changed nothing; and it removed `ue4::pre_exposure_plausible` from the shipping path,
> deleting the only runtime detector for a genuinely bad exposure read while CI kept testing the
> now-orphaned function and stayed green. Both are reverted.
>
> **The rule this earns:** a self-checking pair is only a check if you read BOTH HALVES IN ONE
> READ. Comparing a value from one log line against a value from another is not a measurement of
> a relationship, it is a measurement of two unrelated moments. Row 135 offers a stronger free
> check that does not depend on timing at all: it must read `(denormal, P, 1/P, 0.0)` — `135.x` is
> `int32 NumSceneColorMSAASamples` reinterpreted as a float (≈1.4e-45) and `135.w` is padding,
> exactly 0.0. Dump all four from ONE `memcpy` of the 2448-byte prefix and the mapping validates
> itself.

`ClipToPrevClip` at row 122 was confirmed **in Stray's own TAA shader by pure DXBC instruction
analysis**, no reflection names involved.

#### Row 135 validates itself, and it did — MEASURED 2026-09-01

The retraction below asks for both halves of the pre-exposure pair to be read in ONE `memcpy`
rather than compared across log lines. That diagnostic shipped and fired in the live game:

```
View row 135 (one read): x=1.40129846e-45 y=0.451940 z=2.212684 w=0
  | y*z=1.000000        (want 1.0)
  | x denormal=1        (want 1, it is an int32 MSAA count reinterpreted as float)
  | w==0=1              (want 1, it is padding)
```

**All three independent predictions hold from a single read, so `kPreExposureRow` is the right
offset and the mapping is now HARD, not [derived].** `y*z == 1.0` exactly is the strong one: it
is true by construction (`SceneRendering.cpp:1563-1564`) and could not survive a wrong offset.
`x` being the denormal `1.40129846e-45` is `int32 NumSceneColorMSAASamples == 1` reinterpreted,
and `w` is exactly 0.0 padding — three different data types landing where predicted.

Measured in the same session, PreExposure moved **1.000 -> 4.881 -> 0.051 -> 0.452** across
menu, camera cut and gameplay. That is a ~95x swing, and it is independent support for keeping
`NgxNRTrackExposure` on with a long time constant: no fixed codec scale can be right across
that range.


Traps:

* Row 131 is `(1,1,1,1)` — a decoy for any search expecting an identity-looking row.
* `dcl_constantbuffer cb1[131]` — that 131 is the **highest row indexed**, not the buffer size.
* Offsets are fixed per engine build but not invariant across a licensee edit. Keep checking them.
* **[derived] Never read b0.** UE4 strips the DXBC reflection chunk (`D3DCOMPILER_STRIP_REFLECTION_DATA`),
  so `D3DReflect` is impossible; and loose params land in fxc's implicit `$Globals`, which fxc
  **compacts by removing unused globals**, so b0 offsets are unpredictable from source. b1 is an
  explicit `cbuffer View` with a layout fixed by `FShaderParametersMetadata`.

### 2.7 Jitter

```
InJitterOffsetX = TemporalJitterPixels.X = TemporalAAParams.z = TemporalAAJitter.x * W *  0.5f
InJitterOffsetY = TemporalJitterPixels.Y = TemporalAAParams.w = TemporalAAJitter.y * H * -0.5f
```

The negative Y factor is real — **but only in the derivation**. **[derived] Do not derive anything:**

> **`InJitterOffsetX = TemporalAAParams.z` (byte 2440), `InJitterOffsetY = TemporalAAParams.w`
> (byte 2444). Read and assign straight across. No negation, no scaling.**

`TemporalAAParams.zw` **is** `TemporalJitterPixels`, already in render-resolution pixels, in
`[-0.5, +0.5]`, and NVIDIA's own UE plugin passes it to NGX unmodified.

**[derived]** `TemporalAAParams` is `(JitterIndex, SequenceLength, JitterPixelsX, JitterPixelsY)`;
`TemporalAAJitter` is `(CurX, CurY, PrevX, PrevY)` in clip/NDC units.

**[derived]** In the non-upsampling mode UE4 warps Halton through Box-Muller with
`sigma = 0.47 * r.TemporalAAFilterSize` — a distribution DLSS was **not** trained on. Forcing
`r.TemporalAA.Upsampling=1` switches to plain Halton in `[-0.5, 0.5]`, which is what DLSS *was*
trained on. See §4. Stray ships `r.TemporalAA.Upsampling=True`, so it is already on the Halton
branch at every screen percentage (§5, DLAA section).

**Both branches are bounded to `[-0.5, 0.5]` per axis** — verified in the 4.27.2 source
(`SceneVisibility.cpp:3301-3327`). The Box-Muller path windows the RADIUS: `OutWindow = 0.5`,
`InWindow = exp(-0.5·(OutWindow/Sigma)²)`, and `r = Sigma·sqrt(-2·ln((1-u1)·InWindow + u1))`
gives `r = OutWindow` at `u1 = 0` and `r = 0` at `u1 = 1`, so the samples fill a disc of radius
0.5. The difference from the upscaling branch is the **shape** — a truncated Gaussian disc
versus Halton uniform over the square — not the range. `view_params_plausible`'s `|jitter| > 0.5`
rejection is therefore correct in both modes and is not what blocks DLAA.

### 2.8 Camera cuts

UE 4.27 assigns `PrevViewMatrices = ViewMatrices` on a cut. The observable consequence is
**`View.TemporalAAJitter.zw == .xy`**. Confirmed working as a live cut detector against the running
game (`detector=LIVE`). Observed counts: **3** across splash and main menu, **5** once gameplay is
running.

**[derived] That signal alone is insufficient.** `bCameraCut = !InputHistory.IsValid() ||
View.bCameraCut`, and the history-invalid case never reaches the View buffer. OR three signals for
`InReset`:

1. `View.CameraCut != 0` (row 145.x);
2. `TemporalAAJitter.zw == .xy` (our measured heuristic);
3. **the history or velocity SRV is a 1×1 texture** — most reliable, directly reflects
   `!InputHistory.IsValid()`.

### 2.9 TAA history

The resource at `u0` is extracted by UE 4.27 as the **next frame's `HistoryBuffer[0]`**.
**Overwriting `u0` feeds whatever we write into the next frame's temporal history** — the hook that
lets us replace TAA from outside the engine.

The same resource can also appear as this frame's **scene-colour input**; the two cases are
distinguishable **only by which register it turns up on**. Track by register, never by identity
alone.

### 2.10 Stability observations

Environment facts, independent of any add-on — useful when triaging so we do not chase our own tail:

* `gamescope-wl` segfaulted three times in one afternoon (11:40, 11:42, 14:03) and once the day
  prior. The nvidia driver was unloaded and reloaded at 11:43.
* One GPU `Xid 109 (CTX SWITCH TIMEOUT)` against `Stray-Win64-Shi`, channel `0x00000012`.
* UE4 crash dumps exist **from sessions with no third-party add-on at all**:
  `Unhandled Exception: 0xe06d7363` (a C++ exception) and one `EXCEPTION_ACCESS_VIOLATION reading
  address 0x0000000000000010`. **Stray crashes on its own.** Do not assume a crash is ours.
* Killing the game leaves a `reaper` process (`SteamLaunch AppId=1332010`) behind. While it exists,
  Steam silently ignores further `steam://rungameid/1332010` launches.

### 2.11 Driving the game unattended

* The physical DualSense is held by **Steam** via `/dev/hidraw0`. Nothing holds its evdev nodes.
* Steam Input re-emits it as **"Microsoft X-Box 360 pad 0"** — that node is what the game reads. Its
  `eventN` number is **not stable**; Steam tears it down with the game.
  > **CORRECTED 2026-09-01.** Steam Input is now OFF for this title (`UseSteamControllerConfig 0`
  > in `localconfig.vdf`, a consequence of the DualSense work, which needs the game to see the
  > real pad), so **no X360 node appears at all** and the pad cannot be driven by evdev injection
  > (the game reads it over hidraw). `tools/launch-stray.sh` drives the menu with **Enter on the
  > sysrq keyboard node** instead; the game switches its glyphs to keyboard the moment a key
  > arrives (seen in a ReShade screenshot).
* Writing `input_event` structs directly to `/dev/input/eventN` reaches `input_inject_event()` and
  is seen by every reader. No `uinput`, `ydotool` or `evemu` needed. Neither the pad nor the
  keyboard node is `EVIOCGRAB`'d.
* ReShade's screenshot bind is `KeyScreenshot=44` (`VK_SNAPSHOT`) = Linux `KEY_SYSRQ=99`. Injecting
  it on the real keyboard node makes ReShade write a 4K PNG into the game directory. **This is our
  screenshot channel for visual verification.**
* gamescope's `SIGUSR2` screenshot produced no file. `ffmpeg`'s `kmsgrab` cannot read its
  framebuffer (`XB30`, 10-bit HDR).


### Driving the game unattended, rewritten 2026-09-02: the probe, the safe launcher, the bench

The old launcher's recovery paths (SIGKILL of the launch tree, automatic Steam restarts) wedged
the box four times in an hour, its gameplay gate was a render census the plugin host cannot
produce, and an agent then spent an hour blaming the environment. The replacement is four
small composable scripts sharing `tools/stray-lib.sh`, gated on an IN-ENGINE probe:

* **`mods/StrayProbe`** (UE4SS Lua, loads via dwmapi in every configuration) writes
  `stray-game-state.txt` once a second: `pawn` (BP_CatPawn_C valid), `pawnname`, `pc`, `map`
  (`BaseMap` in gameplay, `HK_Project_Intro`/`HK_Project_MainStart` while loading), `paused`
  (`UGameplayStatics::IsGamePaused`), `ingame`. While `stray-probe-bench` exists it also
  writes `stray-frame.txt` at 4 Hz with `UKismetSystemLibrary::GetFrameCount` and the last
  delta. **Threading rules, each measured:** engine reads on the game thread via
  `ExecuteInGameThread`, file I/O on the async thread, expensive lookups cached (a
  `FindFirstOf` per second was a visible spike), `stray-probe-quiet` silences engine queries
  during a window. **The probe is CRLF under Wine** (`io.open` text mode); readers strip `\r`.
* **`tools/launch-stray-safe.sh [--kill] [--no-drive] [--timeout S] [--shot P]`**: names every
  precondition before launching (CEF port, cef-eval helper in the stage dir, sysrq keyboard,
  reaper, which render host is active), shows the launch helper's output, prints progress
  every 10 s, bails the tick a wait can no longer succeed, never restarts Steam, never kills
  without `--kill`. Drives the menu with Enter until the probe says `ingame=1` (~30 s). Every
  exit writes `stray-launch-verdict.txt`.
* **`tools/stray-reload.sh`**: the user's sequence, START(Esc), DOWN, DOWN, ENTER, RIGHT,
  ENTER — the confirm dialog defaults to NO. It first waits for `paused=1` (right after a
  load the intro keeps the menu locked), and the reload is ACCEPTED when the game unpauses by
  itself: **a checkpoint reload keeps the pawn and the map**, so "ingame drops" never fires.
* **`tools/stray-traverse.sh`**: hold UP and alternate LEFT/RIGHT every 3 s for 15 s (the
  user's scenario; `inject.py traverse`), sampling the engine frame counter: avg fps, slowest
  0.25 s bucket, hitch buckets, worst sampled frame, one CSV row in `stray-bench.csv`. Take
  the start frame only after the counter has MOVED — the file's first write carries the
  previous window's value and inflated a run to 277 fps against a true 140.
* **`tools/stray-bench.sh --runs N --label L`**: N × (reload + traverse) in ONE process. A
  death anywhere is the stability verdict; the surviving windows are the perf result.
  **The scenario is the user's own recording** (`tools/data/stray-recording-user1.txt`,
  staged into the game dir; `tools/stray-record.sh` made it, `inject.py replay` plays it):
  301 events, 16.9 s, Up/Left/Right/Shift. Its ~1.6 s lead-in of Left held before Up turns
  the cat and MUST stay — trimming it changed the path — and so must the 3 s settle after a
  reload. `--synthetic` restores the Up + Left/Right pattern. The pad is not replayable
  this way (the game reads the DualSense over hidraw via the shim, not evdev).
* **`tools/screenshot-gamescope.sh`**: `gamescopectl screenshot`, no ReShade needed.

**Measured with it, 2026-09-02:** the "reload checkpoint" crash (`EXCEPTION_ACCESS_VIOLATION
reading 0x270`) was **StrayFur**, not DLSS, ReShade or the DualSense mod — bisected two cycles
per arm with no render host; fixed by moving its `ExecuteWithDelay`/`LoopAsync` engine calls
onto the game thread. **Baseline with no render host** (StrayTriggers off, fur on): avg
137–149 fps, slowest bucket 99–114 fps, 0 hitch buckets, worst sampled frame 11–24 ms. The
DLSS arms (~53–57 fps) are measured against that by the identical instrument.
**Traps:** `pgrep -f "AppId=1332010"` self-matches the shell running it (use `13320[1]0`);
a launch sequence sent 2 s after reaching gameplay does nothing; Config A needs ReShade's
`dxgi.dll` renamed aside, never `dxgi=b` (wine-builtin dxgi cannot create a swapchain on
vkd3d-proton); `WINEDLLOVERRIDES` must keep `dwmapi=n,b` or UE4SS never loads.
---

## 3. How the add-on works

```
                    ┌────────────────── ReShade add-on events ──────────────────┐
 game frame ─▶ init_pipeline ──▶ fnv1a64 the DXBC ──▶ 0x1708ec956099e259?
                                                      + binding signature + dispatch size
                                                              │ yes
               bind_pipeline ───────────────────────────────── ┘   (stage is `all`, NOT compute!)
               push_descriptors / bind_descriptor_tables
                          └─▶ capture by REGISTER: t0 depth, t2 velocity,
                              t5/t6 colour, u0/u1 output, b1 View CB
               dispatch ──▶ INTERCEPT
                             ├─ map b1, copy the 2448-byte prefix, read rows
                             │    122 ClipToPrevClip · 129/130 rect · 132 buffer size
                             │    135.y PreExposure · 145.x CameraCut · 152 jitter
                             ├─ identify history vs scene colour via last frame's u0 pointer
                             ├─ InReset = CameraCut | jitter.zw==xy | 1x1 history/velocity
                             ├─ our MV-resolve CS → dense RG16_FLOAT at render res:
                             │     EncodedVelocity.x > 0 ? decode : camera motion from
                             │     depth + ClipToPrevClip;  then * (0.5W, -0.5H), negated
                             ├─ barrier inputs → NON_PIXEL_SHADER_RESOURCE, output → UAV
                             ├─ NGX EvaluateFeature(colour, depth, denseMV, jitter) → u0
                             ├─ RESTORE clobbered D3D12 state (heaps, root sig, PSO, ...)
                             ├─ produce u1 (half-res filter of u0) — required!
                             └─ return true  (skip the engine's dispatch)
                             │
               u0 becomes next frame's HistoryBuffer[0] ──▶ engine continues
```

Four stages, each testable in isolation as far as CI allows:

1. **Identify** — hash every compute shader's DXBC at `init_pipeline`; confirm with binding
   signature and dispatch size; never select `0x901e041a7cadc9db`; never hook `0x52101a15e1a0c5cc`.
2. **Capture** — record bound SRVs/UAVs/CB **by register** and read the View CB rows.
3. **Resolve** — our compute pass turning sparse velocity + depth + `ClipToPrevClip` into the dense
   `RG16_FLOAT` field DLSS requires, in DLSS's units and sign.
4. **Evaluate** — NGX into `u0`, restore state, produce `u1`, skip the engine dispatch.

### Staging

* **v0.1 — DLAA.** Render resolution == output resolution. No screen-percentage forcing. Isolates
  one question: *are colour, depth, motion vectors and jitter correct?*
* **v0.2 — DLSS SR.** See §4. Do not start v0.2 before v0.1 is confirmed correct **on the user's
  machine**.

---

## 4. The super-resolution path

**[derived]** The correct way is not to fight the engine. Set in `Engine.ini [SystemSettings]`:

```ini
r.ScreenPercentage=<N>
r.TemporalAA.Upsampling=1
```

Then `PrimaryScreenPercentageMethod` becomes `TemporalUpscale`, TemporalAA.cpp selects
`ETAAPassConfig::MainUpsampling`, `InputViewRect` is render res while `OutputViewRect` is display
res, and the engine allocates a **full-res output texture** and expects the TAA pass to upscale —
**exactly DLSS's contract**. Every downstream pass already uses the upscaled rect.

Three things come free: **plain Halton jitter** in `[-0.5, 0.5]` (what DLSS was trained on, versus
the Box-Muller-warped sequence in the shipped mode); **auto-scaled phase count** reproducing
NVIDIA's `Base × (Target/Render)²`; and the **correct texture mip bias**.

**The cost: the permutation and therefore the DXBC hash change.** `0x1708ec956099e259` will no
longer appear. Plan to re-derive it or match structurally.

Hard constraints:

* **`r.TemporalAA.Algorithm` must stay 0.** Gen5 replaces the single dispatch with a chain of six
  shaders and breaks the hook entirely.
* `r.TemporalAA.HistoryScreenPercentage` must stay 100 — above that switches to `MainSuperSampling`.
* Raise `r.TemporalAASamples` for SR modes; the default 8 is enough for DLAA but Performance needs
  32.
* **Never disable TAA via `r.DefaultFeature.AntiAliasing`.** NVIDIA's guide §8.1.3 is explicit:
  replace the TAA pass, but everything the engine does *because* TAA is on — jitter, velocity
  generation — must keep happening.

---

## 5. Domain quick-reference

The full detail is in `docs/RESEARCH.md`. These are the things that bite.

### The screenshot channel, and how to use it without fooling yourself

Everything else we measure says the pass *runs*; only a picture says it is *right*.
`tools/screenshot-stray.sh` injects `KEY_SYSRQ` on the keyboard node whose `Handlers` include
**`sysrq`** — a Power Button, a PC Speaker and a USB audio device all advertise `kbd`, and
injecting into those reaches nothing — and ReShade writes a PNG into the game directory.

**This scene animates, so a single frame proves nothing in either direction.** Measured
2026-08-31 in the starting apartment, strongly-cyan pixels in the dark left third
(`magick … -crop 900x1440+0+0 -fx "(g>0.6 && b>0.6 && r<0.35)"`), five frames per mode:

| Mode | samples | median | max |
|---|---|---|---|
| `MvDispatch=0` (control) | 2134, 0, 3, 503, 1879 | 503 | 2134 |
| `MvDispatch=2` | 1140, 982, 0, 0, 72 | 72 | 1140 |

**No evidence that interception corrupts the image**; if anything the intercepted frames scored
lower. Something in that room flickers cyan by itself, and the range is 0 → 2134 *in the control
alone*.

The trap is worth remembering because it caught this project twice in one session: one clean
mode-2 frame produced a premature "the image is correct", and then one low control sample (48)
produced an equally premature "it still corrupts". Both were single draws from a distribution
spanning three orders of magnitude. **Take n ≥ 5 per mode and compare distributions**, and pick a
crop the control leaves dark — a whole-frame count is swamped by the animated CRT wall and shows
nothing at all.

### Two descriptor hazards that cost a day, both measured

**1. Never `CopyDescriptorsSimple` out of the game's bound heap.** UE4 binds a shader-visible
heap — measured `type=0, NumDescriptors=500000, SHADER_VISIBLE=YES` — and D3D12 forbids a
shader-visible copy SOURCE: *"D3D12 ERROR #654: SrcDescriptorRangeStart points to a descriptor
heap type that is CPU write only, so reading it (in this case a copy source) is invalid"*
(reproduced in our own CI). vkd3d-proton has no debug layer to object, so the illegal copy
silently writes a descriptor the GPU rejects **only when something reads it** — which is why a
bind-only mode survives and a single 1×1 dispatch dies with `Xid 109 CTX SWITCH TIMEOUT`.

**2. ReShade's `view → resource` map outlives the resource on D3D12.** It never calls
`destroy_resource_view`, so a descriptor slot UE4 has recycled still maps to the **destroyed**
resource. Building an SRV from one faults inside the driver —
`err:vulkan:vkCreateImageView Exception 0xc0000005` — and takes the game with it.
`GetDesc()` does **not** protect you: it reads freed-but-still-mapped memory and returns
entirely plausible values (`2560×1440`, `R32G8X24_TYPELESS`). UE4 rotates these buffers
constantly, so it happens within seconds of gameplay.

Track liveness yourself from `init_resource` / `destroy_resource` and refuse anything not known
live (`is_resource_live`, `src/frame_state.hpp`). Keep a set of LIVE resources, never of dead
ones: D3D12 reuses addresses, so a freed pointer can come back as a different valid resource.
Measured working: `depth=…53465B60 live=0` was skipped and the game survived.

**Together these are a vice.** Copying descriptors is illegal; recreating views needs a resource
pointer ReShade cannot be trusted for. Only liveness-checked view creation satisfies both.

**3. ReShade's descriptor tracking follows a re-created VIEW, not the copied bytes (measured
2026-09-02, `docs/STRAY-RENDERING-FACTS.md` §16).** Its per-slot record for an online
descriptor is the source view HANDLE, resolved to a resource at query time; UE4 recycles its
offline descriptor slots constantly, so once the game re-creates that offline view for another
resource, ReShade reports the NEW resource for an online slot whose bytes still hold the copy.
137 811 slots in one 240 s session, every one convicted with the write sequence on the line.
D3D12 copies descriptors by value; a shadow that does the same (the native backend's) is right
and the oracle is wrong. Also measured: ReShade's `init_resource`/`destroy_resource` events
never carried the swapchain's back buffers, so a liveness set fed by them calls the back buffers
dead. **The native backend (`[STRAYDLSS] NativeMode=drive`) is the one that survives all three
hazards; the ReShade backend remains the default only until Stage 4.**

### ReShade 6.8 add-on API

* **Pin headers to tag `v6.8.0`.** `RESHADE_API_VERSION` is **20**; ReShade rejects anything newer
  than its own. Vendor the 8 headers; do not track `main`.
* **Do not define `RESHADE_API_LIBRARY`** — header-only is the supported path.
* **imgui is pinned to v1.92.5 (`IMGUI_VERSION_NUM == 19250`)**, enforced by `#error`. Headers only.
  Include `imgui.h` *before* `reshade.hpp`. A mismatch makes `register_addon` return **false** and
  the add-on never loads — log to a file *before* `register_addon` so this is diagnosable.
* **`reshade::log_message` no longer exists.** Use
  `reshade::log::message(reshade::log::level::info, buf)`. It takes no printf args.
* **In D3D12 `bind_pipeline` fires with `pipeline_stage::all`, NOT `compute_shader`.** Filtering on
  the compute stage silently misses every event.
* **`dispatch` is the only skip-capable event on our path** — return `true` to suppress. Everything
  else is `void`.
* **`push_descriptors`, `bind_descriptor_tables`, `bind_pipeline`, `create_pipeline` require
  `RESHADE_ADDON >= 2`** — the full add-on build. Assert at startup that we saw one in the first N
  frames, or "wrong ReShade build" is indistinguishable from "UE4 binds differently".
* **Registering the pipeline events has a side effect**: ReShade redirects all PSO creation through
  `ID3D12Device2::CreatePipelineState` and **drops the cached-PSO blob**. Expect first-run hitching
  under vkd3d. If it regresses, identify structurally and drop those events.
* `get_native()` returns **`uint64_t`** — reinterpret_cast it. All handles are
  `struct { uint64_t handle; }`.
* Track the game's resources **by descriptor register**, never by pointer alone (§2.9).
* Read the View CB **inside the `dispatch` callback**, never deferred — the upload ring has not
  advanced yet at recording time. Use `map_buffer_region(..., map_access::read_only, ...)`.
* **Copy `examples/utils/descriptor_tracking.{hpp,cpp}` and `state_tracking.cpp` verbatim.** Register
  descriptor tracking from the very first frame — attach late and there is no retroactive recovery.
* Register both a `nullptr` (settings) overlay and an `"OSD"` overlay. The OSD is the fastest remote
  diagnostic when the user can only send a screenshot.

### DLSS / NGX

* **Feature flags for Stray: `IsHDR | MVLowRes | DepthInverted | AutoExposure` = `0x4B`.** Never set
  `DoSharpening` (deprecated, does nothing).
* **Motion vectors: `RG16_FLOAT`, render-resolution pixels, [0,0] upper-left, pointing BACKWARD.**
  `MV_pixels = (PrevScreen - ThisScreen) * (0.5·W, -0.5·H)`, `InMVScaleX/Y = (1,1)`. Guard with
  `PrevClipPos.w > 0`. This is NVIDIA's own `VelocityCombine.usf` math — copy it, don't invent it.
* **Jitter: pass `TemporalAAParams.zw` straight through. No sign flip.**
* **Presets: only `0, J=10, K=11, L=12, M=13` are valid** (A–D removed, E/F deprecated). Use **K**.
  Set all five hint keys **before** `CreateFeature`; setting them after has no effect.
* **NGX clobbers D3D12 command-list state.** We must save and restore descriptor heaps, root
  signature, PSO, root params, topology, viewports and RTVs. ReShade does not do this for us. This
  is the number-one corruption risk.
* Inputs must be `NON_PIXEL_SHADER_RESOURCE`, output must be UAV with `ALLOW_UNORDERED_ACCESS`.
  Missing that flag can produce a **black output with no error**.
* Init with `Init_with_ProjectID` and our own GUID — **no NVIDIA whitelist needed**. Pass a
  `LoggingCallback` that forwards into `reshade::log::message`.
* Derive the quality mode from `NGX_DLSS_GET_OPTIMAL_SETTINGS` rather than choosing one; retry a
  rejected create with `Balanced` + `Preset_Default`.
* Results are a **bitmask** over `0xBAD00000` — use `NVSDK_NGX_SUCCEED`/`NVSDK_NGX_FAILED`, never
  `== Success`.
* Teardown on one thread, GPU idle first: `ReleaseFeature` → `DestroyParameters` → `Shutdown1`.
  **NGX is not thread-safe.** It holds no references to our resources — we must.

### UE 4.27

* The shader is `TAAStandalone.usf` / `MainCS`.
* `ClipToPrevClip` is built from **jitter-free (NoAA)** matrices, is **row-major**, and needs the
  **row-vector** convention (`mul(v, M)`). Transpose if you rebuild it in a column-vector library.
* Scene colour entering TAA **carries pre-exposure**; write our output back **still pre-exposed**.
* `View.ViewRectMin` is almost always (0,0) after UE4 shifts view rects to the buffer top-left —
  **but read it, do not assume it**.

---

### The denoiser-suppression experiment: DLSS SR does NOT replace the SSD (measured 2026-08-31)

Suppressing all nine cooked `FSSDTemporalAccumulationCS` permutations while DLSS SR ran,
alternating every 900 frames within one gameplay session (The Slums, camera still after one
pan, 32 captures classified by the ALT PHASE log):

```
SUPPRESSED: n=14  median=3599  (min 1078, max 6079)
normal    : n=18  median=1051  (min  914, max 8942)
```

Median frame-to-frame shimmer is **3.4x higher with the denoiser off** — the suppressed
group's MINIMUM sits at the normal group's median. DLSS SR's temporal accumulation does not
absorb the screen-space denoiser's job; SSR/SSGI noise shimmers straight through. Consequence:
"suppress the denoiser and let SR handle it" is dead, and DLSS Ray Reconstruction is the only
candidate for replacing UE's denoiser — with a quantified 3.4x stability gap as its target.

### r.RayTracing=True is the single biggest problem in this game — 3x the frame rate AND the noise (measured 2026-08-31)

**Setting `r.RayTracing=False` in `Engine.ini [SystemSettings]` nearly TRIPLED the frame rate and
eliminated the shimmer artifact we had spent a whole session chasing.** Measured from our own
frame checkpoints, same scene, same save, DLSS RR active in both:

```
                          RT ON      RT OFF     gain
menu      (frames 300-1200)   99.7 fps   131.6 fps   1.3x
load+play (frames 1200-3600)  32.4 fps    95.4 fps   2.9x
```

**Why.** Stray ships `r.RayTracing=True` and the game is launched with `-dx12`, so UE 4.27 builds
ray-tracing acceleration structures (BLAS/TLAS) for scene geometry **every frame** — skinned
meshes included — whether or not any RT effect consumes them. The cost scales with scene
complexity, which is exactly why an almost-empty menu ran at ~100 fps while a dense street ran at
30. Meanwhile the game's world lighting is **baked**, so the RT effects that did run bought
almost nothing visually.

**The artifact was ray-traced noise.** The cyan/red "blips" on neon sources — chased through the
exposure path, both RR-1 variants, and the B/C guide order, all fruitlessly — were RT noise.
Turning the subsystem off removed them at the source.

**This retroactively explains the RR-vs-SR observation.** DLSS SR looked noisier than DLSS RR on
this content because the noise really was ray-traced, and Ray Reconstruction is purpose-built to
denoise exactly that. RR was doing its job; the noise simply should not have been there.

**Operational rules:**

* **Default `r.RayTracing=False` for this title.** `-dx12` is still required (D3D12 is what NGX
  needs), and **DLSS SR does not need RT** — SR is the configuration to ship.
* **RR WITHOUT RT IS THE UNTESTED COMBINATION, AND IT IS THE INTERESTING ONE.**

> **CORRECTED 2026-09-01.** This bullet previously asserted "RR REQUIRES `r.RayTracing=True`,
> so RR is not independently usable here" and treated it as concluded. **That was never
> verified.** It came from a single passing remark, not a measurement or a source, and it was
> then repeated as settled — the exact failure mode the HARD/SOFT discipline exists to prevent.
>
> **Nothing requires it.** `NVSDK_NGX_DLSSD_Create_Params` carries a denoise mode, a roughness
> mode and a depth type, and no ray-tracing anything. Our own RR path takes its guides from the
> **base-pass G-buffer** (SceneColor + GBufferA/B/C/D/E, `src/gbuffer_finder.cpp`), which is
> UE4's ordinary deferred output and is present with RT fully off. `grep -rn RayTracing src/`
> finds nothing that gates it.

**So the combination worth testing is `NgxRR=1` with `r.RayTracing=False`**, and the case for it
is strong: RR is the only candidate for replacing UE's screen-space denoiser (SR provably cannot
— the 3.4x shimmer gap below), the SSD runs with RT off anyway because it denoises SSR/SSGI/SSAO
rather than ray tracing specifically, and keeping RT off preserves the 2.9x frame-time win. RR
also does the upscaling, so it replaces SR rather than adding to it.

The two-phase experiment, with a pre-registered metric:

1. **RR on, RT off, SSD untouched.** Does it create, evaluate and look right? This is a config
   flip — the guides and the DLSSD path are already built and were measured running at ~100% of
   gameplay frames.
2. **RR on, RT off, SSD suppressed.** Does RR cover the denoiser's job? The target is the
   measured 3.4x: suppressed-SSD shimmer must come back down toward the ~1051 median rather than
   the ~3599 it reached under SR.
* When triaging *any* performance or noise report here, check the RT subsystem FIRST. It is
  scene-dependent, so a menu benchmark will hide it completely.
* **A visual artifact and a performance problem sharing one root cause is not a coincidence to
  dismiss.** The user's hunch that they were linked was correct and was what cracked it.

### Measure frame rate from the addon's own frame checkpoints

`[frame N]` log lines carry timestamps, so average fps over any span is
`(N2 - N1) / (t2 - t1)` with no extra tooling — that is how the table above was produced.
`src/perf.{hpp,cpp}` (`[STRAYDLSS] PerfLog`, default ON) now reports this every 600 presents
along with the worst frame time and our own CPU share by bucket. **Two traps:** the
frames-1200-to-3600 span includes the loading screen, so it understates steady-state gameplay;
and screenshot bursts spaced ~15 s apart alias any periodic effect — a "3-state lighting cycle"
measured that way turned out to be the camera rotating between captures. Sample faster than the
phenomenon and hold the camera still.

### DLAA (1:1) is NOT blocked by the shader — the old "different bindings" assumption is wrong

Measured offline 2026-08-31 by scanning **all 27 cooked FTAAStandaloneCS permutations** extracted
from the pak (`tools/shaderlib_extract.py --dump-dir`, then a DXBC declaration scan). They split
by `dcl_tgsm_structured`:

* **15** with the tgsm array — what 4K/50% selects, what we hook today.
* **12** without it — the other half of the family.

**Every one declares the SAME register shape: `SRVs t0-t5`, `UAV u0`.** Only two outliers exist
(`2d67549bedef2c0a` with t0-t4, and `876f85dfc23213d0` with t0-t3) — camera-cut/edge variants,
not the main pass.

So §2.3's earlier claim that 1:1 "selects a different permutation whose bindings we never
characterised" is **wrong at the shader level**: depth, stencil, velocity, colour and history sit
in the same slots. There is no shader-declaration reason DLAA cannot work, and the 27-hash table
already contains every one of them.

> **CORRECTED 2026-08-31 against the UE 4.27.2 source** (public mirror
> `AlexMercer-MA/UnrealEngine-4.27` @ `306a7e9`, `Build.version` reads 4.27.2 / `++UE4+Release-4.27`).
> The split above is **not** upsampling vs non-upsampling, and 1:1 does **not** select
> `ETAAPassConfig::Main`. `PrimaryScreenPercentageMethod` is set in `SceneView.cpp:837-841` on
> `AntiAliasingMethod == AAM_TemporalAA && r.TemporalAA.Upsampling != 0` — **the resolution
> fraction is never consulted**, and the only downgrade in
> `PrepareViewRectsForRendering` (`SceneRendering.cpp:2546`) gates on the AA method and the
> VisualizeBuffer showflag, nothing else. Stray ships `r.TemporalAA.Upsampling=True`, so the
> game runs **`MainUpsampling` at every screen percentage, 100% included**. What changes at 1:1
> is only `FTAAScreenPercentageDim` (`TemporalAA.cpp:726-750`): **1 below 71%, 0 at 1:1** — a
> different permutation of the same config, hence a different DXBC and a different hash.

**Confirmed not working live** (2026-08-31): at 100% screen percentage no DLSS feature is ever
created — only `1920x1080 -> 3840x2160` (50%) and `2688x1512 -> 3840x2160` (70%) appear in the
log, and the NVIDIA DLSS indicator is absent at 100% while present at 50% and 70%.

### Why 1:1 is fragile and <100% is not: `GetOutputExtent()`'s `Max()` (UE 4.27 source)

The dispatch and the output TEXTURE are **two different quantities**, and the engine ties them
together only when it is really upscaling:

```cpp
// TemporalAA.cpp:950 — the DISPATCH
GetGroupCount(PracticableDestRect.Size(), GTemporalAATileSizeX)   // = the OUTPUT VIEW RECT

// TemporalAA.cpp:596 — the TEXTURE
FIntPoint FTAAPassParameters::GetOutputExtent() const {
    FIntPoint InputExtent = SceneColorInput->Desc.Extent;
    if (!IsTAAUpsamplingConfig(Pass)) return InputExtent;
    ...
    return FIntPoint(Max(InputExtent.X, QuantizedPrimaryUpscaleViewSize.X),
                     Max(InputExtent.Y, QuantizedPrimaryUpscaleViewSize.Y));
}
```

**While the pass really upscales**, `InputExtent` is the smaller render buffer, so the `Max()`
returns exactly `Quantize(OutputViewRect)` — the dispatched rect. An equality test between the
dispatch and the output texture is then *guaranteed* to hold, which is why 50% and 70% always
matched, and why the `Max()` also **absorbs** a scene buffer left oversized from an earlier
resolution.

**At 1:1 that term degenerates.** `Quantize(OutputViewRect)` can no longer exceed `InputExtent`
(`SetupViewRect`: `OutputViewRect = InputViewRect` for non-upscaling, and `Validate()` asserts
it), so `GetOutputExtent()` returns the scene-colour buffer's own extent and **nothing relates
it to the dispatch**. Any scene buffer larger than the current view rect — UE4 shrinks the scene
targets lazily, and this session has been observed at both 3840x2160 and 2560x1440 — flows
straight into the output extent while the dispatch stays at the view rect, and an equality test
rejects the real pass. Below 100% the identical inflation is invisible.

**Fixed 2026-08-31** in `match_taa_dispatch`: the equality is now demanded only in the upscaling
branch (where the engine guarantees it, so 50%/70% are byte-identical). Otherwise the dispatch
must merely **fit inside** the output UAV and **cover at least the view rect**. `output_width`/
`output_height` now report the rect the dispatch WRITES (`group count x 8`, clamped to the UAV),
not the allocation — DLSS must be created for the rect, never the texture. Regression tests in
`tests/test_taa_signature.cpp` ("DLAA: ...", "200% ... is still refused").

**Also fixed: the DLSS pin could outlive its pass, silently.** `g_ngx_pass_hash` pins the first
pass that evaluates and refuses every other one — but the pinned value is a *permutation* hash,
and the permutation changes with the screen percentage (above). A pin taken at one ratio then
refused the real pass at every other ratio for the rest of the session, and logged nothing.
`taa_hook.cpp` now releases a pin whose pass has not dispatched for 300 presents, and every
gate that used to refuse a matched TAA pass in silence — unknown hash, unreadable View CB, dead
inputs, pinned elsewhere, no round-trip yet — emits one WARN per pass per reason
(`log_gate_refusal`). **A matched pass that never reaches DLSS must never again be
indistinguishable from a pass that was not there.**

**Also measured: 200% can never work.** It renders 7680x4320 and downsamples to 3840x2160; DLSS
upscales by definition and cannot accept an input larger than its output. The matcher still
rejects it, now on the view-rect lower bound ("dispatch covers less than the view rect -
downsampling, not TAA upscaling"). 70% is the highest working setting and is sharper than 50%.

### OptiScaler frame generation: the hook-up WORKS, the FG swapchain does not (measured 2026-09-01)

**The inversion is real and is now measured.** OptiScaler only replaces upscalers in games that
already call DLSS/FSR2+/XeSS — Stray calls none, which is why this project exists. But **we** call
DLSS, and OptiScaler's `LoadLibrary` hook is process-wide and suffix-matched, so it intercepts
OUR NGX calls. Its log prints our own project GUID:

```
NVSDK_NGX_D3D12_Init_ProjectID InProjectId: 6f2d1c84-9b3e-4a17-8e55-1d0c7a3f6b92
NVSDK_NGX_D3D12_CreateFeature Creating new dlss upscaler
DLSSG_Dx12::SetResource SetTagForFrame type: Velocity result: eOk (0)
DLSSG_Dx12::SetResource SetTagForFrame type: Depth    result: eOk (0)
DLSSG_Dx12::Dispatch Result: Ok
```

So `FGInput=upscaler` harvests the depth, velocity and jitter our TAA hook already computes, and
real DLSS Frame Generation dispatches against them. **The earlier "OptiScaler cannot help" note
was half wrong and is retracted**: the "only works in games that already call DLSS" objection is
answered by us being the caller.

**What actually blocks it: the game dies at a fixed point in the FG pipeline.** Deterministic,
six launches:

| Config | Died at |
|---|---|
| FSR-FG output | FG frame **11**, right after `numGeneratedFrames: 1` |
| DLSSG output | FG frame **32** |
| DLSSG + fakenvapi Reflex | FG frame **36** |
| DLSSG + fakenvapi + **our NgxNR=0** | FG frame **36**, identical |

UE4 reports `EXCEPTION_ACCESS_VIOLATION writing address 0x000000020000000d` — a bit-packed
handle-shaped value, the same signature section 1 documents for a descriptor handle dereferenced
as a pointer. It survives the swapchain `ResizeBuffers` (result 0) and dies about a second after
`SetFullscreenState Fullscreen: 1`.

**Ruled OUT, so do not re-test:** load order (see below), OptiScaler's Vulkan overlay
(`OverlayMenu=false` changed nothing), Reflex (fakenvapi fixed `setReflexTiming ... status -3`
and moved the death by 4 frames, not past it), and **our own NR path** — `NgxNR=0` died at the
identical frame, so feature 18 writing into `u0` is not the cause.

**Solved along the way, and worth keeping:**

* **Coexistence with ReShade is a solved problem, and the answer is OptiScaler's own wiki.**
  Install OptiScaler AS `dxgi.dll`, copy ReShade's DLL to **`ReShade64.dll`** beside it, set
  `LoadReshade=true`. Two independent DXGI proxies racing is wrong; OptiScaler must load ReShade.
  Verified: `CheckWorkingMode Loading ReShade64.dll` then `hkD3D12CreateDevice Caller:
  ReShade64.dll`. **With FG off this configuration reaches gameplay and is stable** — so the
  coexistence is fine and only frame generation breaks.
* **No published OptiScaler build can do `FGOutput=dlssg`.** v0.9.4 and the newest nightly
  (v0.9.5-pre3 / `d12f554`, 2026-08-22) both end `CheckForFGStatus` with
  `if (activeFgOutput != FSRFG && != XeFG) { LOG_WARN("FGOutput is not set to FSR-FG or XeFG"); return false; }`
  — verbatim the warning our first run produced. It exists only on master.
  `.github/workflows/build-optiscaler.yml` builds it and **fails loudly if handed a ref that
  still contains that string**, so a stale ref cannot quietly produce a binary that refuses dlssg.
* **`FGOutput=dlssg` genuinely requires Streamline** (`DLSSG_Dx12.cpp` hard-errors without
  `sl.interposer.dll`). SL 2.13 in `OptiScaler/streamline/` + `nvngx_dlssg.dll` works: loads
  clean, `arch=0x190` Ada, driver 610.43.
* **DXVK-NVAPI's Reflex is not enough for DLSS-G.** `setReflexTiming NvAPI call failed with
  status -3` every frame until `fakenvapi.dll` + `force_reflex=2` was added.
* **The upscaler silently fell back to FSR2** under master (`FSR2FeatureDx12_212::EvaluateInternal
  Dispatch!!`) despite `Dx12Upscaler=dlss`, with `Config::CheckUpscalerFiles nvngx.dll not found!`
  — but the older nightly used DLSS with the same warning, so the fallback is not just that file.
  **Consequence worth remembering: while OptiScaler owns the upscaler, our carefully built NGX
  parameter block can be replaced by FSR2 without anything failing.**
* **GE-Proton 11-1+ ships OptiScaler behind `PROTON_USE_OPTISCALER=1`**, a cleaner packaging than
  hand-installing. The target's own `GE-Proton-dxvk301-ds5-clean-nowl` has the env-var CHECK but
  **no OptiScaler binary**, so the flag is inert there.

**Status: dropped by the user 2026-09-01, box fully reverted to ReShade-as-dxgi.dll.** Everything
above is preserved because the hard part — proving OptiScaler can drive FG from our DLSS hooks —
is done and measured; only the FG swapchain remains.

### DLSS Neural Rendering (feature 18): the identity check, solved (measured 2026-08-31)

Two independent walls stand between a ReShade add-on and NGX feature 18. Both are now
characterised, and the second is the interesting one.

**Wall 1 — the NGX core will not route feature 18.** Asking `nvngx.dll` for it returns
`FAIL_OutOfDate` no matter how the snippet is staged, because the core resolves only
driver-shipped snippets and this runtime is a leaked pre-release build. The way through is to
**drive the snippet through its own `NVSDK_NGX_D3D12_*` exports** and bypass the core entirely.
Only four are required — `Init_Ext`, `CreateFeature`, `EvaluateFeature`, `ReleaseFeature`; the
parameter-block exports are absent from the snippet and must come from the core.

**Wall 2 — the snippet checks who loaded it, and the test is a SUBSTRING.** It calls
`GetModuleFileNameW` on its caller and requires the answer to **contain the substring
`nvngx.dll`**. Measured, by patching the snippet's own `GetModuleFileNameW` import (IAT patch)
and answering differently:

| Reported identity | Contains `nvngx.dll`? | Snippet behaviour |
|---|---|---|
| `stray-dlss.addon64` (pass-through) | no | **one** query, then immediate `FAIL_PlatformError` |
| `…\nvngx_dlssnr.dll` (its own path) | **no** — `nvngx_dlssnr.dll` does not contain it | rejected |
| `…\nvngx.dll` | yes | **three** queries, interrogates a **second** module, no error |

That is also why RTX Remix ships its shim as **`remix_nvngx.dll`** — the name contains the
substring. `NgxNRIdentity=nvngx` is therefore the default; the other modes exist only to
reproduce the measurement.

**Load and initialise are separate states, and initialising at device creation stalls the
game.** RenoDX's own strings say the runtime is *"pre-loaded at device init"* and initialised
*"lazily on first evaluate"* — two states, not one. We had collapsed them into a single eager
call inside `on_init_device`, and the measured consequence is unambiguous: the ReShade log
**stops mid-`Init_Ext`**, immediately after `CreateDXGIFactory1`, with neither a success nor an
error line after it. Loading a 165 MB neural DLL is cheap memory work; *initialising* it during
D3D12 device creation is not, and it is the leading suspect for two `GPU_IS_LOST` events that
each needed a host power-cycle.

So: `LoadLibrary` + export resolution + the IAT patch at device init
(`[STRAYDLSS] NgxNRPreload`, default ON, no GPU contact); `Init_Ext` and the feature create on
first use, behind `[STRAYDLSS] NgxNRWarmupFrames` (default 60 — deliberately more conservative
than RenoDX, whose behaviour is effectively 1; set 0 to match them exactly). `apply()` is only
reached after a successful SR/RR evaluate, so the warmup counts frames in which the device,
queue and swapchain demonstrably worked.

### NR IS A PRESENT STAGE, AND THAT DELETED HALF THE FEATURE (2026-09-03)

**Confirmed working in the game by the user.** DLSS Neural Rendering now runs on our own
present-time command list, over a staging copy of the **back buffer**, and its result is copied
back there. The old site — inside the intercepted TAA compute dispatch, writing the engine's `u0`
— is **gone**, and so is everything that existed only to make that site survivable.

**Three things went with it, and each was expensive to build:**

* **The HDR colour codec** (`nr_codec_pass`, `core/nr_codec`, `nr_encode.hlsl`, `nr_decode.hlsl`).
  Feature 18 is display-referred; the TAA site carried raw unbounded pre-exposed linear HDR, so
  the soft-clip + sRGB proxy + residual transfer WAS that site's input contract. The back buffer
  is `R10G10B10A2_UNORM` with no `SetColorSpace1` (facts §33) — already the network's domain — so
  at Present there is nothing to convert. Keys gone: `NgxNRPaperWhiteScale`, `NgxNRColorStrength`,
  `NgxNRTransferStrength`, `NgxNRTrackExposure`, `NgxNRSmoothExposure`, `NgxNRExposureSmoothing`,
  `NgxNRScaleResetTolerance`. With them go five refusal reasons and one reset source.
* **The history restore** (`nr_history`, `core/nr_history_plan`). It stopped NR's residual
  re-entering the engine's temporal state through `u0`. **Post-tonemap has no feedback path by
  construction** (§5: every `QueueTextureExtraction` into `PrevFrameViewInfo` sits above
  `AddTonemapPass`), so there is no loop left to break. Keys gone: `NgxNRRestoreHistory`,
  `NgxNRRestoreState`.
* **`NgxNRHook` and `NgxNRTopology`.** One site leaves no mode to choose; and the binary audit
  settled the topology question outright — `DLSSNR.ScalingRatio` is read and then unconditionally
  overwritten with `1.0f`, so feature 18 cannot upscale in this runtime and the `sr` shape could
  never have worked.

**What is KEPT, and why it is not an oversight.** The TAA hook still runs: DLSS **SR** lives
there, and it is the only place in the frame where depth and motion vectors are both known-good
and known-fresh. It calls `nrhook::note_guides(...)` every frame to publish them, together with
the camera-cut `reset` (§2.8), for the present stage to consume. **Deleting that call breaks NR
even though nothing in NR's own files mentions it.**

**The general lesson, and it is the one worth carrying to the next feature.** Roughly 4,900 lines
— two compute shaders, a codec with its own CI suite, a history-restore path with a state
constant derived from four UE 4.27 source anchors and zero measurements, five refusal reasons, a
reset source, eleven config keys — existed to make ONE wrong hook point survivable. None of it
was wrong; all of it was work the hook point created. **When a feature keeps growing machinery
to compensate for where it runs, the placement is the bug.** This project spent sessions
diagnosing the codec, the exposure loop and the feedback node as properties of NR, and every one
of them was a property of `u0`.

### The cat is unchanged under NR, and the fur is NOT a motion-vector hole (2026-09-03)

**The observation, the user's:** with NR running as a present stage, *"the rest of the world is
100% perfect"* — and **the cat looks unchanged.** Global controls at the time:
`NgxNRLocalStructure=1.61`, `NgxNRSkinStructure=2`, `NgxNRIntensity=1`, `NgxNRStyle=2`,
`NgxNRAutoMask=1`. Fine fur is exactly the content a detail network should act on, so "the
strength is too low" does not explain it at 1.61.

**Nothing below has run in the game.** Every claim is read out of the UE 4.27.2 source, out of the
fur plugin's source, out of Stray's own shipped executable, or out of **Stray's own cooked
material**. Nothing was written to the box; the game was running throughout and was not touched.

#### The leading hypothesis was that the fur shells write no velocity. It is REFUTED, and every link is HARD.

The cat is drawn by **gFur PRO** (`GiM-GamesInMotion/gFurPro`, branch `4.27` @ `d5238a4` — the
plugin is **open source**; `GFur.uplugin` reads `"FriendlyName": "GFur PRO"`, and Stray's own
material references `/GFur/GFurPRO/gFur/Textures/...`). The proposed mechanism was that ~48 shell
layers write nothing into `GBufferVelocity`, so every fur pixel would fall into
`mv_resolve.hlsl`'s else-branch and be handed the motion of a **static world point at that
depth** — precisely wrong for the one object always moving relative to the camera.

Stray ships `r.BasePassOutputsVelocity=True` (§2.3.1), so the route is the **base pass**, and the
separate velocity pass is correctly excluded for gFur (`VelocityRendering.cpp:417-443`:
`PrimitiveCanHaveVelocity` returns false exactly when the base pass can output velocity, and
gFur's VFs declare `bSupportsStaticLighting = false`). **The base pass has three gates and
nothing else. All three pass:**

| Gate | Where | For the fur | Status |
|---|---|---|---|
| **Blend mode**, at PREPROCESSOR level | `BasePassCommon.ush:52-54` — `WRITES_VELOCITY_TO_GBUFFER` needs `MATERIALBLENDING_SOLID \|\| MATERIALBLENDING_MASKED`, and so does `USES_GBUFFER` (`:41`), which is the other operand of its `\|\|` | **`BLEND_Masked`** | **HARD** — from the game's own asset, below |
| `OutputVelocity > 0` | `BasePassVertexShader.usf:225` / `BasePassPixelShader.usf:985`; fed by `bOutputVelocity \|\| AlwaysHasVelocity()` (`PrimitiveSceneProxy.cpp:385`) | `FurComponent.cpp:42` sets **`bAlwaysHasVelocity = true`**, so it is 1 unconditionally | **HARD** |
| `DrawsVelocity != 0` | `BasePassPixelShader.usf:997` zeroes the result otherwise; `DrawsVelocity()` is `return IsMovable();` (`PrimitiveSceneProxy.h:571-575`) | the cat's fur component is movable | **HARD** |

And the previous position the VS fetches is real, not a stand-in for the current one:
`GFurFactory.ush:722-726` implements `VertexFactoryGetPreviousWorldPosition` → `SkinPreviousPosition`
(`:390-416`), which uses `CalcPreviousBoneMatrix`, `Primitive.PreviousLocalToWorld`, the previous
frame's **fur physics offsets**, and ends `return Position + ResolvedView.PrevPreViewTranslation;`
— the required previous-frame translated-world convention. The bone matrices are explicitly
double-buffered *for this purpose* (`FurSkinData.cpp:218`: *"double buffered bone
positions+orientations to support normal rendering and velocity (new-old position) rendering"*),
bound at `:791-810` as `PreviousBoneMatrices` **and** `PreviousBoneFurOffsets`, with a
discontinuity collapse on LOD change or a skipped frame (`:235-248`).

**Stray's own binary carries all of that machinery**, which is what makes the plugin reading apply
to *this* build rather than to a repo. Exhaustive printable-run extraction of
`Stray-Win64-Shipping.exe` (85 MB, ASCII and UTF-16LE, unanchored) finds
`/Plugin/gFur/Private/GFurFactory.ush`, `/Plugin/gFur/Private/GFurStaticFactory.ush`,
`FFurSkinVertexFactory`, `FPhysicsFurSkinVertexFactory`, `FMorphPhysicsFurSkinVertexFactory`,
`FFurSkinVertexFactoryShaderParameters<Physics>` — and **`PreviousBoneFurOffsets`,
`PreviousFurPosition`, `PreviousFurLinearOffset`, `PreviousFurAngularOffset`** with their
`*Parameter` binding counterparts. Those names exist in a vertex factory for exactly one purpose.
**HARD.**

**The blend mode was the one live gate, and Stray's own pak settles it.** The material
`M_Fur_2sidedshading_backpackON` is a `MaterialInstanceConstant` whose `Parent` is
`M_Base_GFur_2sidedshading`. Both name tables were read (method below), and in **both** the only
`BLEND_*` name present is **`BLEND_Masked`** — alongside `BlendMode`, `EBlendMode` and (on the
instance) `BasePropertyOverrides`. `BLEND_Translucent` appears in neither. The instance also
overrides the shading model to `MSM_SubsurfaceProfile` (parent: `MSM_TwoSidedFoliage`) with
`SubsurfaceProfile = SSS_profil_cat` — both deferred G-buffer shading models, which a translucent
material cannot use, so the asset corroborates itself.

Two-sidedness excludes nothing: `grep IsTwoSided` over `BasePassCommon.ush`,
`BasePassPixelShader.usf`, `BasePassVertexShader.usf` and `BasePassRendering.{h,cpp}` returns
**zero** hits, and every `IsTwoSided()` call site in `Runtime/Renderer/Private` is a cull-mode or
default-material-swap decision. `MSM_TwoSidedFoliage` appears in exactly one define,
`WRITES_CUSTOMDATA_TO_GBUFFER` (`BasePassCommon.ush:44`), and in no velocity define.
`ShouldIncludeMaterialInDefaultOpaquePass` excludes only `IsSky()` and `MSM_SingleLayerWater`.

**So the fur writes velocity, the vectors are right, and the fix is not about motion vectors.**
Recorded at this length because the hypothesis was good, cheap to state, and would have redirected
the whole feature — and because this file has twice paid for the opposite mistake (the
`ClipToPrevClip` transposition, `MVecScale`): *bad motion vectors do not produce one bad frame,
they compound through the accumulation*, so any temporal artefact on one object reads like a
motion-vector bug whether or not it is one. The lesson cuts both ways.

**Three traps found on the way, all worth keeping:**

* **`bPrecisePrevWorldPos` is DEAD METADATA in 4.27.2.** gFur declares it true
  (`IMPLEMENT_VERTEX_FACTORY_TYPE(FFurSkinVertexFactory, "…/GFurFactory.ush", true, false, true,
  **true**, false)`, `FurSkinData.cpp:551-558`, 4th bool), and it is tempting to read that as the
  thing that enables velocity. It is not: `SupportsPrecisePrevWorldPos()` has **zero call sites in
  the entire engine** — only the declaration (`VertexFactory.h:347/385/457`) and the constructor
  store (`VertexFactory.cpp:81/100`). It is evidence of the author's intent and of nothing else.
  The only VF property the velocity code actually reads is `SupportsStaticLighting()`.
* **`BLEND_Masked` does not always compile to `MATERIALBLENDING_MASKED`.**
  `MaterialShared.cpp:1871-1888` emits `MATERIALBLENDING_SOLID` instead when the material
  `WritesEveryPixel()`. Harmless for velocity — both are in the define — but "the shader says
  SOLID" is not evidence the asset is set to Opaque.
* **`FPrimitiveViewRelevance`'s constructor is not a plain memset.** After zeroing itself it sets
  `bOpaque = true` and `bRenderInMainPass = true` (`PrimitiveViewRelevance.h:85-100`, under the
  comment *"only exceptions (bugs we need to fix?)"*). A read that stops at the memset concludes
  gFur's `bVelocityRelevance = IsMovable() && Result.bOpaque && Result.bRenderInMainPass`
  (`FurComponent.cpp:296`) is always false — and that conclusion cost one agent a whole section
  before the fuller quote retired it. Moot here (the separate pass is off), but the shape is the
  lesson: **asserting a negative from a truncated quote is the failure the HARD label exists to
  prevent.**

#### `r.BasePassForceOutputsVelocity=1` is a live, restart-free discriminator — keep it in the kit

`SceneRendering.cpp:330-335`, default 0, flags **`ECVF_RenderThreadSafe` only** — *not*
`ECVF_ReadOnly`, unlike `r.BasePassOutputsVelocity` — so it can be changed live. It reaches the
shader as `View.ForceDrawAllVelocities` (`SceneRendering.cpp:1524`, `SceneView.h:667`) and appears
in no C++ branch at all. **It bypasses exactly two gates** — `OutputVelocity` and `DrawsVelocity`,
at `BasePassVertexShader.usf:225`, `BasePassPixelShader.usf:985` and `:997`. **It cannot bypass
the blend mode**, because `#if WRITES_VELOCITY_TO_GBUFFER` *wraps* all three sites and resolves at
shader-compile time: a translucent permutation contains neither the branch nor the cvar test. So
for any future "does this thing write velocity?" question: flip it, and if the velocity appears
the fault was a per-primitive gate; if it does not, the fault is the blend mode and no cvar will
fix it.

#### Reading a cooked material out of Stray's pak, which is now a solved procedure

The fur materials are Oodle-compressed at the pak level, so `tools/pakextract.py` alone reports
`Error -3 while decompressing`. The full chain, run entirely read-only against the box while the
user was playing (`nice -n 19 ionice -c3`, no writes outside `/tmp`):

```
pakextract.py --raw <pak> rawout 'Cat/Fur/M_Fur_2sidedshading_backpackON\.u'
oodle_unblock.py <entry>.json <entry>.raw out.uasset /tmp/scepad/oozraw
python3 -c 'print sorted printable runs of out.uasset'
```

`/tmp/scepad/oozraw` is an existing ooz build on the box (`usage: oozraw <uncompressed_size> <
block`). **The `.uasset` alone is enough for a property question**: UE4 serialises a
`TEnumAsByte<EBlendMode>` UPROPERTY as a ByteProperty whose *value* is an FName, so the enum
literal (`BLEND_Masked`) lands in the package's name table and the whole 146-name table fits on
one screen — no property-tag walk, no `.uexp`, no CUE4Parse. The same trick reads shading models
(`MSM_*`), material usage flags (`bUsedWith*`) and every parameter name.

#### What is still open, cheapest test first

1. **FRAME GENERATION IS ON, AND IT IS THE CHEAPEST CONFOUND TO REMOVE.** The live session that
   produced the report had `fg_enabled=1`, `fg_generated_presented=6563` against
   `fg_game_presents=6750` — so **roughly half of every frame the user judged was a DLSS-G
   interpolation.** The ordering is right (**HARD**, `present_owner.cpp`: `pc.back_buffer` is the
   FG replacement, `sk->on_present(pc)` runs the NR stage over it, and only then `fg::record`
   generates from `c.replacement[c.mirror.current()]` — the same, already-NR'd resource), so this
   is *not* "NR skips half the frames". But an interpolated frame is a **warp**, and a warp
   preserves fine structure on static content far better than on the fastest-moving thing on
   screen — which is the cat, close to the camera and moving relative to it. **"World perfect, cat
   unchanged" is the exact shape that predicts.** UNCONFIRMED, and it costs one config flip:
   **judge NR with `NgxFG=0` before building anything.**
2. **A shell-fur pixel's guides describe one shell; its colour is a composite of several.** 48
   alpha-masked layers write one depth and one velocity sample per pixel — the frontmost shell
   that survived the mask — while the visible colour accumulates whatever showed through the
   layers behind it. A temporal network reprojecting with those guides fetches history for a
   surface that is only partly the pixel it is correcting, and over fur the depth guide is a dense
   field of shell-to-shell discontinuities rather than a surface. That is a structural
   shell-fur/temporal mismatch of the same family as "temporal network + screen-space
   reflections", and — like that one — **it is not fixable from the velocity side, because the
   velocity is correct.** UNCONFIRMED, and there is no cheap test for it.
3. **The network may simply not read fur as structure.** The strengths are not multipliers on an
   output: the binary audit ("DLSSNR's structure controls") found they are broadcast
   bit-identically into the input tile as five of sixteen channels, so they tell the network *how
   much* local structure to apply and the trained weights decide *where* structure exists. Fur may
   read as noise. **Probe this with `NgxNRIntensity` and nothing else** — it is the only strength
   knob NOT in `CG2R_ResetTemporalHistoryOnControlChange`, so it is the only one that can be moved
   without wiping the accumulation; every structure slider holds `Reset = 1` while it is dragged
   and changes the whole screen for reasons unrelated to its meaning.

#### The instrument: `NgxDumpInputs` now dumps the engine's own velocity, and `NgxDumpAt` moves it

Built, CI-green, **never fired**. `mv_resolve.hlsl` writes a plausible vector for every pixel —
decoded object motion where `EncodedVelocity.x > 0`, reconstructed camera motion everywhere else
— so **the branch a pixel took is invisible in the resolved field**, and a pixel handed the wrong
branch looks exactly like a pixel handed the right one. The engine's raw `R16G16B16A16_UNORM`
buffer is the only resource in the frame that records which pixels UE4 actually wrote a velocity
for. It is now captured as `straydlss_velocity_raw_<n>.bin` beside the colour/depth/output dumps,
with our resolved `RG16_FLOAT` field as `straydlss_mv_<n>.bin`.

`tools/rawdump2png.py` grows two formats. `rgba16unorm` writes **two** images: the decoded object
velocity, and `<stem>_mask.png` — UE's own strict `EncodedVelocity.x > 0` test, white where the
engine wrote a velocity — plus the coverage percentage. Open the mask beside the colour dump of
the same evaluate. **The refutation above predicts a WHITE cat.** A black one would overturn a
chain every link of which is HARD, including the game's own asset, and would be the finding of the
session. `rg16f` renders the resolved field's magnitude and prints its component ranges.

**`[STRAYDLSS] NgxDumpAt` moves the capture points** (default 0 = the shipped 600/900; a
configured value is the first point and the second follows 300 evaluates later; pure and pinned in
`src/core/dump_plan.hpp`). The shipped points were chosen for a session that reached gameplay
quickly. `NR CODEC LUMINANCE` has already been wasted once by firing on a black loading frame, and
a dump costs a whole round trip — set it past the loading screen.

#### `DLSSNR.ControlMask` — reachable, and not yet worth it

The idea: tag the cat with UE4's custom depth/stencil (`bRenderCustomDepth` on the pawn's
components, which **StrayFur already reaches by reflection**, plus `r.CustomDepth=3`), turn that
into `DLSSNR.ControlMask` — an RGB per-pixel control texture, **R = final blend weight, G scales
local tone, B scales local structure** (HARD, from the disassembly) — and raise structure on the
cat alone. It is reachable in principle. It should not be built yet, and the reasons are worth
writing down so the next session does not re-derive them:

* **It costs the skin term outright.** Binding a mask forces `UseAutoMask = 0`, which drives both
  resolved values to `-1.0f`, and **the mask has no skin channel** — skin exists only on the auto
  path. The user currently runs `NgxNRAutoMask=1` with `NgxNRSkinStructure=2` over a world that
  "looks 100% perfect". C trades that away to fix one subject.
* **Five unknowns, none of them free.** Does gFur render into the custom-depth pass at all (its
  VFs declare `bSupportsPositionOnly = false`; the depth-pass permutations are UNCONFIRMED)? Can
  we *find* the custom-depth target at present time — it is bound as a DSV and never as a compute
  UAV, so our pass walk never sees it, and identifying it by shape is the same identification
  problem the TAA pass cost sessions? Is its content valid at Present? What extent does
  `ControlMask` want — it has `Subrect` fields but, unlike `MVec`, **no `ScaleX/Y`**, so whether a
  render-res mask is legal against an output-res colour grid is UNCONFIRMED, and if it is not the
  upscale needs **the first compute dispatch we have ever recorded on the present list**. And do
  `G`/`B` scale ABOVE 1, or only damp? If only damp, the design inverts: raise the global and
  attenuate the world, which changes the half that is already right.
* **It is not free at runtime either.** `r.CustomDepth=3` re-draws the tagged primitives, and with
  `LAYERS=48` that is 48 extra shell draws per frame on a title already at ~55 fps with FG on.

**If it is ever built, the first step is not the mask.** Bind a CONSTANT ControlMask — uniform
1.0, then uniform 0.5 — and confirm from the log and the screen that the runtime reads it and that
`UseAutoMask` dropping to 0 does not by itself wreck the look. That separates "can we drive this
input" from "is our mask right", which is the `NgxPaint` ladder that cracked "DLSS runs but
nothing changes".

**And there is a free mask hiding in the answer to A.** Because the fur DOES write velocity, the
engine's own `EncodedVelocity.x > 0` is already an approximate moving-subject mask, at render
resolution, in a texture `mv_resolve` reads every frame — one extra UAV and no engine change, no
custom-depth pass, no new identification problem. It is not exact (it catches every mover and
loses the cat when the cat is still), but it is the cheap path, and it exists only because A came
back negative.

### DLSS Neural Rendering WORKS — the missing piece was an HDR colour codec (2026-09-01)

**Confirmed on the user's machine: a correct image.** Feature 18 initialises, creates, evaluates
and its result reaches the screen without corruption, at 61.7 fps, past 4800 frames with a clean
`dmesg` — where the previous (red-noise) build died at ~2800 frames.

**The cause of the red noise was never the parameters, the arch gates, the mips or the colour
space flag.** It was that the network is **display-referred** and our hook point carries **raw,
unbounded, pre-exposed linear HDR**. There is no `isHDR` in the runtime — verified by
case-insensitive search in both ASCII and UTF-16, along with no `ColorSpace`, `PaperWhite`,
`Exposure`, `Pre.Exposure` or `Feature.Create.Flags` — so the runtime **cannot be told** about
the input encoding and the conversion is entirely the caller's job.

The fix, ported from a working dxvk-remix integration (`github.com/lunks/dxvk-remix-plus-dlssnr`,
branch `dlssnr`, commits `aa90a180` and `fc4de144`), wraps the evaluate in two compute passes:

```
encode:   proxy = SrgbEncode(SoftClip(max(image, 0) * s))   ->  DLSSNR.Color = the PROXY
evaluate:                                                   ->  neural output
decode:   image += (SrgbDecode(neural) - SrgbDecode(proxy)) / s      IN PLACE
```

* Soft clip: knee `0.75`, shoulder `5.770780`, taken verbatim from RenoDX's encoder. It is C0 but
  **not** C1 — reproduced rather than "corrected", because that is what the working deployment
  ships. **It reaches exactly 1.0 at an input near 3.474**, so anything brighter than ~3.5x
  display white encodes to flat white; that bounds how far paper white can usefully move.
* **Exact piecewise sRGB**, both directions — never an `x^2.2` approximation. The network was
  trained on true sRGB.
* The transfer `result = o + (n - p) / s` carries back the network's *change* rather than its
  output, so **HDR headroom survives** and identity is bit-exact: `n == p` gives a `+0.0` delta
  and returns the original unchanged. CI proves this across 8 paper whites x 9 radiances (to
  60000, far above the knee) x 9 strength pairs. A no-op network must produce a no-op frame.
* `[STRAYDLSS] NgxNRPaperWhiteScale` (default 1.0, `scale = 1/paperWhite`, values below 1.0
  legal), `NgxNRColorStrength`, `NgxNRTransferStrength` (0 is an exact bypass).

**A second, independent bug fixed in the same change: the copy-back.** The pass used to end with a
full-RGBA `CopyResource` of the neural output over the engine image. That clamped the HDR range
*and* overwrote alpha with the network's meaningless one — and on this title **that resource
becomes the next frame's `HistoryBuffer[0]`** (§2.9), so the engine read the damage straight back
in. The decode now writes in place and carries the original alpha through.

**`NgxNRTopology=sr` is refused by design.** The residual needs proxy, neural and original to be
the same pixels; sr-shaped puts colour at render resolution and output at display resolution.
Refusing loudly beats silently reverting to the raw-HDR path.

### NOTHING ON THE NR PATH IS DESTROYED ANYWHERE BUT `nr::on_present` (2026-09-02)

`EvaluateFeature` only RECORDS work and NGX holds no references to anything we pass it (§5), so
"the CPU stopped using this" and "the GPU stopped reading it" are different moments. Under
vkd3d-proton there is no debug layer to notice the difference: releasing a texture, a readback
buffer, a descriptor heap or a PSO that an executing command list still references is a wrong
image or a GPU that leaves the bus, never an error return.

**`src/core/nr_lifetime.hpp` is the rule.** One monotonic fence, signalled on the swapchain's own
queue once per present; work is tagged with the value that present will signal and freed at the
first present whose completed value has passed it. With no queue the fallback is the same
two-frame-cycle present ring `ring::is_safe_to_release` uses everywhere else — never "free now",
never "never free". `nr::on_present(queue)` is the only place any of it is released, and it runs
**even while NR is disabled**, because disabling is what queues the teardown.

Consequences worth knowing before touching that path:

* **`NgxNR` 1 -> 0 destroys nothing on the caller's thread.** It queues; flipping back to 1 before
  the teardown runs cancels it. `ReleaseFeature` additionally requires the present boundary —
  the feature owns GPU resources we cannot keep alive by refcount — and logs the last evaluate's
  fence tag beside the queue's completed value so a live run can prove the ordering held.
* **A resolution change declines NR for a few frames** (`recreating`) rather than releasing the
  feature from inside the intercepted TAA dispatch.
* This came from `RemixProjGroup/dxvk-remix` @ **`a69254ab`**, which found two use-after-frees on
  exactly the enable -> disable transition; the second is ours verbatim — *"Releasing the NGX
  feature on deactivation did not wait for the device. The feature owns GPU resources DXVK cannot
  see and therefore cannot keep alive."*
* **UNCONFIRMED LIVE.** Proven in CI (`tests/test_nr_lifetime.cpp` and the WARP harness's
  `warp_nr_lifetime.inc`, which asserts by refcount that nothing is freed before its fence);
  nothing has run against the game.

### THE CODEC *IS* THE INPUT DOMAIN — so no codec means no evaluate

The soft-clip + exact-sRGB proxy is not a tuning stage sitting in front of feature 18. **It is
what makes the input display-referred**, which is the only domain the network was trained on.
Our hook point carries raw, unbounded, pre-exposed linear HDR; the encode is the entire
conversion, and the runtime has no colour-space, HDR or exposure parameter with which to be told
otherwise.

**A sibling port found the same rule from the opposite direction, which is why it is worth
recording as a rule rather than as our own bug.** `RemixProjGroup/dxvk-remix` (branch
`dlss-nr`, commit **`2df9c812`**, "Neural Uplift: anchor the pass after the sRGB encode") moved
its injection point because both earlier ones were "wrong, and wrong in the same way": in that
runtime tone mapping leaves the image in *linear LDR* and the sRGB encode is a separate late
pass, so anchoring before it handed the network a linear image "which it read as though it were
already gamma-encoded; the runtime then applied the real encode on top. **Effectively two gamma
curves, which shows up as lifted blacks and washed out greys in dark scenes.**" Its conclusion is
the general one: the anchor "is a correctness requirement rather than a tuning choice", and the
frames where that encode does not run "are still linear at this point, so the pass declines them
and forces a history reset rather than evaluating on an input domain the model was not trained
on." Its sibling commit **`a69254ab`** is the lifetime half of the same lesson (see the
deferred-destruction section).

**Two consequences for us, both now enforced in code
(`src/core/nr_hook_plan.hpp`, `nrplan::codec_gate`):**

1. **Any frame that cannot produce a correct proxy declines.** The encode not recording, a call
   site that runs no codec at all, a codec scale pinned at one of `nrc`'s clamps (flat black or
   flat white — the right format carrying no image), or `NgxNRTrackExposure` on with the
   engine's exposure never decoded. Each has its own refusal counter in the periodic NR line and
   in `stray-dlss-status.txt`; declining is the correct behaviour, but a rate that never falls
   is a configuration problem, not a quiet one.
2. **A declined frame forces one `DLSSNR.Reset` on the next evaluate.** Feature 18 reprojects
   its own accumulation with motion vectors describing exactly one frame of motion, so
   reprojecting across a frame it never saw is the same class of error as a wrong `MVecScale` —
   and it compounds through the accumulation rather than costing one frame.

**And the general trap: every knob that moves the codec's operating point moves the network's
input domain.** `NgxNRPaperWhiteScale` and `NgxNRTrackExposure` are not brightness controls; they
decide where the frame lands relative to the 0.75 soft-clip knee, i.e. *what image the network is
shown*. That is why the scale gets its own history-invalidation latch
(`nrc::codec_scale_invalidates_history`) alongside the guide-extent one, why the exposure term
needs a time constant far slower than the loop it sits in, and why "it looked better at 0.1" was
a bug report about a missing wire rather than a preference.

### The u0-feedback diagnosis is REFUTED by experiment (measured 2026-09-01)

I argued at length that the SSR artefact was NR's residual re-entering the engine's temporal
history, because `u0` is both the scene colour and the next frame's history
(`TemporalAA.cpp:696`), and built `NgxNRRestoreHistory` to close that loop: snapshot `u0` after
the SR evaluate, let the frame display the NR image, restore the pristine copy at present so the
next frame's history never sees NR.

**It works exactly as designed, and the artefact is unchanged.**

```
NR HISTORY frame 45000: snapshots=27848 restores=27848 harmfulMisses=0 overwritten=0
                        scratch 3840x2160 fmt=10 created, no refusals
```

Snapshots and restores perfectly paired across 27,848 frames, zero harmful misses, zero
refusals. **The test was valid and the result is negative: the feedback loop is not the cause.**

**The counters are why this is a result rather than a shrug.** "I turned it on and saw no change"
is worthless if the feature quietly refused — which is exactly how a mis-stated resource state or
an unflushed command list would have presented. Instrumenting the thing that could silently
no-op turned an ambiguous non-observation into a clean refutation. Build the counter before you
need it.

**What the evidence now points at instead** — and note it explains every earlier negative at
once. SSR is composited into scene colour BEFORE our hook, so by the time NR sees it a reflection
is just pixels. NR then reprojects its own temporal history using the motion vectors we supply,
which describe **the surface's** motion, reconstructed from depth. A reflection does not move
with its surface — it moves with the reflected geometry, often in the opposite direction. So NR
fetches history from the wrong place for exactly those pixels, every frame the camera moves.

That survives both hook sites (they share the motion vectors), the codec, the exposure work and
the history restore, because none of them change what the vectors say about a reflective surface.
It is also why the artefact concentrates on dark wet ground: that content is largely reflection.

**UE 4.27 cannot fix this for us.** Correct reflection motion vectors would need the reflected
geometry's motion, which the velocity buffer does not carry. The reference does not hit it
because its guides come from a path tracer that writes dense, correct vectors. **Treat
"temporal network + screen-space reflections" as a structural mismatch, not an open bug** — the
available moves are reducing NR's strength on that content, disabling SSR, or accepting it.

### The SSR fade and most of the flicker: resolved, cause not fully isolated (2026-09-01)

The long-running "reflections and fine detail fade over tens of seconds, then recover" stopped
reproducing. **Several fixes landed together, so the attribution is not clean — record it that
way rather than crediting whichever story reads best.** In likely order of contribution:

1. **`DLSSNR.MVecScaleX/Y` back to 1.0** (the user's live A/B, which overturned a code-reading
   argument for the colour/guide ratio). This is the user's own attribution and the mechanism is
   the strongest: **feature 18 reprojects its OWN temporal history with these vectors**, so a 2x
   scale error fetches every history sample from twice the distance it should. That yields both
   symptoms from one cause — instability during motion, and a persistently wrong accumulation
   that reads as drift when the camera is still.
2. **The create-site shape gate.** Before it, cubemap faces and reflection captures were accepted
   as the primary view and DLSS features were created **11 times a session instead of once**.
   Every spurious creation is a full teardown of DLSS's temporal accumulation, and a periodic
   history wipe is a textbook "degrades, then snaps back".
3. The `ClipToPrevClip` transposition fix, which corrected the camera-reconstruction branch for
   all static geometry.

**Timeline caveat, stated so nobody over-credits #1:** MVecScale went 2.0 -> 1.0 -> 2.0 -> 1.0
across one day, and the fade was reported in more than one of those states. So the scale was
probably the dominant term but cannot have been the whole of it.

**The durable lesson: bad motion vectors do not produce one bad frame, they compound through the
accumulation.** Any temporal consumer — DLSS SR, feature 18, the engine's own TAA — integrates a
motion error over its history, so a scale or convention mistake surfaces as drift, smearing and
instability rather than as anything that looks like a motion-vector bug. When a temporal artefact
resists every explanation in the temporal machinery itself, check what you are feeding it.

### Exposure tracking: needed, but only with a LONG time constant (measured 2026-09-01)

> **SUPERSEDED 2026-09-03 — see "NR IS A PRESENT STAGE, AND THAT DELETED HALF THE FEATURE"
> above.** Every key this section tunes (`NgxNRTrackExposure`, `NgxNRExposureSmoothing`,
> `NgxNRScaleResetTolerance`) belonged to the HDR colour codec and is gone: the back buffer
> carries no pre-exposure to follow. The reasoning is kept because it is the record of how the
> placement problem was found, one compensating mechanism at a time.

Three findings that only make sense together, and the third is the one that settles it:

1. **Tracking creates a feedback loop and it rings.** With `NgxNRTrackExposure` on, the measured
   codec scale ramped `13.56 -> 4.82 -> 8.49` — a 2.8x swing on a **~5 second period**, which is
   what the user saw as "flickering every 3-5s" on a *static* image. The loop is
   `scale -> proxy brightness -> NR output -> u0 -> histogram -> eye adaptation -> PreExposure ->
   scale`. Adding a reset-on-scale-change latch made it worse: 52 fires, each discarding feature
   18's accumulation. **A reset latch is right for a rare discrete change (a resolution switch)
   and wrong for a quantity that varies continuously — there it is a metronome.**

2. **The argument for dropping tracking entirely looked strong.** UE's base pass already
   multiplies scene colour by `PreExposure` (`BasePassPixelShader.usf:1512`), and `PreExposure`
   IS the eye-adaptation scale rather than its inverse (`PostProcessEyeAdaptation.usf:127-130`),
   so the image reaching us is already exposure-normalised and a constant *should* suffice.
   RenoDX — the only known-working DLSSNR deployment — uses a hardcoded constant and writes a
   literal `1.0` into every exposure parameter.

3. **But it does not suffice, measured: the MENU FOG needs tracking.** With a fixed scale tuned
   for gameplay the menu's fog is wrong. So UE's pre-exposure does NOT normalise enough to span a
   menu-to-gameplay swing — plausibly because the menu runs a different TAA permutation and a
   different scene-colour format (`R11G11B10` vs `R16G16B16A16`, §5), putting its operating point
   far from gameplay's.

**So the choice was never "fixed (wrong per scene)" versus "tracked (rings)". It is the TIME
CONSTANT.** Keep tracking on and make it far slower than the loop it sits in: `rate` 0.002 follows
a change over ~500 frames (~8 s), fast enough for a menu-to-gameplay transition and far too slow
to participate in a ~5 s resonance. Putting our pole well below the loop's is the ordinary way to
keep an adaptive term without letting it oscillate.

**The general lesson, and it cost most of a session: when an adaptive quantity you consume is
also, indirectly, downstream of what you produce, you have closed a loop — and the question is
never "track or not" but "at what frequency".** Ask what reads your output before deciding to
follow anything the engine computes.

### The drift was mostly EXPOSURE COUPLING, and we caused it by dropping a feature in the port

> **CORRECTED 2026-09-01, on the user's report that `NgxNRTrackExposure` "seem[s] to fix a lot of
> it".** The section below diagnoses the drift as PLACEMENT — NR's residual re-entering the TAA
> `u0`. That is real but was **not the dominant term**. Read this first.

The reference ships **`trackAutoExposure`, default TRUE** (`rtx_neural_rendering.h:137-140`): its
proxy scale is multiplied by the engine's live exposure, so the codec's soft-clip knee follows
scene brightness. **We dropped it in the port and hardcoded a static scale.** The consequence is
a feedback loop we introduced ourselves:

* `u0` feeds the eye-adaptation histogram (`PostProcessing.cpp:626-648`), which drives exposure
  on later frames.
* With a FIXED knee and a MOVING exposure, the codec's operating point drifts, so the residual
  `(n - p)/s` becomes a function of the exposure that the residual just perturbed.
* That compounds — and it is *global*, which fits the measured signature (a whole-frame mean
  decay of 82.9 -> 52.6) far better than a reflection-specific mechanism ever did. It is also
  why `r.SSR.Quality=0` did not stop it.

`[STRAYDLSS] NgxNRTrackExposure` (default ON) multiplies the scale by `OneOverPreExposure` (View
CB row 135.z) on the `taa` site only — post-tonemap sites have no pre-exposure to undo.

**Two lessons, and the second is the general one:**

1. **A hand-tuned constant that works is a bug report.** The user found `NgxNRPaperWhiteScale`
   ≈ 0.1 looked best. `1/PreExposure` measured 0.056, i.e. ~18 — the same quantity. They were
   hand-dialling a value the engine already knew. When a knob's best setting turns out to equal
   something the engine publishes, the knob is standing in for a missing wire.
2. **When porting a working implementation, an omitted feature is a defect even if the code
   compiles and the image looks plausible.** We reproduced the codec's arithmetic faithfully —
   and verified it line by line — while dropping the control loop around it, then spent
   considerable effort diagnosing the resulting instability as an architectural property of our
   hook point. Check what the reference's OPTIONS do, not only what its shaders compute.

**What this does NOT retract:** `u0` genuinely is both scene colour and next-frame history
(`TemporalAA.cpp:696`), NR genuinely does keep its own temporal history, and the user reports the
drift is present with DLSS SR alone and merely amplified by NR. So a residual placement effect
remains, and the SR-only baseline still wants its own explanation. But the dominant, fixable term
was ours.

### NR's output feeds the engine's temporal history, and that compounds (measured 2026-09-01)

**Symptom, user-reported and then measured:** with NR on, everything temporally accumulated
slowly degrades and then snaps back — screen-space reflections on a wet floor drain to flat, and
the **volumetric light shafts in the MAIN MENU** do the same. It is not reflection-specific.

**Measured in the menu** (16 shots, 3 s apart, camera static, no input):

```
frame  3  mean=82.9  sd=96.5  hf=14863
frame  7  mean=62.0  sd=72.9  hf=16403
frame 13  mean=52.6  sd=68.5  hf=18899   <- darkest, flattest, noisiest
frame 16  mean=71.0  sd=86.8  hf=14933   <- recovered
```

Brightness and contrast decay over ~30 s while **high-frequency energy RISES as they fall**. That
inverse relationship is an accumulator degrading, not scene animation. `NgxNR=0` removes most of
it. Whole-frame metrics nearly hide the effect in gameplay (mean varied 1.7%) while the affected
quadrant swung 11% — **measure the region, hold the camera still.**

**Cause: placement, not arithmetic.** `src/core/nr_codec.cpp` is a faithful line-for-line port of
the reference (chroma valve, fade weight, guards and clamp all verified). The divergence is
*where* the result lands:

* The reference writes into `m_finalOutput` — a **terminal** image, consumed downstream by bloom,
  motion blur and the tone curve. Nothing reads it back.
* **We write into the TAA dispatch's `u0`, which §2.9 establishes UE 4.27 extracts as the next
  frame's `HistoryBuffer[0]` — and which is the SAME texture as the downstream scene colour.**
  `TemporalAA.cpp:696` is literally `NewHistoryTexture[0] = Outputs.SceneColor =
  NewHistoryTexture[0];`, and `:969` extracts that same texture. One resource, two roles, so
  **the engine cannot be handed a different image for history than for display at this hook
  point.**

> **CORRECTED against the UE 4.27.2 source.** This section first named the TAA shader's own
> history read as the loop. **That consumer is DEAD:** we `return true` from `dispatch` and
> suppress the engine's TAA every frame, so the shader that samples `HistoryBuffer_0` at `t5`
> never runs. Do not chase it.
>
> **The live conduit is SCREEN-SPACE REFLECTIONS, which read the history directly and bypass the
> TAA shader entirely.** `ScreenSpaceRayTracing.cpp:596-620`, inside
> `RenderScreenSpaceReflections`, falls through to
> `InputColor = View.PrevViewInfo.TemporalAAHistory.RT[0]` (the half-res branch needs
> `GSSRHalfResSceneColor`, which defaults to 0). That is exactly the resource our decode writes.
> So the residual enters the reflections, is composited back into scene colour, TAA'd, DLSS'd and
> residualled again — a closed per-frame loop whose gain is the pixel's reflective contribution,
> which is why a **wet floor** shows it worst. `FSSDTemporalAccumulationCS` on top supplies the
> multi-second time constant.
>
> **A second, weaker loop runs through EYE ADAPTATION** and fits the *global* menu brightness
> decay better than SSR does: `PostProcessing.cpp:626-648` downsamples the post-TAA scene colour
> — our modified image — into the histogram that drives exposure on later frames. The reference
> has this same coupling and is stable, but it also keeps `trackAutoExposure` (default true),
> which we dropped in the port: its proxy scale follows the engine's exposure so the codec's
> operating point cannot drift, while ours is a hardcoded constant.
>
> **And the fix this unlocks:** on the desktop deferred path **nothing after the tonemapper is
> carried into the next frame** — every `QueueTextureExtraction` into `PrevFrameViewInfo` sits at
> `PostProcessing.cpp` 576/599/643 while `AddTonemapPass` is at 777. A post-tonemap hook has zero
> feedback path by construction, and needs no HDR codec either.

**Diagnose before building.** `r.SSR.Quality=0` kills the first loop and `r.EyeAdaptationQuality=0`
the second; one launch each with the menu burst says which fix is actually needed, and
`r.SSR.Quality=0` doubles as an immediate workaround.

**The general lesson, which outlives this feature: our interception point is a FEEDBACK NODE, not
an output.** Any pass that modifies `u0` is writing into the engine's temporal state, so an
effect that is stable frame-to-frame in a normal post-process chain can still diverge here. Ask
"what reads this next frame?" before writing anything into an intercepted TAA output — and note
that porting a correct implementation is not sufficient when the target resource has a different
role in the frame graph.

### The drift, fixable WITHOUT moving the hook: `[STRAYDLSS] NgxNRRestoreHistory` (default **OFF**)

> **SUPERSEDED 2026-09-03 — see "NR IS A PRESENT STAGE, AND THAT DELETED HALF THE FEATURE"
> above.** The hook DID move, and `NgxNRRestoreHistory` / `NgxNRRestoreState` went with it: a
> post-tonemap site has no feedback path to close. The reasoning is kept because it is the
> record of how the placement problem was found, one compensating mechanism at a time.

**Built, CI-green on all three lanes, and SHIPPED OFF.** Read the two paragraphs under "Why it
is off" before turning it on, and read the state ledger before trusting it.

The placement argument above is right and the `preui` attempt that came out of it wrecked the
frame (it clobbered state the next pass was about to use). But the loop can be broken at the
`taa` site — whose image quality is the good one — without moving anything, because of an
asymmetry the placement reading missed:

**SSR reads the history on the NEXT frame, and every SAME-frame consumer of `u0` has already run
by the time the game calls Present.** So there is a window at end of frame where `u0` can be put
back to what the engine would have written, and nothing that has already consumed it notices.

```
TAA hook:  DLSS SR writes u0
           -> CopyTextureRegion(scratch <- u0)     pristine, pre-NR
           -> NR decode writes into u0 in place
           ... the rest of the frame — post, tonemap, UI — uses the NR image ...
present:   CopyTextureRegion(u0 <- scratch)        the engine's history never saw NR
```

The DISPLAYED frame is byte-identical to before. Only what the engine carries forward changes.

**Ordering is HARD, from ReShade v6.8.0's own source.** `DXGISwapChain::on_present`, d3d12 case,
runs `invoke_addon_event<present>` → `present_effect_runtime` → `flush_immediate_command_list()`,
all inside the hooked `IDXGISwapChain::Present` — i.e. after the game has submitted every command
list of the frame to that same queue. The flush is a plain `ExecuteCommandLists` on it, and one
queue executes in submission order. **The trap:** `flush()` early-outs on `!_has_commands`
(`d3d12_impl_command_list_immediate.cpp:122`), and `_has_commands` is set by ReShade's
`command_list` API, NOT by recording onto the native list behind `get_native()`. With an empty
preset nothing else records at present, so a purely native restore would sit unflushed in an open
list. The barriers therefore go through `cmd_list->barrier()` (which sets it and records a real
`D3D12_RESOURCE_BARRIER` — `api::resource_usage` is a bit-for-bit passthrough of
`D3D12_RESOURCE_STATES`) and only the copy is native.

**The one hypothesis, and it is a knob for that reason: which state `u0` is in at present.**
D3D12 cannot be asked, and the restore runs on a different list from the snapshot. Derived from
four UE 4.27 source anchors — `TemporalAA.cpp:969` extracts the history with the two-argument
`QueueTextureExtraction`, whose `AccessFinal` is `RenderGraphBuilder.h:209`'s
`kDefaultAccessFinal = ERHIAccess::SRVMask`; `D3D12Commands.cpp:398-414` maps a readable
`AccessAfter` to `Resource->GetReadableState()`; `D3D12Resources.h:377/389` defines that as
`NON_PIXEL_SHADER_RESOURCE | PIXEL_SHADER_RESOURCE`. Hence **0xC0**, overridable with
`[STRAYDLSS] NgxNRRestoreState`.

**Why the constant matters more than it looks.** We transition back to the value we CLAIMED, so
after our pair the resource really is in that state — while UE4's own CPU-side tracking believes
whatever it last set. A wrong constant therefore desynchronises the ENGINE's bookkeeping, not
just ours. On this target vkd3d-proton softens it (a `ALLOW_UNORDERED_ACCESS` image stays in
`VK_IMAGE_LAYOUT_GENERAL`, so a wrong `StateBefore` costs an access mask rather than a layout),
but that must not be relied on.

**Scope, stated honestly.** This closes loops whose consumer reads the TEXTURE next frame — the
SSR / `TemporalAAHistory.RT[0]` conduit. It does NOT close a loop whose consumer read `u0` within
the same frame and stored the result elsewhere: the eye-adaptation histogram
(`PostProcessing.cpp:626-648`) still sees the NR image. That one already has its own fix
(`NgxNRTrackExposure`).

**Cost:** two full-rect copies of the output texture per frame (~66 MB each way at 4K FP16). It
is gated on `nr::validated()`, so a session with NR off, still warming up, or permanently
refusing pays nothing at all. `snapshots`, `restores` and — the number that must stay zero —
`harmfulMisses` are in the periodic `NR HISTORY` line and in the overlay, where the checkbox is
live so the A/B happens inside one session. Inert and loudly so at the post-tonemap sites, which
have no feedback path to close. Gate and refusal rules: `src/core/nr_history_plan.hpp`,
`tests/test_nr_history_plan.cpp`.

**Why it is OFF by default, and this is the part worth keeping.** Two reasons, and the second
decides it:

1. **As of 2026-09-01 the fade is no longer reproducing.** Something else landed the same day
   fixed it — candidates are the `ClipToPrevClip` transposition fix, `MVecScale` returning to
   1.0, the create-site shape gate (which cut feature creations from 11 to 1, each of which was
   a full history reset), or the exposure work. Which one is unknown and, for this mechanism,
   irrelevant.
2. **The restore runs an unverified state assumption on every frame.** That is the same class of
   assumption that made `preui` wreck a frame the same night. Paying it to fix a problem that is
   not currently occurring is a bad trade even when the code is right.

So it is a TOOL with its diagnosis already written down — reach for it the moment the fade
returns, rather than rediscovering all of this. `tests/test_nr_history_plan.cpp` pins the default
OFF so a future session flipping it does so knowingly.

**What was and was not verified — the honest ledger.**

| Claim | Status |
|---|---|
| `u0` is in `UNORDERED_ACCESS` at the snapshot | **HARD.** `ngx_nr.cpp`'s `image_state` derives the identical constant on the same resource at the same point |
| `addon_event::present` fires before `flush_immediate_command_list()`, and both after the game's submissions | **HARD.** ReShade v6.8.0 `dxgi_swapchain.cpp`, d3d12 case, read directly |
| The immediate list executes on the swapchain's own queue, in submission order | **HARD.** `d3d12_impl_command_list_immediate.cpp:154` |
| `flush()` early-outs on `!_has_commands`, which only ReShade's `command_list` API sets | **HARD.** `d3d12_impl_command_list_immediate.cpp:122`; why the barriers go through `cmd_list->barrier()` |
| `api::resource_usage` is a bit-for-bit passthrough of `D3D12_RESOURCE_STATES` | **HARD.** `convert_usage_to_resource_states:236` is a plain mask |
| The present callback is inside the queue's own mutex, so using the immediate list there is safe | **HARD.** `dxgi_swapchain.cpp`'s `unique_direct3d_device_lock`, with a comment saying exactly that |
| `u0` is in `NON_PIXEL \| PIXEL_SHADER_RESOURCE` (0xC0) at present | **[derived], four source anchors, ZERO measurements.** This is the risk |
| The restore is invisible to the displayed frame | **[derived]** from the ordering facts above. Not seen on a screen |
| Cost is ~1-2% of a 4090's bandwidth | **[derived]** arithmetic. Not measured |
| vkd3d-proton softens a wrong `StateBefore` on a UAV-capable image | **SOFT.** Consistent with its GENERAL-layout policy; not verified against 3.1.0's source |

The last four are what a live run would settle, in that order.

### NR is now a PRESENT STAGE too: `[STRAYDLSS] NgxNRHook = taa | present` (phase 1, 2026-09-02)

> **SUPERSEDED 2026-09-03 — see "NR IS A PRESENT STAGE, AND THAT DELETED HALF THE FEATURE"
> above.** There is no `NgxNRHook` any more: `present` won and `taa` was deleted. The reasoning
> is kept because it is the record of how the placement problem was found, one compensating
> mechanism at a time.

> **This supersedes "The NR hook site is ONE site again", written earlier the same day.** That
> section is kept below verbatim, because *why the earlier sites failed* is what makes this one
> defensible — and because the two failures it records must not be re-run.

**The decision, the user's:** *"let's make the NR be a present layer and clean it up."* NR is not
an upscaler — same resolution in, same resolution out — and it sat inside the intercepted TAA
dispatch only because that is where we already had a hook. Everything painful about it follows
from that one choice:

* the site is **linear, pre-tonemap HDR** while the network is display-referred, which is the
  entire reason for the HDR codec (soft clip + exact sRGB proxy, two compute passes and their two
  shaders), for `NgxNRPaperWhiteScale`, and for `NgxNRTrackExposure` with its smoothing rate and
  its scale-reset latch — a `DLSSNR.Reset` source driven by a continuously varying quantity, which
  this file has already measured making an image *worse*;
* the texture we write is `u0`, which UE 4.27 extracts as the next frame's TAA history AND hands
  downstream as scene colour (`TemporalAA.cpp:696`, `:969`; `ScreenSpaceRayTracing.cpp:596-620`),
  which is the feedback node `NgxNRRestoreHistory` exists to close.

**None of that is needed at Present**, and the reason is one measured fact: Stray's back buffer is
`R10G10B10A2_UNORM` and the game never calls `SetColorSpace1`, so it is **SDR display-encoded
already** — the network's own training domain (facts §32; the colour-space half is SOFT here,
carried from the `dlss-fg` branch). No codec, no paper white, no exposure term, no feedback path.

**Why the two earlier failures do not generalise, which is the whole argument:**

| earlier site | how it died | why the stage is different |
|---|---|---|
| `preui` | clobbered state the **game's** command list needed | the stage records on the present owner's OWN list (`src/backend_native/present_owner.hpp`), where nothing of the game's is bound — so there is nothing to clobber and nothing to restore |
| old `present` | rode `addon_event::reshade_begin_effects`, which **never fires with an empty preset** | the stage is triggered by `icept::Sink::on_present`, which both hosts deliver unconditionally |

**How it is built.** `src/nr_hook.{hpp,cpp}` is the live half (guides, gate, the `nr::apply`
call); `src/nr_stage.{hpp,cpp}` is the NGX-free D3D12 half — one committed texture and two
`CopyResource` calls — split out precisely so the WARP lane can judge the barriers and read the
pixels back (`tests/warp/warp_nr_stage.inc`). The gate is pure and tested
(`nrplan::plan_post_tonemap`, `tests/test_nr_hook_plan.cpp`). Per frame: back buffer -> our
staging copy -> `nr::apply(Site::post_tonemap)` -> staging -> back buffer.

**Four things about it that are load-bearing:**

1. **The guides still come from the TAA site.** Depth, the dense motion vectors, the render extent
   and the camera-cut OR are produced there and PUBLISHED with a sequence counter; the stage
   consumes each capture exactly once. A frame with no TAA dispatch — a loading screen — does not
   advance the counter, so the stage declines it as `guides-stale` with no separate test.
2. **The stage runs AFTER `nr::on_present`, and that ordering is correctness, not tidiness.**
   `nr::on_present` signals the NR lifetime fence on the presenting queue, but our present list is
   only EXECUTED after the whole callback returns — so work recorded *before* the signal would be
   reported complete while its commands had not run, and every deferred free including
   `ReleaseFeature` would happen under an in-flight list. Recording after it tags the work with the
   NEXT present's value. The one cosmetic consequence is one spurious `frame-gap` reset on the very
   first stage frame, which lands on a freshly created feature that carries a reset anyway.
3. **The back buffer's assumed state is `PRESENT` (== `COMMON` == 0), overridable with
   `[STRAYDLSS] NgxNRStageBackBufferState`.** D3D12 *requires* it there when `Present` is called and
   both triggers run inside that call — but "required by the API" and "true on this stack" are not
   the same sentence, and vkd3d-proton has no debug layer to object to a wrong `StateBefore`.
4. **`NgxNRRestoreHistory=1` is IGNORED at this site, loudly.** There is no feedback path to close
   and nothing writes `u0` there to snapshot.

**Known and accepted for phase 1: the HUD is in the image.** `DLSSNR.UICorrection` is sent (it is
RenoDX's own default and ours); its effect is **UNCONFIRMED**. Stray's HUD is sparse, so this is
expected to be tolerable and is a thing to LOOK at rather than argue about. The follow-up is a
pre-UI seam — the frame's first back-buffer render-target bind — which frame generation also wants
for its HUD-less input; **it should be built once and serve both**, and it is deliberately not
built here.

**Phase 1 SHIPS `taa` AS THE DEFAULT**, so the image is byte-identical until someone asks
otherwise. Everything above is **UNCONFIRMED live**: no run on the box has judged it.

**Phase 2 is one launch** with `NgxNRHook=present`: the user judges the image, and the `NR RESETS`
line says whether the `codec-scale=` resets — the current leading suspect for the reported flicker
— have gone to zero, which is the point of the whole exercise. **Phase 3, only after that
confirms**, makes `present` the default and DELETES what the TAA site needed and this one does
not: the codec and its two shaders, `NgxNRPaperWhiteScale` / `NgxNRColorStrength` /
`NgxNRTransferStrength`, `NgxNRTrackExposure` with its smoothing and reset tolerance,
`NgxNRRestoreHistory` and `nr_history_plan`, the `codec-topology` refusal, and the then-unreachable
`no-codec` / `exposure-unknown` / `degenerate-scale` gates — with this file and the facts doc
updated in the same commits, because several of their sections describe machinery that will no
longer exist.

### The NR hook site is ONE site again: `taa` (the `present` / `preui` sites were REMOVED 2026-09-02)

> **SUPERSEDED the same day by the section above** — the `present` site is back, as a STAGE on our
> own present-time command list rather than as a ReShade event. `preui` stays deleted. What is
> preserved here is the record of HOW each earlier site failed, which is the evidence the new
> section's "does not generalise" argument rests on.

This section used to describe a three-way `[STRAYDLSS] NgxNRHook` choice. **Both post-tonemap
sites are gone from the code** (`src/nr_hook.{hpp,cpp}`, the `preui` boundary rule, the
`plan_post_tonemap` gate, `reshade_begin_effects` wiring, `restore_viewports_and_scissors` on
the seam), by the user's decision: neither ever produced a correct frame on the box — `preui`
wrecked a frame the night it was tried, `present` depended on a ReShade event that never fires
with an empty preset — and every measured session, including the native-drive ones (facts §18),
ran NR inside the intercepted TAA dispatch. A leftover `NgxNRHook` key logs a WARN and is
ignored. What survives of that work is the guide-extent latch (`src/core/nr_hook_plan.hpp`) and
the end-of-frame history restore below, which is where the placement argument's remedy lives.

The measured facts that motivated the sites stand: `u0` is both scene colour and next-frame
history, feature 18 keeps its own accumulator, and the hook point is a feedback node. What
changed is the remedy — `NgxNRTrackExposure` and `NgxNRRestoreHistory` at the `taa` site — not
the diagnosis.

### The rival NR implementation, and the STAGE question it raises (2026-09-02)

A shipping mod (`xoxor4d/gta4-rtx` v1.5.2) replaced the user's own DLSS 5 integration — the one
our NR path is a port of — with `Kim2091/dxvk-remix@gta4-atmos-dlss5`, crediting the swap with
*"reduces shadow flickering"*. The full diff of the two implementations against ours, twelve
differences ranked by effect on temporal stability, is **`docs/RESEARCH-RENODX-DLSS5.md` §9**, and
the stage-vs-hook verdict is **§10**. Do not re-derive either; the highlights:

* **HARD: the shipped runtime's NR code IS Kim's branch byte-identical** (blob-SHA comparison,
  §9.0), and it contains **no NR shaders at all** — its display-referred conversion rides on the
  runtime's own sRGB pass. **SOFT: that the NR swap is why it flickers less**, since the same
  release also refactored the Remix vars and quadrupled the RTXDI sample count.
* **Two real gaps, both closed.** Their force-reset is sticky over *every* frame the network did
  not see; ours only covered frames NR was ASKED about and declined, because the arming path lives
  inside `apply()` — which is reached only when the TAA pass matched AND the SR/RR evaluate
  succeeded. `nrplan::note_frame_boundary` now arms at the present boundary. A fresh feature also
  forces one reset explicitly rather than by coincidence.
* **Our biggest remaining reset source is one THEY DO NOT HAVE.** `NgxNRExposureSmoothing` ships
  0.05 while the exposure-tracking section above concludes **0.002**, and `NgxNRScaleResetTolerance`
  ships 0.15 — a hard reset latch on a continuously varying quantity, which is the exact shape this
  file already records firing 52 times and making the image worse. **The defaults were NOT changed**
  (they are eye-tuned and the doc may be stale); the new **`NR RESETS`** line in the periodic report
  breaks every reset down by source and settles it in one *walked* session. Read `codec-scale=`
  first.

**And the bigger prize, §10: NR can run as a STAGE rather than a hook, and it should be tried.**
`src/backend_native/present_owner.*` already owns its own command-list ring, allocators and fence,
and runs after the game's last submission and before Present — the three properties the fork's
`RtxPass` has and our TAA hook does not. **Crucially, nothing of the game's is bound on that list**,
so the clobbered-state failure that wrecked `preui` cannot occur there; and `present` failed only
because it depended on a ReShade event that never fires with an empty preset. Neither prior failure
generalises, and neither was tried against the native present owner, which did not exist then.

A present-time stage would **delete** rather than tune: the whole HDR codec (the back buffer is
post-tonemap `R10G10B10A2_UNORM` with no `SetColorSpace1`, i.e. SDR display-encoded — the network's
own training domain), the entire exposure feedback loop *including* the reset latch above, and the
`u0` feedback node with `NgxNRRestoreHistory`. Risks: the HUD is in the image (mitigation
`DLSSNR.UICorrection`, effect UNCONFIRMED), typed UAV store on `R10G10B10A2_UNORM` must be probed
(the probe exists, `src/nr_codec_pass.cpp:464`), and no compute dispatch has ever been recorded on
that list.

**The experiment is one launch:** write a magenta patch into the back buffer through a typed UAV on
the present list, behind a key, and screenshot — the same `NgxPaint` separation of "can we write
here" from "is the network right" that cracked "DLSS runs but nothing changes". If it passes, this
outranks every difference in §9.

> **BUILT 2026-09-02 by the user's decision, as `[STRAYDLSS] NgxNRHook=present` — see "NR is now a
> PRESENT STAGE too" above.** The staging path went in instead of the magenta probe, because the
> stage's own log answers the same question more directly: a first `NR STAGE` line proves the
> copies reached the back buffer, and the periodic `NR STAGE:` counter names the gate that refused
> if they did not. The typed-UAV probe is reused rather than reimplemented (`nrstage::probe` calls
> `nrp::probe_typed_uav`) and its verdict is logged once per format. **No compute dispatch of ours
> is recorded on that list even now** — the stage is two `CopyResource` calls and NGX's own
> evaluate — so that particular unknown is still open, and it is not on this feature's path.

### Feature 18 has its OWN temporal history, and we were invalidating it silently

Confirmed in the reference (`fc4de144:src/dxvk/rtx_render/rtx_neural_rendering.cpp:220-230`),
verbatim: *"The NGX feature is keyed on the colour grid alone, so switching DLSS quality at a
fixed output resolution moves the guide grid --- and DLSSNR.MVecScaleX/Y with it --- underneath a
temporal history that was accumulated against the old one. Nothing else notices, so latch the
guide extent here and force a single reset frame when it moves."*

That is a **live bug for us**, not a hypothetical: this project runs both 50% and 70% screen
percentage, where the colour/guide ratio is 2.0 versus 1.42857, while the output rect the feature
is keyed on does not move. `nrplan::latch_guide_extent` now forces one `DLSSNR.Reset` when the
extent changes, and — following the reference's `resetGuideHistory = (latched != 0)` — **none on
the first observation**, so no session starts with a spurious reset. It is logged when it fires,
with both extents.

Corollary already in force but worth restating: the camera-cut OR (§2.8) must reach the evaluate
at **every** site. It travels with the published guides for exactly that reason.

### `NgxNRTrackExposure`: the codec's knee has to follow the scene, and the user found this by hand

> **SUPERSEDED 2026-09-03 — see "NR IS A PRESENT STAGE, AND THAT DELETED HALF THE FEATURE"
> above.** There is no codec and no knee at the present stage, so `NgxNRTrackExposure` no longer
> exists. The reasoning is kept because it is the record of how the placement problem was found,
> one compensating mechanism at a time.

**USER MEASUREMENT:** on the `taa` path the best-looking `NgxNRPaperWhiteScale` is about **0.1**,
an effective scale near 10x. That is not a quirk — it is the reciprocal of UE4's pre-exposure,
measured live at **0.056** (§2.6 row 135.y), whose reciprocal is ~18. The soft-clip knee is at
0.75, so a pre-exposed signal has to be lifted by roughly that factor to land near it.

**What it revealed: we dropped `trackAutoExposure` in the port.** The reference
(`rtx_neural_rendering.h:137-140`) defaults it TRUE and multiplies the proxy scale by the engine's
live exposure so the knee follows scene brightness; we hardcoded a constant. **Pre-exposure moves
with the scene**, so a paper white tuned in the dark starting apartment is wrong in a brighter
area — there is no single right constant. `[STRAYDLSS] NgxNRTrackExposure` (default **ON**) makes
the effective scale `proxy_scale(paperWhite) x OneOverPreExposure` (View row 135.z, already
parsed), clamped to the same [1e-6, 1e6] and falling back to the static scale when the View CB did
not decode. One `NR codec scale` line reports the decomposition, so "0.1 looks best" can be read
off as "tracking has made 1.0 the new correct value".

**The asymmetry, so it is not re-litigated:** the SR path's exposure goes through NGX
(`InPreExposure`, the exposure texture, the AutoExposure flag) and is at the runtime's mercy — the
texture mode measured INERT for us, and the NR codec reportedly ignores `DLSS.Pre.Exposure`
outright. **The codec's scale is our own shader arithmetic and cannot be ignored by the runtime**,
which is why this is expected to work where the SR exposure attempt did not.

### The NR luminance diagnostic must not run during a loading screen

`NR CODEC LUMINANCE` reports input -> proxy -> output max Rec.709 over one crop, and is how
`NgxNRPaperWhiteScale` is meant to be chosen. **Its first reading was worthless**: it fired at
`00:29:42` while the game did not reach gameplay until `00:29:56`, so it measured a **black
loading frame** — input `0.000000`, and an absurd recommendation of `scale ~6291456`
(`paperWhite ~0.0000`). That is the §2.4 menu/load trap again, in a new place.

The warmup gate (`NgxNRWarmupFrames`, default 60) elapses about a second after the first SR
evaluate, which is still on the loading screen. **Raise it (~3000) to push initialisation and the
measurement into gameplay before trusting any luminance number.** Note this did NOT stop the
codec from working — `paperWhite=1.0` produced a correct image regardless — so the knob is a
refinement, not a prerequisite, and its real value remains unmeasured.

### The DLSSNR snippet is ARCHITECTURE-GATED, and Ada support is a 13.5 MB graft

`~/Downloads/dlssnr-remix/patch_dlssnr.py` (signature-driven, survives build offset changes)
documents two gates inside `nvngx_dlssnr.dll`:

* **Gate 1** — the `MinHWArchitecture` constant reported by the four `*_GetFeatureRequirements`
  entry points. **Irrelevant to us:** Proton does not implement `GetFeatureRequirements` and we
  drive the snippet directly, so nothing ever reads it.
* **Gate 2** — an architecture switch inside `*_CreateFeature`, defaulting to a
  `"Unsupported GPU architecture"` log site. This one decides whether `CreateFeature` succeeds.

Arch ids: `0x160` Turing, `0x170` Ampere, `0x180` Hopper, **`0x190` Ada (RTX 40)**, `0x1a0`
Blackwell DC, `0x1b0` Blackwell2 (RTX 50).

**Patching the gates is NOT sufficient, and the script says so:** *"no sm_89 cubins in this file
— Ada (RTX 40) will pass the gates and then fail at kernel load."* Gate-only patching therefore
buys a successful `CreateFeature` and a successful-looking `EvaluateFeature` with no SASS behind
it — the silent-wrong-image failure mode this project exists to avoid.

**Measured 2026-08-31 across the three copies on the box (all exactly 165,840,496 bytes):**

| Copy | cubin SASS | gate 1 | gate 2 (Ada) |
|---|---|---|---|
| `SL 2.13/` (pristine) | **sm_120 only** | 4× `0x1b0` | **block** |
| `Downloads/` | sm_120 + **sm_89** | 3× `0x1b0`, 1× `0x190` | ALLOW (stub rewritten) |
| `dlssnr-remix/` | sm_120 + **sm_89** | 4× `0x190` | ALLOW (stub rewritten) |

The two patched copies differ from pristine by **13,555,158 bytes** — a wholesale substitution
of zstd cubin payload, i.e. real Ada SASS grafted in at constant file size, not a relabelling of
Blackwell kernels. They differ **from each other by exactly 3 bytes**, all gate-1 constants, so
**they are functionally identical for our direct-snippet path**; swapping between them cannot
change anything. Only the pristine `SL 2.13` copy is genuinely unusable on Ada.

Ada's rewritten gate-2 stub lands on the **same** target as Blackwell2 (`0x180017f2b`), so arch
selection past that point must happen by cubin lookup rather than by branch.

### NR runs, and then the GPU leaves the bus — a GSP crash, not an Xid (measured 2026-08-31)

First fully working NR session: `Init_Ext SUCCEEDED` → `feature 18 CREATED` → `NR VALIDATED` →
`applied=965 refused=64` (every refusal the warmup). It ran **~48 s / ~2800 frames at 57.6 fps**,
showed **red noise over the whole screen**, then the card vanished.

**The lazy-init change did NOT prevent the loss**, which refutes the earlier "initialising at
device creation causes the GPU losses" hypothesis — it is the **evaluate**. What lazy init *did*
fix is the startup stall: gameplay in 34 s, where the eager build left the ReShade log stopped
mid-`Init_Ext`.

**`dmesg` carries NO Xid whatsoever.** Every line is `_issueRpcAndWait: rpcSendMessage failed
with status 0x0000000f` and `rpcRmApiFree_GSP: GspRmFree failed` — GSP-firmware RPC failure. The
`gmmu_walk.c` / `mmu_walk.c` assertions and `mmuWalkUnmap: Failed to unmap VA Range` lines are
**teardown after the fact**: their status is already `0x0f` = `NV_ERR_GPU_IS_LOST`. Read the
first NVRM line, not the loudest one. A GSP crash and an Xid 109 CTX SWITCH TIMEOUT are
different failures and must not be triaged as one.

**Open, and the whole problem:** the output was `max luminance 0.002709` over the validation
crop — barely above the `1e-5` degeneracy threshold, i.e. very nearly black — while the screen
showed red noise. Leading suspects, in order: the parameters we never set (RenoDX ships
`NRPaperWhiteScale=1.605`, `NRDepthMode=2`, `NRPreset=1`, `NRAutoMask`, `NRUICorrection`,
`NREnableUpscaling=0`, `NRSkinStructure=1.33`; we set only intensity/localTone/localStructure),
and the **resolution mismatch** — we hand it 3840x2160 colour with 1920x1080 depth and motion
vectors, and if the runtime indexes depth at output resolution that is both the garbage image
and a plausible fault.

### DLSSNR's structure controls, read out of the binary (2026-09-01) — and the reset that fooled us

Subject: `nvngx_dlssnr.dll` md5 `eea91faf`, PDB `...\snippets\rel_310_8\source\features\dlssnr\`.
The feature's internal name is **CG2R**. All HARD, read from the disassembly and the decompressed
PTX unless marked.

**THE ONE THAT MATTERS IN PRACTICE: changing a structure strength WIPES THE TEMPORAL HISTORY.**
`CG2R_ResetTemporalHistoryOnControlChange` (`0x180017ad8`) compares the previous frame's controls
against this frame's with epsilon `1e-5` and sets `DLSSNR.Reset = 1` on any difference. In the
list: Style, UseAutoMask, LocalToneStrength, LocalStructureStrength, SkinStructureStrength, and
both resolved values. **`DLSSNR.Intensity` is NOT in the list.**

* Dragging a structure slider holds `Reset = 1` every frame, so accumulation never runs and **the
  whole screen changes** — which is exactly what we observed and nearly mis-diagnosed as "skin
  affects everything". Set the value, release, wait 1-2 s, *then* judge. The runtime logs
  `DLSSNR: reset temporal history for %s after control change` when it fires.
* **Use `Intensity` for "how much NR".** It is the only strength knob that does not cost a
  history reset.

**"Skin" is a GLOBAL SCALAR, not a mask.** The evaluate path passes the strengths only to the
`CCTinlayoutFusedPreBlockSwin1HLayer`, into its launch struct at `+0x98` (resolved skin) and
`+0x9C` (resolved local) — adjacent floats, same setter. The PTX
(`cc_tinlayout_fused_pre_block_swin_1h_32_1`, and its `_fp8`/`_ds` variants) converts them to
fp16 and writes them into the shared-memory input tile as channels alongside style and local
tone, **broadcast bit-identically to every pixel**. Five of the network's 16 input channels are
host tuning scalars. There is no mask, no classifier, no segmentation head, and no branch on
position anywhere.

**So `UseAutoMask` classifies nothing in code.** It is a boolean deciding whether the two strength
channels carry values or `-1`. Any *region* behaviour lives entirely in the trained weights.
**The word "skin" appears ONCE in the whole 165 MB binary — in the parameter name — and zero times
in 59 MB of decompressed cubins and PTX.** Whether it means literal skin is therefore
**unknowable from the binary**; NVIDIA's DLSS 5 material talks about learned skin/subsurface
behaviour, which is SOFT vendor marketing about the model, not about this parameter.

**The sentinel, from the code rather than the reference's docs:** the test is `comiss`/`jae`, so
**any** negative (and NaN) inherits local — it is a sign test, not `== -1`. The snippet's own
defaults are skin `-1.0f`, local `+1.0f`. Binding a `ControlMask` forces `UseAutoMask` to 0, and
`UseAutoMask = 0` drives both resolved values to `-1.0f`.

**`DLSSNR.ControlMask` is an RGB control texture** — R is the per-pixel final blend weight, G
scales local tone, B scales local structure. **It has no skin channel**, so skin is not
expressible per-pixel. The explicit path is per-pixel with no skin term; the auto path is global
and is the only place skin exists.

**`DLSSNR.ScalingRatio` is INERT** — read, then unconditionally overwritten with `1.0f` at
`0x18001a96a`. Neither it nor the absent `DLSSNR.Scale` ever mattered.

**A correction to the reference's option docs:** they say that with the auto mask off the snippet
forces `localStructureStrength` to -1 and it does nothing. **The RAW value still reaches the
network** — when `max(skin, local) < 0` the kernel feeds the flag channel from raw
`LocalStructureStrength` (`param_0+176`). Only the *resolved pair* is neutered.

**Method note, because it is the reason this took a whole investigation:** the confound and the
answer were both in the binary, and neither was in any documentation. When a knob "changes
everything", suspect the machinery around the knob before theorising about its meaning.

### Three ways to hand feature 18 something that hangs the GPU rather than erroring

All three were live in our code at the session's only successful NR run, and all three are now
closed. They share a shape worth internalising: **the neural runtime validates almost nothing,
so a malformed input is a hang or a garbage image, never a returned error.**

1. **A mipped output texture is a documented `DXGI_ERROR_DEVICE_HUNG`**, arriving *a few seconds
   after the neural pass starts*. `ensure_output_texture` did `D3D12_RESOURCE_DESC tex = src`,
   inheriting `MipLevels`, `DepthOrArraySize` and `SampleDesc` from whatever UE4 had allocated.
   The output is now always built as a plain single-mip, single-slice, non-MSAA 2D texture, and
   the source's real counts are logged so one run answers whether it ever mattered. A **mipped
   colour input** cannot be fixed by allocating our own texture and is now refused under a named
   reason (`mipped-input`) rather than passed through.

2. **An unknown parameter name is silently ignored** — an NGX parameter block is an untyped
   string→value map with no validation, so a stale name is indistinguishable from a value that
   was accepted. We wrote `DLSSNR.Scale`, which does not exist in the 310.8.0 runtime, so every
   scaling ratio we ever sent was discarded. The real name is `DLSSNR.ScalingRatio`. **Confirm
   every parameter name by exact null-terminated string search over the runtime binary before
   writing it** — and do not copy a name list off another *caller*, since RenoDX writes seven
   names this build does not implement. `docs/RESEARCH-RENODX-DLSS5.md` §2.2.1 carries the audit.

3. **Declaring a guide's subrect AND scaling its vectors double-counts.** `MVecSubrectWidth/
   Height` already state that the motion vectors live in their own 1920x1080 rect, so
   `MVecScaleX/Y` must be **1.0**, not the output/render ratio. Reference integrations all send
   1.0 and expect motion in pixels of the guide's own subrect.

**Also worth knowing before the next attempt:** the runtime exports
`DLSSNRComputeScalingRatioCallback` and `DLSSNRGetStatsCallback` as parameter names (verified
present in our binary) which `nvngx.dll` would normally populate with function pointers inside
the snippet itself. We leave them unset — guessing an RVA into a leaked DLL is not a bounded
risk — and at least one working third-party integration omits them too.

### The near-black NR output has a ROOT CAUSE: we fed a display-referred network raw HDR

> **SUPERSEDED 2026-09-03 — see "NR IS A PRESENT STAGE, AND THAT DELETED HALF THE FEATURE"
> above.** Correct, and the fix was a codec at a site that needed one. Moving the site removed
> the raw HDR instead, so the codec is gone rather than superseded by a better one. The
> reasoning is kept because it is the record of how the placement problem was found, one
> compensating mechanism at a time.

**Feature 18 is a DISPLAY-REFERRED image network.** It expects a [0,1], sRGB-encoded signal, and
we were handing it Stray's raw unbounded pre-exposed linear HDR straight off the TAA hook. That
is the "open, and the whole problem" from the section above: `max luminance 0.002709` with red
noise on screen is what an out-of-domain input looks like, not a broken runtime.

**There is no HDR, colour-space or exposure parameter anywhere in the runtime** (exhaustive
null-terminated string search over `nvngx_dlssnr.dll`), so the conversion has to happen in OUR
pixels, on both sides of the evaluate. Two compute dispatches now wrap the NGX call:

```
encode:  proxy  = SrgbEncode(SoftClip(max(image,0) * s))      -> DLSSNR.Color = the PROXY
evaluate:                                                        -> our neural texture
decode:  image  = image + (SrgbDecode(neural) - SrgbDecode(proxy)) / s
```

Ported from the only known working, 4090-tested deployment of this codec —
`github.com/lunks/dxvk-remix-plus-dlssnr` commit `aa90a180`, with `calcProxyScale` from
`fc4de144`. Every constant is that tree's. **HARD.**

**The transfer is a RESIDUAL, and that is the whole design.** `n == p` gives a delta of exactly
+0.0, so a no-op network returns the frame bit for bit — pinned in CI across scales and
radiances (`tests/test_nr_codec.cpp`, "EXACT IDENTITY"). HDR headroom survives because the
original is added to, never reconstructed: a pixel at radiance 100 stays near 100 where a naive
`SrgbDecode(neural)/s` would have crushed the frame into SDR. Below the knee it reduces to
`n/s`, i.e. the network's answer verbatim. The full derivation is in `src/core/nr_codec.hpp`
above `decode`; read it before touching either shader.

**Consequences and traps:**

* **The codec is MANDATORY, and `NgxNRTopology=sr` now refuses** (`codec-topology`). The residual
  needs the proxy, the neural answer and the original to be the same pixels; sr-shaped puts
  colour at render res and output at display res, so no residual exists. Falling back to the raw
  HDR path would be silently reverting to the broken configuration.
* **The copy-back is gone.** It was a full-RGBA `CopyResource`, which discarded the HDR range AND
  overwrote alpha with the network's meaningless one — and on this title that resource becomes
  the next frame's TAA history (§2.9), so the engine read the damage straight back in. The decode
  writes RGB in place and carries the original alpha.
* **Soft clip: knee 0.75, shoulder 5.770780, C0 but NOT C1** (the slope jumps to 1/ln2 at the
  knee). That is what the working deployment ships. Reproduce, do not "fix".
* **Exact piecewise sRGB, both directions.** Not an x^2.2 approximation — the network was trained
  on true sRGB.
* **MEASURED: in float32 the soft clip reaches exactly 1.0 at an input of ~3.474.** Anything
  brighter than ~3.5x display white encodes to pure white, so a paper white that leaves the frame
  up there hands the network a flat clipped image.

**`[STRAYDLSS] NgxNRPaperWhiteScale`, default 1.0.** The shader multiplier is `1/paperWhite`
(`calcProxyScale`: `staticExposure / max(paperWhiteScale, 0.01f)`, staticExposure 1), bounded to
the reference's own [0.01, 64]. The reference multiplies this by its tonemapper's auto-exposure
texture; **we have no such texture at a TAA-dispatch hook**, so this is the whole scale.
**Values below 1.0 are legal and are the likely direction here** — `scale = 1/paperWhite`, so
raising it multiplies the colour DOWN, and Stray's scene colour already carries UE4's
pre-exposure (§2.6 row 135.y, ~0.056 measured live). The 16.0 that appears in the reference's
option help is a remark about what a RenoDX-style fixed scale "would use", not a measured
default; RenoDX ships 1.605, and paper white above 1.0 reportedly does nothing in its HDR path.

**Do not guess the value — one log line picks it.** `NR CODEC LUMINANCE` reports the max Rec.709
luminance of the colour INPUT, the encoded PROXY and the neural OUTPUT over the *same* 128x128
centre crop, all linear (the proxy and output are sRGB-decoded first, so they are directly
comparable to the 0.75 knee), plus the effective scale and the paper white that would put this
frame's peak exactly at the knee. Below the knee the codec is just a multiply, so `proxy` should
be about `input x scale`; a proxy far below the knee means paper white is too HIGH, a proxy
pinned at 1.0 means it is too LOW, and a healthy proxy with a black output means the fault is the
runtime rather than the codec. Also added: `NgxNRColorStrength` (0 keeps the original's
chromaticity and transfers only the network's luminance change — the escape hatch for a colour
cast) and `NgxNRTransferStrength` (0 is an EXACT bit-for-bit bypass, so it is the honest A/B).

### Gotchas ledger — hard-won, 2026-08-31, all measured

**Diagnosing "DLSS runs but nothing changes":** the debugging ladder that finally worked, in
order, each step decisive where the previous was blind:

1. **`NgxPaint=1`** clears the captured output to magenta instead of evaluating. Magenta on
   screen proves the output handle, the suppression and the downstream chain in one shot —
   and isolates the fault to the evaluate itself. (It did: the screen went purple.)
2. **`NgxDumpInputs=1`** copies the exact colour/depth inputs (pre-evaluate) and the output
   (post-evaluate) to `straydlss_*.bin` at evaluates 600 and 900. Compare md5s across the two
   samples: changing inputs with a byte-identical output = NGX is not writing. The all-zero
   output proved it had *never* written. Convert with `tools/rawdump2png.py` to see what DLSS
   sees. Readback pattern (copy on the game's list, map 5 presents later) works under vkd3d.
3. **`NVSDK_NGX_D3D12_EvaluateFeature` returning success proves nothing.** The cubin layer
   logs its own failures through the LoggingCallback — grep the ReShade log for
   `NGXCubinD3D12` and `nvapi status`. Status -5 on `GetCudaMergedTextureSamplerObject` /
   `GetCudaIndependentDescriptorObject` is the ReShade ext-vtable patch (§1); the repair is
   `ext_unhook` (default ON).

**Camera-cut frames must be evaluated with `InReset`, never skipped.** Skipping let the
engine's TAA run against DLSS-written history on ~4% of frames — that is the "flicker between
a frozen image and fog" symptom. Post-fix, evaluates track dispatches at 99.7%.

**The menu runs the TAA pass too**, at uncapped fps, and its scene colour is
`R11G11B10_FLOAT` where gameplay's is `R16G16B16A16_FLOAT` — the colour format is NOT an
invariant; trust the §2.3 register map, not a format check. Menu depth reads ~0 everywhere
(the §2.4 gameplay gate), which is how a dump run that never left the menu was identified
after the fact.

**DLSS presets J, K and M are visually indistinguishable here** (user-judged, same scene,
2026-08-31): the white specular sparkle is denoiser residual, not preset-sensitive.
`NgxPreset` (10=J 11=K 12=L 13=M) stays for future models. Next levers for the sparkle: feed
the engine's real exposure instead of the AutoExposure flag; ultimately DLSS Ray
Reconstruction (a separate project: needs albedo/normal/roughness capture and nvngx_dlssd).

**The DLSS on-screen indicator under `PROTON_NVIDIA_LIBS=1`:**
`DXVK_NVAPI_SET_NGX_DEBUG_OPTIONS` is a DXVK-NVAPI feature and does nothing when NVIDIA's own
wine NGX libs provide the NGX core (measured: env var present in the game process, no
indicator drawn). Set the Windows registry key in the prefix instead:
`HKLM\SOFTWARE\NVIDIA Corporation\Global\NGXCore\ShowDlssIndicator = dword 0x400`, with the
wineserver down.

**Steam launch options can be set programmatically** through steamwebhelper's CEF debugger
(`tools/steam_cef_launchopts.py`: port 8080, `SharedJSContext` target,
`SteamClient.Apps.SetAppLaunchOptions`). The debugger is live when
`~/.steam/steam/.cef-enable-remote-debugging` exists (Decky installs it). **Always read the
existing options from `userdata/<id>/config/localconfig.vdf` first and merge** — Stray's
`WINEDLLOVERRIDES="dxgi=n,b" %command% -dx12` is what loads ReShade; clobbering it silently
removes the add-on.

**ReShade config cannot deliver comma-separated values to an addon** (measured via the probe
build): the ini parser stores `a,b,c` as a multi-element array and the addon-facing char getter
returns ONLY element 0 while reporting the full value's size. Any list must go through a
sidecar file (`stray-dlss-dryrun.txt`, `stray-dlss-hashes.txt`), never the ini. Related:
killing the game and immediately rewriting the ini races ReShade's exit flush, which writes its
loaded config back over fresh edits — always wait for the process to be fully gone (plus a few
seconds) before touching ReShade.ini.

**Loading screens are where the resolve path dies.** They produce structural TAA look-alikes
over violently short-lived resources; one passed the liveness gate at capture and was destroyed
before `CreateShaderResourceView` (the §5 vkCreateImageView crash — log truncates at "about to
view velocity"). The resolve/evaluate machinery is therefore gated on `is_known_taa_hash` —
unknown hashes never reach view creation.

**`/tmp` on the box does not survive reboots** and it used to carry the launch tooling —
`tools/launch-stray.sh` is now committed and must be re-staged (`scp` + `pct push`) after any
box reboot, along with `measure.py` and the pad helper. When driving the menu by injected
input, parse `/proc/bus/input/devices` handler tokens AFTER splitting on `=` — the raw line's
first token is `Handlers=eventN`, which does not start with "event" (this exact bug produced
"pad not found" against a present pad).

**Box operational traps:** the games mount moved from `/mnt/GamesLinux` to
`/run/media/deck/GamesLinux` between sessions — a path that worked yesterday can be an empty
stub today; `deploy.sh` writes only the keys it is passed, so stale `[STRAYDLSS]` keys
persist across deploys (pass every key, every time); always `pkill -x Stray-Win64-Shi`
before deploying (`pgrep -f` self-matches its own ssh command line — use `-x`); after a log
check, confirm the timestamps are from the NEW session — greps happily match the previous
session's lines.

**The pak's shader library:** Stray ships **no** `.ushaderbytecode` — shader code libraries
are disabled and all global shaders (every TAA permutation included) live inside
`Engine/GlobalShaderCache-PCD3D_SM5.bin`, which is **Oodle-compressed at the pak level**
(method 2; footer names: 1=Zlib 2=Oodle). Pure Python cannot decode it: use
`tools/pakextract.py --raw` (dumps compressed blocks + JSON map) then
`tools/oodle_unblock.py` with an `ooz` build. **Pak entry field-order trap:** when the
block-size code in the encoded-entry flags word is 0x3F, the explicit CompressionBlockSize
uint32 sits IMMEDIATELY after the flags word, before Offset — decoding it late shifts every
later field, and only bites on large-block entries (small zlib entries decode fine either
way, which is why the bug survived).

### The UE4SS migration (2026-09-01): what is measured so far

Facts in `docs/STRAY-RENDERING-FACTS.md` §11-§15; the plan in
`docs/superpowers/plans/2026-09-01-dlss-sr-ue4ss-plugin.md`. The load-bearing ones:

* **vkd3d-proton shares ONE vtable per class** (command list, queue, resource) and
  **`SetPrivateDataInterface` releases at destruction** — both HARD from the harness on the box.
* **UE4SS starts its C++ mods ~970 ms before the game's `D3D12CreateDevice`**, with `d3d12.dll`
  not yet loaded (HARD, from `ReShade.log` and `UE4SS.log` of one session).
* **The game destroys and recreates its first D3D12 device at startup, and ReShade unloads and
  reloads the add-on across it.** A process-global vtable patch that outlives the DLL it points
  into is an address-0 crash after the reload (measured, root-caused, fixed: the native backend
  restores its slots on detach). Never patch a vtable from a DLL that can be unloaded without
  undoing it.
* **`QueryInterface` on ReShade's proxy device from inside its `init_device` callback deadlocks.**
* **The native backend's resolve agrees with ReShade's on every register of the TAA pass** (§15);
  the residual disagreements are root CBVs into UE4's constant ring buffer that the native
  registry misses, and buffer SRV/UAV slots where the two trackers name different buffers —
  the oracle's known stale view→resource map is the leading (SOFT) explanation for the latter.
* **The UE4SS plugin cannot be BUILT without a `UEPSEUDO_PAT`** at UE4SS SHA 68caddcf: the public
  mirror of the Epic-gated `UEPseudo` tree is two headers behind.

### DLSS Frame Generation without Streamline: the present-twice design (2026-09-02; MEASURED on the box the same day, facts §32.7-32.10)

Facts in `docs/STRAY-RENDERING-FACTS.md` §32. The parts that decide everything:

* **`nvngx_dlssg.dll` has no present path, no queue and no pacer** (§32.1, HARD from the
  strings): `EvaluateFeature` records the interpolation onto the list it is handed. Everything
  Streamline's `sl.dlss_g` did around it — own the swapchain, present twice, pace, hand the
  interpolated frame over — is ours, in `src/backend_native/fg_present.cpp`, and Streamline is
  never loaded (user constraint: its swapchain layer is where OptiScaler's FG died here).
* **The game is handed REPLACEMENT back buffers** from a hooked `IDXGISwapChain::GetBuffer`
  and never touches the real ring. UE 4.27 never calls `GetCurrentBackBufferIndex`; it keeps
  its own counter (+1 per Present, 0 after every Resize, `GetBuffer(i)` for every index after
  a resize — §32.4, HARD from source), so `core::fg::GameIndexMirror` reproduces it and tells us
  which replacement holds this frame. A mirror error would be a STALE presented frame — the
  stage-1 screenshot protocol (magenta band on the generated frame) is what would show it.
* **Two presents per game frame**: copy generated → real[current], Present; wait the pacer's
  half interval on a worker thread; copy real → real[current], Present. `NgxFGPacing=0` is the
  back-to-back control (the issued-interval histogram goes BIMODAL, which the `[fg]` line
  reports), `=2` keeps everything on the game thread for bisection. `ResizeBuffers`,
  `SetFullscreenState` and `ResizeTarget` drain the worker and bump an epoch first — the
  fullscreen transition is where OptiScaler died, so it is the stage-1 test's point.
* **A generated frame never reaches the screen unvalidated** (prime directive 2): in ngx mode a
  64x64 crop of the generated and the real frame is read back three presents later and a black
  or stale (real moved, generated did not, 3x) output REVOKES presenting it, loudly. Every
  refusal is counted by reason (`[fg]` line, `fg_*` in the status file).
* **Every `DLSSG.*` name written is in the snippet's exact null-terminated string set**
  (§32.2, `tools/ngx_param_names.py`); the SL-only names (`EnableInterp`, `CmdQueue`, the sync
  callbacks, `IsRecording`) are absent from the snippet and not written. Re-run the tool on the
  box's SL 2.13 copy before trusting the list there.
* **Stage 1 (`NgxFG=1 NgxFGMode=1`) needs no NGX**: the generated frame is the previous real
  frame under a magenta band. Stage 2 (`NgxFGMode=2`) plugs `src/ngx_fg.cpp` in as the generator.
  Stage 3 (HUD-less) is deferred until the user has inspected stage 2 on the box.
* **Unverified conventions, in the order a wrong one would show:** `DLSSG.MvecScaleX/Y =
  1/renderW, 1/renderH` for our pixel-space vectors (SL guide §7.0), UE4's row-major
  row-vector matrices passed as SL's "row-major" ones, the camera plane pair,
  `ColorBuffersHDR` from the swapchain format + colour space. Each is a knob
  (`NgxFGMvecScale`, `NgxFGCameraNear`/`NgxFGCameraFar`, `NgxFGHDR`) so the box can A/B
  without a rebuild.

  > **CORRECTED 2026-09-03.** This line used to read "`CameraFar = 0` for the infinite
  > reversed-Z projection". **`0` was never a way of saying "infinite" — it was our
  > invention.** `sl_consts.h` makes `cameraNear`/`cameraFar` non-optional with
  > `INVALID_FLOAT` as the unset sentinel (only `clipToLensClip`, `cameraPinholeOffset` and
  > `motionVectorsInvalidValue` are Optional), and Streamline has a status bit for the fault,
  > `eFailCommonConstantsInvalid`. So `0.0` is a degenerate near-zero depth RANGE that DLSS-G's
  > camera reconstruction divides by. NVIDIA, facing the same infinite far plane, does not try
  > to signal infinity at all: `r.Streamline.CustomCameraFarPlane` is a literal `75000.0f`,
  > byte-identical on the UE 4.27 plugin (our exact engine version) and on UE 5.8's 8.7.2, and
  > its help text says it "does not need to match corresponding value used by engine".
  > **Defaults are now NVIDIA's PAIR — near `0.01`, far `75000.0`** — because they are shipped
  > as a matched synthetic pair and mixing our measured `View.NearPlane` with their far is a
  > combination they never test. "More faithful to the engine" is not a virtue for a value the
  > vendor documents as not needing to match it. HARD on what NVIDIA sends; **UNCONFIRMED that
  > it changes an interpolated frame**, since `nvngx_dlssg.dll` is closed.
* **Reflex goes through DXVK-NVAPI's `NvAPI_D3D_*` by function id** (`src/backend_native/
  fg_reflex.cpp`), never `sl.reflex`; every status is logged and nothing gates on it. On the
  box all five entry points exist and `SetSleepMode`/`Sleep`/`SetLatencyMarker` return
  `NVAPI_OK` (HARD); what they do under vkd3d-proton is UNCONFIRMED.

**What the box settled on 2026-09-02 (facts §32.7-32.10), all HARD:** the present-twice path
survives `SetFullscreenState(TRUE)` + `ResizeBuffers` (OptiScaler's wall) and three checkpoint
reloads at exactly 2.00x presents per game present; the NGX core under Proton routes feature
11 to the game-directory `nvngx_dlssg.dll` and `CreateFeature`/`EvaluateFeature` succeed with
our own parameter block, no Streamline in the process; the interpolated frame differs from the
real one and reaches the screen through the crop gate. Three things only the box could teach:

1. **`DLSSG.MultiFrameIndex` is 1-BASED.** Index 0 makes every evaluate `FAIL_InvalidParameter`
   and the snippet says why on the LoggingCallback ("Found index (0) but expected (1)").
2. **Stray never calls `SetColorSpace1`**: its `R10G10B10A2` back buffer is UE4's 10-bit SDR
   default, so `DLSSG.ColorBuffersHDR=0` (the auto rule needs the PQ colour space to say HDR).
3. **The crop gate must treat a black REAL crop as a neutral look**, or every loading screen
   revokes FG and re-validates three looks later (3 325 refused presents in one session);
   and it must remember two looks, because the two alternating output textures of a
   generator that writes nothing read as motion to a one-look memory (caught in CI).

4. **Pacing is a clock, not a delay, and Wine's `wait_for` wakes ~1 ms late** (facts §32.11):
   the per-present trace (`NgxFGTrace`) showed the real frame at +7.15 ms after every hook
   for a 6.1 ms hold, a 7.1 / 5.0 rhythm the user felt on the 165 Hz VRR panel. Now the pair
   is scheduled against the previous REAL present (`core::fg::Schedule`: generated at
   `last_real + I/2`, real at `last_real + I`, `I` the median of 16 game intervals) and the
   last 1.5 ms of each hold are spun; the display receives 6.0-6.2 ms every gap.

Steady-state gameplay is 2.00x; the only refusals are NGX's lazy init (~900 frames), ~60
frames per checkpoint reload (no TAA dispatch, so no guides) and 0-3 per 600 otherwise. Every
fps and pacing number from these sessions is SUSPECT (host CPU contention) and the pacing
histogram's BIMODAL flag is a known false positive under an irregular source; re-read both on
a quiet host. HUD-less (stage 3) is deferred until the user has judged stage 2 in motion.

## 6. Build, CI and testing

* **CMake + GitHub Actions, MSVC, x64 only.** No local build. `windows-latest` is now Windows Server
  2025 + VS 2026; MSVC v143 (14.44) is still available via `-T v143,version=14.44`.
* **Build Release `/MD` and link `nvsdk_ngx_d.lib`.** The NGX libs are real static libraries that
  expose C++ std types across the ABI — a mismatched `_ITERATOR_DEBUG_LEVEL` produces link errors.
* Warnings are errors, `/W4`.
* **Precompile every HLSL shader to DXBC at build time. Never call `D3DCompile` at runtime** —
  `d3dcompiler_47` is frequently absent from a Proton prefix, and this is OptiScaler's single most
  common Linux failure.
* **Unit-test every pure function in CI**: fnv1a64 over DXBC; the UE4 velocity decode against the
  measured constants; the jitter path; `ClipToPrevClip` reconstruction; quality-mode selection;
  Halton; mip bias. Build them for Linux too so they run fast.
* Ship PDBs as artifacts (`/Zi /DEBUG /OPT:REF /OPT:ICF`).
* **Logging is the product's second output.** Version banner, ReShade version, vkd3d detection,
  device API, every detected shader hash, the chosen TAA hash *and why*, the raw View CB rows, the
  computed jitter and MV scale, feature flags, NGX result codes by name, every state transition.
  The user pastes this back; it must diagnose without a second round-trip.
### The three test lanes, and what each can actually prove

| Lane | Runs | Proves | Blind to |
|---|---|---|---|
| Unit (Linux, doctest) | every push | pure functions: hashing, matrix math, jitter, velocity decode | anything touching D3D12 |
| **WARP** (Windows CI) | every push | our D3D12 usage is *legal* — the debug layer and GPU-based validation judge it | vkd3d-proton's behaviour; NGX |
| **Real ReShade** (Windows CI) | every push | the add-on loads into the **shipped ReShade 6.8.0.2155** and its events fire; every harness test runs through ReShade's *real* device and command-list proxies | NGX; the game's actual bindings |
| **Hardware** (`tools/run-harness-proton.sh`) | by hand | our D3D12 usage *behaves* on the real driver through real vkd3d-proton | validation: vkd3d implements no `ID3D12InfoQueue`, so every "no validation errors" assertion is vacuous there |

They are complementary and none of them replaces seeing the game render.

* **The hardware lane needs a private display and a private Wine prefix.** Reusing the game's
  `DISPLAY` freezes it instantly: that display is gamescope's nested X server, Wine creates
  windows as soon as `winex11.drv` initialises, gamescope focuses the newest toplevel, and a
  harness that never presents leaves the compositor stuck on the game's last frame. It reads
  exactly like a game hang and is not one. Reusing the game's prefix joins its `wineserver`.
  The script uses `xvfb-run -a` and its own compatdata; keep it that way.
* **Wine gives the harness no console**, so its stdout is lost. The script redirects inside
  Windows via `cmd` and reads the file back.
* **A "no validation errors" assertion is only evidence if the detector can fire.** The harness
  therefore contains a negative control that deliberately binds a descriptor table to a
  root-CBV parameter and requires the debug layer to report
  `D3D12_MESSAGE_ID_SET_DESCRIPTOR_TABLE_INVALID = 708` (verified in `d3d12sdklayers.h:2726`
  and reproduced in our own CI). That is the mistake ReShade's own `state_block` makes.
* **ReShade attaches its add-on path inside the hooked `D3D12CreateDevice`**
  (v6.8.0 `source/d3d12/d3d12.cpp:40`), *not* at swapchain creation or `Present`, and it refuses
  WARP only in its D3D10/D3D11 paths (`d3d10.cpp:159`, `d3d11.cpp:150`) — there is no adapter
  check anywhere in `source/d3d12/`. **So a headless console app with no window and no swapchain
  is enough to load real ReShade and the real add-on.** Dropping the shipped DLL in as
  `d3d12.dll` makes our own `D3D12CreateDevice` return ReShade's proxy, so every harness test
  then runs through its descriptor-heap wrappers and handle conversion — the code §1 flags as
  having no release-build validation.
* The setup exe is a **self-extracting zip** (`setup/MainWindow.xaml.cs:987`).
  `tools/extract_reshade.py` pulls `ReShade64.dll` out of it; .NET's `ZipArchive` refuses the
  prepended PE, Python's `zipfile` does not.
* **Registering is not receiving.** The add-on logs a census on detach, and CI fails if it saw
  zero compute pipelines or zero dispatches — that is the automated form of the §5 warning that
  "wrong ReShade build" is otherwise indistinguishable from "the game binds differently".
* **The ReShade half of the restore is tested by a fake `command_list`**
  (`tests/warp/fake_reshade_command_list.hpp`). The interface is pure abstract, `state_tracking`
  is a plain struct, and `bind_descriptor_tables` is a defaulted forwarder to pure-virtual
  `bind_descriptor_tables2` — so overriding one records both. `get_native()` returns the
  harness's *real* command list, so the native calls execute and are validated for free.
* Give the user one copy-pasteable launch line for bug reports:
  `DXVK_NVAPI_LOG_LEVEL=info PROTON_LOG=1 VKD3D_DEBUG=warn %command%`. For visual proof DLSS
  is running, the env-var indicator does NOT work under `PROTON_NVIDIA_LIBS=1` — set the
  `NGXCore\ShowDlssIndicator = 0x400` registry key in the prefix instead (Gotchas ledger, §5).

---

## 7. House rules

* Follow the Superpowers process skills: `brainstorming` before creative work,
  `systematic-debugging` before proposing a fix, `test-driven-development` for anything with a
  testable core, `verification-before-completion` before any claim of doneness.
* No speculative features. Ship DLAA correctly before anything else exists.
* Keep files focused. Luma's 17k-line `core.hpp` is a thing to learn from, not imitate.
* Every constant from §2 gets a **named symbol with a comment citing §2.x**, never a bare literal.
* When observation on the user's machine contradicts §2, **update §2 and
  `docs/STRAY-RENDERING-FACTS.md` in the same change** that reacts to it.
* Never commit `nvngx_dlss.dll` or game assets. Redistribution is permitted only as part of an
  application with material additional functionality, never stand-alone — CI fetches it.
