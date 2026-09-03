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

## The submix spike — `HapticSource = measure | submix-fallback | submix`

**READ FIRST (2026-09-03): every `SUBMIX` and `STATUS` line begins with `COILS: ...` — who is
driving the pad right now. Never infer it from `bound=` or `total=`.** `measure` reports and
leaves the coils to the asset path; `submix-fallback` keeps the asset path until the tap carries a
real signal and WARNS every `SubmixWarnSeconds` meanwhile; `submix` is strict — the asset path
never plays, so anything felt came from the submix. The measured reason the tap sees nothing on
PC, and the `SubmixReroute` / `ForcePS5HapticPath` switches built for it, are in
`docs/STRAY-DUALSENSE.md` §14, "Third live run".

**A SPIKE. Nothing in it has run in the game, and the step that reaches the engine is a
virtual call on a pointer found by a memory scan.** It defaults OFF (`HapticSource = assets`),
and with that default not one line of it executes.

### The idea

Stray's haptics are not special data. They are ordinary sounds played through
`AudioComponent`s, sorted into `SCLASS_controllerVibration`, and routed to their own submixes
— **measured** in the box's own `ue4ss/UE4SS_ObjectDump.txt`:

```
SoundSubmix /Game/Sound/tools/settings/Submix_vibration.Submix_vibration
SoundSubmix /Game/Sound/tools/settings/Submix_vibrationMaster.Submix_vibrationMaster
```

UE 4.27 lets a C++ caller register an `ISubmixBufferListener` on a submix and receive its
rendered buffer. If that works here, the engine hands us every concurrent haptic **already
mixed, faded, looped and levelled** — and that deletes, in one move: the asset extraction to
disk, `haptic_loops.txt`, the asset-name mapping, the fade ramps, the per-component stop
bookkeeping, and the concurrency mixer we were otherwise about to write. It also removes the
redistribution question, since no game audio is ever copied to disk.

**The problem it exists to solve is measured and current:** the asset path has ONE playback
slot, so the ambient rain dies the moment the cat is touched
(`Rain_Loop_VIBE ended (stop=0 superseded=1)`). On PS5 they sum on the coils.

### The interface, verified

Read in UE 4.27.2's `Engine/Source/Runtime/Engine/Public/AudioDevice.h:394-407` (public mirror
`AlexMercer-MA/UnrealEngine-4.27`). **HARD:**

```cpp
class ENGINE_API ISubmixBufferListener
{
public:
    virtual void OnNewSubmixBuffer(const USoundSubmix* OwningSubmix, float* AudioData,
                                   int32 NumSamples, int32 NumChannels,
                                   const int32 SampleRate, double AudioClock) = 0;
};
```

**Exactly one virtual, and NO virtual destructor.** `IsRenderingAudio()` is a UE5 addition and
does not exist here — assuming it would have put a second entry in the vtable and sent the
engine into the wrong function. `NumSamples` is **interleaved samples** (`frames * channels`),
not frames (`AudioDevice.h:400`, `AudioMixerSubmix.cpp:1082`).

### FIRST LIVE RUN, 2026-09-02: the tap did NOT register — and the diagnostics said so

Deployed and run on the box. Everything either side of the discovery worked: the submix
resolved by path, the sink opened the pad endpoint (`sink open=1 'Speakers (DualSense Wireless
Controller)' 4ch 48000Hz`), and the refusal named itself rather than looking like silence.

```
submix: FAudioDevice not found (attempt 1): no FAudioDevice pointer appeared in both the UWorld
and the UEngine object; refusing to guess.
submix discovery: FAudioDeviceHandle-shaped candidates - UWorld -> 8, UEngine -> 3
  UWorld +0x0360 device=482C7780 id=0     UWorld +0x0778 device=1673ACC0 id=0
  UWorld +0x0958 device=4E088C00 id=1     ... none shared with UEngine's three
```

