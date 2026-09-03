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

## 13. The interception seam changed nothing (measured 2026-09-01)

Plan Stage 1's acceptance: the add-on rebuilt behind `icept::Backend`/`icept::Sink` (commit
`3193f46` and fixes, CI run 33572090310) deployed to the box, one session from the main menu
through the save select into gameplay in The Slums (slot 1), 150 s dwell. `stray-dlss.log`
diffed against the previous build's, normalised for pointers and counts:

* **The WARN/ERROR sets are identical** — every difference is an address (the four
  `ext_unhook` restores, the snippet's IAT patch site). No new WARN or ERROR line.
* Same milestones: `DLSS feature created: 1920x1080 -> 3840x2160, Performance, preset=13,
  flags=0x4b`, `DLSS evaluate OK`, `View row 135 ... y*z=1.000000`, `NR VALIDATED`,
  `Add-on event check OK`, the DLSS indicator on screen in the menu and in gameplay
  (ReShade screenshots, viewed).
* Gameplay in The Slums renders correctly at 55 fps (`[perf] frames 27000-27600: 55.6 fps`).

Two box facts learned along the way, both now in the tooling:

* **Steam Input is OFF for this title** (`UseSteamControllerConfig 0` in `localconfig.vdf`,
  a consequence of the DualSense work, which needs the game to see the real pad), so the
  "Microsoft X-Box 360 pad 0" node never appears and `launch-stray.sh` drives the menu with
  Enter on the sysrq keyboard instead. The game switches its glyphs to keyboard the moment a
  key arrives (seen in the screenshot).
* **The shader census in gameplay in The Slums is 388-392**, not ~728 (that was the
  apartment); the main menu is 110. The add-on's `in_game` threshold is now 300.

## 14. The native observer installs cleanly but crashes the live game on vkd3d-proton (measured 2026-09-01)

The differential observer (plan Stage 2): our own D3D12 vtable hooks installed BESIDE ReShade,
diffing every dispatch's bindings against ReShade's answer. Two things are settled and one is
open.

**Settled — the resolve is correct.** On WARP and under the real-ReShade CI lane, the hooks
install on the (unwrapped) device, a UE4-shaped frame — depth+stencil as two views of one
`R32G8X24_TYPELESS` resource, velocity, colour, history, 1x1 eye adaptation, a root CBV whose
row 135 validates itself (`PreExposure * OneOverPreExposure == 1`) — resolves through them to
the exact measured registers, the hooked `Dispatch` consumes the oracle's published answer and
`disagreements=0`, a deliberately wrong oracle is caught, and after the depth texture is
released `t2`/`t4` vanish with `unknown_lookups += 2`. The debug layer is clean. (CI 33574…,
`test_native_hooks_ue4_shaped_frame`.)

**Settled — install itself works on the target.** `NativeMode=observe` on the box, from
`ReShade.log` (the add-on's own log truncates at the crash, unflushed):

```
vtable_patch: ID3D12Device::CreateCommittedResource slot 27 …
… (24 more)
native backend: mode=observe device=000000001FAF0080 device-slots=13 list-slots=11 patches=24 increment=32
```

All 24 patches applied; vkd3d-proton shares one static vtable per class (§11), so this reaches
every device and command list process-wide.

**RESOLVED the same evening — the crash was OUR lifecycle, not vkd3d, not ReShade's proxy.**
The bisection (`NativeInstall=device`, `=list`, `NativeSentinel=0`) crashed identically for two
disjoint hook sets and without the sentinel, which ruled out every hook body. The decisive run
was `NativeTarget=proxy` (hooks on ReShade's proxy vtables instead): its install reported
`patches=0` with no `VirtualProtect` error, i.e. **every slot already held our replacement**.
ReShade **unloads and reloads the add-on DLL across the game's device recreate** (`Unloading
add-on` … `Loading add-on` in `ReShade.log`, right after `init_device`); a process-global vtable
patch outlives the image it points into, the DLL comes back at the same base with fresh
statics — every `g_orig_*` NULL — and the first game call through a patched slot calls
address 0: `EXCEPTION_ACCESS_VIOLATION 0x0000000000000000` at `SecondsSinceStart=0`, for any
hook set, sentinel or not. Fix: `app::DlssApp::shutdown()` restores the slots on detach (only
where they still hold our pointer), so the reload re-installs cleanly. In a UE4SS-mod
configuration the DLL is never unloaded and this cannot occur.

(Two smaller facts from the same runs: `QueryInterface` on ReShade's proxy from inside its own
`init_device` callback DEADLOCKS — the process sat alive for 600 s — so the proxy target patches
`ID3D12Device2`'s slot without asking; and the game destroys and recreates its first D3D12
device at startup on this title, which is what triggers the add-on reload in the first place.)

**The observer then ran live, run E (2026-09-01 21:55, 240 s in The Slums):**

```
IN GAME (census=390, taa_pipelines=1)
DIFF SUMMARY [frame 15000] dispatches=82688 agree=0 mismatch=3247 unknown=82688 extra=2077 unconsumed=0 | TAA dispatches=124 disagree=124 | disagreements=82688
NATIVE SHADOW [frame 15000] mode=observe patches=24 resolves=82688 (no-layout 0) unknown-lookups=1005029 unknown-copies=3767242 root-signatures=51 pipelines=674 resources live=3049 (registered 37091, destroyed 34042, sentinel-failures 0) slots=468755 heaps=2
DIFF hash=901e041a7cadc9db 480x270 TAA: 0 mismatch, 1 unknown, 0 extra
  UNKNOWN  heap: oracle=00000000235F0018 native=UNKNOWN
```

* **The TAA pass agreed with ReShade on every register in all 124 of its dispatches** — `t0`-`t5`
  (depth and stencil as one resource), `u0`, and the View constant buffer — and its single
  "unknown" was the HEAP IDENTITY: the oracle names ReShade's proxy heap object, the native side
  the real heap beneath it. That is an object-identity difference, not a binding one; it is now
  its own class (`heap-identity-only`), not a disagreement, and is why `agree=0` reads 0 in
  this build's summary for every dispatch.
* The gameplay-time `mismatch=3247` / `extra=2077` were NOT characterised: the 40-line log cap
  was spent on two menu hashes. All 170 logged `cb:` unknowns were root CBVs into ONE upload
  buffer (`2a185df0`) the registry never saw — either created through an unhooked entry point
  (`ID3D12Device4/8::Create*Resource1/2`, now hooked) or in the detach→reload window.
* `unknown-copies=3.7M` is UE4 copying from null-descriptor slots, which the shadow records as
  "nothing" on both sides — benign, but it is what inflates `unknown-lookups`.
* Cost: `[perf]` was not compared; the session held 55 fps as before (SOFT, from the launcher's
  cadence, not measured).

Run F (per-hash logging, heap identity reclassified) follows in §15.

## 15. The observer's first characterised numbers — run F (2026-09-01 ~22:05, The Slums)

Per-hash disagreement logging, heap identity reported apart. The game reached gameplay and
held 55-56 fps with the observer on (`[perf] frames 7200-7800: 55.2 fps avg`), against 55.1-55.6
without it — the observer's cost is inside the noise.

```
DIFF SUMMARY [frame 7800] dispatches=39505 agree=121 mismatch=1017 unknown=39384 extra=24 heap-identity-only=121 unconsumed=0 | TAA dispatches=125 disagree=4 | disagreements=39384
NATIVE SHADOW [frame 7800] mode=observe patches=28 resolves=39505 (no-layout 0) unknown-lookups=647232 unknown-copies=1623955 root-signatures=52 pipelines=673 resources live=3049 (registered 22683, destroyed 19634, sentinel-failures 0) slots=472507 heaps=2
```

The logged lines (3 per hash), classified:

| class | lines | what |
|---|---|---|
| `MISMATCH t#/u#` | 128 | **all 128 name the same native buffer `29f55df0`** against an oracle buffer (both sides `fmt unknown 0x0`, i.e. buffer views); 2 of them the oracle calls an `R16G16B16A16_FLOAT` texture |
| `UNKNOWN cb` | 564 | root CBVs whose GPU VA resolves to no registered buffer: **311 into `29f55df0`, 253 into `50e82f50`** — two buffers |
| `UNKNOWN t#/u#` | 271 | table slots the shadow has no entry for |
| `HEAP-ID` | 30 | proxy-vs-real heap identity, expected |

* **The TAA pass (`0x901e041a7cadc9db`, 480x270): 121 of 125 dispatches agree on every register**
  — depth+stencil over one resource at t2/t4, velocity, colour, history, eye adaptation, u0, and
  the View constant buffer. The 4 that disagree each carry 31 `cb:` unknowns, all VAs into
  `29f55df0`, and nothing else.
* So the residual is ONE bounded defect, not scattered wrongness: the native registry's
  GPU-VA map does not cover UE4's constant ring buffer(s) `29f55df0`/`50e82f50`, and the shadow
  attributes a set of buffer SRV/UAV slots to `29f55df0` where ReShade's tracker names others.
  Leads, in order: (1) a buffer registered lazily from a view carries a `GetDesc().Width` that
  is not the range the game addresses (placed/sub-allocated), so `buffer_for_va` misses;
  (2) a resource address REUSED after death, where ReShade's never-cleared view->resource map
  (CLAUDE.md §5) would name the stale identity — which would make the ORACLE the wrong side
  for those 128 lines (SOFT); (3) the buffer was created in the detach->reload window and only
  seen through views. UNCONFIRMED which; the "native resolve: root CBV ... in no registered
  buffer" line never printed, which itself says the VA lookup did not fail on the native side
  — pointing at (1)/(2), an offset or identity disagreement rather than an absent buffer.
* `unknown-copies=1.6M`: UE4 copying from null-descriptor slots; both sides record nothing
  there, so it never reaches the diff.

**Verdict for the Stage 2 gate: NOT zero, and characterised.** `disagreements=39384` of 39505
dispatches, but 39 384 - 24 of them are the two-buffer defect above; the TAA registers the
hook needs are reproduced exactly. Stage 3 (`drive`) must wait for that defect to be closed
and a run with `disagreements=0` outside the heap-identity class.

**A second hazard found while root-causing the crash, not yet closed:** the sentinels attached
to resources during the add-on's FIRST life outlive the unload; when those resources die after
the reload, the runtime calls `Release` on objects allocated by an image that is gone (same
base address, different heap). It did not crash in three sessions, which proves nothing. The
UE4SS-mod configuration has no reload; in the ReShade-hosted one, either pin the DLL
(`GET_MODULE_HANDLE_EX_FLAG_PIN`) or keep the sentinel out of the ReShade host.

## 16. The observer gate, closed (runs G-K, 2026-09-01 23:05 - 2026-09-02 00:00, The Slums)

Same box, same save (The Slums, slot 1), the launcher's Enter-driven path to gameplay,
240 s dwell after `IN GAME`, one build per run. Every number below is the `[frame 15000]`
report of that run's `stray-dlss.log`, verbatim except where a line is marked cut.

| run | build (CI run) | what changed | DIFF SUMMARY (cut to the counts) | TAA |
|---|---|---|---|---|
| F (§15) | e79a4b1 | — | `dispatches=82705 agree=121 mismatch=2200 unknown=82584 … disagreements=82584` | 125, disagree 4 |
| G | 9b8fa28 / 33581700761 | current-per-parameter oracle CBs, null slots, range VA lookup, same-RS no-op, adjudication | `dispatches=82650 agree=34775 mismatch=10000 unknown=43494 extra=21 heap-identity-only=29666 … disagreements=47875` | 128, disagree 1 |
| H | fc6b03d / 33582624785 | tombstones, copy provenance | `dispatches=82654 agree=33321 mismatch=2121 unknown=48610 extra=30 … disagreements=49333` | 128, disagree 1 |
| I | 0f073b6 / 33583278244 | RESHADE-VIEW-RECREATED, per-dispatch gate | `dispatches=86276 agree=45284 mismatch=2615 unknown=39738 extra=212 … disagreements=40992` — `DIFF GATE … oracle-wrong=40780 UNRESOLVED=212` | 124, disagree **0** |
| J | 45787bb / 33583907692 | the oracle reports the slots it drops | `dispatches=82809 agree=32174 mismatch=3903 unknown=48744 extra=32 … disagreements=50635` — `oracle-wrong=50616 UNRESOLVED=19` | 125, disagree 0 |
| **K** | 5c2a78e / 33584631939 | RESHADE-LIVENESS-GAP | see below | 126, disagree 0 |

**Run K, the closing run, verbatim:**

```
DIFF SUMMARY [frame 15000] dispatches=82640 agree=36342 mismatch=9132 unknown=42632 extra=26 heap-identity-only=32682 unconsumed=0 | TAA dispatches=126 disagree=0 | disagreements=46298
DIFF VERDICTS [frame 15000] slots: RESHADE-STALE=1583 RESHADE-COPY-STALE=0 RESHADE-VIEW-RECREATED=137811 RESHADE-LIVENESS-GAP=21 NATIVE-BLIND=0 LIVENESS-CONFLICT=0 NATIVE-MISSED=0 ORACLE-MISSED=0 BOTH-LIVE=0 unadjudicated=0
DIFF GATE [frame 15000] disagreeing dispatches: oracle-wrong=46298 (every differing slot convicts ReShade) UNRESOLVED=0 (TAA unresolved=0) - the gate is UNRESOLVED=0
NATIVE SHADOW [frame 15000] mode=observe patches=28 resolves=82640 (no-layout 0) unknown-lookups=370971 null-lookups=478987 dead-lookups=104792 unknown-copies=0 root-signatures=51 pipelines=671 resources live=3049 (registered 37117, destroyed 34068, sentinel-failures 0, unarmed 0) slots=524555 heaps=2
```

`disagreements` is not zero and never will be: **every one of the 46 298 disagreeing
dispatches is adjudicated against ReShade**, slot by slot, from evidence the log carries.
The gate the plan asked for — `disagreements=0` *or every remaining one explained with
evidence naming which side is wrong* — is met by the second clause, and the TAA pass agrees
outright (126/126 in K; 124/124 and 125/125 in I and J).

### 16.1 The adjudication table

Every differing slot gets a verdict from three liveness answers (ReShade's own
`is_resource_live`, the registry's sentinel liveness, whether the registry ever registered
the resource) plus the native shadow's provenance for the slot. Classes, with what each
proves and how many slots of run K fell into it:

| verdict | slots (K) | evidence on the line | which side is wrong |
|---|---|---|---|
| `RESHADE-VIEW-RECREATED` | 137 811 | the native online slot was **copied** at seq N from offline view S; the native entry for S was written at seq M > N and names the oracle's resource (or the oracle dropped S as dead/unmapped) | **ReShade.** Its descriptor tracking stores the source *view handle* and resolves it at query time; UE4 recycles offline slots, so a re-created S changes what ReShade reports for an online slot whose bytes still hold the copy. D3D12 copies by value; the native shadow does too. |
| `RESHADE-STALE` | 1 583 | the native slot is a **tombstone**: the resource it viewed died (sentinel fired) after the write, and the oracle names that very address — reused since | **ReShade.** The §5 never-cleared view→resource map, now with the address-reuse variant both liveness sets are blind to. |
| `RESHADE-LIVENESS-GAP` | 21 | the native side names a resource whose sentinel is still attached (live, the runtime's word) while ReShade's init/destroy_resource-fed set calls it dead — every instance one of the three swapchain back buffers (`R10G10B10A2_UNORM 3840x2160`) | **ReShade** (its liveness events never carried the back buffers). |
| `RESHADE-COPY-STALE` | 0 | the copy's source, per ReShade's own map, agrees with native | — (defined, never fired) |
| `NATIVE-BLIND` | 0 | the oracle names a live resource the registry never saw, or a slot never written since attach | native |
| `NATIVE-MISSED` | 0 | the oracle names a resource live on both sides; the native walk has no such slot | native |
| `LIVENESS-CONFLICT` | 0 | the two trackers disagree about a resource in a way neither rule above explains | ambiguous |
| `BOTH-LIVE` | 0 | both sides name a live resource and differ, no provenance settles it | ambiguous |
| `ORACLE-MISSED` | 0 | the native side names a live resource and the oracle has nothing, unexplained | ambiguous |

Verbatim lines from run K (the DIFF line and the first two verdicts under it):

```
DIFF hash=f50e8aa52541b8c2 120x68 dispatch: 8 mismatch, 16 unknown, 0 extra (native unknown-lookups so far 370013) | oracle srvs=64 uavs=16 cbs=2 | native srvs=50 uavs=14 cbs=2 | shadow rs=000000004EB18460 tables=3 root-cbv=2 root-srv=0 root-uav=0 consts=0
  MISMATCH t12: oracle=(res 4ea0c050 fmt unknown 64x64) native=(res 2a195df0 fmt unknown 0x0) [oracle-res live rs=1 reg=1 seen=1 | native-res live rs=1 reg=1 seen=1] (native slot 2401fe53 copied at seq 4456 from 4ea21bea, which now holds res 4ea0c050 written at seq 336573 - AFTER the copy; ReShade's view map says 4ea0c050) => RESHADE-VIEW-RECREATED
  UNKNOWN  t10: oracle=(res 80eab9b0 fmt unknown 0x0) native=UNKNOWN [oracle-res live rs=1 reg=1 seen=1 | native-res live rs=-1 reg=-1 seen=-1] (native slot 2401fe13 is a TOMBSTONE: res 4ea45310 died after the slot was written at seq 4454; copied from 23629cca, which now holds res 80eab9b0 at seq 512308 - AFTER the copy) => RESHADE-VIEW-RECREATED
```

and the liveness gap, from run J (the same 21 slots recur in K):

```
  EXTRA    t28: oracle=ABSENT native=(res 2a1a77d0 fmt R10G10B10A2_UNORM 3840x2160) [oracle-res live rs=-1 reg=-1 seen=-1 | native-res live rs=0 reg=1 seen=1] => LIVENESS-CONFLICT
```

(named `LIVENESS-CONFLICT` by J's build; K's build names the same evidence
`RESHADE-LIVENESS-GAP`).

### 16.2 What the runs taught, in order

* **Run F's "cb: unknown" class was the ORACLE's** (SOFT in §15, now HARD): the ReShade
  backend's `constant_buffers` was an append-only list of every root CBV pushed since the
  list's Reset — 31 entries with one offset seven times at a TAA dispatch — not what was
  bound. Made current-per-root-parameter, the class vanished (run G: `cbs=2 | cbs=2` on both
  sides of every DIFF line) and the four TAA disagreements with it.
* **The 3.6M "unknown copies" were null descriptors**: UE4 fills every unbound register of a
  table from its null views. Recorded as known-null slots and copied as such:
  `unknown-copies=0` from run G on; `null-lookups` reported apart (478 987 in K).
* **The per-register disagreements beyond the shader's declarations** (run G's
  `NATIVE-MISSED`, run H's `BOTH-LIVE`: the ring buffer against some other resource at
  `t10`, `t14`, `t22`…) all carried one signature once provenance was on the line: the online
  slot was copied from an offline view the game re-created afterwards. The TAA shader reads
  `t0`-`t5`; the disputed slots were leftovers in the 64-wide table, and the native side's
  value — the copy's — is the D3D12 one.
* **`unknown-lookups` is not a defect counter.** With null slots (`null-lookups`) and
  tombstones (`dead-lookups`) reported apart, what remains (370 971 in K) is table slots of
  the online ring never written since attach — UE4's heap is 52 048 descriptors and most of it
  is stale between frames; the oracle reports nothing for those either.
* **Cost**: `[perf]` at frames 14400-15000 read 53.3 (G), 53.2 (J), 54.3 (K) fps with the
  observer on, against 55.1 with it off (run offB) — within the noise of the earlier
  55.1-55.6 baseline.

## 17. The §15 sentinel hazard, closed (2026-09-01)

The sentinel is now a hand-laid-out COM object whose vtable lives in a `VirtualAlloc`'ed
page outside the image, with three inert x64 stubs in the same page; `uninstall()` (host
detach) rewrites the vtable to the stubs, so a first-life sentinel released after ReShade's
add-on reload lands in mapped code that returns and frees nothing. The sentinel objects are
process-heap allocations for the same reason. A resource still carrying a first-life sentinel
gets a fresh one on re-registration (`SetPrivateDataInterface` replaces; the runtime's release
of the old one hits the stub).

Proven in CI on WARP and under the real ReShade 6.8.0 proxy (`test_registry_sentinel_survives_detach`,
run 33580516489 and every run since), verbatim:

```
[test] resource registry: sentinels outlive a detach without reaching this image
  vtable page 000001D4C2380000 is in module 0000000000000000; this code is in module 00007FF6AD960000
  ok: the sentinel vtable page is mapped by NO module (VirtualAlloc, not the image)
    [INFO ] resource_registry: detached - 2 live sentinel(s) made inert on page 000001D4C2380000; the page is leaked on purpose so a later Release cannot reach an unmapped image
  ok: the two sentinels were orphaned
  ok: an orphaned sentinel's Release reaches NO callback
    [INFO ] resource_registry: sentinel page 000001D4C2390000 (vtable outside the image; stubs at +0/+24/+40)
  ok: life 2 has its own vtable page; life 1's is left inert
  ok: the survivor's death fires life 2's sentinel, exactly once
```

On the box, every run G-K goes through the game's startup device recreate (the reload §14
describes) and the drive sessions do too; `sentinel-failures 0, unarmed 0` in every
`NATIVE SHADOW` line, no crash in eight sessions. What is NOT proven: a resource dying in
the window between the unload and the reload on the box — the harness proves the mechanism
(the stub is reached, nothing of the image is), the game merely did not contradict it.

## 18. NativeMode=drive on the box (2026-09-02 00:03-00:23, The Slums)

Build 3e76e9a (CI 33585956202); ReShade 6.8.0 still loaded as `dxgi.dll` and hosting;
`[STRAYDLSS] NativeMode=drive`; NGX on the native device with `ext_unhook`; the rest of
`ReShade.ini` the user's (NgxNR=1, NgxNRHook=taa, MvDispatch=2). Three sessions: driveB
(build 5c2a78e, 00:03), offB (control, `NativeMode=off`, 00:09) and driveC (3e76e9a, 00:17).

**(1) DLSS evaluates with the native side driving.** driveC, verbatim:

```
NativeMode=drive ([STRAYDLSS] NativeMode, read as "drive"): native backend: mode=drive scope=all sentinel=1 target=real device=000000001FB00080 device-slots=17 list-slots=11 patches=28 increment=32. Grep 'DIFF' and 'NATIVE SHADOW'.
seam: backend is native(drive)+reshade(present). The ReShade host still delivers device/list lifetime, present, swapchain, the render-target/draw/copy taps and PIXEL pipelines; the native hooks deliver compute pipelines, pipeline binds, list resets and dispatches.
DLSS feature created: 1920x1080 -> 3840x2160, Performance, preset=13, flags=0x4b
DLSS evaluate OK: 1920x1080 -> 3840x2160 jitter=0.3594,0.3025 reset=0 preExposure=0.454
NATIVE DRIVE [frame 3600] dispatches delivered=108836 suppressed=3428 compute-pipelines delivered=34
NATIVE DRIVE [frame 9000] dispatches delivered=368036 suppressed=8828 compute-pipelines delivered=34
NATIVE DRIVE [frame 15000] dispatches delivered=656036 suppressed=14828 compute-pipelines delivered=34
[frame 3600] resolve attempts=3949 skipped_stale=0 (0.0%)
[frame 3600] NR applied=3364 refused=64 validated=1 ...
```

Same resolution, mode and preset as the ReShade-driven run (offB: `DLSS feature created:
1920x1080 -> 3840x2160, Performance, preset=13, flags=0x4b`). `suppressed` — the engine's TAA
dispatch NOT forwarded from the native `Dispatch` hook — is 14 828 over frames ~120-15000,
one per frame at 99.7%; the NR counter (`applied=3364` at frame 3600) rides on the SR
evaluates and is the evaluate cadence's second witness. The NR runtime, its codec and the
history round-trip behaved exactly as under ReShade.

**(2) The DLSS indicator** (registry `NGXCore\ShowDlssIndicator=0x400` in the prefix, verified
in `system.reg`) is on screen in every drive screenshot, bottom-left, and reads:
`DLSS v310.7.129 (nvapp_override) DX12 cubin: sm89 HDR Mode: Perf(0.5000) (1920x1080 -> 3840x2160) Net: 1920,1080` /
`Render Preset: M` / `DLSSNR v310.8.0 (default) ON | 3840x2160`. Identical text in the `off`
control — it says NGX evaluates, the log says who drives.

**(3) Image, by temporal stability.** Camera still at the save's spawn (the cat sits before
the guitarist), eight ReShade screenshots per session at 0.8 s, 45 s after `IN GAME`; the
metric is the standard deviation (x1e5) of the grey DIFFERENCE between consecutive shots
over a crop with no animated content (the shutter posters, `1400x500+1100+300`; the left
wall, `900x700+0+0`), seven pairs per session:

| session | posters: pairs | median | left wall: pairs | median |
|---|---|---|---|---|
| driveB (drive) | 953 1237 928 1437 1209 920 1071 | **1071** | 615 446 430 759 485 484 592 | **485** |
| offB (off) | 844 1041 746 905 1005 820 936 | **905** | 461 470 450 464 529 448 489 | **464** |
| driveC (drive) | 1037 888 848 1090 773 847 1012 | **888** | 460 471 441 473 466 460 441 | **460** |

The two drive sessions straddle the control (driveC is 2% below it on the posters, 1% on the
wall; driveB 18% and 5% above), so the between-session spread of the SAME mode is larger than
any drive-versus-off difference: **no evidence the native path changes the image**, at this
instrument's power (n=7 pairs per session). Viewed at 960x540, the drive and off frames are
the same scene with the same lighting; the visible differences are the cat's and the
guitarist's idle animation, which is why the crops exclude them.

**(4) Stability and cost.** All three sessions ran 240 s past `IN GAME` (frames ~2100 to
15 000-15 300) and were alive at collection; `dmesg` carries no Xid and no new NVRM line
(the one `NV_ERR_NO_MEMORY` from `_kgmmuClientShadowFaultBufferPagesAllocate` is stamped
79134 s of host uptime, two hours before the first session of this batch). `[perf]` at
frames 13200-15000: driveB 53.6-54.3 fps, driveC 53.5-54.2, offB 55.0-55.1 — the drive
sessions sit ~1.5 fps (~3%) under the control, inside the 53.2-55.6 band the observer runs
occupy. Three `ERROR` lines per session, all the expected `[pre-NGX] vkd3d ID3D12DeviceExt slot 8
is HOOKED BY RESHADE` trio that `ext_unhook` answers.

`NativeMode=off` was restored in `ReShade.ini` after driveC.

## 19. The UE4SS plugin host on the box (2026-09-02 01:16-02:49): what runs, what does not

Stage 4: `mods/StrayDLSS` 0.1.0 as the host, launched WITHOUT ReShade (`WINEDLLOVERRIDES="dxgi=b;dwmapi=n,b"`
— the `dxgi=b` matters: merely dropping the `dxgi=n,b` override left ReShade's app-local
`dxgi.dll` loading, measured at 01:20 by a fresh `ReShade.log`; Proton's own default for dxgi is
native-first). Six launches, each read from `stray-launch-verdict.txt` (tools/launch-stray.sh).

**Two crashes in `start_mod`, both root-caused from the Proton log's `virtual_unwind` backtrace:**

1. 01:20, 01:52 — `c0000005` at `MSVCP140.dll +0x12eb0` (the prefix's **2020-11-11** msvcp140,
   `native` at 0x6FFFFCE00000) from `main.dll`, before the first plugin log line. The MSVC
   17.10+ constexpr `std::mutex` constructor against an old msvcp140: `src/log.cpp` (whose
   `g_mutex` is the first lock the host takes) is compiled into `stray_dlss_core`, and
   `_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR` was defined only on `stray_dlss_native`. The add-on
   never hit it because it is /MT. Fixed globally for MSVC (`ac7e3f3`). HARD.
   (The d3d12.dll force-load from start_mod, removed in `5c0060c`, was not the cause; the
   removal stands — the probe's poll-for-d3d12 path is the measured-safe one.)
2. 02:19, 02:42 — `EXCEPTION_ACCESS_VIOLATION reading 0x0`, 61-62 s in. **SYMBOLISED AND
   ROOT-CAUSED in §20: it is a wine-builtin-DXGI-vs-vkd3d-proton mismatch caused by Config A's
   `dxgi=b`, NOT the present owner's own code.** The `main.dll +0x17DDC/+0x17EA3` frames are our
   `CreateSwapChain`/`CreateSwapChainForHwnd` hooks forwarding correctly to the original; the
   fault is INSIDE that original. Superseded by §20.

**Verified working in the plugin host (02:42 log, verbatim where it matters):**

* `host: d3d12.dll was already loaded ... first bytes 55 41 54 -> pristine (we are the first hook)`
  — no ReShade in the process; `host: D3D12CreateDevice #1 ... real device 000000001F910080
  (vtable in d3d12core.dll)`; the startup device recreate (#2, same object, facts §11) handled.
* `StrayDLSS.ini loaded (50 values)`, `NGX is ENABLED`, `DLSS evaluation is ENABLED` (after
  `533bcc4`: the ini lives at the mod ROOT, not `dlls/`; the first run looked in `dlls/`, found
  nothing, and every key silently defaulted — EnableNGX/NgxEvaluate OFF).
* `native backend: mode=drive ... device-slots=18 list-slots=11 patches=29` on the real device,
  `ext_unhook: captured 4 pristine vtable slots` (slot 8 unhooked: no ReShade, the repair is inert).
* `present owner: queue #1 ... DIRECT`, `#2 copy`, `#3 compute` from the CreateCommandQueue hook.
* The DXGI factory EXPORT hooks (`533bcc4`) see the game's factories (`patched the game's
  factory 00000000033945C0` and `...17D10690`). **Correction, same day:** those lines say
  "4 slot(s) newly hooked" because `patch_factory` counts a slot patch_slot reports as ALREADY
  ours (it returns the stored original, non-null) — a counting artefact, not evidence of a second
  vtable. Both factories share the throwaway's vtable (`00006FFFFCC113E0`), so DXVK's DXGI
  objects DO share one vtable per class like vkd3d's D3D12 ones, the throwaway patch of the first
  design already reached the game's factory, and the export hooks are redundant (harmless). The
  02:19 run — throwaway patch only — crashed at the same 61 s with the same signature, which is
  the same swapchain-creation hook faulting. SOFT->HARD downgrade of the earlier claim.
  > **CORRECTED in §20.** The vtable `00006FFFFCC113E0` this line calls "the throwaway's" is
  > **wine-builtin dxgi.dll's** (module `00006FFFFCBE0000-…FCC1B000` in the crash minidump), not
  > DXVK's. Under Config A's `dxgi=b` the whole process's DXGI is wine-builtin, and it is
  > wine-builtin's `d3d12_swapchain_create` that faults. The one-vtable-per-class observation is
  > unaffected; the DLL it names is wrong.

**Not reached:** the swapchain hook, so no `on_present`, no status heartbeat, no NGX init (frame
120 is a present count), no DLSS evaluate under the plugin. Config A and Config B verification of
the plugin remain OPEN. The box was left in the known-good state (plugin `StrayDLSS : 0`, add-on
`NativeMode=off`, ReShade loaded, original launch options) and a confirming launch reached
gameplay: `VERDICT: OK: IN GAME census=388 taa_pipelines=1`.

## 20. The 61 s swapchain crash is a wine-builtin-DXGI vs vkd3d-proton mismatch (`dxgi=b`), symbolised from the minidump (2026-09-02)

Stage 4's 61-62 s `EXCEPTION_ACCESS_VIOLATION reading 0x0` (§19 crash 2), symbolised offline from
the UE4 minidump (`Saved/Crashes/UE4CC-…EA9444C2…`, the 02:41 run) against the plugin build's
`main.pdb` and the box's own wine DLLs, with `llvm-symbolizer`. No launch was needed to root-cause
it. HARD unless marked.

**The faulting instruction.** Exception thread context: `Rip = libvkd3d-1.dll+0x3e7b0`,
`Rcx = 0`, code `c0000005` reading `0x0`. That RVA is `vkd3d_instance_get_vk_instance`, whose
entire body is `mov rax,[rcx]; ret` — it dereferences its argument. Its caller is **wine-builtin**
`dxgi.dll` `d3d12_swapchain_create+0x6eaa`, which does:

```
call vkd3d_instance_from_device   ; rcx = the D3D12 device
mov  rcx, rax                      ; rax = the vkd3d instance it returned
call vkd3d_instance_get_vk_instance ; deref rcx  -> reads 0x0, CRASH
```

`vkd3d_instance_from_device` is `mov rax,[rcx+0xed8]; ret`, and it returned **NULL**. The device
handed to it is `R14 = 0x1F910080` — our real device, `vtable in d3d12core.dll`, i.e. a
**vkd3d-proton** device. wine's own libvkd3d does not recognise a vkd3d-proton device object, so
the field it reads is not a wine-libvkd3d instance, and the next deref faults on 0x0.

**Two vkd3d/DXGI stacks are in the process, and the crash is the wrong pairing.** The three
`dxgi.dll` images are distinguishable by size and content:

| DXGI image | size | identity | D3D12 backend it bridges to |
|---|---|---|---|
| wine-builtin `dxgi.dll` | 727 289 B (`SizeOfImage 0x3b000`) | exports `d3d12_swapchain_create`, imports `vkd3d_instance_from_device` — the **wined3d / wine-libvkd3d** bridge | wine's own libvkd3d (`libvkd3d-1.dll`) |
| DXVK `dxgi.dll` (prefix `system32`, native) | 4 853 760 B | contains `DxvkContext…` | vkd3d-proton (`d3d12core.dll`) |
| ReShade `dxgi.dll` (game dir) | 5 592 064 B | "ReShade …" | chains to whichever native dxgi it finds |

In the crash run the DXGI vtable our own present owner patched was `00006FFFFCC113E0`, which lies
inside the **wine-builtin** dxgi module (`00006FFFFCBE0000-…FCC1B000` in the minidump). So the
entire process was on wine-builtin DXGI, whose D3D12-swapchain path targets wine-libvkd3d — while
the D3D12 device is vkd3d-proton. **wine-builtin DXGI cannot create a D3D12 swapchain on a
vkd3d-proton device; that is the crash, and it is independent of our hooks.** Our
`hk_CreateSwapChain`/`hk_CreateSwapChainForHwnd` frames on the stack (`main.dll +0x17DDC`,
`+0x17EA3`, symbolised to `present_owner.cpp:196` and `:207` — the lines right after each
`g_orig_*` forward) were forwarding to the correct wine original, which then faulted on its own.

**Why wine-builtin DXGI was selected: `dxgi=b`.** §19's Config A recipe launched with
`WINEDLLOVERRIDES="dxgi=b;…"`. `b` = builtin = wine's own dxgi.dll — the wined3d bridge — NOT
DXVK. The game's D3D12 is always vkd3d-proton, so forcing wine-builtin DXGI in front of it is the
mismatch. **The correct Config A keeps `dxgi=n,b` (native-first) and merely renames the game-dir
ReShade `dxgi.dll` out of the way; native resolution then falls to the prefix's DXVK
`system32\dxgi.dll`, giving DXVK DXGI + vkd3d-proton, the compatible pairing, with ReShade absent.**
UNCONFIRMED until a launch shows which module provides dxgi and that the swapchain is created.

**A separate, real latent bug the same investigation found, and FIXED.** The present owner stored
the four `CreateSwapChain*` originals in single globals. Two distinct DXGI vtables can coexist in
one Proton process (wine-builtin, and DXVK/ReShade), each with its own originals; a global collapses
them, and a re-entrant `CreateSwapChain -> CreateSwapChainForHwnd` on one class then forwards into
the OTHER class's implementation — the same NULL-deref shape. Fixed by resolving every original
PER-VTABLE from the `self` handed to the hook (`vtable_patch::original_for`), and by restructuring
the swapchain interception to be **lazy**: the creation hook now only records the returned pointer,
patches that object's `Present`/`ResizeBuffers` slots, and returns — with a thread-local depth guard
so DXVK's `CreateSwapChain`→`CreateSwapChainForHwnd` re-entry notes the swapchain once, at the
outermost call. Every use of the object (`QueryInterface` for SwapChain3/1, the queue pick,
`GetDesc`/`GetBuffer`, `Present1`/`ResizeBuffers1` patching, `report_back_buffers`) moves to the
FIRST `Present`, when the object is fully built. This removes all work against a half-constructed
swapchain and the global-collision surface. It does NOT by itself fix the wine-builtin mismatch —
only the DXGI-provider change does — but it is required for the DXVK path to be robust.
Tested in the WARP harness (`original_for` resolves per-vtable and returns null for an unpatched
slot). HARD (code + symbolisation).

**CONFIRMED LIVE (2026-09-02, build `f97c2f0`, Config A: ReShade `dxgi.dll` renamed `.off`,
`dxgi=n,b` kept).** The plugin log, verbatim where it matters:

```
present owner: factory 000000000161E8A0 (class implemented by C:\windows\system32\dxgi.dll) 4 CreateSwapChain* slot(s) patched
present owner: swapchain 000000000159C860 via CreateSwapChain device-arg=0000000001669DC0 recorded; 2 Present/Resize slot(s) patched. QI/queue/back-buffers deferred to first Present.
present owner: swapchain 000000000159C860 FINALISED at first Present (SwapChain1=1 SwapChain3=1) device-arg=0000000001669DC0 -> the SAME queue 0000000001669DC0 (ok); 2 extra slot(s) patched
present owner: FIRST Present delivered on_present (queue=... present_list=... back_buffer=...) - the frame boundary is live
DLSS feature created: 1920x1080 -> 3840x2160, Performance, preset=13, flags=0x4b
DLSS evaluate OK: 1920x1080 -> 3840x2160 jitter=0.3750,0.0556 reset=0 preExposure=0.360
```

The DXGI provider is now DXVK's `system32\dxgi.dll` (not wine-builtin, not game-dir ReShade); the
swapchain is recorded lazily and finalised at first Present; `on_present` fires; DLSS creates and
evaluates. The game ran **190 s+** with `native_drive_dispatches` climbing past 430 000 — the 61 s
crash is gone. The session's launcher verdict was nonetheless **FAILED**: the menu-driving input
(Enter on the sysrq keyboard node) never advanced the game past an early screen (shader census
stuck at **34**, far below the ~110 interactive main menu), so it never reached gameplay. That is a
launcher/input problem in the no-ReShade config, NOT the plugin. Config A "the plugin runs DLSS with
no ReShade" is PROVEN; "drives to gameplay unattended" is still OPEN, and Config B is untested.

## 21. Box operating discipline: the box epoch, and silent waits (2026-09-02)

Two rules earned when a session assumed the box was fine across a container restart. Both are
operational, not rendering facts, but they belong with the box traps in §2.11 / CLAUDE.md §5.

* **The box epoch. Read it at the START of a session and BEFORE every box action, and compare.**
  ```
  ssh -o ConnectTimeout=10 root@192.168.0.210 'pct status 113; pct exec 113 -- cat /proc/sys/kernel/random/boot_id /proc/uptime'
  ```
  `boot_id` is a random UUID minted at each container boot; a low `uptime` confirms a recent one.
  **If `boot_id` differs from the one recorded at session start, EVERY box-state assumption is void**
  — staged `/tmp` files (`launch-stray.sh`, `inject.py`) are gone, Steam has freshly started, there
  is no game and no heartbeat — and the state must be re-established from scratch before anything
  else. A path or a process that was there a minute ago can be absent after a restart nobody told
  you about.
* **A silent wait is never evidence that anything is running.** The launch script is the component
  that waits, and it prints a progress line at least every ~10 s; an `ssh` call of YOURS that
  produces no output for 60 s is a hung or dead container, not a running game. Give every box `ssh`
  `-o ConnectTimeout=10 -o ServerAliveInterval=15 -o ServerAliveCountMax=2`, so a container that
  restarts mid-call KILLS the call instead of freezing it until the tool cap. Never sit out a
  multi-minute wait with no re-check; poll the box's own evidence (heartbeat age, plugin log,
  verdict file) on a short interval and read what it actually says.
* **"Stray is already running" requires a heartbeat under 30 s old**, never the mere presence of
  the process (`launch-stray.sh` enforces this; comment added 2026-09-02). Before any box action,
  check and log: the game process, the heartbeat age, the reaper, and who launched it.

## 22. The native host's shader census is compute-only; the >=300 in_game gate is structurally false (2026-09-02)

Measured live in Config A (build `f97c2f0`), user in gameplay to frame 20700+ with DLSS SR + NR
running, heartbeat read `shader_census=34 compute_pipelines=34 taa_pipelines=1 in_game=0`.

**The census counts distinct COMPUTE shaders only.** The native backend feeds the application's
`on_pipeline` from `store_pipeline`, called by `hk_CreateComputePipelineState` and the stream
`hk_CreatePipelineState` (which extracts only the CS subobject) — there is no graphics-PSO hook.
So `distinct_shader_hashes` holds ~34 in The Slums, where the ReShade host's `init_pipeline` saw
ALL pipelines (PS + CS), ~390 in The Slums and ~728 in the apartment. HARD.

**Consequence:** the heartbeat's `in_game = census >= 300` (calibrated for the add-on) can never
fire under the host, so the launcher's "reached gameplay" gate was structurally false — NOT an
input failure (the earlier "menu-drive input never advanced" reading in §20 was this gate, not
the pad). `in_game` is now `taa_pipelines>=1 && dispatches advancing` — "DLSS is live and
driving", which is what an unattended A/B needs. It is not a strict menu-vs-gameplay classifier:
the menu also runs the TAA pass (CLAUDE.md §5), and the host does not sample the §2.4 depth-range
gate that would separate them. Whether to teach the host graphics PSOs (restoring a menu/gameplay
census step) or add the depth gate is deferred — neither is needed for the perf work.

Dispatch-side visibility is unaffected: the diff observer counted 126/126 TAA dispatches, and
DLSS drives every frame; only the pipeline-CREATION census is compute-only.

## 23. Launch channel and the reaper wedge (2026-09-02)

Measured while driving Config A unattended. Operational, belongs with §2.10/§21.

* **The reliable launch channel is the steam.pipe URL, not CEF `RunGame`.** After Steam restarts
  inside the gamescope session, `SteamClient.Apps.RunGame("1332010","",-1,100)` via CEF returns
  normally but produces NO `content_log.txt` state change (measured: five calls, nothing), while
  `echo steam://rungameid/1332010 > /home/deck/.steam/steam.pipe` launches first try. The FIFO
  blocks with no reader, so wrap it in `timeout 5`. `launch-stray.sh` now uses the pipe first and
  CEF as fallback (`ask_steam_to_launch`).
* **A hard-killed game (or a dismissed UE4 fatal dialog) leaves a `reaper SteamLaunch
  AppId=1332010` process, and Steam then silently ignores every launch until it is killed**
  (§2.10, confirmed again: killing `pgrep -f "reaper SteamLaunch AppId=1332010"` un-wedged a
  launch that had been refused for minutes). This is the FIRST thing to check when a launch is
  ignored — before any Steam restart. Restarting Steam to clear it churns the whole session
  (four restarts in one hour desynchronised the compositor); prefer the reaper kill.
* **A launcher wait on the plugin heartbeat is doomed if no host is active.** With `StrayDLSS : 0`
  AND `dxgi.dll` renamed `.off`, there is no plugin and no ReShade, so nothing writes
  `stray-dlss-status.txt`; a heartbeat wait then always times out. A control arm with the plugin
  off needs a host-independent readiness signal (the game process, or a UE4SS-side probe), not the
  plugin's own heartbeat.

## 24. The present owner is NOT the frame-pacing cost — measured, hypothesis refuted (2026-09-02)

The perf instrumentation (`kPresentOwner`, `kPresentWait`, the 1 ms histogram) shipped and ran in
The Slums gameplay under the plugin host (DLSS SR + NR on, build with §22's gate). Verbatim:

```
[perf] frames 37200-37800: 52.4 fps avg (19.1ms), worst 40.4ms | p50 18ms p95 24ms p99 26ms p99.9 38ms | frames>16ms 501 >33ms 3
[perf] our CPU/frame: intercept 1.49ms (8%), mv_resolve 0.02ms, gbuf_resolve 0.00ms, ngx_sr 0.19ms (1%), ngx_rr 0.00ms (0%), ngx_nr 0.43ms (2%), restore 0.01ms - total 2.15ms (11% of 19.1ms)
[perf] present owner/frame: mechanics 0.012ms (0%), fence-wait 0.000ms (0%)
```

* **The present owner's per-present ring work is 0.012 ms and the fence never waits (0.000 ms).**
  The hypothesis that its `allocator/list Reset + queue Signal` every present, or a cross-frame
  fence stall, caused the plugin-vs-add-on gap is REFUTED. Do NOT remove that work as an
  optimisation — it costs nothing. (It is still correct to make it lazy for cleanliness, but not
  for perf.)
* Our total CPU is 2.15 ms/frame (11% of a 19 ms frame): intercept 1.49, NGX NR 0.43, NGX SR
  0.19. That is the same order as the add-on's timed buckets (the coordinator's ~2.2 ms), so the
  remaining plugin-vs-add-on gap (~52 vs ~57 fps in this scene) is NOT in our timed CPU and NOT in
  the present owner — it is elsewhere (GPU-side, or an untimed native-hook path such as the
  per-dispatch `SetDescriptorHeaps`/table resolve over UE4's 500k-descriptor heap). The histogram
  shows the hitches are rare (p99 26 ms, only 3 frames >33 ms per 600), so "lags off" is a
  tail-latency phenomenon, not a mean regression. Next: an A/B with the traverse, comparing the
  histogram tails, and GPU timestamps if the CPU stays exonerated.
* **All menu figures are void** (58-63 fps p99 19 ms were the MAIN MENU, a different TAA
  permutation and scene-colour format, §5). Every number above is gameplay in The Slums.

## 25. The checkpoint-reload crash is in UE4SS's ProcessEvent path, not the DLSS plugin (2026-09-02)

Reproduced deterministically: in-game pause menu -> RELOAD LAST CHECKPOINT -> confirm YES (the
confirmation defaults to NO; RIGHT then ENTER selects YES). The frame freezes, a UE4 "Fatal
error!" dialog appears (process stays alive under it; ENTER dismisses it and the process then
exits), and a crash dir is written. `EXCEPTION_ACCESS_VIOLATION reading address 0x0000000000000270`.

Symbolised from the minidump's portable callstack (UE4 already resolves module+offset):

```
Stray-Win64-Shipping +0x2b3e0ef   <- FAULT, reads [null + 0x270]
Stray-Win64-Shipping +0x2fa3da5
Stray-Win64-Shipping +0x2f48ac2
UE4SS                +0x4464a5     <- UE4SS ProcessEvent hook (dispatch to mods)
Stray-Win64-Shipping +0x2f33e66
Stray-Win64-Shipping +0x2f51c72
Stray-Win64-Shipping +0x2b2d1b3
UE4SS                +0x446175     <- UE4SS ProcessEvent hook (outer)
Stray-Win64-Shipping +0x8322a1
Stray-Win64-Shipping +0x838c3f
```

* **`main.dll` (the DLSS plugin) is NOWHERE on the crash stack.** The fault is UE 4.27 engine
  code dereferencing a null `UObject` at offset 0x270, reached through UE4SS's ProcessEvent hook
  (`UE4SS+0x446xxx`, the trampoline that dispatches UFunction calls to mods — a Lua mod like
  StrayTriggers). The plugin only hooks D3D12/DLSS and never touches UObjects or ProcessEvent, so
  it cannot be on this path. This is the UE4SS mod layer reacting to the level teardown a
  checkpoint reload performs, not the DLSS work.
* **CONTROL, stated honestly as UNCONFIRMED.** A first attempt to reload with the plugin off was
  INVALID — `StrayDLSS : 0` in mods.txt did NOT stop UE4SS loading the C++ mod (a status heartbeat
  still appeared), so DLSS was still on; the crash there proves nothing. To disable the plugin for
  real the DLL must be renamed (`dlls/main.dll` -> `main.dll.OFF`), which was done, but the
  DLSS-off reload run did not complete (the box launch wedged). The callstack is the load-bearing
  evidence; the control run is still owed. The right control is StrayTriggers on / plugin DLL
  renamed off, and then StrayTriggers off, to pin whether it is UE4SS itself or a specific Lua mod.

## 26. The render-host A/B: plugin vs ReShade add-on, checkpoint-reload + traverse, five arms (2026-09-02 13:31-13:54)

The user's protocol, run with the new unattended tooling (`tools/launch-stray-safe.sh`,
`stray-bench.sh`, the in-engine `StrayProbe`): per arm ONE launch to gameplay in The Slums
(slot 1), then three cycles of {reload last checkpoint, settle, 15 s traverse: ARROW UP held,
LEFT/RIGHT alternating every 3 s}. The instrument is the engine's own `GetFrameCount` sampled at
4 Hz on the game thread by the probe — host-independent and identical in every arm. The mod set
is identical in every arm (StrayTriggers 0, StrayFur on with the fix below, StrayProbe on);
only the render host changes. Rows verbatim from `stray-bench.csv`
(`time,label,avg fps,slowest 0.25 s bucket fps,hitch buckets,buckets,worst sampled frame ms,elapsed s,map`):

```
13:31:46,baseline-nohost-1,141.3,113.5,0,57,12.4,15.1,BaseMap
13:32:25,baseline-nohost-2,149.4,113.4,0,57,11.5,15.1,BaseMap
13:33:04,baseline-nohost-3,136.8,98.6,0,57,24.3,15.1,BaseMap
13:37:24,reshade-sr-1,116.6,79.2,0,57,11.5,15.1,BaseMap
13:38:03,reshade-sr-2,117.5,68.0,0,57,11.2,15.1,BaseMap
13:38:42,reshade-sr-3,117.5,67.8,0,57,14.2,15.1,BaseMap
13:42:04,reshade-sr-nr-1,54.7,34.1,0,57,30.1,15.0,BaseMap
13:42:43,reshade-sr-nr-2,54.4,37.9,0,57,27.9,15.0,BaseMap
13:43:22,reshade-sr-nr-3,55.7,49.2,0,57,26.9,15.0,BaseMap
13:45:26,plugin-sr-1,95.0,52.8,6,57,28.2,15.1,BaseMap
13:46:06,plugin-sr-2,96.8,41.5,8,57,20.8,15.1,BaseMap
13:46:46,plugin-sr-3,97.2,49.1,10,57,17.1,15.1,BaseMap
13:49:00,plugin-sr-nr-1,53.5,30.1,1,57,30.2,15.1,BaseMap
13:49:40,plugin-sr-nr-2,54.5,45.4,0,57,24.4,15.0,BaseMap
13:50:20,plugin-sr-nr-3,54.7,37.7,0,57,22.5,15.0,BaseMap
13:52:43,plugin-reshade-1,53.6,41.7,0,57,25.6,15.1,BaseMap
13:53:22,plugin-reshade-2,52.3,34.1,0,57,60.6,15.0,BaseMap
13:54:02,plugin-reshade-3,54.3,37.9,0,57,39.8,15.0,BaseMap
```

| arm | host | avg fps (3 runs) | slowest 0.25 s bucket | hitch buckets /57 | worst frame ms |
|---|---|---|---|---|---|
| baseline | none (no ReShade, no plugin) | 141.3 / 149.4 / 136.8 | 113.5 / 113.4 / 98.6 | 0 / 0 / 0 | 12.4 / 11.5 / 24.3 |
| A | ReShade add-on, SR only | 116.6 / 117.5 / 117.5 | 79.2 / 68.0 / 67.8 | 0 / 0 / 0 | 11.5 / 11.2 / 14.2 |
| B | ReShade add-on, SR + NR | 54.7 / 54.4 / 55.7 | 34.1 / 37.9 / 49.2 | 0 / 0 / 0 | 30.1 / 27.9 / 26.9 |
| C | plugin alone, SR only | 95.0 / 96.8 / 97.2 | 52.8 / 41.5 / 49.1 | **6 / 8 / 10** | 28.2 / 20.8 / 17.1 |
| D | plugin alone, SR + NR | 53.5 / 54.5 / 54.7 | 30.1 / 45.4 / 37.7 | 1 / 0 / 0 | 30.2 / 24.4 / 22.5 |
| E | plugin + ReShade loaded (Config B), SR + NR | 53.6 / 52.3 / 54.3 | 41.7 / 34.1 / 37.9 | 0 / 0 / 0 | 25.6 / 60.6 / 39.8 |

**Readings (HARD, n=3 per arm, one process per arm, 9 reloads survived under the plugin):**

* **What SR and NR cost.** Against the 141 fps no-host baseline, DLSS SR under the add-on costs
  ~24 fps (141 -> 117, and the game renders at 50% scale so this is the upscaler's own GPU
  time plus the interception). **NR costs another ~62 fps (117 -> 55): feature 18 halves the
  frame rate** under EITHER host. NR is the bottleneck of the user's usual configuration, not
  the host.
* **With SR + NR (B vs D): parity.** 54.7/54.4/55.7 against 53.5/54.5/54.7 — same avg, same
  worst frames, hitch buckets 0-1. Config B (E) is the same again. The user's "plugin is
  slower" is not visible in this scenario with NR on.
* **With SR only (A vs C): the plugin is ~20 fps (17%) slower AND hitches.** 117 -> 96 avg,
  slowest bucket 68-79 -> 42-53, and **6-10 hitch buckets out of 57 where the add-on has 0.**
  This is the one real deficit, and it is masked whenever NR is on because NR's GPU time
  dominates.
* **Where the plugin's SR-only deficit is NOT (plugin log, arm C, cross-referenced):**
  `present owner/frame: mechanics 0.012ms (0%), fence-wait 0.000ms` — the present owner ring
  is exonerated again (§24). Our timed CPU is `intercept 1.65ms, ngx_sr 0.18ms, total 1.87ms`
  — 22% of an 8.5 ms frame, but the add-on's timed buckets are the same order and it does not
  lose 20 fps. So the loss is in the UNTIMED native-hook paths that only the plugin has: the
  per-call `Create*View` / `CopyDescriptors` shadow (exclusive `shared_mutex` per descriptor
  write, `g_by_resource` push_back per copy), `note_heap_bound` (`GetDesc` on every
  `SetDescriptorHeaps`), and the per-dispatch table resolve over UE4's 500k-descriptor heap —
  the "descriptor_shadow" suspect in the coordinator's list, now the only one left standing.
  The hitch buckets (6-10/57) fit the same suspect: an unbounded `unordered_map`/`vector`
  growing under rehash. UNCONFIRMED which of these; a `kDescriptorShadow` bucket was NOT added
  (the user called a stop after this A/B), and that is the first thing to time next.
* **The wine `nvapi status -5` lines** (`NvAPI_D3D12_GetCudaIndependentDescriptorObject`) appear
  under the plugin with `ext_unhook` inert (slot 8 unhooked, no ReShade) at a steady trickle;
  DLSS creates, evaluates and the indicator is on screen, so they are not the §1 ReShade
  ext-vtable failure. Their cost and meaning are UNCONFIRMED.

**The checkpoint-reload crash is SOLVED and was never DLSS (coordinator's bisection, same day).**
With no render host at all: StrayFur on -> `EXCEPTION_ACCESS_VIOLATION reading 0x270` on every
reload; StrayFur off -> clean, with StrayTriggers on or off. Cause: StrayFur's
`ExecuteWithDelay`/`LoopAsync` callbacks ran on a UE4SS thread and touched UObjects during the
level teardown. Fixed on `dualsense` (`8b401b6`: every engine call via `ExecuteInGameThread`),
verified with fur on across two reload cycles, and confirmed here by nine reloads under the
plugin without a crash. §25's reading (UE4SS ProcessEvent path, `main.dll` absent) was right;
the specific mod was StrayFur.

**Config B is CONFIRMED working:** ReShade loaded as `dxgi.dll`, its add-on `.disabled`, the
plugin driving — `present owner: factory ... (class implemented by ...\Win64\dxgi.dll)` (ReShade's
proxy factory), `DLSS feature created: 1920x1080 -> 3840x2160`, three reload cycles, no crash,
parity with the other SR+NR arms.


## 29. The Config-B flicker: the online heap is created through ReShade's proxy device (2026-09-02)

The user saw the image and the DLSS on-screen indicator flicker in sync under Config B (plugin
with ReShade loaded on top, add-on `.disabled`), and NOT under Config A (plugin alone). The
indicator is drawn by the NGX evaluate, so a flicker frame is a frame with no evaluate: our hook
refused the pinned TAA dispatch and the engine's own TAA ran. Measured, and it is a number.

The signal: `NATIVE DRIVE ... suppressed=` (engine-TAA dispatches replaced with DLSS) per bench
window, as drive_ratio = suppressed-delta / present-delta:

| arm | shadow | drive_ratio | still shadow-crop diff x1e3 |
|---|---|---|---|
| D (plugin alone) | fast | 1.00 | steady ~1.5 |
| E (plugin+ReShade), before | fast | ~0.81 | alternates 1.5 <-> 20 (flicker) |
| E, ShadowMode=debug | debug | 1.00 | flat ~1.5 |
| E, fast + overflow (fix) | fast | 0.999 / 1.000 / 1.000 | ~4 + transient, no sustained alternation |

So ~1 frame in 5 under Config-B fast was undriven - the engine TAA ran, the indicator blinked,
and shadow regions (denoised differently by DLSS vs the engine's TAA) alternated. resolve_failed
read 0 throughout: it was a resolve MISS, not an error - the pinned pass' depth/velocity/colour
resolved to unknown, the gate refused ("depth or velocity SRV is missing or not known live"), and
that path does not increment resolve_failed.

The gap, named by the orphan counters (this fix's diagnostics), Config-B fast:
`fast heaps: create=29 bind=0 | ORPHANS view=0 copy-src=0 copy-dst=3.86M overflow-live=61194`.
Every view creation and every copy SOURCE fell in a heap the fast path registered at
CreateDescriptorHeap; every copy DESTINATION did not. That destination is UE4's big online
shader-visible descriptor heap, into which it copies thousands of descriptors a frame - and under
ReShade it is created through ReShade's PROXY device, which our real-device CreateDescriptorHeap
hook never sees. So the online slots the pinned TAA reads were never in the flat array. Config A
has no proxy, so all ~30 heaps are ours and the orphan count is 0 (HARD).

The proper fix: the fast path's overflow map. A view/copy/lookup whose handle is in no registered
heap now goes to a sharded overflow map (touched ONLY by orphans, so Config A never locks there)
instead of being dropped. The pinned TAA then resolves its online inputs from the overflow and
drives every frame: drive_ratio 0.999-1.000, hitch buckets 0, under Config-B fast. The overflow's
live size (61 194 = the online heap) and per-site orphan counts are in the NATIVE SHADOW GROWTH
line; a proxy-device CreateDescriptorHeap patch that registers the online heap into the flat array
(unwrapping ReShade's heap to the real pointer) would move those to the flat path and drop
overflow-live to 0 - the remaining purely-perf refinement, since Config B is NR-bound (~52 fps)
and the overflow's lock cost there is invisible.

Interim shipping choice: ShadowMode=auto (dc29457) selects DEBUG under ReShade, which was already
1.00 and flat and carries no overflow; the overflow makes FAST correct under ReShade too (drive
1.0). Both fix the user's flicker. Config A is unaffected: fast, no overflow, no orphans, drive
100%.


## 30. Perf work after the shadow rewrite: where the plugin stands vs the add-on (2026-09-02)

The SR-only deficit (facts §26: plugin ~20 fps under the add-on with 6-10 hitch buckets) was
hunted with the per-call-site [perf] buckets, on the user's recorded traverse, 3 reload+traverse
cycles per arm. Changes, each measured, one at a time, CI green before each deploy:

| change | arm C avg (recorded) | hitch buckets | what the buckets named |
|---|---|---|---|
| baseline (sharded-only, pre-work) | 52-60 | 4-8 | shadow-copy 6-63 ms/frame - the reverse-index push_back |
| drop the reverse index (generation liveness) | ~57 | 6-8 | shadow-copy still 6-63 ms - the exclusive lock |
| shard by address + range copies | ~85 | 0-1 | shadow-copy 5-14 ms - per-call std::vector allocs |
| allocation-free stack-chunked copy | ~87 | 0 | shadow-copy 3.6-11 ms - the sharded map itself (hash+lock) |
| root_shadow per-list shared_mutex | ~87 | 0 | root-bind 0.5->0.9 ms (the second, smaller convoy) |
| **fast flat lock-free shadow** (default) | **~104** | **0** | shadow-copy 1-3 ms, hitches gone - the parity change |

against **add-on SR-only (arm A) ~113 fps / 0 hitches**. So the fast shadow closed most of the
gap (52 -> ~104) and eliminated the hitches; ~9% remains. With SR+NR (arms B/D/E) everything is
~52 fps (NR-bound) and at parity already (facts §26).

**Two caveats on the last rows, stated honestly:**

1. **The overflow map (§29) regressed Config A to ~88 fps** because note_copy_range routed every
   descriptor through read_slot/write_slot and bumped one global atomic per descriptor (7 000x a
   frame across every RHI thread). Fixed by inlining the both-slots-flat case and batching the
   counter (bde9184); Config A's orphan count is 0 so it never touches the overflow.
2. **SUSPECT - host CPU contention, not our code (see §31).** Every bench row in this section
   taken after ~16:30 today is INVALID: the Proxmox host running the SteamOS LXC was CPU-starved
   by a neighbour tenant (load 50 on 32 cores), which inflates the plugin's CPU-side cost
   specifically. The "100/86/84" decline, the overflow "regression", and the "noregress"/"final"
   rows are all suspect and must not be used to gate anything. The thermal wording here is
   WITHDRAWN (no GPU throttle reason was ever active). The only trustworthy rows above are the
   morning ones (52-60 -> ~104 progression, and add-on ~113) taken before the contention; the
   real A-vs-C comparison must be re-run on a quiet host, guarded by the load check (§31).

**The remaining ~9% and the next levers (per the buckets, Config A fast):** native hooks total
3.5-5.5 ms/frame summed over threads - shadow-copy ~3 ms (7 000 flat atomic copies a frame, each
a slot_for binary search + 3 atomic stores), root-bind ~1 ms (a shared_mutex map lookup per
SetComputeRoot*, 700-860/frame), resolve ~0.6 ms. The obvious next pass is a thread-local cache
of the last (list -> ListState*) so consecutive SetComputeRoot* on one list skip the map lookup,
and of the last heap in slot_for so a run of copies into the online heap skips the binary search -
both target the two named buckets with no lock. drive_ratio is 0.999-1.000 and orphans 0 in every
Config A arm, so correctness is not in question; this is pure CPU shaving. Deferred: it wants a
stable box and is its own pass with its own table.


## 31. The SR-only "regression" was box degradation, not the overflow - bisected (2026-09-02)

§30 blamed the overflow map for arm C falling from ~104 to ~86 fps across a session. Bisected by
build, same box, back to back, and it is REFUTED. The real cause is the box's own baseline
dropping system-wide over a long uptime.

The user's control first: rerun arm A (the ReShade ADD-ON, SR only - carries none of our shadow
code) in the current box state. It read 82.9 / 100.2 / 101.6 (cycle 1 is shader warmup; steady
~100), down from 113-117 at 13:37 and 110-117 at 14:25 the same session. A restart of Steam did
not restore it (93.3 / 95.7 / 97.0 after). So a build with zero shadow code lost ~16%, across
game and Steam restarts - the box changed, not our code.

The bisection, arm C, both builds deployed and measured back to back on that degraded box:

| build | DLL | arm C 3 cycles | drive_ratio |
|---|---|---|---|
| clean box, this morning | 06b8ff7f | 104.9 / 104.2 / 102.2 | - |
| c90be36 (pre-overflow, flat shadow) | 06b8ff7f | 84.5 / 81.0 / 81.8 | 0.999 |
| bde9184 (overflow + inline-copy fix) | 50aaa509 | 94.9 / 98.8 / 77.8 | 0.999-1.000 |

The SAME 06b8ff7f binary that was 104 in the morning is 82 now, and the overflow build (bde9184)
is if anything HIGHER (mean ~90 vs ~82) - so the overflow is not a regression, and §30's
inline-copy "fix" was fixing a phantom (it is still correct and cheaper, just not the cause).
Both drive_ratio 0.999-1.000; correctness is intact.

**GPU is not the cause:** at rest clocks_event_reasons.active reads 0x1 (idle), and during a run
the SM boosts to 2775 MHz at 68-71 C with a 600 W board limit and no throttle reason ever
recorded. Memory 18 GB free. No leftover extra process (one Stray, the expected reaper). The
degradation is elsewhere in the 27 h-uptime session - the gamescope compositor or the driver
state - and persists across game and Steam restarts, so only a container/system reboot will
restore a clean baseline. Within-arm variance is also high on the degraded box (bde9184
95/99/78), which a clean box should remove.

**Consequence for the perf passes:** absolute A-vs-C numbers from any run after ~15:00 today are
not comparable to the morning's 104/113 and must not be used to gate pass 1. The passes (the
thread-local caches of last-list-state and last-heap, §30) are still the right next step, but on
a REBOOTED box; on the degraded box a small CPU-shaving delta cannot be resolved against ~20 fps
of box noise. The thermal explanation in §30 is withdrawn: no throttle reason was ever active;
the loss is a per-session/system accumulation, not temperature.


### 31.1 The cause was the Proxmox HOST, and the bench now guards against it (2026-09-02 ~18:00)

The "box degraded" reading above is right about the symptom and now has its cause: the **Proxmox
host** carrying the SteamOS LXC was under **CPU contention from another tenant** at the time - four
java processes at 856% / 656% / 337% / 262% CPU pushed the 32-core host to a 1-minute load average
of **50** (5-minute 61.6), ~1.6x oversubscribed, while the game wanted ~3.5 cores. The plugin's
added cost is CPU-side (the native hooks, the resolve), which is exactly what a starved host
inflates - so this is very likely the whole of the "drift" as well.

**The container's loadavg IS the host's.** An LXC shares the host kernel, so `/proc/loadavg`
inside container 113 reads the host's load (50.2 on 32 CPUs), which is precisely why the bench can
detect a neighbour's burst at all. And `/proc/stat` steal is always 0 in an LXC (it is not a VM),
so steal is not a usable signal here - loadavg is.

**SUSPECT rows.** Every stray-bench.csv row after ~16:30 today - the "drift" C rows (100/86/84),
the bisect rows (c90be36 84/81/82 and bde9184 95/99/78), the A rechecks (82/100/101 and post-Steam
93/96/97), and every "final"/"noregress"/"overflow" row - is INVALID and draws no conclusion. The
bisection §31 ran DURING the contention, which is why the same 06b8ff7f binary read 82 vs its
morning 104; it does still show the overflow build is not slower than the pre-overflow one (both
were equally starved), so the overflow is not exonerated OR convicted by it - re-run on a quiet
host.

**The guard.** stray-bench.sh now reads `/proc/loadavg` at the start of every cycle and REFUSES to
run when load1/nproc exceeds STRAY_LOAD_MAX (default 0.5; the game alone is well under 0.3), naming
the host contention. load1, load5 and nproc are written into every CSV row (gpu_sm@tempC too), so a
starved run can never again be mistaken for a code regression. Pass 1 (the thread-local caches) and
the build bisection (c90be36 vs bde9184 artifacts staged, accumulation counters at cycle
boundaries) are ready to run in one go the moment the host is quiet (load1/nproc < 0.3).

**Host scheduling change (2026-09-02, coordinator).** The Proxmox host now sets the SteamOS
container (113) to `cpu.weight 1000` and the work container (105) to 50 (live + persisted via
`pct set --cpuunits`), so under contention the game wins ~20:1 rather than splitting evenly. It
does NOT lift the pause: a loaded host still perturbs timing even when the game wins the
scheduler, so the load guard stays and benches wait for a quiet host. On release, the arm-A
add-on control is the first run - it also tells whether the weight alone restored the add-on's
morning 113-117. stray-bench.sh writes a per-cycle accumulation snapshot (NATIVE SHADOW GROWTH,
native-hooks buckets, NATIVE DRIVE, live-resource/root-signature counts) to stray-bench-accum.log
so the bisection can see whether any counter climbs monotonically while fps falls (a leak) versus
moves with the host load (contention).

## 32. Stray's build configuration: it IS Shipping — the VRAM-query storm is the D3D12 Residency Manager, not STATS (2026-09-02)

### Starting point

A same-day measurement (`VramQueryWatch`, a vtable patch on `IDXGIAdapter3::QueryVideoMemoryInfo`)
found the shipped `Stray-Win64-Shipping.exe` calling that API **~21 times per frame** — 167,146
calls in one session, all from a **single return address inside the game's own executable**,
alternating `group=0` (LOCAL) and `group=1` (NON_LOCAL), with no throttle of any kind: typical
calls cost 0.01-0.06 ms, but calls above 5 ms run a median 14.7 ms and a max 36.5 ms, arriving in
bursts of three every 11.46 s. HARD, inherited from that measurement.

A straight read of UE 4.27.2's source finds exactly ONE call site for this API in the stock
engine: `FD3D12CommandContextBase::UpdateMemoryStats()`
(`D3D12CommandContext.cpp:695`), called once per frame from `RHIEndFrame()`, gated
`#if PLATFORM_WINDOWS && STATS` and querying the **LOCAL** group only. Two predictions follow,
and BOTH are wrong against the measured shape: if `STATS` is 0 (the Shipping default), this path
should call the API **zero times a frame**; if `STATS` were 1, it should still only be **one
LOCAL-only call a frame**, not 21 alternating calls. Since the source predicts at most 1/frame
either way and the game does 21, **the user's hypothesis was that Stray is not actually a true
Shipping build** — that `STATS` (and the rest of Development/Test instrumentation) is compiled
in, which would also explain the mismatch away.

It does not. The real explanation is a second, completely different call site the source review
never reached, and it settles the Shipping question as a side effect.

### 32.1 Method

Read-only against the box (`ssh ... pct exec 113`, per this task's rules — nothing launched,
nothing written there). `strings -n 6` (ASCII) and `strings -n 6 -e l` (UTF-16LE) were streamed
directly over ssh into this session's own scratch directory, never written to the box's disk:
161,499 ASCII lines and 39,645 UTF-16LE lines from `Stray-Win64-Shipping.exe` (85,043,200 bytes,
2026-09-01). Cross-referenced against UE 4.27.2's own source — the public mirror
`AlexMercer-MA/UnrealEngine-4.27` this project already cites elsewhere, fetched via `gh api
search/code` (authenticated, so full-text code search works) and raw `curl` for exact file
content — and against `docs/game-config/` (this repo's own pak-extracted inis, §2.3.1).

### 32.2 The build IS Shipping — HARD, straight from the binary's own compiled content

`UEngine::InitEngine` (`UnrealEngine.cpp:1774-1815`) builds a table, `EngineStats`, of every
"stat" HUD command (`stat fps`, `stat unit`, etc.) as literal `TEXT("STAT_...")` strings passed
to `FEngineStatFuncs` constructors — **this table is unconditional runtime code, never dead-code
eliminated, and is completely separate from the `STATS`-gated stat SYSTEM** (§32.3). Most of its
entries are compiled in every configuration, but several are wrapped in
`#if !UE_BUILD_SHIPPING`:

```cpp
#if !UE_BUILD_SHIPPING
EngineStats.Add(FEngineStatFuncs(TEXT("STAT_Version"), ...));
#endif // !UE_BUILD_SHIPPING
...
#if !UE_BUILD_SHIPPING
EngineStats.Add(FEngineStatFuncs(TEXT("STAT_SoundMixes"), ...));
EngineStats.Add(FEngineStatFuncs(TEXT("STAT_SoundModulators"), ...));
EngineStats.Add(FEngineStatFuncs(TEXT("STAT_SoundModulatorsHelp"), ...));
EngineStats.Add(FEngineStatFuncs(TEXT("STAT_AudioStreaming"), ...));
EngineStats.Add(FEngineStatFuncs(TEXT("STAT_SoundReverb"), ...));
EngineStats.Add(FEngineStatFuncs(TEXT("STAT_Sounds"), ...));
EngineStats.Add(FEngineStatFuncs(TEXT("STAT_SoundCues"), ...));
EngineStats.Add(FEngineStatFuncs(TEXT("STAT_SoundWaves"), ...));
#endif // !UE_BUILD_SHIPPING
...
#if !UE_BUILD_SHIPPING
EngineStats.Add(FEngineStatFuncs(TEXT("STAT_UnitMax"), ...));
EngineStats.Add(FEngineStatFuncs(TEXT("STAT_UnitGraph"), ...));
EngineStats.Add(FEngineStatFuncs(TEXT("STAT_UnitTime"), ...));
EngineStats.Add(FEngineStatFuncs(TEXT("STAT_Raw"), ...));
EngineStats.Add(FEngineStatFuncs(TEXT("STAT_ParticlePerf"), ...));
#endif // !UE_BUILD_SHIPPING
```

Searched both encodings, case-insensitive, substring, over the whole binary:

| Marker (only compiled if `!UE_BUILD_SHIPPING`) | ascii | utf16 |
|---|---|---|
| `STAT_Version` | 0 | 0 |
| `STAT_SoundMixes` | 0 | 0 |
| `STAT_SoundModulators` | 0 | 0 |
| `STAT_SoundModulatorsHelp` | 0 | 0 |
| `STAT_AudioStreaming` | 0 | 0 |
| `STAT_SoundReverb` | 0 | 0 |
| `STAT_Sounds` | 0 | 0 |
| `STAT_SoundCues` | 0 | 0 |
| `STAT_SoundWaves` | 0 | 0 |
| `STAT_UnitMax` | 0 | 0 |
| `STAT_UnitGraph` | 0 | 0 |
| `STAT_UnitTime` | 0 | 0 |
| `STAT_Raw` | 0 | 0 |
| `STAT_ParticlePerf` | 0 | 0 |

**Zero of fourteen.** Every one of these strings sits in executed, non-dead-strippable code
(each is an argument to a constructor call inside a function that always runs at engine init) —
if `UE_BUILD_SHIPPING` were 0, all fourteen would be in the binary. None are. **HARD:
`UE_BUILD_SHIPPING=1` in this executable** — it is genuinely, not just nominally, a Shipping
build.

**The trap this closes.** The unconditional siblings of the same table — `STAT_FPS`,
`STAT_Summary`, `STAT_Unit`, `STAT_DrawCount`, `STAT_Hitches`, `STAT_AI`, `STAT_Timecode`,
`STAT_FrameCounter`, `STAT_ColorList`, `STAT_Levels`, `STAT_Detailed`, `STAT_NamedEvents` — ARE
present (utf16). A search for `"STAT_"` alone, without checking WHICH stat names are
Shipping-gated and which are not, finds hits either way and settles nothing. This is exactly
the "too narrow a search" trap the task brief warned about: the discriminator is not whether
`STAT_`-prefixed strings exist, but which SPECIFIC ones do.

### 32.3 STATS is off too, independently — HARD

`UpdateMemoryStats()` (`D3D12CommandContext.cpp:695-723`) is the one stock call site, and its
whole body is gated:

```cpp
void FD3D12CommandContextBase::UpdateMemoryStats()
{
#if PLATFORM_WINDOWS && STATS
	DXGI_QUERY_VIDEO_MEMORY_INFO LocalVideoMemoryInfo;
	ParentAdapter->GetLocalVideoMemoryInfo(&LocalVideoMemoryInfo);
	...
	SET_MEMORY_STAT(STAT_D3D12UsedVideoMemory, LocalVideoMemoryInfo.CurrentUsage);
	SET_MEMORY_STAT(STAT_D3D12AvailableVideoMemory, AvailableSpace);
	SET_MEMORY_STAT(STAT_D3D12TotalVideoMemory, Budget);
	...
#endif
}
```

`Stats.h`'s `#else //STATS` branch (the one that applies whenever `STATS` is 0) redefines every
one of these to nothing: `#define DECLARE_MEMORY_STAT_EXTERN(...)` and
`#define SET_MEMORY_STAT(...)` both expand to empty. So when `STATS=0` the whole function body —
`GetLocalVideoMemoryInfo`, hence `QueryVideoMemoryInfo`, included — disappears. The four stats
are declared with exact description text in `D3D12Stats.h:61-64`:

```cpp
DECLARE_MEMORY_STAT_EXTERN(TEXT("Used Video Memory"), STAT_D3D12UsedVideoMemory, STATGROUP_D3D12RHI, );
DECLARE_MEMORY_STAT_EXTERN(TEXT("Available Video Memory"), STAT_D3D12AvailableVideoMemory, STATGROUP_D3D12RHI, );
DECLARE_MEMORY_STAT_EXTERN(TEXT("Total Video Memory"), STAT_D3D12TotalVideoMemory, STATGROUP_D3D12RHI, );
DECLARE_MEMORY_STAT_EXTERN(TEXT("Texture allocator wastage"), STAT_D3D12TextureAllocatorWastage, STATGROUP_D3D12RHI, );
```

All four searched, both encodings: **zero hits for all four, in both ascii and utf16.** If
`STATS` were 1, these exact description strings would be embedded (the macro passes them
straight through as `FStartupMessages` metadata) regardless of whether the stat ever fires.
**HARD: `STATS=0`.** `UpdateMemoryStats()`'s body is dead code in this binary — it makes
**zero** calls to `QueryVideoMemoryInfo`, ever. So the measured 167,146 calls cannot be coming
from the path the source review examined, in either direction of the Shipping question. They
have to come from somewhere else.

### 32.4 The real source: the D3D12 Residency Manager — compiled in unconditionally on Windows, gated by neither `STATS` nor `UE_BUILD_SHIPPING`

`D3D12RHI.h:53-54`:

```cpp
#if PLATFORM_WINDOWS || PLATFORM_HOLOLENS
	#define ENABLE_RESIDENCY_MANAGEMENT				1
```

No `STATS`, no `UE_BUILD_SHIPPING` — this is a platform gate only, true in every Windows
configuration including Shipping. `D3D12Adapter.cpp:22-29`:

```cpp
#if ENABLE_RESIDENCY_MANAGEMENT
bool GEnableResidencyManagement = true;
static TAutoConsoleVariable<int32> CVarResidencyManagement(
	TEXT("D3D12.ResidencyManagement"),
	1,
	TEXT("Controls whether D3D12 resource residency management is active (default = on)."),
	ECVF_ReadOnly
);
#endif // ENABLE_RESIDENCY_MANAGEMENT
```

and, read once right after device creation (`D3D12Adapter.cpp:443-447`):

```cpp
#if ENABLE_RESIDENCY_MANAGEMENT
	if (!CVarResidencyManagement.GetValueOnAnyThread())
	{
		UE_LOG(LogD3D12RHI, Log, TEXT("D3D12 resource residency management is disabled."));
		GEnableResidencyManagement = false;
	}
#endif
```

`FD3D12CommandListManager::ExecuteCommandLists` (`D3D12DirectCommandListManager.cpp:548-578`,
both the single-list and the batch code path) routes every submission through it when the flag
is set:

```cpp
if (GEnableResidencyManagement)
{
	VERIFYD3D12RESULT(GetParentDevice()->GetResidencyManager().ExecuteCommandLists(
		D3DCommandQueue, Payload.CommandLists, Payload.ResidencySets, Payload.NumCommandLists));
}
else
{
	D3DCommandQueue->ExecuteCommandLists(Payload.NumCommandLists, Payload.CommandLists);
}
```

`GetResidencyManager()` wraps Microsoft's own `D3DX12Residency` helper — vendored verbatim from
`microsoft/DirectX-Graphics-Samples`, `Libraries/D3DX12Residency/d3dx12Residency.h` — and its
`ResidencyManager::ExecuteSubset` (the function this call reaches) does this, **every single time
it runs, unconditionally, with no cache and no throttle**:

```cpp
HRESULT ExecuteSubset(ID3D12CommandQueue* Queue, ID3D12CommandList** CommandLists, ResidencySet** ResidencySets, UINT32 Count)
{
	...
	DXGI_QUERY_VIDEO_MEMORY_INFO LocalMemory;
	GetCurrentBudget(&LocalMemory, DXGI_MEMORY_SEGMENT_GROUP_LOCAL);

	DXGI_QUERY_VIDEO_MEMORY_INFO NonLocalMemory;
	GetCurrentBudget(&NonLocalMemory, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL);
	...
}
// GetCurrentBudget:
void GetCurrentBudget(DXGI_QUERY_VIDEO_MEMORY_INFO* InfoOut, DXGI_MEMORY_SEGMENT_GROUP Segment)
{
	if (Adapter)
	{
		RESIDENCY_CHECK_RESULT(Adapter->QueryVideoMemoryInfo(NodeIndex, Segment, InfoOut));
	}
	...
}
```

**This is an exact shape match to the measurement**: one LOCAL call and one NON_LOCAL call —
"alternating `group=0`/`group=1`" — per invocation, with genuinely no throttle of any kind (the
source review's other correct observation, just attributed to the wrong call site). Since this
fires on every `ExecuteCommandLists` batch submitted to the direct queue, not once a frame, a
frame that submits ~10-11 batches (plausible for a UE4.27 deferred frame — shadow passes, base
pass, post-process chain, UI, each a potential flush point) produces ~20-22 calls — matching the
measured ~21/frame. **The per-invocation LOCAL+NON_LOCAL shape and the total absence of
throttling are HARD, read from source; the "~10-11 batches/frame" arithmetic that reproduces the
21/frame figure is SOFT — plausible, not independently counted on this title.**

**Direct confirmation the mechanism is live in Stray's own binary**, not just present in the
stock source: the cvar's exact name and help text are both in the exe, verbatim, utf16:

```
D3D12.ResidencyManagement
Controls whether D3D12 resource residency management is active (default = on).
```

### 32.5 A trap in the pak's own config: `BuildConfiguration=PPBC_Development`

`docs/game-config/Hk_project_Config_DefaultGame.ini:234`, inside the same
`ProjectPackagingSettings` block as `StagingDirectory=(Path="X:/Packages")`:

```ini
Build=IfProjectHasCode
BuildConfiguration=PPBC_Development
BuildTarget=Stray
StagingDirectory=(Path="X:/Packages")
```

This looks, at a glance, like direct evidence the shipped build is Development. **It is not.**
`ProjectPackagingSettings` records the editor's own "Package Project" quick-package UI state —
whichever configuration a developer had last selected there — not a log of how the actual retail
Steam build was produced. That build was almost certainly driven by a separate CI/build script
that explicitly requested `Configuration=Shipping`, which is the only way Unreal Build Tool
appends the `-Shipping` suffix to the executable name in the first place (`Stray-Win64-Shipping.exe`
is a name UBT constructs, not one a developer types). §32.2 and §32.3 read this straight out of
the binary's own compiled content and settle it independently of what this leftover editor
setting says. Two lines further down, the same block also carries
`PakFileCompressionLevel_TestShipping=5` right next to `PakFileCompressionLevel_DebugDevelopment=3`
— evidence the packaging PIPELINE handles both configurations, which says nothing about which one
produced this particular binary. **A grep for "Shipping" vs "Development" over the pak's ini
alone would have been misled by exactly this line — the binary is still the only authority.**

### 32.6 What this means for the stall, and the fix

**There is no "stat gathering" to turn off, because `STATS` was never on.** The task's second
question — can it be disabled at runtime, and would that stop the calls — doesn't apply to the
path it was asked about; that path already makes zero calls. The lever that actually matters is
`D3D12.ResidencyManagement`.

**It is `ECVF_ReadOnly`**, read exactly once, immediately after `D3D12CreateDevice`
(`D3D12Adapter.cpp:443`, quoted above) — a console command after the game is running cannot
change it. This project has already established a route for exactly this class of cvar:
`Engine.ini [SystemSettings]` is read before RHI init (`GSystemSettings.Initialize()` at
`LaunchEngineLoop.cpp:2248`, versus `RHIInit()` at `:2597` — **HARD, confirmed in source, that
the ordering is right**), and CLAUDE.md §1 records this exact mechanism working for another
`ECVF_ReadOnly` cvar, `r.UsePreExposure`. Applying it here:

```ini
[SystemSettings]
D3D12.ResidencyManagement=0
```

is the candidate fix. **This specific cvar accepting an ini override the same way `r.UsePreExposure`
did is SOFT — by analogy to a working precedent in this project, not independently tested for
`D3D12.ResidencyManagement`.**

**What flipping it would stop.** With `GEnableResidencyManagement=false`,
`FD3D12CommandListManager::ExecuteCommandLists` falls through to
`D3DCommandQueue->ExecuteCommandLists(...)` directly at every call site quoted in §32.4 — no
residency wrapper, no budget query, at all. Since `UpdateMemoryStats()`'s path is already dead
(§32.3), this should collapse the measured population to near zero: the one-shot LOCAL query at
adapter enumeration (once, at startup) and an Intel-only NON_LOCAL heuristic (irrelevant on this
NVIDIA target) are the only calls left, both negligible.

**What it costs.** The residency manager exists to page D3D12 resources under VRAM pressure;
disabling it removes that. On this target the trade looks safe: the same session's VRAM reading
was 16,285 of 24,564 MiB free — comfortably clear of anything the residency manager would need
to react to. That is a snapshot, not a guarantee for every scene in the game.

**Acceptance test, not run here** (this task is read-only and may not launch the game): the same
method already established for the stall — `grep -o 'f=[0-9]* t=[0-9.]* frame [0-9.]* ms'
stray-dlss-plugin.log`, medians of at least 15 samples, before/after — plus `VramQueryWatch`'s own
periodic report, which should show total calls collapse from ~21/frame toward ~0.

### 32.7 What is still UNCONFIRMED

* Whether `Engine.ini [SystemSettings] D3D12.ResidencyManagement=0` actually takes effect here —
  reasoned by analogy to `r.UsePreExposure` and by the confirmed init ordering, not directly
  tested for this cvar.
* The exact count of `ExecuteCommandLists` batches Stray submits to the direct queue per frame —
  the "~10-11 batches -> ~21-22 calls" arithmetic that reproduces the measured rate is plausible,
  not independently counted.
* Whether disabling residency management has any cost beyond the currently-ample VRAM headroom —
  a more VRAM-constrained scene, or GPU contention from another tenant on the shared box (§31.1),
  could change the trade.
* `STATGROUP_D3D12RHI`'s own group-description text was not independently located in source; the
  `STATS=0` conclusion in §32.3 rests on the four member stats' exact description strings plus
  the §32.2 `EngineStats` table, not on every possible `STATS`-only string in the engine.
