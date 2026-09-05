# Raising Stray's HDR brightness to the SDR reference — measurement, source, levers

**Date:** 2026-09-05. **Metric (set by the user):** peak brightness of the displayed frame, in nits,
after ~30 s of eye-adaptation settling, HDR against the SDR configuration as the reference; a
mid-tone statistic (median) is carried alongside so a lever that only lifts highlights is visible
as such. **Scene:** the Slums save (slot 1) spawn, cat facing the musician, nothing pressed after
the load. **Provenance labels** follow CLAUDE.md §0.5: HARD = read in source or measured on the
box; SOFT = inferred from a version that is not the one on the box, or a forum-grade claim;
UNCONFIRMED = not verified.

Sources: UE 4.27.2 public mirror `AlexMercer-MA/UnrealEngine-4.27` @ `306a7e9f7fb5` (all UE line
numbers below are from it); gamescope `ValveSoftware/gamescope` master @ `ad26ec4cfbc0`
(2026-09-04) — the box runs `gamescope 3.16.25-1` (pacman), which is not a Valve release tag, so
every gamescope source claim is HARD for master and **SOFT for the box** unless it was also
measured. Captures: `/tmp/hdrshots/` on the box (46 files, 570 MB), copies under
`/Users/lunks/.ccs/instances/contact/jobs/edb7b93b/tmp/hdr-shots/`; analysis scripts in
`.../jobs/edb7b93b/tmp/hdrcalc/` (`curves.py`, `analyze.py`, `compare.py`, `sdrcheck.py`).

---

## 0. The answer in five lines

1. **HDR is dark in the mid-tones, not at the peak.** Same scene, same spawn: SDR median **5.6 nits**
   vs HDR median **0.32–0.38 nits** (17×, +4.1 EV); SDR peak **155 nits** vs HDR peak **90–133 nits**
   (1.2–1.7×, +0.3..0.8 EV). Every bin from 0.35 to 30 SDR-nits is 13–17× darker in HDR; only
   above ~100 nits does the gap close (§2).
2. **A gamescope PNG is not a measurement of an HDR frame** (it is gamma-2.2 of nits/500). The
   faithful channel is `gamescopectl screenshot <path>.avif 1`: a lossless 10-bit **PQ** AVIF
   whose codes decode to nits (§1). The SDR reference can only be *modelled*: 202.64 nits ×
   code^2.2 (§1.3).
3. **No console cvar in 4.27 moves the HDR mid-tones.** `r.Color.Mid`, `r.TonemapperGamma`, the
   game's Gamma slider and the film-curve settings only touch the SDR branch of the LUT shader;
   `r.HDR.Display.OutputDevice 4` and `HDRDisplayOutputNits` only touch highlights; UE5's
   `r.HDR.Aces.SceneColorMultiplier` does not exist here (§3, §6).
4. **The one engine lever that reaches the HDR ODT without being rewritten by the game is
   `APlayerCameraManager::ColorScale`** (a uniform pre-ODT gain, `View.ColorScale`), written by
   reflection from the existing `StrayHdr` Lua or the plugin. Inverting the ODT on the measured
   data gives a remarkably flat **k ≈ 6.7 (+2.75 EV)** for shadows and mid-tones, and **k ≈ 1.5
   (+0.6 EV)** to match the *peak* (§4, §7).
5. **The two targets are irreconcilable with any uniform gain**: matching the peak leaves the
   mid-tones 11× too dark; matching the mid-tones lifts the peak to ~600 nits (4× SDR). Only a
   curve — a PQ-domain gain that falls to 1.0 towards the peak, applied by our present stage — can
   satisfy both, and it has not been built (§4.3, §7).

---

## 1. Measurement method and its validity

### 1.1 What a gamescope screenshot encodes

`gamescopectl screenshot <path> <type>` (type 1 = base plane at render resolution, 4 = screen
buffer). Read from `src/steamcompmgr.cpp` @ `ad26ec4`:

* The output encoding is chosen by **file extension and type**: `bHDRScreenshot = path.extension()
  == ".avif" && ColorspaceIsHDR(layer0) && type != SCREEN_BUFFER` (`:3432-3435`);
  `frameInfo.outputEncodingEOTF = bHDRScreenshot ? EOTF_PQ : EOTF_Gamma22` (`:3479`). **A PNG is
  always Gamma-2.2 output**; a `.avif` of an HDR layer is PQ. HARD (master), and measured to hold on
  the box (below).
* The AVIF is 10-bit lossless, primaries BT.2020 / transfer SMPTE2084 for HDR, BT.709 / sRGB for
  SDR, stored as raw GBR planes under `MatrixCoefficients = IDENTITY` (`:3616-3625`,
  "storing raw GBR (yes not RGB) data ... always lossless!"). Measured: `avifdec --info` reports
  `Bit Depth 10, Transfer 16, Primaries 9, CLLI 1499,799` for HDR captures and `Transfer 13,
  Primaries 1` for SDR captures. **`avifdec -d 16` returns the planes reversed relative to the
  PNG** — permutation (2,1,0) agrees to 1.42/255 mean, identity differs by 51/255 — so the
  decoder's output must be read as BGR (fixed in `analyze.py`; the first pass of numbers in this
  session was computed before the fix and is not quoted).