**The rule was wrong twice over.** The shape test — a `{weak ptr; pointer; small int}` triple
whose pointer has an in-image vtable — is really "a pointer to any polymorphic object whose
class lives in the exe", and a `UWorld` is full of UObject pointers, every one of which passes.
And demanding an IDENTICAL pointer in both objects was never sound: a world audio device and
the main audio device are allowed to be different `FAudioDevice` instances. The real candidate
was outvoted by noise it should never have been competing with.

The replacement is positive and per-candidate — not a UObject, holds a standard sample rate,
and then the decisive signals — and the acceptance ladder is now a pure function
(`src/SubmixChoice.cpp`) so CI proves it against **that run's own candidate list**
(`tests/test_submix_choice.cpp`), including a negative control showing the old rule refuses it.

### SECOND LIVE RUN + A MEMORY MEASUREMENT, 2026-09-03

Still did not bind — and then the guessing stopped: the running game's memory was read directly
through `/proc/<pid>/mem`, which settled in one pass what three build cycles had not.

* **`UWorld` points at no audio device at all.** Its handle is empty because the world uses the
  MAIN device. So the world-index match and the shared vtable — the two strongest rungs — are
  **dead on this build**, and a ladder that needed either refuses forever.
* **`UEngine` holds eleven pointees containing a standard sample rate**, so surviving the rate
  test decides nothing.
* **`Engine+0x0A88 -> vt at +0, 48000 at +0x0C`** is the one shaped like a device — and +0x0C is
  exactly where stock UE 4.27 puts it: `FAudioDevice`'s first two data members are
  `int32 NumStoppingSources; int32 SampleRate;` (`AudioDevice.h:1786-1789`) and `FMixerDevice`
  inherits it as the primary base. Prediction and measurement agree.

Why it was missed is mundane: `UEngine` declares **268 UPROPERTYs** before
`MainAudioDeviceHandle` (`Engine.h:1735`), so it sits thousands of bytes in and the 0x2000 window
stopped right about there — and the scan was shape-centric, looking for a whole
`{TWeakObjectPtr; FAudioDevice*; FDeviceId}` triple, so a device pointer not wrapped in exactly
that was invisible at any depth.

**So the discriminator is now the rate's OFFSET, not its presence**, and the scan is
pointer-centric and 0x8000 deep, with a second hop through plain heap blocks — which is how
`FAudioDeviceManager` gets followed without knowing its layout: it has no vtable of its own, but
the devices it owns are ordinary objects and its pointers to them look like any other.

### How the pointer and the call are reached

`FAudioDevice` is not a UObject, `RegisterSubmixBufferListener` is not reflected, and no
Blueprint-callable function in UE 4.27 reaches it — so UE4SS's reflection is no help and the
two pieces have to be found by hand. Both are validated before anything is called, and every
step is logged.

* **The vtable index is 16.** Counted from `AudioDevice.h`'s virtuals in declaration order, and
  the count is unusually trustworthy: **not one `FAudioDevice` virtual sits inside a
  preprocessor conditional**, so the vtable is identical in Shipping, Development and Editor.
  `FAudioDevice : public FExec` is single inheritance with no virtual bases, and `FExec` has
  exactly two unconditional virtuals. HARD for the source, INFERRED for the index. Stray is a
  **licensee build**, so it is a knob (`SubmixRegisterSlot`) and the log dumps the whole vtable
  as RVAs, with `/OPT:ICF`-folded stubs marked, so a wrong value is fixable in one run.
