# DLSSNR.Style — is there a "cinematic" mode, and what is it?

Research report, 2026-09-02. Subject: `nvngx_dlssnr.dll` (DLSSNR 310.8.0, the exact copy Stray
loads on the target box — `md5 eea91faf55a8993656c66815f0497b3b`, 165,840,496 bytes, staged at
`.../Hk_project/Binaries/Win64/nvngx_dlssnr.dll`), plus two independent open-source callers of
the same runtime. Provenance labels follow CLAUDE.md §0.5: **HARD** = read verbatim from a
primary source (the binary's own bytes, a source tree, our own measurement); **SOFT** = a
credible secondary source or a third party's own characterisation, not independently re-derived;
**UNCONFIRMED** = open.

**No game was launched, killed, or written to for this report.** The DLL was read (not modified)
from the SteamOS box over SSH; all analysis ran locally against a copy.

## 0. Answer, up front

`DLSSNR.Style` is real, is a distinct parameter from `DLSSNR.Hint.Render.Preset`, and is exactly
the axis the user's hunch was pointing at. **"Cinematic" is not a string that exists anywhere in
the binary — HARD, exhaustive search, zero hits.** It is a **community-assigned label** for
`DLSSNR.Style = 2`, three values wide (0/1/2), authored by the `OptiScaler_DLSSNR` fork's own
menu and stated by that fork's own comments to be exactly that: a guess from testing, not a name
NVIDIA shipped. Two independent third-party callers (RenoDX, OptiScaler_DLSSNR) write this
parameter; we did not, before this change.

## 1. The binary itself: exhaustive substring search, both encodings

Method: `strings -n4` is not enough — it is ASCII-only and (per the brief that started this
investigation) an earlier pass anchored matches with `^...$`, which misses any substring, any
case variant, and every UTF-16LE string a PE resource or wide literal might carry. This pass instead
extracted every printable run of length >= 4 from the raw bytes twice — once as ASCII
(`[\x20-\x7e]{4,}`), once as UTF-16LE (`(?:[\x20-\x7e]\x00){4,}` decoded) — with no anchoring, so
a hit anywhere inside a longer identifier still counts, and grepped both dumps case-insensitively
for `cinema|film|movie|photo|portrait|natural|vivid|neutral|style|mode|preset|look|sharp|soft|
balanced|quality|performance`. [HARD]

**Result: zero occurrences of `cinema`, `film`, `movie`, `photo`, `portrait`, `natural`, `vivid`,
or `neutral`, anywhere, in either encoding, in a 165 MB file.** The `style`/`mode`/`preset`
substring hits are all accounted for: `DLSSNR.Style` (appears **once**, as the bare parameter
name), `DLSSNR.Hint.Render.Preset`, `PerfQualityValue`, `output-composite mode`, and a handful of
unrelated CRT/registry noise (`SOFTWARE\...`, `compatibility mode`, `TCC mode`). There is no
second string anywhere that could be a label for `DLSSNR.Style`'s values — no `style_0`,
`Cinematic`, `Natural`, `styleName`, or anything of that shape. [HARD]

The full extraction and search commands, for reproduction:

```
python3 extract_strings.py nvngx_dlssnr.dll   # writes .ascii.txt and .utf16le.txt
grep -inE "cinema|film|movie|photo|portrait|natural|vivid|neutral|style|preset|..." \
    nvngx_dlssnr.dll.ascii.txt nvngx_dlssnr.dll.utf16le.txt
```

## 2. The full `DLSSNR.*` parameter census — 60 names, HARD

Exact-string census over the same file (`grep -oE "DLSSNR\.[A-Za-z0-9_.]+" | sort -u`). This
supersedes `docs/RESEARCH-RENODX-DLSS5.md` §2.2's RenoDX-derived list and §2.2.1's audit — both
were built by cross-referencing what RenoDX writes against the runtime, so they only ever saw the
subset RenoDX itself uses. This is every `DLSSNR.*` identifier the runtime's own string table
contains, independent of any caller:

```
DLSSNR.Backbuffer(SubrectBaseX/Y/Width/Height)
DLSSNR.BidirectionalDistortionField(SubrectBaseX/Y/Width/Height)
DLSSNR.Color(SubrectBaseX/Y/Width/Height)
DLSSNR.ControlMask(SubrectBaseX/Y/Width/Height)
DLSSNR.Depth(SubrectBaseX/Y/Width/Height)
DLSSNR.DepthInverted
DLSSNR.Enabled
DLSSNR.Height
DLSSNR.Hint.Render.Preset
DLSSNR.Intensity
DLSSNR.LocalStructureStrength
DLSSNR.LocalToneStrength
DLSSNR.MVec(SubrectBaseX/Y/Width/Height)
DLSSNR.MVecScaleX / DLSSNR.MVecScaleY
DLSSNR.Output(SubrectBaseX/Y/Width/Height)
DLSSNR.Reset
DLSSNR.ScalingRatio
DLSSNR.SkinStructureStrength
DLSSNR.Style
DLSSNR.UI(SubrectBaseX/Y/Width/Height)
DLSSNR.UIAlpha(SubrectBaseX/Y/Width/Height)
DLSSNR.UICorrection
DLSSNR.UseAutoMask
DLSSNR.Width
```

New in this census, not in either prior audit or in our own code: **`DLSSNR.Backbuffer`,
`DLSSNR.BidirectionalDistortionField`, `DLSSNR.ControlMask`, `DLSSNR.UI`, `DLSSNR.UIAlpha`** (all
with their four Subrect fields). `ControlMask` and `UI`/`UIAlpha` match what CLAUDE.md's own
disassembly notes already describe (a per-pixel RGB control texture, and UI-aware composition);
`Backbuffer` and `BidirectionalDistortionField` are new to every document in this repo and are
UNCONFIRMED beyond "the runtime declares string names for them" — not investigated further here,
out of scope for the Style question. Also present but outside the `DLSSNR.` namespace:
`DLSS.Indicator.Invert.X.Axis` / `DLSS.Indicator.Invert.Y.Axis` (shared with the SR indicator,
not investigated here).

**No name anywhere in this census reads as a style label.** Confirms §1 by a different method:
if the binary carried a second string table mapping Style's values to words, it would show up
here as `DLSSNR.Style.*` or similar, and it does not — `DLSSNR.Style` has no children.

## 3. What `DLSSNR.Style` is consumed as, and what it is not

**It is read at feature-build time and is in the control-change reset list** — this repeats and
narrows what CLAUDE.md's own prior disassembly notes already record (the "DLSSNR's structure
controls" section): `CG2R_ResetTemporalHistoryOnControlChange` compares the previous frame's
Style against this frame's and forces `DLSSNR.Reset = 1` on any difference. [HARD, per that
existing entry — not re-derived by this pass; see §6 for what this pass did and did not
re-verify].