* For the PNG of an HDR frame, the screenshot LUT path is "PQ -> G22, leverage the display's
  native brightness": `tonemapping.g22_luminance = flInternalDisplayBrightness` (`:387`), i.e.
  `code = (nits / L)^(1/2.2)` (`color_helpers.cpp:651-658`). **Measured against the AVIF of the
  same moment**: the hypothesis `PNG = g22(nits/500)` fits with mean |error| 3.7/255 (temporal
  drift between the two captures included) against 19.9/255 for "PNG = PQ code" and 5.3–10.8 for
  every sRGB variant — so `L ≈ 500`, gamescope's default `flInternalDisplayBrightness`
  (`rendervulkan.hpp:498`), even though the connector reports CLL 1499 (box-version detail, SOFT).
  **The PNG clips everything above ~500 nits and quantises the 0.3-nit median to 14/255; it is
  not usable for nits.**
* **Type 4 is not the raw screen buffer on this box**: HDR type 4 and type 1 PNGs are pixel-identical
  (mean |diff| 0.42/255) and so are SDR ones (1.44/255, the remainder is scene animation), whereas
  master's code would make type 4 carry PQ codes of the live composite. Recorded as a box-version
  difference (SOFT); consequence: **the on-screen composite of an SDR frame cannot be captured**.

### 1.2 The HDR side is a true measurement

The game's swapchain under the HDR configuration is `VK_FORMAT_A2B10G10R10_UNORM_PACK32` with
`VK_COLOR_SPACE_HDR10_ST2084_EXT` (HARD: `[Gamescope WSI] Creating swapchain` in
`/home/deck/steam-1332010.log`, this session), so the layer is PQ; the type-1 `.avif` goes through
the PQ→PQ screenshot LUT ("passthrough, though this does apply gain", `steamcompmgr.cpp:405-409`)
with `flHDRInputGain = 1` (the `GAMESCOPE_HDR_INPUT_GAIN` atom is not set on the root window,
measured with `xprop`) and `flBacklightLutGain = 1` (external display, `:604-608`). **Decoded PQ
code → nits is therefore the game's own output, at 10 bits.** Peak is reported as the mean of the
top 0.01 % of pixels (830 px at 4K) with code-1.0 pixels excluded, because an overlay writes raw
1.0 into the PQ buffer — see §1.5.

This corrects CLAUDE.md §5 ("Stray never calls `SetColorSpace1`"): that was measured in the SDR
configuration. Under HDR, `FD3D12Viewport::EnsureColorSpace` maps devices 3/4 to
`DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020` and calls `SwapChain4->SetColorSpace1`
(`WindowsD3D12Viewport.cpp:530-570`, HARD), and vkd3d-proton turns that into the HDR10 colour space
above. In the SDR configuration the same log shows `VK_COLOR_SPACE_SRGB_NONLINEAR_KHR`.

### 1.3 The SDR side is a model, and here is the model

An SDR frame's AVIF/PNG carries the game's 10/8-bit sRGB-encoded codes. On screen, gamescope
composites an SDR layer onto the PQ output through a G22→PQ LUT with
`tonemapping.g22_luminance = flSDROnHDRBrightness` (`steamcompmgr.cpp:373-375`), decoding the
content as **pure gamma 2.2** (`color_helpers.cpp:636-638`: `pow(input, 2.2) * g22_luminance`;
`colorspace_to_eotf` treats sRGB content "as native Gamma 22 curve", `shaders/colorimetry.h`).
`flSDROnHDRBrightness` is driven by the X atom `GAMESCOPE_SDR_ON_HDR_CONTENT_BRIGHTNESS`
(`:8798`, `:7247-7249`), which on the box reads `1128965079 = 0x434aa3d7 = 202.64 f` (measured:
`xprop -root` on `:0` as deck) — Steam's "SDR content brightness" slider. So:

```
SDR on-screen nits = 202.64 * code^2.2          (code = the game's sRGB-encoded output)
```

`--hdr-itm-enabled` is on the live command line. In master the ITM shader path exists
(`shaders/colorimetry.h:172-323`, BT.2446 method A; `composite.h:99-102`) behind the
specialisation constant `c_itm_enable` (`descriptor_set.h:10`, default false) — and **no call of
`CVulkanDevice::pipeline(...)` ever passes `itm_enable = true`** (grep over `src/`; the only uses
of `g_bHDRItmEnable` force a full composite in the three backends). So in master ITM is inert:
the flag only costs a composite. **For 3.16.25 this is SOFT**, and it is the one thing that could
make the on-screen SDR reference brighter than the model above (BT.2446a with the defaults
`g_flHDRItmSdrNits = 100`, `g_flHDRItmTargetNits = 1000`, `rendervulkan.cpp:3779-3780`, would
map SDR white far above 203 nits). If the user's memory of SDR is brighter than 155-nit peaks,
this is the first thing to suspect — and it would be a compositor artefact, not a game property.

**Plainly: HDR numbers below are measured; SDR numbers are `202.64 × code^2.2` and cannot be
verified on this box, because type-4 captures do not carry the composite.** The two are compared
in nits anyway because that is the only common unit; the caveat travels with every SDR figure.

### 1.4 Protocol actually followed