* **The `FAudioDevice*` is found by a STRUCTURAL SCAN, not an offset.** `FAudioDeviceHandle`
  (`AudioDeviceManager.h:81-125`) is `{TWeakObjectPtr<UWorld> World; FAudioDevice* Device;
  Audio::FDeviceId DeviceId;}`; one lives in `UWorld::AudioDeviceHandle` and another in
  `UEngine::MainAudioDeviceHandle`, both at offsets that shift with `#if` blocks. The shape
  test finds them — and, as the live run showed, a great deal else — so each candidate then
  has to earn it:
  * **it must not be a UObject.** `FAudioDevice` is not one, and a UObject's `ClassPrivate`
    (+0x10) chain ends at the `UClass` UClass, which is its own class — a fixed point that
    identifies a UObject whatever its class. **Self-checked** against the known UWorld and
    UEngine: if they do not test as UObjects the offset is wrong for this build, and the
    rejection is disabled and said so rather than discarding the right candidate.
  * **it must hold a standard sample rate.** `FAudioDevice::SampleRate` is an `int32`
    (`AudioDevice.h:1789`) with a second copy in the `PlatformSettings` beside it. A random
    UObject holds none.
  * **then the decisive signals, strongest first:** a UWorld handle whose weak pointer names
    that very world (its `ObjectIndex` equals the world's own `InternalIndex` — exact and
    essentially unforgeable); a UEngine handle with `FAudioDeviceManager*` immediately before
    it (`Engine.h:1732`); or a **shared vtable** across the two objects, which is the
    cross-check that actually holds — two different instances are still both `FMixerDevice`.
  * **and it refuses rather than guesses.** Several undistinguished survivors, or none, is a
    logged refusal; calling a virtual on the wrong heap object is a crash in someone's game.
* **It binds from the GAME THREAD**, from inside the first UFunction hook that fires, and from
  nowhere else. `on_update` is UE4SS's own jthread; a UObject read there is an unsynchronised
  cross-thread read, which this plugin does not do anywhere.
* **Registration is safe cross-thread and asynchronous.**
  `FMixerDevice::RegisterSubmixBufferListener` dispatches with
  `AsyncTask(ENamedThreads::AudioThread, ...)` (`AudioMixerDevice.cpp:2405`), and
  `FMixerSubmix` holds `BufferListenerCriticalSection` across the whole listener loop
  (`AudioMixerSubmix.cpp:1376`) — so we are never re-entered for one submix, and blocking in
  the callback would stall the game's own audio.

### The listener object outlives the DLL, on purpose

Because unregistration is asynchronous, the engine can still hold and call our pointer after
the call returns — and after UE4SS unloads the mod. So the listener is **hand-laid-out**: a
one-qword object, a one-slot vtable, and a 15-byte trampoline, all in a `VirtualAlloc` page
that is **deliberately never freed** (`src/SubmixTap.cpp`). The trampoline loads a target
pointer from that same page and tail-jumps to it, or returns immediately when it is null;
`Detach()` nulls it with one aligned store. After shutdown the engine can call us forever and
reach nothing but a `ret`. The precedent is `src/backend_native/resource_registry.cpp`'s
sentinel — an object whose lifetime the other side controls.

`Detach()` also waits (bounded, 250 ms, and it says so if it expires) for a callback already
*inside* the handler to leave. Nulling the target stops new calls but says nothing about one in
flight, and the ring it may be mid-write into belongs to the `Runtime`, which is about to be
destroyed.

### The numbers proof, and where to read it

Once a second, to the log and to **`<gamedir>/stray-dualsense-submix.txt`** (one line,
rewritten in place, so it can be read with `cat` over ssh with no overlay):

```
SUBMIX measure bound=1 cb=1407 (46.9/s) ch=2 rate=48000 frames/cb=1024 peak=0.03112 rms=0.00714
  bad=0 | master-probe cb=1407 peak=0.41 | ring fill=0/16384 drop=0 under=0
  | sink open=0 '' 0ch 0Hz frames=0 fail=0
```

The proof is three readings, in order: **silence with nothing playing**, **a live signal during
the rain**, and **a HIGHER peak when the rain and a purr overlap** — that last one is the whole
argument, because it is precisely what our one-slot asset player cannot do.

**A null result is a diagnosis, not a shrug**, which is what the master probe is for:

