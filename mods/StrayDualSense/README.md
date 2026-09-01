# StrayDualSense — a UE4SS C++ plugin

Restores Stray's PS5 features on PC/Proton — **adaptive triggers, haptics and controller-speaker
audio** — in **one** DLL. It replaces the working three-part arrangement described in
`docs/STRAY-DUALSENSE.md` §11:

| was | now |
|---|---|
| `libScePad.dll` shim (real DLL renamed `libScePad_orig.dll`, 14 forwarding exports) | gone — the plugin resolves the exports out of the copy the game already mapped |
| `StrayTriggers.lua` UE4SS Lua mod | ported to C++ hooks in this DLL |
| `stray_trigger.state`, `stray_vibe.cmd`, `stray_spk.cmd` file IPC | gone — it is all one process now |

Nothing about the *behaviour* is new. Every value, every convention and every threading rule
below was measured on hardware and is documented in `docs/STRAY-DUALSENSE.md`; this is a port,
not a redesign. **What is new is the code, and none of it has been run.** See
[What is unverified](#what-is-unverified) — it is the most important section here.

## What it does

* **Adaptive triggers.** Hooks `COMP_CatScratchableComponent_C:SetPS5TriggerActivated(State,
  Side)` and drives `scePadSetTriggerEffect` with the game's own authored effect,
  `FEEDBACK{position 0, strength 2}`.
  The game calls this **twice per event, once per side** (§8), so L and R are tracked
  independently and **both stay in the trigger mask** — masking a side out leaves it stuck
  stiff. `_OnAfterUseDone` releases both.
* **Haptics.** Hooks `StartPS5Vibration` / `StopPS5Vibration` / `SetPS5VibrationLevel` and plays
  the game's own VIBE amplitude envelopes (one `uint8` per 5 ms) through `scePadSetVibration`.
  Gated on `HKGameUserSettings.PadVibrationEnabled`. Master gain is unity, because the game's
  vibration chain is unity gain end to end (§9).
* **Controller speaker.** Hooks `StartPS5ControllerSound` (and the `...OnAudioComponent`
  variant), `StopPS5ControllerSound` and `SetPS5ControllerSoundLevel`, and plays the `_CONTROL`
  assets on the pad's **WASAPI endpoint** — `Speakers (DualSense Wireless Controller)`,
  4ch/48000/32-bit float. **FL/FR only**; RL/RR are the voice coils and the rumble path covers
  them. The game's own `SBFX_Boost` (+5 dB = ×1.7783) is applied and the result is clamped to
  [-1, 1]. Nothing touches ALSA and nothing uses Sony's audio API — `scePadSetAudioOutPath`
  returns `0x80920007` here and always will (§7).

## How it reaches the pad without the shim

The old shim existed for one reason: a Lua mod cannot call Sony's API. A C++ plugin runs in the
game's own process, so it does not need to *be* `libScePad.dll` — it can resolve the exports out
of the module the game has already mapped:

```
GetModuleHandleW(L"libScePad.dll")   ->   GetProcAddress(...)
```

Two consequences worth knowing:

* **`libScePad.dll` is delay-load import #9** (§1), so it is usually **not mapped** when the mod
  initialises. The pad watcher polls for it and says so in the log after 30 s of waiting.
* **A plugin cannot intercept `scePadOpen`**, which is how the shim got its handle. Instead
  `scePadGetHandle(userId, 0, 0)` is called for user slots 1..4 — verified to return the same
  handles the game's own Opens produced.

### Pad selection is the one behavioural fix

The shim bound to the first positive handle and **worked by luck**. Measured: the game opens
slots 1, 2, 3 and 4 and **all four return positive handles** (`0x101`, `0x202`, `0x303`,
`0x404`), and `scePadGetControllerInformation` returns **success for empty slots too**.

The only reliable discriminator is the struct contents. An occupied slot decodes as
`pixelDensity 44.86, touchpad 1920x1080, deadzones 13/13, connectedCount 1, connected 1`; an
empty slot is **all zeroes**. This plugin adopts the slot whose **`connected` byte (offset 12)**
is set, and logs the decoded struct for every slot either way. Do not trust a return code
anywhere in this API.

## Threading

Learned the hard way, and reproduced here deliberately (§11): **the thread that receives a
request is never the thread that does the work.** An inline looping playback made the old
watcher deaf to `stop`, so the purr never stopped.

| thread | does |
|---|---|
| game thread (UFunction hooks) | records intent only: two bools, a name, a float, a sequence bump. Never blocks, never touches the disk, never calls the pad. The `PadVibrationEnabled` read lives here too, because this is the only thread on which reading a UObject is sound. |
| UE4SS event-loop thread (`on_update`) | **not the game thread** — UE4SS fires it from its own jthread with a 5 ms sleep (`UE4SSProgram.cpp:431`). Config reload, the status line, hook-registration retries, log mirroring. Reads no UObject. |
| trigger worker | `scePadSetTriggerEffect` (a USB HID write) and the optional 120 ms readback |
| haptic worker | envelope load + the 5 ms playback loop; a newer request supersedes one in flight; runaway cap at 12000 steps |
| speaker worker | WASAPI enumerate/activate/render; same supersede rule |
| pad watcher | waits for the delay-loaded DLL, probes the slots, re-probes if the pad stops reporting connected |

## Install

1. Build the DLL (CI does this — see the `dualsense-plugin` job in `.github/workflows/`), or
   download it from the workflow artifacts.
2. Put it at `<gamedir>/ue4ss/Mods/StrayDualSense/dlls/main.dll` and add `StrayDualSense : 1`
   to `ue4ss/Mods/mods.txt`.
3. Generate the assets into `<gamedir>/vibe/` and `<gamedir>/spk/`:
   ```sh
   GAME=/path/to/Stray/Hk_project/Binaries/Win64 tools/dualsense/extract_assets.sh
   GAME=/path/to/Stray/Hk_project/Binaries/Win64 tools/dualsense/envgen.sh
   ```
4. **Remove the old arrangement**: restore the original `libScePad.dll` (i.e. rename
   `libScePad_orig.dll` back) and delete the `StrayTriggers` Lua mod. Running both is not
   supported and they will fight over the pad.
5. Optionally drop `StrayDualSense.ini` next to the DLL. Every key already holds its default.

`<gamedir>` is `.../Stray/Hk_project/Binaries/Win64`. The plugin derives it from the running
executable and never hardcodes it; the resolved paths are printed in the log.

## Requirements

Unchanged from the two-part mod:

* **Steam's PlayStation Controller Support must be OFF** — the *global* setting, not the
  per-game one. It rewrites the pad's HID output reports ~60×/sec and fights everything here.
* **GE-Proton with the `proton-ds5-haptic` series** (11-6 or newer).
  `PROTON_SONY_WINDOWS_DEVICE_NAMES=1` and `PROTON_KEEP_SONY_AUDIO_ENDPOINT_VISIBLE=1` are what
  expose the pad as a Windows audio endpoint; GE-Proton sets both.
* The pad's ALSA mixer is **not** involved. Both paths were verified with `PCM` muted.

## Diagnosing

The log is `<gamedir>/stray-dualsense.log`, truncated per session. It is the product's second
output: a version banner, the resolved asset directories, the decoded information struct for
**every** pad slot, each hook as it registers (and a loud `hook NEVER REGISTERED` for any that
does not), every trigger transmit with its return code, every asset load or miss, and a
`STATUS` line every 30 s carrying every counter.

Two lines to look for first:

* `adopted pad: user slot N, handle 0xNNN (connected byte set)` — if this is absent, nothing
  else can work, and the reason is almost always Steam's global PlayStation Controller Support.
* `hook NEVER REGISTERED: <name>` — that path of the mod is dead for the session. A hook that
  silently never registers is indistinguishable from "the game never called it", which is why
  it is an ERROR rather than a shrug.

## Building

There is **no released UE4SS C++ SDK**. Verified 2026-09-01 against the v3.0.1 release: its
four assets are `UE4SS_v3.0.1.zip`, `zCustomGameConfigs.zip`, `zDEV-UE4SS_v3.0.1.zip` and
`zMapGenBP.zip`. `zDEV` is the **dev build of UE4SS** — `UE4SS.dll`, its PDB and the Lua mods —
with zero `.lib` and zero `.hpp`. A C++ mod must therefore be built inside a checkout of
`UE4SS-RE/RE-UE4SS`, exactly as the official template does: a parent `CMakeLists.txt` that does
`add_subdirectory(RE-UE4SS)` and then `add_subdirectory(StrayDualSense)`.

`mods/StrayDualSense/CMakeLists.txt` plays both roles. Standalone it builds only the portable
pure-logic test (so a Linux runner can check that in seconds); as a sub-project of such a parent
it detects the `UE4SS` target and builds the DLL.

Two pins that are not negotiable:

* **`Game__Shipping__Win64`, MSVC, C++23, `/MD`.** `CppUserModBase.hpp` says it in as many
  words: *"C++ mods will break if UE4SS and the mod don't use the same C Runtime library
  version. This includes them being compiled in different configurations (Debug/Release)."*
  `std::wstring` and `std::function` cross the DLL boundary in both directions. This is the
  opposite of the ReShade add-on in this repo, which is `/MT` for its own reasons.
* **Pin the UE4SS COMMIT, never the tag.** The box reports "v3.0.1 Beta, Git SHA 68caddcf", and
  `68caddcf` is a **main-branch commit dated 2026-08-31** — *not* the v3.0.1 release tag
  (`d935b5b2`, 2024-02-14). The banner reads 3.0.1 only because main's version macros still do.
  The two differ in API (`on_ui_init` and `on_cpp_mods_loaded` do not exist at the tag) and
  therefore in ABI.

### The wall in the build, and how CI gets past it

`RE-UE4SS`'s `deps/first/Unreal` submodule points at `git@github.com:Re-UE4SS/UEPseudo.git`, and
**both that repository and the `Re-UE4SS` organisation return 404 unauthenticated**. RE-UE4SS's
own CI clones it with `token: ${{ secrets.UEPSEUDO_PAT }}`
(`.github/workflows/cmake_build_ue4ss.yml`).

So `.github/workflows/dualsense.yml` takes whichever route is available:

* **With a `UEPSEUDO_PAT` repository secret** — every submodule is fetched at its recorded pin.
  This is the correct build. Getting such a token is not a formality: RE-UE4SS's own guide opens
  with *"Make an Epic account and link it to your GitHub account"* and accepting the invitation
  to the `@EpicGames` organisation, which is the plausible reason the Unreal headers live in a
  private repository at all.
* **Without one** — the Unreal headers come from a pinned public mirror that vendors the whole
  RE-UE4SS tree, and the job prints a loud `::warning::` saying so. The mirror's copies of
  `UE4SS/include/Mod/CppUserModBase.hpp` and the root `CMakeLists.txt` are byte-identical to
  RE-UE4SS at `68caddcf`, which is *evidence* the Unreal headers match the recorded submodule
  pin (`eb40a05f`) but not *proof*. **If the DLL fails to load, or loads and misbehaves, suspect
  this first.**

## What is unverified

**None of this has been run.** There is no Windows machine, no game and no pad in the loop that
produced it; it compiles in CI and that is all CI can prove. Ranked by how likely each is to be
the first thing that misbehaves:


1. **The UE4SS SDK surface in `src/Mod.cpp`.** Every signature there was read out of RE-UE4SS
   at `68caddcf` and out of a public vendored copy of its private `deps/first/Unreal`
   submodule; each is annotated **HARD** in the file header with its source path. None has been
   *compiled* — the Unreal headers hard-require MSVC (`_MSC_VER`, `_CPPUNWIND`, `_WIN32_WINNT`
   checks) and refuse to parse under mingw, so the first real compile of that file happens in
   CI. **This is where to look first if the build fails.**
2. **Whether the mirror's Unreal headers match the box's UE4SS.** See "The wall in the build"
   above. An ABI mismatch here can be a clean load failure (best case) or silent corruption.
3. **The parameter layouts.** They are read through each `UFunction`'s own reflection rather
   than a hand-declared struct, precisely so that a surprise is visible rather than silent — the
   log prints `params of SetPS5TriggerActivated: State:bool@0, Side:uint8/enum@1` or whatever
   the engine actually reports. **Read that line before anything else.** If `Side` is not a
   1-byte parameter at a small offset, the §8 trap is live and one trigger will stick.
4. **Whether `HKGameUserSettings.PadVibrationEnabled` is found and is an `FBoolProperty`.** The
   log says either `PadVibrationEnabled bound` or a warning naming exactly which half failed.
   It is read inside the `StartPS5Vibration` hook — the game thread — because `on_update` is
   **not** the game thread (see the threading table above).
5. **Whether `scePadGetHandle` really returns usable handles for a plugin.** The old shim
   intercepted `scePadOpen`; a plugin cannot, so this uses `scePadGetHandle(userId, 0, 0)`. The
   brief records that as verified on hardware, but not from *this* code.
6. **Everything the old shim did that is not reproduced.** The shim also wrapped
   `scePadSetVibrationMode`, `scePadSetLightBar`, `scePadReadState` and the rest, purely to log
   them. None of that is here. The lightbar is deliberately absent: no shipped Stray content
   drives it, so driving it would be inventing a feature rather than restoring one.
7. **The interaction with the game's own `scePadSetVibration` calls.** The PC build calls it
   itself (call site `009FDE67`), and the shim observed traffic at ~60 Hz. Our writes interleave
   with the game's, exactly as the shim's did. That worked; it has not been reasoned about.
8. **`HapticLoop` / `SpeakerLoop` default to 1.** The working Lua mod passed `loop=1` for
   *every* `StartPS5Vibration` and relied on `StopPS5Vibration` — ported faithfully rather than
   "corrected". If a one-shot ever latches, the runaway cap fires after ~60 s and logs an ERROR
   naming the asset; set `HapticLoop = 0` in the INI without rebuilding.

### What *has* been checked, so it is not re-checked

* The eight non-UE4SS sources compile with `-Wall -Wextra -Werror` and link against
  `ole32`/`oleaut32`/`winmm`/`uuid` under mingw-w64. That covers the pad binding, the trigger
  param block, the envelope player, the WASAPI speaker path and the threading. It is a *proxy*
  for MSVC, not a substitute — CI runs it as its own lane.
* `ShortAssetName` is unit-tested against the real asset names, including the empty-input cases
  that must be treated as an error rather than opening `<dir>/.env`.
* One live crash was found and fixed by reading UE4SS's source rather than assuming: passing a
  default-constructed `std::function` as `RegisterHook`'s **post** callback would throw
  `std::bad_function_call` inside the game's script VM, because
  `UObjectGlobals.cpp`'s `GlobalScriptHookPost` invokes it unconditionally. An explicit no-op is
  passed instead.
