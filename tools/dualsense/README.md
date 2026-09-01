# Stray DualSense support — reference implementation and asset tooling

Restores Stray's PS5 features on PC/Proton: adaptive triggers, haptics on the voice coils and
controller-speaker audio. The PC build implements all of it and simply never asks for it.

Full findings: `docs/STRAY-DUALSENSE.md`. The coil-haptics and trigger-enum findings that
define the current design are §12 and §13.

> **The shim + Lua pair here is the REFERENCE IMPLEMENTATION the plugin was extracted from.**
> `libScePad_shim.c` and `StrayTriggers.lua` are what was measured working on hardware on
> 2026-09-01 (§12/§13); `mods/StrayDualSense/` is that design as a single UE4SS C++ plugin,
> with the experiments — endpoint probes, tone tests, the envelope path, the command files,
> the heuristic trigger — discarded. **Install the plugin, not these.** They remain here
> because they are the measured behaviour: when the plugin and the shim disagree, the shim is
> the one that was seen to work. Do not run both; they fight over the pad and the HID report.

## Layout

| file | what it is |
|---|---|
| `libScePad_shim.c` | the reference: `libScePad.dll` proxy (forwards to `libScePad_orig.dll`) that also writes the HID mode byte, plays waveforms over WASAPI and drives the triggers. Messy by construction — it grew by experiment |
| `StrayTriggers.lua` | the reference for the game-intent half: UE4SS Lua hooks on the Blueprint UFunctions, argument discovery by type, per-component stops |
| `extract_assets.sh` | pak → Oodle-decompress → carve Ogg (needs `oozraw`); selects by sound class pak-wide and writes a combined `haptic_loops.txt` from the assets' own `bLooping` |
| `wavegen.sh` | Ogg/ADPCM → `haptic/*.f32` (stereo 48 kHz), `spk/*.f32` (mono 48 kHz), and splits the loop flags into `haptic_loops.txt` / `spk_loops.txt` (the plugin accepts the combined list for the speaker when the split one is absent) |
| `ue4_soundwave_extract.py` | decodes UE4 `USoundWave` assets (streamed ADPCM + inline Ogg); its `read_names()` is what the loop lists use |
| `uasset_props.py` | reads scalar properties out of a cooked `.uasset`/`.uexp` |

Game assets are deliberately **not** committed — the scripts regenerate them from your own
installed copy. `envgen.sh` (amplitude envelopes for the `scePadSetVibration` path) is gone
with that path: the API reads two bytes and cannot carry a waveform (§12).

## Generating the assets (needed by the plugin too)

```sh
GAME=/path/to/Stray/Hk_project/Binaries/Win64 ./extract_assets.sh
GAME=/path/to/Stray/Hk_project/Binaries/Win64 ./wavegen.sh
```

The loop-list emission and the `_CONTROL` mono conversion in `wavegen.sh` were written
against the layout `extract_assets.sh` leaves in `$WORK` and have **not** been run since;
check that `haptic_loops.txt` lists 22 names and `spk/` holds four files.

## Building the reference shim (only to reproduce a measurement)

```sh
x86_64-w64-mingw32-gcc -O1 -shared -o libScePad.dll libScePad_shim.c \
    -static-libgcc -lole32 -loleaut32 -luuid -lsetupapi -lhid
```

Rename the game's `libScePad.dll` to `libScePad_orig.dll`, drop this in beside it, install
`StrayTriggers.lua` as `ue4ss/Mods/StrayTriggers/Scripts/main.lua`. Command files in the game
dir drive it (`stray_vibe.cmd`: `hap <name> <lvl>`, `hapstop`, `hapflag <hex>`;
`stray_spk.cmd`: `spk <name> <lvl> 1`, `spkstop`).

## Requirements

* **Steam's PlayStation Controller Support must be OFF** (global setting, not per-game) —
  it rewrites the pad's HID output reports ~60x/sec and fights the mode byte.
* GE-Proton with the `proton-ds5-haptic` series (11-6 or newer): `PROTON_SONY_WINDOWS_DEVICE_NAMES=1`
  and `PROTON_KEEP_SONY_AUDIO_ENDPOINT_VISIBLE=1` are what expose the pad as a Windows audio
  endpoint.
* The pad's ALSA mixer is **not** involved. Don't chase it.
