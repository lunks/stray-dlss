# StrayFur — better cat fur at runtime

A UE4SS **Lua** mod that raises the quality and density of the cat's fur without touching the
pak. Written 2026-09-01; **not yet run on hardware** — the box was occupied by the DLSS work.

## Why this is possible without a pak edit

The fur is [GFur](https://github.com/GiM-s/GFur) shell fur, compiled into the exe
(`/Script/GFur.GFurComponent`). Every knob that matters is a property on the live component or
a scalar on its material, so a mod can set them on the spawned instance. The player cat's
Blueprint (`BP_CatPawn`) is **nativized** — a `DynamicClass` with no cooked asset — so its
*defaults* live in the exe and cannot be edited; the *instance* can.

Everything measured about the cat assets (models, materials, the HD material diff, the
nativization census) is recorded in the session notes for `docs/STRAY-DUALSENSE.md`'s sibling
investigation; the numbers in `Scripts/main.lua` cite their source inline.

## The two tiers (switch at the top of `main.lua`)

| tier | what | risk |
|---|---|---|
| **1** | Apply the nine scalars where the game's own `M_Fur_2sidedshading_backpackON_HDScreenshots` differs from the shipped fur material: finer tips, height variation, roughness 0.8, stronger AO. Shading only; no geometry, no cost. | none — the values are the developers' own |
| **2** | Raise `LayerCount` (shells) on the component and on every LOD, keep `MinScreenSize` at 0 so distance never drops the fur; optionally `FurLength` / `ShellBias`. Cost scales linearly with shells. | one **UNCONFIRMED**, below |

## Verified on hardware (2026-09-02, three launches)

All four unknowns below are settled, in the order they were listed:

1. **The hook fires.** `NotifyOnNewObject` resolves the nativized `DynamicClass`; the mod
   applied to `BP_CatPawn_C_2147480326` in `BaseMap` ~25 s after load, every launch.
2. **The player cat's shipped values** — the number nobody could read from the pak:
   `LayerCount=16  FurLength=1.15  ShellBias=1.0  MinScreenSize=0.0`. Twice the companion
   cats' 8 shells, which is why the mod's original 16 was a no-op.
3. **GFur does NOT rebuild on a property write, and a visibility toggle does not force it.**
   The rebuild trigger is the plugin's own BlueprintCallable, `GFurComponent:RegenerateFur()`
   (no parameters, found in the object dump). With it: `AFTER: LayerCount=32 FurLength=2.0`,
   and the user reports the face visibly furrier than the shipped game.
4. **Material scalars apply live** — `applied 9/9 HD scalars` — once
   `CreateDynamicMaterialInstance` is called with its real three-parameter signature
   `(ElementIndex, SourceMaterial, OptionalName)`; two parameters throw.

Verbatim from `ue4ss/UE4SS.log`, launch three:

```
[StrayFur]   material slot 0: applied 9/9 HD scalars
[StrayFur]   SHIPPED: LayerCount=16 FurLength=1.1499999761581 ShellBias=1.0 MinScreenSize=0.0
[StrayFur]   RegenerateFur() -> called
[StrayFur]   AFTER:   LayerCount=32 FurLength=2.0 ShellBias=1.0 MinScreenSize=0.0 (0 LODs)
```

`(0 LODs)`: the `LODs` array iterates empty. Either the component carries no per-LOD
overrides or the TArray-of-struct walk does not iterate; with `MinScreenSize=0` nothing
undoes the change either way, so it has not mattered. UNCONFIRMED which.

## Historical: what was UNCONFIRMED before the first launch

1. **Does the hook fire?** Look for `[StrayFur] applying to …BP_CatPawn_C…` in `ue4ss/UE4SS.log`.
   The mod registers `NotifyOnNewObject` on the nativized class **and** polls with
   `FindFirstOf` every 2 s, so one of the two should land; the log says which.
2. **The player cat's shipped values.** The `SHIPPED: LayerCount=… FurLength=…` line is the number
   nobody could read from the pak. Record it in the docs.
3. **Does GFur rebuild shells on a live component?** If `AFTER: LayerCount=16` prints but the fur
   looks unchanged, the render state was not recreated; the visibility toggle in `applyDensity`
   is the line that did not work, and the fallback is applying before the component registers
   (hook the pawn's `ReceiveBeginPlay` instead of the constructor).
4. **Material scalars.** `material slot N: applied 9/9 HD scalars` — anything less than 9 means a
   parameter name did not resolve on the dynamic instance.

## Deploy (when the box is free)

```sh
GAME=/run/media/deck/GamesLinux/SteamLibrary/steamapps/common/Stray/Hk_project/Binaries/Win64
# stage into the container, then:
mkdir -p "$GAME/ue4ss/Mods/StrayFur/Scripts"
cp main.lua   "$GAME/ue4ss/Mods/StrayFur/Scripts/main.lua"
cp enabled.txt "$GAME/ue4ss/Mods/StrayFur/enabled.txt"      # UE4SS loads it without editing mods.txt
chown -R deck:deck "$GAME/ue4ss/Mods/StrayFur"
```

Then relaunch and read `ue4ss/UE4SS.log` for the `[StrayFur]` lines above. **Confirm the
timestamps are from the new session** — greps match the previous run too.

## Removing it

Delete `ue4ss/Mods/StrayFur/`. Nothing on disk is modified by the mod at runtime.