* Every state settled ≥ 30 s before capture (eye adaptation: `AutoExposureSpeedUp 3 / Down 1`
  EV/s by default, `Scene.cpp:491-492`; the game's own values were not read).
* Each experiment: one console line via `stray-console.cmd` (the `StrayConsole` mod echoes
  `[StrayConsole] ran:` into `ue4ss/UE4SS.log`; every command below has its echo line quoted),
  capture, restore, capture.
* The SDR reference cycle (authorised once): backups
  `Engine.ini.bak-hdrref-194544` / `GameUserSettings.ini.bak-hdrref-194544`; SDR config =
  the five `r.AllowHDR` / `r.HDR.*` lines commented (not only `EnableHDROutput` — see below) plus
  the pre-HDR `GameUserSettings.ini` (`bUseHDRDisplayOutput=False`, `Gamma=2.875000`, the user's
  own SDR setting; the HDR-era file carries `Gamma=2.650000`); the game was stopped and the
  reaper waited out (60 s) before editing, because UE rewrites `GameUserSettings.ini` on exit;
  relaunched with `cef_run.py`; restored from the backups the same way and verified byte-identical
  (`cmp`). Why all five lines: `GetTonemapperOutputDeviceParameters` uses the cvar verbatim when
  `Family.bIsHDR` is false (`PostProcessTonemap.cpp:460-467`), so an `r.HDR.Display.OutputDevice=3`
  left in `[SystemSettings]` would PQ-encode into an sRGB swapchain and the reference would be a
  washed-out non-image.
* Menu-vs-gameplay was decided by looking at the capture (menu at 19:48/19:50/19:52; save
  select 19:55; gameplay 19:59). On the user's mid-run instruction, Enter was tapped on the K400
  node (`/dev/input/event7`, `tools/inject.py key ... 28 80`) to load slot 1 — three taps for the
  SDR run, four for the HDR run.

### 1.5 The 10 000-nit overlay

Every HDR capture that contains the NVIDIA DLSS indicator text has 3 569 pixels at PQ code 1.0
(bbox x 8..1047, y 8..20) = **10 000 nits** — the indicator is written as raw 1.0 into the PQ
buffer. With `r.HDR.UI.CompositeMode 0` the game's HUD does the same (8 785 saturated px, bbox to
y 1238). These are excluded from every peak figure; they are also worth knowing about, because on
a 1 500-nit panel that text is the brightest thing on screen by an order of magnitude.

---

## 2. The numbers

All luminances are Rec.2020 Y in nits; "peak" = mean of the top 0.01 % unsaturated pixels.

### 2.1 Same scene (Slums spawn, after load, ≥ 2 min in), SDR reference vs HDR

| capture | time | config | median | p90 | p99 | p99.9 | peak | max |
|---|---|---|---|---|---|---|---|---|
| `SDRref_200045_t1.avif` | 20:00:45 | SDR (model 202.64·c^2.2) | **5.55** | 32.9 | 114.7 | 146.9 | **155.4** | 159.4 |
| `SDRg22_200153_t1.avif` | 20:01:53 | SDR, after `GAMMA 2.2` | 6.08 | 33.9 | 114.7 | 148.9 | (indicator) | — |
| `SDRrest_200413_t1.avif` | 20:04:13 | SDR, after `GAMMA 2.875` | 6.33 | 34.5 | 108.6 | 146.6 | 154.6 | 158.9 |
| `B2_hdr_201047_t1.avif` | 20:10:47 | HDR, device 3 | **0.32** | 2.56 | 29.0 | 70.8 | **89.6** | 101.6 |
| `E2_dev4_201326_t1.avif` | 20:13:26 | HDR, device **4** | 0.386 | 2.80 | 29.7 | 112.7 | 143.7 | 152.9 |
| `E2r_dev3_201415_t1.avif` | 20:14:15 | HDR, device 3 restored | 0.381 | 2.81 | 28.2 | 91.8 | 133.0 | 136.0 |
| `E3_uicomp0_201504_t1.avif` | 20:15:04 | HDR, `r.HDR.UI.CompositeMode 0` | 0.382 | 2.85 | 29.9 | (HUD sat.) | — | — |

Frame-to-frame variation at the same spot (lantern flicker, the musician) is ±30 % on the peak
(B2 89.6 vs E2r 133.0 under identical settings) and ±20 % on the median; read the table with that
in mind. **Ratios SDR/HDR: median 17.2× (+4.11 EV); peak 1.17–1.73× (+0.23..+0.79 EV).**

Per-pixel mapping, SDR reference against B2 (`compare.py`; bins by SDR nits, HDR median inside
the bin):

```
SDR-nits bin        n        HDR median   ratio SDR/HDR (EV)
  0.20..0.50      614882        0.02     15.51 (+3.96)
  0.50..1.00      808581        0.05     14.94 (+3.90)
  1.00..2.00      894864        0.08     17.27 (+4.11)
  2.00..5.00     1622749        0.18     17.71 (+4.15)
  5.00..10.00    1280190        0.42     17.30 (+4.11)
 10.00..20.00    1499634        0.84     16.33 (+4.03)
 20.00..50.00    1129041        2.18     13.56 (+3.76)
 50.00..100.00    310592        7.49      8.65 (+3.11)
100.00..150.00    104023       39.19      3.27 (+1.71)
150.00..200.00      3448       74.53      2.04 (+1.03)
```

### 2.2 Other captures

| capture | time | scene / config | median | peak |
|---|---|---|---|---|
| `B1_hdr_194130_t1.avif` | 19:41:30 | HDR dev 3, dark alley where the cat stood before the cycle | 0.82 | 26.6 |
| `E1_dev4_194232_t1.avif` | 19:42:32 | same, `r.HDR.Display.OutputDevice 4` | 0.82 | 23.4 |
| `E1r_dev3_194342_t1.avif` | 19:43:42 | same, device 3 restored | 0.82 | 25.3 |
| `SDRmenu_195307_t1.avif` | 19:53:07 | SDR main menu (model) | 0.025 | 202.6 (clipped white) |
| `hdr_enc_192342_t1.avif` | 19:23:42 | HDR alley, first encoding test | 0.59 (pre-fix channels) | — |

