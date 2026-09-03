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

## 32. The DLSS-G (NGX feature 11) parameter contract, read out of `nvngx_dlssg.dll` (2026-09-02, offline)

Frame generation, stage 2 of the FG plan. CLAUDE.md §5 rule: an NGX parameter block is an
untyped string->value map with no validation, so **a name the snippet does not implement is
silently ignored** — every `DLSSG.*` name the plugin writes had to be confirmed by exact
null-terminated string search over the binary that reads it BEFORE any code depends on it.
The instrument is `tools/ngx_param_names.py` (re-runnable against any copy of the DLL).

**Subject.** The box is offline (2026-09-02), so its staged `SL 2.13` copy could not be read.
The contract was read from the newest public Streamline SDK release on GitHub,
`NVIDIA-RTX/Streamline` **v2.12.0** (2026-06-23, `streamline-sdk-v2.12.0.zip`, 231 958 617 B):

| file | size | md5 |
|---|---|---|
| `bin/x64/nvngx_dlssg.dll` (release, the one the contract is read from) | 7 519 856 | `9937f1746696ebcad879ef2781343168` |
| `bin/x64/development/nvngx_dlssg.dll` | 13 959 792 | `66d30d107ca0de33cf75d1f7f329c6c5` |
| `bin/x64/sl.dlss_g.dll` (the reference CALLER, cross-check) | 612 992 | — |

**UNCONFIRMED until the box is back:** that SL 2.13's `nvngx_dlssg.dll` (7.4 MB on the box)
carries the same name set. Run `python3 tools/ngx_param_names.py <box copy> --check <the
list below>` first thing; a name that flips to ABSENT retires the corresponding write.

### 32.1 Verdicts, verbatim from the tool (release 2.12.0 snippet)

94 of the 106 names checked are PRESENT (exact `\0NAME\0`). Every one the plugin writes is in
the PRESENT set. The 12 ABSENT ones, and why each was checked:

```
  check DLSSG.EnableInterp        ABSENT   Nukem's dlssg-to-fsr3 reads it; NVIDIA forum 247260 lists it
  check DLSSG.IsRecording         ABSENT   forum 247260 calls it "really important" (2023, DLSS 3 era)
  check DLSSG.CmdQueue            ABSENT   forum 247260; sl.dlss_g.dll DOES contain it
  check DLSSG.CmdAlloc            ABSENT   forum 247260; sl.dlss_g.dll DOES contain it
  check DLSSG.FenceEvent          ABSENT   forum 247260; sl.dlss_g.dll DOES contain it
  check DLSSG.NumFrames           ABSENT   forum 247260
  check DLSSG.CameraForwardX      ABSENT   forum 247260 (the real name is DLSSG.CameraFwdX)
  check Enable.OFA                ABSENT   forum 247260 (optical flow: DLSS 3 only; DLSS 4 FG has none)
  check DLSSG.QueueSubmitCallback ABSENT   sl.dlss_g.dll contains it
  check DLSSG.SyncSignalCallback  ABSENT   sl.dlss_g.dll contains it
  check FrameInterpolation.Available ABSENT  the SDK header (v310.7.0) has NeedsUpdatedDriver / MinDriverVersionMajor / FeatureInitResult for FrameInterpolation, no Available
  check FrameGeneration.Available ABSENT   guessed by analogy with SuperSampling.Available
```

Substring search ("fragment") is also 0 for `EnableInterp`, `CmdQueue`, `CmdAlloc`, `FenceEvent`,
`Sync`, `QueueSubmit`, `IsRecording`, `NumFrames`, `OFA`, `Present`, `Flip` — the snippet does
not build those names at run time either. **Reading:** the queue/allocator/fence-event/sync
callbacks and `EnableInterp` are Streamline-internal keys shared between `sl.dlss_g` and
`sl.common` (the SL pacer and present thread live there, not in the snippet); Nukem's
replacement reads `EnableInterp` because SL's parameter object carries every key it ever set.
**The snippet itself has no present path, no queue and no pacer: `EvaluateFeature` records
interpolation work onto the command list it is handed, and presenting the result is the
caller's job.** HARD (from the strings; the OptiScaler session in CLAUDE.md §5 measured the
snippet's `Dispatch Result: Ok` through SL, which is the same call).

### 32.2 The full `DLSSG.*` name set of the 2.12.0 snippet (135 names), classified

Classification is from three sources: present in the snippet (S), present in `sl.dlss_g.dll`
(C, i.e. the reference caller sets it), read by Nukem's dlssg-to-fsr3 (N, i.e. a caller-set
value that a replacement snippet consumes). A name that is S but not C is either an OUTPUT
of the snippet or a registry/DRS-driven tuning key; a name that is C but not S is SL-internal.

**Creation-time (S+C), and the plain NGX names:** `Width` `Height` `CreationNodeMask`
`VisibilityNodeMask` (the core's own creation keys; Nukem reads `Width`/`Height` as the
swapchain size), `DLSSG.Width` `DLSSG.Height` `DLSSG.InternalWidth` `DLSSG.InternalHeight`
`DLSSG.BackbufferFormat` `DLSSG.ColorBuffersHDR` `DLSSG.DynamicResolution`
`DLSSG.MultiFrameCount` `DLSSG.MultiFrameIndex` `DLSSG.MultiFrameCountMax`
`DLSSG.UserInterfaceRecompositionEnabled` `DLSSG.MenuDetectionEnabled` `DLSSG.TargetFrameRate`
`DLSSG.StreamlineMode` `DLSSG.StreamlineVersionTag` `DLSSG.ModelVersion` `DLSSG.EvalFlags`
`DLSSG.AutomodeOverrideReset` `DLSSG.NvAppOvrAppliedVal.MultiFrameCount`
`DLSSG.NvAppOvrAppliedVal.StreamlineMode`.

**Per-evaluate resources (S+C+N), each with `SubrectBaseX/BaseY/Width/Height` siblings:**
`DLSSG.Backbuffer` (required; the FINAL colour frame, `InputBackbufferSubrect*` also exist, S only)
`DLSSG.Depth` (required) `DLSSG.MVecs` (required) `DLSSG.HUDLess` (optional, stage 3)
`DLSSG.UI` + `DLSSG.UIAlpha` (optional) `DLSSG.NoWarp` (optional) `DLSSG.BidirectionalDistortionField`
(+ `LowPrecision.Bias/Scale/IsLowPrecision`, optional) `DLSSG.OutputInterpolated` (the generated
frame, UAV; required) `DLSSG.OutputReal` (S+C+N: Nukem COPIES the back buffer into it when
provided; optional for us). `DLSSG.BackbufferFrameID` (S+C: a per-frame id, SL sets it).

**Per-evaluate constants (S+C, Nukem reads the starred ones):** `DLSSG.JitterOffsetX/Y`*
`DLSSG.MvecScaleX/Y`* `DLSSG.MvecJittered`* `DLSSG.MvecDilated`* `DLSSG.MvecInvalidValue`
`DLSSG.CameraMotionIncluded` `DLSSG.DepthInverted`* `DLSSG.OrthoProjection`* `DLSSG.Reset`*
`DLSSG.ColorBuffersHDR`* `DLSSG.CameraViewToClip`* `DLSSG.ClipToCameraView` `DLSSG.ClipToPrevClip`
`DLSSG.PrevClipToClip` `DLSSG.ClipToLensClip` `DLSSG.CameraNear`* `DLSSG.CameraFar`*
`DLSSG.CameraFOV`* `DLSSG.CameraAspectRatio` `DLSSG.CameraPinholeOffsetX/Y`
`DLSSG.CameraPosX/Y/Z` `DLSSG.CameraUpX/Y/Z` `DLSSG.CameraRightX/Y/Z` `DLSSG.CameraFwdX/Y/Z`
`DLSSG.MinRelativeLinearDepthObjectSeparation` `DLSSG.NotRenderingGameFrames`
`DLSSG.FullscreenMode` `DLSSG.OutputDisableInterpolation`. The matrices are `void*` to a
row-major float[16] (`sl_consts.h`: "All SL matrices are row-major and should not contain
any jitter offsets"; `clipToPrevClip = clipToView * viewToViewPrev * viewToClipPrev`,
`prevClipToClip = clipToPrevClip.inverse()`).

