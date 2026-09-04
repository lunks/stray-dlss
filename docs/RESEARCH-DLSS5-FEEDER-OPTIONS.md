# DLSS5-Feeder and Deep Fried Chicken: what their options actually do

**Question asked:** the user follows `jlrouzies-fr/DLSS5-Feeder` and asked about its newer options,
naming **"deep fried chicken"** and **"post processing only"** — *"check what it disables/enables —
I have a feeling it might be doing more than what we do."*

**Answered against:** `jlrouzies-fr/DLSS5-Feeder` @ `7927553` (2026-09-03, the repo's tip at the time
of reading; releases up to `v0.13.0-beta.1` checked), and the Deep Fried Chicken 1.4.0 / 1.4.4 /
1.4.8-alpha documentation vendored under `external/deepfried/`. Baseline for "do we already do it"
is this project at `ec0ce00`, `src/ngx_nr.cpp`, `src/nr_hook.cpp`, `src/nr_stage.cpp`,
`docs/RESEARCH-RENODX-DLSS5.md` and `docs/RESEARCH-DLSSNR-STYLES.md`.

---

## 0. The framing correction that has to come first

**Neither of the two things the user named is a DLSS5-Feeder option, and the feeder writes no
`DLSSNR.*` parameter at all.**

* **"Deep fried chicken" is not an option — it is a separate closed-source add-on.** Deep Fried
  Chicken (by "Alexander", Discord-distributed, no public source) is the *neural consumer*. Since
  0.11.0-beta.1 (2026-09-02, release `v0.11.0-beta.1`) it is the feeder's **recommended default**
  consumer, replacing Krish's `renodx-dlss5.addon64`
  (`DLSS5-Feeder/README.md:59-118`). It is the thing that calls NGX feature 18.
* **"Post processing only" is a Deep Fried Chicken checkbox**, labelled
  `Post-process: restore native tone & color, keep neural detail`, documented as *Native Look
  detail-only output* (`external/deepfried/v1.4.8-alpha/README.md:333-353`,
  `QUICK-START.md:139`). It is an output-composite mode, not a hook-placement mode.
* **The feeder itself does the SR half of what we do, not the NR half.** It manufactures a
  synthetic DLSS **feature 1** (SR/DLAA) contract for games that ship no DLSS, so a neural add-on
  that detours feature 1 has something to detour. A grep of the whole feeder source for
  `DLSSNR.` returns **nothing**; every `NVSDK_NGX_Parameter_*` it touches is DLSS-SR
  (`src/dlss5-feed.cpp:2526-2530`, `:2587`, `:2709-2712`). Its role maps onto **our TAA hook +
  SR evaluate**, not onto `src/ngx_nr.cpp`.

So the comparison the user is really asking for is **our NR path vs Deep Fried Chicken**, with the
feeder as the transport that puts DFC in the same *placement* we are in.

### Evidence quality, stated up front

| Source | Label |
|---|---|
| Feeder C++ source, its shader, its config table | **HARD** — read in source |
| DFC *documentation* (README / QUICK-START / RELEASE-NOTES / COMPATIBILITY-TEST) | **SOFT** — the author's prose about a **closed binary**. No DFC source exists in the repo; only docs are vendored |
| DFC's exported interop ABI values | **HARD** — the feeder author recovered them from the binary and says so (`src/feed_dfc.h:20-27`) |
| Our own runtime facts (`nvngx_dlssnr.dll` 310.8.0 disassembly) | **HARD** — CLAUDE.md §5, `docs/RESEARCH-RENODX-DLSS5.md` |

**Every mechanism claim below marked SOFT is the DFC author's own description of code nobody
outside has read.** Do not treat any of it as verified, and specifically do not assume the author
verified that a parameter name exists in the runtime — that is the `DLSSNR.Scale` lesson, and
nothing in their docs shows a string audit.

---

## 1. The option table

Ordered by how much it could matter to us.

