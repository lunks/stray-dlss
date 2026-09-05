# RESEARCH: the in-game DLSS menu — what was closed, what crashed, what works

Companion to `docs/RESEARCH-STRAY-MENU-OPTIONS.md`, the feasibility study of the game's own settings page; this is what happened when it was tried.

**Status 2026-09-04: WORKING, in C++ only.** `mods/StrayDLSS/src/DlssMenu.{hpp,cpp}`. F10 opens
a three-row menu (PERFORMANCE / BALANCED / QUALITY) drawn in the game's own font over the main
menu or gameplay; LEFT/RIGHT choose; F10 closes and applies through `r.ScreenPercentage`, which
NGX answers by recreating the feature (`2573x1448 -> 3840x2160, Quality` in the log). Measured on
the box with screenshots at every state, two full launches. `[STRAYDLSS] DlssMenu=0` disables it.

This document is the ledger of how it got there, because the road is the useful part: six
routes into the game's own settings page were closed by measurement, nine Lua attempts at a new
widget each rendered once and crashed, and the C++ build took six box runs, each of which fixed
exactly one thing. Every claim below is HARD unless marked.

---

## 1. Why not the game's own resolution row (six routes, all closed)

The obvious design was to relabel the Settings page's `ScreenPercentageListBox` with DLSS
presets and let the game's own navigation drive it. Each route below was tried on the box and
closed by what was measured, not by argument:

| # | Route | What was measured | Verdict |
|---|---|---|---|
| 1 | Write the row's option `TSet` on the live page | `GraphicsSettingsWidget` and `ListBoxWidget` expose **zero** UFunctions; the set is a non-reflected member. A bounded memory scan (`src/core/menu_scan`, CI-tested) found it at **+0x580** of a 1488-byte instance and wrote it | Rendered; **reopening the menu rolled it back** — the page is rebuilt from the CDO |
| 2 | Write the CDO as well as the instance | Same scan on the class default object | Rolled back the same way; the values come from elsewhere |
| 3 | Replace the row widget (`page.ScreenPercentageListBox = ours`) | Assignment reflects fine; the game's **native index-based navigation** over 11 `BindWidget` slots never visits a foreign widget; `SetIsFocusable` no effect | Not navigable by gamepad |
| 4 | Hook the apply path (`UGameUserSettings::ApplySettings/SaveSettings`, `HKButton:_OnClicked`) | User changed resolution 70% -> 50% in-game with the gamepad, then walked Settings from the main menu: **none of them fired** | The game applies through a non-reflected path |
| 5 | Watch the page's non-reflected region (0x4B0..0x530) for the write | Watch armed, sampled at 1 Hz across a live change | Nothing observable to hook |
| 6 | `PrintString` on-screen text as the display | Compiled out in Shipping | No output at all |

**Retired conclusions worth keeping:** `UMG_PauseMenu_C` (root `Overlay`) **crashes** when used
as a container; `UMG_DebugMenu_C`, `UMG_HUD_Debug_C`, `UMG_DebugInput_C` (root `CanvasPanel`)
are **inert and safe** — they add to the viewport, draw nothing, survive a resolution change.
UE4SS's `NotifyOnNewObject` fires at **construction**, before `WidgetTree` and `BindWidget`
members exist; reading them there is the AV at 0x10.

## 2. A new widget from Lua: nine crashes, one cause

With the game's row closed, the design became a separate top-level widget. In Lua the shape
that RENDERED was: create `UMG_DebugMenu_C` through `WidgetBlueprintLibrary::Create`,
`AddToViewport`, force `SetVisibility(0)`, then a `UMG_ListBox_C` or `BP_HKTextBlock_C` row
added with `AddChildToCanvas` and laid out top-left at (200,200) 900x70 (centre anchors made
it vanish). It rendered — and the game died seconds later or on the next input, nine times.

| arrangement | survives? |
|---|---|
| container alone (any of the three debug widgets) | yes, even across a resolution change |
| `UMG_ListBox_C` parented into our container | crash |
| `BP_HKTextBlock_C` parented into our container | crash |
| either row inside the game's OWN live settings page | renders fine |

Three theories were built on and retired: the `r.ScreenPercentage` relayout (a crash with **no**
resolution change retired it, after three fixes had been stacked on it), touching the row after
the resize, and the widget type. The distinguishing factor is **who holds a reference**: the
viewport roots a top-level widget and the game's page roots its own children, but a row we
construct and host is referenced by nothing the collector respects, so it is collected and the
next tick reads freed memory. DeepWiki proposed this; the C++ route proved it by being the only
thing that changed.

