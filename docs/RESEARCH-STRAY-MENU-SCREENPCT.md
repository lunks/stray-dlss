# Stray's Screen Percentage row — reverse-engineered offsets

Read-only, offline investigation against a local, Steamless-unpacked copy of
`Stray-Win64-Shipping.exe` (85,043,200 bytes, PE32+, image base `0x140000000`). No game process
was involved; the box (`192.168.0.210`) was never touched. Same conventions as `CLAUDE.md` and
`docs/RESEARCH-STRAY-MENU-OPTIONS.md` (its sequel, this document): **HARD** = read directly out
of the binary and independently reproducible, **SOFT** = a plausible reading not directly
verified, **UNCONFIRMED** = a real open question with a proposed one-run experiment.

**Bottom line up front (Q3):** No data-driven lever was confirmed working this session. The two
byte ranges almost certainly holding the row's real, non-reflected state are now **precisely
bounded and HARD** — `ListBoxWidget` object offset `0x358..0x438` (224 bytes) and
`GraphicsSettingsWidget` object offset `0x4B0..0x530` (128 bytes) — but their internal field
layout inside those ranges is **UNCONFIRMED**, so a `RegisterCustomProperty` lever cannot yet be
safely wired without guessing a sub-offset and a C++ type. Writing the already-reflected
`m_screenPercentages`/`m_resolutions` (offsets given below) is proven **inert** for the display
(established in the task brief's own prior on-box experiment, reproduced structurally here). No
`.rdata` table and no responsible code-patch candidate were found. The cheapest way to finish
this is not more static analysis — it's a five-minute, read-only, on-box byte dump of the two
bounded ranges before and after a left/right click, which this task's hard rules forbid me from
running.

---

## 1. Method: how a property's byte offset is read straight out of the binary

UE 4.27's UHT codegen embeds every reflected property's byte offset as a literal `int32` inside a
small, compile-time-const descriptor struct (`UECodeGen_Private::F*PropertyParams`), one struct
per property, laid out **contiguously in `.data`, in declaration order** — this is the same
mechanism `IMPLEMENT_CLASS`/`GENERATED_BODY` always uses; it exists in Shipping builds because the
engine needs it to construct `UClass`/`FProperty` objects at boot, string-stripping only removes
`METADATA_PARAMS` (editor tooltips), never the offsets.

For a plain `UObject*` (BindWidget) property the struct is 48 (`0x30`) bytes:

```
+0x00  NameUTF8              const char*  (points at the property's ANSI name in .rdata)
+0x08  RepNotifyFuncUTF8      const char*  (null here)
+0x10  PropertyFlags          uint64
+0x18  Flags (EPropertyGenFlags) uint32
+0x1C  ObjectFlags            uint32
+0x20  ArrayDim               int32
+0x24  Offset                 int32        <-- the byte offset on the object, from `this`
+0x28  ClassFunc/tail         ptr (8 bytes; for object props, a function returning the referenced UClass)
```

`FTextPropertyParams` (FText fields) and struct-typed properties (`FLinearColor`) use the same
48-byte stride for the descriptor header; `FArrayPropertyParams`/`FSetPropertyParams`
(`TArray`/`TSet`) additionally emit a *second*, 40-byte "inner" descriptor immediately before the
outer one, describing the container's element type — its own `Offset` field is always 0 and must
be ignored; the offset that matters is the outer descriptor's.

**Reproduction recipe**, entirely mechanical:

1. Find the property's ANSI name string (`b"PropertyName\0"`) in `.rdata`, compute its VA.
2. Search the whole file for that VA appearing as a little-endian 8-byte pointer — it will be
   in `.data`, at the descriptor's `+0x00`. (UE4's per-property descriptors are almost always
   pointer-unique across the whole binary; if a name string is shared by several classes — e.g.
   plain `"Border"` or `"Text"` — anchor on an *adjacent, class-unique* name instead, such as
   `"SelectionBorder"`, and use position to disambiguate.)
3. Read the 4 bytes at descriptor-start `+0x24` as a signed little-endian `int32`: that is the
   property's offset from the object's `this` pointer.
4. Walking 0x30 bytes forward/backward from a known descriptor and re-running step 3 walks the
   whole class's declared-order property list for free — this is how the full tables below were
   built and cross-checked (offsets increase by exactly 8 between two adjacent pointer properties,
   which is an internal consistency check that caught zero errors here).

