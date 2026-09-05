# Adding stray-dlss options to Stray's own settings menu — feasibility

> **OUTCOME 2026-09-04: every route below into the game's OWN settings page was tried on the
> box and closed by measurement, and the menu was built instead as a separate C++ widget — on
> screen, applying, two launches. The ledger of the closed routes, the nine Lua crashes and the
> six-run C++ build is `docs/RESEARCH-STRAY-MENU-CPP.md`. This document is kept as the
> feasibility study it was; read it for how the settings page is built, not for what to do.**


Read-only investigation, 2026-09-02. Same conventions as `CLAUDE.md`: **HARD** = read out of the
game's own reflection data or measured, **SOFT** = general UE4SS/Unreal knowledge not measured
against this game, **UNCONFIRMED** = a real gap, with a one-run experiment that closes it.
**[derived]** = a conclusion built on HARD facts by inference, not itself directly observed.

Evidence source: the live object dump on the SteamOS box,
`.../Hk_project/Binaries/Win64/ue4ss/UE4SS_ObjectDump.txt.keep` (56 MB, taken in gameplay
2026-09-01), read over `ssh root@192.168.0.210 'pct exec 113 -- grep ...'` — read-only, the game
was never touched. Every line number below is a `grep -n` result against that file; re-running the
same grep reproduces it. Line numbers will shift if the dump is regenerated, but the paths and
class names will not.

**Bottom line up front:** Level 2 (real rows inside an existing settings page) is buildable and the
rows will look and behave like the game's own — but their *effect* (toggling SR/NR/FG) has to be
wired entirely by us, because the row widgets are native, closed C++ classes with no exposed hook
point. Level 3 (a new, native-looking category tab) is **not** a bigger version of the same task —
the tab system is four hardcoded pointer slots on a native class with no array, no generic
switching function, and (found late in this investigation) a **second, independent** per-page tab
cycler that would also need to know about a fifth category. Recommend **Level 2**, and read §6
before deciding on Level 3.

---

## 1. How a settings page is built (Q1)

**HARD.** `UMG_GraphicsSettings.UMG_GraphicsSettings_C` (a `WidgetBlueprintGeneratedClass`,
dump line 89415) has a fixed, individually-named `WidgetTree`. Its option rows are not generated
from a runtime list; each is a separately named child instance, authored once:

```
WidgetTree.EffectsQualityListBox      UMG_ListBox_C     (line 97929)
WidgetTree.FrameRateListBox           UMG_ListBox_C     (97930)
WidgetTree.FullscreenModeListBox      UMG_ListBox_C     (97931)
WidgetTree.MeshQualityListBox         UMG_ListBox_C     (97932)
WidgetTree.ResolutionListBox          UMG_ListBox_C     (97933)
WidgetTree.ScreenPercentageListBox    UMG_ListBox_C     (97934)
WidgetTree.ShadowQualityListBox       UMG_ListBox_C     (97935)
WidgetTree.TextureQualityListBox      UMG_ListBox_C     (97936)
WidgetTree.MotionBlurSliderBox        UMG_SliderBox_C   (97937)
WidgetTree.SharpnessSliderBox         UMG_SliderBox_C   (97938)
WidgetTree.VSyncCheckBox              UMG_CheckBox_C    (99165)
```

So there are three reusable **row template** classes: `UMG_ListBox_C` (left/right value cycler),
`UMG_SliderBox_C` (slider), `UMG_CheckBox_C` (checkbox). Each of these Blueprint templates is
itself thin — it just lays out named sub-widgets (line 89356–89372 for `UMG_ListBox_C`:
`SelectionBorder`, `ListBoxText`, `Box`, `LeftButtonImage`/`RightButtonImage`, `Text` (the label),
`LeftButton`/`RightButton`, both `BP_HKButton_C`). **All the row's logic lives one level down, in a
native C++ base class the Blueprint's owner binds to by name** (Unreal's `BindWidget` idiom):

