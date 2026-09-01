# stray-dlss — agent operating manual

A ReShade **add-on** (not an effect/shader preset) that injects **NVIDIA DLSS Super Resolution**
into **Stray** (Unreal Engine 4.27.2, **D3D12**) by intercepting the engine's own temporal AA
compute dispatch and replacing it with an NGX evaluation.

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
percentage, i.e. `ETAAPassConfig::MainUpsampling`. At 1:1 the engine selects
`ETAAPassConfig::Main`, a **different permutation** whose bindings we never characterised. The
matcher is not broken; it is matching a shape the game no longer produces, and the passes it
does find (which do exhibit the history round-trip) are other temporal effects.

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
> percentage share a hash and differ only in dispatch size; going from 50% to 100% flips
> `AA_UPSAMPLE` and compiles different code. That is why `0x901e…` declares a
> `dcl_tgsm_structured` `float4[64]` that exists only under `AA_UPSAMPLE == 1`. Never gate on it alone; the depth+stencil-over-one-resource signature plus a
> dispatch covering the output rect is what actually identifies the pass. Also measured in the
> same frames: the View CB appeared at **b4**, not b1, so that register is not invariant either.

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

**Reading the pak.** It is version 11, **unencrypted**, 5.3 GiB, 55,120 entries.
`tools/paklist.py` lists it and `tools/pakextract.py` extracts by regex, both reading only the
index blobs rather than the archive — worth knowing, because a bulk copy is neither necessary
nor kind to a machine someone is playing on.

**Shader census:** 728 distinct PS/CS shaders in gameplay, `not_dxbc=0`, `dxil=0` — every one is
**DXBC**. ~150 in the main menu, rising to ~728 on entering gameplay.

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
| `PreExposure` | 135.y | 2164 | [derived] |
| `OneOverPreExposure` | 135.z | 2168 | [derived] |
| `NearPlane` | 142.x | 2272 | [derived] |
| `DeltaTime` | 143.x | 2288 | [derived] |
| `CameraCut` | 145.x | 2320 | [derived] |
| `TemporalAAParams` | 152 | 2432 | measured |

Everything we need is in a single **2448-byte prefix**. **Rows beyond 152 were not verified — do
not use them.**

`ClipToPrevClip` at row 122 was confirmed **in Stray's own TAA shader by pure DXBC instruction
analysis**, no reflection names involved.

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

**[derived]** In the shipped (non-upsampling) mode UE4 warps Halton through Box-Muller with
`sigma = 0.47 * r.TemporalAAFilterSize` — a distribution DLSS was **not** trained on. Forcing
`r.TemporalAA.Upsampling=1` switches to plain Halton in `[-0.5, 0.5]`, which is what DLSS *was*
trained on. See §4.

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
* Writing `input_event` structs directly to `/dev/input/eventN` reaches `input_inject_event()` and
  is seen by every reader. No `uinput`, `ydotool` or `evemu` needed. Neither the pad nor the
  keyboard node is `EVIOCGRAB`'d.
* ReShade's screenshot bind is `KeyScreenshot=44` (`VK_SNAPSHOT`) = Linux `KEY_SYSRQ=99`. Injecting
  it on the real keyboard node makes ReShade write a 4K PNG into the game directory. **This is our
  screenshot channel for visual verification.**
* gamescope's `SIGUSR2` screenshot produced no file. `ffmpeg`'s `kmsgrab` cannot read its
  framebuffer (`XB30`, 10-bit HDR).

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
  needs); RT is a separate switch and DLSS/RR do not need it.
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