| vibration submix | master probe | reading |
|---|---|---|
| fires, signal | fires | **the tap works and the engine is mixing haptics for us** |
| fires, silent | fires | the tap works; the game is not rendering haptics into that submix on PC |
| never fires | fires | the submix object is wrong (an endpoint/soundfield submix never calls listeners) |
| never fires | never fires | the tap itself is broken — the device pointer or the slot |

The `NO CALLBACKS` line says so explicitly rather than looking like silence, and the ring's
`drop` / `under` counters exist so that a dropped buffer can never be mistaken for a tap that
did not fire.

### What it is NOT

The trigger path is untouched. The asset path is untouched and remains the default. There is
no concurrency mixer, no refactor, and nothing is deleted — if the spike answers "yes", a
follow-up does the deletions.

**Deploy:** `tools/dualsense/deploy-submix-spike.sh [GAMEDIR]`, `--revert` to undo. It restores
`libScePad_orig.dll` and sets `StrayTriggers : 0` first, because the shim and the plugin both
drive the coils and the HID mode byte and would fight.

## Source layout

Only `src/Mod.cpp` includes a UE4SS header. Everything else is plain Win32 and is compiled
and link-tested without the SDK.

| file | role | provable in CI |
|---|---|---|
| `TriggerEffect` | game↔Sony enum translation, `ScePadTriggerEffectParam` layout (§3/§13) | unit test |
| `LoopList` | `haptic_loops.txt` / `spk_loops.txt` | unit test |
| `Fade` | `FadeInTime` / `FadeOutTime` gain ramps | unit test |
| `SubmixDsp` | the spike's signal path: channel fold, soft clip, resampler, level meter, SPSC ring | unit test |
| `SubmixChoice` | which FAudioDevice candidate to believe — the part that got it wrong in the game | unit test |
| `AssetName` | `"SoundWave /Game/.../X.X"` → `X` | unit test |
| `HidMode` | the `valid_flag0` write and its re-assert thread | mingw compile+link |
| `Wasapi` | finds the endpoint, opens a shared-mode 32-bit float stream | mingw |
| `AudioPlayer` | one worker per route (coils / speaker): supersede, level, fades, loop | mingw |
| `ScePad` | binds the game's already-mapped `libScePad.dll`; pad selection by the `connected` byte | mingw |
| `SubmixTap` | the `ISubmixBufferListener` itself: leaked page, hand-built vtable, trampoline | mingw |
| `SubmixDiscovery` | finding `FAudioDevice*` and calling vtable slot 16, with every check | mingw |
| `SubmixSink` | the ring -> RL/RR WASAPI stream for the submix path | mingw |
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

0. **THE SUBMIX SPIKE — MEASURED WORKING up to the pad's endpoint on 2026-09-03** (build
   0.3.0, `docs/STRAY-DUALSENSE.md` §14 "Third live run"): the FAudioDevice search, slot 16, the
   reroute through slot 14, the `DebugPS5Haptic` gate, the handover and the sink all ran in the
   game, twice. **Still UNCONFIRMED: how it feels on the pad** (nobody was holding it), whether
   the PlayStation glyphs show when the pad is the active device, and every session-long
   property (stability across level loads, a second HKPlayerController after a reload). The
   original ranking follows. It defaults OFF.
   Turned on, it calls a virtual on a pointer found by scanning two UObjects for a struct
   shape, at an index counted out of engine source, in a licensee build. Every step is
   validated and logged before the call, and the log line immediately before it names itself as
   the suspect — but a wrong device pointer or a wrong slot is a crash, not a bad number.
   **Run once on the box, 2026-09-02: it refused to register** and named the reason; the
   discovery has been rewritten around that data and CI now tests the ladder against it, but
   the rewrite itself has NOT run in the game. Still unverified beyond it: whether the vtable
   index is 16 in this licensee build, whether Stray's PC build renders any audio into
   `Submix_vibration` at all (the PS5 paths are platform-gated), and whether the buffer we
   would receive is the vibration mix or its parent's accumulation.
   See [The submix spike](#the-submix-spike--hapticsource--measure--submix).

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
