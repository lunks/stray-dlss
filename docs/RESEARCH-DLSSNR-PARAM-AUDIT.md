# Re-auditing the DLSSNR parameter set — is the string search a complete method?

Research report, 2026-09-04. Subject: a **local** copy of `nvngx_dlssnr.dll`, DLSSNR **310.8.0.0**,
165,840,496 bytes, **`md5 5b9443990497077444f059696266a764`**. This is a **different copy** from
the one every prior document in this repo was read against (`md5 eea91faf…`), and establishing
whether that matters was part of the brief.

Provenance labels follow CLAUDE.md §0.5: **HARD** = read verbatim from a primary source (the
binary's own bytes, a header, our own measurement); **SOFT** = a credible secondary source or an
inference from documented behaviour; **UNCONFIRMED** = open.

**The test box was not touched.** No SSH, no SCP, no game launch. Everything below was produced
locally from the file named above. No extracted payload is committed — offsets and short quoted
strings only.

---

## 0. Answer, up front

**None of the six names exists. Lookup is by string literal, and the enumeration is now closed
rather than merely exhaustive.**

1. **`DLSSNR.Scale`, `DLSSNR.InputWidth`, `DLSSNR.InputHeight`, `DLSSNR.OutputWidth`,
   `DLSSNR.OutputHeight`, `DLSSNR.Output.Width`, `DLSSNR.Output.Height`, `DLSSNR.Upscaling` are
   ABSENT** — zero occurrences in ASCII, UTF-16LE and UTF-16BE, in the raw file, in all 15
   decompressed fatbins, in the decompressed PTX, and in `.rsrc`. **HARD.** The prior verdict
   stands, and it now stands on a method that could have overturned it.

2. **The fatbin hypothesis is REFUTED, directly rather than by argument.** All 15 NVIDIA fatbins
   in `.data` were located and decompressed (30 entries, 49.1 MB) and searched: **zero
   occurrences of the substring `DLSSNR`, in any encoding, case-insensitive.** `.rsrc` is a plain
   uncompressed weight blob (entropy 5.90 bits/byte, plaintext tensor names, zero genuine
   compressed frames) and likewise carries no parameter name. **HARD.**

3. **Parameter lookup is BY STRING, at the only boundary that matters.** Every name is loaded as a
   `lea rXX, [rip + disp]` pointing at a NUL-terminated literal in `.rdata` and passed as the
   second argument to a virtual call on the `NVSDK_NGX_Parameter` vtable, whose return value is
   then tested with `and eax, 0xfff00000 / cmp eax, 0xbad00000` — the `NVSDK_NGX_FAILED` mask.
   **HARD.** No hashing anywhere on this path.

4. **And the enumeration is CLOSED, which is stronger than "exhaustive".** Three independent
   facts, each **HARD**:
   * The binary imports **only** `version.dll`, `advapi32.dll`, `user32.dll` and `kernel32.dll`
     (129 imports, none NGX). It contains **no** `NVSDK_NGX_Parameter_*` helper symbol and never
     names `nvngx.dll` except as the caller-identity substring. There is no second route by which
     a parameter could be read.
   * A code-driven scan (not a string scan) of every vtable call site recovered **61 distinct
     `DLSSNR.*` names**, and each of the 61 NUL-terminated `DLSSNR.*` strings present in the file
     is referenced from **exactly one** call site (`DLSSNR.ScalingRatio` from three). Strings and
     call sites agree perfectly in both directions: no unreferenced name, no name reached from a
     table or an index.
   * Runtime construction is impossible: there is **no bare `DLSSNR.` prefix string**, **no bare
     suffix string** (`Scale`, `InputWidth`, `Upscaling`, …), and **no `DLSSNR.%s`-shaped format
     string** anywhere in the file. A concatenated name has nothing to be built from.

5. **The copy you have locally is NOT the copy the box deploys, and it says so itself.** Its
   version resource carries `NGXGpuArchitecture = NVSDK_NGX_GPU_Arch_Blackwell2`, every one of its
   15 PTX entries is `.target sm_120`, and the architecture gate at `0x180017ef6` writes a
   **minimum required arch of `0x1b0`** — so it refuses Ada (`0x190`) at `CreateFeature` and
   carries no sm_89 SASS to run if it did not. **This is the pristine Blackwell2 build.** **HARD.**
   Whether the deployed (grafted) copy's `.rdata` name table is identical is **UNCONFIRMED** — see
   §6, which gives the one command that settles it.

6. **Two findings that change what we do**, both new:
   * **`DLSSNRGetStatsCallback` and `DLSSNRComputeScalingRatioCallback` are OUTPUTS, not inputs.**
     The snippet **sets them itself**, from its own `NVSDK_NGX_D3D12_PopulateParameters_Impl`
     (`0x180015f20`), with function pointers into its own `.text`. CLAUDE.md's note that
     `nvngx.dll` would populate them and that "we leave them unset — guessing an RVA into a leaked
     DLL is not a bounded risk" is **wrong on both halves**: nothing in the runtime ever *reads*
     them, so leaving them unset costs nothing, and no RVA needs guessing because the populating
     function is an export. **HARD.**
   * **`DLSSNR.ScalingRatio` is inert at `CreateFeature` too**, not only at evaluate. It is read
     into `[rsp+0x60]` at `0x180018000` and **unconditionally overwritten with `1.0f` six bytes
     later at `0x180018006`**, exactly as it is at `0x18001a96a` on the evaluate path. The network
     extent is therefore always the requested extent. **HARD**, and it closes the "can feature 18
     upscale" question at both call sites rather than one.

7. **The complete read-type table is now HARD** (§4), and it retires a SOFT claim in
   `docs/RESEARCH-RENODX-DLSS5.md` §2.2.1 in both directions: subrects really are read as **signed
   `int`** (so the 2026-09-02 change was correct), but **`DLSSNR.Width`/`Height` are read as
   `unsigned int` while we now write them as `int`** — and since NR demonstrably creates features
   and produces a correct image with that mismatch in place, the NGX core's parameter block
   **coerces between the integer overloads**. That removes a class of worry §2.2.1 raised, rather
   than adding one.

---

## 1. Was the old method actually broken? — the honest answer is "no, but it could not have known"

`tools/ngx_param_names.py` searches the raw file for NUL-terminated ASCII runs and their UTF-16LE
twins. Against the brief's four hypotheses:

| Hypothesis | Would the old tool have missed it? | Verdict here |
|---|---|---|
| Name only inside a compressed fatbin | **YES — a real gap.** `.data` holds 15 zstd-compressed fatbins; a plain scan sees the compressed bytes. | **Checked and empty.** Zero `DLSSNR` in 49.1 MB decompressed. |
| UTF-16**BE**, or unaligned UTF-16 | **YES for BE** (the tool has no BE pattern). Alignment is a non-issue: byte-pattern search matches at any offset. | Checked. Zero BE hits outside `.rsrc`'s version resource. |
| Length-prefixed / unterminated string | The tool's *listing* requires `\0NAME\0`; its `--check` also reports a bare-substring "fragment". | This audit used an **unanchored** `DLSSNR[\x20-\x7e]*` scan — 136 raw matches, all accounted for. No unterminated name. |
| Name built at runtime | **YES — invisible to any whole-string search.** | **Impossible here**: no prefix string, no suffix strings, no `DLSSNR.%s` format. |
| Lookup by hash rather than string | **YES — would invalidate the whole method.** | **Not the case.** 61/61 names are literal pointers at virtual-call sites. |

**So the tool's answer was right and its method had two genuine holes** (compressed containers,
UTF-16BE) plus one it could never have closed on its own (runtime construction). The fix is not to
distrust it but to **stop searching for strings and start enumerating call sites**, which is what
§3 does and which is decidable rather than merely thorough.