**Snippet outputs / self-populated (S only, or S+N where Nukem's replacement SETS them):**
`DLSSG.MustCallEval` `DLSSG.BurstCaptureRunning` `DLSSG.ReflexWarp.Available`
`DLSSG.GetCurrentSettingsCallback` `DLSSG.EstimateVRAMCallback` (the four the snippet's
`PopulateParameters_Impl` writes for SL to read; Nukem's `NVSDK_NGX_D3D12_PopulateParameters_Impl`
sets exactly these) plus `DLSSG.IndicatorLevel` `DLSSG.UseReflexMatrices` `DLSSG.AsyncCreateEnabled`
`DLSSG.LinearizedDepth_Scale` `DLSSG.LinearizedDepth_NearFarPartition`
`DLSSG.ResourceAlwaysProvidedFlags` `DLSSG.ResourceNeverProvidedFlags` `DLSSG.OutputRealSubrect*`
`DLSSG.OutputInterpolatedSubrect*` `DLSSG.InputBackbufferSubrect*` (S only; DRS/registry or
internal). `DLSSG.UserDebugText` is in the development snippet and `sl.dlss_g` only.

**SL-internal (C only):** `DLSSG.EnableInterp` `DLSSG.CmdQueue` `DLSSG.CmdAlloc`
`DLSSG.FenceEvent` `DLSSG.IsRecording` `DLSSG.QueueSubmitCallback[Data]`
`DLSSG.SyncFlushCallback[Data]` `DLSSG.SyncSignalCallback[Data]` `DLSSG.SyncSignalOnlyCallback[Data]`
`DLSSG.SyncWaitCallback[Data]` `DLSSG.SyncWaitOnlyCallback[Data]` `DLSSG.VkOFAModeRequest`
`DLSSG.run_lowres_mvec_pass`.

### 32.3 Other facts read from the same binaries and the SDK

* **Snippet-side exports** (the API `nvngx.dll` calls; the same shape as `nvngx_dlssnr.dll`,
  CLAUDE.md §5): `NVSDK_NGX_D3D12_Init`, `_Init_Ext`, `_CreateFeature`, `_EvaluateFeature`,
  `_ReleaseFeature`, `_Shutdown`, `_Shutdown1`, `_GetScratchBufferSize`,
  `_GetFeatureRequirements`, `_PopulateParameters_Impl`, `_PopulateDeviceParameters_Impl`, and
  the D3D11/Vulkan/CUDA twins, plus `NVSDK_NGX_GetSnippetVersion`, `NVSDK_NGX_GetGPUArchitecture`,
  `NVSDK_NGX_GetDriverVersionEx`, `NVSDK_NGX_SetOverrideStatusCallback`,
  `NVSDK_NGX_SetTelemetryEvaluateCallback`. HARD.
* **The feature id is `NVSDK_NGX_Feature_FrameGeneration = 11`** (`nvsdk_ngx_defs.h:210`, the
  vendored v310.7.0 SDK); Nukem's `NGXHandle::Allocate(11)` agrees. HARD.
* **The snippet's own D3D12 error strings** are only "Invalid D3D12 device" / "Invalid D3D12
  graphics command list" / "Invalid D3D12 graphics device" — it validates the objects it is
  handed and nothing about the resources. A missing or wrong resource is therefore the
  CLAUDE.md §5 "hang or garbage, never a returned error" class, same as feature 18. HARD
  (string set), consequence SOFT.
* **The cubin layer's format table** (`NGXCubinFormat_*`) includes `RGB10A2UN`, `RGBA16F`,
  `RG16F`, `RG32F`, `R32F`, `R16F`, `RGBA8UN`, `BGRA8UN`, `R8UN`, `RGB11F` — so an
  `R10G10B10A2_UNORM` back buffer is a format the D3D12 cubin path knows. HARD that the name is
  there; SOFT that it is accepted for `DLSSG.Backbuffer`/`OutputInterpolated` (the SL guide
  §11.0, HARD: "please make sure to use UINT10/RGB10 pixel format and HDR10/BT.2100 color
  space ... DLSS-G currently does NOT support FP16 pixel format and scRGB").
* **Motion-vector scale convention (SL guide §7.0, HARD):** `mvecScale = {1,1}` if the buffer
  is already in [-1,1]; `{1/renderWidth, 1/renderHeight}` if it is in pixel space. Our resolve
  emits render-resolution PIXELS (CLAUDE.md §5), so `DLSSG.MvecScaleX/Y = 1/renderW, 1/renderH`,
  `DLSSG.MvecJittered = 0` (jitter-free by construction), `DLSSG.MvecDilated = 0`,
  `DLSSG.CameraMotionIncluded = 1` (dense field, camera branch reconstructed). Sign convention
  is the NGX one we already feed SR. UNCONFIRMED until an interpolated frame is seen.
* **Depth (guide §5.1, HARD):** "Same depth data used to generate motion vector data ... this is
  the same set of requirements as DLSS-SR, and the same depth can be used for both" — so the
  `R32G8X24_TYPELESS` depth-stencil SR consumes today is the right input; `DLSSG.DepthInverted = 1`
  (reversed-Z, CLAUDE.md §2.4). `sl_consts.h:248-249`: with `depthInverted` true the snippet
  linearises as `1/depth`, and `cameraNear/cameraFar` default `INVALID_FLOAT` (must be set).
* **What DLSS-G under Streamline requires that the snippet does NOT:** the SL guide's "DLSS-G
  takes over frame presenting" (§12.0), the Reflex requirement (§8.0), and
  `eFailGetCurrentBackBufferIndexNotCalled` are all properties of `sl.dlss_g`'s swapchain
  proxy and pacer. None of those names or mechanisms exist in the snippet (32.1). Driving the
  snippet through the NGX core makes presenting, pacing and the back-buffer index OUR job — the
  present-twice design below — and removes the Streamline-only failure modes. HARD for the
  absence; the consequence is the design bet of this branch.

### 32.4 UE 4.27.2's back-buffer indexing, from the source (`D3D12Viewport.cpp`, `WindowsD3D12Viewport.cpp`, mirror @ `306a7e9`)

The present-twice design hands the game REPLACEMENT back buffers (our textures returned from a
hooked `IDXGISwapChain::GetBuffer`) and copies the presented image into the real swapchain
buffer at each of our presents, so the real ring may advance twice per game frame without the
game noticing. That requires knowing which replacement the game rendered into, and UE4 never
asks the swapchain (`GetCurrentBackBufferIndex` has zero call sites, migration doc §7.4):

* `FD3D12Viewport::Present` (`D3D12Viewport.cpp:869-875`): after `PresentChecked` returns true,
  `CurrentBackBufferIndex_RHIThread++; %= NumBackBuffers`. `PresentChecked` returns true whenever
  it called `SwapChain->Present` at all — the HRESULT is only inspected for the three fatal
  device-removed codes. It returns false WITHOUT presenting only on a fullscreen-state
  mismatch (`GetFullscreenState` disagrees with `bIsFullscreen`), in which case our hook is
  not called either. **So the game's index advances exactly once per Present that reaches our
  hook.** HARD.
* `FD3D12Viewport::Resize` (`WindowsD3D12Viewport.cpp:387`): after `ResizeBuffers`/`ResizeBuffers1`
  and re-fetching every buffer through `GetBuffer(i)` (`D3D12Viewport.cpp:214`, one call per
  index, `i < NumBackBuffers`), `CurrentBackBufferIndex_RHIThread = 0`. **So the index resets to
  0 at every ResizeBuffers, and GetBuffer is called for every index right after it.** HARD.
* `WindowsDefaultNumBackBuffers = 3` (`:15`), `DXGI_SWAP_EFFECT_FLIP_DISCARD` (`:120/167/207`),
  and `SetFullscreenState` is followed by a `Resize` (`:279` then `:354/376`). Before the
  resize UE4 releases its references to every back buffer (`D3D12Viewport.cpp:474-512`) and
  checks its OWN wrapper refcount is 1 — it never inspects the D3D12 resource's refcount, so
  a replacement texture we keep a reference to passes. HARD.
* `Present` transitions the back buffer to `D3D12_RESOURCE_STATE_PRESENT` (= COMMON) before
  presenting (`:824-828`) — the state our copy-out finds the replacement in. HARD.

Consequence: the mirror is `index = presents_since_last_resize % NumBackBuffers`, advanced on
every Present/Present1 that reaches the hook (whatever the HRESULT), reset to 0 by
ResizeBuffers/ResizeBuffers1 — `core::fg_plan::GameIndexMirror`, unit-tested. There is no
in-process cross-check available today (the native host has no render-target-bind tap), so a
mirror error would show as a STALE presented frame; that is what the stage-1 screenshot
protocol looks for.

### 32.5 No Streamline, by construction — what the snippet reads that only Streamline used to supply

Hard constraint from the user (2026-09-02): Streamline's interposer/swapchain layer is where
OptiScaler's FG died on this box (CLAUDE.md §5), so `sl.interposer.dll`, `sl.dlss_g.dll`,
`sl.common.dll` and the SDK are off the table in any form, including loading them ourselves.
The snippet is driven through the NGX core exactly like SR, and OUR present owner does what
Streamline's swapchain layer did. The question that constraint raises — does the snippet read
anything only Streamline can supply? — was answered from the strings (32.1/32.2) and from
Nukem's shim, which sits between `sl.dlss_g` and the snippet and so shows both sides:

| what Streamline supplies | in the snippet's strings? | verdict |
|---|---|---|
| the pacer / present thread / `DLSSG.CmdQueue` `CmdAlloc` `FenceEvent` `Sync*Callback` `QueueSubmitCallback` | **no** (32.1: 0 exact, 0 fragment) | SL-internal; the snippet records onto the list it is handed and returns. Ours: the present owner (fg_present). HARD |
| `DLSSG.EnableInterp` / `IsRecording` | **no** | SL-internal gating; Nukem reads them off SL's shared block, the snippet cannot. HARD |
| Reflex (`eFailReflexNotDetectedAtRuntime`) | no `Reflex` marker name; only `DLSSG.ReflexWarp.Available` (a snippet OUTPUT) and `DLSSG.UseReflexMatrices` (S-only) | the snippet has no Reflex dependency; Reflex goes through DXVK-NVAPI's `NvAPI_D3D_*` (`src/backend_native/fg_reflex.cpp`), status-logged, never gating. HARD for the absence |
| `GetCurrentBackBufferIndex` (`eFailGetCurrentBackBufferIndexNotCalled`) | no (`Present`, `Flip`: 0) | a property of SL's swapchain proxy; ours mirrors UE4's counter (32.4). HARD |
| `DLSSG.GetCurrentSettingsCallback` `EstimateVRAMCallback` `MustCallEval` `MultiFrameCountMax` `ReflexWarp.Available` | yes, and Nukem's `PopulateParameters_Impl` SETS them | snippet OUTPUTS the core populates for the caller; we read `MultiFrameCountMax`/`MustCallEval` back after CreateFeature and log them. HARD |
| every other `DLSSG.*` name `sl.dlss_g` sets (32.2, S+C) | yes | provided by `src/ngx_fg.cpp` per evaluate, values from the TAA hook's View CB and our resolve. HARD that the names are read; the VALUES' conventions (MvecScale, matrices' handedness, CameraFar=0) are UNCONFIRMED until an interpolated frame is judged on the box |

So nothing the snippet reads is Streamline-only. What remains UNCONFIRMED is behavioural:
whether the core under Proton routes feature 11 to a game-directory `nvngx_dlssg.dll` at all
(it routed feature 11 for OptiScaler through SL, which uses the same core call, CLAUDE.md §5 —
SOFT that our direct call is treated identically), and whether the snippet evaluates
correctly with `DLSSG.OutputReal` provided by us rather than by SL (a knob, `NgxFGOutputReal`).

### 32.6 The present-twice path on WARP under the debug layer (CI, 2026-09-02) — stage 1's offline half

`tests/warp/warp_fg_present.inc`, run by every CI push on Windows Server's WARP adapter with the
D3D12 debug layer and GPU-based validation on, over a REAL flip-model DXGI swapchain
(`CreateSwapChainForHwnd`, hidden window, `R10G10B10A2_UNORM`, 3 buffers, `FLIP_DISCARD` — the
game's own configuration, CLAUDE.md §2.1). The harness plays UE 4.27's part exactly (§32.4):
`GetBuffer(i)` for every index after creation and after every `ResizeBuffers`, one `Present`
per frame, its own counter reset only by `ResizeBuffers`. Verbatim from CI run 33683363510; the NGX=ON leg is green end to end from run 33684873745 on
(the intervening failures were the test's own model of UE4's counter, and a silent fail-fast
inside the swapchain's final Release caused by the HARNESS's closed command list still
referencing the real back buffers - a lesson for the plugin too: never release a swapchain
while any closed, un-reset list of ours names its buffers).

```
[WARN ] fg: ARMED for swapchain ...: 3 replacement back buffer(s) 640x360 fmt 24 (real flags 0x1), 2 generated, 3 crop readbacks
  ok: the game's buffer 0 is a replacement, not the swapchain's real buffer 0
  [creation] frame 0: Present hr=0x00000000 issued=1 generated=0
  [creation] frame 1: Present hr=0x00000000 issued=2 generated=1
    real ring: last=(0,1023,0) want (0,1023,0) | generated centre=(1023,0,0) want previous (1023,0,0) | band=(1023,0,1023) want magenta
  ok: the last presented real buffer holds the frame the game just rendered (mirror + copy correct)
  ok: the generated frame before it holds the PREVIOUS game frame
  ok: the generated frame carries the magenta band
  ok: counters: 4 game presents -> 7 presents issued, 3 generated
  ok: no validation errors across the present-twice sequence
[INFO ] fg: ResizeBuffers begins: worker drained, GPU idle on our lists, epoch 0 suspended
[WARN ] fg: ResizeBuffers done (hr 0x00000000): epoch 1, replacements DROPPED (the next GetBuffer re-arms), mirror count 3
[WARN ] fg: ARMED for swapchain ...: 3 replacement back buffer(s) 800x450 fmt 24 ...
  ok: no validation errors across resize, fullscreen-state and the presents after them
```

* **Every present of the pair lands the right pixels in the right real buffer**, read back
  through the ORIGINAL `GetBuffer` on the real ring: the buffer presented last holds the colour
  the "game" cleared its replacement to, the one before it holds the previous frame with the
  magenta band. That is the mirror, the replacement hand-off, both copies and both presents,
  end to end, with zero debug-layer errors. HARD on WARP.
* **`ResizeBuffers` with the game's references released succeeds while we hold ours** (the
  replacements are not swapchain buffers), the replacements are dropped and re-armed at the
  new size by the very next `GetBuffer`, and the mirror restarts at 0. HARD on WARP.
* **`SetFullscreenState` drains the presenter and bumps the epoch without dropping the
  replacements**; with no `ResizeBuffers` after it (WARP, windowed) the next present already has
  a previous frame, as UE4's un-reset counter implies. HARD on WARP.
* **The worker-thread presenter issues exactly 2 presents per game present** (counters read
  after the drain): 6 game presents -> 12 issued, 6 generated, issued-interval histogram
  unimodal (p50 1 ms, p99 6 ms at the harness's 1 ms pacing). HARD on WARP.
* **Under ReShade's D3D12 proxy (the real-ReShade CI lane, not Config A)** every pixel check
  passes except the band's exact colour, which reads a constant (389,84,389) instead of
  (1023,0,1023): written, transformed by something on the proxied path. UNCONFIRMED what.

What WARP cannot say (the box's half of stage 1): DXVK's DXGI + vkd3d-proton's swapchain under
gamescope — whether a second thread's `Present` is tolerated, what `GetCurrentBackBufferIndex`
returns between our two presents, whether the real `SetFullscreenState(TRUE)` transition (which
UE4 follows with a `ResizeBuffers`) survives with the worker drained, and whether the mirror
matches the game's counter in the live process (a mismatch shows as a stale frame in the
gamescope screenshot). Those are the stage-1 box measurements.

### 32.7 Stage 1 on the box: present-twice through our own present owner SURVIVES (2026-09-02 18:46-18:56)

Config A (ReShade `dxgi.dll` renamed aside, plugin `main.dll` = CI run 33686313374 of `ea784ee`,
`StrayDLSS : 1`, `StrayTriggers : 0`, `NgxNR=0`), `NgxFG=1 NgxFGMode=1 NgxFGPacing=1`
(experiment mode: the generated frame is the previous real frame under a magenta band, no NGX).
Box epoch `067020b6-…`, host load1 5-10 over 32 CPUs during the session (the neighbour's
contention was lower than the 0.8-0.96/CPU reported earlier, but **every frame-rate and pacing
number below is SUSPECT**, per the user's instruction; only correctness and stability count).

**Launch, verbatim from `stray-dlss-plugin.log`:**

```
fg: swapchain 000000000159C860 recorded; 3 slot(s) patched (GetBuffer/SetFullscreenState/ResizeTarget)
fg: ResizeBuffers done (hr 0x00000000): epoch 1, replacements DROPPED (the next GetBuffer re-arms), mirror count 3
fg: ARMED for swapchain 000000000159C860: 3 replacement back buffer(s) 3840x2160 fmt 24 (real flags 0x1), 2 generated, 3 crop readbacks
fg/reflex: NvAPI_D3D_SetSleepMode(lowLatency=1 boost=0 markers=1) -> 0 (NVAPI_OK) on device 000000001F900080; Sleep=available markers=available async=available
fg: finalised on queue 0000000001669E00; presenter=worker thread; armed=1 (GetBuffer already seen)
present owner: FIRST Present delivered on_present (... back_buffer=000000002A5984C0)   <- a REPLACEMENT
fg/reflex: first NvAPI_D3D_SetLatencyMarker(1, frame 0) -> 0 (NVAPI_OK)
fg/reflex: first NvAPI_D3D_Sleep -> 0 (NVAPI_OK)
fg: SetFullscreenState(TRUE) begins: worker drained, GPU idle on our lists, epoch 1 suspended
fg: SetFullscreenState(TRUE) done (hr 0x00000000): epoch 2, replacements kept, mirror count 3
fg: ResizeBuffers done (hr 0x00000000): epoch 3, replacements DROPPED (the next GetBuffer re-arms), mirror count 3
fg: ARMED ... 3840x2160 fmt 24 ...
[fg] frame 1800: game presents=1801 issued=3598 generated=1798 (2.00x) | refused: no-previous-frame=2 | pacer 12.17 ms hitches=3 | issued-interval p50=5 ms p99=8 ms BIMODAL (back-to-back presents) | worker waits=30 | epoch=3 reconfigures=3 | 3840x2160 fmt 24 colourspace -1
```

* **The fullscreen transition — where OptiScaler's FG died (CLAUDE.md §5) — is survived.**
  `SetFullscreenState(TRUE)` then `ResizeBuffers(3, 3840x2160, fmt 24)`, the worker drained
  before each, the replacements dropped and re-armed by the game's next `GetBuffer`, and the
  game went on to gameplay. HARD.
* **Exactly two presents per game present from then on**: `issued = 2 * game_presents - 2`
  at every `[fg]` line, the only refusals the two `no-previous-frame` frames after the two
  resizes. HARD. (`fg_presents_issued=89338` over `fg_game_presents=44670` at the end.)
* **The alternate frame reaches the display.** 12 `gamescopectl` screenshots (type 4, 0.37 s
  apart): 7 carried the magenta band (top 120 rows mean RGB (255,0,255)), 5 did not
  (band (115,65,17) = scene content), centre identical (static camera). The user, watching
  the screen, reported "pink strip on top of the frame, flickering" — the marker. HARD.
* **Stability:** `stray-bench.sh --runs 3` (three checkpoint reloads + the user's recorded
  traverse) completed, `bench exit 0`, the game alive at frame 47 400+ (~10 min), zero
  `[ERROR]` lines in the plugin log, no new crash directory (the newest is 05:23, before the
  session), `dmesg -T` shows no NVRM/Xid line after 15:17. HARD.
* **Reflex through DXVK-NVAPI 0.9.2 works at the API level:** `nvapi64.dll` exports all five
  entry points, `NvAPI_Initialize` and `NvAPI_D3D_SetSleepMode` return `NVAPI_OK` on the real
  vkd3d device, `Sleep` and `SetLatencyMarker` return `NVAPI_OK` every frame. HARD that the
  calls succeed; what they DO under vkd3d-proton/gamescope is UNCONFIRMED.
* **`colourspace -1`: the game never calls `SetColorSpace1`.** UE 4.27's default back buffer is
  the 10-bit `R10G10B10A2` in SDR, so `DLSSG.ColorBuffersHDR` must be 0 here; the auto rule
  was corrected (`ba6ab94`). HARD (the observation), the SDR reading [derived] from UE4's
  `r.DefaultBackBufferPixelFormat` default.
* **SUSPECT (host contention), recorded for later:** bench rows `fg1-experiment-1..3`: avg
  83.0 / 84.1 / 83.0 fps (engine frames, i.e. game presents), slowest bucket 60-68, 0 hitch
  buckets, worst frame 17-33 ms; `[perf]` 77-80 fps; pacer EMA 10-13 ms; the issued-interval
  histogram p50 6 ms p99 13-14 ms is flagged BIMODAL by the detector — at a 12 ms cadence
  with a 6 ms delay that is a false positive of the two-non-adjacent-buckets rule (5 and 7
  ms), not back-to-back presents; do not act on it until the host is quiet.

### 32.8 Stage 2 on the box, first session: the core ROUTES feature 11, CreateFeature SUCCEEDS, evaluate refused for a 1-based index (2026-09-02 18:56)

Same build, `NgxFGMode=2 NgxFGHDR=0`. Verbatim:

```
NVSDK_NGX_D3D12_Init_with_ProjectID succeeded
DLSS feature created: 1920x1080 -> 3840x2160, Performance, preset=13, flags=0x4b
fg/ngx: guide copies (re)created: depth 1920x1080 fmt 19 flags 0x2, mvecs 1920x1080 fmt 34
  ngx: [NGXCubinD3D12::Init:135] Enabling texmode_raw                      <- a THIRD cubin init: the FG snippet's
  ngx: [EndpointConfiguration::ReadRegkeys:171] INFO: Preset B selected, enabling UIR.
  ngx: [EndpointConfiguration::ParseNGXParametersCreateTime:50] NVSDK_NGX_DLSSG_Parameter_UserInterfaceRecompositionEnabled: (true)
  ngx: [NGXOverrideStatusCallback:2020] Feature [FrameGeneration] Override Reported: ModelPreset, Value: 2, Requested: Yes, Applied: Yes
fg/ngx: DLSS-G feature CREATED: 3840x2160 fmt 24 hdr=0 internal 1920x1080 | snippet says MultiFrameCountMax=absent MustCallEval=absent | output-real=provided
  ngx: [EndpointCoreInputs::ComputeAndValidateTimeFactor:418] Error: Multi frame is not supported on this device. Found index (0) but expected (1)
fg/ngx: EvaluateFeature(FrameGeneration): 0xbad00005 (FAIL_InvalidParameter)
[fg] frame 7200: game presents=7201 issued=7200 generated=0 (1.00x) | refused: no-previous-frame=2 source-missing=7199 | ... crops ok=0 ... validated=0
```

* **The NGX core under Proton routes `NVSDK_NGX_Feature_FrameGeneration` (11) to the
  game-directory `nvngx_dlssg.dll` (SL 2.13's copy, md5 `c9bd8831…`) and `CreateFeature`
  succeeds on our own parameter block — no Streamline anywhere in the process.** HARD. This
  retires the SOFT "does the core route feature 11 for a direct caller" question of §32.5.
* **`DLSSG.MultiFrameIndex` is 1-BASED.** With `MultiFrameCount=1, MultiFrameIndex=0` every
  evaluate returns `FAIL_InvalidParameter` and the snippet says why on the LoggingCallback.
  Fixed in `ba6ab94` (index 1). HARD. Nukem's shim tolerates both (it only refuses `> 1`),
  which is why the reference did not settle it.
* **The loud path held:** 11 685 refused evaluates, `generated=0`, every present real, the
  status file's `fg_generated_presented=0 fg_refused=7590`, no image damage. HARD.
* **The registry preset on this box selects "Preset B" and turns UI recomposition on**
  (`EndpointConfiguration::ReadRegkeys`), overriding our `UserInterfaceRecompositionEnabled=0`;
  the NV-app `ModelPreset` override "Value: 2 ... Applied: Yes". Noted; a knob if it matters.
* The snippet's `MultiFrameCountMax` and `MustCallEval` outputs are NOT populated into our
  parameter block by the Proton core (`absent`), unlike what `sl.dlss_g` reads — so the
  create-time capability read-back is unavailable here. HARD (this core), consequence: the
  only availability test is CreateFeature itself, as with SR (CLAUDE.md §1).
* `nvapi status -5` from `GetCudaIndependentDescriptorObject` appeared 5 times at the SR
  feature's creation, as in facts §26 (Config A, ext_unhook inert) — not FG's, and SR
  evaluated fine after.

### 32.9 Stage 2 on the box: DLSS-G evaluates and its frames reach the screen — no Streamline (2026-09-02 19:05-19:10)

Plugin = CI run 33688046770 of `ba6ab94` (index 1-based, HDR auto = SDR), Config A as in §32.7,
`NgxFGMode=2 NgxFGHDR=-1`. Verbatim:

```
fg/ngx: guide copies (re)created: depth 1920x1080 fmt 19 flags 0x2, mvecs 1920x1080 fmt 34
fg/ngx: DLSS-G feature CREATED: 3840x2160 fmt 24 hdr=0 internal 1920x1080 | snippet says MultiFrameCountMax=absent MustCallEval=absent | output-real=provided
fg/ngx: first DLSS-G evaluate OK: backbuffer 3840x2160, guides 1920x1080, jitter=0.3750,0.0556 reset=1 fov=1.2870 rad aspect=1.7778 near=1.000 far=0.000 mvecScale=0.000521,0.000926 hdr=0
fg: first crop verdict first-look: generated nonzero 3376/4096 hash 64d92a7cc1e09c8b | real nonzero 3376/4096 hash 64d92a7cc1e09c8b
fg: first crop verdict ok: generated nonzero 3525/4096 hash 487dd8454049e16f | real nonzero 3376/4096 hash 64d92a7cc1e09c8b
fg: generated output VALIDATED (3 consecutive ok crops: generated nonzero 3525/4096, real 3376/4096) - generated frames reach the screen from now on
[fg] frame 1800: game presents=1801 issued=2694 generated=894 (1.50x) | refused: no-previous-frame=2 not-validated=5 source-missing=899 | ...
fg: generated output REVOKED: crop verdict BLACK (generated nonzero 0/4096 hash 442c96fd51e00383, real nonzero 0/4096 hash 442c96fd51e00383); real frames only until it validates again
fg: generated output VALIDATED (3 consecutive ok crops: generated nonzero 4096/4096, real 4096/4096) - generated frames reach the screen from now on
[fg] frame 19200: game presents=19201 issued=33938 generated=14738 (1.77x) | refused: no-previous-frame=2 not-validated=3325 source-missing=1135 | pacer 12.08 ms hitches=11 | ...
```

* **`EvaluateFeature` on `NVSDK_NGX_Feature_FrameGeneration` succeeds with our parameter
  block** — the snippet driven through the Proton NGX core, presenting through our present
  owner, Streamline absent from the process. HARD.
* **The generated frame is an interpolation, not a copy:** the first valid look after the
  first-look has `generated hash 487dd8… != real hash 64d92a…` with the real frame unchanged
  from the look before (the `ok` verdict requires the generated crop to differ from both
  previous looks while the real one is compared against its own previous), and the
  `identical` verdict never fired. The two screenshots pulled and viewed (s1, s4 of six) are
  clean frames of The Slums with no corruption; a static camera makes generated and real
  indistinguishable by eye there, which is the point. HARD that it evaluates and differs;
  the QUALITY of the interpolation is the user's call (motion, eye test).
* **Three checkpoint reloads + traverses with DLSS-G active** (`stray-bench.sh --runs 3
  --label fg2-dlssg`): bench exit 0, game alive past frame 19 200, no crash directory, no new
  NVRM/Xid line. HARD. Rows (SUSPECT, host contention): avg 82.2 / 82.8 / 82.0 engine fps,
  slowest bucket 49-64, 0 hitch buckets, worst frame 29-32 ms — the same engine rate as
  stage 1 and the plugin-SR arms of §26 under the same contention, i.e. the evaluate's cost
  is inside the noise here; presented frames 1.5-1.8x game presents over the session.
* **What kept the ratio under 2x, both fixed:** `not-validated=3325` — every loading screen
  and fade made the REAL crop black too, the judge called it BLACK, revoked, and re-validated
  three looks later (`3fcc52a`: both-black is a neutral `dark` look); `source-missing=1135` —
  frames with no TAA dispatch (loads, menus) have no guides and correctly get no generated
  frame.
* Conventions passed and NOT yet judged by eye in motion: `MvecScale = 1/1920, 1/1080`,
  UE4's row-major matrices as SL's, `CameraFar = 0`, FOV 1.2870 rad (73.7 deg vertical from
  ViewToClipNoAA[1][1]), the camera basis from TranslatedWorldToView. UNCONFIRMED until the
  user reports the interpolation looks right while moving; each is a knob.
* The snippet does not populate `DLSSG.MultiFrameCountMax` / `MustCallEval` through this
  core, and the box's registry preset selects "Preset B" with UI recomposition (§32.8). HARD.

### 32.10 Stage 2, final session of the day: the gate no longer flickers, 1.91x and climbing (2026-09-02 19:14-19:18)

Plugin = CI run 33688856852 of `3fcc52a` (both-black looks are neutral). Same protocol.
`stray-bench.sh --runs 3 --label fg3-dlssg-dark`: bench exit 0, three reloads + traverses,
no crash directory, no new NVRM/Xid, zero `[ERROR]` lines. Verbatim:

```
fg/ngx: DLSS-G feature CREATED: 3840x2160 fmt 24 hdr=0 internal 1920x1080 | ...
fg/ngx: first DLSS-G evaluate OK: backbuffer 3840x2160, guides 1920x1080, jitter=0.3750,0.0556 reset=1 fov=1.2870 rad aspect=1.7778 near=1.000 far=0.000 mvecScale=0.000521,0.000926 hdr=0
fg: generated output VALIDATED (3 consecutive ok crops: generated nonzero 3525/4096, real 3376/4096)
[fg] frame 12600: game presents=12601 issued=24043 generated=11443 (1.91x) | refused: no-previous-frame=2 not-validated=5 source-missing=1150 | pacer 11.79 ms hitches=13 | issued-interval p50=6 ms p99=18 ms unimodal | worker waits=191 | epoch=3 reconfigures=3
```

* `not-validated` stays at the 5 frames of the one initial validation for the whole session
  (it was 3 325 in §32.9's run): no revoke through three reloads' black screens. HARD.
* `source-missing=1150` of 12 601 is the load/reload frames with no TAA dispatch — the
  remaining gap to 2.0x, and correct (no guides, no generated frame). The ratio climbs
  towards 2.0x as gameplay frames accumulate (1.24x at 1200, 1.91x at 12 600). HARD.
* SUSPECT (host contention): bench rows 81.6 / 80.2 / 82.2 engine fps, slowest bucket ~60, 0
  hitch buckets, worst frame 17-18 ms; pacer 11.4-16.9 ms; the issued-interval histogram
  read `unimodal` for most of this session and `BIMODAL` for a few windows (p50 6 ms, p99
  17-19 ms) — the detector's false positive under an irregular source; not acted on.
* The plugin log of this session is kept at `scratchpad/plugin-fg3.log` (agent-local); the
  box was restored to the playable configuration afterwards (§ box state in the report).
* **The `source-missing` refusals, attributed per 600-present window of the same log** (each
  window's presents-issued ratio and its refusal delta):

  ```
  frame    600: ratio 1.00  +source-missing 599   <- NGX not initialised (lazy init deadline frame 900)
  frame   1200: ratio 1.49  +source-missing 300   <- NGX init at ~900 + NgxFGWarmupFrames=120
  frame   1800: ratio 2.00  +source-missing   0
  frame   2400: ratio 1.90  +source-missing  60   <- checkpoint reload 1 (no TAA dispatch on the load)
  frame   3000: ratio 2.00  +source-missing   1
  frame   4200: ratio 1.90  +source-missing  61   <- reload 2
  frame   7200: ratio 1.90  +source-missing  59   <- reload 3
  frame  10200: ratio 1.95  +source-missing  33   <- reload 4 (the bench's cycles straddle windows)
  frame  10800: ratio 1.95  +source-missing  29
  every other window: ratio 2.00, +source-missing 0-3
  ```

  So **steady-state gameplay is exactly 2.00x**; the 9% headline is the startup (NGX's lazy
  init, 899 frames) plus ~60 frames per checkpoint reload where the engine runs no TAA
  dispatch and therefore publishes no guides, plus 0-3 frames per 600 (camera cuts / frames
  without the pass). Legitimately not generatable: a generated frame needs this frame's depth
  and motion vectors. HARD. The `[fg/ngx]` line and `fg_ngx_refused_*` status keys added in
  the follow-up commit attribute future refusals without this reconstruction.

### 32.11 The pacing fault, named by the trace, and the phase-locked pacer (2026-09-02 19:59-20:30)

User verdict from the eye test (plugins-only config, 165 Hz VRR panel via gamescope
`--adaptive-sync`, `bUseVSync=False`, engine steady at 82 fps): "frame pacing is visibly
wrong with FG on", unchanged with NR off. `NgxFGTrace=200` (a per-present timestamp trace,
`f901bb8`) on a quiet host (load1 2.4/32), NR off, verbatim (steady-state rows):

```
[fg-trace]    930 | hook + 11.67 | gen +  0.04 [ 0.01] | real +  7.13 [ 0.01] | delay  6.73 |
[fg-trace]    945 | hook + 11.78 | gen +  0.04 [ 0.01] | real +  7.15 [ 0.01] | delay  6.17 |
[fg-trace]    955 | hook + 12.00 | gen +  0.04 [ 0.01] | real +  7.15 [ 0.01] | delay  6.10 |
[fg-trace] gaps: 7.1 5.0 7.1 5.3 7.1 4.8 7.1 5.1 7.1 5.0 7.1 5.3 7.1 5.0 7.1 5.1 7.1 4.9 7.1 5.1 ...
```

* **The real frame went out at +7.15 ms after every hook while the pacer asked for 6.1-6.2 ms:
  `std::condition_variable::wait_for` under Wine wakes ~1 ms LATE.** The generated frame was
  on time (+0.04 ms, Present blocks 0.01 ms: no vsync, no display back-pressure). So the
  display received gaps of 7.1 / 5.0 instead of 6.05 / 6.05 — a 2 ms short-long alternation
  every 12 ms, which a VRR panel shows as-is. HARD.
* The anchor was also the wrong one in principle: holding a predicted half interval after
  the game's Present puts every bit of the game's own frame-time jitter on one gap of the pair
  (gaps `(I_k - d, d)`); the first rows of the trace, with the EMA still settling from
  20-30 ms warm-up frames, show it (gaps 26.6 / 4.1). The `pacer 16.5 ms` in the user's
  session was that EMA chasing a hitch.

**The fix (`6d5b38f`), two parts:** the last 1.5 ms of every hold are spun on the dedicated
worker thread (lands within microseconds); and the pair is scheduled on a clock anchored to
the previous REAL present — generated at `last_real + I/2`, real at `last_real + I`, `I` the
median of the last 16 game intervals — with late frames catching up and early ones held at
most three quarters of an interval so the game is never throttled (`core::fg::Schedule`,
unit-tested).

**After (`6d5b38f`, same protocol, host load1 0.4/32), verbatim:**

```
[fg-trace] gaps: 6.1 6.2 6.1 6.1 6.1 6.1 6.1 6.1 6.1 6.1 6.1 6.1 6.1 6.1 6.1 6.2 6.1 6.1 6.1 6.1 6.1 6.1 6.1 6.1 6.1 6.2 6.1 6.1 6.1 6.2 6.1 6.2 6.1 6.1 6.1 6.1 6.1 6.1 6.1 6.1
[fg-trace] gaps: 6.0 6.1 6.0 6.1 6.0 6.1 6.0 6.1 6.0 6.1 6.0 6.1 6.0 6.1 6.0 6.1 6.1 6.1 6.1 6.1 6.1 6.1 6.0 6.1 6.1 6.1 6.1 6.1 6.1 6.1 1.2 3.0 6.4 5.6 6.0 5.2 7.4 6.1 6.0 5.4
[fg] frame 2400: ... pacer median 9.43 ms hitches=4 (schedule: holds=1014 catchups=94 reanchors=9) | issued-interval p50=6 ms p99=16 ms unimodal
```

* **The steady-state cadence the display receives is 6.0-6.2 ms, every gap, for hundreds of
  presents** (the 165 Hz panel's period is 6.06 ms) — where it was 7.1 / 5.0 before. The
  remaining irregular stretches in the 200-present window are the game's own hitches during
  the post-load settle (23 ms, 18 ms, 15 ms frames: `[perf] worst 265 ms, p99 29 ms` in the
  same window), which the schedule re-anchors on (`reanchors=9`) and recovers from within a
  pair. HARD (the issue timestamps); the user's eye is the judge of what the panel shows.
* `holds=1014 catchups=94`: 91% of pairs went out on the clock, 9% caught up after a late
  game frame. The 9.43 ms median at frame 2400 is the load-screen cadence still inside the
  16-sample window; steady gameplay reads 12.1-12.2.

## 32.12 The vkd3d pipeline cache is NOT defeated by our create-path forwarding (offline, 2026-09-02)

The box shows, with the plugin loaded, every material compiling on first sight every session
(objects pop in ~200 ms apart) while with no host they appear instantly, and
`vkd3d-proton.cache` sits at 284 KB with the Proton log reading
`vkd3d_pipeline_library_disk_cache_merge: No write cache exists`. The hypothesis was that our
patch on `ID3D12Device2::CreatePipelineState` (slot 47) drops the CachedPSO blob, as ReShade's
pipeline events do (CLAUDE.md §5).

**Read from UE 4.27.2 source (`WindowsD3D12PipelineState.cpp`, mirror @ `306a7e9`), HARD:**
`CreatePipelineStateWrapper` (`:819`) uses the STREAM API `ID3D12Device2::CreatePipelineState`
whenever `pDevice2 && bUseStream` (`:831-832`) — for **both graphics and compute** PSOs. So
every material PSO passes through our slot-47 hook, and `Stream.CachedPSO = this->CachedPSO`
(`:73/:111`) puts the app's cached blob in that stream.

**Our hook is transparent, PROVEN in the WARP lane** (`tests/warp/warp_pso_cache.inc`, every
CI push): `hk_CreatePipelineState` calls `g_orig_CreatePipelineState(self, desc, ...)` with the
caller's **exact desc pointer**; the stream is only WALKED (read-only) AFTER the create returns,
to hash a compute shader for the census. The test creates a compute PSO through the patched
slot with a real CachedPSO blob and asserts, via `native::hooks::last_create_forward()`, that
the forwarded stream pointer and the `{blob pointer, size}` the runtime received are byte-for-
byte the caller's, and that the re-create from the PSO's own cached blob SUCCEEDS (a dropped or
corrupted blob would make D3D12 fail per spec). It does. **So our create hooks do not strip
CachedPSO and are not what defeats the cache.** HARD.

Two changes landed with the proof: graphics PSOs are no longer recorded at all (only compute,
for the census + TAA hash — `hk_CreatePipelineState` forwards then observes); and the forwarded
`CreatePipelineState`/`CreateComputePipelineState` are timed for stall attribution (§32.13).

**What remains to check on the box** (needs a launch, the user is on the box): the process cwd.
vkd3d writes `vkd3d-proton.cache` and its `.write` sibling RELATIVE to the process cwd
(`cache.c:3229` `VKD3D_SHADER_CACHE_PATH`, empty → cwd). The running game's cwd is
`/run/media/deck/GamesLinux/SteamLibrary/steamapps/common/Stray` (measured from `/proc/PID/cwd`),
yet the 284 KB read cache found is under `Binaries/Win64`. If UE4SS/the plugin changes the cwd
(vs the no-host run), the `.write` is written to a directory that is never promoted into the
read cache the game loads next startup — "No write cache exists" + recompile every session,
with no CachedPSO involvement. The check: compare the game's cwd and where a `.write` appears,
with the plugin vs without. UNCONFIRMED.

### 32.12.1 The cwd theory, narrowed offline (2026-09-03)

Two of the three ways our host could be responsible are now closed without a launch:

* **Nothing of ours changes the working directory.** `grep -rn "SetCurrentDirectory\|chdir"`
  over `src/`, `mods/` and `tools/` returns NOTHING. The plugin, the host and the launch
  tooling all leave the cwd exactly as Proton set it. So "the plugin chdir's and vkd3d's
  `.write` lands somewhere else" is dead as stated. HARD (absence in our own tree).
* **Our create-path forwarding is already exonerated** by the WARP proof above.

**What the section's own two measurements cannot both be true of one directory, and that is
the lead.** §32.12 records the game's cwd as `/run/media/.../common/Stray` (from
`/proc/PID/cwd`) while the 284 KB read cache sits under `.../common/Stray/Hk_project/Binaries/Win64`.
With `VKD3D_SHADER_CACHE_PATH` empty, vkd3d resolves BOTH the read cache and the `.write`
sibling against the cwd — so if the cwd really is the game root, the file in `Binaries/Win64`
is a **stale orphan from an older configuration that is never loaded**, and vkd3d has been
reading and writing in `common/Stray` all along. That alone produces "No write cache exists"
plus a full recompile every session, **with or without our plugin loaded** — i.e. it would not
be our bug at all. (Note our own sidecars — the status file, the verdict, the hash lists — are
opened by relative path and DO land in `Binaries/Win64`, which is evidence the cwd is
`Binaries/Win64` at least while the plugin writes them. One of the two readings is wrong, and
which one is the whole question.)

**A third candidate the earlier framing missed: every test session is killed, never quit.**
`tools/stray-lib.sh:81` and `tools/launch-stray-safe.sh:124` both end a session with
`pkill -x Stray-Win64-Shi` after a 10 s grace. A SIGTERM'd Wine process runs no
`DLL_PROCESS_DETACH` and no D3D12 device destruction, so whatever flush or promote vkd3d does
at teardown never runs. SOFT — it depends on whether vkd3d's disk cache is written
incrementally during the session or only at teardown, which has not been read from
vkd3d-proton 3.1.0's source.

**The sharpened check, and most of it needs no launch:** survey the filesystem for every
`vkd3d-proton.cache*` under the Steam library and note each one's size and mtime — if the
`Binaries/Win64` copy's mtime is old while something in `common/Stray` is fresh, the orphan
reading is confirmed outright. Then, in ONE session: `readlink /proc/<pid>/cwd`, and `ls` both
directories mid-session to see whether a `.write` exists at all and where. Only if a `.write`
does exist mid-session does the kill-versus-clean-exit question arise. UNCONFIRMED.

### 32.12.2 CLOSED, and the premise was wrong: the write cache works (measured 2026-09-02 21:24)

Read-only survey of the box, no launch of mine, immediately after another agent's SR+NR
plugin session (the plugin loaded, ReShade absent). The Proton log of that very session,
verbatim:

```
vkd3d_instance_deduce_config_flags_from_environment: shader_cache is used, global_pipeline_cache is enforced.
vkd3d_pipeline_library_init_disk_cache: Remapping VKD3D_SHADER_CACHE to: vkd3d-proton.cache.
vkd3d_pipeline_library_init_disk_cache: Attempting to load disk cache from: vkd3d-proton.cache.
vkd3d_pipeline_library_disk_cache_merge: Merging disk caches.
vkd3d_pipeline_library_disk_cache_merge: Done merging shader caches, existing entries: 1157, new entries: 1.
vkd3d_pipeline_library_disk_cache_merge: Successfully replaced shader cache with merged cache.
```

**`No write cache exists` does not appear anywhere in that log.** A write cache from the
previous session existed, was found, and was merged: 1157 existing entries plus 1 new one,
"successfully replaced". So the disk cache is loading, accumulating and promoting, **with the
plugin loaded** — the mechanism §32.12 set out to debug is not broken, and every hypothesis
in §32.12.1 is moot rather than merely unconfirmed.

**The cwd question resolves the other way from §32.12's reading, and the filesystem says so:**

| path | size | mtime |
|---|---|---|
| `.../common/Stray/Hk_project/Binaries/Win64/vkd3d-proton.cache` | 283 984 | **2026-09-02 21:24** (that session) |
| `.../compatdata/1332010/pfx/drive_c/vkd3d-proton.cache` | 432 | 2026-08-31 (an empty stub, dead) |

The `Binaries/Win64` copy is the live one and was rewritten by the session that had just
ended, so **the game's cwd at device creation is `Binaries/Win64`**, exactly where our own
sidecars land — consistent, and it retires the "stale orphan" reading I floated in §32.12.1.
The 432-byte file under `drive_c` is a header-only cache from some earlier configuration whose
cwd resolved to `C:\`; it is not consulted.

**Also retired: the kill-not-quit candidate.** A write cache from a session that ended in
`pkill` was still on disk and was merged at the next startup, so vkd3d's cache survives
SIGTERM and does not depend on a clean exit. The `pkill` in `stray-lib.sh` costs nothing here.

**What the 284 KB and "compiles on first sight" observations were, then, is UNCONFIRMED** —
they were real when recorded, and the plausible readings are a session that genuinely was the
first with a given driver/plugin combination (nothing to merge yet), or a cache that had been
cleared. `new entries: 1` is the number to watch: it stays near zero once warm, and a session
that recompiles everything would show it in the hundreds. Re-read those three log lines
before ever reopening this.

## 32.13 Stall attribution built into the host ([STRAYDLSS] StallWatch, default ON)

The user sees the DLSS indicator blink, the music cut out and a visible jump at the same
moments, under BOTH the plugin and the ReShade add-on with SR only, "since ever". Measured
under the add-on (host load 2.5/32, flat 6.1 ms/165 fps, our CPU a steady 1.2 ms) every few
seconds ONE frame lands at 24-28 ms while the surrounding windows are worst 6.4 ms — a 4x
single-frame stall a few times a minute: an audio underrun + a frame with no evaluate (so no
indicator, and NR amplifies it to a whole-screen change) + a visible jump. The -5 nvapi lines
are a one-time 5-line burst at the first evaluate, not this.

`src/perf.cpp` now prints one `[stall]` line for every present whose interval exceeds **3x the
running median** (a 64-present window), attributing that frame: our per-bucket CPU (sum AND the
single worst call), whether an NGX evaluate ran, **how many pipelines were created and how long
the forwarded vkd3d `CreatePipelineState` took** (suspect (1), §32.12: a first-sight compile
with no write cache), heaps/resources created, fence waits, and time inside the forwarded
`Present`. The per-frame accumulators reset each present; the cost is a few relaxed atomics per
present plus timing around the already-rare create calls. This is instrumentation only — no
behaviour change — so the culprit names itself in one box session (SR only, 3-minute menu idle,
where the stall reproduces). Attribution table to follow from that run.

## 32.14 The blink stall ATTRIBUTED: it is outside every hook we own, on an 11 s clock (2026-09-02 21:33-21:37)

One session, plugin host, **SR only** (`NgxNR=0`, `NgxFG=0`, `EnableNGX=1`, `NgxEvaluate=1`,
`StallWatch=1`), `--no-drive` so it idled in the MAIN MENU for ~4 minutes / 59 000 presents at a
flat 164.8 fps (6.07 ms). Proxmox host quiet throughout (load1 2.10 on 32 cores = 0.07, well
under the 0.3 bar); no Xid, no NVRM line, no GSP error inside the window.

**86 `[stall]` lines. 84 of them are the user's blink; 2 are startup compiles.** The split is
`PSO created`, and it is clean:

| | startup (n=2) | **the blink (n=84)** |
|---|---|---|
| frame time, median | 240.1 ms | **25.8 ms** (min 12.6, max 808) |
| PSO created | 32 each | **0** |
| forwarded `CreatePipelineState` | 37.61 ms total | **0.00 ms** |
| fence waits | 0.00 ms | **0.00 ms** |
| forwarded `Present` | 0.06 ms | **0.07 ms max** |
| resources / heaps created | 0 / 0 | **0 / 0** |
| NGX evaluate ran | 2/2 | **80/84** |
| our CPU, whole frame | 5.79 ms | **2.75 ms** median |
| worst single call of ours | — | **1.05 ms** median (always the `dispatch` bucket) |
| **unaccounted for** | 234 ms (98%) | **23.1 ms (89%)** |

Verbatim, a typical one:

```
[stall] frame 25.00 ms (median 6.07, 4.1x) #38 | PSO created=0 (compute=0) origCompile sum=0.00
max=0.00 ms | eval=1 | res +0 -0 heaps +0 | fenceWait=0.00 ms | orig exec=0.00 present=0.06 ms |
ourCPU sum=2.62 ms worst-call=1.01 ms (dispatch) | dispatchPath=1.61 resolve=0.44 rootBind=0.18
shadowCopy=0.16 restore=0.01 ms
```

**Every suspect the instrument can see is RULED OUT, and each by a number rather than by
argument:**

* **vkd3d pipeline compiles on first sight.** `PSO created=0` and `origCompile=0.00 ms` on all
  84. The two frames that DID compile are 240 ms monsters at startup and look nothing like the
  blink. (Consistent with §32.12.2: the disk cache is warm, so there is nothing left to compile.)
* **A synchronous path inside the NGX evaluate.** The evaluate RAN on 80 of the 84, and the whole
  `ngx_sr` bucket is 0.17 ms/frame in the same session's perf report. It is also not the
  *absence* of an evaluate: the earlier reading that the indicator vanishes because no evaluate
  happened that frame is **wrong** — the evaluate is there on 95% of blink frames.
* **The forwarded `Present`** (compositor, vsync, driver): 0.07 ms worst, out of 25 ms.
* **GPU fence waits:** exactly 0.00 ms, on all 84.
* **Resource or heap creation:** zero of both, on all 84.
* **Our own CPU:** 2.75 ms median of a 25.8 ms frame, and our worst *single call* is ~1 ms. Our
  cost does not rise on a stall frame in any way that could explain it.

**So ~23 ms — 89% of the stall frame — elapses somewhere no hook of ours can see.** That is a
positive result, not a null one: it says the stall is not in the D3D12 path at all, and every
fix aimed at our render work is aimed at the wrong place.

### The clock: exactly 1800 frames, and it is not the probe and not us

The per-600-frame perf windows make the structure unmistakable — **three stalled frames in one
window, then two completely clean windows, repeating for the entire session** (99 windows):

```
start    fps   worst  >16ms       burst window starts: 39600 42000 43800 45600 47400 49200
 45000  164.8    9.4  .                                51000 52800 54600 55200 57000 58800
 45600  164.6   23.1  ###         gaps, frames: 1800 2400 1800 1800 1800 1800 1800 1800 1800 ...
 46200  164.8    9.6  .
 46800  164.8    9.6  .           median gap 1800 frames at 164.0 fps = 10.97 s
 47400  161.9   51.2  ###
 48000  164.8   10.1  .
```

**A burst every 1800 frames = every 10.97 s**, dead regular. Three stalls of ~25 ms is ~75 ms of
lost time in one place — one visible hitch, which is exactly what a "blink" plus an audio
underrun looks like.

**Two cadences are thereby excluded outright, and one of them was the specific worry:**

* **StrayProbe ticks at 1 Hz** = every ~165 frames. Were it the culprit, every 600-frame window
  would carry 3-4 stalls; instead two windows in three are perfectly clean (`worst 9.4 ms`,
  `frames>16ms 0`). **The probe is not it**, and it could not have hidden inside this measurement.
* **Our own status heartbeat writes every 30 frames** (`dlss_app.cpp:1094`, an fopen/fprintf/
  fclose onto the external mount, on the present thread). Same argument, 60x over: it cannot
  produce a burst every 1800 frames.

**What has an 11 s period is UNCONFIRMED and is the next question.** Nothing in our code does.
Worth noting the number is suspiciously round in FRAMES (1800 exactly, repeatedly) rather than in
seconds, though at a pinned 164.8 fps this session cannot separate the two — a run at a different
frame rate would, and that is the cheapest next measurement. The instrument's own gap is that a
`[stall]` line carries neither a frame number nor a timestamp, so whether the three stalls in a
burst are CONSECUTIVE frames or spread across the window is not yet known; adding both to the
line is a two-line change and would settle it in one session.

**Do not attribute this to anything in this branch.** It reproduces under the plugin and under
the ReShade add-on, with SR only, and the user reports it predates all of this work.

## 32.15 The blink NAMED: the RHI thread blocks in the NVIDIA kernel driver's RM lock (2026-09-02 21:49-21:53)

§32.14 exonerated our code. This finds the thing itself. Method: a 486 Hz sampler over the
game's hot threads recording each one's scheduler state and its kernel `wchan`
(`tools`-external, `/tmp/stallprobe.py`), with every `[stall]` line timestamped as it reached
the log, pipewire's xrun counter once a second, and `dmesg -T -w`, all in one 150 s SR-only
gameplay session. 173 stalls captured.

**The finding, and it is a clean discriminator:**

| | inside stall windows | baseline (>=250 ms from any stall) |
|---|---|---|
| samples | 4 670 | 13 932 |
| RHIThread running (R) | 32.2% | 73.5% |
| **RHIThread in D (uninterruptible kernel sleep)** | **7.9%** | **0.0%** |
| **blocked in `os_acquire_rwlock_read`** | **365 samples** | **0 samples** |

`os_acquire_rwlock_read` is a symbol in NVIDIA's kernel module: it is the driver taking the
Resource Manager read/write lock. **It occurs 365 times inside stalls and exactly zero times in
13 932 baseline samples** — it is not background noise, it is the event.

**The duration matches the symptom exactly.** RHIThread's D episodes measure **median 18.5 ms,
mean 20.3 ms, p90 30.8 ms, max 39.1 ms** (57 episodes, 791 ms total in 150 s). A 6.1 ms frame
with ~19 ms of the RHI thread parked in the driver is the 24-28 ms frame the user sees.

### Who holds the lock: not another process, and not another thread of the game

Both halves were measured rather than assumed, from the Proxmox host where every GPU client is
visible (`/tmp/holder.py`, `/tmp/holder2.py`).

* **Every other GPU client was sampled at 486 Hz and none is elevated during the D episodes.**
  CPU ticks accrued while RHIThread was blocked, as a ratio to that state's share of wall time:
  `mangoapp 0.00x`, `gamescope-wl 0.00x`, `Xwayland 0.00x`, `steam 0.00x`, `steamwebhelper
  0.00x`, `wineserver 0.55x`, `xalia.exe 0.81x`. **Nothing above 1.0x.** MangoHud's NVML polling
  was the leading external suspect and is refuted outright.
* **All 268 threads of the game were sampled the same way.** The only thing over-represented
  while RHIThread is blocked is `RenderThread 1` sitting in `ntsync_schedule` at 1.8x — that is
  the render thread waiting on the RHI thread, i.e. the CONSEQUENCE, not the cause. No thread of
  the game is burning CPU or sitting in an nvidia path.

**So the writer holding the RM lock is not CPU-bound at all.** That is the informative part: an
RM operation that waits on the GPU rather than on the CPU. On this host the driver is the **open
kernel module 610.43.02 with GSP firmware active** (`EnableGpuFirmware: 18`, GSP 610.43.02) —
and on the open modules GSP is not optional. An RM request that round-trips to GSP firmware
holds the lock while consuming no CPU, which is exactly the signature measured. Corroborating,
this box's own history carries `NVRM ... NV_ERR_NO_MEMORY` from `_memdescAllocInternal` and
`_kgmmuClientShadowFaultBufferPagesAllocate`, and CLAUDE.md §5 records `_issueRpcAndWait:
rpcSendMessage failed` GSP RPC failures — the same subsystem.

**SOFT, and stated as such:** that the holder is specifically a GSP round-trip is an inference
from "holds the RM lock, burns no CPU, on a GSP-mandatory driver", not a direct observation. A
direct one needs `/proc/driver/nvidia/gpus/*/gsp_logs` or a kernel stack trace from
`/proc/<tid>/stack`, neither of which was read. **HARD** are: the D state, the
`os_acquire_rwlock_read` wchan, its 365-vs-0 exclusivity, the 18.5 ms median, and the
exoneration of every other process and thread.

### Two things this rules out that were being assumed

* **The kernel log is silent.** Zero `dmesg` lines of any kind inside the probe window; every
  NVRM line in the buffer is from 05:00 or 15:17, hours earlier. So the stall leaves no kernel
  trace, and grepping `dmesg` for it will always come up empty.
* **The system is not freezing.** Our own 486 Hz sampler kept its cadence straight through every
  stall (median inter-sample gap 2.06 ms, only 2 gaps above 15 ms in 72 868 samples). The stall
  is confined to the game process — specifically to the one thread that talks to the GPU.

### The audio is a SEPARATE event — measured, not assumed

pipewire's xrun counter was sampled once a second across the same 150 s that contained **173
visual stalls**: it started at 0, ended at 0, **delta 0, with not one second showing a new
xrun**. The audio dropouts the user hears are not these frames. This confirms the user's own
correction and closes the question with data rather than an inference.

### Consequence

**This is not ours, and it is not fixable inside this project.** No hook we own is on the path,
and the blocking is one thread of the game waiting on the NVIDIA kernel driver. Mitigations, if
the user wants them, live at the host/driver level and are their call, not a code change here.
The measurement scripts are kept at `/tmp/stallprobe.py`, `/tmp/holder.py` and `/tmp/holder2.py`
on the box so the result can be reproduced or re-run against a different driver.

## 32.16 The blink is WALL-CLOCK, and the kernel stack names the call: a VRAM-info ioctl (2026-09-02 22:09-22:22)

§32.15 named the block (`os_acquire_rwlock_read`, the NVIDIA RM lock) but left two questions:
frames or seconds, and what the RHI thread was actually doing. Both are now answered, and the
second one by reading the kernel call chain rather than inferring it — `/proc/<tid>/stack` is
readable on this host **with symbols** (`kptr_restrict=0`).

### It is seconds, not frames — the A/B inside one session

`[stall]` now carries `f=<present index> t=<unix epoch>` (`src/perf.cpp`), so the period can be
read directly instead of reconstructed from 600-frame windows. One gameplay session, frame cap
flipped mid-session through `mods/StrayConsole` (`t.MaxFPS`), everything else identical:

| | uncapped | capped |
|---|---|---|
| frame rate | 100.8 fps | 57.0 fps |
| **Δ frames between stalls** | **~101** | **~57** |
| **Δ seconds between stalls** | **1.007 0.995 1.012 0.992 1.009 1.003 0.998** | **1.017 0.997 0.992 1.000 1.012 1.003 0.997** |

**The frame gap tracks the frame rate; the time gap does not move off 1.00 s.** The trigger is
wall-clock 1 Hz and nothing in the render path or in our stack is driving it. HARD.

### The kernel stack, verbatim — and it is a framebuffer-info query

Captured the instant RHIThread was seen in D (25 dumps, all identical):

```
[<0>] os_acquire_rwlock_read+0x31/0x50 [nvidia]
[<0>] portSyncRwLockAcquireRead+0x10/0x40 [nvidia]
[<0>] rmapiLockAcquire+0x2c4/0x360 [nvidia]
[<0>] serverTopLock_Prologue+0x5c/0xe0 [nvidia]
[<0>] serverControl+0xc5/0x590 [nvidia]
[<0>] _rmapiRmControl+0x4f2/0x820 [nvidia]
[<0>] rmapiControlWithSecInfo+0x79/0x140 [nvidia]
[<0>] _rmControlForDeprecatedApi+0x1f/0x30 [nvidia]
[<0>] _ctrl_convert_v2_NV2080_CTRL_CMD_FB_GET_INFO+0x195/0x2c0 [nvidia]
[<0>] _nv04ControlWithSecInfo+0x47/0xa0 [nvidia]
[<0>] RmIoctl+0x934/0xda0 [nvidia]
[<0>] rm_ioctl+0x66/0x4e0 [nvidia]
[<0>] nvidia_unlocked_ioctl+0x69a/0xa50 [nvidia]
[<0>] __x64_sys_ioctl+0xa2/0x100
```

**`NV2080_CTRL_CMD_FB_GET_INFO` is an RM control that queries framebuffer (VRAM) information**,
reached through `_rmControlForDeprecatedApi` — the legacy v1 control path the driver converts.
So once per second the game's RHI thread issues a VRAM-info ioctl, and blocks taking the RM
**top lock** in read mode. That the caller above the ioctl is UE4's own memory-stats update via
`IDXGIAdapter3::QueryVideoMemoryInfo` (translated by vkd3d-proton) is the obvious reading but
is **SOFT** — it was not traced into user space, and vkd3d-proton's own budget refresh is an
equally shaped candidate. **We are not the caller: `QueryVideoMemoryInfo`, `IDXGIAdapter3` and
`SetVideoMemoryReservation` appear nowhere in `src/` or `mods/`.** HARD.

### Nothing else on the box is blocked — swept twice, at two widths

* Every process holding the nvidia device, plus `pvestatd`, `rrdcached` and
  `nvidia-persistenced`, sampled by STATE and wchan rather than by CPU (the earlier CPU test
  could not have detected a poller, since a poller blocked in the driver burns no CPU either):
  **not one thread of any of them was in D or in an nvidia wchan while RHIThread was blocked.**
  111 D samples against 18 876 non-D. `pvestatd`'s 10 s cadence is therefore not it.
* Widened to **every task on the entire host, kernel threads included**, 12 full `/proc` sweeps
  taken at the instant RHIThread was in D: **NONE**. Not one other task anywhere was in D.

So the RM write-lock holder is not visible as a blocked task. **Its identity is UNCONFIRMED**,
and there is a known gap in the method: a holder spinning on a GSP RPC would sit in **R**, and
both sweeps looked only for **D**. That is the next measurement, not a conclusion.

### Ruled out by measurement

* **VRAM pressure**: 7 824 of 24 564 MiB used, **16 285 MiB free**; BAR1 72/256 MiB. The
  `NV_ERR_NO_MEMORY` lines in this box's history are from 05:00 and 15:17 and are unrelated.
* **Throttling**: `clocks_event_reasons.active 0x0`, 73 C.
* **Kernel log**: silent for the whole window.
* **Audio**: pipewire xrun delta 0 across 150 s containing 173 stalls (§32.15).

### Two things §32.14 left unconfirmed, now answered

* **The three stalls in a menu burst are NOT consecutive frames.** `f=6506 / 6509 / 6511` inside
  61 ms — one disturbance smeared over ~10 frames, three of which crossed the threshold.
* **The menu's period is 11.4 s, gameplay's is 1.0 s** (`t=482.98 -> 494.42 -> 505.85`, and
  `f=6506 -> 8385 -> 10264`, i.e. 1879 frames at a pinned 164.8 fps). The likely reconciliation
  is that the 1 Hz event is always there and only its COST varies — the menu's disturbances
  measure 19-38 ms against gameplay's 49-83 ms, and the detector only fires above 3x the median
  (18.2 ms in the menu, 30 ms in gameplay), so in the menu only the occasional expensive one is
  logged. **That reconciliation is inference, not measurement** — no stack was captured in the
  menu.

## 32.17 Mitigating the blink — options for the user, none of them applied

**Nothing here has been done to the box.** Each option says what to change, how to revert, and
what number decides whether it worked. Take them in order: the first two are free and reversible
inside the game directory, the third is a host-level driver change.

**The measurement is the same for all of them, and it is cheap.** Run a session, then:

```
grep -o 'f=[0-9]* t=[0-9.]* frame [0-9.]* ms' stray-dlss-plugin.log | tail -20
```

Success is the `t=` gaps ceasing to be ~1.00 s apart, or the `frame` figures dropping from
49-83 ms toward the 10 ms median. **Judge on the medians of at least 15 stalls, never on one**
(CLAUDE.md §5). A run where `[stall]` stops appearing at all is the win condition.

### Option 1 — stop the once-per-second VRAM query (free, in-game, MOST LIKELY TO HELP)

The stack shows a framebuffer-info RM control once per second on the RHI thread. If that is
UE4's memory-stats update — **SOFT, §32.16** — then UE 4.27 may expose a way to stop it, and
`Engine.ini [SystemSettings]` is how this game takes cvars (§2.2, command-line arguments do not
work). **Do NOT paste a cvar name from memory into that file**: the first step is to read UE
4.27.2's `D3D12Adapter.cpp` / `D3D12Viewport.cpp` for the call site of `QueryVideoMemoryInfo`
and find what actually gates it. An invented cvar name is silently ignored and would produce a
convincing-looking null result.

Revert: delete the line from `Engine.ini`.

### Option 2 — confirm the caller first, which costs one run

Before changing anything, settle whether the 1 Hz caller is UE4 or vkd3d-proton, because they
have different fixes. `ltrace`/`strace` is too heavy for a running game, but the RHI thread's
USER-space stack at the moment of the block is enough, and the box already has everything
needed. That single answer decides between option 1 and an upstream vkd3d-proton report.

Worth reporting upstream either way: the call goes through `_rmControlForDeprecatedApi`, i.e.
the **legacy v1 control API** that the driver has to convert, and it costs 49-83 ms on a
completely idle GPU with 16 GiB of VRAM free. That is pathological for what is a status query,
and it is the kind of thing vkd3d-proton or the driver team would want to know.

### Option 3 — the driver: proprietary modules, GSP off (HOST-LEVEL, the user's call)

The blocked call is an RM control taking the RM top lock. On this host the driver is the **open
kernel module 610.43.02**, where **GSP firmware is mandatory** (`EnableGpuFirmware: 18`, GSP
610.43.02), so RM controls round-trip to firmware. The testable alternative is the
**proprietary** kernel modules, which allow GSP to be turned off:

```
# /etc/modprobe.d/nvidia-gsp.conf
options nvidia NVreg_EnableGpuFirmware=0
```

then `update-initramfs -u` and reboot. **This requires the proprietary driver flavour; setting
it on the open modules does nothing**, because they have no non-GSP path.

Revert: delete the file, `update-initramfs -u`, reboot. Confirm with
`cat /proc/driver/nvidia/params | grep EnableGpuFirmware` and `nvidia-smi -q | grep GSP`.

**Caveats, stated plainly.** This is the biggest-blast-radius option and the least certain: the
GSP explanation for the lock hold is **SOFT** (§32.15/§32.16 — nothing was observed holding the
lock at all), the host runs other tenants that share this GPU, and a driver flavour change
affects all of them. It is worth doing only after options 1 and 2 have failed, and only when the
user is willing to reboot the host.

### What NOT to spend time on

Measured and dead: mangoapp/MangoHud's NVML polling, `pvestatd`, `rrdcached`,
`nvidia-persistenced`, gamescope, Xwayland, steam and steamwebhelper (none blocked at the
instant, §32.16); VRAM pressure and thermal/clock throttling; anything in our own render path
(§32.14, 89% of the stall is outside every hook we own); and StrayProbe or our status-file write
(both excluded by period, §32.14, and by the frame/second A/B, §32.16).

## 32.18 The caller NAMED: UE4 itself, ~21 QueryVideoMemoryInfo calls PER FRAME (2026-09-02 22:51)

§32.16 left the user-space caller SOFT. `[STRAYDLSS] VramQueryWatch` (default OFF,
`src/backend_native/vram_query_watch.cpp`) patches `IDXGIAdapter3::QueryVideoMemoryInfo` on the
adapter reached by the device's own LUID and logs, per call, the return address, the module it
lands in and the forwarded call's duration. One SR-only gameplay session answers it:

```
vram query watch: INSTALLED on IDXGIAdapter3::QueryVideoMemoryInfo (adapter LUID 00000000:000003f2)
[vram] QueryVideoMemoryInfo CALLER: module=Stray-Win64-Shipping.exe ret=00006FFFF8DE8BDC offset=+0x1768bdc
vram query watch: calls=167146 callers: Stray-Win64-Shipping.exe=167146
```

**The caller is the GAME. 167 146 calls, 100% of them from `Stray-Win64-Shipping.exe`, from a
SINGLE return address** (`+0x1768bdc` — only one CALLER line was ever emitted, and the watch logs
one per distinct module). Not vkd3d-proton, not DXVK, not DXVK-NVAPI, not NVML. HARD.

**And the rate is the finding.** The periodic report advances 12 618 calls per 600 presents —
**~21 calls PER FRAME**, roughly 2 100 per second at this frame rate, alternating
`group=0` (LOCAL) and `group=1` (NON_LOCAL). Each one is a real ioctl into the NVIDIA driver
that takes the global RM lock.

| | |
|---|---|
| typical call | **0.01 - 0.06 ms** |
| slow calls (logged above 5 ms) | **median 14.7 ms, max 36.5 ms** |
| ratio | ~1000x |

**The slow ones arrive in bursts of 3, every 11.46 s, and the regularity is extraordinary:**
`2.43, 6.92, 11.42, 11.53, 11.39, 11.46, 11.49, 11.45, 11.46, 11.46, 11.46` seconds between
burst starts. **That is §32.16's menu burst period (11.4 s) reproduced exactly**, now with the
call that causes it named.

### What this does NOT explain, stated plainly

**Only 21 of 133 stalls in that session fall within 100 ms of a logged slow call.** So the VRAM
query is *a* cause — the 11.46 s burst — and **not the whole stall population**. Possible
readings, none yet tested: the watch only logs calls above 5 ms, so a stall built from several
2-4 ms calls is invisible to it; or a second mechanism shares the RM lock. **Do not present this
as the complete explanation of the blink.** The earlier gameplay session measured stalls 1.00 s
apart (§32.16) where this one bursts at 11.46 s, so the cadence itself varies between runs and
is not yet understood.

### Against expectation, and worth recording as a research lesson

A source review of UE 4.27.2 (`AlexMercer-MA/UnrealEngine-4.27`, all 60 files of
`D3D12RHI/Private/`) predicted the opposite on two counts, and BOTH predictions were wrong
against the running game:

* It found the per-frame path `FD3D12CommandContextBase::RHIEndFrame` (`D3D12CommandContext.cpp:571`)
  -> `UpdateMemoryStats()` (`:642`, body `:695`) -> `FD3D12Adapter::GetLocalVideoMemoryInfo`
  (`D3D12Adapter.cpp:1204`) -> `QueryVideoMemoryInfo` (`:1210`), with **no throttle of any kind**
  and **no cvar gating it** — the only related cvar being `D3D12.AdjustTexturePoolSizeBasedOnBudget`
  (default 0) on the separate `RHIGetTextureMemoryStats` path.
* It predicted the whole body is compiled out by `#if PLATFORM_WINDOWS && STATS`, and that
  `STATS` is 0 in a Shipping non-editor build (`Build.h:324-326`, `FORCE_USE_STATS` default 0) —
  i.e. that a retail game **never makes this call**.

**The measurement says a retail `Stray-Win64-Shipping.exe` makes it 21 times a frame.** So either
this title ships with `STATS` forced on, or the calls come from a path the review did not cover.
Either way: **a source review of the stock engine is a hypothesis about a licensee's build, never
a fact about it.** The binary is the authority, and one vtable patch settled in a single session
what the source reading got backwards twice.

## 32.19 Proposed, NOT built: cache the answer in the slot we already own

Recorded as a proposal because the coordinator asked for one rather than an implementation.

**The case.** ~2 100 driver round-trips per second, each taking the global RM lock, for a
statistic whose value moves slowly and which the engine only feeds to memory counters. The
normal cost alone (~20 us x 2 100/s) is ~4% of a core; the tail is a 14-36 ms frame.

**The change.** The hook already forwards through `original_for`; make it serve a cached
`DXGI_QUERY_VIDEO_MEMORY_INFO` per `(node, group)` and refresh from the driver only every N ms
(a few hundred), returning the cached copy otherwise. At 500 ms that is ~1 000x fewer ioctls and
proportionally less chance of meeting the lock held.

**Why it is safe here, and the one reason it might not be.** UE 4.27 consumes this only through
`UpdateMemoryStats`'s `SET_MEMORY_STAT` counters and, if `D3D12.AdjustTexturePoolSizeBasedOnBudget`
were ever set to 1, through texture-pool sizing — a stale-by-half-a-second budget is harmless to
the first and merely slightly late for the second. **The risk is that Stray's build uses it for
something the stock source does not show**, which is exactly the assumption that just proved
wrong above; so ship it OFF, behind its own key, and A/B it with the same `[stall]` and `[vram]`
lines that found the problem.

**Acceptance, pre-registered:** medians of at least 15 stalls. The 11.46 s burst must disappear
from the `[vram]` log, and the `[stall]` rate must fall by at least the 21/133 share the bursts
account for. Anything less means the remaining stalls are the second mechanism, which is a
separate investigation and should be reported as such rather than absorbed into this one.

---

## 33. The back buffer is SDR display-encoded: R10G10B10A2_UNORM with no SetColorSpace1 (2026-09-02)

Recorded here because a whole feature rests on it — the DLSS Neural Rendering PRESENT STAGE
(`src/nr_hook.hpp`) hands the network the back buffer unconverted on the strength of this
section, and if it is wrong the network is handed a doubly-encoded image and the symptom is
"it looks washed out", not an error.

> **UPDATED 2026-09-03.** This section was written while the present stage was one of three
> selectable sites behind `[STRAYDLSS] NgxNRHook`. The user confirmed the stage working in the
> game that day and the other two sites were deleted, along with the HDR colour codec that only
> the `taa` site needed. So this section no longer merely *justifies a bypass*: it is the whole
> reason NR needs no colour conversion at all.

**HARD, from this document's own §1 and from ReShade's log of every session:** every swapchain
Stray creates is `NewFormat = 24` = `DXGI_FORMAT_R10G10B10A2_UNORM`, `BufferCount = 3`, at
3840x2160 and at 2560x1440. Ten bits per channel is a *precision* choice, not an HDR one.

**The colour space is what settles it, and the evidence is an ABSENCE.** A D3D12 swapchain is
`DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709` — plain sRGB/Rec.709, SDR — until the application calls
`IDXGISwapChain3::SetColorSpace1` to say otherwise; HDR10 output specifically requires
`RGB_FULL_G2084_NONE_P2020`. **Stray never calls it.** Carried from the frame-generation work on
the `dlss-fg` branch, where the swapchain vtable is patched slot by slot and every call the game
makes through it is logged.

**PROVENANCE, stated plainly: this half is SOFT on `nr-present-stage`.** The measurement was made
on another branch and is repeated here rather than re-run; nothing in this worktree has observed
it. It is also consistent with the compositor setup (gamescope's `--hdr-itm-enabled` is
inverse *tone mapping* — it takes an SDR image and expands it, which is only needed because the
game hands it one), but that is corroboration, not proof.

**Why it matters, in one line:** feature 18 is a display-referred network trained on sRGB. At
the old TAA site the image was raw linear HDR, so the soft-clip + sRGB proxy WAS the input
contract; at the back buffer that same encode would be a *second* application of a transfer the
game's tonemapper already applied. Same rule, opposite action — and it is why the codec could be
deleted with the site rather than carried forward.

**What would falsify it:** a `SetColorSpace1(RGB_FULL_G2084_NONE_P2020)` in a log from this
title, or a present-stage image that looks *flat and washed out* rather than merely HUD-processed.
There is no longer a second site to fall back to, so the answer then would be to reinstate a
codec at this site — not to revive the TAA one, whose feedback node was the reason it went.
