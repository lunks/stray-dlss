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
* **Controller speaker: WORKING**, playing the game's own `_CONTROL` assets on the pad's
  Windows audio endpoint, with the game's own +5 dB trim. See §10.
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
| 4 / 5 | audio volume / audio path is valid |
| 6 / 7 | mic LED / mute |

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
