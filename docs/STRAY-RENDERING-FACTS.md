# Stray — measured rendering facts

What is **known, measured and verified** about Stray's own rendering, gathered while building a
ReShade D3D12 add-on against it. Nothing here is about that add-on's own feature work.

**Scope rules used when writing this.** Every line below is something that was observed on
hardware, read out of the game's own shader bytecode, or read from the game's own files. Anything
inferred, assumed, or believed-but-unconfirmed has been left out rather than hedged. Where a fact
has a stated provenance in the source tree it is carried here verbatim. No recommendations, no
interpretation of what any of it implies, and no suggested direction.

Hardware/software this was observed on: NVIDIA RTX 4090, driver 610.43.02 (open kernel modules),
Linux 6.17.13 host, SteamOS guest, Proton `GE-Proton-dxvk301-ds5-clean-nowl`, vkd3d-proton,
gamescope (DRM backend, `--hdr-enabled --hdr-itm-enabled`), ReShade 6.8.0.2155 with add-on support.

---

## 1. Engine and process

| Fact | Value |
|---|---|
| Engine | Unreal Engine 4.27.2 |
| Executable | `Stray-Win64-Shipping.exe`, PE32+ (x86-64) |
| Graphics API in use | D3D12 |
| Project name | `Hk_project` |
| Game version string seen on the title screen | `v1.54368 (Revision 26632)` |

Observed swapchain configurations, from `IDXGISwapChain::ResizeBuffers`:

* `BufferCount = 3, Width = 3840, Height = 2160, NewFormat = 24`
* `BufferCount = 3, Width = 2560, Height = 1440, NewFormat = 24`

`NewFormat = 24` is `DXGI_FORMAT_R10G10B10A2_UNORM`.

---

## 2. Filesystem layout

Game directory (this install):

```
<SteamLibrary>/steamapps/common/Stray/Hk_project/Binaries/Win64/
```

Config and saves live in the Proton prefix, **not** in the game directory:

```
<compatdata>/1332010/pfx/drive_c/users/steamuser/AppData/Local/Hk_project/Saved/
    Config/WindowsNoEditor/Engine.ini
    SaveGames/
    Crashes/UE4CC-Windows-<GUID>_0000/      CrashContext.runtime-xml, UE4Minidump.dmp
    Logs/                                    (observed empty on this install)
```

Steam AppID: **1332010**.

Two observations about configuration on this install:

* Engine.ini settings were observed to take effect; command-line arguments were not.
* There are two `compatdata` trees for this title on this machine. The one under
  `/home/deck/.local/share/Steam/steamapps/compatdata/1332010` is the live one. The one on the
  secondary library (`GamesLinux`) is a ~6.1 MB skeleton that is not read.

---

## 3. The TAA pass

Stray uses UE 4.27's standalone temporal AA compute shader, `FTAAStandaloneCS`.

**Primary pass, identified by shader hash** (fnv1a64 over the DXBC):

```
0x1708ec956099e259
```

Its measured binding signature — compute, shader model 5.0, all resources 1920×1080 at the
resolution it was measured at:

| Register | Role | Format |
|---|---|---|
| `t0` | depth | `r32_g8_typeless` |
| `t2` | velocity | `r16g16b16a16_unorm` |
| `t5`, `t6` | colour | `r16g16b16a16_float` |
| `u0` | `OutComputeTex` — the TAA output | `r16g16b16a16_float` |
| `u1` | `OutComputeTexDownsampled` (optional, declared by the shader) | — |

**A second TAA candidate exists in the same title:**

```
0x52101a15e1a0c5cc     t0 depth, t3 velocity, t7 colour, t8 r16g16_float
```

UE 4.27 compiles `FTAAStandaloneCS` in more than one permutation. `ETAAPassConfig::Main` and
`ETAAPassConfig::MainUpsampling` produce different DXBC and therefore different hashes.

**A measured false positive**, recorded so it is not re-discovered: `0x901e041a7cadc9db` scores
confidence 150 on a class-quorum test with colour=1, depth=2, velocity=0.

