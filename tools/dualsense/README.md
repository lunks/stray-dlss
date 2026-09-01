# Stray DualSense support

Restores Stray's PS5 features on PC/Proton: adaptive triggers, haptics, controller-speaker
audio, and PS5 glyphs. The PC build implements all of it and simply never asks for it.

Full findings: `docs/STRAY-DUALSENSE.md`. Design overview: §11 there.

## Layout

| file | what it is |
|---|---|
| `libScePad_shim.c` | drop-in `libScePad.dll` replacement; forwards to `libScePad_orig.dll` |
| `StrayTriggers.lua` | UE4SS mod hooking Stray's Blueprint UFunctions |
| `ue4_soundwave_extract.py` | decodes UE4 `USoundWave` assets (streamed ADPCM + inline Ogg) |
| `extract_assets.sh` | pulls the haptic/speaker assets out of the pak (needs `oozraw`) |
| `envgen.sh` | turns the VIBE waveforms into amplitude envelopes |
| `uasset_props.py` | reads scalar properties out of a cooked `.uasset`/`.uexp` |

Game assets are deliberately **not** committed — the scripts regenerate them from your own
installed copy.

## Build

```sh
x86_64-w64-mingw32-gcc -O1 -shared -o libScePad.dll libScePad_shim.c \
    -static-libgcc -lole32 -loleaut32 -luuid
```

## Install

1. Rename the game's `libScePad.dll` to `libScePad_orig.dll`, drop ours in beside it.
2. Install `StrayTriggers.lua` as a UE4SS mod (`ue4ss/Mods/StrayTriggers/Scripts/main.lua`).
3. Generate the assets into `<gamedir>/vibe/` and `<gamedir>/spk/`:
   ```sh
   GAME=/path/to/Stray/Hk_project/Binaries/Win64 ./extract_assets.sh
   GAME=/path/to/Stray/Hk_project/Binaries/Win64 ./envgen.sh
   ```

Everything else is discovered at runtime: the shim derives its paths from its own module
location, and finds the pad's audio endpoint by name.

## Requirements

* **Steam's PlayStation Controller Support must be OFF** (global setting, not per-game) —
  it rewrites the pad's HID output reports ~60x/sec and fights the trigger/haptic path.
* GE-Proton with the `proton-ds5-haptic` series. `PROTON_SONY_WINDOWS_DEVICE_NAMES=1` and
  `PROTON_KEEP_SONY_AUDIO_ENDPOINT_VISIBLE=1` are what expose the pad as a Windows audio
  endpoint; GE-Proton 11-6 sets both.
* The pad's ALSA mixer is **not** involved — both rumble and speaker audio were verified
  working with `PCM` muted. Don't chase it.

## Runtime knobs

Command files in the game dir, one line each (mostly for debugging — the game drives all of
this by itself):

```
gain <0-255>        master haptic gain          -> stray_vibe.cmd
play <name> <lvl> <loop>
stop
spk <name> <lvl> <loop>                         -> stray_spk.cmd
spkboost <float>    speaker gain (default 1.7783 = the game's SBFX_Boost +5 dB)
```