B1 is not comparable to the SDR reference (different place); it is the alley the user was standing
in when this started, and it shows how dark HDR gets in Stray's darker streets: 0.8-nit median,
27-nit peak, nothing above 64 nits.

---

## 3. The exposure and tone-mapping math on this path, from source

### 3.1 Exposure (both configurations identical)

* `FSceneView::StartFinalPostprocessSettings` resets `FinalPostProcessSettings.SetBaseValues()`,
  applies the `r.DefaultFeature.*` project defaults, then `World->AddPostProcessingSettings`
  blends the volumes (`SceneView.cpp:1766-1846`). Volumes that are not unbound contribute weight
  0 when the view location is outside `BlendRadius` (`World.cpp:7838-7849`) — which is why writing
  `AutoExposureBias` on `PostProcessVolume_slums_0` / `_trans_momoflat` did nothing: the
  reflection probe read `unbound=false` and the view was outside them (HARD mechanism, the
  "outside" part measured by the earlier probe).
* `ULocalPlayer::CalcSceneView` then blends, in order: camera-anim blends, **the view target's
  `POV.PostProcessSettings` at `POV.PostProcessBlendWeight`** (`LocalPlayer.cpp:857-874`),
  photography, stereo, then `EndFinalPostprocessSettings`. The POV comes from
  `APlayerCameraManager::UpdateViewTarget` → `SetBaseValues()`, weight 1.0, then
  `CamActor->GetCameraComponent()->GetCameraView` or `Target->CalcCamera`
  (`PlayerCameraManager.cpp:501-527`, `:480-497`); `AActor::CalcCamera` uses the **first active
  `UCameraComponent`** (`Actor.cpp:2686-2705`); `UCameraComponent::GetCameraView` copies its
  `PostProcessSettings` when `PostProcessBlendWeight > 0` (`CameraComponent.cpp:340-345`). That is
  the whole chain behind the earlier observation that writing the cat camera component's
  `bOverride_AutoExposureBias`/`AutoExposureBias` reached `ViewTarget.POV` (`biasOvr=true`) — every
  link is HARD. What re-writes that struct every ~8 s after a camera respawn is game code
  (`BP_CameraThirdPerson_C`), not engine code: UNCONFIRMED cause, but expected of a Blueprint
  camera that sets its own post-process on begin-play.
* `OverridePostProcessSettings` lerps only members whose `bOverride_*` is set, with the weight
  clamped to [0,1] (`SceneView.cpp:1317-1330`, `LERP_PP(AutoExposureBias)` at `:1428`).
* Exposure itself (`PostProcessEyeAdaptation.cpp:263-420`, `.usf:106-134`,
  `PostProcessHistogramCommon.ush:158-180`): the histogram average between `LowPercent`/`HighPercent`
  is clamped to `[AutoExposureMinBrightness, AutoExposureMaxBrightness] × 0.18` (defaults
  0.03 / 8.0, `Scene.cpp:186-187`; in "extended luminance range" mode they are EV100 — Stray does
  not ship `r.DefaultFeature.AutoExposure.ExtendDefaultLuminanceRange`, so they are luminances),
  `TargetExposure = clamped / 0.18`, adapted in log2 at `SpeedUp`/`SpeedDown` EV/s (exponential
  within `r.EyeAdaptation.ExponentialTransitionDistance`, linear beyond), and the frame is scaled by
  `ExposureScale = 2^AutoExposureBias / SmoothedExposure` (`:177-191`, `.usf:118-130`). So a bias
  step of 0.05 EV is a 3.5 % change in linear light. **"0.30 → 0.35 looked like a large jump" is
  not explained by this math**; the candidates are the adaptation transient after the write and
  the game's own rewrite landing between the two looks. NOT EXPLAINED.
* Pre-exposure (`View.PreExposure`) is an internal scale removed before tone mapping and does not
  change the output; `r.EyeAdaptation.PreExposureOverride` is therefore not a brightness lever.

### 3.2 The colour-grading LUT is where SDR and HDR part ways (HARD, `PostProcessCombineLUTs.usf`)

The tonemap pass only samples a 32³ LUT (`PostProcessTonemap.usf:124-127`: for HDR devices the
lookup key is `LinearToST2084(LinearColor * 100)`); the LUT is rebuilt **every frame** for the
primary view (`PostProcessing.cpp:753-756`) by `CombineLUTsCommon`:

```
LinearColor  = ST2084ToLinear(LUTEncodedColor) / 100          (HDR devices, :212-217)
BalancedColor= WhiteBalance(LinearColor)
ColorAP1     = sRGB_2_AP1 * BalancedColor; ExpandGamut (:233-260); ColorCorrectAll (:262)
GradedColor  = AP1_2_sRGB * ColorAP1              // "Store for ... Linear HDR output" (:265)
ColorAP1     = lerp(ColorAP1, FilmToneMap(ColorAP1), ToneCurveAmount)   (:285-287)
FilmColor    = max(0, AP1_2_sRGB * ColorAP1)      (:293)
FilmColor    = ColorCorrection(FilmColor)         // r.Color.Min/Mid/Max polynomial (:339)
FilmColorNoGamma = lerp(FilmColor * ColorScale, OverlayColor.rgb, OverlayColor.a)   (:342)
GradedColor  = lerp(GradedColor * ColorScale, OverlayColor.rgb, OverlayColor.a)     (:344)
FilmColor    = pow(FilmColorNoGamma, InverseGamma.y)      // 2.2 / DisplayGamma (:350)

device 0 (sRGB):      OutDeviceColor = LinearToSrgb(FilmColor)                          (:357-367)
device 3 (ACES1000):  OutDeviceColor = LinearToST2084(AP1_2_Output * ACESOutputTransforms1000(GradedColor))  (:381-390)
device 4 (ACES2000):  ... ACESOutputTransforms2000(GradedColor) ...                       (:394-403)
```