**Independent structural check, used throughout:** every class's property descriptors are also
referenced from a single `PropPointers[]` array (an array of 8-byte pointers to the descriptors,
in the same order) — found by searching for a QWORD equal to the *first* descriptor's own file
address, then verifying the following N QWORDs equal the addresses of the rest, in sequence. This
array is itself referenced **exactly once**, from the class's `Z_Construct_UClass_..._Statics::
ClassParams` struct, whose neighbouring fields (`NumProperties`) independently confirm the
property count. This closes the loop: name string → descriptor → offset, and descriptor → array →
class registration, agree.

All addresses below are given as **file offsets into `Stray-Win64-Shipping.exe.unpacked.exe`**
(imagebase `0x140000000`, so `VA = imagebase + RVA`, and for `.data`/`.rdata` at these addresses
`RVA = raw_file_offset - raw_section_start + section_RVA`, trivially recovered from the PE section
table). No game bytes beyond short structural snippets are reproduced verbatim below.

---

## 2. `GraphicsSettingsWidget` — full reflected-property offset table (HARD)

Native base of `UMG_GraphicsSettings_C` (`/Script/Hk_project.GraphicsSettingsWidget`,
`RESEARCH-STRAY-MENU-OPTIONS.md` §1-2). Descriptor table found at file offset `0x48d6380`
(`ScrollBox`) through `0x48d69a0`+`0x28` (`m_screenPercentages`, outer descriptor), all
contiguous 48-byte-strided entries in `.data`, cross-checked against the class's `PropPointers[]`
array at file offset `0x3bdb4a0` (34 of 36 top-level entries — see note below — matched exactly;
`NumProperties` field in the neighbouring `ClassParams` struct at `0x3bdb5f8+0x18` reads **36**,
which is 32 simple properties + 2×(inner+outer) for the two `TSet`s — exact arithmetic match).

| Property | Offset (dec) | Offset (hex) | Shape |
|---|---:|---:|---|
| `ScrollBox` | 752 | `0x2F0` | `UObject*` |
| `MotionBlurSliderBox` | 760 | `0x2F8` | `UObject*` |
| `SharpnessSliderBox` | 768 | `0x300` | `UObject*` |
| `FullscreenModeListBox` | 776 | `0x308` | `UObject*` |
| `ResolutionListBox` | 784 | `0x310` | `UObject*` |
| `FrameRateListBox` | 792 | `0x318` | `UObject*` |
| `GraphicsMemoryBox` | 800 | `0x320` | `UObject*` |
| `GraphicsMemoryText` | 808 | `0x328` | `UObject*` |
| **`ScreenPercentageListBox`** | **816** | **`0x330`** | **`UObject*` — the row widget itself** |
| `EffectsQualityListBox` | 824 | `0x338` | `UObject*` |
| `ShadowQualityListBox` | 832 | `0x340` | `UObject*` |
| `TextureQualityListBox` | 840 | `0x348` | `UObject*` |
| `MeshQualityListBox` | 848 | `0x350` | `UObject*` |
| `VSyncCheckBox` | 856 | `0x358` | `UObject*` |
| `GammaButton` | 864 | `0x360` | `UObject*` |
| `BackButton` | 872 | `0x368` | `UObject*` |
| `DefaultsButton` | 880 | `0x370` | `UObject*` |
| `m_resetToDefaultsDialogText` | 888 | `0x378` | `FText` (24 B) |
| `m_memoryGigabytesText` | 912 | `0x390` | `FText` |
| `m_memoryUnavailableText` | 936 | `0x3A8` | `FText` |
| `m_frameRateUncappedText` | 960 | `0x3C0` | `FText` |
| `m_qualityLowText` | 984 | `0x3D8` | `FText` |
| `m_qualityMediumText` | 1008 | `0x3F0` | `FText` |
| `m_qualityHighText` | 1032 | `0x408` | `FText` |
| `m_qualityVeryHighText` | 1056 | `0x420` | `FText` |
| `m_qualityFullText` | 1080 | `0x438` | `FText` |
| `m_windowedText` | 1104 | `0x450` | `FText` |
| `m_fullscreenText` | 1128 | `0x468` | `FText` |
| `m_windowedFullscreenText` | 1152 | `0x480` | `FText` |
| `SwitchTabTextBlock` | 1176 | `0x498` | `UObject*` |
| `TabLeftButton` | 1184 | `0x4A0` | `UObject*` |
| `TabRightButton` | 1192 | `0x4A8` | `UObject*` |
| **— no reflected property —** | **1200–1328** | **`0x4B0`–`0x530`** | **128 non-reflected bytes, see §4** |
| `m_resolutions` | 1328 | `0x530` | container (outer descriptor genflag matches `m_screenPercentages`'s; inner element genflag matches the `FString` shape) |
| `m_screenPercentages` | 1408 | `0x580` | container, `TSet<uint32>` per prior investigation; inner element genflag is the small-integer shape |

(The `PropPointers[]` array match noted above hit 33/34 of the entries I manually walked exactly;
the one mismatch was because `m_resolutions` and `m_screenPercentages` each contribute **two**
array slots — inner element descriptor *and* outer container descriptor — and I'd only listed one
of the two for `m_screenPercentages` in my manual walk. `NumProperties=36` resolves this
unambiguously: 32 simple properties + 2 containers × 2 slots = 36. The **offsets themselves**
(1328, 1408) come directly from each container's own outer descriptor at `+0x24`, independent of
the array-matching exercise, so this footnote affects nothing load-bearing.)

`m_screenPercentages` and `m_resolutions` are **already ordinary reflected properties** —
`RESEARCH-STRAY-MENU-OPTIONS.md` and the task brief both confirm UE4SS Lua can already read/write
them by name (`obj.m_screenPercentages`) with no `RegisterCustomProperty` needed. The brief's own
experiment already established that writing them, on the live object *and* on the class default
object, has **no effect** on what the row displays.

---

## 3. `ListBoxWidget` — full reflected-property offset table (HARD)

Native base of `UMG_ListBox_C` (`/Script/Hk_project.ListBoxWidget`). Descriptor table at file
offset `0x48e7458` (`SelectionBorder`) through `0x48e75d0`+`0x30` (`m_selectedSelectionBorderColor`),
same 48-byte stride. `PropPointers[]` array at file offset `0x3bf0970`, matched **exactly** (9 of
9 entries — this class has no container properties, so no inner/outer split); `NumProperties` in
the neighbouring `ClassParams` reads **9**.

| Property | Offset (dec) | Offset (hex) | Shape |
|---|---:|---:|---|
| `SelectionBorder` | 752 | `0x2F0` | `UObject*` |
| `LeftButton` | 760 | `0x2F8` | `UObject*` |
| `RightButton` | 768 | `0x300` | `UObject*` |
| `Text` | 776 | `0x308` | `UObject*` |
| `ListBoxText` | 784 | `0x310` | `UObject*` |
| `Border` | 792 | `0x318` | `UObject*` |
| `m_listBoxText` | 800 | `0x320` | `FText` (24 B) |
| `m_selectedTextColor` | 824 | `0x338` | `FLinearColor` (struct, 16 B) |
| `m_selectedSelectionBorderColor` | 840 | `0x348` | `FLinearColor` (16 B) |
| **— end of reflected properties —** | **856** | **`0x358`** | |
| **— no reflected property —** | **856–1080** | **`0x358`–`0x438`** | **224 non-reflected bytes, see §4** |

I verified there is nothing reflected hiding immediately after `0x358`: the bytes there are the
*struct-type* metadata tail of the `m_selectedSelectionBorderColor` descriptor itself (a pointer
to the `LinearColor` struct-type name, not a new property), and the *next* valid property-shaped
descriptor found by a wider scan belongs to a visibly unrelated class (`TimeWithoutLoadingBeforeEndingScreen`
at its own offset 40 — a loading-screen widget, structurally nothing to do with settings).

---

## 4. Class sizes, and the two confirmed non-reflected regions (HARD, cross-validated two ways)

**`sizeof(GraphicsSettingsWidget) = 1488` bytes (`0x5D0`), alignment 8.**
**`sizeof(ListBoxWidget) = 1080` bytes (`0x438`), alignment 8.**

Each was found **twice**, independently, and both routes agree exactly for both classes:

**Route A — the class's own `StaticClass()` singleton getter.** Each native `UClass`'s getter
ends by calling one function shared by ~2,609 classes in this binary (VA `0x1412FF250`, matching
the shape of `UClass::GetPrivateStaticClassBody`), passing the class's compile-time `sizeof`/
`alignof`/flags on the stack immediately before the call:

```
GraphicsSettingsWidget's getter, VA 0x1410FAE20:
  0x1410FAEA6   mov dword [rsp+0x30], 0x10000000   ; class flags
  0x1410FAEAE   mov dword [rsp+0x28], 8             ; alignment
  0x1410FAEB6   mov dword [rsp+0x20], 0x5D0         ; <-- sizeof(GraphicsSettingsWidget) = 1488
  0x1410FAEBE   call 0x1412FF250

