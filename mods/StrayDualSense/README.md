# StrayDualSense — a UE4SS C++ plugin

Restores Stray's PS5 features on PC/Proton — **adaptive triggers, haptics on the voice coils
and controller-speaker audio** — in **one** DLL, from inside the game's own process, as a
**thin shim between three things that already exist**: the engine's audio graph, Sony's own
`libScePad.dll` (the game's copy), and the pad's Windows audio endpoint.

```
UNREAL SIDE (hooks, src/Mod.cpp)              SONY SIDE (the game's libScePad.dll)
  DebugPS5Haptic gate (pre-hook, per call)      scePadGetHandle
  reroute the two dead endpoint submixes:       scePadSetAudioOutPath(3) + scePadSetVolumeGain
    Submix_vibrationMaster  -> Submix_unused    scePadSetTriggerEffect
    Submix_controllerMaster -> Submix_unused
  tap both (RegisterSubmixBufferListener)     TRANSPORT (src/Runtime.cpp, src/SubmixSink.cpp)
  trigger hooks -> enum translation             ONE WASAPI client on the pad's 4-ch endpoint:
                                                  FL/FR <- speaker tap   RL/RR <- vibration tap
                                                HidMode valid_flag0 = 0x00 (§12, measured)
```

**The engine does the mixing, the fades, the levels, the looping and the speaker's boost chain
(`Submix_controllerPre`'s `SBFX_Boost`).** The plugin makes the two pad submixes render on PC
(their roots are dead endpoint submixes — `docs/STRAY-DUALSENSE.md` §14/§16), moves the samples
from where the engine put them to where the pad reads them, and makes Sony's DLL select the
speaker route. That is the entire job. There is no asset replay, no loop list, no fade ramp, no
fallback of any kind: a silent pad names the step that failed.

**Coils: measured working on the pad from `9553b9e`** (the identical reroute, watchdog, watch,
HidMode and RL/RR write; `docs/STRAY-DUALSENSE.md` §14–§17). **Speaker via the tap: UNCONFIRMED
— never run.** The Sony routing call it depends on IS confirmed (`SONY ACCEPTED the route`,
§16). See [What is unverified](#what-is-unverified).

## What it does

| feature | mechanism | measured in |
|---|---|---|
| **Coils out of rumble emulation** | Writes DualSense USB output report `0x02` with `valid_flag0 = 0x00` (claim nothing) over the raw HID device (SetupAPI + `HidD_GetAttributes`, VID `054C` PID `0CE6`), every 2 s and again on the coil lane's silence → signal edge. Without this every audio path is silent: the firmware is busy synthesising rumble on the same coils. | §12 |
| **Haptics from the engine's own mix** | The game's `StartPS5Vibration` Blueprints play the `_VIBE` asset on the `ControllerVibration` AudioComponent once `DebugPS5Haptic` is true (written from our pre-hook, on the hook's own Context object, every call). `Submix_vibrationMaster` is re-parented under `Submix_unused` at volume 0 and re-registered so the engine renders it; an `ISubmixBufferListener` receives the mix; the sink writes it to **RL/RR**. Rain + purr + scratch **sum**, as on PS5. | §14, §15, §17 |
| **Speaker from the engine's own mix** | The identical treatment for `Submix_controllerMaster` (dead for the identical reason), written to **FL/FR of the same stream**. Tapped post-effects, so the game's own +5 dB `SBFX_Boost` is already in the samples (`SpeakerGain` is 1.0 and is an A/B knob, not a level). | §16 (routing), §18 |
| **Pad speaker ROUTING** | `scePadSetAudioOutPath(3 = SPEAKER)` + `scePadSetVolumeGain({80, 80, 0, 0})`, resolved out of the libScePad the game already mapped and called with the handle `scePadGetHandle` gives us — the retired shim's `audio_probe`, verbatim. **The pad's default routing MUTES its internal speaker**; without this the samples reach the pad and go nowhere. | §16, user-confirmed |
| **Adaptive triggers** | Reads `HKPlayerController::m_scratchablePS5TriggerEffect` (`{Mode, Value1, Value2, Value3}`, game enum space), translates the game's `EPS5TriggerEffectMode` to Sony's (they are in **different orders**), and drives `scePadSetTriggerEffect` on both sides, accumulated per side from `SetPS5TriggerActivated(State, Side)`. | §13 |
| **PS glyphs** | A post-hook on `InputSubsystem:GetGameControllerType` rewrites a gamepad answer to PS5; keyboard prompts are left alone. | §14 |

What is **not** here, on purpose: `scePadSetVibration` (reads two bytes; structurally cannot
carry a waveform — §12), any extracted game asset, any player of ours, the raw HID audio claim
(Sony's call works; a refusal is logged and that is the end of it), and every probe the shim grew.

## Source layout — every file is one of: a hook, the reroute, the tap, the sink, a Sony call, HidMode, triggers, config, log

Only `src/Mod.cpp` includes a UE4SS header. Everything else is plain Win32 and is compiled and
link-tested without the SDK (the mingw lane).

| file | role | provable in CI |
|---|---|---|
| `Mod.cpp` | **hooks**: UFunction registration, reflective parameter reads, the `DebugPS5Haptic` gate, the glyph post-hook, resolving the three submixes and writing the two UPROPERTYs the reroute needs | MSVC lane only |
| `Runtime` | the **two lanes** (coils, speaker) and the one sink; the reroute submission; the watchdog and the watches; the Sony routing call on the pad thread; the status lines | mingw |
| `SubmixDiscovery` / `SubmixChoice` | finding `FAudioDevice*` and calling `RegisterSoundSubmix` (**the reroute**) and `RegisterSubmixBufferListener` (**the tap**) on it, with every check | choice: unit; discovery: mingw |
| `SubmixTap` | the `ISubmixBufferListener` itself: leaked page, hand-built vtable, trampoline; the per-lane meter and ring write | mingw |
| `SubmixDsp` | the **sink's arithmetic**: channel fold, soft clip, resampler, level meter, SPSC ring, and the two-lane interleave onto FL FR RL RR | **unit** |
| `SubmixSink` | the **sink**: one WASAPI client, two rings → one 4-channel stream | mingw |
| `SubmixWatch` | the three pure instruments: the per-start watch (NoData / Silent / Mixed), the N-lane reroute watchdog, the lane verdict | **unit** |
| `HidMode` | the `valid_flag0` write and its re-assert thread | mingw |
| `ScePad` / `PadAudio` | binding the game's already-mapped `libScePad.dll`; pad selection by the `connected` byte; **the Sony calls** (triggers, speaker route) and their measured values | pad audio: **unit** |
| `Triggers` / `TriggerEffect` | per-side accumulation, transmit on change; game↔Sony enum translation and the `ScePadTriggerEffectParam` layout | effect: **unit** |
| `Wasapi` | finds the endpoint, opens a shared-mode 32-bit float stream | mingw |
| `Config` / `Log` / `Platform` / `AssetName` / `Version` | INI + hot reload; mutex-guarded file log; paths; the short asset name for the log lines | mingw |

## Threading

The thread that receives a request never does the work (§11). UFunction hooks run on the game
thread and only record intent (and, for the taps, resolve UObjects — the only place that is
allowed); `on_update` is **not** the game thread (UE4SS's own jthread at ~200 Hz) and reads no
UObject. The HID writer, the trigger worker, the pad watcher and the sink are each their own
thread. The taps run on the engine's **audio render thread** and do no locking, allocation or
I/O there: they fold, meter and write a lock-free ring, and nothing else.

## Install

1. Build the DLL (CI: `.github/workflows/dualsense.yml`, artifact `stray-dualsense-plugin`).
2. `<gamedir>/ue4ss/Mods/StrayDualSense/dlls/main.dll`, and `StrayDualSense : 1` in
   `ue4ss/Mods/mods.txt`. `tools/dualsense/deploy-submix-spike.sh` does this and retires the
   shim arrangement (below) in the same run.
3. **Remove the shim arrangement**: restore the original `libScePad.dll` (rename
   `libScePad_orig.dll` back) and disable the `StrayTriggers` Lua mod. Both would fight this
   plugin for the pad and for the HID report.
4. Optionally drop `StrayDualSense.ini` next to the DLL (or in the mod root, or the game's
   `Binaries/Win64`). Every key already holds its default. Keys from before 0.4.0 are logged as
   retired and ignored.

`<gamedir>` is `.../Stray/Hk_project/Binaries/Win64`, derived from the running executable.

## Requirements

* **Steam's PlayStation Controller Support OFF** — the *global* setting. It rewrites the
  pad's HID output reports ~60×/s and would undo the mode byte continuously.
* **GE-Proton with the `proton-ds5-haptic` series** (11-6 or newer): that is what exposes the
  pad as `Speakers (DualSense Wireless Controller)`, 4ch/48 kHz/float, and what makes
  `scePadSetAudioOutPath` answer something other than `0x80920007`.
* **USB.** The mode byte is written as USB output report `0x02`. Bluetooth uses report `0x31`
  with a CRC and is not written; the plugin logs the device's `OutputReportByteLength` and
  warns when it is not 48.

## Diagnosing

`<gamedir>/stray-dualsense.log`, truncated per session, and `<gamedir>/stray-dualsense-submix.txt`
— **two lines, one per lane, rewritten every second**. Every status line starts with who drives
that pair: `COILS: driven by the SUBMIX` / `SPEAKER: NOBODY - SILENT, and there is no fallback |
<why>`. Read that, never `bound=`.

```sh
L=stray-dualsense.log
# 1. the reroute and both listeners
grep -n "REROUTE submitted\|registration submitted\|REFUSED" "$L"
# 2. both subtrees render: cb= climbing on BOTH lines
grep -o "SUBMIX [a-z]* bound=[0-9] live=[0-9] cb=[0-9]* ([0-9.]*/s)" "$L" | tail -4
# 3. one verdict per Start, per lane: what the engine put in that submix for that asset
grep -n "submix watch \[" "$L" | tail -8
# 4. the handovers
grep -n "FIRST REAL SIGNAL" "$L"
# 5. the pad's own speaker route
grep -n "pad audio: scePad\|SONY ACCEPTED" "$L" | head -3
# 6. the level-load repair, if it fired
grep -n "STOPPED RENDERING\|RE-SUBMITTED" "$L"
```

| what the log says | reading |
|---|---|
| `submix watch [vibration] 'X_VIBE': the engine MIXED it - peak 0.7…` | the coils are on the engine's mix; unchanged from `9553b9e` |
| `submix watch [speaker] 'cat_purr_loop_01_CONTROL': the engine MIXED it` + `FIRST REAL SIGNAL on 'speaker'` + `SONY ACCEPTED the route` | **the speaker works** if it is audible; if not, the fault is downstream of the tap — `SpeakerGain`, `PadSpeakerGain`, or the endpoint's FL/FR |
| `submix watch [speaker] …: NO DATA FROM THE TAP` while `[vibration]` mixes | **the hypothesis "the same reroute works on the speaker tree" is dead**: `Submix_controllerMaster` is not being rendered. Read the `REROUTE` lines and `resolved speaker` (is it a plain `SoundSubmix`?) |
| `submix watch [speaker] …: the engine mixed NOTHING` with `frames` > 0 | rendered but silent: the `_CONTROL` sound is not landing in that tree — the gate (`gate[open=…]`), or `SCLASS_controller`'s default submix is elsewhere |
| `SUBMIX speaker … NO CALLBACKS` with `SUBMIX vibration … cb=` climbing | same as NoData |
| `scePadSetAudioOutPath(3=SPEAKER)=0x80920007` | libScePad believes the pad has no audio: the Proton device tree (§7), not ours |
| `STOPPED RENDERING` → `RE-SUBMITTED` → `cb=` climbing again | a level load dropped the reroute and the watchdog repaired it (§17) |
| `COILS: NOBODY … the listener registration was REFUSED` | a vtable/slot check failed; the ERROR above it names which. Nothing plays, by design |

A `STATUS` line every 30 s carries every counter: `coils=` and `speaker=` with their reasons,
`gate[open= writes= misses=]`, `vib[starts= stops= compStops= live=]`, `spk[starts= stops= live=]`,
`padaudio[api= sonyOk= pathResult= …]`, `trig[…]`, `hid[…]`.

## What is unverified

"UNCONFIRMED" is used in the CLAUDE.md sense: plausible from source, tested in CI where it is
pure, not seen on the box.

1. **The speaker lane, end to end.** That `Submix_controllerMaster` re-parents and renders like
   the vibration master did; that the shipped Blueprint plays the `_CONTROL` asset into that
   tree once `DebugPS5Haptic` is true (the vibration one is measured to; the speaker one shares
   the gate shape, §14); that `SCLASS_controller`'s default submix lies inside that tree (the
   chain was read from the probe, its class routing was not); that the tapped mix on FL/FR is
   what the pad's speaker wants at `SpeakerGain = 1.0`. One `Q` press at a checkpoint and the
   grep above answer all four.
2. **The coil lane is unchanged in behaviour but not in bytes.** The reroute now registers the
   parent, then the vibration master, then the speaker master, in one pass; the sink opens on
   the first lane to carry a real signal and drains both rings. A difference would show as the
   coil `submix watch` verdicts changing from `9553b9e`'s.
3. **Everything that was unconfirmed before** and is unchanged: the licensee vtable slots (16 and
   14, both measured working once), a second `HKPlayerController` after a reload, Bluetooth.