Consequences, all HARD:

* **The HDR branch consumes `GradedColor`, not `FilmColor`.** Everything applied between `:285`
  and `:350` — the film tone curve (`FilmSlope/Toe/Shoulder/BlackClip/WhiteClip`, defaults
  0.88/0.55/0.26/0/0.04, `Scene.cpp:415-419`), `ToneCurveAmount`, `BlueCorrection`,
  `r.Color.Min/Mid/Max` (`PostProcessCombineLUTs.cpp:76-90`, `MappingPolynomial`), and the display
  gamma `InverseGamma.y` — **does not exist in the HDR image.** Only white balance, the
  shadow/mid/highlight colour correction (`ColorGain`, `ColorGamma`, ...), `ExpandGamut`,
  `ColorScale` and the fade `OverlayColor` reach the ODT.
* `ACESOutputTransforms1000` (`TonemapCommon.ush:309-317`) is `aces = sRGB_2_AP0 * (color * 1.5)`,
  `RRT` (`ACES.ush:662-705`; tonescale `segmented_spline_c5_fwd` with midPoint 0.18 → 4.8, `:305-320`),
  then `ODT_1000nits` (`:1070-1093`: minPoint 0.0001 nits, **midPoint 10.0 nits**, maxPoint 1000,
  low/high slopes 3.0/0.06, minus 3.5e-5). `ODT_2000nits` (`:1134-1157`) has the **same 10-nit
  midPoint** and maxPoint 2000. Output is linear nits, PQ-encoded by `LinearToST2084`
  (`GammaCorrectionCommon.ush:155-173`).
* `r.TonemapperGamma` only rewrites device 0/1 to "explicit gamma" (`PostProcessTonemap.cpp:478-480`)
  and `InverseGamma.z` is consumed only in that branch (`usf:434`).
* `r.DefaultFeature.AutoExposure.Bias` is read once, at construction of `FCameraExposureSettings`
  / `FPostProcessSettings` (`Scene.cpp:169`, `:489-491`) — a default for settings objects, replaced
  by any `bOverride_AutoExposureBias` blend. Changing it at runtime cannot affect an existing view.
* `r.EyeAdaptation.LensAttenuation` only enters through `LuminanceMaxFromLensAttenuation`, which
  returns 1.0 unless `ExtendDefaultLuminanceRange` is on (`PostProcessEyeAdaptation.cpp:110-123`).
* `r.ExposureOffset` — the one direct EV offset — is `#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)`
  (`SceneView.cpp:1975-1978`). Gone in this build.
* UE5's `r.HDR.Aces.SceneColorMultiplier` / `r.HDR.Aces.GamutCompression`: zero hits in
  `Renderer/Private/PostProcess` and `Shaders/Private` at 4.27.2. Not available.

### 3.3 The HDR plumbing (HARD)

`HDRSettingChangedSinkCallback` (`UnrealEngine.cpp:720-768`) runs only when
`r.HDR.EnableHDROutput` *changes*, reads `GetCurrentHDRDisplayNits()` (0 → 1000) and calls
`ChooseHDRDeviceAndColorGamut`, which for AMD/NVIDIA returns **OutputDevice 5 (1000 nits) or 6,
gamut 2** — scRGB (`WindowsPlatformMisc.h:238-247`). A non-editor D3D12 build's HDR swapchain is
`PF_A2B10G10R10` (`WindowsD3D12Device.cpp:855-870`), for which the comment itself says devices 3/4;
hence `StrayHdr`'s rewrite to 3, and hence `HDRDisplayOutputNits` is inert here: its only readers
are the chooser (5 vs 6) and the viewport's HDR metadata (`WindowsD3D12Viewport.cpp:414-427`,
`DisplayMaxOutputNits` 1000/2000 by device). `r.HDR.UI.CompositeMode` / `r.HDR.UI.Level`
(`SlateRHIRenderer.cpp:48-60`, composite only when HDR is enabled `:711-716`) run
`CompositeUIPixelShader.usf:117-146`: scene PQ → linear, blended with the UI at `UILevel`, back to
PQ; where the UI alpha is 0 the scene is bit-exact passthrough, and under translucent UI a
Reinhard to `UILevel` is applied. Measured E3: turning it off changes the scene median by 0 and
puts the HUD at code 1.0.

### 3.4 What the curves predict, and where measurement disagrees

`curves.py` ports the neutral-axis path of both branches (RRT/ODT splines with their constants,
`FilmToneMap` with the defaults, the ×1.5, `LinearToSrgb`, the gamescope 202.64 × code^2.2 model):

```
 scene   SDR nits   HDR1000   HDR2000   SDR/HDR   EV gap
 0.020      0.49      0.73      0.73     0.66    -0.60
 0.050      4.17      2.86      2.86     1.46    +0.54
 0.180     36.95     17.36     17.40     2.13    +1.09
 0.500    106.78     62.23     63.72     1.72    +0.78
 1.000    147.97    133.27    139.34     1.11    +0.15
 2.000    175.41    256.42    279.58     0.68    -0.55
 16.000   202.64    781.33   1156.87     0.26    -1.95
```