ListBoxWidget's getter, VA 0x141113F80:
  0x141114006   mov dword [rsp+0x30], 0x10000000
  0x14111400E   mov dword [rsp+0x28], 8
  0x141114016   mov dword [rsp+0x20], 0x438          ; <-- sizeof(ListBoxWidget) = 1080
  0x14111401E   call 0x1412FF250
```

Each getter was located via the class's own `ClassParams` structure (see §1/§2's `PropPointers[]`
xref), reading the neighbouring function-pointer field (`ClassParams-0x40` from the `PropPointers`
xref site) — this is the `ClassNoRegisterFunc`-shaped slot, and disassembling it lands on exactly
this "singleton-once, then call the shared body constructor" shape for both classes.

**Route B — the class's own size-mismatch `checkf`.** UE4's hot-reload path re-validates a
class's registered size against a fresh compile and formats an error naming both the class and
the size if they disagree. That check's trampoline is reached from a small stub next to a
`lea`-to-string, and the class name string it references is the **`U`-prefixed** literal
(`"UGraphicsSettingsWidget"`, `"UListBoxWidget"`) — UE4's `DECLARE_CLASS` macro stringizes the
*full* C++ class name (`#TClass`) and skips the leading `U` at runtime, so the compiled string
literal genuinely carries the `U`, two bytes before the substring an exact name search finds
(confirmed: the four bytes immediately preceding both `"GraphicsSettingsWidget"` and
`"ListBoxWidget"` in `.rdata`, read as UTF-16LE, are `"\0U"` — i.e. a null terminator then `U`).
Once xref'd from that two-bytes-earlier address, both trampolines carry the same size value
`r8d` a second time, at a completely different code location than Route A:

```
GraphicsSettingsWidget, VA 0x1406B0120:
  mov r9d, 0x4D509687
  lea rdx, [format string]
  mov r8d, 0x5D0          ; <-- 1488 again, independent site
  lea rcx, [rip - 2 -> "UGraphicsSettingsWidget"]
  jmp 0x14146CAB0          ; shared checkf handler

ListBoxWidget, VA 0x1406C23F0:
  mov r9d, 0xA8A63505
  lea rdx, [format string]
  mov r8d, 0x438           ; <-- 1080 again, independent site
  lea rcx, [rip - 2 -> "UListBoxWidget"]
  jmp 0x14146CAB0
```

Two unrelated code sites, reached by two unrelated search strategies (one via the class
registration array, one via the class-name string), agree exactly for both classes. I treat the
sizes as **HARD**.

**Consequence — the two non-reflected regions are therefore also HARD-bounded, by simple
subtraction, not by pattern-matching:**

* `GraphicsSettingsWidget`: reflected properties run `0x2F0..0x4A8` (752–1200), the class ends at
  `0x5D0` (1488), and `m_resolutions`/`m_screenPercentages` occupy `0x530..0x5D0` (1328–1488,
  exactly to the end — no reflected *or* non-reflected tail after them). That leaves **exactly
  128 bytes, `0x4B0..0x530` (1200–1328), fully unaccounted for by reflection** — this is real,
  native, non-reflected state, not a scanning gap.
* `ListBoxWidget`: reflected properties end at `0x358` (856), the class ends at `0x438` (1080).
  That leaves **exactly 224 bytes, `0x358..0x438` (856–1080), fully unaccounted for by
  reflection.**

Neither region can be a scanning artefact: they are derived from the *difference* between two
independently, doubly-confirmed numbers (the last reflected offset, and the compiler's own
`sizeof`), not from a heuristic search that could simply have missed something.

