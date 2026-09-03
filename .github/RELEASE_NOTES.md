This release carries **two hosts for the same code**, as two separate zips. They are alternatives:
install one.

* **`stray-dlss-plugin-*.zip` — the UE4SS C++ plugin. This is the project's primary host** and
  what the documentation describes. It loads inside the game process, installs its own D3D12
  hooks, and needs no ReShade.
* **`stray-dlss-reshade-addon-*.zip` — the ReShade add-on.** The original host, still built and
  tested, for the ReShade configuration only.

## Requirements

Both hosts need:

- An NVIDIA RTX GPU and a driver providing `nvngx.dll` / `_nvngx.dll` to the Proton prefix.
- `nvngx_dlss.dll` beside the game executable. It is not redistributed here — take it from
  https://github.com/NVIDIA/DLSS (`lib/Windows_x86_64/rel/`).

The plugin additionally needs **UE4SS** installed in the game directory.

The add-on additionally needs **ReShade 6.8.0 or newer, with add-on support** — earlier versions
crash with D3D12 DLSS under vkd3d-proton, and the fix landed in 6.8.0.

## Install

Game directory below means
`<SteamLibrary>/steamapps/common/Stray/Hk_project/Binaries/Win64/`.

- **Plugin:** extract into `<game directory>/ue4ss/Mods/`, then add the line from
  `mods.txt.fragment` to `ue4ss/Mods/mods.txt`.
- **Add-on:** extract into `<game directory>`. On Linux also install `msvcrt40` and `vcrun2022`
  into the prefix with protontricks.

## Reporting a problem

Attach `stray-dlss.log` from the game directory, plus `UE4SS.log` (plugin) or `ReShade.log`
(add-on). Launch with:

```
DXVK_NVAPI_LOG_LEVEL=info PROTON_LOG=1 VKD3D_DEBUG=warn %command%
```