**Lua cannot express the fix.** Rooting is `UObject::SetRootSet()` and the game-thread hop is a
ProcessEvent pre-callback; neither is reachable from the Lua API. That is the whole reason for
§3, and it is why "we had agreed to use C++" was the right call.

## 3. The C++ route, one fix per run

Every box run fixed exactly one thing, and every fix was checked against the engine or UE4SS
source before it shipped. The game **never crashed** in any of the six runs.

| run | log said | cause (source) | fix |
|---|---|---|---|
| 0 | (never launched: the launcher must run INSIDE the container, as a pushed script — inline `bash -c` through the lock wrapper silently mangles quotes) | — | `/tmp/go.sh` |
| 1 | `Create returned null` | `TextBlock` is not a `UUserWidget` child (`UserWidget.cpp:2021`); null world context | class + context |
| 2 | null again, live `PlayerController` context, intro map | key callbacks run on UE4SS's **event-loop thread**, not the game thread (`UE4SSProgram.hpp:280`) | queue + `RegisterProcessEventPreCallback`, run when `IsInGameThread()` (what UE4SS does for Lua's `ExecuteInGameThread`, `LuaMod.cpp:4094`) |
| 3 | null again, game thread, main menu, `BP_MenuPlayerController_C` | unknown: class or layout | diagnostic build |
| 4 | layout `0/8/16/24`; **control `UMG_DebugMenu_C` created**; `BP_HKTextBlock_C isUserWidget=0` | the class: `BP_HKTextBlock_C` is a `TextBlock` subclass | container = `UMG_DebugMenu_C`, row = `StaticConstructObject` + `AddChildToCanvas` |
| 5 | container + row created and rooted; **F10 applied, NGX rebuilt at Quality**; `WidgetTree` read as 0 | `GetValuePtrByPropertyName` searches the own class only (`UObject.cpp:685`); `WidgetTree` is on the parent | `GetValuePtrByPropertyNameInChain` (`UObject.hpp:359`) |
| 6 | every step OK | — | **on screen** |

And the one that was fixed BEFORE it could fail: the first commit rooted with
`SetFlags(RF_MarkAsRootSet)`. DeepWiki declined to confirm it; UE 4.27 `UObjectBase.cpp:181-185`
consumes that flag **once at `AddObject`** and clears it, so on a live object it is inert. It
compiled clean and would have crashed exactly like the Lua builds. `SetRootSet()` is the real
call, and `IsRootSet()` is read back into the log so a rooting that did not take is an ERROR
line, not a crash.

**`Create` dispatches on the context's TYPE** (`WidgetBlueprintLibrary.cpp:37-62`): a
`PlayerController` context is taken as an implied owner and routed to `CreateWidget(PC)`, which
needs a local controller with a `Player`; only a non-controller, non-widget context reaches
`GetWorldFromContextObject`. On this title the menu's own controller passes, so it is used.

## 4. Method notes that transfer

* **Build the params buffer from the UFunction's own property offsets** (`TFieldRange<FProperty>`,
  `GetOffset_Internal`, return at the `CPF_ReturnParm` property). UE4SS's docs say the return is
  first, UE's convention says last, and the example the docs cite has no parameters; UE4SS's Lua
  bridge (`LuaUObject.cpp:176-303`) uses offsets, so this cannot be wrong about layout.
* **DeepWiki is right about what exists and wrong about specifics.** It found `SetRootSet` by
  hedging, and confirmed the thread; it was wrong about `FText` assignment, `GetName()`'s type,
  and `on_update`'s thread. Read the cited file for every specific the code depends on.
* **A diagnostic that prints the same value for two causes has not diagnosed anything.** Run 5's
  `tree=0` meant "not found", not "null"; the message now says which.
* **One change per run, and run twice before saying it works.** Run 6 was repeated without a
  redeploy: `opened on try 1 / alive +5s / applied QUALITY (67%) / DLSS feature created: 2573x1448 -> 3840x2160, Quality` — identical to run 6.

## 5. What it is not, yet

Three rows, one font, no background panel, no gamepad binding (F10 and the arrows are keyboard
keys through UE4SS's own key table). Persisting the choice, a gamepad chord, and matching the
game's pause-menu styling are all open, and none of them touches the four facts above.