Shader census on this install: **728 distinct PS/CS shaders** seen in gameplay, `not_dxbc=0`,
`dxil=0` — i.e. every pixel/compute shader observed is DXBC, none DXIL. During the main menu the
same census reads **~150**; the count rises to ~728 on entering gameplay. (This count covers PS
and CS only and says nothing about DXR.)

---

## 4. Depth

* The depth resource bound at `t0` is **`r32_g8_typeless`** — a typeless, planar depth-stencil.
* The SRV the game creates over it is **`r32_float_x8_uint`**.
* UE 4.27 renders with reversed-Z.

A statistic gathered over the depth texture during the main menu and loading screens reads
`below 0.25: 3456000, above 0.75: 0, mean 0.00000` — i.e. menu and load frames carry no usable
depth range. Gameplay frames do carry range.

---

## 5. Velocity

The velocity buffer at `t2` is `r16g16b16a16_unorm`, and it is **sparse**: UE 4.27 writes it only
for pixels covered by moving objects. Static geometry carries no velocity and its motion must be
reconstructed from depth and the camera matrices.

**The encoding, from UE 4.27 `Engine/Shaders/Private/Common.ush:1537-1570`:**

```
EncodedV.xy = V.xy * (0.499f * 0.5f) + 32767.0f / 65535.0f          // encode
V.xy        = EncodedV.xy * InvDiv - 32767.0f / 65535.0f * InvDiv   // decode
InvDiv      = 1.0f / (0.499f * 0.5f)
```

**Both constants were located in Stray's own DXBC**, not merely taken from the engine source:

| Constant | Value | Bit pattern | Notes |
|---|---|---|---|
| `InvDiv` (decode scale) | `4.00801611f` | `0x408041AB` | bytes `AB 41 80 40` |
| Folded MAD bias | `2.00397754f` | `0x4000412B` | appears **negated** in a `mad` as `0xC000412B`, bytes `2B 41 00 C0` |
| Bias term | `32767/65535 = 0.49999237f` | `0x3EFFFF00` | the bias is **not** 0.5 |

`0.49999237f * 4.00801611f = 2.00397754f`. The decode's second term is folded into a MAD immediate
by the compiler rather than appearing as a separate subtract.

The game's own decode helper is named `DecodeVelocityFromTexture`.

---

## 6. The View constant buffer

Stray's TAA shader carries the stock UE 4.27.2 `View` uniform buffer at register **`b1`**.

Observed sizes across different shader permutations in the same session — the buffer's total size
varies, but the row offsets below did not:

* `size = 126 float4s (2016 bytes)`
* `size = 131 float4s (2096 bytes)`
* `size = 145 float4s (2320 bytes)`

Row offsets (rows are float4 rows, i.e. byte offset / 16). The stock UE 4.27.2 layout was
established twice independently — read out of `VIEW_UNIFORM_BUFFER_MEMBER_TABLE`
(`SceneView.h:582-774`) and recomputed by a layout script over the same declaration list:

| Field | Row | Byte offset |
|---|---|---|
| `ViewToClip` | 28 | 448 |
| `ViewToClipNoAA` | 32 | 512 |
| `ClipToPrevClip` | 122 | 1952 |
| `TemporalAAJitter` | 126 | 2016 |
| `ViewRectMin` | 129 | 2064 |
| `ViewSizeAndInvSize` | 130 | 2080 |
| `LightProbeSizeRatioAndInvSizeRatio` | 131 | 2096 |
| `TemporalAAParams` | 152 | 2432 |

The six rows a jitter recovery needs — `proj=28 noaa=32 clip=122 jitter=126 size=130 params=152` —
were located in Stray's running View buffer and reported at the strongest tier (`tier=full`).

`ClipToPrevClip` at row 122 was confirmed **in Stray's own TAA shader by pure DXBC instruction
analysis**, with no reflection names involved.

Three notes on reading this buffer:

* `LightProbeSizeRatioAndInvSizeRatio` at row 131 is `(1,1,1,1)`, and is a decoy for a naive
  search that expects an identity-looking row.
* The shader declares `dcl_constantbuffer cb1[131]`. That 131 is the **highest row the shader
  indexes**, not the buffer's size — `ViewSizeAndInvSize` ends at byte 2096.
* These offsets are fixed for a given engine build but are not invariant across a licensee edit to
  the member table, which is why they were checked rather than trusted.

**Jitter convention**, from the engine source and consistent with the above:

```
InJitterOffsetX = TemporalJitterPixels.X = TemporalAAParams.z = TemporalAAJitter.x * W *  0.5f
InJitterOffsetY = TemporalJitterPixels.Y = TemporalAAParams.w = TemporalAAJitter.y * H * -0.5f
```

Note the Y term is negative.

---

## 7. Camera cuts

UE 4.27 assigns `PrevViewMatrices = ViewMatrices` on any frame that is a camera cut. The
observable consequence in the View buffer is that `View.TemporalAAJitter.zw` becomes equal to
`.xy`. This was used as a live cut detector and confirmed working against the running game
(reported as `detector=LIVE`).

Observed cut counts: **3** across the splash and main menu, **5** by the time gameplay is running —
i.e. entering gameplay from the menu produces cuts.

---

## 8. TAA history

The resource written at `u0` (`OutComputeTex`) is extracted by UE 4.27 as the **next frame's
`HistoryBuffer[0]`**. Overwriting `u0` therefore feeds whatever was written into the next frame's
temporal history.

The same resource can also appear bound as this frame's **scene-colour input** (at the colour SRV
register) rather than as the history slot; the two cases are distinguishable only by which
register it turns up on at a given dispatch.

---

## 9. Stability observations on this install

Recorded because they are host/environment facts, independent of any add-on:

* `gamescope-wl` segfaulted three times in one afternoon (11:40, 11:42, 14:03), and once the day
  prior. The nvidia driver was unloaded and reloaded at 11:43.
* One GPU `Xid 109 (CTX SWITCH TIMEOUT)` was recorded against `Stray-Win64-Shi`, channel
  `0x00000012`.
* UE4 crash dumps exist from sessions with no third-party add-on installed at all, with
  `ErrorMessage: Unhandled Exception: 0xe06d7363` (a C++ exception) and one
  `EXCEPTION_ACCESS_VIOLATION reading address 0x0000000000000010`.
* Killing the game leaves a `reaper` process (`SteamLaunch AppId=1332010`) behind. While it
  exists, Steam silently ignores further `steam://rungameid/1332010` launches.

---

## 10. Input, on this machine

Not a property of the game, but needed to drive it unattended:

* The physical DualSense is held by **Steam** via `/dev/hidraw0`. Nothing holds its evdev nodes.
* Steam Input re-emits it as **"Microsoft X-Box 360 pad 0"**, and that node is what the game
  reads. Its `eventN` number is not stable — Steam tears it down with the game.
* Writing `input_event` structs directly to `/dev/input/eventN` reaches `input_inject_event()` in
  the kernel and is seen by every reader of that node; no `uinput`, `ydotool` or `evemu` needed.
  Neither the pad nor the keyboard node is `EVIOCGRAB`'d.
* ReShade's screenshot bind is `KeyScreenshot=44` (`VK_SNAPSHOT`), which is Linux `KEY_SYSRQ=99`.
  Injecting it on the real keyboard node makes ReShade write a 4K PNG into the game directory.
* gamescope's `SIGUSR2` screenshot produced no file. `ffmpeg`'s `kmsgrab` cannot read its
  framebuffer, which is `XB30` (`XBGR2101010`, 10-bit HDR).

---

## 11. vkd3d-proton's object model, measured (2026-09-01)

