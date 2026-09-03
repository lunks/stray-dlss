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
| `DLSSNR.Backbuffer` / `DLSSNR.BidirectionalDistortionField` — what they are for | **UNCONFIRMED.** New in this census, not investigated; out of scope for the Style question |

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
