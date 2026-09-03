# Stray — DualSense support on PC/Proton

Measured 2026-09-01 on the target box (RTX 4090, SteamOS guest, `GE-Proton-dxvk301-ds5-clean-nowl`).
Same conventions as `CLAUDE.md`: **HARD** = read out of a binary or measured on hardware,
**[derived]** = inferred from something HARD, **SOFT** = a claim we have not verified.

**Result so far:**

* **Adaptive triggers: WORKING**, driven from the game's own scratch signals, using the
  values the game itself authored. See §6. The game drives **each trigger separately** —
  see §8.
* **Haptics: WORKING, as real waveforms on the voice coils** — the native DualSense
  mechanism, not motor emulation. See §12, which supersedes the envelope approach in §9.
* **Controller speaker: WORKING AGAIN, confirmed on the pad 2026-09-03.** It had regressed when
  the `libScePad` shim was retired, removing the only caller of
  `scePadSetAudioOutPath(3 = SPEAKER)` — and the pad's default routing MUTES its internal
  speaker. The plugin now makes those calls itself through `scePadGetHandle`, with no proxy DLL:
  `scePadSetAudioOutPath(3=SPEAKER)=0x00000000`, `SONY ACCEPTED the route`. See §16.
* **Coil haptics from the submix: INTERMITTENT.** Confirmed working in `BaseMap` (peak 0.708),
  then silent for a whole session in `03_Slums`. Two defects found: the watch could not tell
  "no data" from "silence", and the reroute was only ever submitted once so a level load ended
  it. Both fixed, neither run. See §17.
* **0.4.0 (2026-09-03): the plugin is a THIN SHIM, and the speaker comes from the engine's own
  submix too.** `Submix_controllerMaster` gets the identical reroute + tap as the vibration
  master and lands on FL/FR of the one stream that carries the coils on RL/RR; the asset
  replay path, its extraction tooling, `HapticSource` and its fallback, and the HID
  speaker-route claim are all deleted. **The speaker lane is UNCONFIRMED — never run.** See §18.
* **Lightbar: implemented in the binary, never driven by any shipped content.** Driving it
  would be inventing a feature, not restoring one — no evidence Stray designed a behaviour.

The PC build implements all of this and simply never asks for it; we drive it from a
`libScePad` shim plus UE4SS hooks.

---

## 1. What the PC build actually ships

**HARD.** `libScePad.dll` — Sony's official PlayStation pad SDK for PC — ships with the game
(`Hk_project/Binaries/Win64/libScePad.dll`, 176,640 bytes, dated to the install) and is
**mapped into the running process**.

**HARD.** It is **delay-load import #9**, not a static import and not `LoadLibrary`.
*This is why plain xref scans find nothing*: delay-load name strings are referenced by a data
descriptor, never by code. Parse the delay-import directory instead.

The exe resolves ten entry points, and every one has real call sites:

| function | call sites (RVA, unpacked exe) |
|---|---|
| `scePadInit` | `009FD084` |
| `scePadOpen` | `009FC3F4`, `009FCE54` |
| `scePadSetVibrationMode` | `009FC44F` |
| `scePadSetVibration` | `009FDE67` |
| `scePadSetLightBar` | `009FC501`, `009FCCF1`, `009FCD6F` |
| `scePadResetLightBar` | `009FC509`, `009FCD07`, `009FCDA2` |
| **`scePadSetTriggerEffect`** | **`009FC6A9`** |
| `scePadGetTriggerEffectState` | `009FC713` |
| `scePadGetControllerInformation` | `009FD44F` |
| `scePadReadState` | `009FD3F2` |

It does **not** resolve the speaker (`scePadSetAudioOutPath`, `scePadSetVolumeGain`) or gyro
(`scePadSetMotionSensorState`) functions the DLL also exports. So the PC build never does
true audio-driven haptics — `scePadSetVibration` drives the voice coils in compatibility
mode, which is why it feels like ordinary rumble.

Game-side content confirms intent, not just linkage: `EPS5TriggersState::{None, Scratchable}`,
`m_scratchablePS5TriggerEffect`, `SetPS5TriggersState`, `PS5Vibration_Jump`,
`PS5Vibration_ZurkGrab`, `PS5VibrationSound_ZurkSucking`, `m_PS5VibrationSubmix`.

---

## 2. The dispatcher, and why triggers never fire

**HARD**, from the Steamless-unpacked exe. A virtual method at **RVA `0x9FC470`**
(`rcx` = pad array, `edx` = controller id, `r8` = property struct) switches on an **FName
device property**:

```
cmp [rbx], FName("SonyLightColor")     -> scePadSetLightBar / scePadResetLightBar
cmp [rbx], FName("PS5TriggerEffect")   -> scePadSetTriggerEffect   (call at 0x9FC6A9)
jne                                    -> return, silently
```

Game-side property struct:

| offset | field |
|---|---|
| `+0x00` | FName — `PS5TriggerEffect` or `SonyLightColor` |
| `+0x08` | triggerMask (bit0 = L2, bit1 = R2) |
| `+0x09` | effect mode selector, 0..3 |
| `+0x0a..0x0c` | three effect parameter bytes |

For `SonyLightColor`: `+0x08` = enable (0 → `ResetLightBar`), `+0x0c/0x0d/0x0e` = colour.

**ROOT CAUSE (HARD): no shipped PC content ever sets the `PS5TriggerEffect` property.**
Searching the Oodle-decompressed pak for `PS5TriggerEffect` / `PS5TriggersState` /
`SetPS5TriggersState` returns **zero files**, against a control that demonstrably fires
(`ObjectProperty` 112 files, `BoolProperty` 70, `/Script/Engine` 112). The vibration half
*does* ship as content (`Sound/SFX/controllers/Vibrations/Scratch_VIBE`, `CatScratch_VIBE`,
64 assets total) — which is exactly why rumble works and triggers do not.

---

## 3. `ScePadTriggerEffectParam` — the layout that cost a day

**HARD**, read from the game constructing the struct at `rbp-0x59` before `0x9FC6A9`, then
confirmed by the library accepting it (`0x00000000`) where four guessed layouts all returned
`0x80920001` `INVALID_ARG`.

```
+0x00  triggerMask          (bit0 L2, bit1 R2)
+0x08  command[0].mode      (uint32)
+0x10  command[0] data bytes
+0x40  command[1].mode      (uint32)
+0x48  command[1] data bytes
```

**COMMAND STRIDE IS `0x38` (56 bytes); the whole block is ~120 bytes.** Strides of
16 / 12 / 20 / 8 are all rejected. This is the single fact that unblocks everything.

**Sony mode numbering**, cross-checked against the game's own dispatch (its 2-parameter
branch emits mode 1; its two 3-parameter branches emit modes 2 and 3), matching the public
SDK: **1 = FEEDBACK{position, strength}**, 2 = WEAPON{start, end, strength},
3 = VIBRATION{position, amplitude, frequency}.

Do **not** infer the mode order from a `strings | sort` of `EPS5TriggerEffectMode` — sorting
alphabetises the enumerators and destroys the numbering.

---

## 4. Getting the pad to libScePad under Proton

libScePad enumerates with `SetupDiGetClassDevsW` + `HidD_GetAttributes` and needs a **real
Windows HID device**. Measured behaviour:

| Steam Input | `winedevice` holds `/dev/hidraw0` | `scePadOpen` | game's controller type |
|---|---|---|---|
| on (default) | **no** — pad is in `SDL_GAMECONTROLLER_IGNORE_DEVICES` | fails | PS5 (glyphs) |
| off | yes | **succeeds** | Xbox (glyphs) |
| on **+ `SDL_GAMECONTROLLER_IGNORE_DEVICES=`** | yes | **succeeds** | PS5 (glyphs) |

The third row is the useful one: Steam Input still provides its virtual X360 node (needed for
automated input) while Proton also passes the real DualSense through as HID.

**HARD, and it kills an attractive theory:** with PS5 glyphs confirmed — i.e.
`GetGameControllerType()` returning PS5 — `scePadSetTriggerEffect` was *still* never called.
**The trigger gate is not controller type.** Glyphs come from Steam Input (`SteamInput005`),
not from libScePad, so they say nothing about the haptic path.

Proton is not a blocker: this build's only `PROTON_DISABLE_HIDRAW` line for DualSense fires
solely for God of War Ragnarök on Steam Deck.

---

## 5. Evidence that it works

Driven from a `libScePad` shim (real DLL renamed `libScePad_orig.dll`, shim exports all 14
names and forwards):

* `scePadSetTriggerEffect` returned `0x00000000` on **every** call — 300+ across sessions,
  `failed=0`, in-game on the real pad handle `0x101`, ~35 min with no crash and no Xid.
* **Hardware readback.** `scePadGetTriggerEffectState` derives its two output dwords from
  `[deviceData+0x85]`, a byte in the **controller's own input report** (nibble per trigger).
  It reads `L2state=1 R2state=1` after engage and `0/0` after release, repeating.
  That is the DualSense reporting its own state, not an API echoing our call.

Trap: the *first* engage after open reads back `0/0` — the pad has not sent an input report
yet. Wait for the second cycle before judging.

---

## 6. Adaptive triggers — DONE, with the game's own values

Driven from the game's own scratch signals via UE4SS, not a heuristic:

```
COMP_CatScratchableComponent_C:_OnUseStarted        -> engage
COMP_CatScratchableComponent_C:SetPS5TriggerActivated(State: bool, Side)
COMP_CatScratchableComponent_C:_OnAfterUseDone      -> release
```

**HARD, read from the running game:** the authored effect is
`HKPlayerController.m_scratchablePS5TriggerEffect` at **+0x730**, a
`PS5TriggerEffectData{Mode, Value1, Value2, Value3}` reading **Mode=3, V1=0, V2=2, V3=0**
— i.e. `FEEDBACK{position 0, strength 2}`: a *light* resistance starting at the very
beginning of trigger travel. Do not invent values; `position=2, strength=8` (an early
guess) feels like a hard block and is wrong.

`EPS5TriggerEffectMode` in **declaration** order is `0 None, 1 Weapon, 2 Vibration,
3 Feedback`, which maps onto Sony's `1 FEEDBACK, 2 WEAPON, 3 VIBRATION`. That mapping was
derived twice independently — from the dispatcher's branch shapes (2-param vs 3-param) and
from the enum order — and they agree. **Never infer the order from `strings | sort`**;
sorting alphabetises the enumerators and destroys it.

Triggers work with Steam Input either on or off. Glyphs are separate: they come from Steam
Input's virtual pad (`type=ps5`), so turning PlayStation Controller Support off loses PS
glyphs but keeps the triggers.

---

## 7. Audio haptics — superseded by §9/§10, kept for the ALSA/Steam findings

**MEASURED: real PS5 haptic waveforms drive the DualSense's voice coils on this box.**
Two things were silently blocking it, neither of which is in any of the write-ups:

1. **The pad's own ALSA `PCM` control was at 0% / −100 dB.** The stream was flowing the
   whole time (`hw_ptr` advancing) straight into an attenuator. `amixer -c 0 sset PCM 80%`.
2. **Steam was rewriting the pad's HID output reports ~60x/sec** — measured at 295 KB per
   3 s on `/dev/hidraw0` — which overrides the audio path continuously. Turning off
   **Steam → Settings → Controller → PlayStation Controller Support** dropped that to
   ~6 KB/4 s. The per-game Steam Input toggle is NOT enough; it is the global setting.

### The asset families — HARD

Two sets, not one 4-channel file:

| path | count | goes to |
|---|---|---|
| `Sound/SFX/controllers/Vibrations/*_VIBE` | **64** | voice-coil actuators |
| `Sound/SFX/controllers/sounds/*_CONTROL` | **4** | controller speaker |