---

## 5. Why offset-*value* cross-referencing alone is unsafe (a methodology finding worth keeping)

Before finding the class-size route above, I tried the obvious shortcut: search `.text` for
instructions that write to `[reg+0x580]` (`m_screenPercentages`'s offset) or `[reg+0x330]`
(`ScreenPercentageListBox`'s), on the theory that the function touching the *most* of
`GraphicsSettingsWidget`'s known offsets together must be its constructor or populate function.

**This produced dozens of false positives, and I traced two to ground to be sure why.** One
"19-distinct-offsets-matched" candidate turned out to be an ICU/Unicode-block static-table
initializer (`"VedicExtensions"`, `"PhoneticExtensions"`, `"UnicodeBlock"`, …) whose *stack frame*
(`rbp`-relative locals) happened to number through 0x2F0..0x3E0 by coincidence — filtering out
`rsp`/`rbp`-based memory operands (i.e. requiring a plausible `this`-pointer base register)
removed that specific class of false positive, but did **not** solve the problem: a second
"17-distinct-offsets" candidate (VA `0x1409B09E0`, only 230 bytes — genuinely a small,
plausible-looking constructor, zeroing `[rbx+0x2F0]` through `[rbx+0x370]`) turned out to belong
to a **completely different, unrelated `UUserWidget`-derived class** — one with its own
`OnGameSuspended`/`OnGameUnsuspended` delegates and an `m_stateMachine` field at much lower
offsets (552, 568, 584), that *also* happens to declare ~17 sequential pointer-sized properties
starting right after the ~752-byte `UUserWidget` base, purely by coincidence of member count.

**The underlying fact: dozens of native settings/menu-adjacent classes in this game inherit from
the same `UUserWidget`-sized base and independently declare a similar number of sequential
`BindWidget` pointer properties**, so *the specific integer values* 752, 760, 768, … recur across
many unrelated classes with no shared code between them. Offset-value matching, even restricted
to non-stack base registers, cannot discriminate *which* class a given `[reg+0xNNN]` access
belongs to — only a class-unique anchor (a name string, a registration-array pointer, or a vtable
address) can. Every HARD claim in §2–§4 above is anchored that way; nothing here relies on raw
offset-value search.

I also tried anchoring on the format string `"%d%%"` (present once as ANSI ×2, once as UTF-16 —
plausibly how a percentage row renders its label) and on a cluster of literal `mov`
instructions loading `50, 60, 70, …, 200` (found once, cleanly, via a `skipdata`-safe Capstone
disassembly rather than raw byte pattern matching — the raw-byte version produced the same kind of
false positive as above, landing inside the same Unicode table). Neither format-string nor the
`50..200` immediate cluster resolved to a function that could be tied, by a class-unique anchor,
to `GraphicsSettingsWidget` or `ListBoxWidget` specifically, within this session's time budget.
**I did not find the code that actually builds the row's displayed list, or the click handler.**
Given §4's confirmed regions, I recommend the live, on-box read described in §7 over continuing
this line of static search — it answers the same question far more cheaply and with certainty
rather than inference.

---

## 6. Answers to the four questions from the brief

**Q1 — where is the list built?** Not located with confidence (see §5). What *is* now confirmed:
whatever builds it must write into one of the two non-reflected regions in §4, because there is no
other unaccounted byte range on either class. `m_screenPercentages`/`m_resolutions` (the `TSet`s)
are populated by *something* — the task brief's CDO experiment proves a fresh instance's TSet
value is whatever the CDO held at spawn time, which is standard UE4 CDO-property-copy behaviour —
but that TSet is not what the row reads to render itself (see Q2).

**Q2 — why did the writes have no effect?** Two explanations remain open, now sharper than before
the offset work: (a) native code copies the TSet's contents into one of the confirmed non-reflected
regions **once**, at construct time, and never re-reads the TSet afterward — consistent with the
regions existing exactly where a cache would be expected; or (b) `m_screenPercentages` is used for
something other than populating the display (e.g. validating/clamping an applied value) and the
display was never sourced from it at all. Static analysis in this session could not distinguish
(a) from (b).

**Q3 — is there a data-driven lever?** See the bottom-line verdict above; restated and ranked
exactly per the coordinator's redirect in §8.

**Q4 — what does left/right do?** Not located. `ListBoxWidget` and `GraphicsSettingsWidget` both
expose **zero** reflected `UFunction`s (confirmed independently in `RESEARCH-STRAY-MENU-OPTIONS.md`
§1), which rules out the standard `Button->OnClicked.AddDynamic(this, &Class::Handler)` pattern —
that needs a `UFunction` target. The click handling is therefore almost certainly wired at the
raw Slate level (`TSharedPtr<SButton>`'s native, non-dynamic `FOnClicked` delegate,
`.BindUObject`/`.CreateUObject`), inside a `NativeConstruct()` override — consistent with the
`RESEARCH-STRAY-MENU-OPTIONS.md` §1 finding that the whole row-template family binds this way. I
could not isolate that specific virtual override this session (same §5 problem: virtual dispatch
tables need a class-unique vtable address to anchor on, and I ran out of session budget before
reliably locating `GraphicsSettingsWidget`'s or `ListBoxWidget`'s own vtable pointer with
confidence — the `mov qword [rbx], rax` vtable-store idiom is visible in constructors, as shown in
§5's false-positive example, but attributing a *specific* vtable address to *this* class, rather
than a sibling, needs the same kind of anchor work as §4's size hunt and wasn't completed). Any
clamp against the value list, if the code contains one, has nowhere to read a live index from
except the two §4 regions.

---

## 7. Verdict on Q3, ranked exactly as redirected

**1. Non-reflected member at a known offset, via `RegisterCustomProperty` — the best possible
outcome, and it is *partially* delivered.** Both candidate regions are now HARD-bounded:

* `ListBoxWidget` **`this + 0x358`, 224 bytes, through `this + 0x438`** (the end of the object).
  This is the more promising of the two — it is *per-row-instance* state, which is exactly what
  "this row's current value/index" should be, and 224 bytes is generous: room for e.g. a
  `TArray<int32>` of values (16 B) or a `TArray<FString>` of labels, a current-index `int32`, one
  or two cached `TSharedPtr<SButton>`/`FDelegateHandle` pairs for the native click bindings, and a
  back-pointer to the owning page — all comfortably inside 224 bytes with room to spare.
* `GraphicsSettingsWidget` **`this + 0x4B0`, 128 bytes, through `this + 0x530`**. Less obviously
  row-shaped: 128 bytes doesn't divide cleanly by the row count in any way I'd trust (8 ListBox
  rows × 16 B *would* fit, but so would a single navigation-cursor index plus something else
  entirely — the page also has two slider rows and one checkbox row whose live values must be
  cached *somewhere*, and this is the only unaccounted space on the whole class for that too).

**What is missing before either is safely usable:** the sub-offset and C++ type of whatever field
inside these ranges actually holds the value/index. `RegisterCustomProperty` needs both, and
guessing them (e.g. declaring an `ArrayProperty<IntProperty>` at `+0x358` when the real field is a
`TSet` or a raw `int32[8]`) risks a wrong-shaped Lua-side read/write against live game memory —
exactly the "quiet wrong image" class of failure `CLAUDE.md` warns against, just for UI memory
instead of a render target. **This is not a "stop" — it is one cheap experiment short of done**;
see §8.

**2. A reflected property we missed, an ini value, or a cvar.** `m_screenPercentages` and
`m_resolutions` **are** reflected and already Lua-writable by name — no new discovery needed
there — but writing them is proven inert (task brief's own experiment, and nothing in this
session's static analysis found a second reflected property anywhere on either class: §2 and §3
are exhaustive over both classes' full reflected surface, confirmed by the `NumProperties`
cross-check). No `[SystemSettings]`/`Engine.ini` key or cvar was searched for in this session
(out of scope for a binary-only investigation — a value read from `GConfig` wouldn't necessarily
leave a static trace distinguishable from any other config read) and is **UNCONFIRMED** rather
than ruled out, but nothing found here points at one.

**3. An `.rdata` table patchable at runtime.** **Not found, and unlikely to exist in the simple
form hoped for.** `m_screenPercentages` is a `TSet<uint32>`, populated (if it's populated by
`Add()` calls at all, rather than by a copy from elsewhere) via a sequence of *code*, not a static
const array read at runtime — there is no `50, 60, 70, …, 200` array sitting in `.rdata` for a
patch to retarget. If the *actual* display list is instead built directly by a hardcoded loop
(rather than from the TSet), the loop's step and bounds would be code, not data, putting this
option out of reach without first solving §1.

**4. An inline constant needing a code patch.** **Not found, and I am not naming a candidate.**
The task brief asks that, if this is the only route, I describe it precisely — address, original
bytes, patched bytes. §5 demonstrates concretely why I cannot responsibly do that here: this
investigation's own false positives (a Unicode-block table, an unrelated sibling widget's
constructor) show that *any* address I might otherwise have proposed carries a real, demonstrated
risk of belonging to the wrong class. Naming a wrong address for a **shipped code patch into the
game binary** — a risk class this project has never taken (`CLAUDE.md` never patches game code
except the one validated inline hook, `pool-name-hook`, and that was gated on multiple independent
agreeing constants before being trusted) — would be worse than saying nothing. This rung of the
ladder is honestly empty.

---

## 8. Recommended next step (not run here — needs the box)

A five-minute, **read-only** UE4SS Lua experiment closes the remaining gap far more cheaply than
more static analysis would:

1. Reach the Graphics settings page (`FindFirstOf("GraphicsSettingsWidget")` or similar, per the
   idiom `RESEARCH-STRAY-MENU-OPTIONS.md` already establishes as safe).
2. Dump the raw bytes at object offset `0x4B0..0x530` (128 B) on the `GraphicsSettingsWidget`
   instance, and at `0x358..0x438` (224 B) on the live `ScreenPercentageListBox` row instance
   (`obj.ScreenPercentageListBox`, itself now known-readable at `+0x330` on the page). UE4SS's
   memory-read primitives (or a `RegisterCustomProperty` declared as a raw byte array purely for
   *reading*, which carries none of the type-guessing risk a *write* would) are sufficient; no
   game state needs to change.
3. Click the row's right arrow once (mouse, per the existing hard rule against input injection —
   this needs the user, not an agent).