| Option (whose) | Mechanism | Do we do it? | Does our runtime support it? | Verdict |
|---|---|---|---|---|
| **Native Look / "post processing only"** (DFC) | Output composite, not an NGX parameter: compare final neural luminance against the retained proxy, clamp the relative change to ±0.50 stops, apply it as a **scalar gain** to the untouched native frame; discard equal-luminance chroma change. **SOFT** | **No — mechanism we have no equivalent of.** Our present stage replaces the back buffer wholesale | N/A — pure shader arithmetic on our side of NGX | **The one genuinely interesting idea.** Cheap, placement-compatible, testable. See §2 |
| **Passes 1–30** (DFC) | N sequential DLSSNR evaluates chained in one output-resolution colour domain; each pass its own feature handle and its own temporal history; one shared codec wraps the whole chain. **SOFT** | **No — mechanism we have no equivalent of.** We evaluate feature 18 exactly once per frame | Nothing forbids N features. Cost is ours to pay | Plausible but expensive, and their own **Clean Fry** exists to clean up what it produces. See §3 |
| **Neural work scale 10–150%** (DFC 1.4.4+) | Run the neural work at `ceil(display × pct/100)` and reconstruct to native output. **Their own resample — not `DLSSNR.ScalingRatio`.** **SOFT** | **No.** Our stage is 1:1 on the back buffer | **Consistent with our HARD finding** that `DLSSNR.ScalingRatio` is read then overwritten with `1.0f` (`0x18001a96a`) — feature 18 cannot upscale, so any scaling must be the caller's | A perf knob. Real, but it moves the colour/guide ratio, which our own guide-extent latch says costs a history reset. See §4 |
| **Clean Fry** (DFC 1.4.4+) | Spatial guard for ≥2 passes: 3×3 YCoCg range envelope + log-luminance residual, moving each pixel only along its own proxy→neural line. Cleanup Strength ~0.35, Detail Retention ~0.85. **SOFT** | **No** | N/A — our-side shader | Only meaningful if multi-pass lands first. Its existence is evidence the cascade needs it |
| **NR Preset: Default / #1 / #2 / #3** (DFC per pass) | `DLSSNR.Hint.Render.Preset`, four values | **Yes — same parameter.** We ship `g_preset = 1` (`src/ngx_nr.cpp:187`, RenoDX's value); DFC's golden default is `Default` | **Present in the runtime string set** (`RESEARCH-RENODX-DLSS5.md:174`) | **Likely inert on both sides.** The shipped DLL is reported to contain a single network (`RESEARCH-RENODX-DLSS5.md:519-521`, web-sourced). A four-way selector over one network is a knob shaped like a feature |
| **NR Style: Default / Natural / Cinematic** (DFC per pass) | `DLSSNR.Style` = 0/1/2 | **Yes — same parameter, same default.** `g_style = 0` (`src/ngx_nr.cpp:193`) | **Yes**, and DFC's three-way naming **independently corroborates** our `docs/RESEARCH-DLSSNR-STYLES.md` mapping, which was previously SOFT off a single fork's menu text | No gap. Corroboration is the value here |
| **Intensity / LocalTone / LocalStructure / SkinStructure** (DFC per pass) | The four strength floats | **Yes — same four parameters, different values.** Ours `1.0 / 1.74 / 1.0 / 1.33`; DFC golden `2.0 / 2.0 / 2.0 / 2.0` | Yes | Different taste, not a different mechanism — **but see §6, where their own default neuters two of the four** |
| **Automatic Mask** (DFC per pass) | `DLSSNR.UseAutoMask` | **Yes — same parameter, opposite default.** Ours `1`, DFC golden **off** | Yes | See §6. Ours is the configuration in which skin/local-structure actually reach the network |
| **NR UI Correction** (DFC per pass) | `DLSSNR.UICorrection` | **Yes — same parameter, opposite default.** Ours `1`, DFC golden **off** | **Yes, but we measured it INERT for us**: both consumers require a bound `DLSSNR.Backbuffer` (`0x180019016`, `0x18001cbec`) and we bind none (`src/ngx_nr.cpp:118-129`) | No gap. DFC ships it off; ours is on and doing nothing. Cosmetic |
| **Depth Convention: game flag / force normal / force inverted** (DFC per pass) | `DLSSNR.DepthInverted` override | **Yes — same parameter**, driven from UE4's reversed-Z fact rather than exposed as a knob | Yes | No gap. A knob we do not need because we know the answer (CLAUDE.md §2.4) |
| **Motion Scale X/Y multipliers** (DFC per pass) | `DLSSNR.MVecScaleX/Y` multiplier | **Yes — same parameters, same value.** We send `1.0` with subrects declared | Yes | No gap, and our own gotchas ledger says why 1.0 is the only correct value once subrects are declared |
| **Shared colour transfer: Paper White 1.765 / HDR Transfer 1.0 / Colour Strength 1.0** (DFC) | The RenoDX-derived soft-clip + sRGB proxy + residual codec, wrapping the whole pass chain | **We had exactly this and DELETED it** (2026-09-03, CLAUDE.md "NR IS A PRESENT STAGE"). `NgxNRPaperWhiteScale` / `NgxNRColorStrength` / `NgxNRTransferStrength` are gone | N/A — our-side shader | **Not a candidate.** They keep it because their hook can land pre-tonemap; ours cannot. See §5 |
| **HDR carrier rule** (DFC) | Linear-HDR codec chosen only when `DLSS.Feature.Create.Flags` says HDR **and** the carrier is a validated FP format; UNORM/PQ carriers stay on the non-linear-safe path. **SOFT** | **Implicitly yes** — our stage hard-codes "the back buffer is `R10G10B10A2_UNORM` with no `SetColorSpace1`, therefore no codec" | N/A | No gap in effect. Worth recording as a **rule**: gate on flag **and** carrier, never either alone |
| **Texture Boost** (DFC) | Accepts *only* an exact 3840×2160 neural output, produces an 8K intermediate, +506.25 MiB. **SOFT** | No | Unknown | **Not a candidate.** 506.25 MiB is exactly two 7680×4320 RGBA16F images, so the mechanism is almost certainly a supersampled evaluate (**UNCONFIRMED**). No source, one supported resolution, large VRAM |
| **DLSS bias-current-colour mask** (feeder, SR side) | The feeder validates every optical-flow vector and passes the failures to **DLSS SR** as `pInBiasCurrentColorMask` (`src/dlss5-feed.cpp:4152`, `:4563`, `:4973`, `:5235`) | **No — we send no bias mask on the SR path** (grep for `BiasCurrentColor` in `src/` is empty) | It is a stock NGX SR field | **The one feeder-side idea worth a thought.** See §7 |
| **`arm=0` vs `enabled=0`** (DFC), **`enabled=0`** (feeder 0.13) | Separating "install no hooks at all, restart-only" from "run no work this frame" | Partially — our `NgxNR=0` queues a teardown but the plugin stays hooked | N/A | Not a feature, a **diagnostic discipline**. See §8 |
| **Multi-pass / per-pass reset button**, presets, schema migration, smart host discovery, FG native-safe bypass, `LoadFromDllMain` self-registration | Consumer-side plumbing for a general-purpose add-on shipped to strangers | No, and correctly not | N/A | Cosmetic for us — we ship to one machine |