All are `SoundWave` assets holding **Ogg Vorbis, 48 kHz stereo** (`ENCODER=UnrealEngine4`).
The pad's audio device is `4ch 48 kHz, FL FR RL RR` where **FL/FR is the speaker and RL/RR
are the actuators** — so a VIBE asset's stereo pair maps exactly onto the coils, and one
4-channel stream can carry speaker + haptics together. This is the same thing libScePad
does internally (it writes PCM into the pad's 4-channel USB audio interface).

The 4 speaker assets are only `cat_backpack_01`, `cat_backpack_removed_01`,
`cat_purr_loop_01`, `zurg_sucking_loop_02`.

**There is no meow asset in either family** — so "press Circle to meow" is a useless test
event. Good test events: scratching, `drink`, `Pushable1/2`, `Keyboard`, and above all
**purring**, which is the only event exercising both families at once (`CatPurr2_VIBE`
19.4 s + `cat_purr_loop_01_CONTROL`).

### Why Sony's own audio API refuses — HARD, from the disassembly

`scePadIsSupportedAudioFunction` / `scePadGetContainerIdInformation` /
`scePadSetAudioOutPath` all return **`0x80920007`**. Traced to a table of `0x2408`-stride
entries: the pad's handle is at `entry+0x10` and a **byte flag at `entry+0x00`** must be
non-zero. The handle lookup *succeeds* (otherwise the error would be `0x80920003
INVALID_HANDLE`), so the pad is in the table — the **flag is zero**, meaning "this pad has
no audio".

What sets that flag is decided entirely from the **device tree**: libScePad imports
**only SETUPAPI + HID**. There are no audio APIs at all — `WINMM` is used purely for timing
(`timeGetTime`, `timeBeginPeriod`). It reads properties with
**`SetupDiGetDeviceRegistryPropertyW`** (legacy `SPDRP_*`), *not*
`SetupDiGetDevicePropertyW`/`DEVPKEY_*`.

**The structural cause:** on Linux the pad exposes four USB interfaces — three audio
(class 01, `snd-usb-audio`) and one HID (class 03, `usbhid`). Wine's winebus only creates a
node for the interface it claims, so the prefix contains **only
`USB\VID_054C&PID_0CE6&MI_03`** and no audio siblings and no composite parent. libScePad
walks for sibling audio interfaces, finds none, and clears the flag.

**Two registry-only workarounds were tried and both FAILED** — record them so nobody
repeats them:

* adding the `DEVPKEY_Device_ContainerId` property subkey to the HID node (wrong API:
  libScePad uses the legacy `SPDRP_*` call, and the ContainerId values *already matched*
  — HID node `"ContainerId"="{0CE6054C-0001-0004-0000-000000000000}"` equals the audio
  endpoint's `{8C7ED206-...},2`);
* synthesising `MI_00/01/02` MEDIA-class sibling nodes with the same ContainerId.

Neither flips the flag. This needs real code in `setupapi`/`mmdevapi`, not registry edits.

### The actual fix: GE-Proton's `proton-ds5-haptic` series

GE-Proton carries **~191 patches** in `patches/proton-ds5-haptic/`, applied by
`patches/protonprep-valve-staging.sh` (`for patch in ../patches/proton-ds5-haptic/*.patch`),
so **stock GE-Proton releases include them**. The series maps onto the findings above
one-for-one:

| finding | patch |
|---|---|
| uses `SetupDiGetDeviceRegistryPropertyW` | `0005-setupapi-SetupDiGetDeviceRegistryProperty-now-return` |
| ContainerId association HID↔audio | `0002-setupapi-Set-DeviceContainers-registry-values` |
| missing USB audio sibling interfaces | `0003-mmdevapi-setupapi-additionally-set-usb-audio-en` |
| haptics routing | `0013-winepulse-route-dualsense-usb-haptics-to-raw-node` |
| audio path chosen at discovery | `0176-ds5-select-complete-windows-audio-path-during-discovery` |

The series was last updated **2026-08-28**, the same day **GE-Proton11-6** shipped. A
custom `ds5` build from an earlier snapshot is missing the tail of it — that is what
"my ds5 patch is incomplete" means in practice. **Installed `GE-Proton11-6-x86_64` to
`compatibilitytools.d`.**

**Caveat that survives the Proton fix:** Stray's PS5 vibration Blueprints are
**platform-gated** — `StartPS5Vibration`, `StartPS5VibrationOnAudioComponent`,
`StopPS5Vibration`, `SetPS5VibrationLevel` all contain
`CallFunc_GetPlatform_ReturnValue` + an equality test. On Windows that gate is false, so
even with libScePad fully working the game may never call them.
`/Script/Hk_project.HKUtilities:GetPlatform` is a hookable native UFunction returning a
ByteProperty (`EHKPlatform`: `0 Windows, 1 PS4, 2 PS5, 3 XboxOne, 4 XboxSeriesX,
5 WindowsGDK, 6 Unknown`), and overriding it to 2 was tried — it fires, but only ~6 times,
so the value is evidently cached early and the override reaches little. Treat it as
unproven.

### The fallback that works today

`tools`-side daemon (`/tmp/scepad/hapticd.py` on the box) watches
`<gamedir>/stray_haptic.state` and plays any of the 64 waveforms into the actuators,
one-shot or looping, emitting quad with silence in FL/FR. Verified end to end. It does not
need libScePad, the ContainerId, or any Proton patch.

## 7. Tooling notes

* **Steam DRM.** The shipped exe is SteamStub **Variant 3.1 (x64)**: `.text` entropy 8.00,
  entry point in `.bind`. Static analysis is impossible until unpacked. `Steamless` v3.1.0.5
  handles it (runs under `mono`; copy `Steamless.API.dll` out of `Plugins/` next to the CLI).
  A live `/proc/pid/mem` dump of `.text` also decrypts (entropy 6.69) but is not a valid PE.
* **Oodle.** Pak assets are Oodle-compressed (method 2) and the decompressor is **statically
  linked into the exe** — no `oo2core` DLL ships. Build `oozraw` from `rarten/ooz` (gcc,
  x86-64) and drive it with `tools/oodle_unblock.py`.
* **A raw grep of the pak is not a valid control.** Path strings in the index are
  uncompressed while asset contents are not, so a "control" string can match purely from
  filenames. Decompress first, then control on something that only exists inside cooked
  assets (`ObjectProperty`, `BoolProperty`).
* **Never `tar xzf` a payload into the game dir as root.** The archive's `.` entry carries
  the builder's ownership and will chown `Binaries/Win64`, silently blocking *new* file
  creation while existing files still update — which looks like three unrelated failures at
  once (no `UE4SS.log`, no `stray-dlss.log`, no launcher heartbeat).
* `/proc/bus/input/devices`: split handler lines on `=` before matching `event*`; the raw
  token is `Handlers=event13`.


---

## 8. The trigger protocol: the game drives each side SEPARATELY — HARD

`SetPS5TriggerActivated` is called **twice per event, once per side**, a fraction of a
millisecond apart:

```
arg2=true(boolean)  arg3=0(number)      State=true,  Side=Left
arg2=true(boolean)  arg3=1(number)      State=true,  Side=Right
```

`EPS5TriggersSide::PS5TriggersSide_Left` / `_Right` = 0/1, read from the exe's own enum
strings (`EPS5TriggerEffectMode::PS5TriggerEffectMode_Feedback` confirms the mode 1 we send).

**The trap:** carrying a single "current side" value means the second call overwrites the
first and only one trigger ever hardens. Track L and R independently. Keep **both** triggers
in the `triggerMask` and vary the per-trigger *mode* instead — masking one out leaves it
stuck stiff from the previous call.

## 9. Haptics: the level the game asks for — HARD

**There is no global volume to inherit, by design.** The chain is
`Submix_vibration -> Submix_vibrationMaster -> VibrationEndpointSubmix` — an *endpoint*
submix. The game's `MasterVolume` / `EffectsVolume` act on `Submix_menuSliders_sfx` under
`Submix_Master`, a different branch, so they never reach the pad. The settings menu offers
only the `PadVibrationEnabled` **bool** (`HKGameUserSettings`), no intensity slider.

Every stage is **unity gain**: no `Volume` on any SoundWave (the name does not appear in
their name tables at all), no override on `SCLASS_controllerVibration`, none on
`Submix_vibration(Master)`. So the waveform amplitude *is* the authored level, and one
global scale across the asset set is the faithful normalisation.

What the game *does* supply, measured live:

| control | value |
|---|---|
| `StartPS5Vibration(SoundVibration, FadeInTime, Level)` | purr `Level=1.0`, fade 1.0 s |
| `SetPS5VibrationLevel(Level)` @ ~60 Hz | `0.0` idle, `~0.47-0.52` while scratching |
| `PadVibrationEnabled` | honoured as a gate |

**ALSA is NOT in this path.** Measured: rumble works with the pad's `PCM` at 0% / −100 dB
and the switch `off`. The DualSense has no ERM motors — rumble is synthesised on the same
voice coils — so there is also no mechanical dead zone to clear, and no compression is
needed to lift quiet assets. (An earlier compression scheme here was reasoning from classic
rumble hardware this controller does not have; it was reverted.)

**Watch which asset the game actually plays.** Scratching uses `Scratch_VIBE` (peak 217),
not `CatScratch_VIBE` (peak 7). Tuning against the wrong file wastes a session.

## 10. Controller speaker: the pad is a Windows audio endpoint — HARD

Stray ships **four** speaker assets in `Sound/SFX/controllers/sounds/`, all `_CONTROL`
suffixed and referencing `SCLASS_controller` (not `...Vibration`): `cat_purr_loop_01`,
`cat_backpack_01`, `cat_backpack_removed_01`, `zurg_sucking_loop_02`. A full API drives
them — `StartPS5ControllerSound(SoundVibration, FadeInTime, Level)`, `Stop...`,
`SetPS5ControllerSoundLevel`, plus `...OnAudioComponent` variants — gated on the same
platform check as the haptics. **The game calls it during normal play.**

**The route is Windows-side, not ALSA.** With `PROTON_SONY_WINDOWS_DEVICE_NAMES=1` and
`PROTON_KEEP_SONY_AUDIO_ENDPOINT_VISIBLE=1` (both already set by GE-Proton), the pad
enumerates as a WASAPI render endpoint:

```
[2] Speakers (DualSense Wireless Controller)   4ch 48000Hz 32bit
```

FL/FR are the speaker, RL/RR the haptic coils. Write **FL/FR only** — writing the coil
channels never reached them in testing, and the rumble path covers that side. Confirmed:
speaker audio plays with ALSA `PCM` muted, so Sony's path governs it end to end.

### `SBFX_Boost` is a +5 dB gain stage — HARD

`Submix_controllerPre` carries `SBFX_Boost`, a `SubmixEffectDynamicsProcessorPreset`:

```
InputGainDb = +5.000   ThresholdDb = 0.000   Ratio = +1.000   KneeBandwidthDb = 0.000
```

**Ratio 1:1 with a 0 dB threshold means it compresses nothing** — it is a dynamics preset
used purely as a level trim, hence the name. Apply ×1.7783 to speaker playback; without it
the output is audibly quieter than PS5. That the correction sounded right is also the
evidence that these assets really do traverse the `Pre` stage, which the asset graph alone
did not settle.

### The purr is ADPCM, not Vorbis — HARD

The other three `_CONTROL` assets carve trivially at an `OggS` magic. `cat_purr_loop_01`
has no `OggS` anywhere: it is **Microsoft ADPCM** (`wFormatTag=2`, blockAlign 512,
samplesPerBlock 1012) in a RIFF/WAVE layout, split across two UE4 streaming chunks —
261632 bytes inline in the `.uexp` plus 171008 in the `.ubulk`, whose round 256 KiB is
mostly **zero padding**. `261632 + 171008 = 432640 = 845 x 512`, a whole number of blocks,
and the decoded length matches the asset's own `Duration=19.382585` exactly.
`tools/dualsense/ue4_soundwave_extract.py` parses the real structures and handles both the
streamed and inline-Ogg shapes.

**Trim to `TotalSamples`.** ADPCM blocks are fixed-size so the encoder pads the tail — 368
samples here, an audible seam on a looping asset.

### Reading UE4 property values: the tag layout

`FPropertyTag` is `Name(8) + Type(8) + Size(4) + ArrayIndex(4) + HasPropertyGuid(1)`, so a
`FloatProperty` value sits at **+25**, not +28. A wrong offset gave `Ratio = 0.000` for
`SBFX_Boost` — *implausible* rather than merely wrong, which is the only reason it was
caught. Prefer parsing a struct whose values you can sanity-check.

## 11. Mod design

Three pieces, because neither half can do the job alone:

```
   UE4SS Lua mod                command files              libScePad shim
   (knows the game's intent)    (the bridge)               (can call the pad)
   ------------------------     -----------------          --------------------
   hooks Blueprint UFunctions   stray_trigger.state   -->   trigger state thread
   StartPS5Vibration        --> stray_vibe.cmd        -->   vibe watcher -> play thread
   StartPS5ControllerSound  --> stray_spk.cmd         -->   spk  watcher -> WASAPI thread
   GetGameControllerType (glyphs, returns in-process)
```

* **The Lua mod** sees *what the game wants* — which asset, what level, which trigger side —
  but cannot call Sony's API.
* **The shim** replaces `libScePad.dll` (the real one is renamed `libScePad_orig.dll` and
  every export forwards to it). It is loaded because the game loads it, so it can call the
  pad API, but has no idea what the game is doing.
* **Small command files** in the game directory bridge them. Crude, but it works across the
  two runtimes and is drivable from a shell, which is how every test here was run.

**Watchers must never block.** A watcher that plays an envelope inline cannot read the next
command, so a looping sound makes it deaf to `stop` — the purr then never stops. Watchers
only record requests; worker threads do the long work and abort on a sequence bump.

**Give each path its own command file.** Vibration and speaker events fire in the same frame
for the purr; sharing one one-shot file loses whichever wrote first.

### SUPERSEDED 2026-09-01 by a single UE4SS C++ plugin — `mods/StrayDualSense/`

The three-part design above was forced by ONE constraint: a UE4SS **Lua** mod cannot call
Sony's API, so the pad half had to live somewhere that could — hence the `libScePad.dll`
shim, the rename to `libScePad_orig.dll` and the command files bridging two runtimes.

**A UE4SS C++ plugin removes the constraint entirely.** It runs in the game's own process, so
it can resolve libScePad's exports out of the module the game has already mapped
(`GetModuleHandleW(L"libScePad.dll")` + `GetProcAddress`) while hooking the same Blueprint
UFunctions. No proxy DLL, no rename, no `stray_trigger.state` / `stray_vibe.cmd` /
`stray_spk.cmd`.

Everything measured above is unchanged and is what the plugin implements. Three notes:

* **A plugin cannot intercept `scePadOpen`**, which is how the shim obtained its handle.
  `scePadGetHandle(userId, 0, 0)` is used instead — verified to return the same handles.
* **Pad selection was a real bug in the shim, not a port detail.** It bound to the first
  positive handle. Measured: the game opens user slots 1..4 and **all four return positive
  handles** (`0x101`, `0x202`, `0x303`, `0x404`), and `scePadGetControllerInformation` returns
  **success for empty slots too** — they simply come back all-zero, where an occupied slot
  decodes as `pixelDensity 44.86, touchpad 1920x1080, deadzones 13/13, connectedCount 1,
  connected 1`. **The `connected` byte at offset 12 is the only reliable discriminator.**
  Nothing in this API can be judged by its return code.
* **The threading rule of §11 survives, with one addition.** UE4SS fires `on_update` from its
  **own event-loop jthread**, not the game thread (RE-UE4SS `UE4SSProgram.cpp:431,
  m_event_loop = std::jthread{&UE4SSProgram::update, this}`, looping on a 5 ms sleep). UFunction
  hooks DO run on the game thread. So any UE reflection — reading
  `HKGameUserSettings.PadVibrationEnabled`, for one — belongs in a hook, never in `on_update`.

**None of the plugin has been run.** `mods/StrayDualSense/README.md` lists what is unverified
and in what order to check it.

### REWRITTEN 2026-09-01 to §12/§13 — the shim is the reference the plugin was extracted from

The plugin above was built two design generations ago, on §9's envelope model
(`scePadSetVibration` fed RMS envelopes, per-file normalisation, a master gain, a 5 ms loop).
§12 made all of that dead: the API reads two bytes and cannot carry a waveform, and the coils
take real audio once the HID mode byte stops claiming compatible-vibration. §13 changed the
trigger data from constants to the game's authored struct with an enum translation.

`mods/StrayDualSense/` now implements §12/§13, and the relationship between the two trees is:

* **`tools/dualsense/libScePad_shim.c` + `StrayTriggers.lua` are the REFERENCE
  IMPLEMENTATION.** They are what was measured working on hardware, experiments and all
  (`open_pad_hid()` / `set_valid_flag0()`, `hap_play()`, `asset_loops()`, the Lua's
  `findSound` / `findLevel` / `playingComponent`). When the plugin and the shim disagree, the
  shim is the one that was seen to work.
* **The plugin is that design with the experiments removed:** no endpoint/container probes,
  no tone tests, no envelope path, no heuristic trigger, no command files, no `libScePad_orig`
  rename. What it adds over the shim is only what §12 lists as "still not honoured": fades
  (`FadeInTime` / `FadeOutTime` as gain ramps) — and, from the earlier plugin, pad selection
  by the `connected` byte.
* **Still not done in either:** concurrent haptics do not mix (one playback slot per route;
  a new waveform supersedes the current one, where PS5 sums rain + purr + footstep).

> **SUPERSEDED 2026-09-03 by §18.** The plugin no longer plays anything itself: fades, loops,
> levels and concurrency are the engine's, through the rerouted submixes. The shim's
> `hap_play` / `spk_play` and the Lua's argument discovery stay the reference for the hook
> half and the DLL half; the asset-replay half of the plugin is gone.


---

## 12. Coil haptics: the one byte that changes everything — HARD

**MEASURED WORKING 2026-09-01.** The game's VIBE waveforms play on the DualSense's voice
coils as audio, which is what they were authored for. Everything in §9 about envelopes,
normalisation and master gain was compensating for a limitation that no longer applies.

### Why every audio attempt failed until now

The coils were in **rumble-emulation mode the whole time**. DualSense USB output report
`0x02`, byte 1 is `valid_flag0`:

| bit | meaning |
|---|---|
| 0 | `COMPATIBLE_VIBRATION` |
| 1 | `HAPTICS_SELECT` |
| 2 / 3 | right / left trigger FFB data is valid |
| 5 | `SPEAKER_VOLUME_ENABLE` |
| 6 | `MIC_VOLUME_ENABLE` |
| 7 | `AUDIO_CONTROL_ENABLE` |

> **CORRECTED 2026-09-03 against the Linux kernel's own DualSense driver**
> (`drivers/hid/hid-playstation.c:154-164`, `DS_OUTPUT_VALID_FLAG0_*`). Rows 4/5 and 6/7 used
> to read "audio volume / audio path is valid" and "mic LED / mute", and **both were wrong**.
>
> The one that mattered: **there is no "audio path" validity bit.** The output path lives in
> the `audio_control` BYTE at report offset 8, bits 5:4, gated by **bit 7**. Anyone acting on
> the old table would have claimed headphone and speaker volume while supplying zeros — the
> two-writers trap below, in its purest form — and never selected a path, leaving the speaker
> silent while the log said the claim went out. (The mic LED and mute controls are real, but
> they live in `valid_flag1` bits 0 and 1, not here.)
>
> The full corrected layout, the report offsets, the output-path routing table and the
> `never touch bits 0..3` invariant are in `mods/StrayDualSense/src/PadAudio.hpp`, with a unit
> test that pins them (`tests/test_pad_audio.cpp`). **This correction is why §16 exists.**

With bit 0 set, the firmware **synthesises rumble on the coils** from the two motor
amplitude bytes — so the actuators are busy emulating motors and audio sent to them goes
nowhere. Nothing in libScePad ever clears it, because libScePad has no haptic-audio API
(all 25 exports enumerated; only `scePadSetVibration` and `scePadSetVibrationMode` relate
to haptics at all).

**Write an output report whose `valid_flag0` does NOT claim compatible-vibration, and the
coils take the waveform.** Values cross-checked against `EDSVibrationMode` in
`rafaelvaloto/Dualsense-Multiplatform` (MIT): `DefaultRumble = 0xFF`,
`HapticsRumble = 0xFC`.

### The trap that cost two rounds: these are VALIDITY claims, not a mode

Re-asserting `0xFC` every 2 s **broke the adaptive triggers** — first they stopped
resisting, then they latched on permanently after a bump. `0xFC` has bits 2 and 3 set,
which declares *"this report contains valid trigger FFB data"*, and we were sending
**zeros** for it. Whether an effect was cleared or frozen depended on whether our write or
libScePad's landed last.

**Claim only the bits you actually supply.** We send `valid_flag0 = 0x00`: it asserts
nothing, and critically does not re-assert compatible-vibration, which is all that is
needed. `[STRAYDLSS] hapflag <hex>` overrides it live.

The mode must be **re-asserted** (every 2 s, and immediately before each waveform):
libScePad writes its own output reports for triggers and rumble, and those carry the same
flag byte, so a single write at startup is undone the moment the game touches the pad.

### The assets say what the hardware is

| | channels | rate |
|---|---|---|
| `*_VIBE` (haptic) | **stereo** (64 of 67) | 48 kHz |
| `*_CONTROL` (speaker) | mono | 44.1 kHz |

The DualSense has **two coils** (one per grip) and **one speaker**. The haptic assets are
stereo for exactly that reason: left channel drives the left grip. Feed them to the
endpoint's **RL/RR** at 48 kHz float32 — its native mix format, so nothing is resampled.
`tools/dualsense/wavegen.sh` produces them.

### `scePadSetVibration` cannot carry a waveform — HARD, from the disassembly

At RVA `0xDC50` it reads **exactly two bytes** and nothing else:

```asm
movzbl (%rdi),%r8d        ; byte 0
movzbl 0x1(%rdi),%edx     ; byte 1
call   0x18000a8d0        ; -> writes them to report offsets 3 and 4
```

The callee stores them at report offsets **3 (right)** and **4 (left)** — the classic motor
fields. No length, no buffer, and the read is unconditional, so `scePadSetVibrationMode`
cannot change it. This also confirms `{large, small}` == `{left, right}`.

### What was measured and is NOT the answer

Recorded so nobody re-runs them: the coils are **not** reachable via ALSA channels 2/3,
**not** via the WASAPI rear pair while in emulation mode, and there is **no** second
DualSense render endpoint in any device state (all 9 enumerated). Every one of those
negatives was a symptom of the mode bit, not of the audio path.


### Loops come from the asset, never from the caller — HARD

Looping every haptic makes a bump buzz forever: a 0.24 s impact retriggers several times a
second. UE4 only serializes `bLooping` when TRUE, so its presence in a SoundWave's name
table IS the flag — **22 of 63** VIBE assets loop (`CatPurr2`, `Rain_Loop`, `Scratch`; the
impacts do not). `tools/dualsense/wavegen.sh` and the extraction step emit
`haptic_loops.txt`; the shim consults it and **the asset overrides whatever the caller
asks for**.

### Still not honoured, and known

* **Fades.** `StartPS5Vibration` passes `FadeInTime` (the purr's is 1.0 s) and
  `StopPS5Vibration` passes `FadeOutTime`; we start and stop abruptly. libScePad has no
  fade API, but on the coil path we generate the samples so this is only a gain ramp.
* **Concurrent haptics do not mix.** One playback slot: a new waveform supersedes the
  current one. On PS5 they sum on the coils (rain + purr + footstep).
* ~~The authored trigger effect~~ — **DONE**, see §13.

### The game never calls `scePadSetTriggerEffect` — HARD, measured

Count is **0** across a session with triggers working. The PS5 paths are platform-gated and
the native side never reaches the pad, which is why a pak mod calling `SetPS5TriggersState`
also did nothing. **There is no call to pass through; the shim must synthesise it.** The
proxy is otherwise a pure pass-through: all 25 exports forward unmodified, and only
`scePadOpen` (records the handle) and `scePadSetVibrationMode` (logs) have custom wrappers,
both still forwarding unchanged.


---

## 13. The trigger effect is authored data, and the two enums disagree — HARD

`HKPlayerController::m_scratchablePS5TriggerEffect` is a
`PS5TriggerEffectData {Mode, Value1, Value2, Value3}` and it is the game's own definition
of the scratch resistance. It reads **`mode=3 v1=0 v2=2 v3=0`**.

**`EPS5TriggerEffectMode` and Sony's `ScePadTriggerEffectMode` are in DIFFERENT orders.**
Read the exe's enum strings in **binary (declaration) order**, never sorted:

| value | game | Sony |
|---|---|---|
| 0 | None | Off |
| 1 | **Weapon** | **Feedback** |
| 2 | **Vibration** | **Weapon** |
| 3 | **Feedback** | **Vibration** |

So the game's `3` means Feedback, and passing it through unchanged asks Sony for
*Vibration* — an effect that wants `{position, amplitude, frequency}` and does nothing with
`{0, 2}`. The triggers go dead with no error. `sony_trigger_mode()` translates.

Two corollaries:

* The authored values are `Feedback{position 0, strength 2}` — **exactly the constants that
  were hardcoded before**, so the original observation was right. They are now read rather
  than asserted, and will follow if any surface ever specifies something else.
* A default written as `1` meaning "Feedback" became **Weapon** the moment translation was
  added. Defaults must be stated in the enum space they are used in.

### Both triggers, for the whole scratch — HARD, measured

```
21:00:28  state=true  side=0
21:00:28  state=true  side=1      <- same instant
   ... 9 s of scratching, ZERO calls ...
21:00:37  state=false side=0
21:00:37  state=false side=1      <- same instant
```

The game sets both sides together and clears them together, and issues **nothing during the
scratch**. So it hardens **both** triggers for the duration and does **not** alternate them
per paw — the on-screen paw prompt is visual only. Accumulate per side; do not treat a call
as authoritative for the whole state, or the second call of the pair silently wins and only
the right trigger fires.

`SetPS5TriggersState` (the higher-level `_triggersState`/`_triggerSide` function) is never
called — **0 times** in a session with triggers working. Only the component's
`SetPS5TriggerActivated` matters, and it passes exactly `State` + `Side`; the `Trigger
State` / `TriggerSide` entries in the object dump are Blueprint locals, not parameters.

### Lua scope order fails silently — and `luac -p` cannot see it

A `local function` is invisible to functions defined **above** it, so an earlier caller
resolves the name as a **global**, gets `nil`, and throws at runtime. That is what killed
the trigger state write when the effect reader was added below `publish`. The file is
syntactically perfect, so the syntax gate passes it. This is the same declaration-order
mistake the C shim hit five times, except C fails at compile time and Lua does not:
**declare before use, and check it explicitly.**


### The full controller-sound census — HARD, pak-wide

Selecting by **sound class** across all 7,608 `Sound/` headers in the pak (never by folder):

| class | SoundWaves | of which loop |
|---|---|---|
| `SCLASS_controller` (speaker, mono 44.1 kHz) | **4** | 2 |
| `SCLASS_controllerVibration` (coils, stereo 48 kHz) | **66** | 23 |

Every other sound class in the game was enumerated; nothing else routes to the controller.
Every speaker sound has a coil twin of identical duration (backpack on/off, purr, zurg) — the
paired speaker+haptic design found for the purr is how all four work.

**A folder-scoped extraction silently missed two coil assets** that live outside
`Sound/SFX/controllers/`: `character/sentinel/TazerElectricity_VIBE` (one-shot, 0.71 s) and
`gpe/window/window_oneWay_squeak_VIBE` (loops, 0.34 s). Both found only by the class scan.

Names are not a reliable signal for anything:

* `Elevator_loop_VIBE` (30 s) and `WagonEndingStop_VIBE` (28 s) do **not** loop — `bLooping`
  is false; the game plays them once. Looping on the name would have run both forever.
* `strongLight_refilled` has no `_VIBE` suffix and is on the coil class. A suffix-based
  extractor would have missed it.

`tools/dualsense/extract_assets.sh` therefore selects by class pak-wide and takes each asset's
real name from the pak path recorded in its `.json`, and `wavegen.sh` regenerates from a clean
work dir (a stale one once carried the three speaker files into `haptic/`) and refuses to run
without its `ue4_soundwave_extract.py` sibling rather than dying on an import mid-copy.

---

## 14. The submix spike: take the haptics from the engine's own mix — UNCONFIRMED, never run

§12's coil path plays the game's VIBE **assets from disk**, and §9's "one global scale" reading
assumes we are the mixer. Both of those exist because we reproduce what the engine already
does. **The engine may be willing to hand us the finished mix instead.**

### What is MEASURED (2026-09-02, from the box's own `ue4ss/UE4SS_ObjectDump.txt`)

The haptic audio has its own submix chain, loaded and live in the PC build:

```
SoundSubmix /Game/Sound/tools/settings/Submix_vibration.Submix_vibration
SoundSubmix /Game/Sound/tools/settings/Submix_vibrationMaster.Submix_vibrationMaster
SoundSubmix /Game/Sound/tools/settings/Submix_controller.Submix_controller
SoundSubmix /Game/Sound/tools/settings/Submix_controllerPre.Submix_controllerPre
SoundSubmix /Game/Sound/tools/settings/Submix_controllerMaster.Submix_controllerMaster
```

24 submixes exist in total, and the two sound classes `SCLASS_controllerVibration` and
`SCLASS_controller` match the asset selection `tools/dualsense/extract_assets.sh` already
makes pak-wide. This agrees with §9's chain
`Submix_vibration -> Submix_vibrationMaster -> VibrationEndpointSubmix`.

### Why it is worth a spike

The asset path has **one playback slot**: a new waveform supersedes the current one, so the
ambient rain dies the moment the cat is touched — `Rain_Loop_VIBE ended (stop=0 superseded=1)`,
observed by the user. §12 already lists "concurrent haptics do not mix" as a known gap. UE's
own mixer solves it, along with the fades, the loops, the levels and the asset extraction.

### What is verified about the mechanism (UE 4.27.2 source, HARD)

* `ISubmixBufferListener` (`AudioDevice.h:394-407`) has **exactly one** virtual and **no**
  virtual destructor. `IsRenderingAudio()` is UE5 and is not present.
* `NumSamples` is **interleaved samples**, `frames * channels` — not frames.
* Buffer listeners are invoked LAST in `FMixerSubmix::ProcessAudio`
  (`AudioMixerSubmix.cpp:1367-1385`), after the effect chain, after the output volume, and
  after the mix into the parent — so the buffer we see is the parent's accumulation, not this
  submix alone.
* They are only invoked when the owning object `Cast`s to `USoundSubmix`
  (`AudioMixerSubmix.cpp:1370`): an endpoint or soundfield submix registers cleanly and then
  never calls.
* `FMixerDevice::RegisterSubmixBufferListener` (`AudioMixerDevice.cpp:2343-2377`) is safe to
  call from any thread (it `AsyncTask`s to the audio thread) and a **null submix means the
  MASTER submix**. UNregistration is asynchronous, so a listener must outlive the caller.

### What is NOT verified, and is the whole question

* **Whether Stray's PC build renders anything into `Submix_vibration` at all.** The PS5 paths
  are platform-gated (§12: `scePadSetTriggerEffect` is called 0 times), and the submix objects
  being *loaded* proves only that something references them.
* Whether `FAudioDevice::RegisterSubmixBufferListener` really sits at vtable index **16** in
  this licensee build. The index is derived, not measured.
* Whether the `FAudioDevice*` structural scan finds the right pointer.

`mods/StrayDualSense`'s `HapticSource = measure` answers the first without letting anything new
reach the pad, and its master-submix probe distinguishes "the game is silent here" from "our
tap is broken". Deploy with `tools/dualsense/deploy-submix-spike.sh`; the full design,
including why the listener's vtable lives in a leaked page, is in that mod's README.

> **`HapticSource` and its modes were deleted in 0.4.0 (§18).** The tap is the only source now;
> the master-submix probe (`SubmixProbeMaster`) survives as the diagnostic it always was.

### First live run, 2026-09-02: the tap refused, and two defects it exposed

`HapticSource=measure` was deployed and run. **The listener never registered**, and everything
around it worked — the submix resolved by path, and the coil sink opened the pad's endpoint
(`4ch 48000Hz`). Three findings:

1. **The FAudioDevice search was far too loose, and its cross-check was wrong.** It looked for a
   `{TWeakObjectPtr<UWorld>; FAudioDevice*; FDeviceId}`-shaped triple whose pointer had an
   in-image vtable — which is really "a pointer to any polymorphic object whose class lives in
   the exe". A `UWorld` is full of UObject pointers and **8** of them passed, against **3** in
   the UEngine object, with none shared; the rule demanded an identical pointer in both and so
   refused. That demand was never sound anyway: a world audio device and the main audio device
   are allowed to be **different instances**. What IS invariant is that both are `FMixerDevice`,
   so the cross-check is a shared **vtable**, and each candidate now has to earn its place — not
   a UObject (`ClassPrivate` fixed-point test, self-checked against objects known to be
   UObjects), holds a standard sample rate (`FAudioDevice::SampleRate`, `AudioDevice.h:1789`),
   and then a decisive signal: a UWorld handle whose weak pointer names that world, a UEngine
   handle with `FAudioDeviceManager*` immediately before it (`Engine.h:1732`), or a shared
   vtable.
2. **Pad discovery raced the game.** On the first launch slot 1 reported `connected=1` and the
   triggers transmitted; on the second, every slot handed back a handle and an all-zero
   information struct. libScePad only knows about a pad **the game has opened**, and our pad
   thread starts as soon as the module maps — so the probe can simply be too early. The retry
   loop existed but ran at the 2 s steady-state cadence and re-logged the same five lines each
   time; it now polls at 1 s for the first two minutes while no pad is adopted, throttles the
   per-slot dump, treats the first miss as expected rather than an error, and reports
   `probes=`/`misses=` in the STATUS line.
3. **The plugin did not look for its ini where the deploy script wrote it** (the mod root),
   so the first run silently used the default `HapticSource=assets`. The log named the file it
   loaded, which is how it was caught. The search now includes the mod root — the conventional
   place for a UE4SS mod's config — every candidate is logged hit or miss, and the deploy script
   writes beside the DLL where every version looks.

**The lesson worth keeping is the first one's shape.** A structural signature that matches "the
kind of thing we want" is not a test; it has to exclude the far more numerous kind of thing that
merely resembles it. Eight false positives and one refusal is a better outcome than one false
positive and a crash — but only because the refusal was loud and printed its candidates.

### Second live run + a memory measurement, 2026-09-03: the search was looking in the wrong places

The tap still did not bind. But this round the coordinator stopped guessing and **read the
running game's memory through `/proc/<pid>/mem`**, which settled in one pass what three build
cycles of offset archaeology had not. **The method is the finding worth keeping**: the box's
`/proc/<pid>/mem` is readable as root, Wine's addresses map linearly, and the plugin already logs
`World=` and `Engine=` on every attempt — so "guess an offset, build, deploy, launch, read the
log" becomes a measurement taken while the game runs.

**Three things it established, all HARD:**

1. **`UWorld` does not point at the audio device at all.** Every pointer-shaped qword in its
   first 4 KB was followed and none of the targets holds an aligned standard sample rate.
   `UWorld::AudioDeviceHandle` (World.h:1243) is EMPTY on this build — the world uses the main
   audio device rather than requesting its own, so `Device` is null and there is no handle to
   find. Every discriminator that needed UWorld — the weak-pointer index match, a vtable shared
   between the two objects — is therefore **dead here**, and a ladder that depended on one
   refuses forever.
2. **`UEngine` holds ELEVEN pointees containing an aligned standard rate**, so surviving a
   sample-rate test cannot decide anything either.
3. **The one shaped like an audio device is `Engine+0x0A88 -> vt at +0, 48000 at +0x0C`** — and
   that offset is exactly what stock UE 4.27 predicts. `FAudioDevice`'s first two data members
   are `int32 NumStoppingSources; int32 SampleRate;` (AudioDevice.h:1786-1789) and `FMixerDevice`
   inherits `FAudioDevice` as its primary base, so vtable(8) + NumStoppingSources(4) puts
   `SampleRate` at **+0x0C**. The other ten hold their rate hundreds or thousands of bytes in;
   two hold 48000 AND 44100, which no single device does. **Prediction and measurement agree,
   which is the strongest evidence available short of binding.**

**Why the earlier scans missed it, and it is mundane.** `UEngine` declares **268 UPROPERTYs**
before `MainAudioDeviceHandle` (Engine.h:1735), putting it thousands of bytes into the object;
the scan window was 0x2000 and stopped right about there. And the scan was shape-centric — it
looked for the whole `{TWeakObjectPtr; FAudioDevice*; FDeviceId}` triple — so a device pointer
that is not surrounded by exactly that triple was invisible however deep the window went.

**The search is now pointer-centric and the discriminator is the rate's OFFSET, not its
presence.** Plus a second hop through plain heap blocks, which is how `FAudioDeviceManager` is
followed without knowing its layout: it has no vtable of its own so a direct scan cannot see it,
but the devices it owns are ordinary polymorphic objects and its pointers to them look like any
other. A candidate found that way records the container's slot, which names the manager.

**Two defects the same run exposed, both fixed:**

* **`HapticSource=submix` left the user with NO haptics** when the tap did not bind — the asset
  path was disabled on configuration alone. It now stands down only once the tap is bound AND has
  delivered a real signal, so a tap that never binds costs nothing.
* **The sink streamed 1,097,761 frames of pure underrun** to the pad endpoint while unbound. It
  no longer opens the endpoint until the listener has registered.

### Third live run, 2026-09-03: the tap BOUND, the submix is EMPTY, and here is exactly why

`submix bound=1` for the first time — discovery through `UEngine` works — and then
**`total=0` callbacks for a whole session, "last NEVER"**, while the master probe on the same
machinery fired 15,739 times with real signal. So the tap is sound and `Submix_vibrationMaster`
is genuinely never rendered on PC.

**The vibration the user felt that night came from the ASSET path** (`hap[starts=2 played=2]`,
`CatPurr2_VIBE` and `Scratch_VIBE`), which the previous build kept in charge until the tap
carried signal, and nothing in the log said so in words. That is fixed in 0.3.0: every `SUBMIX`
and `STATUS` line now starts with `COILS: driven by the ASSET path (FALLBACK) - what you feel is
NOT the submix` / `COILS: driven by the SUBMIX` / `COILS: NOBODY ...`, the verdict is a pure
function (`src/CoilOwner.cpp`, `tests/test_coil_owner.cpp`), `HapticSource=submix` is now
STRICT (the asset path never plays, so a felt vibration proves the submix) and
`submix-fallback` is the old behaviour with a WARN every 10 s while the fallback is in charge.

#### Why it is empty — HARD, from the UE 4.27.2 source plus one probe read

`mods/StrayAudioProbe` (UE4SS Lua, drivable through `stray-audio-probe.cmd`) read the live
graph:

```
EndpointSubmix  VibrationEndpointSubmix    EndpointType = "Vibration Output"
  SoundSubmix   Submix_vibrationMaster     OutputVolume 1.0, no effects
    SoundSubmix Submix_vibration           OutputVolume 1.0, no effects
EndpointSubmix  ControllerEndpointSubmix   EndpointType = "Pad Speaker Output"
  SoundSubmix   Submix_controllerMaster -> Submix_controller -> Submix_controllerPre (SBFX_Boost)
SoundClass      SCLASS_controllerVibration Properties.DefaultSubmix = Submix_vibration
                                           bIsUISound = true, parent SCLASS_master_soundmix_01
SoundAttenuation PS5VibrationAttenuation   SubmixSendSettings[0].Submix = VibrationEndpointSubmix
HKPlayerController.m_PS5VibrationSubmix  = Submix_vibrationMaster
HKPlayerController.ControllerVibration   = an AudioComponent, Sound null, no attenuation override
```

The root of the vibration tree is a `UEndpointSubmix`, and UE 4.27.2 does this with it:

1. `UEndpointSubmix::GetAudioEndpointForSubmix()` → `IAudioEndpointFactory::Get("Vibration
   Output")` (`SoundSubmix.cpp:888-890`). No factory of that name is registered on Windows, so
   `Get` logs *"No endpoint implementation for %s found for this platform. Endpoint Submixes set
   to this type will not do anything."* and **returns the DUMMY factory** (`IAudioEndpoint.cpp:
   172-174`, `GetDummyFactory`).
2. `FMixerSubmix::SetupEndpoint` therefore creates a dummy `IAudioEndpoint`, so
   `EndpointData.NonSoundfieldEndpoint` is valid and `IsExternalEndpointSubmix()` is true
   (`AudioMixerSubmix.cpp:898`) — the submix lands in `ExternalEndpointSubmixes`
   (`AudioMixerDevice.cpp:1603`).
3. Every render callback runs `Submix->ProcessAudioAndSendToEndpoint()` on it
   (`AudioMixerDevice.cpp:750-757`), whose first statement is
   `if (IsDummyEndpointSubmix()) { zero the buffer; return; }` — **before `ProcessAudio`, so
   before the child loop.** `Submix_vibrationMaster` and `Submix_vibration` are never processed,
   their buffer listeners are never invoked, and sources routed to them are never pulled.

That is the whole explanation for `total=0`: not a broken tap, not a silent submix, a subtree the
mixer skips by design on a platform without the endpoint. The same applies to the speaker tree
(`"Pad Speaker Output"`).

#### The Blueprint gate — HARD, measured with the probe's command file

`BP_HKPlayerController_C` carries a `DebugPS5Haptic` bool (object dump, offset `0x778`) next to
the platform check in every `StartPS5Vibration` Blueprint. Calling the Blueprint from Lua:

| `DebugPS5Haptic` | `StartPS5Vibration(Scratch_VIBE)` | `ControllerVibration.IsPlaying()` | `AudioComponent:SetSound/Play` hooks |
|---|---|---|---|
| false | entered (our hooks fire) | false | 0 |
| **true** | entered | **true, Sound = Scratch_VIBE** | **SetSound=2, Play=2** |

So the gate is `(GetPlatform() == PS5) || DebugPS5Haptic`, and with the bool set the shipped
Blueprint plays the `_VIBE` asset on the `ControllerVibration` AudioComponent, whose base submix
is `Submix_vibration` through the sound class. `GetPlatform` is called ~700 times a minute by the
UI and returns 0 (Windows); it was NOT overridden.

**And the engine renders that source the moment it has a rendered submix to reach.** With the
component playing, `ControllerVibration:SetSubmixSend(Submix_menuSliders_sfx, 1.0)` drove the
MASTER probe's peak from ~0.03 to **1.00** (the master limiter) — the haptic waveform reached the
speakers — while the tap on `Submix_vibrationMaster` still read zero. The source, the mixer and
the tap all work; only the endpoint root is dead.

#### The way round it, built and UNCONFIRMED (never run): `SubmixReroute=1` + `ForcePS5HapticPath=1`

Re-parent `Submix_vibrationMaster` under a submix the mixer DOES process every callback
(`Submix_unused`: ParentSubmix null → the master tree, OutputVolume 1.0, no children, no
effects — measured), after writing that parent's `OutputVolume = 0` so nothing leaks into the
speakers; then call `FAudioDevice::RegisterSoundSubmix(parent, true)` and `(master, true)` —
the virtual at **slot 14**, two below the listener slot that is validated live, counted from the
same `AudioDevice.h` table (`:854`). `RegisterSoundSubmix` → `RebuildSubmixLinks` reads the
UObject's `ParentSubmix` and re-links the live instances (`AudioMixerDevice.cpp:1034-1060`);
`Init(bAllowReInit)` re-reads `OutputVolume` (`InitInternal`). Listeners run after a submix's own
output volume and before its parent's, so the tap on `Submix_vibrationMaster` (volume 1.0) sees
the full mix and `Submix_unused` (volume 0) kills it before the speakers. The attenuation's own
send to the dead root is simply lost; the class-default routing to `Submix_vibration` is what
carries the sound. `ForcePS5HapticPath` writes `DebugPS5Haptic=true` from the plugin's own Start
pre-hooks, before the Blueprint's gate runs.

**What a run must show, in order:** `submix: REROUTE submitted` → the `SUBMIX` line turning from
`NO CALLBACKS` into a steady `cb=... (46.9/s)` with `peak=0` (the subtree renders) → `peak > 0`
during a purr/scratch (the engine mixes haptics) → `HANDOVER: the SUBMIX now drives the coils`
in `submix-fallback`/`submix` → the user feeling it. Nothing past the first arrow has been seen.

**Risks stated:** slot 14 is derived, not measured (the call refuses if the slot shares its
address with a neighbour, the ICF signature of an empty base stub); `Submix_unused` is assumed
unused on the strength of its name; `DebugPS5Haptic` may open more than the haptic path (the
speaker Blueprints share the shape — harmless, their endpoint is dead too).

#### Glyphs — HARD shape, UNCONFIRMED effect

`/Script/Hk_project.InputSubsystem:GetGameControllerType(_forceGamepad: bool @0) ->
EGameControllerType @1` (1 byte; `0 Unknown, 1 XBOX, 2 PS4, 3 PS5, 4 SwitchPro,
5 KeyboardMouse`), called by `UMG_KeyIcon_C:Set Key`. The plugin's POST hook writes 3 into
`RESULT_DECL` and the frame copy (`Glyphs=ps5`, default) and logs the observed call shape once.
Whether the prompts actually change on screen has not been seen on this build.

#### MEASURED 2026-09-03, build 0.3.0 (`341139e`): the reroute renders, the engine mixes, the coils are on the mix

Two launches, both driven from the shell through `mods/StrayAudioProbe`'s command file (the
plugin's own Start pre-hook opened the gate before the Lua's did: `DebugPS5Haptic(before)=true`).

**Run A — `measure` + `SubmixReroute=1` + `ForcePS5HapticPath=1`:**

```
submix: REROUTE UObject writes: 'Submix_unused'.OutputVolume 1.000 -> 0.0, 'Submix_vibrationMaster'.ParentSubmix VibrationEndpointSubmix -> Submix_unused
submix: REROUTE - about to call vtable slot 14 ... (parent, bInit=true) then (master, bInit=true)
submix: REROUTE submitted.
COILS: driven by the ASSET path | SUBMIX measure bound=1 live=0 cb=1728 (47.0/s) ch=8 rate=48000 frames/cb=1024 peak=0.00000   <- the subtree RENDERS
  ...vib Scratch_VIBE...
COILS: driven by the ASSET path | SUBMIX measure ... cb=2056 (45.9/s) peak=0.70795 rms=0.29357 | master-probe peak=0.00774      <- the engine MIXES, no speaker leak
  ...stop...
COILS: driven by the ASSET path | SUBMIX measure ... peak=0.00000
```

Slot 14 is therefore **HARD** for this build (the game did not die, the links were rebuilt), and
so is the whole mechanism: a submix the mixer skipped for a whole session renders at 47 callbacks
a second the moment its parent is a rendered one, and the `_VIBE` source played by the shipped
Blueprint lands in it at peak 0.708 while the master probe stays at ~0.007 (`Submix_unused` at
volume 0 holds). `ch=8`: the mixer runs at the device's 7.1.

**Run B — strict `submix` + reroute + gate:**

```
haptics: 'Scratch_VIBE' NOT played on the asset path: COILS: NOBODY - the pad is SILENT by configuration ...
submix: FIRST REAL SIGNAL (peak 0.70795) - HANDOVER: the SUBMIX now drives the coils.
submix sink: 'Speakers (DualSense Wireless Controller)' 4ch 48000 Hz buf=48000, queue-ahead 1920 frames
COILS: driven by the SUBMIX | SUBMIX submix bound=1 live=1 cb=2191 (46.0/s) ... peak=0.70795 rms=0.29317 | rerouted=1 | ring fill=14143/16384 drop=1940480 under=0 | sink open=1 'Speakers (DualSense Wireless Controller)' 4ch 48000Hz frames=288960 fail=0
STATUS coils=SUBMIX (HapticSource=submix: the engine's own mix, no asset path) ...
```

The chain is complete up to the pad's endpoint: no asset played, the engine's signal caused the
handover, the sink streamed the mix to RL/RR. **What is NOT measured: how it feels.** Nobody was
holding the pad. The user's test is the strict configuration
(`deploy-submix-spike.sh --strict --reroute --gate`): whatever is felt came from the engine's mix,
and a purr on top of rain should now be a SUM rather than a supersession.

**Defect found by the numbers and fixed in the next build:** before the handover the ring was
full of silence (`16384/16384 drop=1843200`) because the tap wrote from its first callback and
nothing drained it, and after the handover it stayed near full — ~300 ms of latency. The ring is
now attached, empty, at the handover, so the sink's 40 ms queue-ahead is the whole budget.

**Glyphs:** `GetGameControllerType observed: RESULT_DECL=... (value 1) ... ReturnValue@1 size 1
(value 37) _forceGamepad=true`. RESULT_DECL carries the real answer (1 = XBOX); the frame copy is
garbage (37), so the post-hook's RESULT_DECL write is the one that matters. Three calls were forced
to 3 during the menu; the pause menu opened by KEYBOARD showed ENTER/ESC prompts, which is the
gamepad-only rule working (KeyboardMouse is left alone). **Whether the prompts show PlayStation
glyphs when the pad is the active device is UNCONFIRMED** — the game reads the DualSense over
hidraw, so a pad press cannot be injected from the shell; the user has to look.

**Ring fix measured (`da014c5`, strict run):** `ring fill=0/16384 drop=0` before the handover;
after it the fill sits at 864–1344 frames (18–28 ms) with `drop=0` and a single 1153-frame
startup underrun that never grows. With the sink's 40 ms queue-ahead that is the whole path
latency between the engine's mix and the coils. **This is the build left on the box**, in
`HapticSource=submix` + `SubmixReroute=1` + `ForcePS5HapticPath=1` + `Glyphs=ps5`
(`deploy-submix-spike.sh --strict --reroute --gate`).

---

## 15. Fourth live run, 2026-09-03: `peak=0.00000` does NOT mean the submix is empty

> **SETTLED 2026-09-03 by a later capture, and both open questions closed the same way.** The
> instrument fixes below shipped, and with them the purr reproduces from a **single `Q`
> press**:
>
> ```
> 04:00:01.378  StartPS5Vibration 'CatPurr2_VIBE' level=1.000 fadeIn=1.00 loop=1(asset)
> 04:00:01.426  StartPS5ControllerSound 'cat_purr_loop_01_CONTROL' level=1.000 fadeIn=1.00
> 04:00:02.098  submix: FIRST REAL SIGNAL (peak 0.25548) - HANDOVER: the SUBMIX drives the coils
> 04:01:04.222  COILS: driven by the SUBMIX | cb=17024 (46.9/s) peak=0.70795 rms=0.15550 bad=0
> ```
>
> * **The handover was EARNED, not tripped on a noise floor.** `peak 0.25548` at the handover
>   against the `1e-4` threshold, then sustained `0.70795` — the same figure §14 run A measured
>   for a real VIBE asset. The "a handover at 0.0001 and a handover at 0.7 are completely
>   different worlds" worry resolves to the second world. `SubmixLiveThreshold` does not need
>   raising.
> * **The `FindFirstOf` defect was real and its fix holds** — `hap[... padVibe=1]` with the
>   gate open and the vibration felt. It remains an inference that it was the *cause* of the
>   loading-screen symptom; what is now HARD is that the corrected build works.
> * **The user FEELS the purr and HEARS nothing from the pad speaker.** That asymmetry is the
>   subject of §16, and its cause is not in this section: the tap, the sink and the asset
>   player were all doing their jobs.

The user's report on the `da014c5` build: *"The scratch vibration is wrong. The vibration for
the purr is right but it doesn't have sound. There is a loading screen that has a purr that
should be purring in the controller, but it isn't."* The pasted evidence, strict `submix` +
reroute + gate:

```
COILS: driven by the SUBMIX | SUBMIX submix bound=1 live=1 cb=22067 (47.0/s) ch=8 rate=48000
  frames/cb=1024 peak=0.00000 rms=0.00000 bad=0 | master-probe cb=22073 peak=0.01482
  | rerouted=1 | ring fill=512/16384 drop=0 under=9889
  | sink open=1 'Speakers (DualSense Wireless Controller)' 4ch 48000Hz frames=21202080 fail=0
hap[starts=5 played=0 done=0 missing=0 fail=0 endpoint=0 now='' compStops ok=1 ignored=684 padVibe=1]
```

### The line does not say what it was read to say — and that is the first finding

**`peak` is the LAST STATUS WINDOW ONLY.** `LevelMeter::Take()` reads *and resets*
(`SubmixDsp.hpp`), and `SubmixStatus` runs once a second. So `peak=0.00000` means "nothing was
in the submix during that one second", which is also exactly what a perfectly working tap
prints whenever nothing is playing. It cannot distinguish a dead submix from a quiet second,
and the quoted line was captured with no vibration in flight — the nearest `StartPS5Vibration`
in the same excerpt is `Scratch_VIBE` at `03:13:32`, while `cb=22067` at 47/s puts the tap
about 470 s into its life.

**Three fields in the SAME line contradict "the submix delivers nothing":**

| field | what sets it | what it proves |
|---|---|---|
| `live=1` | `Runtime::StartSinkAtHandover`, and NOTHING else | a status window's peak reached the live threshold at least once this session |
| `sink open=1` | opened only inside that handover | the handover really ran |
| `frames=21202080` | 441 s of streaming at 48 kHz | it ran ~29 s after the tap bound, not at the end |

So the tap has carried *something*. **What it has not carried is a number anyone has seen**:
the threshold is `1e-4` (−80 dBFS) and the handover's own line prints the peak that tripped it,
which nobody has read. A real VIBE asset measures ~0.708 (§14, run A). A handover at 0.0001 and
a handover at 0.7 are completely different worlds, and in strict mode both disable the asset
path for the rest of the session.

**Verdict: the cause of `peak=0` is NOT established, and the pasted evidence cannot establish
it.** What is established is that the instrument was too weak to be read that way. Everything
below is either a fix for that, a defect found while looking, or a capture that would settle it.

### The instrument, fixed (0.3.1)

* **`submix watch`.** Every `StartPS5Vibration` opens a correlation window
  (`SubmixWatchSeconds`, default 3 s); every status window's peak is folded in; when it closes,
  ONE line names the asset and says what the engine put in the submix *for that asset*:

  ```
  submix watch 'Scratch_VIBE': the engine MIXED it - peak 0.70795 over 3.0s (3 window(s))
  submix watch 'Scratch_VIBE': the engine mixed NOTHING - peak 0.00000 over 3.0s (3 window(s))
  ```

  The negative is a WARN and names the three things upstream of the tap that can cause it. The
  logic is pure and tested (`src/SubmixWatch.{hpp,cpp}`, `tests/test_submix_watch.cpp`),
  including that a NaN reading can never fabricate a positive verdict and that a start during
  an open watch reports the one it replaces rather than swallowing it.
* **`peakEver=` and `lastSignal=`** on every SUBMIX line, so a single pasted line is
  self-diagnosing: `peakEver=0.00000` is a submix that has never carried anything;
  `peakEver=0.70795 lastSignal=142.3s ago` is a working submix in a quiet moment.
* **`gate[open=N writes=N misses=N]`** on the STATUS line — see below.
* **`SubmixLiveThreshold`** is now a setting, default unchanged at `1e-4`, with the cost of
  that default written next to it rather than buried.

### DEFECT, found in RE-UE4SS's own source: `FindFirstOf` returns the LAST derived match

**HARD**, read in `deps/first/Unreal/src/UObjectGlobals.cpp:354-383` at the mirror SHA CI pins:

```cpp
UObjectGlobals::ForEachUObject([&](UObject* Object, ...) {
    UClass* Class = Object->GetClassPrivate();
    if (Class->GetNamePrivate().Equals(ClassName) && IsValidObjectForFindXOf(Object)) {
        ObjectFound = Object;
        return LoopAction::Break;          // EXACT class-name match: stops here
    }
    if (!IsValidObjectForFindXOf(Object)) { return LoopAction::Continue; }
    for (UStruct* super_struct : TSuperStructRange(Class))
        if (super_struct->GetNamePrivate().Equals(ClassName)) {
            ObjectFound = Object;
            break;                          // INNER break only
        }
    return LoopAction::Continue;            // ... and the scan carries on
});
```

The live controller is `BP_HKPlayerController_C`, a **subclass**, so it takes the second branch —
which never breaks the outer loop. `FindFirstOf("HKPlayerController")` therefore returns the
**LAST** matching object in `GUObjectArray`, after walking every object in the process with a
superstruct walk per object. Two consequences:

* **Correctness.** `ForcePS5HapticPathOnGameThread` opened the `DebugPS5Haptic` gate on
  whichever controller happened to sort last, not on the one the Blueprint was executing on.
  With a stale controller still alive across a level load — which is exactly the shape of "the
  loading-screen purr is not felt" — the gate is set on the wrong object and the live
  Blueprint returns before it plays anything.
* **Cost.** The comment claimed the per-instance cache kept the search off the 60 Hz path; the
  cache was consulted *after* the search had already run, so the full scan happened on every
  `StartPS5Vibration` / `StopPS5Vibration` / `StartPS5ControllerSound`.

**Fixed by using the hook's own `context.Context`** — `UObject* Context` in
`UFunctionStructs.hpp`, the object the UFunction is executing on, i.e. the controller itself.
Free, and right by construction. `FindFirstOf` survives only as a fallback for a null context
and logs loudly when it is used. `CDO`s were never a risk here: `IsValidObjectForFindXOf`
excludes `RF_ClassDefaultObject | RF_ArchetypeObject`.

**This is a real defect and a plausible cause of the loading-screen symptom. It is NOT proof of
the whole report** — nothing in the pasted log shows the gate's state, which is why
`gate[open=…]` now exists.

### `Scratch_VIBE` starting at `Level=0.000` is CORRECT, and lifting it would be a bug

`StartPS5Vibration 'Scratch_VIBE' level=0.000(seen=1)` on the plain (non-component) path. The
code lifts a zero level to 1.0 only for component-attached starts. **Keep it that way:**

* **The game genuinely starts the scratch at zero and drives it afterwards.** §9, measured:
  `SetPS5VibrationLevel(Level)` runs at ~60 Hz and reads `0.0` idle, `~0.47-0.52` while
  scratching. A start level of 0 is the *beginning of that ramp*, not a missing argument —
  `seen=1` says the parameter was read, and its value was zero.
* **The asset path already honours the ramp.** `AudioPlayer::PlayOne` re-reads `m_level` for
  every WASAPI buffer (`AudioPlayer.cpp`), so `SetLevel` takes effect on a playing asset.
  Lifting the start to 1.0 would make the scratch begin at full amplitude and then be pulled
  down by the first `SetPS5VibrationLevel` — an audible attack the game did not author.
* **The component path's lift has a different justification and it does not transfer.** Those
  starts carry their level in the submix send (`PS5VibrationAttenuation`, `bAttenuate=False`,
  constant 1.0), so a 0 there means "unset", not "silent".
* **On the submix path our level is not used at all.** The engine applies the level upstream of
  the tap, which is why `OnSetVibrationLevel` returns early when the submix owns the coils. So
  this question cannot be the cause of a silent submix either way.

The one thing that *is* worth knowing: if the game starts the scratch at 0 and the submix
carries it at ~0.5 while the asset path used to play it at 1.0, **the scratch will feel weaker
than it did on the asset path**, and "the scratch vibration is wrong" is consistent with that.
The new `submix watch` line reports the peak, so one run says which.

### The capture that settles it — run on the build ALREADY on the box, no rebuild needed

Against `<gamedir>/stray-dualsense.log` (`Hk_project/Binaries/Win64/`):

```sh
L=stray-dualsense.log

# 1. THE DECIDER: what peak tripped the handover?
grep -n "FIRST REAL SIGNAL" "$L"

# 2. Has the tap EVER carried a real signal? (strip the master-probe field first: it also
#    contains "peak=", and it is the game's whole soundtrack, not the vibration submix)
sed 's/ | master-probe.*//' "$L" | grep -o 'peak=[0-9.]*' | sort -t= -k2 -g | tail -3

# 3. Is the Blueprint gate open?
grep -n "ForcePS5HapticPath" "$L" | head -5

# 4. Did the game ever ask for the SPEAKER, and did it play?
grep -n "StartPS5ControllerSound" "$L" | head -10
grep -o "spk\[[^]]*\]" "$L" | tail -3

# 5. The submix around the scratch: the 8 lines before and after each start
grep -nE "SUBMIX|StartPS5Vibration|FIRST REAL SIGNAL" "$L" | grep -A8 -B2 "Scratch_VIBE" | head -60
```

**How to read it:**

| result | meaning |
|---|---|
| (1) `peak 0.7…` | the submix carried a real haptic; the handover was earned |
| (1) `peak 0.0001…` | the handover fired on the noise floor and strict mode then silenced the pad for the session — raise `SubmixLiveThreshold` |
| (2) max is `0.00000` | the submix has NEVER carried anything: the fault is upstream of the tap |
| (2) max is `~0.7` | the submix works and the pasted line was a quiet second |
| (3) `DebugPS5Haptic 0 -> 1` | the gate opened; 0.3.1's `context.Context` fix is not the cause |
| (3) `no bool property` / `Context` fallback / nothing at all | the gate never opened — the Blueprint returned before it played anything, and THAT is the whole report |
| (4) starts > 0, `spk[... endpoint=0 fail>0]` | the speaker path is failing to open the pad endpoint — a separate bug from the coils |
| (4) no `StartPS5ControllerSound` at all | the game never asked for the purr's speaker sound, so the gate or the platform check is what stops it |

The 0.3.1 build answers (1), (2), (3) and (5) from a **single** `submix watch` line per start
plus `peakEver=` on any status line, which is the point of the change.

### What is UNVERIFIED in all of the above

Everything, on the target: the plugin has not been built by CI at the time of writing and has
never been run. The `FindFirstOf` finding is HARD (read in the source CI pins) and the
`peak`-is-one-window finding is HARD (read in this repo's own code), but **"this is why the
user's pad felt wrong" is an inference from both, not a measurement.**

---

## 16. The pad speaker is a ROUTING choice, and we stopped making it — 2026-09-03

### The symptom, and why it was so confusing

One `Q` press fires both halves of the purr. The user **feels** it and **hears nothing** from
the pad. Every counter we own says the speaker path worked:

```
spk[starts=1 played=1 missing=0 fail=0 endpoint=1 now='cat_purr_loop_01_CONTROL']
sink open=1 'Speakers (DualSense Wireless Controller)' 4ch 48000Hz frames=2982240 fail=0
```

The asset was found (`spk/cat_purr_loop_01_CONTROL.f32`, 3,721,460 bytes), the pad's WASAPI
endpoint opened, samples went in, zero failures. **We were writing correct audio into the pad
and the pad was routing it nowhere.**

### The asymmetry has a structural cause

**THE COILS ARE A WAVEFORM PATH THAT NEEDS NOTHING CLAIMED; THE SPEAKER IS A ROUTING CHOICE
THAT MUST BE CLAIMED EXPLICITLY.** §12's whole finding is that the coils take audio as soon as
*nothing re-asserts* `COMPATIBLE_VIBRATION` — an absence is sufficient. The speaker is the
opposite: the pad's default output path sends audio to the headphone sinks and **mutes the
internal speaker**, and no absence ever changes that. Something has to select it.

### THE REGRESSION — HARD, from the repo's own history

`tools/dualsense/libScePad_shim.c:476-502` (`audio_probe`, committed in `bf87d91`) called
Sony's own audio API, and the pad speaker worked:

```c
supp = resolve("scePadIsSupportedAudioFunction");   supp(h, 0, 0, 0)
/* scePadSetAudioOutPath(handle, path) takes the path BY VALUE; 3 = SPEAKER
   0 STEREO_HEADSET 1 MONO_HEADSET 2 MONO_HEADSET_SPEAKER 3 SPEAKER 4 OFF */
path = resolve("scePadSetAudioOutPath");            path(h, 3, 0, 0)
/* s_ScePadVolumeGain { uint8 SpeakerVol, JackVol, Reserved, MicGain } */
unsigned char g[8] = { 80, 80, 0, 0, 0, 0, 0, 0 };
gain = resolve("scePadSetVolumeGain");              gain(h, &g, 0, 0)
```

`tools/dualsense/deploy-submix-spike.sh` retires the shim (`libScePad.dll` ←
`libScePad_orig.dll`) — **correctly**, because two writers of `valid_flag0` is the §12 trap —
and **nothing has called those functions since**. The pad fell back to its default routing.
That is the whole of "it worked as-is and it is not working right now".

Note the shim also **crashed the game** calling `scePadGetContainerIdInformation` on a guessed
struct, and its own comment says we do not need it. Do not call it.

### The fix needs NO proxy DLL — `scePadGetHandle` is the reason

The shim obtained its handle by intercepting `scePadOpen`, which is why it had to be a proxy.
A UE4SS plugin cannot intercept `scePadOpen` — but it does not need to. **`ScePad::SelectPad`
already holds a live handle from `scePadGetHandle` (§11, "verified to return the same
handles"), confirmed live tonight as `handle=0x101`.** So the whole mechanism is four
resolutions against a module the game has already mapped:

```
GetModuleHandleW(L"libScePad.dll")     // already in the process; never LoadLibrary a copy
GetProcAddress: scePadGetHandle  scePadIsSupportedAudioFunction
                scePadSetAudioOutPath  scePadSetVolumeGain
```

Full `scePad*` export list of the shipped `libScePad.dll` (md5
`7a492fe29202487f3c94fe094f135c48`), read 2026-09-03 — all four are present:

```
scePadClose  scePadGetContainerIdInformation  scePadGetControllerInformation
scePadGetControllerType  scePadGetHandle  scePadGetJackState  scePadGetParticularMode
scePadGetTriggerEffectState  scePadInit  scePadIsSupportedAudioFunction  scePadOpen
scePadRead  scePadReadState  scePadResetLightBar  scePadResetOrientation
scePadSetAngularVelocityDeadbandState  scePadSetAudioOutPath  scePadSetLightBar
scePadSetMotionSensorState  scePadSetParticularMode  scePadSetTiltCorrectionState
scePadSetTriggerEffect  scePadSetVibration  scePadSetVibrationMode  scePadSetVolumeGain
```

**Why plugin-level beats a proxy, and it is not merely simpler:**

* **Sony's dll stays the SINGLE writer of pad output reports.** The §12 two-writers hazard
  never arises, because we are asking it to change a setting rather than writing bytes beside
  it.
* **No export forwarding.** A proxy that drops or mistypes one of 25 exports breaks the pad
  entirely; that whole failure class disappears.
* **No deploy surgery.** No `_orig`, no `_shim`, nothing renamed — which is the drop-in goal.

### Implemented, and NOT YET RUN

> **SUPERSEDED 2026-09-03.** Sony's call was confirmed on the pad
> (`scePadSetAudioOutPath(3=SPEAKER)=0x00000000`, `SONY ACCEPTED the route`, `db83a32`), and
> 0.4.0 deleted `PadSpeakerRoute`, the `hid`/`both`/`auto` escalation and the raw HID audio
> claim (§18). What follows is the record of the design that was built; only the `sony` arm
> of it exists now, unconditionally, with `PadSpeakerPath` / `PadSpeakerGain` kept.

`[StrayDualSense] PadSpeakerRoute` = `sony` | `hid` | `both` | `auto` | `off`, default
**`auto`**. `PadSpeakerPath = 3` (Sony's enum) and `PadSpeakerGain = 80` are the shim's
measured values, configurable rather than baked. `ScePad::ApplyAudioRoute` runs on the pad
thread — the only thread holding a libScePad handle — when a pad is adopted, when the handle
changes, and when the settings change (they are hot-reloadable on purpose, so `sony` versus
`hid` is one ini edit and one keypress rather than one relaunch per arm).

Every result code is logged in hex exactly as the shim logged them, and `0x80920007` is named
in the log as the §7 device-tree refusal rather than left as a number.

### The HID fallback, and the bit table that was wrong

`PadSpeakerRoute = hid` writes the routing into the USB output report ourselves. It exists
because §7 measured `scePadSetAudioOutPath` returning `0x80920007` ("this pad has no audio")
before the GE-Proton11-6 upgrade — libScePad decides from the device tree, and Wine exposed no
USB audio sibling interfaces. If that ever comes back, the fallback needs no rebuild.

Building it required correcting §12's bit table against `drivers/hid/hid-playstation.c`; see
the correction note there. The routing table (kernel's own comment, `:1366-1377`):

| path | headphone L | headphone R | internal speaker |
|---|---|---|---|
| 0 | L | R | **MUTED** — the pad's default, and our bug |
| 1 | L | L | MUTED |
| 2 | L | L | **R** |
| 3 | muted | muted | **R** |

**The internal speaker is fed from the RIGHT channel in every path.** `kSpeakerRoute` writes
the mono `_CONTROL` asset into both FL and FR, so the signal is already on R — but
"optimising" that route to FL alone would silence the pad speaker completely.

**Sony's enum and the kernel's are DIFFERENT and must not be mixed.** Sony's 3 = SPEAKER; the
kernel's 3 = "headphones muted, speaker gets R". `PadSpeakerPath` is Sony's;
`PadSpeakerHidPath` is the kernel's.

### Why the coils cannot be affected

* **`sony` never writes a HID byte.** It calls two Sony functions that set audio routing and
  audio levels. `HidMode`'s report — the one thing that decides the coils' mode — is untouched.
* **The HID claim contributes bits 5 and 7 only**, and `ComposeValidFlag0` masks bits 0..3 out
  of it unconditionally, so the coil-mode base (`HapticValidFlag0`, the §12 measurement) is the
  only source of those bits. It is folded into the report `HidMode` already writes, so there is
  no second writer. `tests/test_pad_audio.cpp` pins this over every path/volume/preamp and
  every coil base, and over a deliberately hostile `flag0 = 0xFF`.
* **`auto` never writes the claim speculatively** — only after Sony's API has been tried and
  refused. In the world where the speaker works, the coils never see it at all.

### The platform gate is NOT what silenced the speaker — the game DID ask for the sound

`tools/dualsense/StrayTriggers.lua:279` hooks `/Script/Hk_project.HKUtilities:GetPlatform` and
rewrites the return to `PS5 = 2`, behind a `stray_platform.on` flag file. The plugin has no
equivalent (`grep -rn "GetPlatform\|EHKPlatform\|HKUtilities" mods/StrayDualSense/src/` is
empty) and gates on `DebugPS5Haptic` instead. It is a real lever and a more honest emulation
than flipping individual debug booleans. **It is not, however, this bug.**

**The decisive evidence is in the capture that opened this section**, and it is one line:

```
04:00:01.426  StartPS5ControllerSound 'cat_purr_loop_01_CONTROL' level=1.000 fadeIn=1.00
```

**The game asked for the pad-speaker sound.** The speaker Blueprints share the haptic
Blueprints' gate shape (§14), `DebugPS5Haptic` had already opened it, and `spk[starts=1
played=1 missing=0 fail=0 endpoint=1]` says we then found the asset and played it into an open
endpoint. Nothing upstream of the routing was blocked. A platform override cannot fix a step
that already ran.

Two further reasons not to build it as part of this change:

* **It was tried and it barely reached** (§7 caveat): the override "fires, but only ~6 times, so
  the value is evidently cached early". Meanwhile `GetPlatform` is called **~700 times a minute
  by the UI** and returned 0 throughout the 2026-09-03 runs (§14) — in which the shipped
  Blueprint nonetheless played the `_VIBE` asset, because `DebugPS5Haptic` alone was enough.
* **Claiming PS5 opens every platform-gated path at once** — input handling, UI, save behaviour,
  trophies — some of which may expect a PS5 API that is not there. The Lua kept it behind a flag
  file precisely because it was a **diagnostic, never a shipped default**.

**Recommendation:** worth having later as a config key defaulting OFF, as a broader alternative
to `ForcePS5HapticPath`; not worth adding on the strength of a symptom the evidence already
attributes elsewhere. `GetPlatform` is a post-hook return-value rewrite, the identical shape to
the `GetGameControllerType` glyph hook the plugin already has, so it is cheap when it is wanted.

### Environmental prerequisites, so a second machine does not mystify anyone

The pad's Windows audio endpoint exists at all only because of **`PROTON_SONY_WINDOWS_DEVICE_NAMES=1`**
and **`PROTON_KEEP_SONY_AUDIO_ENDPOINT_VISIBLE=1`**, both already set by GE-Proton (§10). If
either is missing, the endpoint disappears and nothing in §10 or this section can work — the
symptom would be `endpoint=0` in `spk[]`, not a routing problem.

Also confirmed: **the Lua never calls any `scePad*` function.** It hooks UFunctions and wrote
command files the shim consumed; all device work was the shim's. So there is no third source of
pad calls to reconcile — only the plugin and Sony's dll.

### `HidMode` is NOT redundant, and `scePadSetVibrationMode` does not replace it

Recorded because the opposite was proposed and is wrong. **The game already calls
`scePadSetVibrationMode` itself** — the shim only *forwarded* it
(`libScePad_shim.c:542-555`), with a `vibmode` debug command that could override the mode
(`:453-459`, default `-1` = no override). That override existed as an **A/B knob because nobody
knew whether the game's chosen mode was right**, and that question is still open.

So the likely reading is the reverse of "Sony's API already does this": `valid_flag0 = 0x00`
exists because the game's chosen vibration mode does **not** leave the coils accepting
waveforms, and we are **countermanding** the game rather than duplicating Sony. Leave `HidMode`
alone without evidence; "can Sony's API replace it" is a separate question for a later session.

Corroboration from the same function: it captured the pad handle as `g_lastPadHandle = a`, a
second source besides `scePadOpen`. Not needed now that `scePadGetHandle` is confirmed present,
but it does establish that the game calls this with a live handle.

**What this change does to `HidMode`: nothing, in the default path.** With no claim set
(`PadSpeakerRoute = sony` succeeding, or `off`), `ComposeValidFlag0(base, {})` returns `base`,
`valid_flag1` stays 0, the length floor is unchanged and no extra byte is written — the report
is **byte-identical** to the one §12 measured working.

### `kSpeakerBoost` must NOT be carried across to the live tap

Flagged now so step 2 does not inherit it silently. `AudioPlayer.hpp` bakes in the game's
`SBFX_Boost` as `kSpeakerBoost = 1.7783` (+5 dB, §10) because the asset path replays a
`SoundWave` from disk and therefore bypasses `Submix_controllerPre`, where that trim lives.
**On a rerouted-submix path the engine applies its own submix chain, so the +5 dB is already in
the signal and applying it again would double it.** The shim had a `spkboost` runtime A/B
(`libScePad_shim.c:450-452`) precisely because the value was being questioned; keep it
configurable, and re-derive it for the tap rather than porting the constant.

### The target shape, and how much of this stays bespoke

The end state the user has set: **a drop-in DLL, no extracted assets, ever.** The `spk/*.f32`
and `haptic/*.f32` files, `AudioPlayer`'s replay path,
`tools/dualsense/ue4_soundwave_extract.py` and the `SpkDir`/`HapticDir` settings are all
**condemned** — they were a workaround for a submix that would not render, and §14's reroute is
the actual fix. Extracted game audio in a mod directory is also redistribution of Stray's
assets, which `CLAUDE.md` already forbids.

**The speaker submix is dead for the IDENTICAL reason as the vibration one** (§14):
`ControllerEndpointSubmix` has `EndpointType = "Pad Speaker Output"`, hits the same
`IAudioEndpointFactory::Get` dummy factory, and `ProcessAudioAndSendToEndpoint` returns before
touching a child. So the same reroute should work on it. Order, and do not collapse it:

1. **Prove the speaker ROUTES.** Orthogonal to where the samples come from — this section.
   **DONE, user-confirmed on the pad.**
2. **Reroute the speaker submix** and switch the source from replayed files to the live tap.
   **BUILT in 0.4.0 (§18), UNCONFIRMED.**
3. **Delete** the asset player, the extracted files, the extraction tooling and their settings.
   **DONE in 0.4.0.**

**One consequence to face rather than discover:** the asset path is currently also the fallback
when the submix tap fails to bind. Under the target shape there is no fallback — a failed tap
is a silent pad. That is the right trade (a silent pad that says why beats a pad that
pretends), but it makes the tap's refusal reporting **load-bearing**: `COILS:` and `spk[]` must
name the failure, not merely report zeros.

**How generic is this really?** Honestly: mostly, but not entirely.

* **Passive bridging**, and it is the bulk of it: tapping a submix the engine mixes and pushing
  it at the channel pair the hardware expects; asking Sony's dll to select a route.
* **Not passive**, and it cannot be: the **reroute mutates the engine's audio graph**
  (re-parenting a submix and calling `RegisterSoundSubmix`), the `DebugPS5Haptic` gate **writes
  a Blueprint property**, and the trigger path **translates a genuine enum permutation**
  between the game's `EPS5TriggerEffectMode` and Sony's (§13).

Those three exist because parts of the PS5 path are structurally *absent* on PC rather than
merely switched off — **you cannot bridge a stream that was never produced.** The dead endpoint
submixes have no factory, the Blueprint gate is false, and nothing ever sets the
`PS5TriggerEffect` device property. Making the engine produce the stream is the minimum
intervention, not an embellishment.

### UNVERIFIED — everything in this section

Per §0.3: **none of it has run.** The plugin has not been built by CI at the time of writing
and the routing has never been applied in the game. What is HARD is the shim's source (read in
this repo), the export list (read from the shipped DLL), the kernel's bit layout (read in
mainline) and the pure logic (tested in CI). **"This is why the pad speaker was silent" is an
inference from all four, not a measurement.**

### The one-run test

Deploy the build, press `Q` once at a checkpoint, then against
`<gamedir>/stray-dualsense.log`:

```sh
L=stray-dualsense.log
grep -n "pad audio:"        "$L"     # the decider
grep -o "padaudio\[[^]]*\]" "$L" | tail -3
grep -o "spk\[[^]]*\]"      "$L" | tail -3
grep -n "AUDIO CLAIM"       "$L" | head -3   # only if it escalated to hid
```

| what the log says | reading |
|---|---|
| `scePadSetAudioOutPath(3=SPEAKER)=0x00000000` + `SONY ACCEPTED` | claimed and, if the purr is audible, **done** |
| `SONY ACCEPTED` but still silent | routing is right and the fault is downstream — levels, the endpoint's FL/FR, or the two WASAPI clients on one endpoint |
| `=0x80920007` | libScePad refuses: "this pad has no audio" (§7, device tree). `auto` escalates to the HID claim on its own; read the `AUDIO CLAIM` line next |
| `=0x8092xxxx` (other) | asked wrong. `scePadIsSupportedAudioFunction`'s hex on the same line says whether the capability is there at all |
| `(absent)` on `scePadSetAudioOutPath` | this libScePad has no such export; set `PadSpeakerRoute=hid` |
| no `pad audio:` line at all | no pad was adopted — read `pad=NO` in STATUS, not this section |

**And in every case check that `COILS: driven by the SUBMIX` and the peaks are unchanged.** If
the coils regressed, set `PadSpeakerRoute=sony` (which never writes a HID byte) or `off`, and
report it — that would be the first evidence against the claim's safety argument.

---

## 17. The 03_Slums silence: the instrument lied, and the reroute was never re-applied — 2026-09-03

### What the log actually said, and what it was read to mean

Strict `submix` + reroute + gate, `03_Slums`, launched from Steam. Every asset the game asked
for:

```
submix watch 'generic_hit_01_VIBE': the engine mixed NOTHING - peak 0.00000 over 1.2s (0 window(s))
submix watch 'CatPurr2_VIBE':       the engine mixed NOTHING - peak 0.00000 over 4.7s
COILS: NOBODY - the pad is SILENT by configuration (HapticSource=submix, no fallback)
```

That was read as "the engine is not mixing haptics into the submix", which sent the diagnosis
upstream to the Blueprint gate. **The line does not say that, and `(0 window(s))` is the tell.**

`Runtime::SubmixStatus` only folded a window into the watch when `level.frames != 0`:

```cpp
if (level.frames != 0)          // <- the defect
    m_submixWatch.Sample(level.peak);
```

So a tap delivering **no audio at all** contributed nothing, `m_windows` stayed 0, `m_peak`
stayed 0, and `carried = m_peak >= threshold` came out false — printed as a claim about the
MIXER. **This is §15's own lesson recurring inside the instrument built to fix §15**: a reading
of zero that cannot distinguish "silence" from "no measurement".

**Fixed (0.3.2).** `WatchVerdict::result` is now a three-state `WatchResult`:

| result | means | licenses |
|---|---|---|
| `NoData` | the tap delivered 0 frames while the watch was open | **nothing about mixing.** Suspect the reroute or the binding |
| `Silent` | frames arrived and their peak stayed under the threshold | the real negative: the game asked and the submix carried silence |
| `Mixed` | the peak reached the threshold | the asset reaches the coils |

`Sample(peak, frames)` is now called on **every** window, frames included; a frameless window is
counted but cannot contribute a peak, so it can neither be mistaken for silence nor launder a
stale meter reading into a signal. Pure and pinned: `tests/test_submix_watch.cpp`.

### And the real bug: the reroute is submitted ONCE, so a level load ends the session

`SubmixWantsBinding()` returned false forever once `m_submixBound` was set, and **both halves of
the reroute are gated on it**:

* the glue's UObject writes — `Submix_unused.OutputVolume = 0` and
  `Submix_vibrationMaster.ParentSubmix = Submix_unused` (`Mod.cpp:739-805`, whose own comment
  says the writes are "idempotent, so it runs every attempt" — but the attempt never came again);
* our `FAudioDevice::RegisterSoundSubmix` calls, which tell the engine to re-link the live
  `FMixerSubmix` instances.

A level load rebuilds the submix graph. Nothing re-applied either half, so the subtree stopped
being rendered and stayed that way for the rest of the session — **exactly** the measured shape:
peak 0.708 in `BaseMap`, zero callbacks in `03_Slums`.

**Fixed with a watchdog, `SubmixRerouteWatchdogSeconds` (default 5.0).** When the tap is bound,
not refused, and its callbacks have not advanced for that long *after having advanced at least
once*, it re-arms: `SubmixWantsBinding()` goes true again, the glue re-writes the UObject links
and we re-submit `RegisterSoundSubmix`. **The listener is NOT re-registered** — UE 4.27's
`FMixerSubmix::RegisterBufferListener` appends without de-duplicating, so a second registration
would deliver every callback twice into one ring. Capped at 20 re-arms, then it says so loudly
rather than papering over a structural fault.

**This is not a fallback.** It never hands the coils back to the asset path. Strict mode stays
strict and a silent submix stays a silent pad; the watchdog only repairs the thing that made
strict mode unreliable.

### The `GetPlatform` question, evaluated — and the premise does not hold on this build

The proposal: replace `ForcePS5HapticPath` (which writes `DebugPS5Haptic` on a
`BP_HKPlayerController_C` **instance**) with a hook on
`/Script/Hk_project.HKUtilities:GetPlatform` (a **static** utility, so nothing to go stale).

**The argument's load-bearing claim is that a level transition leaves the instance write stale.
On this build it cannot.** `ForcePS5HapticPathOnGameThread(context.Context)` is called from the
PRE-hook of `StartPS5Vibration`, `StopPS5Vibration` and `StartPS5ControllerSound`
(`Mod.cpp:1004`, `:1025`, `:1050`), and `context.Context` is the UObject the UFunction is
executing on. So the bool is written **on the live controller, microseconds before the gate that
reads it, on every single call.** A new controller after a level load gets the write on its very
first `StartPS5Vibration`. There is no window in which the gate is unset.

That is not theory — it is the §15 `FindFirstOf` fix, which replaced exactly the stale-object
mechanism being described here.

**And this session's own log confirms the gate worked in `03_Slums`:** `spk[starts=1 played=1
missing=0 fail=0 endpoint=1]`, the first honoured `StopPS5VibrationOnAudioComponent` this
project has logged, and four `submix watch` lines — which only open on a vibration start. The
hooks fired, the gate opened, the Blueprints played. **The tap delivered no frames.** Different
layer entirely.

Answering the four questions directly:

1. **Does hooking `GetPlatform` open the vibration and speaker paths without `DebugPS5Haptic`?**
   **Structurally yes, UNVERIFIED live.** §14 established the gate as
   `(GetPlatform() == PS5) || DebugPS5Haptic`, so satisfying the first disjunct suffices. Only a
   run proves it.
2. **Per-decision or cached?** **Per-decision, ~700 calls/minute** (§14, measured). The
   Blueprints hold it in `CallFunc_GetPlatform_ReturnValue`, a per-execution local, so each
   execution re-calls the UFunction. **§7's "it fires, but only ~6 times, so the value is
   evidently cached early" is almost certainly an artifact of two things, neither of which is
   caching:** the Lua logs at `plat <= 3 or plat % 200 == 0`, so a reader sees ~3-6 lines
   regardless of the real count; and the Lua writes `ReturnValue:set(PS5)` — **the frame copy**,
   which §15 measured for `GetGameControllerType` as carrying *garbage* (37) while `RESULT_DECL`
   held the real answer. So the Lua override probably never took effect at all, and §7's
   conclusion should be treated as **retracted, not merely unproven**. The plugin already knows
   the correct target: its glyph post-hook writes `RESULT_DECL` *and* the frame copy.
3. **What else gates on platform?** **I cannot answer this from here and will not guess.** It
   needs the pak on the box: decompress, then
   `grep -c CallFunc_GetPlatform_ReturnValue` across cooked Blueprints and list the owning
   assets. What is already known: the vibration Blueprints, the speaker Blueprints (§10, and the
   Lua's own comment at `:246-248`), and `HKUtilities` being a general utility means UI and input
   are plausible consumers at ~700 calls/minute. **That volume is itself the risk**: whatever
   those callers are, they are not the haptic path.
4. **Recommendation: do NOT make it the default, and do not switch the primary mechanism.**
   Argued:
   * The stated reason to switch — instance staleness — **is not a real defect here** (point
     above). Swapping a mechanism that is right by construction for one that is broader and
     unverified trades a non-problem for an unmeasured risk.
   * `DebugPS5Haptic` is **narrow and measured**: §14's table shows it flipping
     `ControllerVibration.IsPlaying()` false→true with `SetSound=2, Play=2`. `GetPlatform` is
     **broad and unmeasured** — it claims PS5 to every caller, ~700 times a minute.
   * It is worth building as an **opt-in diagnostic** (`GetPlatform=ps5`, default off), because
     it would answer Q1 and Q3 in one run and is the same post-hook shape as the existing glyph
     hook. **I have deliberately not built it in this change**: the house rule is no speculative
     features, the live bug is elsewhere, and building it now would mix an untested broad
     override into the run that is supposed to test the watchdog.
   * If a future run shows the gate genuinely failing to open — `gate[open=0 …]` with vibration
     starts present — then it becomes the right answer, and the evidence will exist.

**The honest summary: `GetPlatform` is a better emulation of "we are a PS5" and a worse thing to
turn on blind. `DebugPS5Haptic` is a narrower lie that is measured to work and cannot go stale.**

### The capture that settles the next run

```sh
L=stray-dualsense.log
grep -nE "submix watch|NO DATA FROM THE TAP"   "$L" | tail -10
grep -n  "STOPPED RENDERING\|REROUTE RE-SUBMITTED\|REROUTE submitted" "$L"
grep -o  "SUBMIX [a-z-]* bound=[0-9] live=[0-9] cb=[0-9]* ([0-9.]*/s)" "$L" | tail -5
grep -o  "gate\[[^]]*\]" "$L" | tail -3
```

| what it says | reading |
|---|---|
| `submix watch … the engine MIXED it - peak 0.7…` | working; the coils should be felt |
| `NO DATA FROM THE TAP` then `STOPPED RENDERING` then `REROUTE RE-SUBMITTED` then `cb=` climbing | **the level-load hypothesis is confirmed and the watchdog fixed it** |
| `NO DATA` and `REROUTE RE-SUBMITTED` but `cb=` stays 0 | the reroute is not what fails; the tap or the device pointer is. Do NOT blame the gate |
| `the engine mixed NOTHING` with `frames` > 0 | the real negative at last — now the gate and the level the game passed are the suspects |
| `gate[open=0 …]` with vibration starts | the gate genuinely is not opening; **this** is when `GetPlatform` earns its place |

### UNVERIFIED

Everything here. The watch fix and the watchdog are CI-green and have not run in the game; "a
level load drops the reroute" remains the leading **hypothesis**, and the watchdog is
instrumented to confirm or kill it in one run rather than to assume it.

---

## 18. 0.4.0: the thin shim — the speaker through the same tap, and the deletions — 2026-09-03

Built from `9553b9e` on branch `dualsense-thin`, in three commits: (a) the speaker via the tap,
(b) the deletions, (c) this documentation. **Nothing in it has run on the box**, which was under
test with `9553b9e` at the time; CI is the only build.

### The shape, which is now a constraint

```
UNREAL SIDE (hooks)                          SONY SIDE (game's libScePad.dll)
  DebugPS5Haptic gate (pre-hook, per call)     scePadGetHandle
  reroute dead endpoint submixes:              scePadSetAudioOutPath(3)  + scePadSetVolumeGain
    Submix_vibrationMaster  -> Submix_unused   scePadSetTriggerEffect
    Submix_controllerMaster -> Submix_unused
  tap both (RegisterSubmixBufferListener)    TRANSPORT
  trigger hooks -> enum translation            ONE WASAPI client on the pad's 4-ch endpoint:
                                                 FL/FR <- speaker tap   RL/RR <- vibration tap
                                               HidMode valid_flag0 (§12, measured necessary)
```

The engine does the mixing, the fades, the levels, the looping and the boost chain. The plugin
moves samples from where the engine put them to where the pad reads them, and makes Sony's DLL
select the route. Every remaining source file is a hook, the reroute, the tap, the sink, a Sony
call, HidMode, triggers, config or log (`mods/StrayDualSense/README.md`, "Source layout").

### The speaker: the coil code generalised to two submixes

`Submix_controllerMaster` is dead on PC for the identical reason `Submix_vibrationMaster` was
(§16: `ControllerEndpointSubmix` is a `UEndpointSubmix` of type "Pad Speaker Output", no
factory on Windows, dummy endpoint, subtree skipped). So it gets the identical treatment, in
the same code: a `Lane` — tap, ring, live flag, per-second meters, its own three-state watch —
used twice; ONE sink pulling both rings into one 4-channel stream (`InterleaveLanes`, pure,
tested); ONE reroute submission that registers the parent and then EVERY master; ONE watchdog
(`StallWatchdog`, pure, tested over N lanes: never before a lane's first callback, any lane
that once rendered and then stalls, one re-arm at a time, capped). No second ring type, no
second WASAPI client, no second watchdog, no new abstraction beyond `Lane`.

The speaker's own `submix watch [speaker]` line opens on every `StartPS5ControllerSound`, and
its status line carries `SPEAKER:` beside `COILS:`, with the same three-state verdict
(`NoData` / `Silent` / `Mixed`) and the same "NOBODY, and why" reasons.

**What is DIFFERENT about the speaker submix tree, and what each difference means** (all read
from §14's probe dump and §10; none measured against the tap):

| difference | vibration tree | speaker tree | consequence |
|---|---|---|---|
| depth and effects | `vibrationMaster -> vibration`, no effects | `controllerMaster -> controller -> controllerPre (SBFX_Boost)` | the tap on the master is post-effects, so the +5 dB is IN the samples. `SpeakerGain = 1.0`; carrying `kSpeakerBoost` over would double it |
| class routing | `SCLASS_controllerVibration.DefaultSubmix = Submix_vibration`, **measured** | `SCLASS_controller` referenced by the `_CONTROL` assets, its `DefaultSubmix` **not read** | whether a `_CONTROL` sound lands in the tapped tree is [derived] from the chain existing. A `Silent` verdict with frames > 0 is what a wrong default submix looks like |
| the gate | `DebugPS5Haptic` measured flipping `ControllerVibration.IsPlaying()` (§14) | the speaker Blueprints "share the gate shape" (§14) and DO call `StartPS5ControllerSound` when it is open (§15/§16) — but that the engine then PLAYS the `_CONTROL` asset was never observed: the `played=1` in those logs was OUR asset player | the speaker `submix watch` answers it in one run |
| asset format | stereo 48 kHz | mono 44.1 kHz (§12) | the engine resamples to the mixer rate before the submix; the tap sees 48 kHz either way. A mono source panned centre arrives equal on FL and FR, which matters because the pad feeds its speaker from R (§16 table) |
| the handle on the controller | `HKPlayerController.m_PS5VibrationSubmix = Submix_vibrationMaster` (§14) | no speaker equivalent seen in the dump | nothing of ours depends on it |

**One deliberate change to the coil sequence, stated so it is not discovered:** the reroute
now registers `parent`, then the vibration master, then the speaker master, in one pass, and
refuses to bind until ALL THREE objects resolve. The reason is UE 4.27's own order of
operations: `RegisterSoundSubmix(parent, bInit=true)` re-inits the parent, and it is each
master's own `RebuildSubmixLinks` that puts it back under it — so binding one lane now and the
other later would re-init the parent underneath the first lane's link. Both masters are
siblings in the same settings package and were both in the object dump, so "all three resolve"
is expected to hold on the first attempt exactly as "both resolve" did before. The sink opens
on the first lane to carry a real signal and drains both rings from then on; each ring is
attached, empty, at its own lane's handover (the §14 latency fix, unchanged).

### The deletions

Gone, in full: `AudioPlayer`, `Fade.hpp`, `LoopList`, `HapticDir`/`SpkDir` and the `haptic/`
and `spk/` directories, `extract_assets.sh`, `wavegen.sh`, `ue4_soundwave_extract.py`,
`uasset_props.py`, `HapticSource` and every mode of it, `CoilOwner` (it reduced to "the submix
or nothing"; the lane verdict in `SubmixWatch` judges both pairs by the same rule), the
`PadSpeakerRoute` `auto`/`hid`/`both` escalation and the `PadSpeakerHid*` keys with the raw HID
audio claim behind them, the `SubmixReroute` and `ForcePS5HapticPath` switches (both always on:
without them there is nothing to tap and no other source exists), and the deploy script's mode
flags. **No fallback anywhere. No extracted assets anywhere.** Retired ini keys are named and
ignored at load, so the ini on the box keeps working.

Per file, lines before (`9553b9e`) → after: `Runtime.cpp` 1309 → 1115, `Runtime.hpp` 240 → 258,
`Config.cpp` 297 → 289, `Config.hpp` 251 → 183, `HidMode.cpp` 318 → 253, `HidMode.hpp` 115 →
95, `PadAudio.hpp` 249 → 69, `PadAudio.cpp` 160 → 33, `SubmixSink.cpp` 255 → 262, `SubmixSink.hpp`
95 → 106, `SubmixDsp.cpp` 317 → 358, `SubmixDsp.hpp` 181 → 225, `SubmixWatch.cpp` 76 → 195,
`SubmixWatch.hpp` 103 → 206, `Mod.cpp` 1138 → 1141, `Platform` 165 → 149; deleted 1 001 lines
of source (`AudioPlayer` 464, `CoilOwner` 199, `LoopList` 59, `Fade` 39, and the HID half of
`PadAudio`) plus 507 lines of asset tooling. The `src/` tree is 8 984 → 7 896 lines in all;
the tests grew 1 445 → 1 249 net of the three deleted files, with 249 new lines of cases for
the interleave, the N-lane watchdog and the lane verdict.

### What stays, and why — `HidMode` in particular

`HidMode` writes USB output report `0x02` with `valid_flag0 = 0x00` every 2 s and on the coil
lane's silence → signal edge, and that is the one non-Sony write to the pad the plugin makes.
It is NOT redundant with the game's own `scePadSetVibrationMode`, and the reason is measured
rather than argued (§12, §16): the game already calls `scePadSetVibrationMode` itself — the
shim only forwarded it, with a `vibmode` debug override that existed because nobody knew
whether the game's chosen mode was right — and with the game's own mode in force the coils were
demonstrably in rumble emulation, every audio path silent, until a report that does not
re-assert `COMPATIBLE_VIBRATION` went out. So the byte is countermanding the game, not
duplicating Sony; "can Sony's API replace it" is a separate question that has never been run,
and the rule stands: leave it alone without evidence. With the HID audio claim gone the report
is byte-identical to the one §12 measured working — id, flag byte, zeros — and there is exactly
one writer of it.

### The one-run test

Deploy, press `Q` once at a checkpoint, then against `<gamedir>/stray-dualsense.log`:

```sh
L=stray-dualsense.log
grep -n "REROUTE submitted\|registration submitted\|REFUSED" "$L"               # 1
grep -o "SUBMIX [a-z]* bound=[0-9] live=[0-9] cb=[0-9]* ([0-9.]*/s)" "$L" | tail -4   # 2
grep -n "submix watch \[" "$L" | tail -8                                          # 3
grep -n "FIRST REAL SIGNAL" "$L"                                                  # 4
grep -n "pad audio: scePad\|SONY ACCEPTED" "$L" | head -3                         # 5
```

| what it says | reading |
|---|---|
| (3) `[vibration] 'CatPurr2_VIBE': the engine MIXED it - peak 0.7…` | **the coils are unchanged** from `9553b9e` — the first thing to check, before the speaker |
| (3) `[speaker] 'cat_purr_loop_01_CONTROL': the engine MIXED it` + (4) `on 'speaker'` + (5) `SONY ACCEPTED` | **the speaker works** if the purr is audible from the pad. Inaudible: the fault is downstream of the tap — `SpeakerGain`, `PadSpeakerGain`, or the endpoint's FL/FR |
| (3) `[speaker] …: NO DATA FROM THE TAP` while `[vibration]` mixes; (2) `SUBMIX speaker … NO CALLBACKS` | **kills the hypothesis** that the same reroute renders the speaker tree. `Submix_controllerMaster` is not being rendered; read the `REROUTE` lines and the `resolved speaker` line (a plain `SoundSubmix`?) |
| (3) `[speaker] …: the engine mixed NOTHING` with `frames` > 0 | rendered but silent: the `_CONTROL` sound is not landing in that tree — `SCLASS_controller`'s default submix, or the gate opens the vibration Blueprint and not the speaker one (`gate[open=…]`) |
| (1) `REFUSED` | a vtable/slot check failed; the ERROR names which. Nothing plays, by design, and `COILS:`/`SPEAKER:` both say `NOBODY` with that reason |
| (5) `=0x80920007` | libScePad believes the pad has no audio (§7): the Proton device tree, not ours |
| (3) the vibration verdicts differ from `9553b9e`'s | the reroute order change above is the suspect; report it before anything else |

### The downmix now matches the engine's — `docs/RESEARCH-UE-PAD-AUDIO-ENDPOINT.md`

That companion document reads Epic's own source for the endpoint contract the plugin is
mirroring; its §7 is a gap table against this build and its §8 ranks what to do about them.
**One HARD gap is closed here, and only that one.** The endpoint submix renders at the DEVICE
channel count — the `ch=8` every SUBMIX line reports — and `ProcessAudioAndSendToEndpoint`
folds it to stereo with the engine's AC-3 table before the real PS5 endpoint ever sees it
(§3.3 there; `AudioMixerChannelMaps.cpp:86-91` @ 4.27, `ToStereoMatrix`, identical numbers in
5.8's `ChannelMap.cpp:26-31`): `L = FL + 0.707·(C + SL + BL)`, `R = FR + 0.707·(C + SR + BR)`,
LFE dropped. Our `DownmixToStereo` kept channels 0/1 and discarded the rest, so a haptic send
that the 3D panner had put behind the cat — and Stray's sends ARE spatialised
(`PS5VibrationAttenuation`, HARD from the pak) — reached the engine's endpoint at −3 dB and
reached our coils as exactly zero. Both lanes now fold the engine's way, including its quad
special case (channels 0 1 2 3 → columns 0 1 4 5, `Get2DChannelMapInternal:379-404`); mono
still goes to both grips, which the submix cannot produce anyway. Pinned column by column in
`tests/test_submix_dsp.cpp`. **HARD** that this is the arithmetic the engine applies;
**SOFT** how much of Stray's haptic content lands outside FL/FR, which is what a run will say.

The doc's other gaps are deliberately NOT addressed here: the container-id endpoint match, the
40 ms queue-ahead against Epic's ~3-11 ms, the soft clip the engine does not have, and the
structural one — being the endpoint via a registered `IAudioEndpointFactory` instead of
tapping a re-parented submix (§8 #2 there, a design that has never been built).

#### Why we REPRODUCE the fold instead of calling the engine's, and what would delete ours

Asked before this landed: we are in the game's process, so use the engine's own mixing code
rather than copying constants out of it — copied constants rot, and reconstructing what the
engine already does is the habit this project has been removing. The three routes were checked
against the 4.27 source; the call chain the endpoint really takes is

```
FMixerSubmix::ProcessAudioAndSendToEndpoint      AudioMixerSubmix.cpp:1537-1642
  -> FMixerSubmix::DownmixBuffer(...)                                :1623
       -> FMixerDevice::Get2DChannelMap(false, in, out, false, map)  :380-385
       -> Audio::DownmixBuffer(in, out, buf, buf, map.GetData())
```

**1. Call the engine's downmix — NOT REACHABLE in this build, HARD.** `Audio::DownmixBuffer` is
`SIGNALPROCESSING_API` and `FMixerDevice::Get2DChannelMap` is a static member of an
`AUDIOMIXER_API` class; both macros expand to nothing in a monolithic build, and Stray is one
`Stray-Win64-Shipping.exe`, so neither has an export to import. None of the three candidates is
virtual either (static, free function, non-virtual member), so there is no devirtualisable call
to piggyback on the way the tap and the reroute do — this plugin already calls
`RegisterSubmixBufferListener` by *vtable index* for exactly that reason. What remains is a
signature scan, and that is the bad half of the trade here: small leaf routines, SIMD variants,
no anchoring string, `/OPT:ICF` folding, and a wrong match is invoked with raw buffer pointers,
i.e. memory corruption in the user's game — against eight float constants CI checks column by
column. Recorded in `SubmixDsp.hpp` as a reproduction of `AudioMixerChannelMaps.cpp:86-91` for
**engine 4.27.2 specifically**, which will not track a different build.

**2. Have the engine fold for us — IMPOSSIBLE FOR A BUFFER LISTENER, HARD.** An `IAudioEndpoint`
declares its own channel count and the engine folds and resamples *to* it; a listener gets
whatever the submix rendered, and `FMixerSubmix::NumChannels` is assigned
`MixerDevice->GetNumDeviceChannels()` unconditionally (`AudioMixerSubmix.cpp:303`, `:317`,
`:1073` — 4.27 has no per-submix channel format left), with the listener called with exactly
that (`:1380`). **There is no property we could write to make this tap deliver stereo**, so on
the tap architecture the fold has to be ours. Being the endpoint (§8 #2 of the research doc) is
what would delete this function outright — it is scoped there, it has never been built, and it
is deliberately not built here.

**Verdict: option 3, knowingly**, with the citation in the code and the two negatives above as
the reason rather than an omission.

### UNVERIFIED

Everything in this section beyond the pure functions (`InterleaveLanes`, `StallWatchdog`,
`JudgeLane`, `DownmixToStereo`, all unit-tested) and the two CI lanes (unit, mingw) that ran
locally. The MSVC lane is CI's. The speaker lane has never delivered a sample to a pad, and
no rear-channel haptic has been felt through the new fold.