4. Diff the two byte dumps. Whatever changed is the current-value/index field, at an exact
   sub-offset, with its size implied by how many bytes actually flipped.

This is strictly cheaper than continuing to search `.text` for the populate/click-handler
functions, and it produces certainty instead of inference — exactly the trade the honest §5
failure argues for.

---

## 9. Appendix — key addresses, for reproducibility

All are file offsets into `Stray-Win64-Shipping.exe.unpacked.exe` unless marked VA.

| What | File offset / VA |
|---|---|
| `GraphicsSettingsWidget` descriptor table start (`ScrollBox`) | `0x48d6380` |
| `GraphicsSettingsWidget` `PropPointers[]` array | `0x3bdb4a0` |
| `GraphicsSettingsWidget` `ClassParams` (xref to the array above) | `0x3bdb5f8` |
| `GraphicsSettingsWidget` `StaticClass`-equivalent getter | VA `0x1410FAE20` |
| `GraphicsSettingsWidget` size-carrying `checkf` trampoline | VA `0x1406B0120` |
| `ListBoxWidget` descriptor table start (`SelectionBorder`) | `0x48e7458` |
| `ListBoxWidget` `PropPointers[]` array | `0x3bf0970` |
| `ListBoxWidget` `ClassParams` (xref to the array above) | `0x3bf09e8` |
| `ListBoxWidget` `StaticClass`-equivalent getter | VA `0x141113f80` |
| `ListBoxWidget` size-carrying `checkf` trampoline | VA `0x1406c23f0` |
| Shared `GetPrivateStaticClassBody`-shaped function (≈2,609 callers) | VA `0x1412ff250` |
| Shared `checkf`-style handler | VA `0x14146cab0` |
| False-positive #1 (Unicode block table, do not reuse) | VA `0x14152ff60` region |
| False-positive #2 (unrelated sibling widget ctor, do not reuse) | VA `0x1409b09e0` |

Method scripts used (already present beside the binaries, not modified): `callers.py`, `xref.py`,
`fnbounds.py`. All ad-hoc Python used for the property-table walk and the size cross-check was
written fresh for this session and is not committed (per the task's scope — this document is the
deliverable, not new tooling), but §1's recipe is complete enough to regenerate every number above.