So the engine's own curves already make HDR mid-tones **0.5–1.1 EV darker** than SDR at 202.64-nit
white while letting highlights go to 1000 nits; the 2000-nit device differs only above ~100 nits.
The measured gap is **4 EV** in the mid-tones. Adding the game's SDR `Gamma=2.875` as a display
gamma (`InverseGamma.y = 2.2/2.875`) widens the modelled gap to ~2 EV at mid-grey, still 2 EV
short. **The remaining factor of ~4 is UNEXPLAINED** (§6); it is not the UI composite (E3), not the
output device (E1/E2), and the per-bin ratio's flatness says it behaves like a constant pre-ODT
gain, not like a curve change. The recommendation in §7 is therefore fitted to the *measured*
mapping through the (HARD) ODT, not to the model.

---

## 4. Candidate levers, ranked by how close they bring the HDR peak to the SDR reference

Inverting the ODT on the measured bins (`hdr_nits(k·x)` with the HARD ODT, `curves.py`):

```
SDR nits  HDR nits  scene_in(HDR)  scene_needed  gain k   EV
    0.72      0.05      0.00325      0.01977     6.09  +2.61
    3.20      0.18      0.00805      0.05396     6.70  +2.75
    7.00      0.42      0.01388      0.09309     6.71  +2.75
   14.00      0.84      0.02190      0.15330     7.00  +2.81
   30.00      2.18      0.04153      0.27504     6.62  +2.73
   70.00      7.49      0.09766      0.55318     5.66  +2.50
  120.00     39.20      0.34087      0.90364     2.65  +1.41
  170.00     74.50      0.58409      1.27700     2.19  +1.13
peak: SDR 155 vs HDR 90..133 -> k = 1.16 .. 1.68
```

| # | lever | lives in | HDR peak vs SDR 155 | mid-tones (median 0.32 → ?) | cost | provenance |
|---|---|---|---|---|---|---|
| 1 | **`APlayerCameraManager::ColorScale` = (k,k,k), `bEnableColorScaling = true`** — a uniform gain on `GradedColor` before the ODT (`LocalPlayer.cpp:763-769` → `View.ColorScale` → `PostProcessCombineLUTs.cpp:167` → `usf:344`) | engine, written by reflection (Lua `StrayHdr` or the plugin); `UPROPERTY`s `ColorScale` / `bEnableColorScaling` (`PlayerCameraManager.h:228`, `:417`); `SetDesiredColorScale` is *not* a UFUNCTION (`:688`) so write the fields directly | k=1.5: 155→~200 (match/slightly over); k=1.7: ~220 | k=1.5: ×1.5 (still 11× under SDR); **k=6.7: matches (5–7 nits) but peak → ~570–630 nits (4× SDR)** | zero | chain HARD; whether the game itself drives `ColorScale` (it is set to `DefaultColorScale` (1,1,1) at `InitializeFor`, `:826-828`, and interpolated in `DoUpdateCamera`, `:988-1003`) UNCONFIRMED — the earlier probe never read it |
| 2 | `AutoExposureBias` on the active camera component (`2^bias`, `PostProcessEyeAdaptation.cpp:177-191`) | engine, reflection | +0.6 EV ≈ lever 1 at k=1.5 | +2.75 EV ≈ k=6.7 | zero | HARD math; the game rewrites the struct ~8 s after each camera respawn (measured earlier) — rejected by the user for that reason |
| 3a | **plugin: uniform gain in PQ domain** at the present stage (`nits_out = k·nits_in`) | plugin (`src/backend_native/present_owner.*`, `src/nr_stage.*`) | k=1.7: exact peak match by construction | ×1.7 only (0.32→0.55) | one 4K compute pass, ~0.1–0.3 ms est. (SOFT); no compute dispatch has ever been recorded on that list (CLAUDE.md) | HARD in principle; UNCONFIRMED implementation |
| 3b | **plugin: PQ-domain *curve*** — gain g(n) ≈ 6.7 for n ≲ 30 nits falling to ≈1.5 at n ≳ 120 nits (the measured per-bin table *is* the curve) | plugin, same stage | match (peak ≈ 155–200) | match (≈ 5–7 nits) | same pass; re-quantises an already tone-mapped 10-bit image — shadow banding risk (SOFT: PQ 10-bit has ~0.002-nit steps at 0.3 nits, gain 6.7 keeps it under one JND) | the only lever that satisfies both metrics; HUD and the 10 000-nit indicator are inside the frame at that point and would need masking; UNCONFIRMED |
| 4 | `r.HDR.Display.OutputDevice 4` (2000-nit ODT) | engine cvar, live | +8 % peak (133→144), p99.9 +23 % | 0 (0.381→0.386) | zero | measured E2/E2r; predicted by the shared 10-nit midPoint |
| 5 | `HDRDisplayOutputNits` (1000→2000) | engine setting | only via device 4/6 selection and metadata | 0 | zero | HARD (`WindowsPlatformMisc.h:243`, `WindowsD3D12Viewport.cpp:420`); inert while `StrayHdr` forces 3 |
| 6 | Colour grading `ColorGamma` / `ColorGain` (`ColorCorrectAll`, pre-ODT) — shapes mids without the peak | engine, needs a post-process carrier the game does not rewrite: a UE4SS-spawned unbound `PostProcessVolume` at high priority | tunable | tunable | zero | mechanism HARD (`usf:262`); carrier UNCONFIRMED |
| 7 | gamescope `GAMESCOPE_HDR_INPUT_GAIN` atom (`flHDRInputGain`, `steamcompmgr.cpp:8799`, applied to the PQ layer "passthrough (though this does apply gain)" `:405-409`) | compositor, `xprop -root -f GAMESCOPE_HDR_INPUT_GAIN 32c -set ...` as deck | uniform nits gain: k=1.7 matches peak | ×1.7 only | zero | HARD master, SOFT box; **not tested** in this session (outside the authorised experiment set) |
| 8 | `r.Color.Mid`, `r.TonemapperGamma`, film curve settings, the game's Gamma slider, `r.DefaultFeature.AutoExposure.Bias`, `r.EyeAdaptation.LensAttenuation` | engine | 0 | 0 | — | inert on the HDR path by source (§3.2); the first two and the bias were also measured inert |
| 9 | `r.EyeAdaptationQuality 0` / eye adaptation off | engine | uncontrolled | "far too bright" | — | with adaptation off `GetEyeAdaptationParameters` locks both white points to 1.0 and compensation to 1.0 (`:336-347`), so the scene's own −3.75..+1 EV biases stop applying — the brightness jump is the loss of the game's bias, not a lever. SOFT on the `r.EyeAdaptationQuality`→showflag link (not traced) |