---

## 2. "Post processing only" — the real answer

`Post-process: restore native tone & color, keep neural detail`, default **off**
(`external/deepfried/v1.4.8-alpha/README.md:333-353`, `QUICK-START.md:139`,
`RELEASE-NOTES.md:105-109`). **SOFT throughout** — this is the author's prose about a closed binary.

**Mechanism, as described.** The codec already retains (a) the exact native frame and (b) a
matching neural-domain proxy of it. After the last pass, the mode:

1. compares the **final neural luminance** with that exact proxy;
2. clamps the relative change to **±0.50 stops**;
3. applies the clamped ratio as a **scalar gain** to the untouched native frame.

Native RGB ratios, the tone-mapped base, HDR range and alpha stay owned by the game.
Equal-luminance chroma change is **deliberately discarded**. It is explicitly *not* an inverse
tone map and it does not recover anything applied after the intercepted output. The 1.4.8 notes
record that earlier builds high-passed the gain over a radius-one neighbourhood, which "erased
most multi-pixel face and surface changes and made the option look like a neural bypass" — i.e.
the version before this one was close to a no-op and shipped that way.

**Is this a hook-placement difference?** **No — and that is the important part of the answer.**

| | Where feature 18 sees the image |
|---|---|
| **DFC in a native-DLSS game** | Hooks NGX **feature 1**, so it sees DLSS SR's output — in most engines still **pre-tonemap linear HDR scene colour**. Hence the codec |
| **DFC behind the feeder** (the configuration the user is looking at) | The feeder issues its synthetic DLAA evaluate from inside **ReShade's effect chain over the back buffer** (`README.md:660-690`) — **post-tonemap, display-encoded, HUD in the image** |
| **Us** | Present-time stage over the **back buffer** — post-tonemap, display-encoded, HUD in the image |