**It is NOT `DLSSNR.Hint.Render.Preset`, and the two are legibly different axes in the binary's
own strings.** Preset selects among **embedded trained weight sets** — the runtime logs
`DLSSNR: Created feature %u (output %ux%u, network %ux%u, preset=%d -> %s)` where `%s` resolves
through `CG2RFindWeightByPreset` / `CG2RNetworkManager::ResolvePresetToDescriptor` against
config codenames actually present in the strings (`CC_Control_History_Blend_Quantize_With_
Teacher_honest_tench_2026_07_04_22_30_weights` / `WEIGHTS_HT`, `CC_SILVER_AARDWOLD`) — and this
build ships exactly one weight set, so every preset value other than the one it's registered
under falls back to it (`DLSSNR: preset %d is not available in this DLL build; falling back to
shipping default preset %d ('%s')`, confirmed separately live per `src/backend_reshade/
addon_entry.cpp`'s own comment: "310.8 ships one weight set registered as preset 1 and falls back
to it for every other value"). **Style has no equivalent fallback string, no per-value config
name, and no weight-set indirection anywhere in the census** — it is a plain integer consumed
directly, not a selector into a table of named things. [HARD for the string evidence; the
"plain integer, no table" reading is the natural inference from that evidence, not a
disassembly-confirmed absence of a lookup]

## 4. Third-party corroboration: two other callers write this parameter

### 4.1 RenoDX (docs/RESEARCH-RENODX-DLSS5.md §2.2.1)

Already on record: `DLSSNR.Style` is one of the names RenoDX writes that was confirmed present
in the 310.8.0 runtime by exact string search (that audit's own method, independently repeated
by this one with a different search technique and the same result). RenoDX does not name Style's
values anywhere in its own strings; it is written as a bare `SetUInt`. [HARD, restated from the
existing audit]

### 4.2 OptiScaler_DLSSNR (`github.com/Dagherbou/OptiScaler_DLSSNR`, branch `dlss-neural-rendering`) — new source

A third, independent, and — unlike the other two — **fully readable** implementation of a
DLSSNR caller, fetched today from GitHub (`OptiScaler/dlssnr/`). It writes `DLSSNR.Style` as a
plain `SetUInt` at both create and evaluate time (`DlssNr_Proxy.cpp:112,285`):

```cpp
SetUInt(params, "DLSSNR.Style", (unsigned int) cfg.DlssNrStyle.value_or_default());
```

**Its menu is the direct answer to this task**, `DlssNr_Menu.cpp:253-268`:

```cpp
static const char* nrStyleNames[] = { "Default (standard)", "Natural", "Cinematic" };
int style = (int) config->DlssNrStyle.value_or_default();
if (style > 2) style = 2;
if (ImGui::Combo("Style", &style, nrStyleNames, IM_ARRAYSIZE(nrStyleNames)))
    config->DlssNrStyle = (uint32_t) style;

HelpMarker("The model's own processing profiles."
           "\n\nDefault (standard): the strongest. Boosts local contrast and deepens"
           "\nlighting, and can oversaturate or look stylised -- most of what reads as"
           "\n'the model changed my game's look' is this profile."
           "\n\nNatural: the same detail work with a gentler hand. Keeps skin tones and"
           "\ntonal balance closer to what the game rendered."
           "\n\nCinematic: tones down the shine and over-processing for a film-like look."
           "\n\nRead when the model is built, so a change rebuilds it after a moment. The"
           "\nnames come from community testing; NVIDIA ships no names in the binaries.");
```

That last sentence is the fork's own authors saying, in their own shipped help text, exactly
what §1's string search independently found: **the names are not NVIDIA's.** [SOFT — this is
the fork's own characterisation of its own testing, not independently reproduced by us against
the live game; but it is a primary source for "where did 'cinematic' come from", and it agrees
with the negative result in §1 rather than merely repeating it — two different methods (exhaustive
binary string search here; the fork's own visual A/B there) converging on the same conclusion is
stronger than either alone]

Their config default (`OptiScaler/Config.h:266`) is `DlssNrStyle { 0 }` — "Default (standard)" —
matching this project's choice to default the new knob to 0.

### 4.3 Not investigated further here, flagged per the coordinator's request

Three commits on the OptiScaler_DLSSNR branch bear on open questions elsewhere in this project
but are outside this report's scope (Style/"cinematic" only); recorded for whoever picks them up
next, not acted on:

* **`f9c407a6` "Take the game's exposure by default"`** — flips `DlssNrWhitePointFromExposure`
  from off to on-by-default. Their mechanism (read the game's own exposure texture, when present,
  to set the codec's white point; byte-identical no-op when a game supplies none) is a different
  design from our `NgxNRTrackExposure` (which multiplies by `1/PreExposure` from the View CB, not
  from a runtime-queried exposure texture) but addresses the same open question — whether to
  track engine exposure at all, and how fast. Relevant to the unresolved flicker/ringing
  discussion in CLAUDE.md's "Exposure tracking" section. [SOFT — their commit message and diff,
  not verified by us]
  **DATED 2026-09-03: `NgxNRTrackExposure` no longer exists.** It was part of the HDR colour
  codec, which went with the TAA hook site when NR became a present stage — the back buffer is
  display-referred and carries no pre-exposure to track. Their commit therefore has no analogue
  in our tree any more; the open question it addressed is closed by placement rather than by
  tuning.
* **`a5e4c5c2` "Stop detail strength above 1 from extrapolating"`** — fixes an `lerp` that
  extrapolates past 1.0 in **their own post-composition blend shader** (`transfer`, their
  "Detail strength" slider, 0-2 range in their menu). **This is a different knob from
  `DLSSNR.LocalToneStrength`/`SkinStructureStrength`** — those are separate runtime parameters
  their menu ranges 0-2 (LocalTone, LocalStructure) and -1-2 (SkinStructure) without a documented
  clamp-to-1 in that source. We ship `LocalToneStrength=1.74` and `SkinStructureStrength=1.33`,
  both above 1 — this commit does **not** show the runtime itself clamping those to [0,1]; it
  shows a bug in OptiScaler's own separate compositing math when *their* blend factor exceeds 1.
  Whether our two above-1 runtime values are themselves a problem is UNCONFIRMED and not settled
  by this commit — it is evidence about a different shader, not about `DLSSNR.LocalToneStrength`'s
  own valid range.
* **`c88d83c1` "Normalise the hue direction before scaling it"`** — colour-handling fix in their
  own composition shader; not inspected in detail here.

## 5. What this means for our own defaults

We ship `LocalToneStrength=1.74`, `SkinStructureStrength=1.33` and `Preset=1` (RenoDX's own
shipped values, per `docs/RESEARCH-RENODX-DLSS5.md` §2.2.1) but have never written
`DLSSNR.Style` at all. Per §3, an unwritten parameter behaves however the runtime's own
zero-initialised/absent-key handling treats it — we have not measured what that default state
renders as, only that it is whatever Stray has looked like in every session to date. Writing
`DLSSNR.Style = 0` explicitly (this change) is the same value OptiScaler defaults to and is
presumed [SOFT] to be behaviourally identical to leaving it unwritten, matching the "written
unconditionally, zero included" pattern CLAUDE.md's own DLSSNR notes already establish for
`Hint.Render.Preset` (skipping a write for "default" can leave a stale value from earlier in the
session still in effect — the same trap, avoided the same way).

## 6. What this pass did and did not do — the honest ledger

| Claim | Status |
|---|---|
| `DLSSNR.Style` exists as a bare parameter-name string in the exact runtime Stray loads | **HARD** — direct read, this pass |
| No `cinema`/`film`/`movie`/`photo`/`portrait`/`natural`/`vivid`/`neutral` substring exists anywhere in the binary, ASCII or UTF-16LE | **HARD** — exhaustive search, this pass, zero hits |
| The 60-name `DLSSNR.*` census in §2 is complete for this build | **HARD** for what a string search over compiled-in identifiers can find; would NOT find a name assembled from parts at runtime (none of our other findings suggest this runtime does that) |
| `DLSSNR.Style` selects among conditioning constants read at feature-build time and is in the control-change reset list | **Carried over from CLAUDE.md's existing disassembly notes, NOT re-derived by this pass.** We did not re-run a disassembler against `CG2R_ResetTemporalHistoryOnControlChange` this session — attempted a light xref search with `r2`/`rabin2` against the 165 MB file, which did not complete in the time budgeted, and the string evidence in §3 was judged sufficient without it |
| "Default (standard)/Natural/Cinematic" are the values 0/1/2 name, and what each visually does | **SOFT.** Sourced entirely from the OptiScaler_DLSSNR fork's own menu text, itself sourced from "community testing" per that fork's own admission — not independently verified against the live game by us. Nothing in the binary confirms which numeric value produces which look, only that the fork settled on this ordering |
| The compressed cubin/PTX payload (~59 MB, per CLAUDE.md's earlier NR investigation) was searched again this session for style-related names | **NOT DONE.** The tooling that decompressed it before (`patch_dlssnr.py`, referenced from `~/Downloads/dlssnr-remix/` in CLAUDE.md) was not present on the box this session (`find` came back empty at that path). Given §1's exhaustive top-level search and §4's two independent corroborating implementations already converge on "no names exist", this was judged not worth the extra round trip — but it is the one item from the original brief not exhausted, and is where to look first if a future session wants the very last mile |
| `DLSSNR.Backbuffer` / `DLSSNR.BidirectionalDistortionField` — what they are for | **UNCONFIRMED here; answered in §8** (2026-09-03). Backbuffer is consumed and validated; BidirectionalDistortionField is read and then never used again |

## 7. The code change

`[STRAYDLSS] NgxNRStyle` (default 0) now writes `DLSSNR.Style` alongside the other `DLSSNR.*`
parameters — `src/ngx_nr.cpp`/`.hpp` (the setter and the two write sites, create and evaluate),
`src/app/dlss_app.{hpp,cpp}` (`NrLive::style`, ini read, `apply_nr_live_impl`), and
`src/backend_reshade/addon_entry.cpp` (the overlay combo and the "Save to ReShade.ini" write).
0/1/2 only, clamped where it is consumed (`apply_nr_live_impl`, not at ini-read time, so an
out-of-range ini value stays visible rather than being silently rewritten). Default 0 is
byte-identical to today: the parameter was never written before this change existed, and 0 is
both the OptiScaler fork's own default and the "Default (standard)" label. Per §3/§6, changing it
forces the snippet's own temporal-history reset for one frame — the overlay comment and the
header doc both say to judge the result a second or two after moving the slider, never on the
frame it changes, the same rule already in force for every other control on this runtime's reset
list.

---

## 8. The five unbound parameters, settled — 2026-09-03

§6 left `DLSSNR.Backbuffer` and `DLSSNR.BidirectionalDistortionField` UNCONFIRMED and out of
scope, and §2's census flagged `ControlMask`, `UI` and `UIAlpha` alongside them as names we
declare but never write. This pass answers, for all five, the only question that decides whether
they are worth building: **does the runtime READ them, and does what it reads reach the kernel?**

Method: the same file (`md5 eea91faf…`), disassembled locally with capstone. A linear sweep over
`.text` (0x1000..0xabdfc, 187,527 instructions, 248 bytes unresolvable) collected every
RIP-relative reference, which located the single call site of every `DLSSNR.*` string. `.pdata`
gave exact function bounds — **merged across contiguous entries, which matters**: a first pass
that trusted one `RUNTIME_FUNCTION` per function truncated `0x18001c920` at 165 bytes instead of
2257 and produced the wrong answer for `ControlMask`. No game was launched and nothing was
written to the box.

### 8.1 All 61 names are read; they land in one 0x140-byte struct

> **REPRODUCED AND SLIGHTLY CORRECTED 2026-09-04 against a DIFFERENT copy of the same 310.8.0
> runtime (`md5 5b944399…`, the pristine Blackwell2 build) — `docs/RESEARCH-DLSSNR-PARAM-AUDIT.md`.**
> Every offset in the table below reproduces byte for byte, which is strong cross-copy evidence
> that the `.rdata` name table and `ReadEvalParams` are the same in both. Two amendments:
> the struct is **0x15c** bytes, not 0x140 — `ReadEvalParams` initialises fields up to `+0x158`;
> and the table omits **`DLSS.Indicator.Invert.X.Axis` / `.Y.Axis`**, read from this same evaluate
> block at **`+0x110` / `+0x114`** as `int`. That audit also carries the full slot→TYPE decode for
> every name (`+0x40` `void**`, `+0x58` `int*`, `+0x60` `unsigned*`, `+0x70` `float*`), and finds
> `ScalingRatio` clobbered to `1.0f` at `CreateFeature` as well as at evaluate.

`ReadEvalParams` at **0x180019f30** (`rcx` = out struct, `rdx` = the NGX parameter block) is the
only caller of any of them, and is itself called once, from `EvaluateFeature`
(0x180018620..0x180019e67) at 0x1800186e4. Layout, HARD:

| Offset | Parameter | Subrects |
|---|---|---|
| +0x00 | `Color` | +0x08/0x0c/0x10/0x14 |
| +0x18 | `MVec` | +0x20…+0x2c |
| +0x30 | `Depth` | +0x38…+0x44 |
| +0x48 | `Output` | +0x50…+0x5c |
| +0x60 | **`ControlMask`** | +0x68…+0x74 |
| +0x78 | **`UI`** | +0x80…+0x8c |
| +0x90 | **`UIAlpha`** | +0x98…+0xa4 |
| +0xa8 | **`Backbuffer`** | +0xb0…+0xbc |
| +0xc0 | **`BidirectionalDistortionField`** | +0xc8…+0xd4 |
| +0xd8/+0xdc | `MVecScaleX/Y` (default 1.0f) | |
| +0xe0 | `Intensity` (1.0f) | +0xe4 `LocalToneStrength` (1.0f), +0xe8 `LocalStructureStrength` (1.0f) |
| +0xec | `Style` (0) | +0xf0 `UseAutoMask` (0), +0xf4 `SkinStructureStrength` (**-1.0f**) |
| +0xf8/+0xfc | resolved skin / resolved local | |
| +0x100 | `Reset` (0) | +0x104 `DepthInverted` (**1**), +0x108 `Enabled` (**1**), +0x10c `UICorrection` |
| +0x120 | `ScalingRatio` | |

**Every resource is fetched through the same parameter-block vtable slot (+0x40) and every
subrect through the same slot (+0x58)** as `Color`/`Depth`/`MVec`/`Output` and theirs. That
settles the TYPE question for all five new parameters without decoding the vtable: write them the
way the four working ones are written. A failed `Get` stores a null pointer, so **writing a null
is the supported way to UNBIND** — the parameter block persists across evaluates and nothing
clears it.

`ScalingRatio` is re-confirmed INERT: read into +0x120 at 0x18001a964, then **unconditionally**
overwritten with `1.0f` at 0x18001a96a.

### 8.2 `ControlMask`, `UI`, `UIAlpha` — genuinely consumed

At **0x18001c920** (2257 bytes, `r9`/`rdi` = the struct) each non-null resource is registered
through the cubin device's vtable slot +0xa8 — the identical call `Color` and `MVec` get, with
the same read-only usage word — and its 24-byte descriptor is handed to the guide-rect builder at
**0x18001c520**, which emits a float record `{baseX, baseY, activeW, activeH, 1.0f/texW,
1.0f/texH}` into the launch array. Positions in that array: `Color` rbp+0x150, **`ControlMask`
rbp+0x198, `UI` rbp+0x1b0, `UIAlpha` rbp+0x1c8**, plus internal textures at +0x168/+0x180/+0x1e0/
+0x1f8. A zero subrect Width/Height is filled from the resource's own extent (0x18001c596), and a
null pointer zeroes the whole record. **HARD.**

The `1.0f/texW` terms are strongly suggestive of normalised-coordinate sampling, which would make
the mask's own resolution free — but that is inference from the record's shape, not a reading of
the kernel, and is **UNCONFIRMED**.

The runtime never inspects a caller-supplied texture's `DXGI_FORMAT` on this path: the guide-rect
builder reads only Width and Height (vtable +0x190), and the registration call passes the resource
and a usage word. So the format is interpreted entirely by the CUDA texture object the driver
mints from the D3D12 descriptor, and **the kernel's assumed channel type is not discoverable from
the host code**. That is why `src/nr_mask.hpp` ships the format as a knob with a SOFT default
rather than a constant.

### 8.3 `Backbuffer` — consumed, and dangerous

Read, validated and passed on (9 accesses in `EvaluateFeature`, subrects included). It has its own
failure string, and it is not an error return: **`DLSSNR: Skip feature evaluate: Invalid
Backbuffer/active Output rect configuration Backbuffer=%p subrect=(%u,%u %ux%u) Output=%p
activeRect=(%u,%u %ux%u)`** (0x1800afab0, reached from 0x180019dac). A mismatched Backbuffer rect
therefore turns the whole evaluate into a silent no-op. At 0x180018f2b it SUBSTITUTES for `Color`
where present, which is consistent with "the pre-composite image" — but that reading is
**UNCONFIRMED**.

### 8.4 `BidirectionalDistortionField` — DECLARED, PARSED, INERT

After the reader stores it at +0xc0, the pointer is touched **exactly once** more in the entire
module: `cmp qword ptr [rbp+0xe0], 0` at **0x180019cd8**, which ORs bit 2 into a presence bitmask
at rbp+0x4fc in the telemetry record the runtime hands to its stats callback (bit 0 ControlMask,
1 Backbuffer, 2 BDF, 3 UI, 4 UIAlpha). Its pointer is never registered, its four subrect fields
are never read anywhere in `.text`, and no guide-rect record is ever built for it. A module-wide
sweep for `lea reg, [greg + 0xc0]` finds only the reader's own out-pointer. **HARD: supplying
forward motion vectors to this parameter would cost a texture and change nothing in this build.**

### 8.5 `UICorrection` requires a `Backbuffer`, so ours has been a no-op

Two consumers, and neither can arm without one:

* `EvaluateFeature` 0x180019016: armed only when `UICorrection && Backbuffer && !(UI || UIAlpha)`.
* the registration path 0x18001cbec: armed only when `UICorrection && (UI || UIAlpha) && Backbuffer`.

We have written `DLSSNR.UICorrection = 1` for months and bind no Backbuffer, so it has done
nothing. **HARD.**

### 8.6 The structure sentinel, both branches

`0x18001aa4b`: a non-null `ControlMask` sets `UseAutoMask = 0` unconditionally, whatever the
caller asked for. `0x18001aa59`: with `UseAutoMask == 0` **both** resolved strengths (+0xf8, +0xfc)
take the constant at 0x1800afc40, which is exactly **-1.0f**, and neither raw value survives.
`0x18001aa62`: only on the auto path does the sign test run — `comiss`/`jae`, so any negative and
any NaN inherits `LocalStructureStrength`, while local itself passes through.

So binding a mask does **not** slave skin to local structure; it retires the resolved pair and
leaves the texture as the only spatial control. Both branches are transcribed in
`src/core/nr_mask_plan.cpp` and pinned in `tests/test_nr_mask_plan.cpp`. **HARD.**

### 8.7 Toggling the mask resets the temporal history; changing its values does not

`CG2R_ResetTemporalHistoryOnControlChange` (0x1800179d0) compares exactly seven fields of the
input struct against the previous frame's snapshot, with epsilon 1e-5 (0x1800afc20), and forces
`DLSSNR.Reset = 1` on any difference: **+0xe4 LocalToneStrength, +0xe8 LocalStructureStrength,
+0xec Style, +0xf0 UseAutoMask, +0xf4 raw SkinStructureStrength, +0xf8 resolved skin, +0xfc
resolved local**. `Intensity` (+0xe0) is not among them — which confirms CLAUDE.md's existing note
that Intensity is the one strength knob that does not cost a reset — and neither is the
ControlMask pointer itself.

**But binding a mask moves three of the seven** (UseAutoMask 1 -> 0, and both resolved strengths
to -1.0), so `NgxNRMask` 0 -> 1 and 1 -> 0 each wipe feature 18's accumulation for a frame.
Changing what the mask CONTAINS moves none of them. So a mask-value A/B is clean and immediate,
while an on/off A/B has to be judged a second or two after the toggle. **HARD.**

### 8.8 What the KERNEL does with the mask — G and B are dead, and the format question is closed

§8.2 established that the host registers the mask and gives it a guide rect. This section is the
other end: what the compiled kernel actually fetches. All **HARD** unless marked.

**Extraction.** The cubins and PTX are **not** in `.rsrc` — that 147 MB section is a single
resource named `WEIGHTS_HTS` holding the weight blob. The code lives in **`.data`** as **15 NVIDIA
fatbins** (magic `50 ED 55 BA`), 45 entries in all — 15 × (sm_89 cubin, sm_120 PTX, sm_120 cubin),
~57 MB decompressed. This copy carries **real sm_89 cubins with the same kernel symbol set as
sm_120**, so the PTX is a faithful proxy for what runs on Ada. (This also corrects §6's note that
the payload lives where `patch_dlssnr.py` looked.)

**Only two kernels read the mask**, both with a single bindless
`tex.2d.v4.f32.f32` — float coordinates, float4 result:
`cc_tinlayout_fused_post_block_swin_1h_32_control_mask` (and its `_fp8` / `_full_rect` variants,
texture object at `param+256` of a 312-byte struct) and `cg2r_post_process_kernel`
(`param_0+96`). There is no `tld4`, no `suld` and no `sured` anywhere in the corpus.

**The arithmetic, and it is short:**

```
w       = saturate(DLSSNR.Intensity * mask.x)      // cvt.ftz.sat.f32.f32
out.rgb = saturate(lerp(originalColour.rgb, network.rgb, w))
out.a   = 1.0f
```

`Intensity` is proven to be the other factor by the sibling `..._simple_blend` kernel, which is
the identical block with `saturate(Intensity)` used directly and no texture at all, and by the
host's own gate at 0x18001c9f2: the blend path is taken when
`1.0f > Intensity || ControlMask != null`.

**`mask.R` is the per-pixel final blend weight — CONFIRMED. `G` and `B` are DEAD — REFUTED.**
The `.y`/`.z`/`.w` registers the `tex` instruction produces appear **exactly once each in 9.9 MB
of PTX**, as its own destinations, and are never read. CLAUDE.md's "G scales local tone, B scales
local structure" does not hold for 310.8.0. "No skin channel" is confirmed: the pre-block has five
texture slots, none of them a mask, and no `_control_mask` variant exists.

**Format: no validation of any kind.** `NGXCubinD3D12::GetInputTextureViewHandle64` (0x18005d640,
vtable slot +0xA8) calls `GetDesc`, runs the format through a pure typeless/sRGB/depth
canonicalizer (0x18005da00 — `R16G16B16A16_TYPELESS→_FLOAT`, `R8G8B8A8_UNORM_SRGB→_UNORM`,
`D32_FLOAT→R32_FLOAT`, everything else passed through unchanged), builds
`{Format, TEXTURE2D, Shader4ComponentMapping = 0x1688, MipLevels = desc.MipLevels}` and calls
`CreateShaderResourceView`, then `NvAPI_D3D12_GetCudaMergedTextureSamplerObject`. **No whitelist,
no rejection.** So the texture unit resolves the format: **UNORM comes back normalised to [0,1],
FLOAT comes back raw, and both are correct.** The `NGXCubinFormat_*` enum is used only for the
runtime's own allocations.

**The one broken class is `*_UINT` / `*_SINT`:** the canonicalizer passes it through, the SRV is
created, and `tex.2d.v4.f32.f32` then reads an integer texture as float — undefined values, no
error. `nrmaskplan::format_is_integer` refuses it.

**Coordinates are normalised and the guide rect's `1/texW, 1/texH` terms ARE used:**
`u = (activeW * (px + 0.5)/blendW + baseX) * (1/texW)`. A zero subrect means the whole texture and
the reciprocals are zero-guarded, so a mask of **any resolution works**. But the sampler is
**`MIN_MAG_MIP_POINT`, CLAMP, `MinLOD == MaxLOD == 0`** (template 0x1800b7770, selected as kind 2
at 0x18001cbae for every caller guide), so a mismatched mask is nearest-neighbour resampled —
blocky, with no error. Matching the blended rect buys 1:1 texels; at a present-stage hook that
rect is the back buffer.

**Range: `saturate` on the product clamps everything, NaN to 0**, so out-of-range values are safe
but only [0,1] is meaningful.

**Hazards, in order.** (1) A non-`TEXTURE2D` or MSAA resource: the SRV is built with
`ViewDimension = TEXTURE2D` unconditionally and `desc.Dimension` is never checked, so a buffer or
a multisampled texture yields an invalid SRV that reaches the cubin — the highest hang risk.
(2) An integer format, as above. (3) A wrong resource state; the mask wants
`NON_PIXEL_SHADER_RESOURCE` like every other guide. (4) Mip levels are **benign here** — the SRV
exposes them all but the sampler pins LOD 0; this is the input path and is unrelated to the
documented `DXGI_ERROR_DEVICE_HUNG` from a mipped *output*, which goes through the surface path
(vtable +0xB0). (5) A wrong size is benign but silently wrong.

**`DLSSNR.UI` / `DLSSNR.UIAlpha`, from `cg2r_post_process_kernel`:** only `UIAlpha.x` (RED) is
used, and only `UI.w` (ALPHA) as its fallback — the UI texture's RGB is fetched and never read, so
this kernel reads UI *opacity* and does not composite UI colour. The combined weight is
`Intensity × ControlMask.R × (1 − saturate(uiAlpha))`, i.e. **suppress the neural effect where the
HUD is opaque**. Format, coordinates and sampler are identical to the mask's in every respect.

**One semantic footgun:** binding a ControlMask forces the blend path on even at `Intensity == 1.0`
(host gate 0x18001ca04), and the blended result is written to a **different surface** (`param+192`)
with RGB saturated to [0,1] and alpha forced to 1.0, while the raw unsaturated network output goes
to `param+16`. At a post-tonemap hook that saturation costs nothing — the back buffer is
`R10G10B10A2_UNORM` already — but it would have mattered at the old TAA site.

The `param+224` "blend-toward" texture is the un-processed image: **SOFT** on the wiring (the
launch site was not walked), **HARD** on the semantics, since `simple_blend` lerps toward it by
`Intensity` alone and `EvaluateFeature` creates and fills a `dlssnr_original_color` RGBA16F
snapshot at 0x180019664 for exactly this shape.