The **peak metric ranks 3a, 7 and 1(k≈1.5) equal and first**; the **visible problem** (§2's 4 EV
in the mid-tones) is answered only by 1(k≈6.7), 2, 3b or 6, and 3b is the only one that keeps
the peak where the user asked for it.

---

## 5. Live experiments (timestamps local, UTC in the UE4SS log = local + 3 h)

| # | command / action | ran (UE4SS.log) | captured | result |
|---|---|---|---|---|
| B1 | — HDR baseline, dark alley | — | 19:41:30 `B1_hdr_194130_t1.{avif,png}`, `_t4.png` | median 0.82, peak 26.6 nits |
| E1 | `r.HDR.Display.OutputDevice 4` | 22:41:37Z | 19:42:32 `E1_dev4_194232_t1.avif` | median 0.82 (=), peak 23.4 (noise) — inert at this level |
| E1r | `r.HDR.Display.OutputDevice 3` | 22:42:37Z | 19:43:42 `E1r_dev3_194342_t1.avif` | back to 0.82 / 25.3 |
| SDR | config swap, relaunch 19:47:14; Enter ×3 (user-authorised) 19:55–19:57; gameplay 19:59 | — | 20:00:45 `SDRref_200045_t1.{avif,png}`, `_t4.png`; menu `SDRmenu_195307_*` | **reference: median 5.55, peak 155.4 (model)** |
| G1 | `GAMMA 2.2` (UEngine exec, `UnrealEngine.cpp:4245`, not under a Shipping guard) | 23:01:06Z | 20:01:53 `SDRg22_200153_t1.avif` | median 6.08 — **no effect** (see §6) |
| G2 | `GAMMA 2.875` | 23:02:10Z | 20:04:13 `SDRrest_200413_t1.avif` | median 6.33 — no effect |
| HDR | config restored from `*.bak-hdrref-194544` (byte-identical), relaunch 20:06:17; Enter ×4 20:08; gameplay 20:08:51 | `stray-hdr.txt` 20:06:26 `OutputDevice 5 -> 3` | 20:10:47 `B2_hdr_201047_t1.{avif,png}`, `_t4.png` | **HDR at the reference spot: median 0.32, peak 89.6** |
| E2 | `r.HDR.Display.OutputDevice 4` | 23:11:03Z | 20:13:26 `E2_dev4_201326_t1.avif` | median 0.386, peak 143.7, p99.9 112.7 |
| E2r | `r.HDR.Display.OutputDevice 3` | 23:13:41Z | 20:14:15 `E2r_dev3_201415_t1.avif` | median 0.381, peak 133.0, p99.9 91.8 |
| E3 | `r.HDR.UI.CompositeMode 0` | 23:14:30Z | 20:15:04 `E3_uicomp0_201504_t1.avif` | median 0.382 — composite is not the darkening; HUD at code 1.0 |
| E3r | `r.HDR.UI.CompositeMode 1` | 23:15:17Z | — | restored |

Encoding tests: `hdr_base_191627_t{1,4}.png` (t4 ≡ t1, 0.42/255), `hdr_enc_192342_t1.{avif,png}`,
`_t3.avif` (PNG = g22(nits/500) fit), `SDRref` t1/t4/avif (t4 ≡ t1 1.44/255; avif channel order).

---

## 6. Earlier observations, explained or not