**Feeder + DFC is the same placement we already chose.** They arrived at it from the other
direction: we moved the hook and deleted the codec; they kept a codec that can serve both
placements and select between them from the create flags plus the carrier format. Native Look is
**orthogonal to placement** — it is a choice about *how much of the network's answer to keep*,
which is a question we have never asked.

**Would it work for us?** Mechanically yes, and it is the cheapest idea in this document:

* it needs no pre-tonemap site, no codec, no exposure term, no feedback path;
* both inputs already exist in `src/nr_stage.cpp` — the staging copy is the "native frame" and the
  neural output is the other half;
* it is one compute dispatch on the present list.

**The cost and the risk, honestly:**

* **We have never recorded a compute dispatch on the present-owner command list.** CLAUDE.md
  flags exactly this as still open ("no compute dispatch of ours is recorded on that list even
  now"). The typed-UAV probe for `R10G10B10A2_UNORM` already exists
  (`nrp::probe_typed_uav`, called from `nrstage::probe`) so the format question is answerable
  without new code, but the *dispatch* question is not yet answered.
* It would make NR strictly **weaker** by construction — it throws away every chroma change and
  bounds luminance to ±0.5 stops. That is the point (it is a stability/΄native look' trade), but
  it is not "more than what we do"; it is a governor on what we do.
* A ±0.5-stop clamp over a `R10G10B10A2_UNORM` back buffer will band far more visibly than over
  the FP16 image DFC usually operates on. **UNCONFIRMED** and worth expecting.

**What a test would show.** Ship it behind one key, default off, and take the CLAUDE.md §5
screenshot protocol: n ≥ 5 per mode, camera still, the temporal-stability metric (frame-vs-itself
HF energy) rather than absolute sharpness. If the SSR/wet-ground artefact the project has chased
for sessions is a *chroma* artefact, discarding chroma change would remove it while keeping the
detail — that is a real, falsifiable prediction and the only reason this option is at the top of
the table.

---

## 3. Multi-pass (1–30)

**SOFT.** `Passes` is "the total number of direct DLSSNR evaluations, from one through thirty"
(`external/deepfried/v1.4.8-alpha/README.md:266-290`). Passes 1..N−1 are private pre-passes, the
last is the final owner; colour chains through FP16 intermediates in one output-resolution
domain; guides (depth, MV) are "never cascaded, fabricated, or rewritten"; the shared codec wraps
the entire chain once rather than per pass. Each pass carries its own full parameter panel and its
own temporal history — "Resetting a private pass also resets every downstream neural history".

Cost, from their own table (`README.md:419-430`): one 3840×2160 RGBA16F intermediate ≈ 63.28 MiB,
29 of them ≈ 1.79 GiB, before NVIDIA's model and history allocations; the baseline codec itself
retains two native-format plus two FP16 images (≈190–253 MiB at 4K). Their own notes call passes
11–30 "an extreme VRAM/TDR stress mode".

**Do we do it?** No. We evaluate feature 18 once per frame.

**Is it "more than what we do"?** Yes, mechanically — and it is the only option in the set that
genuinely adds capability rather than governing it. But two things temper it:

1. **A cascade of temporal filters sharing one motion field compounds.** Each pass reprojects its
   *own* accumulation with the *same* vectors. This project has already written the lesson down:
   *"bad motion vectors do not produce one bad frame, they compound through the accumulation"*.
   Two passes double the exposure to that.
2. **Clean Fry exists because the cascade produces artefacts.** A default-off spatial guard whose
   stated job is bounding "unsupported halos and accumulated colour drift" from multi-pass output
   is machinery built to compensate for a design choice — the exact shape CLAUDE.md's own
   present-stage lesson tells us to be suspicious of.

**If it were tried here:** one extra feature handle, one extra 4K FP16 intermediate (~63 MiB),
one extra evaluate per frame. Our single-pass NR already sits around 57–62 fps in CLAUDE.md's
measurements; a second pass is not free and must be judged on the bench protocol
(`tools/stray-bench.sh`, identical instrument per arm), not by eye. **Not recommended before
Native Look**, which is cheaper and answers a question we actually have.

---

## 4. Neural work scale (10–150%)

**SOFT.** `QUICK-START.md:188-195`: work dimensions are `ceil(display × percent / 100)`, always
reconstructed to the unchanged display extent; exactly 100% keeps the original path; above 100%
is "experimental neural supersampling"; an over-limit request fails closed to the native frame.
Applying it performs a fenced graph rebuild.

**This corroborates one of our HARD binary facts rather than contradicting it.** DFC states
plainly that "all of its private and final passes are 1:1 in the resolved output domain"
(`README.md:306-309`) — so the scaling is *their own resample around* feature 18, never a request
to feature 18. That is precisely what our disassembly forces: `DLSSNR.ScalingRatio` is read and
then unconditionally overwritten with `1.0f` at `0x18001a96a`. Two independent implementations
landing on caller-side resampling is good evidence the parameter is dead for everyone.

**For us:** a pure cost knob, and one we do not currently need — but note that changing the colour
extent while the guides stay at render resolution **moves the colour/guide ratio**, which is
exactly the condition `nrplan::latch_guide_extent` was built for (one forced `DLSSNR.Reset` when
it moves). Anyone adding this must wire it through that latch or it will silently corrupt the
accumulation, the same way a screen-percentage change did.

---

## 5. Their colour codec is our deleted codec

DFC's shared section — Paper White `1.765`, HDR Transfer `1.0`, Colour Strength `1.0`
(`README.md:299-302`, `:319-331`) — is recognisably **the same RenoDX-derived soft-clip + exact
sRGB proxy + residual transfer** this project ported, shipped, tuned for weeks, and then deleted
on 2026-09-03 when NR moved to the present stage. Their own licensing note says as much: *"The
color-preserving tone-transfer codec contains separately identified RenoDX-derived source
portions"* (`README.md:82-85`).

**They keep it because they must serve a pre-tonemap placement.** We removed it because ours is
post-tonemap by construction. **This is not a gap and must not be read as one** — re-adding a
codec to a display-referred back buffer would be re-introducing the exact double-conversion the
dxvk-remix `2df9c812` lesson in CLAUDE.md warns about.

The one durable thing to take from their version is the **selection rule**, which is stricter than
our hard-coded assumption: choose the linear-HDR path only when the NGX HDR flag is set **and**
the actual output carrier is a validated floating-point format; a UNORM or PQ-compatible carrier
stays on the non-linear-safe path (`README.md:291-298`, `FEEDER-COMPATIBILITY.md:191-194`). Ours
is right for Stray for exactly that reason; it is right by hard-coding rather than by rule.

---

## 6. Where their golden defaults are quietly weaker than ours — from OUR binary read

DFC's supplied config: Pass 1 at `Default` preset and style, **all four NR strengths at 2.0**,
**Automatic Mask off**, **UI Correction off**, game NGX depth convention, Motion Scale X/Y 1.0
(`README.md:312-317`, `QUICK-START.md:129-133`).

Against our own HARD disassembly of `nvngx_dlssnr.dll` 310.8.0 (CLAUDE.md §5, "DLSSNR's structure
controls, read out of the binary"):

* **`UseAutoMask = 0` drives both resolved strength values to `-1.0f`.** The evaluate path passes
  only the *resolved* skin and local values into the network's input tile. So with Automatic Mask
  off — DFC's shipped default — **two of the four sliders they set to 2.0 do not reach the
  network as strengths at all**; only the raw `LocalStructureStrength` still feeds the flag
  channel (via `param_0+176`, the `max(skin, local) < 0` branch). Their "known-good NR strengths
  of 2.0" is, for skin and local structure, largely a setting with no consumer in that
  configuration.
* **We ship `UseAutoMask = 1`** (`src/ngx_nr.cpp:188`), which is the configuration where the
  resolved pair is live. On that axis **we do more than their default, not less.**
* **Changing Style, UseAutoMask, LocalTone, LocalStructure or SkinStructure wipes the temporal
  history** — `CG2R_ResetTemporalHistoryOnControlChange` at `0x180017ad8`, ε `1e-5`. `Intensity`
  is not in that list. DFC exposes all five as live per-pass sliders across up to thirty passes
  and **their documentation never mentions this**; they document only an explicit reset button.
  A user dragging a strength slider holds `Reset = 1` every frame and sees the whole screen
  change — the exact confound this project nearly mis-diagnosed as "skin affects everything".
* **`UICorrection` is inert without a bound `DLSSNR.Backbuffer`** on both sides. DFC ships it off;
  we ship it on and it does nothing. Neither is wrong; ours is untidy.

**This is the strongest single finding in the document, and it points the other way from the
user's hunch:** on the NGX-parameter axis we are not behind. We send the same names, we know which
ones are dead and why, and our defaults sit on the live side of a gate their defaults sit on the
dead side of.

---

## 7. The feeder's SR-side idea: a bias-current-colour mask

**HARD.** `DLSS5_Feed.fx` reprojects every optical-flow vector into the previous frame and runs
four tests — disocclusion by depth, vector consistency, a luma test, and a *static-hypothesis*
test asking whether "did not move" explains the pixel better on illumination-normalised structure
— with two-frame hysteresis on the static test. Failing vectors are zeroed **and** the pixel is
written into a `DLSS5_Mask` (R8) texture handed to DLSS as `pInBiasCurrentColorMask`
(`README.md:640-660`; `src/dlss5-feed.cpp:1031`, `:4152`, `:4563`, `:4973`, `:5235`).

**We send no bias mask on the SR path** — grep for `BiasCurrentColor` across `src/` is empty.

**Why it is worth a thought here even though their motivation does not apply to us.** Their
vectors are *estimated optical flow*, which lies about flames and flickering lights; ours are
engine-derived and reconstructed from depth + `ClipToPrevClip`, so the flow-confidence problem is
not ours. **But we have a documented class of pixels whose vectors are structurally wrong and
cannot be fixed:** CLAUDE.md's conclusion that screen-space reflections move with the reflected
geometry, not the reflecting surface, so *"treat 'temporal network + screen-space reflections' as
a structural mismatch, not an open bug"*, with the only listed moves being "reduce strength,
disable SSR, or accept it".

`pInBiasCurrentColorMask` is **DLSS's own mechanism for exactly that sentence** — "do not trust
history for this pixel". It is a fourth move that the CLAUDE.md list does not contain, and it
arrives at the SR path where our vectors are already computed.

**What we would need, and it is not free:** a per-pixel signal for "this is a reflection". UE 4.27
does not hand us one at the TAA hook. Candidates are all speculative (a roughness/G-buffer term
via `src/gbuffer_finder.cpp`, or a disagreement test between the sparse encoded velocity and the
depth-reconstructed camera motion — the same shape as their consistency test, on inputs we
already have in the MV-resolve pass). **UNCONFIRMED that it would help, and it is a new shader on
the SR path, which is the one part of the frame currently known-good.** Record it; do not build it
on this evidence.

---

## 8. Two operational habits worth stealing, no code required

* **`arm=0` vs `enabled=0`.** DFC separates "install no hooks at all, restart-only" from "do no
  work this frame", and the feeder's own README calls out why it matters: *"only `arm=0` truly
  takes it out of the picture"* when ruling a component out of a problem
  (`DLSS5-Feeder/README.md:423-425`). The feeder then found the same bug in itself: 0.13.0-beta.1's
  headline change is that `enabled=0` *"now actually disables everything"*, because previously it
  was consulted in four places inside the frame path while hooks, module scans, key polling and
  MinHook trampolines all kept running — *"that made 'set enabled=0 and see whether it still
  crashes' a test that proved nothing, which is exactly what it had been used for"*
  (release `v0.13.0-beta.1`). Our `NgxNR=0` queues a teardown but leaves the plugin hooked; that
  is fine as long as nobody reads a `NgxNR=0` run as exonerating the plugin.
* **Their bisection ladder is ours.** `stall_log_ms` splits a slow frame into time inside the NGX
  evaluate, time in the rest of the add-on, and everything else, then names which dominated
  (`README.md:878`). That is the same instrument as our `src/perf.cpp` bucket report, arrived at
  independently, and it is the right answer to "the feed is slow" vs "the consumer is slow".

---

## 9. Clean negatives — things that look like features and are not

* **NR Preset with four values over a single-network runtime.** Both DFC and we expose it; our
  research says the shipped DLL contains one network. A selector is not evidence of a mechanism.
* **UI Correction**, on either side, without a bound `DLSSNR.Backbuffer`. HARD, ours.
* **`DLSSNR.ScalingRatio`**, on either side. HARD, ours — and DFC's caller-side resample is
  independent confirmation.
* **The feeder's `work_upscale=2`** (DLSS SR on synthetic jitter as the expand-back filter) is
  documented by its own author as *"cfg-only experiment, not recommended … measured to cost as
  much as 100% and to shimmer"* (`README.md:863`). A negative result, published, which is more
  than most projects do.
* **`renodx-dlss` supersedes the feeder for 64-bit D3D9/11/12** by the feeder author's own
  admission (`README.md:31-58`, commit `d38ca53`). The feeder's remaining reason to exist is
  32-bit, Vulkan and D3D9 — none of which is us.
* **Nothing in either project touches the engine.** No reflection, no cvars, no engine state
  enabled or disabled. Everything they do is between the game's output and NGX. The user's
  question was *"check what it disables/enables"* — the honest answer is that they disable and
  enable **each other** (consumer mutual exclusion, `arm`, `LoadFromDllMain` ordering, FG
  bypass, Smooth Motion) and **nothing inside the renderer**. As a UE4SS plugin with UObject
  reflection we have a lever neither of them has.

---

## 10. Verdict

**On NGX parameters: we are not behind.** Every `DLSSNR.*` name DFC exposes, we already send;
several we know to be dead and they apparently do not; and their shipped default sits on the dead
side of the `UseAutoMask` gate that ours sits on the live side of.

**On mechanisms, three things exist there and not here**, in the order they deserve attention:

1. **Native Look / "post processing only"** — a bounded, luminance-only, chroma-discarding
   composite of the neural result over the untouched native frame. Placement-compatible, one
   compute dispatch, and it makes a falsifiable prediction about the SSR artefact. **Worth a
   test.** The unresolved risk is that we have never recorded a compute dispatch on the present
   list.
2. **Multi-pass** — real added capability, but it compounds motion-vector error through N
   accumulations and needs its own cleanup pass to be usable. **Not before Native Look.**
3. **A bias-current-colour mask on the SR path** — a fourth option for the reflection problem
   CLAUDE.md currently calls structural. **Record, do not build**, until there is a real
   per-pixel reflection signal.

Everything else is either something we already do, something we deliberately deleted, or a knob
over a parameter one of us has already proven inert.