For the native D3D12 hook layer (`docs/superpowers/plans/2026-09-01-dlss-sr-ue4ss-plugin.md`,
Task 1). Run on the box through `tools/run-harness-proton.sh` — the real RTX 4090, vkd3d-proton
3.1.0, private Wine prefix and Xvfb display, no ReShade — from the harness built by CI at
commit `2a597a2`. Verbatim:

```
[test] whether vtables are shared across objects of a class
  vtable-static: list=1 queue=1 resource=1 device=1 (second device SAME object)
  vtables: list=00006FFFFCC60F40/00006FFFFCC60F40 queue=00006FFFFCC61FE0/00006FFFFCC61FE0 resource=00006FFFFCC64740/00006FFFFCC64740 device=00006FFFFCC625E0/00006FFFFCC625E0
  (recorded, not asserted)

[test] whether private-data interfaces are released at object destruction
  SetPrivateDataInterface hr=0x00000000
  private-data-release: fired=1 (fired_before_destroy=0)
  (recorded, not asserted)
```

* **`ID3D12GraphicsCommandList`, `ID3D12CommandQueue` and `ID3D12Resource` each share ONE
  vtable across distinct objects** (two lists on one allocator, two queues, two committed
  buffers — different objects, identical vtable pointers). Patching a slot in that vtable
  therefore reaches every existing and future object of the class, process-wide. HARD.
* **A second `D3D12CreateDevice` on the same adapter returns THE SAME device object**, so the
  device row is trivially 1; whether two *distinct* vkd3d devices would share a vtable was not
  measured and does not matter — there is only ever one.
* **`SetPrivateDataInterface`'s interface is released when the resource is destroyed**, and not
  before (`fired_before_destroy=0`): the final `Release()` is a usable destruction callback.
  HARD on vkd3d-proton 3.1.0, as it is documented on Microsoft's runtime.

Same run, the earlier probes: no `ID3D12InfoQueue` on vkd3d (validation is a CI/WARP property),
the resolve pass, the camera-branch orientation and the clobber/restore golden test all PASS.

## 12. UE4SS attach timing, from two logs of one session (2026-09-01)

Not from a probe — from the box's own `ReShade.log` (local time, America/Sao_Paulo = UTC-3) and
`ue4ss/UE4SS.log` (UTC, "Timezone: UTC (local disabled due to wine)") of the same launch, laid
side by side. Both are wall clocks in one process.

```
ReShade.log  18:10:03:386  Initializing crosire's ReShade version '6.8.0.2155' ... dxgi.dll
UE4SS.log    21:10:03.440  Console created / UE4SS - v3.0.1 Beta #0 - Git SHA #68caddcf
UE4SS.log    21:10:03.444  Starting mods (from mods.txt ...)            <- start_cpp_mods() here
ReShade.log  18:10:04:235  Redirecting CreateDXGIFactory1(...)
ReShade.log  18:10:04:390  Installing delayed hooks for 'C:\windows\system32\d3d12.dll' (Just loaded via LoadLibrary('d3d12.dll'))
ReShade.log  18:10:04:414  Redirecting D3D12CreateDevice(...)
```

* **UE4SS starts its mods ~970 ms BEFORE the game's `D3D12CreateDevice`** (21:10:03.444 against
  21:10:04.414 UTC), and ~790 ms before the game even creates its DXGI factory. A C++ mod's
  `start_mod()` runs in `UE4SSProgram`'s constructor at that point (HARD from the UE4SS source,
  `UE4SSProgram.cpp:386-420`), so early attach is available to a UE4SS mod on this title.
* **`d3d12.dll` is NOT yet loaded at that moment**: the game `LoadLibrary`s it at 04.390, 24 ms
  before creating the device. An export hook on `D3D12CreateDevice` from `start_mod()` therefore
  has to load `d3d12.dll` itself or wait for it — and whatever installs a hook on that export
  after ReShade's delayed hooks (04.390) sits ABOVE ReShade's and sees ReShade's proxy, not
  vkd3d's device. The probe build (`mods/StrayDLSS` 0.0.1) measures that directly.