```
/Script/Hk_project.ListBoxWidget    (line 17181) — BindWidget: SelectionBorder, LeftButton,
                                       RightButton, Text, ListBoxText, Border
                                       + m_listBoxText (FText), m_selectedTextColor,
                                         m_selectedSelectionBorderColor
/Script/Hk_project.SliderBoxWidget  (17724) — Border, SliderText, LeftButton, RightButton,
                                       Slider, m_notchCount, hover-color structs, Text (FText)
/Script/Hk_project.CheckBoxWidget   (14458) — Border, CheckBox (BP_HKCheckBox_C), CheckBoxText,
                                       Text (FText), hover-color structs
```

**Neither `UMG_ListBox_C`/`UMG_SliderBox_C`/`UMG_CheckBox_C` nor their native bases expose any
`Function`** in the dump (`grep -c 'UMG_ListBox_C:'` = 24 lines, all `WidgetTree.*`;
`Hk_project.ListBoxWidget:` = 9 lines, all properties, zero `Function`). Compare this to a class
that *does* expose Blueprint functions — `BP_HKPlayerController_C:StartPS5Vibration` (line 91122)
shows up exactly as expected, with its own parameter properties — so the dump format does surface
Blueprint UFunctions when they exist; their total absence here is a real signal, not a dump
artifact. **[derived]** the row's left/right-click handling is wired natively at the Slate level
(`SButton`'s non-dynamic `OnClicked`, or an internal delegate bound directly to a private C++
method) rather than through a reflected `UFUNCTION`+`AddDynamic` pair — consistent with the
UE4SS 3.0.1 (68caddcf) constraint the task brief states: BlueprintCallable UFunctions are callable
from Lua, non-UFunction C++ methods are not. **This is the load-bearing fact for §5.**

**The container**, found by walking every `UMG_GraphicsSettings_C:WidgetTree.*` object (72 total,
lines 89416–99165): a `ScrollBox` (89439, exactly one child slot, `ScrollBoxSlot_0` at 89440) and a
`VerticalBox` named `VerticalBox_130` (89444) with 13 `VerticalBoxSlot` children (slots
1,2,3,4,5,7,8,9,10,12,15,16,17 — exactly the count of the 11 rows above plus 2 label/header
`TextBlock`s). **UNCONFIRMED**: the dump's `[or:]` field is UObject *Outer* (all widgets in a
`WidgetTree` share the tree as Outer; only *Slot* objects are outered to their visual parent panel),
so I cannot prove from static text alone that `VerticalBox_130` is `ScrollBoxSlot_0`'s *content*
rather than merely a WidgetTree sibling. It is the only plausible candidate (nothing else in the
tree is a bare container of that shape), and `GraphicsSettingsWidget` — the native class, see
§2 — exposes a `ScrollBox` BindWidget property (line 15026) but no `VerticalBox_130` property,
meaning native code only cares about the leaf rows and the scroll container, never the layout
box — consistent with `VerticalBox_130` being pure Designer-authored layout. One-run experiment:
`FindFirstOf("UMG_GraphicsSettings_C")`, then walk `obj.WidgetTree.ScrollBox.Slot.Content` (or
equivalently call `AddChildToVerticalBox` on the object found at that path and watch a new row
appear on screen) to settle it definitively; this is exactly the kind of read that
`mods/StrayFur/Scripts/main.lua`'s `get()`/`pcall` idiom already does safely.

## 2. How a change is applied and persisted (Q2)

**HARD.** The settings values themselves are plain reflected properties on a **native** (not
Blueprint) `UGameUserSettings` subclass, `/Script/Hk_project.HKGameUserSettings` (class at line
16065). Its property list (32 lines, 16066–16097+) is a 1:1 match to the row names above:
`MeshQuality` (IntProperty), `ScreenPercentage`/`SteamDeckScreenPercentage` (UInt32Property),
`Sharpness`/`MotionBlurAmount` (FloatProperty), `SteamDeckEffectsQuality`/`SteamDeckMeshQuality`/
`SteamDeckTextureQuality`/`SteamDeckShadowQuality` (IntProperty), plus unrelated audio/input/
gameplay fields (`MasterVolume`, `PadVibrationEnabled`, `InvertLookXEnabled`, …). **No DLSS-shaped
property exists**, unsurprisingly.

