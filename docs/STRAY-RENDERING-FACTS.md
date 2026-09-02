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
2. 02:19, 02:42 — `EXCEPTION_ACCESS_VIOLATION reading 0x0`, 61-62 s in, UE4 dump with an empty
   callstack; the Proton log's backtrace is `main.dll +0x17DDC -> main.dll +0x17EA3 -> fault at
   0x6FFFFCA0E7B0` (a DXGI/wine builtin). `+0x17DDC/+0x17EA3` sit just past the four
   `CreateSwapChain*` hooks the log places at `+0x17870..+0x17AB0`: **the present owner's
   swapchain-creation path** (`hk_CreateSwapChain*` -> `note_swapchain` / `report_back_buffers`).
   The game's last recorded act is creating a SECOND DXGI factory (`present owner: patched the
   game's factory 0000000017D10690`), i.e. the swapchain is being created at that moment. No
   `present owner: swapchain ... via ...` line was ever written, so the fault is INSIDE that
   hook before it logs. OPEN — see the report for the candidate fix.

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

**Not reached:** the swapchain hook, so no `on_present`, no status heartbeat, no NGX init (frame
120 is a present count), no DLSS evaluate under the plugin. Config A and Config B verification of
the plugin remain OPEN. The box was left in the known-good state (plugin `StrayDLSS : 0`, add-on
`NativeMode=off`, ReShade loaded, original launch options) and a confirming launch reached
gameplay: `VERDICT: OK: IN GAME census=388 taa_pipelines=1`.