---

## 2. Containers: where the names could have hidden, and what is in each

All **HARD**, from the PE itself (`imagebase 0x180000000`, PE32+).

| Section | vaddr | raw size | What it holds | `DLSSNR` hits |
|---|---|---|---|---|
| `.text` | `0x00001000` | 699,904 | code, 187,522 instructions | 0 (code, not strings) |
| `.rdata` | `0x000ac000` | 210,944 | **every parameter name**, log formats, vtables | **141 ASCII** |
| `.data` | `0x000e0000` | 17,180,672 | **15 NVIDIA fatbins** (magic `50 ED 55 BA`) | **0** |
| `.pdata` | `0x0115b000` | 34,816 | unwind | 0 |
| `_RDATA` | `0x01164000` | 512 | — | 0 |
| `.rsrc` | `0x01165000` | 147,697,152 | one resource, `WEIGHTS_HTS` | **0** (3 UTF-16 hits are the version resource's `NVIDIA DLSSNR` product strings) |
| `.reloc` | `0x09e40000` | 5,120 | — | 0 |
| overlay | — | 10,352 | Authenticode signature | 0 |

**The fatbins, and how to reproduce the extraction cheaply.** The previous session's route
(`docs/RESEARCH-DLSSNR-STYLES.md` §8.8) is correct but harder than it needs to be. Each fatbin is
`magic(4) ver(2) hdrSize(2) fatSize(8)` followed by entries of a 120-byte header
(`kind(2) ver(2) hdrSize(4) paddedPayloadSize(8) … flags(8)@0x28 … uncompressedSize(8)@0x38`).
**Entries with `flags & 0x8000` carry a raw zstd frame**, which Python 3.14's stdlib
`compression.zstd` decompresses directly — no external tool, no `ooz`, no fatbin library:

```python
dec = zstd.ZstdDecompressor()          # NOT zstd.decompress(): the payload is zero-padded
raw = dec.decompress(payload)          #  and the one-shot API rejects the trailing bytes
```

This copy: **15 fatbins × 2 entries = 30** (one PTX + one cubin each), **49.1 MB** decompressed.
`docs/RESEARCH-DLSSNR-STYLES.md` §8.8 reports **45 entries** and ~57 MB for the `eea91faf` copy —
`15 × (sm_89 cubin, sm_120 PTX, sm_120 cubin)`. **That difference is the graft, and it is expected**
(§6): this copy is pristine and carries no sm_89 code at all.

**`.rsrc` is not a container that could hide anything.** It is a single `WEIGHTS_HTS` resource of
raw tensor data: entropy 5.90 bits/byte over a 1 MB sample (an encrypted or compressed blob reads
~8.0), plaintext tensor names such as `block0.layer0.layer` in the clear, zero zstd/zlib/lz4/fatbin
frames, and **zero of the 41 gzip-magic byte sequences inflates** — they are coincidence in binary
weights. **HARD.**

---

## 3. The method that closes the question: enumerate call sites, not strings

Instead of asking "which names appear in the file", ask **"which names does the code ask for"**.
A linear capstone sweep of `.text` (187,522 instructions) recorded every RIP-relative reference,
then matched the pattern the runtime uses everywhere:

```
mov  rax, qword ptr [rbx]              ; rbx = NVSDK_NGX_Parameter*
lea  r8,  [rdi + <field>]              ; out slot in the eval struct
lea  rdx, [rip + <disp>]               ; -> the NUL-terminated name literal
mov  rcx, rbx
mov  rax, qword ptr [rax + <slot>]     ; the overload's vtable slot
call qword ptr [rip + ...]             ; __guard_dispatch_icall_fptr (CFG)
and  eax, 0xfff00000
cmp  eax, 0xbad00000                   ; NVSDK_NGX_FAILED
```

Verbatim from `ReadEvalParams`, `0x18001a07c`–`0x18001a09a`. **HARD.**

**Result: 133 distinct names asked for across the whole binary**, of which 61 are `DLSSNR.*`, 2 are
`DLSS.Indicator.*`, 5 are other NGX names, and the rest are CUDA kernel names looked up through an
unrelated interface. **Every one of the 61 `DLSSNR.*` strings in `.rdata` is reached by exactly one
of these sites** (`ScalingRatio` by three: one `Set`, two `Get`). There is no `DLSSNR.*` string in
the file that the code does not use, and no site that reads a name from anywhere but a literal.

Reproduce with: locate the fatbins, decompress, grep; then sweep `.text` with capstone and match
the pattern above. Everything needed is in this document; nothing needs the box.

---

## 4. The complete table — names, containers, vtable slot, read type

The vtable slot decodes exactly, because **MSVC reverses the declaration order of an overload
group** (the same behaviour CLAUDE.md §2.9 records for `RHISetUAVParameter`).
`NVSDK_NGX_Parameter` (`third_party/ngx/include/nvsdk_ngx_params.h`) declares
`Set(ULL, F, D, UI, I, ID3D11*, ID3D12*, void*)` then `Get(…)` in the same order, so:

| slot | function | slot | function |
|---|---|---|---|
| `+0x00` | `Set(void*)` | `+0x40` | `Get(void**)` |
| `+0x08` | `Set(ID3D12Resource*)` | `+0x48` | `Get(ID3D12Resource**)` |
| `+0x10` | `Set(ID3D11Resource*)` | `+0x50` | `Get(ID3D11Resource**)` |
| `+0x18` | `Set(int)` | `+0x58` | **`Get(int*)`** |
| `+0x20` | `Set(unsigned int)` | `+0x60` | **`Get(unsigned int*)`** |
| `+0x28` | `Set(double)` | `+0x68` | `Get(double*)` |
| `+0x30` | `Set(float)` | `+0x70` | **`Get(float*)`** |
| `+0x38` | `Set(unsigned long long)` | `+0x78` | `Get(unsigned long long*)` |

**The reversal is confirmed three independent ways, none of them circular** — `DLSSNR.Intensity`
is read at `+0x70` into a field defaulted to `0x3f800000` (a float); `SizeInBytes` is written at
`+0x38` by `GetScratchBufferSize` (a `size_t`); `CreationNodeMask` / `VisibilityNodeMask` are read
at `+0x60` (D3D12 `UINT` node masks). A non-reversed reading puts a float at `+0x48` and would
break all three. **HARD.**

### 4.1 The 61 `DLSSNR.*` names

Every one is ASCII, NUL-terminated, in **`.rdata`**, and referenced from **`.text`**. Struct offsets
are into the 0x15c-byte evaluate struct that `ReadEvalParams` (`0x180019f30`) fills.

| Name (+ its four `SubrectBaseX/BaseY/Width/Height`, all `+0x58` `int`) | resource slot | struct | we write? |
|---|---|---|---|
| `DLSSNR.Color` | `+0x40` `void**` | `+0x00` | **yes** |
| `DLSSNR.MVec` | `+0x40` | `+0x18` | **yes** |
| `DLSSNR.Depth` | `+0x40` | `+0x30` | **yes** |
| `DLSSNR.Output` | `+0x40` | `+0x48` | **yes** |
| `DLSSNR.ControlMask` | `+0x40` | `+0x60` | **yes** |
| `DLSSNR.UI` | `+0x40` | `+0x78` | **no** |
| `DLSSNR.UIAlpha` | `+0x40` | `+0x90` | **no** |
| `DLSSNR.Backbuffer` | `+0x40` | `+0xa8` | **no** |
| `DLSSNR.BidirectionalDistortionField` | `+0x40` | `+0xc0` | **no** |

| Scalar name | call site | slot | read as | struct (default) | we write? | our type |
|---|---|---|---|---|---|---|
| `DLSSNR.MVecScaleX` | `0x18001a8c8` | `+0x70` | `float` | `+0xd8` (1.0f) | yes | `float` ✓ |
| `DLSSNR.MVecScaleY` | `0x18001a8fc` | `+0x70` | `float` | `+0xdc` (1.0f) | yes | `float` ✓ |
| `DLSSNR.Intensity` | `0x18001a930` | `+0x70` | `float` | `+0xe0` (1.0f) | yes | `float` ✓ |
| `DLSSNR.LocalToneStrength` | `0x18001a993` | `+0x70` | `float` | `+0xe4` (1.0f) | yes | `float` ✓ |
| `DLSSNR.LocalStructureStrength` | `0x18001a9c7` | `+0x70` | `float` | `+0xe8` (1.0f) | yes | `float` ✓ |
| `DLSSNR.SkinStructureStrength` | `0x18001aa2f` | `+0x70` | `float` | `+0xf4` (**-1.0f**) | yes | `float` ✓ |
| `DLSSNR.UseAutoMask` | `0x18001a9fb` | `+0x58` | **`int`** | `+0xf0` (0) | yes | `unsigned` ✗ |
| `DLSSNR.Style` | `0x18001aac0` | `+0x60` | `unsigned` | `+0xec` (0) | yes | `unsigned` ✓ |
| `DLSSNR.Reset` | `0x18001aaf6` | `+0x58` | **`int`** | `+0x100` (0) | yes | `int` ✓ |
| `DLSSNR.DepthInverted` | `0x18001ab27` | `+0x58` | **`int`** | `+0x104` (**1**) | yes | **both** ✓ |
| `DLSSNR.Enabled` | `0x18001ab5b` | `+0x58` | **`int`** | `+0x108` (**1**) | yes | **both** ✓ |
| `DLSSNR.UICorrection` | `0x18001ab8f` | `+0x58` | **`int`** | `+0x10c` (0) | yes | `unsigned` ✗ |
| `DLSSNR.ScalingRatio` | `0x18001a964` **and** `0x180018000` | `+0x70` | `float` | `+0x120` — **clobbered `1.0f` at both sites** | yes | `float` ✓ |
| `DLSSNR.Width` | `0x180017fa0` | `+0x60` | **`unsigned`** | create-local (default **0**) | yes | `int` ✗ |
| `DLSSNR.Height` | `0x180017fc7` | `+0x60` | **`unsigned`** | create-local (default **0**) | yes | `int` ✗ |
| `DLSSNR.Hint.Render.Preset` | `0x18001803d` | `+0x58` | **`int`** | create-local, then `btr ecx, 31` | yes | `unsigned` ✗ |

The `btr ecx, 0x1f` after the preset read — the runtime clearing the sign bit of what it just read
— is independent confirmation that `+0x58` really is the **signed** overload. **HARD.**

### 4.2 Names outside the `DLSSNR.` namespace that this runtime reads

**New to every document in this repo**, all **HARD**:

| Name | call site | slot | read as | Notes |
|---|---|---|---|---|
| `DLSS.Indicator.Invert.X.Axis` | `0x18001abc0` | `+0x58` | `int` | read into the **evaluate** struct at `+0x110`; the on-screen indicator's orientation |
| `DLSS.Indicator.Invert.Y.Axis` | `0x18001abf1` | `+0x58` | `int` | `+0x114` |
| `CreationNodeMask` | `0x180017f4a` | `+0x60` | `unsigned` | **defaults to 1** if absent (`mov r13d,1; cmovne`) — safe to omit |
| `VisibilityNodeMask` | `0x180017f78` | `+0x60` | `unsigned` | **defaults to 1** — safe to omit |
| `PerfQualityValue` | `0x180012ed3` | `+0x60` | `unsigned` | read **only** by the `ComputeScalingRatio` callback, which nothing in our host calls |
| `BufferAllocCallback` / `Tex2DAllocCallback` / `ResourceAllocCallback` / `ResourceReleaseCallback` | `0x18005a3f9` … | `+0x40` | `void**` | optional host allocators the caller may supply |
| `SizeInBytes` | `0x180013055` etc. | `+0x38` | *written* `ULL` | `GetScratchBufferSize` output |
| `DLSSNRGetStatsCallback`, `DLSSNRComputeScalingRatioCallback` | `0x180015fff`, `0x18001601c` | `+0x00` | *written* `void*` | **outputs**, see §5 |

The four create-time reads are consecutive: `CreationNodeMask` (`0x180017f4a`),
`VisibilityNodeMask` (`0x180017f78`), `DLSSNR.Width` (`0x180017fa0`), `DLSSNR.Height`
(`0x180017fc7`), then `DLSSNR.ScalingRatio` (`0x180018000`) and `DLSSNR.Hint.Render.Preset`
(`0x18001803d`) — the whole sequence is `0x180017f35`–`0x180018043`.

### 4.3 Names we are NOT writing

**20 of the 61**, all four of the unbound guides plus their subrects: `DLSSNR.UI`,
`DLSSNR.UIAlpha`, `DLSSNR.Backbuffer`, `DLSSNR.BidirectionalDistortionField`. Their consumption is
already characterised in `docs/RESEARCH-DLSSNR-STYLES.md` §8.2–§8.5 and nothing here changes it:
`UI`/`UIAlpha` are read (only `UIAlpha.x`, with `UI.w` as fallback), `Backbuffer` is required
before `UICorrection` can arm, and `BidirectionalDistortionField` is parsed and inert.

Outside the namespace, the only genuinely new candidates are `DLSS.Indicator.Invert.X/Y.Axis`
(cosmetic) and the four allocator callbacks (we have no reason to supply host allocators).
**Nothing in this audit uncovers a quality knob we did not know about.**

---

## 5. The two `Callback` names are OUTPUTS — CLAUDE.md is wrong about them

`NVSDK_NGX_D3D12_PopulateParameters_Impl` at **`0x180015f20`** (an export) does exactly three
things, all **HARD** from its 34 instructions:

1. `GetModuleHandleExW(6, <return address>, &hmod)` → `GetModuleFileNameW` → `wcsstr(path,
   L"nvngx.dll")` (the wide literal is at file `0xad480` / VA `0x1800ae280`). This is CLAUDE.md's
   Wall 2 caller-identity gate, now located precisely; it fails with
   `"Error: Not called from NGX runtime - %S"` and `0xbad00002`.
2. `params->Set("DLSSNRGetStatsCallback", <fn @ 0x180015060>)` via slot `+0x00` (`Set(void*)`).
3. `params->Set("DLSSNRComputeScalingRatioCallback", <fn @ 0x180012e80>)` via slot `+0x00`.

The same pair is set by the D3D11, Vulkan and CUDA variants (`0x18001414f`, `0x1800253cf`,
`0x180026f3f`). **Nowhere in the binary is either name read.**

**Consequences.** CLAUDE.md currently says these are names "which `nvngx.dll` would normally
populate with function pointers inside the snippet itself. We leave them unset — guessing an RVA
into a leaked DLL is not a bounded risk." Both halves are wrong:

* The **snippet** populates them, not `nvngx.dll`. Because we drive the snippet's exports directly
  (CLAUDE.md's Wall 1), `PopulateParameters_Impl` is simply never called on our block.
* **Leaving them unset costs nothing at all** — they are a courtesy API for the *application*, not
  an input the runtime consults. The sentence implies a residual risk that does not exist.
* And no RVA needs guessing: `PopulateParameters_Impl` is an **export**, and its only gate is a
  `GetModuleFileNameW` result our NR path **already** patches (`NgxNRIdentity=nvngx`). If either
  callback is ever wanted, calling the export under the existing IAT patch installs both.

`DLSSNRComputeScalingRatioCallback` reads `PerfQualityValue`, switches on 0..5 through a jump table
at `0x180012f6c`, and `Set`s `DLSSNR.ScalingRatio` to a constant float. **Since `ScalingRatio` is
clobbered to `1.0f` at both of its read sites, that callback cannot influence anything.**

---

## 6. Do the copies differ? — this one is pristine, and the check is one command

**What is HARD about the local copy:**

* Version resource: `FileVersion 310,8,0,0`, `InternalName DLSSNR`,
  `NGXMinimumDriverVersion 615.00`, and **`NGXGpuArchitecture = NVSDK_NGX_GPU_Arch_Blackwell2`**.
* All 15 PTX entries are `.version 9.4 / .target sm_120`. **No sm_89 anywhere.**
* The architecture gate at `0x180017eef`–`0x180017f21` builds its error with a **minimum required
  arch of `0x1b0`** (Blackwell2) and returns `0xbad00001`; Ada is `0x190`.

So this file **cannot create a feature on the user's RTX 4090** and is therefore **not** the copy
`dlss5-stage/` deploys. It matches CLAUDE.md's description of the pristine `SL 2.13/` copy exactly.

**Is the parameter set copy-invariant?** The names live in `.rdata` (`0x000ad5d8`–`0x000b0400`) and
are reached from `.text`; CLAUDE.md's own byte-diff measurement says the patched copies differ from
pristine by 13,555,158 bytes of **cubin payload** plus a rewritten gate stub, and from each other
by **exactly 3 bytes**, all gate-1 constants. On that measurement `.rdata` is untouched and the
parameter set is identical. **But that is SOFT here — I have one file.**

**The check, and it needs no game and no analysis.** Against any other copy:

```
python3 -c "import hashlib,sys;d=open(sys.argv[1],'rb').read();print(hashlib.sha256(d[0x000ad5d8:0x000b0400]).hexdigest())" <copy>
```

For this pristine copy the answer is
`10d2fb97efe977e664522c4c66346837f892a9b322aa338adaa7c8ea984ca6f8`.
A match proves the name table is byte-identical and every conclusion here transfers verbatim. Full
section digests, for a coarser comparison:

| section | sha256 |
|---|---|
| `.text` | `f62bc45ea5c87ad8970181d27aaea54d3cef39b386b62144bcb58756ee2ed0cb` |
| `.rdata` | `de228462dbffab2d4db5910add8ba414310869e34f447217ac620bf16ba703a6` |
| `.data` | `4235c83b9c03e9c2d5bb3883629fa3057f10194fd7954d3fa26c05f1fc19b371` |
| `.rsrc` | `457679bc3260712777986f397cfda549cc9a58d997afea5c1aafffd279902b3e` |

**Expect `.text` and `.data` to differ** (the gate stub and the grafted cubins) and `.rdata` to
match. If `.rdata` ever does **not** match, every parameter conclusion in this repo becomes
copy-specific and must be re-derived against the deployed file.

---

## 7. Types: one SOFT claim confirmed, one reversed, and the worry retired

`docs/RESEARCH-RENODX-DLSS5.md` §2.2.1 argued **SOFT**, from a third party's disassembly, that the
snippet reads subrects as `int`, and changed our writes from `unsigned` to `int` on 2026-09-02.

**That reading is now HARD and correct** — every `*Subrect*` is read through `+0x58`, `Get(int*)`.

**But the same table shows three writes that do not match, and one of them is decisive evidence
that the mismatch does not matter:**

* `DLSSNR.Width` / `DLSSNR.Height` are read as **`unsigned int`** (`+0x60`) and we write them as
  **`int`** (`nrparam::signed_entry`, `src/core/nr_params.cpp`). On a failed `Get` they fall back to
  `esi`, which the create path zeroes at `0x180017e5d` — so a failure would mean a `0x0` network
  extent, a `"DLSSNR: CreateFeature begin requested resolution 0x0"` line, and no feature.
  **NR demonstrably creates features and produces a correct image with this exact mismatch in
  place** (CLAUDE.md, 2026-09-03, present stage confirmed by the user). **Therefore the NGX core's
  parameter block coerces between the integer overloads.** SOFT, but from behaviour rather than
  from reading someone else's disassembly, and it is corroborated on the pointer side: the shipped
  DLSS SR contract has the caller `Set(ID3D12Resource*)` (slot `+0x08`) what the snippet `Get`s as
  `void**` (slot `+0x40`), and that works everywhere.
* `DLSSNR.UseAutoMask`, `DLSSNR.UICorrection` and `DLSSNR.Hint.Render.Preset` are read as **`int`**
  (`+0x58`) and we write them as **`unsigned`**. §2.2.1 lists these as "not audited, therefore
  unchanged, UNCONFIRMED". **They are now audited**, and by the argument above the mismatch is
  benign.

**So the recommendation is deliberately NOT a code change.** The evidence says the block coerces;
changing types would be a speculative edit that no test on this side can distinguish from a no-op,
and CLAUDE.md §7 forbids exactly that. If anyone wants belt-and-braces, `src/ngx_nr.cpp` already
carries the pattern — `DepthInverted` and `Enabled` are written through **both** overloads — and
extending it to the four names above is three lines with no behavioural risk. The number to watch
if it is ever suspected live is the runtime's own
`DLSSNR: enabled=%d localTone=%.3f localStructure=%.3f intensity=%.3f` line.

---

## 8. What this retires, corrects and leaves open

**Retired (settled, do not re-derive):**
* The six absent names. Confirmed by a method that searched every container and every encoding, and
  then by a method that does not depend on searching at all.
* "Could a parameter name be hiding in the cubins?" No. Zero, in 49.1 MB.
* "Is lookup by hash?" No. 61 literal pointers, one per name.

**Corrected in this commit:**
* CLAUDE.md's `DLSSNRComputeScalingRatioCallback` / `DLSSNRGetStatsCallback` paragraph (§5 above).
* CLAUDE.md's `DLSSNR.ScalingRatio` note — inert at **both** call sites, not one.
* `docs/RESEARCH-RENODX-DLSS5.md` §2.2.1 — the subrect type is HARD, the remaining "not audited"
  list is audited, and the type-mismatch hazard has no evidence of ever having bitten.
* `docs/RESEARCH-DLSSNR-STYLES.md` §8.1 — the eval struct is **0x15c** bytes, not 0x140, and its
  table omits `DLSS.Indicator.Invert.X/Y.Axis` at `+0x110`/`+0x114`. Everything else in that table
  reproduces exactly on this copy, which is a strong cross-copy agreement in itself.

**Open, and each with the run that would settle it:**
* **UNCONFIRMED: that the deployed (grafted) copy's `.rdata` is byte-identical.** One `sha256` of
  the byte range in §6, against the file already on the box. No launch, no analysis.
* **UNCONFIRMED: whether the NGX core's parameter block truly coerces, or whether Width/Height
  happen to work for another reason.** Settled by one line in a live log: the runtime's own
  `DLSSNR: CreateFeature begin requested resolution %ux%u (network %ux%u)`. Non-zero numbers there
  prove the `int`→`unsigned` read succeeded.
* **UNCONFIRMED: whether `DLSS.Indicator.Invert.X/Y.Axis` do anything visible** through our
  direct-snippet path. Cosmetic; not worth a round trip on its own.