**Each settings page also has its own native brain**, one level up from the Blueprint page:
`/Script/Hk_project.GraphicsSettingsWidget` (line 15025) `BindWidget`s **every one of the 11 row
names above by exact name**, plus `GraphicsMemoryBox`/`GraphicsMemoryText`, `VSyncCheckBox`,
`GammaButton`, `BackButton`, `DefaultsButton`, `SwitchTabTextBlock`, `TabLeftButton`,
`TabRightButton` (15026–15057), a `SetProperty m_resolutions` and `m_screenPercentages` (populated
at runtime from the actual supported display modes, 15058–15059), and a run of `TextProperty`
fields that are pre-baked quality-level label text (`m_qualityLowText`, `m_qualityMediumText`, …,
`m_windowedText`, `m_fullscreenText`, 15043–15054).

**`GraphicsSettingsWidget` itself has zero `Function` entries in the dump — same signature as
`ListBoxWidget`.** So: applying a change is not a callable UFunction we can hook (a `SetRow`,
`ApplySettings`, or `OnValueChanged` we could `RegisterHook` on). **[derived]** the wiring —
"EffectsQualityListBox's right-button click decrements/increments `HKGameUserSettings.MeshQuality`,
clamps, re-renders the label, calls the base engine's `ApplySettings`/`SaveSettings`" — is compiled
directly into native, non-reflected C++ inside the eleven fixed `BindWidget` slots.
`HKGameUserSettings` itself has zero own `Function`s too (16065's own package shows none), so
"apply"/"save" is inherited from the stock engine `UGameUserSettings::ApplySettings()` /
`SaveSettings()` — **SOFT**, standard engine API, not itself found as a distinct dumped line for
this subclass but not expected to be (inherited functions are dumped once, on the base engine
class, which was out of scope to grep here).

**What the DualSense mod hooks here, for comparison:** nothing in this menu. `StrayTriggers.lua`
and `mods/StrayDualSense` hook `BP_HKPlayerController_C:StartPS5Vibration` and friends (real,
separately-compiled Blueprint functions — the *positive* case the row widgets are not), and the
native `HKUtilities:GetPlatform` / `InputSubsystem:GetGameControllerType`. Nothing in the existing
codebase has ever touched `UMG_SettingsMenu` or its children. This is genuinely new ground.

**UNCONFIRMED, and it is the practical question for Level 2:** exactly what a left/right click does
end-to-end (which `HKGameUserSettings` field changes, whether `ApplySettings`/`SaveSettings` fires
per-click or only on leaving the page) cannot be read from static reflection since the logic is
non-reflected C++. One-run experiment, following `mods/StrayFur`'s own "before/after" logging
idiom: `LoopAsync` a poll of every `HKGameUserSettings` field while manually clicking
`EffectsQualityListBox`'s right button in-game, diff the snapshots, and watch whether the value
survives a menu close/reopen (proving `SaveSettings` fired) or only a page revisit (proving only
`ApplySettings` fired). **This does not block Level 2** — see §5, our injected row does not need to
call into this native logic at all; it needs its own persistence, exactly like every other
stray-dlss config value already has (`[STRAYDLSS]` keys in `ReShade.ini`).

## 3. Gamepad focus/navigation (Q3)

**SOFT, leaning hand-rolled — UNCONFIRMED which.** No `SelectedIndex`/`CurrentIndex`/`FocusIndex`
property exists on any of the classes above (the only `CurrentIndex` hits in the whole 56 MB dump
are on an unrelated `/Game/GUI/Drone/UMG_Drone_Digicode` widget, lines 117320/117347 — a keypad
minigame, nothing to do with settings). What *does* exist, on every interactive row/button class in
this family, is a bespoke **`Selected`** concept distinct from Unreal's native focus state:

```
HKButton      :SelectedTextColor, m_changeTextColorOnSelected   (15190, 15193)
ListBoxWidget :m_selectedTextColor, m_selectedSelectionBorderColor (17189-17190)
CheckBoxWidget:m_onSelectedSound                                 (14463)
```

A native UMG widget that relies on Slate's built-in keyboard/gamepad focus would style off
`IsFocused()`/`IsHovered()`; a widget that instead carries its own `Selected`-flavoured color and
sound properties is the shape of a hand-rolled highlight-cursor system layered on top of (or
instead of) native focus navigation — matching `InputSubsystem` already being a custom class the
DualSense mod hooks for controller-type detection (`StrayTriggers.lua` line 301). This is
suggestive, not proof: it is equally possible native `UUserWidget` focus navigation drives the
*mechanics* (which widget is "current") while this custom coloring is purely cosmetic ON TOP of
native focus. Both readings are consistent with the evidence found.

**Why it matters, stated by the task brief and worth repeating:** if navigation is a hand-rolled
index (e.g., the currently-selected row is tracked by an int and Up/Down just increments/decrements
it against a fixed count baked in at construct time), an injected row added to `VerticalBox_130`
after construction is invisible to the stick/d-pad — the cursor will skip over it even though it is
on screen and clickable with a mouse. If navigation is native `SetFocus`/`FWidgetPath` traversal
over the panel's actual children, an injected row is picked up automatically because native
navigation walks the live widget tree, not a cached count.

**One-run experiment that settles it, and it must run before committing to either Level:** add a
single throwaway row (even an unwired `TextBlock` with a background color, no logic) to
`VerticalBox_130` via `AddChildToVerticalBox` at runtime, then press d-pad/stick down repeatedly
from the last real row and watch whether the highlight ever lands on the new row. This is a ~10
minute experiment on the box and is the single most important unknown in this whole investigation
— it determines whether Level 2 rows are usable by a gamepad at all, which is the DLSS Super
Resolution target audience's primary input device on this Linux/Steam Deck-adjacent target.

## 4. Localisation (Q4)

**HARD.** The game does use `StringTable` assets for UI text — `/Game/Technical/TAB_GUI.TAB_GUI`
(dump line 87292) is the obvious candidate for settings-menu labels, alongside `CommonTexts`,
`TAB_Names`, `TAB_UIPlaceholders`, etc. (87166–89745). The mechanism is the standard engine one:
`KismetTextLibrary::TextFromStringTable` (line 33722) is a native BlueprintCallable function that
resolves `(TableId, Key) -> FText`, almost certainly called from each settings page's Construct
graph to populate labels once at widget creation.

**But the actual storage on every row/native class found in §1–2 is a plain `TextProperty`**
(`ListBoxWidget:m_listBoxText`, `SliderBoxWidget:Text`, `CheckBoxWidget:Text`,
`GraphicsSettingsWidget:m_qualityLowText` etc. — all `TextProperty`, not a `StringTable` reference
type). An `FText` sourced from a string table and a literal `FText::FromString("DLSS Super
Resolution")` are indistinguishable once constructed — there is no runtime tag forcing a property
to only ever hold a table-sourced value. **[derived]: yes, a literal FText can be handed to a
duplicated row** — set `Text` (or `m_listBoxText`, if the label needs to look like a live value
rather than a static caption) directly via reflection, exactly as `mods/StrayFur` already sets
`FName`-keyed scalar parameters on a live material instance.

**SOFT, not measured in this repo:** whether UE4SS Lua at 3.0.1 (68caddcf) exposes an `FText()`
constructor callable from Lua the way it exposes `FName()` (used throughout `StrayTriggers.lua`).
This is standard, documented UE4SS Lua API surface but has not been exercised anywhere in this
codebase yet. One-line experiment: `obj.Text = FText("test")` (or whatever the binding's actual
constructor name is) against a throwaway row's `Text` property and screenshot it.

## 5. A new category (Q5) — level 3

**HARD, and this is the headline finding.** `UMG_SettingsMenu_C`'s tabs are not data-driven. Its
native base class, `/Script/Hk_project.SettingsMenuWidget` (line 17714), has exactly **eight**
`ObjectProperty` `BindWidget` slots, one per button and one per page, individually named:

```
AudioButton, GraphicsButton, InputButton, GameplayButton     (17715-17718)
AudioSettings, GraphicsSettings, InputSettings, GameplaySettings  (17719-17722)
```

matched exactly by four named children of `UMG_SettingsMenu_C:WidgetTree` (lines 98088–98095):
`AudioSettings`/`GameplaySettings`/`GraphicsSettings`/`InputSettings` (each a full page widget) and
`AudioButton`/`GameplayButton`/`GraphicsButton`/`InputButton` (each `UMG_TabButton_C`), the buttons
sitting in a `HorizontalBox` named `TabBox` with exactly 4 slots (90010–90014). **There is no
array, no count, no generic "for each registered tab" anything** — the same `BindWidget`-by-name
mechanism as §1's rows, and the same consequence: **a fifth, differently-named button and page
added to the `WidgetTree` will render, but `SettingsMenuWidget`'s native code has no property to
bind it to and will never know it exists.** Its click will do nothing unless we wire it ourselves,
and — because `SettingsMenuWidget` shows zero `Function` entries just like `GraphicsSettingsWidget`
in §2 — there is no single hookable "switch to tab N" function to extend; the show/hide logic for
the four existing pages is (by the same reasoning as §1) compiled into non-reflected native code,
one native binding per button.

**A second, independent complication found while confirming this, not anticipated going in:**
`GraphicsSettingsWidget` (§2) *also* exposes `TabLeftButton`/`TabRightButton`/`SwitchTabTextBlock`
(lines 15055–15057) — a **second, per-page** tab-cycling affordance (almost certainly a
shoulder-button "cycle between the four pages while already inside one" control, separate from
clicking a `TabBox` button directly). `UMG_GraphicsSettings_C`'s own `WidgetTree` independently
confirms `TabLeftButton`/`TabRightButton`/`TabLeftIcon`/`TabRightIcon` exist as real children
(98019–98020, 98339–98340). **A native-looking fifth category needs BOTH mechanisms to know about
it** — the `TabBox` button row AND every existing page's shoulder-cycle — or the feature will feel
broken half the time (reachable by mouse-clicking a new button, unreachable by shoulder-cycling
from an adjacent page, or vice versa). Neither is a registered list; both are fixed-count native
code. This roughly doubles the native-behavior-to-fake surface area relative to what §1 alone
suggested.

## 6. Concrete plan

### Level 2 — real rows inside an existing page

**Buildable. The UE4SS Lua/C++ capabilities named in the task brief (`CreateWidget`,
`AddChildToVerticalBox`, `FindFirstOf`, `NotifyOnNewObject`, `ExecuteInGameThread`, `SetText`) are
all standard native `UFUNCTION`-backed UMG/engine calls — exactly the category UE4SS 3.0.1 can
call from Lua — so nothing here needs a capability the toolchain lacks.**

Steps:
1. **Confirm §1's container and §3's navigation** with the two one-run experiments described above
   — do this FIRST; a hand-rolled navigation index (§3 negative) would mean gamepad users cannot
   reach the new rows at all, which changes whether Level 2 is worth shipping before it changes how
   it is built.
2. On `UMG_GraphicsSettings_C` (or whichever page is thematically right — Graphics is the obvious
   home for SR/NR/FG), locate the row container (`ScrollBox.Slot.Content`, expected
   `VerticalBox_130`) via `FindFirstOf`/`NotifyOnNewObject`, on the game thread
   (`ExecuteInGameThread`, per the `StrayFur`/`StrayProbe` threading rule — UI object touches from a
   `LoopAsync`/timer thread are a proven crash, see `mods/StrayFur/Scripts/main.lua`'s own account
   of an `EXCEPTION_ACCESS_VIOLATION` from exactly this mistake).
3. `CreateWidget` a new instance of an existing row template class (`UMG_CheckBox_C` for on/off —
   the closest fit for "DLSS Super Resolution: On/Off" — or `UMG_ListBox_C` if a per-feature
   quality/mode cycle is wanted later), `AddChildToVerticalBox` it into the container, `SetText`
   its label (§4: a literal FText is fine) and give it a distinguishing internal name.
4. **Do not expect the native row logic to do anything for our row.** Per §1/§2, `ListBoxWidget`/
   `CheckBoxWidget`'s click handling binds at the Slate/native level to the ELEVEN specific
   `GraphicsSettingsWidget`-known slots, not generically to "any `ListBoxWidget` instance in this
   tree." Our clone's `LeftButton`/`RightButton` (or `CheckBox`)'s `OnClicked`/
   `OnCheckStateChanged` are still ordinary native multicast delegates on `UButton`/`UCheckBox` —
   standard engine `UFUNCTION`s, and dynamic-multicast-delegate binding from Lua is itself standard
   UE4SS surface (**SOFT**, general UE4SS knowledge, not yet exercised in this repo) — so bind our
   OWN Lua/C++ handler to them directly, rather than relying on `GraphicsSettingsWidget` to notice.
5. **Persistence and the actual effect.** `HKGameUserSettings` cannot gain a new property (native,
   closed, no source) — this rules out reusing the game's own save file. Two options, both
   consistent with precedent already in this codebase:
   - a UE4SS-side sidecar file (`stray-dlss-menu.txt` or similar), matching the exact pattern
     `StrayTriggers.lua`/`StrayProbe` already use for state that must survive/communicate across
     module boundaries; **or**
   - write straight into the same `[STRAYDLSS]` section of `ReShade.ini` that
     `reshade::get_config_value`/`set_config_value` already read (`src/addon.cpp`), if the UE4SS
     side is willing to hand-roll an INI writer — riskier, since `CLAUDE.md`'s own gotchas ledger
     already documents that `ReShade.ini` writes and reads can race the game's own process
     lifecycle, and the comma-list limitation, though irrelevant to a single bool, is a reminder
     that ReShade's own ini reader has real parsing quirks.
   The sidecar-file route is lower-risk and reuses a mechanism already proven working in this
   process (`mods/StrayDualSense`/`StrayProbe` both do atomic sidecar writes today).
6. **Bridge into `stray-dlss.addon64`.** Today, `EnableNGX`/`NgxRR` are read exactly **once**, in
   `on_init_device` (`src/addon.cpp:307-346`) — there is no live toggle for Super Resolution or Ray
   Reconstruction anywhere in the addon; only `NgxNR` (Neural Rendering) has a runtime-live path,
   via the ImGui overlay's `Checkbox("Enabled", ...)` -> `nr::set_enabled()` -> a plain global
   `bool g_enabled` checked at the top of every evaluate (`src/ngx_nr.cpp:767-777`, `:844/:900/
   :910/:1397`) — "the master switch only gates whether `apply()` runs; the runtime stays loaded
   either way" (comment at `src/addon.cpp:1335`). **For a Super Resolution or RR menu toggle to do
   anything, `EnableNGX`/`NgxRR` need the same treatment**: turn `State::ngx_enabled` (already an
   `std::atomic<bool>`, `src/addon.cpp:106`) and a new `ngx_rr` atomic into something re-read from
   the sidecar file on a cadence (e.g. once every N presents in `on_present`), mirroring the
   pattern `nr::set_enabled` already established. This is a real, scoped, moderate C++ change —
   not free, but a small fraction of the total effort, and it is the ONLY piece of this whole plan
   that touches `stray-dlss`'s own D3D12 hot path, so it is also the piece worth the most CI
   coverage (WARP + real-ReShade lanes can prove the toggle doesn't crash; only the human's box can
   prove the image responds).
7. **Frame Generation has no toggle to build yet.** `grep -rn 'FGOutput\|dlssg\|FrameGen'
   src/*.cpp src/*.hpp` finds nothing — FG was only ever explored through OptiScaler
   (`CLAUDE.md` §"OptiScaler frame generation"), and that integration was **dropped by the user
   2026-09-01** with the FG swapchain crash unresolved. A menu row for FG today would control
   nothing; either build it as a visibly-disabled placeholder, or defer it until FG exists as a
   real feature in this addon.

**Effort, Level 2 (SR + NR toggles, no FG):** comparable to — a bit less than —
`mods/StrayDualSense`, which is the only precedent of comparable shape in this codebase (a UE4SS
C++ plugin reading/writing native game objects, with hot-reloadable config) and which, per its own
README, **has never been run in the game** despite compiling clean in CI since 2026-09-02. That
caveat transfers directly: nothing here can be verified without the human's box, each round trip
is expensive (`CLAUDE.md` prime directive #1), and the UI-injection surface (§1/§3) is genuinely
new — no code in this repo has touched a `UMG_*` widget before. Rough order of magnitude: several
box round-trips across the widget-injection, navigation-confirmation, and IPC-bridge pieces before
first light, then more to get persistence and the SR/NR runtime-toggle plumbing solid — call it
comparable to the DualSense plugin's build effort so far, i.e. not a small patch, but well short of
a rewrite.

### Level 3 — a new native-looking category

**Not recommended as a first move.** §5 establishes that the tab system, and the per-page shoulder
cycler, are both fixed-count native `BindWidget` slots with no array and no exposed switch
function — the exact shape that made Level 1 (hijacking) attractive and Level 3 hard for the same
underlying reason (closed native code, nothing generic to extend). A faithful Level 3 needs, on
top of everything in Level 2:

* A fifth `UMG_TabButton_C`-shaped button added to `TabBox`, with its `OnClicked` hooked by us
  (native delegate, same mechanism as Level 2's rows).
* A fifth full page widget (clone `UMG_GraphicsSettings_C`'s shell: `Blur`/`Canvas`/`BackBox`/
  `DefaultsBox`/`ScrollBox`/`VerticalBox` structure) added to the menu's `WidgetTree`.
* **Hand-rolled show/hide for all five pages on every one of the four existing buttons' clicks, in
  addition to our new one** — since native code will still independently show/hide only its own
  four pages when its own four buttons are clicked, our page must be hidden by us whenever any of
  the other four is shown, or two pages will render on top of each other. This means hooking (or
  polling and correcting) all four existing buttons, not just adding a fifth.
* **The same, again, for `TabLeftButton`/`TabRightButton` on every page** (§5's second mechanism) —
  cycling from `GraphicsSettings` via the shoulder button needs to reach our page, and cycling away
  from it needs to reach the *next* real page, both hand-wired.
* Re-verifying §3's navigation experiment specifically for the tab row, since it may use a
  different (or the same) hand-rolled mechanism than the row list.

None of this is unbuildable — it is the same toolkit as Level 2, applied to roughly 2-3x the
surface (a page's worth of rows, plus two independent switching systems instead of zero) — but the
failure mode is worse: a broken row is one dead control; a broken tab desyncs the whole menu (two
pages visible, or the DLSS tab unreachable from one of the two cycling paths). **Recommendation:**
ship Level 2 first, inside Graphics, with a clear "DLSS" section header row (a `TextBlock`, no
native binding required, purely cosmetic) separating it from the stock rows. That gets most of the
visual "it belongs here" goal from the user's original ask without touching either switching
system, and is the natural place to stop unless the user specifically wants the tab.

## 7. Runtime application, and what already exists to build on (Q7)

`grep -rn "EnableNGX\|NgxNR\|NgxFG\|NgxRR" src/*.hpp src/*.cpp`:

* **`EnableNGX`** (`src/addon.cpp:307-311`) gates whether NGX initialises at all
  (`State::ngx_enabled`, an `std::atomic<bool>`). **Read once, at `on_init_device`. No runtime
  toggle exists today.**
* **`NgxRR`** (`:337-355`) selects SR / RR-probe / RR-full / RR-1 mode. **Also read once, at
  device init**, via `ngx::set_rr_mode`/`taa_hook::set_ngx_rr`. No overlay control, no live
  re-read.
* **`NgxNR`** (`:415` on) is the one config surface with real hot-reload precedent: the ImGui
  overlay's `Checkbox("Enabled", &g_nr_ui.enabled)` (`:1336`) calls `nr::set_enabled()` live, which
  flips a plain global checked at the top of `nr::apply()`/every evaluate site — **immediate,
  same-frame, no feature teardown, no GPU work either way** (the runtime DLL stays loaded
  regardless — see the comment at `:1335`). This is the model to copy for `EnableNGX`/`NgxRR`.
* **No `NgxFG`/`FGOutput`/`dlssg` key exists anywhere in `src/`.** Frame Generation was only ever
  reached via OptiScaler intercepting our own NGX calls (`CLAUDE.md`, "OptiScaler frame
  generation"), which is explicitly recorded as **dropped 2026-09-01**, box reverted, FG swapchain
  crash unresolved. A menu row for FG has nothing to flip today.

**What "apply on next frame" implies, concretely, per feature:**

| Feature | Today | Cost of a live toggle |
|---|---|---|
| NR | Already live-toggleable | none — reuse `nr::set_enabled` |
| SR (`EnableNGX`) | Init-only | needs `on_present`-cadence re-read of the atomic, mirroring NR's pattern; SR's own feature object is created lazily on frame `kNgxInitFrame` already, so an OFF->ON flip after that point needs to trigger creation, not just a flag flip — more than a copy-paste of NR's pattern |
| RR (`NgxRR`) | Init-only | same shape as SR, plus RR's own feature-creation/fallback machinery (`docs/RESEARCH-RR-GBUFFER.md`, `RESEARCH-RR1-DENOISER-CONFIG.md`) already has multiple mode transitions to reason through |
| FG | Does not exist | out of scope until FG is rebuilt as a real feature; no amount of menu wiring gives it something to control |

The overlay/UI plumbing itself (checkbox -> atomic -> per-frame check) is cheap and proven for NR;
extending it to SR/RR is a bounded, well-understood change. The **new** piece this task introduces
is the IPC bridge from a UE4SS-side Lua/C++ mod (living in a different module than
`stray-dlss.addon64`, per §6 step 6) into that same atomic — nothing in the codebase does this
today, because every existing `[STRAYDLSS]` config value is read by the ReShade addon from its own
`ReShade.ini`, never written to it by anything else at runtime.

---

## 8. Top unknowns, ranked

1. **Is navigation native or hand-rolled (§3)?** Determines whether Level 2 rows are reachable by
   gamepad at all — the primary input device for this target. One ~10-minute experiment, must run
   before committing engineering effort.
2. **Does `VerticalBox_130` really sit inside the page's `ScrollBox` (§1)?** Affects only where the
   `AddChildToVerticalBox` call targets; low risk, but currently inferred from absence-of-native-
   reference rather than measured. One `FindFirstOf` + property walk.
3. **What exactly does a native row's click do to `HKGameUserSettings`, and when does it persist
   (§2)?** Not load-bearing for Level 2 (our row doesn't need to replicate it), but load-bearing
   for anyone tempted to make the new row *look* more integrated by writing through the same
   settings object — worth knowing before that temptation becomes Level 1 by accident.

None of the widget classes examined here (`UMG_ListBox_C`, `UMG_SliderBox_C`, `UMG_CheckBox_C`,
`UMG_SettingsMenu_C`, `UMG_GraphicsSettings_C`, `UMG_TabButton_C`) are `DynamicClass`/nativized —
every one dumped as an ordinary `WidgetBlueprintGeneratedClass` under a `/Game/` package (e.g. line
89415), unlike `BP_CatPawn_C` (`mods/StrayFur`'s nativized precedent, whose *defaults* are
unreadable though its *instances* are editable). **This is a genuine advantage over the Fur
precedent**: these Blueprints are cooked normally, so their CDOs/defaults should be inspectable,
not just their live instances — not separately re-verified here since it was not needed for this
investigation, but worth confirming with a `StaticFindObject`-style default lookup if Level 3 is
ever attempted and needs to clone a page's exact designer layout offline.
