# StrayDualSense — a UE4SS C++ plugin

Restores Stray's PS5 features on PC/Proton — **adaptive triggers, haptics on the voice coils
and controller-speaker audio** — in **one** DLL, from inside the game's own process. It is the
design that was measured working on 2026-09-01 as a `libScePad` shim + Lua mod
(`tools/dualsense/`, `docs/STRAY-DUALSENSE.md` §12–§13), extracted into a plugin with the
experiments left behind.

**None of this build has been run in the game.** As of CI run 33582881130 (`4e8fb0c`,
2026-09-02) the full plugin DLL now compiles and links under **MSVC** against RE-UE4SS
`68caddcf`, not just the portable core and the mingw proxy lane — `src/Mod.cpp`, the only file
that touches a UE4SS header, included. Compiling is not the same as working: nothing below has
been observed on the box. See [What is unverified](#what-is-unverified) before anything else.

## What it does

| feature | mechanism | measured in |
|---|---|---|
| **Coils out of rumble emulation** | Writes DualSense USB output report `0x02` with `valid_flag0 = 0x00` (claim nothing) over the raw HID device (SetupAPI + `HidD_GetAttributes`, VID `054C` PID `0CE6`), every 2 s and again immediately before each waveform. Without this every audio path is silent: the firmware is busy synthesising rumble on the same coils. | §12 |
| **Haptics as waveforms** | Hooks `StartPS5Vibration` / `...OnAudioComponent`, plays `<gamedir>/haptic/<name>.f32` (stereo float32 @ 48 kHz) on the pad's WASAPI endpoint, **left channel → RL (left grip), right → RR**. Level from the game; fade-in from `FadeInTime`; fade-out from `StopPS5Vibration`'s `FadeOutTime`. | §12 |
| **Loops from the asset** | `<gamedir>/haptic_loops.txt` is the game's own `bLooping` (22 of 63). The caller never decides; a one-shot never loops. | §12 |
| **Per-component stops** | `StopPS5VibrationOnAudioComponent` (~700/session) is honoured only when it names the component that is playing. `StopPS5Vibration` stops everything. | §12 |
| **Adaptive triggers** | Reads `HKPlayerController::m_scratchablePS5TriggerEffect` (`{Mode, Value1, Value2, Value3}`, game enum space), translates the game's `EPS5TriggerEffectMode` to Sony's (they are in **different orders**), and drives `scePadSetTriggerEffect` on both sides, accumulated per side from `SetPS5TriggerActivated(State, Side)`. Both triggers stay in the mask; the per-side mode varies. | §13 |
| **Controller speaker** | `<gamedir>/spk/<name>.f32` (mono @ 48 kHz) → FL/FR on the same endpoint through its **own** `IAudioClient`, so it mixes with the haptics rather than displacing them. The game's `SBFX_Boost` (+5 dB = ×1.7783) is a named constant. | §10 |

What is **not** here, on purpose: `scePadSetVibration` (reads two bytes; structurally cannot
carry a waveform — §12), amplitude envelopes, normalisation, master gain, the lightbar (no
shipped content drives it), glyph/platform overrides, and every probe the shim grew.

## Source layout

Only `src/Mod.cpp` includes a UE4SS header. Everything else is plain Win32 and is compiled
and link-tested without the SDK.

| file | role | provable in CI |
|---|---|---|
| `TriggerEffect` | game↔Sony enum translation, `ScePadTriggerEffectParam` layout (§3/§13) | unit test |
| `LoopList` | `haptic_loops.txt` / `spk_loops.txt` | unit test |
| `Fade` | `FadeInTime` / `FadeOutTime` gain ramps | unit test |
| `AssetName` | `"SoundWave /Game/.../X.X"` → `X` | unit test |
| `HidMode` | the `valid_flag0` write and its re-assert thread | mingw compile+link |
| `Wasapi` | finds the endpoint, opens a shared-mode 32-bit float stream | mingw |
| `AudioPlayer` | one worker per route (coils / speaker): supersede, level, fades, loop | mingw |
| `ScePad` | binds the game's already-mapped `libScePad.dll`; pad selection by the `connected` byte | mingw |
| `Triggers` | per-side accumulation, transmit on change, optional readback | mingw |
| `Runtime` | game intent → the engines; component ownership; `PadVibrationEnabled` | mingw |
| `Config` / `Log` / `Platform` | INI + hot reload; mutex-guarded file log; paths | mingw |
| `Mod.cpp` | UE4SS: hook registration, reflective parameter reads, the two game-state reads | **CI only, MSVC — compiles, CI run 33582881130** |

## Threading

The thread that receives a request never does the work (§11). UFunction hooks run on the game
thread and only record intent; `on_update` is **not** the game thread (UE4SS's own jthread at
~200 Hz) and reads no UObject; each of the HID writer, trigger, coil and speaker workers is its
own thread, and a newer request supersedes one in flight. The two reads of game state
(`PadVibrationEnabled`, the authored trigger effect) happen inside hooks, on the game thread.

## Install

1. Build the DLL (CI: `.github/workflows/dualsense.yml`, artifact `stray-dualsense-plugin`).
2. `<gamedir>/ue4ss/Mods/StrayDualSense/dlls/main.dll`, and `StrayDualSense : 1` in
   `ue4ss/Mods/mods.txt`.
3. Generate the assets and loop lists into `<gamedir>`:
   ```sh
   GAME=/path/to/Stray/Hk_project/Binaries/Win64 tools/dualsense/extract_assets.sh
   GAME=/path/to/Stray/Hk_project/Binaries/Win64 tools/dualsense/wavegen.sh
   ```
   That produces `haptic/*.f32`, `spk/*.f32`, `haptic_loops.txt`, `spk_loops.txt`.
4. **Remove the shim arrangement**: restore the original `libScePad.dll` (rename
   `libScePad_orig.dll` back) and delete the `StrayTriggers` Lua mod. Both would fight this
   plugin for the pad and for the HID report.
5. Optionally drop `StrayDualSense.ini` next to the DLL. Every key already holds its default.

`<gamedir>` is `.../Stray/Hk_project/Binaries/Win64`, derived from the running executable.

## Requirements

* **Steam's PlayStation Controller Support OFF** — the *global* setting. It rewrites the
  pad's HID output reports ~60×/s and would undo the mode byte continuously.
* **GE-Proton with the `proton-ds5-haptic` series** (11-6 or newer): that is what exposes the
  pad as `Speakers (DualSense Wireless Controller)`, 4ch/48 kHz/float.
* **USB.** The mode byte is written as USB output report `0x02`. Bluetooth uses report `0x31`
  with a CRC and is not written; the plugin logs the device's `OutputReportByteLength` and
  warns when it is not 48.

## Diagnosing

`<gamedir>/stray-dualsense.log`, truncated per session. Lines to look for, in order:

1. `adopted pad: user slot N, handle 0x... (connected byte set)` — without it nothing reaches
   the triggers.
2. `hidmode: opened \\?\hid#...` and then `hidmode: wrote valid_flag0=0x00` — without these
   the coils stay in rumble emulation and every waveform is silent.
3. `loops: haptic -> ... (22 looping asset(s))` — an ERROR here means every asset is a one-shot.
4. `fields of SetPS5TriggerActivated: State:bool[1]@0, Side:...@1` — the reflected layout.
5. `trigger effect (authored by the game): mode=3(Feedback) v1=0 v2=2 v3=0 -> Sony Feedback`.
6. `hook NEVER REGISTERED: ...` — that path is dead for the session.
7. `haptics: play CatPurr2_VIBE (... loop=1 ...) -> 'Speakers (DualSense ...)' 4ch 48000Hz`.

A `STATUS` line every 30 s carries every counter, including `compStops ok=/ignored=` (the
per-component rule working) and `effect=... (game)` versus `(FALLBACK)`.

## What is unverified

Ranked by how likely each is to be the first thing that misbehaves. "UNCONFIRMED" is used in
the CLAUDE.md sense: plausible from source, not yet compiled by CI or seen on the box.

1. **Hardware behaviour — all of it.** `src/Mod.cpp` now **compiles under MSVC** against
   RE-UE4SS `68caddcf` (CI run 33582881130, `4e8fb0c`) — see
   [What has been checked](#what-has-been-checked). Compiling only proves the API calls exist
   with the signatures this file assumes; nothing about their runtime BEHAVIOUR — whether a
   hook actually fires, whether a reflected offset is right, whether `GetStruct()` returns the
   struct this code expects — has been observed. That is what the rest of this list tracks.
   One real bug surfaced by the MSVC compile and already fixed: `UStruct::ForEachProperty()` is
   deprecated at this SHA (`[[deprecated]]`, treated as an error under `/WX`) and was replaced
   with the exact call the header's own deprecation message names,
   `TFieldRange<FProperty>(owner, EFieldIterationFlags::IncludeDeprecated)`.
2. **The HID write reaches the pad from inside the game.** The shim did exactly this
   (`open_pad_hid()` / `set_valid_flag0()`, 48-byte `WriteFile`) and it worked; this code is a
   port, plus a caps read (`HidP_GetCaps`) that the shim did not do. If `hidmode: opened` is
   present but waveforms stay silent, suspect the report length first.
3. **The authored effect read.** `FindFirstOf("HKPlayerController")` →
   `m_scratchablePS5TriggerEffect` as an `FStructProperty` → members `Mode/Value1..3` by
   reflection. The Lua mod read the same fields through UE4SS's Lua binding and got
   `3/0/2/0`; the C++ path is UNCONFIRMED. Failure is loud (`FALLBACK` in STATUS) and falls
   back to the same values.
4. **Argument resolution by type.** `Level` is the last float, `FadeInTime` the first of two,
   `FadeOutTime` the first float of a stop; the sound is the object whose full name contains
   `SoundWave`, the component the one containing `AudioComponent`. The log prints every
   resolved argument per call (`StartPS5Vibration args: ...`); read it before trusting a level.
5. **`StopPS5VibrationOnAudioComponent`'s parameters** are assumed to be an `AudioComponent`
   plus an optional float. Its reflected fields are logged at registration.
6. **The speaker loop list.** `spk_loops.txt` is produced by the same `bLooping` rule as the
   haptic list but was never measured. When it is absent the speaker consults the combined
   `haptic_loops.txt` that `extract_assets.sh` writes (it lists every controller-class asset,
   `_CONTROL` included) and says so; only when both are missing is every `_CONTROL` asset a
   one-shot, which is wrong for the purr and logged as an ERROR.
7. **Queue-ahead of 100 ms.** The shim filled the whole 1 s buffer; this keeps ~100 ms queued
   so stops, fades and the 60 Hz level follow promptly. Underruns under Wine's WASAPI would
   show as crackle; raise `QueueAheadFrames` in `AudioPlayer.cpp` if so.
8. **Fades** are new code on both paths and have never been heard.
9. **The mirror's Unreal headers matching the box's UE4SS** — see the CI workflow header.

### What has been checked

* All twelve non-UE4SS sources compile under mingw-w64 GCC 12 with `-Wall -Wextra -Werror`
  and link into a DLL against `ole32 oleaut32 uuid setupapi hid`, importing
  `HidD_GetAttributes`, `SetupDiGetClassDevsW` and `CoCreateInstance` as the design requires.
* **`src/Mod.cpp` compiles and links under MSVC** against RE-UE4SS `68caddcf` (CI run
  33582881130, commit `4e8fb0c`, first green build of the full plugin DLL). Confirmed by that
  compile: the includes `<Unreal/Property/FStructProperty.hpp>` and
  `<Unreal/UScriptStruct.hpp>` (both forwarding headers, silenced via
  `RC_UNREAL_DISABLE_*_DEPRECATION_WARNINGS`), `FStructProperty::GetStruct()` returning a
  `TObjectPtr<UScriptStruct>` that converts implicitly to `UScriptStruct*`, and
  `UScriptStruct : public UStruct` making the upcast in `DescribeFields(type, ...)` legal — all
  previously UNCONFIRMED. `x86_64-w64-mingw32-objdump -p` on the artifact confirms PE32+,
  imports `UE4SS.dll` and `MSVCP140.dll` (dynamic CRT, `/MD` as required), and exports both
  `start_mod` and `uninstall_mod`.
* `TriggerEffect`, `LoopList`, `Fade` and `AssetName` pass their unit tests (`ctest`, 4/4).
* `docs/STRAY-DUALSENSE.md` §12/§13 findings this build encodes, each in one place:
  the flag value and its re-assertion (`HidMode`), the enum order (`TriggerEffect`), per-side
  accumulation (`Triggers`), loops from the asset (`Runtime` + `LoopList`), the component
  ownership rule (`Runtime`), stereo→RL/RR (`AudioPlayer::kCoilRoute`), the +5 dB trim
  (`kSpeakerBoost`), fades (`Fade` + `AudioPlayer`), `Level=0` on the component path meaning
  1.0 (`Runtime::OnStartVibration`).