| observation | verdict | why |
|---|---|---|
| `r.Color.Mid` inert | **explained (HARD)** | applied to `FilmColor` at `usf:339`; the HDR branch uses `GradedColor` (`:384`, `:397`) |
| `r.TonemapperGamma` inert | **explained (HARD)** | `PostProcessTonemap.cpp:478-480`: only devices 0/1 are remapped; `InverseGamma.z` used only at `usf:434` |
| `r.DefaultFeature.AutoExposure.Bias` ±3 inert | **explained (HARD)** | construction-time default (`Scene.cpp:169`, `:489`); any `bOverride_AutoExposureBias` blend replaces it (`SceneView.cpp:1428`) |
| `r.HDR.Display.OutputDevice 4` inert | **explained (HARD + measured)** | both ODTs pin mid-grey at 10 nits; they differ only above ~100 nits — measured +8 % peak, 0 median |
| `r.EyeAdaptation.LensAttenuation` inert | **explained (HARD)** | reaches nothing unless `ExtendDefaultLuminanceRange=1` (`PostProcessEyeAdaptation.cpp:110-123`); Stray does not set it (`docs/game-config/`, live `Engine.ini`) |
| `r.EyeAdaptationQuality 0` far too bright | **explained (mechanism HARD, trigger SOFT)** | adaptation off ⇒ white points 1.0 and compensation 1.0 (`:336-347`): the game's negative biases stop applying |
| volumes' `AutoExposureBias` inert | **explained (HARD)** | `DoPostProcessVolume` weight 0 outside a bound volume's blend radius (`World.cpp:7838-7849`); both Slums volumes are `bUnbound=false` |
| camera component bias reached the POV | **explained (HARD)** | `CalcCamera` → first active camera component → `GetCameraView` copies `PostProcessSettings` at `PostProcessBlendWeight` → `UpdateViewTarget` → `CalcSceneView` `OverridePostProcessSettings` (`LocalPlayer.cpp:874`) |
| the struct is rewritten ~8 s after a respawn | **not explained** (game Blueprint, not engine) | consistent with a camera Blueprint applying its own post-process on begin-play; not in the mirror |
| brightness "did not track the number" (0.30→0.35 "large jump") | **not explained** | the math gives +3.5 % linear for +0.05 EV; a jump that size is either the adaptation transient (`SpeedUp 3 / SpeedDown 1` EV/s) or the rewrite landing between looks |
| `GAMMA` console command inert in SDR | **not explained** | it is compiled (`UnrealEngine.cpp:4245`, no Shipping guard), the route reaches `GEngine->Exec` (`GameViewportClient.cpp:2951`), and `InverseGamma.y = 2.2 / Family.RenderTarget->GetDisplayGamma()` (`PostProcessTonemap.cpp:485`) resolves to `GEngine->DisplayGamma` (`UnrealClient.cpp:248-261`) — unless `FSceneViewport::ViewportGammaOverride` is set (`SceneViewport.cpp:1556-1562`), which is the leading candidate. By the same token the game's `Gamma=2.875` setting is probably not `DisplayGamma` |
| measured 4 EV mid-tone gap vs 0.5–2 EV modelled | **not explained** | flat across two decades ⇒ a constant pre-ODT gain the SDR path has and the HDR path lacks (or the reverse); candidates: the game's brightness setting applied through a post-process material or `ColorGamma`, or an SDR-only exposure term in game code; not the UI composite (E3), not the ODT choice (E2) |
| "SDR looks brighter" could be gamescope ITM | **partly answered** | in master ITM is inert (`c_itm_enable` never set true); on 3.16.25 SOFT. Even with ITM inert the modelled SDR is 17× brighter in the mid-tones, so the compositor is not needed to explain the complaint — but if ITM is live on the box, the true SDR reference is *brighter* than every SDR number here |

---

## 7. Recommendation

**Lever: `APlayerCameraManager::ColorScale` (with `bEnableColorScaling = true`), written by
reflection from the existing `StrayHdr` Lua helper (it already owns the HDR fix-up and polls only
cvars) or from the plugin.** It is the one place in 4.27 where a uniform gain reaches the ACES ODT
input that the game does not overwrite every few seconds (UNCONFIRMED: the earlier probe never
read `ColorScale`; the first run must log it), costs nothing, and does not feed back into eye
adaptation (the LUT sits after the histogram).

**Value that matches the reference on the user's metric: k = 1.5 (+0.6 EV).** Measured HDR peak
90–133 nits × the ODT's response to a 1.5× input ≈ 160–200 nits against the SDR reference's 155
(match to slightly over). **Say it plainly: this leaves the mid-tones 11× darker than SDR** — the
median goes from 0.32 to about 0.5 nits against 5.6 — because the peak was never the problem. The
value that matches the mid-tones is **k = 6.7 (+2.75 EV)**, flat across every bin from 0.35 to 30
SDR-nits, and it takes the peak to ~600 nits (4× SDR; on a 1000-nit ODT feeding a 1499-CLL panel
that is ordinary HDR headroom, not clipping). Ship the key with a default in between only if the
user judges it by eye; **do not ship the plugin's PQ-domain gain for the peak metric alone**, it
buys the same ×1.7 everywhere and nothing for the darkness.

**If both the peak and the mid-tones must land on the SDR numbers**, the only lever is the
plugin's present-stage **PQ-domain curve** (§4 row 3b): decode PQ → nits, apply the measured
per-bin gain (≈6.7 below ~30 nits, easing to ≈1.5 above ~120), re-encode, with the HUD and the
DLSS indicator masked out. It is a re-tonemap of a tone-mapped image, it has never been run on
that command list, and it should be built only after the `ColorScale` lever has been judged.

**Verification, so the next round trip is one launch:** capture `gamescopectl screenshot
<path>.avif 1` at the Slums spawn ≥ 2 min after the load, run `hdrcalc/analyze.py <decoded>.16.png`,
and read median and peak against **5.6 / 155 nits**. Any PNG number is not evidence (§1.1).

**Box state at the end of this session (20:18 local):** game running (pid 132346), HDR configuration
restored and byte-identical to the pre-cycle backups (`Engine.ini.bak-hdrref-194544`,
`GameUserSettings.ini.bak-hdrref-194544`, both left in place), swapchain
`VK_COLOR_SPACE_HDR10_ST2084_EXT`, `StrayHdr` reported `OutputDevice 5 -> 3` at 20:06:26, every
experiment cvar restored (`OutputDevice 3` at 23:13:41Z, `UI.CompositeMode 1` at 23:15:17Z),
`stray-console.cmd` empty, no processes of ours left running; `/tmp/inject.py` was staged on the
box for the user-requested Enter presses and `/tmp/hdrshots/` (570 MB) holds the captures.
