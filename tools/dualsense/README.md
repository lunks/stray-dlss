# Stray DualSense support — the reference implementation

Restores Stray's PS5 features on PC/Proton: adaptive triggers, haptics on the voice coils and
controller-speaker audio. The PC build implements all of it and simply never asks for it.

Full findings: `docs/STRAY-DUALSENSE.md`. The shipping implementation is the UE4SS C++ plugin
in `mods/StrayDualSense/`; its shape is described there and in `docs/STRAY-DUALSENSE.md` §18.

> **The shim + Lua pair here is the REFERENCE IMPLEMENTATION the plugin was extracted from.**
> `libScePad_shim.c` and `StrayTriggers.lua` are what was measured working on hardware on
> 2026-09-01 (§12/§13), and the shim's `audio_probe` is the measured-working pad-speaker
> routing recipe the plugin reproduces verbatim (§16). **Install the plugin, not these.** They
> remain here because they are the measured behaviour: when the plugin and the shim disagree,
> the shim is the one that was seen to work. Do not run both; they fight over the pad and the
> HID report.

## Layout

| file | what it is |
|---|---|
| `libScePad_shim.c` | the reference: `libScePad.dll` proxy (forwards to `libScePad_orig.dll`) that also writes the HID mode byte, plays waveforms over WASAPI and drives the triggers. Messy by construction — it grew by experiment. Its `audio_probe` (`scePadSetAudioOutPath(3)` + `scePadSetVolumeGain({80,80,0,0})`) is the speaker recipe |
| `StrayTriggers.lua` | the reference for the game-intent half: UE4SS Lua hooks on the Blueprint UFunctions, argument discovery by type, per-component stops |
| `deploy-submix-spike.sh` | installs a plugin build on the box, retires the shim arrangement, writes the 0.4.0 ini; `--revert` undoes it |

**No asset tooling.** The extraction scripts (`extract_assets.sh`, `wavegen.sh`,
`ue4_soundwave_extract.py`, `uasset_props.py`) were deleted with the asset-replay path in
0.4.0: the plugin takes both the haptics and the speaker audio from the engine's own submixes,
so no game audio is ever copied to disk (CLAUDE.md §7 forbids redistributing game assets, and
the docs had already condemned that path). Their findings that still matter — the two asset
families, the ADPCM purr, `SBFX_Boost` being a +5 dB trim — are recorded in
`docs/STRAY-DUALSENSE.md` §7 and §10.

## Building the reference shim (only to reproduce a measurement)

```sh
x86_64-w64-mingw32-gcc -O1 -shared -o libScePad.dll libScePad_shim.c \
    -static-libgcc -lole32 -loleaut32 -luuid -lsetupapi -lhid
```

Rename the game's `libScePad.dll` to `libScePad_orig.dll`, drop this in beside it, install
`StrayTriggers.lua` as `ue4ss/Mods/StrayTriggers/Scripts/main.lua`. Command files in the game
dir drive it (`stray_vibe.cmd`: `hap <name> <lvl>`, `hapstop`, `hapflag <hex>`;
`stray_spk.cmd`: `spk <name> <lvl> 1`, `spkstop`). The shim's `hap`/`spk` commands need the
`haptic/` and `spk/` `.f32` files the deleted tooling produced; regenerate them from your own
installed copy if you need to reproduce a measurement.

## Requirements

* **Steam's PlayStation Controller Support must be OFF** (global setting, not per-game) —
  it rewrites the pad's HID output reports ~60x/sec and fights the mode byte.
* GE-Proton with the `proton-ds5-haptic` series (11-6 or newer): `PROTON_SONY_WINDOWS_DEVICE_NAMES=1`
  and `PROTON_KEEP_SONY_AUDIO_ENDPOINT_VISIBLE=1` are what expose the pad as a Windows audio
  endpoint.
* The pad's ALSA mixer is **not** involved. Don't chase it.
