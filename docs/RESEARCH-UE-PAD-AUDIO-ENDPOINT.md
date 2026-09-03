# How Unreal drives the PS5 pad's audio ports and device properties — the contract, read from Epic's source

Read 2026-09-03 against `EpicGames/UnrealEngine` (the user's own access, `gh api`, pull-only).
Companion to `docs/STRAY-DUALSENSE.md` — this file does **not** repeat §1/§14/§16/§18 of that
document; it answers one question they leave open: *what does the engine's real endpoint
implementation do, so the plugin can mirror the contract instead of guessing it.*

Labels as in `CLAUDE.md` §0.5: **HARD** = read in the cited source or measured; **SOFT** = a
claim inferred from HARD facts or from a comment rather than code; **UNCONFIRMED** = a design
or expectation that has not run anywhere.

Every engine citation is `path:line @ ref`. Refs used, all resolved on 2026-09-03:

| ref | commit | `Engine/Build/Build.version` |
|---|---|---|
| `release` | `16d75d84714512edfb744e1fd0a59e9c74d57873` | **5.8.2** (`BranchName: UE5`) |
| `5.8` | `86d8588a019896c7f2227be98b5b4e31cbe5e9cc` | 5.8.2 |
| `ue5-main` | `3dbf60c8380da77526883316431c49953d5027e6` | 5.9.0 |
| `4.27` | `3abfe77d0b24a6d8bacebd27766912e5a5fa6f02` | — (Stray's engine, 4.27.2) |

`release` and `ue5-main` carry **byte-identical** blobs for the two files that matter most
(`WinDualShock.cpp` = `0a88aee0…`, `IAudioEndpoint.h` = `ce08ca9d…`), so "latest release" and
"main" are the same source for this question. Lines below are from `release` unless marked.

---

## 0. The verdict in six lines

1. **The PS5 pad-audio implementation itself is not reachable** — see §1. Nothing below
   speculates about its internals.
2. **The contract it must satisfy is entirely public, and it is byte-for-byte the same in
   4.27 and 5.8**: an `IAudioEndpointFactory` registered as the modular feature
   `"External Audio Endpoint"` under the type name `"Vibration Output"` or
   `"Pad Speaker Output"`, producing an `IAudioEndpoint` from which the engine's own
   `FMixerSubmix::ProcessAudioAndSendToEndpoint` pulls — rendered at the device's channel
   count, **downmixed to 2 channels with the engine's AC-3-style stereo matrix**, resampled to
   48 kHz, and delivered as **256-frame interleaved stereo float blocks on the audio render
   thread**. (§2, §3)
3. **Epic ships the Windows half of exactly this in the public tree**, in the *same* plugin
   Stray cooked: `Engine/Plugins/Runtime/Windows/WinDualShock`. It is the Sony platform
   controller class (`FPlatformControllers` from the restricted `ApplicationCore_Sony` /
   `ApplicationCore_PS5` modules) plus a Windows audio back end: **XAudio2, one 4-channel
   48 kHz float stream on the pad's WASAPI endpoint, frames laid out `[speakerL, speakerR,
   vibL, vibR]`** — which is the FL/FR/RL/RR mapping the plugin already uses. (§4)
4. **In 4.27 that plugin had no audio endpoints at all.** The endpoint half appeared in
   UE 5.0 (first commit 2021-11-18). So on Stray's engine the two endpoint types had no
   Windows factory by construction, which is what §14 measured (the dummy factory). (§5)
5. **UE5 made the device properties public and typed** — `FInputDeviceLightColorProperty`,
   `FInputDeviceTriggerFeedbackProperty`, `…Resistance…`, `…Vibration…` — and they are one
   struct per Sony trigger mode. Stray's 4.27 licensee names `SonyLightColor` /
   `PS5TriggerEffect` are the pre-public shape of the same dispatch. (§6)
6. **What the plugin gets right**: rate, per-lane channel count, channel-pair mapping, the
   render-thread → lock-free ring → device-thread shape, linear resampling, zero-fill on
   underrun. **What it gets wrong against the contract**: the 8→2 fold (it keeps FL/FR and
   drops C/SL/SR/BL/BR, where the engine folds them in at 0.707), and — structurally — it
   taps a re-parented submix instead of being the endpoint. (§3.3, §7)

---

## 1. What was reachable, and what was not

**Reachable (HARD).** `EpicGames/UnrealEngine`, permissions `pull: true, push: false`. Branches
`release`, `ue5-main`, `5.8`, `5.7`, `5.6`, `5.5`, `5.2`, `5.1`, `5.0`, `4.27` all resolve.
Contents and code search work.

**Not reachable (HARD).**

* `Engine/Platforms/` on `release` and on `ue5-main` contains exactly `Android`, `IOS`,
  `VisionOS`, `Windows`. There is no `PS5`, `PS4` or `Sony` directory. On `4.27` the
  `Engine/Platforms` path does not exist at all (404).
* No Sony platform repository is visible to this account. `EpicGames/UnrealEngine-PS5`,
  `EpicGames/PS5`, `EpicGames/UnrealEngine-Sony` all 404; the org listing
  (`orgs/EpicGames/repos?type=all`, 100 rows) shows the private repos this account can see as
  `UnrealEngine`, `UnrealTournament`, `ARTv2`, `zen`, `UGCExample`, `Shave-And-A-Haircut` —
  none is a console tree.
* Code search across the whole repo for the Sony-side symbols the Windows plugin depends on
  returns **only the Windows plugin's own files**: `ESonyControllerType` (2 files),
  `FPlatformControllers` (1), `ApplicationCore_Sony` (1), `LibScePad` (3),
  `SCE_USER_SERVICE_MAX_LOGIN_USERS` (2), `PadSpeaker` (4). `SonyEngine`, `sceAudioOut`,
  `SonyLightColor`, `PS5TriggerEffect`: **0 hits**. The whole public contact surface with the
  PlayStation pad is one plugin, and every symbol it *uses* from the Sony side is defined in a
  tree this account cannot read.

**Consequence.** The PS5 implementation of `"Vibration Output"` / `"Pad Speaker Output"` is
in the restricted `ApplicationCore_PS5` (or a sibling) module and is **unreachable**. This
document extracts everything the public contract implies and everything the Windows
implementation — which reuses the PS5 controller class — reveals about it, and stops there.

---

## 2. How UE5 reaches a PS5 pad: the layers, and which layer is public

```
game content            UEndpointSubmix { EndpointType = "Vibration Output" | "Pad Speaker Output" }
                        SoundAttenuation.SubmixSendSettings / SoundClass.DefaultSubmix   (Stray's assets, HARD)
                                   │
engine, PUBLIC          FMixerSubmix::Init  -> IAudioEndpointFactory::Get(EndpointType)         §3.1
identical 4.27 / 5.8    FMixerSubmix::ProcessAudioAndSendToEndpoint  (render, downmix, resample) §3.2-3.3
                        IAudioEndpoint::RunCallbackSynchronously -> OnAudioCallback(256 frames) §3.4
                                   │
platform module,        IAudioEndpointFactory subclass registered by the platform's IInputDeviceModule
RESTRICTED on PS5,      IAudioEndpoint subclass forwarding to the platform pad-audio device
PUBLIC on Windows       FPlatformControllers (ApplicationCore_Sony): pad state, force feedback,
                          SetDeviceProperty, SetAudioGain(padSpeaker, headphones, mic, output)      §4
                                   │
Sony library            libScePad  (pad.h, pad_audio.h)  +  the console's pad audio ports (PS5)
                                                         +  XAudio2 -> pad WASAPI endpoint (Windows)
```

Three facts fix where the public/restricted line runs:

* **The endpoint type names are platform-agnostic by Epic's own statement.** The GameInput
  plugin registers under the same `"Vibration Output"` name *"so existing content ("Vibration
  Output" endpoint submixes) routes here on GameInput platforms without requiring asset
  changes"* (`GameInputHapticEndpointFactory.cpp:45-48`), and refuses to register if the
  WinDualShock factory already owns the name (`:21-36`). Content authored once — Stray's
  `VibrationEndpointSubmix` / `ControllerEndpointSubmix` — resolves per platform to whichever
  module registered that name. **HARD** for the mechanism; **SOFT** that PS5 uses the identical
  strings (the only public evidence is that Epic calls them platform-agnostic and that the
  Windows plugin, which is the Sony controller class, uses them).
* **The Windows plugin's endpoints are described by Epic as PS5 virtual devices**:
  *"these endpoints can be used to send audio to arbitrary virtual audio devices supported by
  the PS5"* (`WinDualShockExternalEndpoints.h:12-14`), and its port enum is
  `{ PadSpeakers, Vibration /* ignored on DualShock controllers */ }`
  (`WinDualShock.h:20-25`). **HARD** (the comment); the PS5 device names it maps to are not in
  the tree.
* **The controller class is shared with the console.** `class FWinDualShockControllers :
  public FPlatformControllers` (`WinDualShockControllers.h:17`), with
  `PrivateIncludePathModuleNames = { "ApplicationCore_Sony", "ApplicationCore_" + PlatformName }`
  and `LIBSCEPAD_PLATFORM_INCLUDE` (`WinDualShock.Build.cs:28-51`, `WinDualShock.cpp:37`).
  Everything the Windows plugin calls on `Controllers.*` is therefore PS5 code compiled for
  Windows: `ConnectStateToUser(SCE_USER_SERVICE_STATIC_USER_ID_1 + UserIndex, UserIndex)`
  (`WinDualShock.cpp:106-113`), `SetForceFeedbackChannelValue(s)` (`:155-163`),
  `SetDeviceProperty(ControllerId, Property)` (`:175-178`), `SetAudioGain(padSpeaker,
  headphones, mic, output)` (`WinDualShockControllers.h:28-38`), `GetSupportsAudio`,
  `GetControllerType` (`:45-53`). **HARD.**

---

## 3. The endpoint contract, as the engine defines it

### 3.1 Resolution: name → factory → endpoint

* `UEndpointSubmix` carries `FName EndpointType`, `TSubclassOf<UAudioEndpointSettingsBase>
  EndpointSettingsClass`, `UAudioEndpointSettingsBase* EndpointSettings`
  (`Sound/SoundSubmix.h:573-590`; 4.27 `:459-478`). `GetAudioEndpointForSubmix()` is one line,
  `return IAudioEndpointFactory::Get(EndpointType);` (`SoundSubmix.cpp:1114-1116`; 4.27
  `:888-890`). **HARD.**
* `IAudioEndpointFactory::Get` walks the modular-feature list `"External Audio Endpoint"`
  (`IAudioEndpoint.cpp:145`), returns the first factory whose `GetEndpointTypeName()` equals
  the name, otherwise logs *"No endpoint implementation for %s found for this platform.
  Endpoint Submixes set to this type will not do anything."* and returns the **dummy
  factory** (`:159-181`; 4.27 `:155-175`). **HARD.** One 5.x addition: the factory must also
  have `bIsImplemented == true` (`:172`); 4.27 does not check it (`:166`).
* `FMixerSubmix::Init`, `UEndpointSubmix` branch: `NumChannels = MixerDevice->
  GetNumDeviceChannels(); NumSamples = NumChannels * NumOutputFrames;` then
  `SetupEndpoint(EndpointFactory, EndpointSettings)` (`AudioMixerSubmix.cpp:478-487`; 4.27
  `:315-324`). **The endpoint submix renders at the DEVICE channel count** — on the box that
  is 8 (`STRAY-DUALSENSE.md` §15 measured `ch=8`). **HARD.**
* `SetupEndpoint`: `check(!ParentSubmix.IsValid())`; if the submix has no `EndpointSettings`
  object it uses `InFactory->GetDefaultSettings()`, calls `GetProxy()` on it, builds
  `FAudioPluginInitializationParams` and calls `InFactory->CreateNewEndpointInstance(InitParams,
  *SettingsProxy)` (`:2274-2310`; 4.27 `:1759-1789`). `FAudioPluginInitializationParams` is
  `{NumSources, NumOutputChannels, SampleRate, BufferLength, FAudioDevice* AudioDevicePtr}`
  (`IAudioExtensionPlugin.h:126-151`). **HARD.**
* The mixer keeps endpoint submixes in `ExternalEndpointSubmixes` (a submix with **no parent**
  and a live endpoint, `AudioMixerSubmix.cpp:1111-1114`) and, **after every ordinary submix
  has rendered**, calls `ProcessAudioAndSendToEndpoint()` on each
  (`AudioMixerDevice.cpp:1723-1752`; 4.27 `:750-757`). Their audio never enters the master
  buffer. **HARD.**
* 5.x only: `UEndpointSubmix::PostLoad` **resets an EndpointType that no registered factory
  provides to `"Default Endpoint"`** with a warning (`SoundSubmix.cpp:1079-1093`). 4.27 has no
  such reset, which is why Stray's assets still read `"Vibration Output"` at runtime
  (`STRAY-DUALSENSE.md` §14, probe read). **HARD.** Note the ordering this imposes on a UE5
  title: the factory must exist before the submix asset loads, or the asset is rewritten to
  the default in memory.

### 3.2 What the engine hands the endpoint, per callback

`FMixerSubmix::ProcessAudioAndSendToEndpoint` (`AudioMixerSubmix.cpp:2052-2160`; 4.27
`:1537-1642`, same logic line for line), in order. **HARD.**

1. `IsDummyEndpointSubmix()` (`!NonSoundfieldEndpoint->IsImplemented()`, `:1121-1126`) →
   zero the buffer and **return before rendering anything**. This is the path Stray's PC
   build is on.
2. `ProcessAudio(EndpointData.AudioBuffer)` — render the subtree at `NumChannels` (device
   channels) × `NumOutputFrames` (the mixer's callback size, default 1024).
3. First time, or after a disconnect: `Input = Endpoint->PatchNewInput(DurationPerCallback,
   OutSampleRate, OutNumChannels)` — the **endpoint dictates** its sample rate and channel
   count (`IAudioEndpoint.h:97`, `.cpp:18-36`).
4. If the endpoint's rate differs from the mixer's: `Resampler.Init(EResamplingMethod::Linear,
   ratio, NumChannels)` — **linear** (`:2104-2115`).
5. If the endpoint's channel count differs: `FMixerDevice::Get2DChannelMap(false,
   NumChannels, EndpointChannels, false, map)` then `DownmixBuffer(...)` (`:2130-2139`).
6. `Input.PushAudio(buffer)` into the endpoint's `FPatchMixer`, then
   `Endpoint->ProcessAudioIfNeccessary()` (`:2145-2146`).

### 3.3 The downmix the engine applies — and the plugin does not

For 8 → 2 the map is the AC-3-style matrix (`SignalProcessing/Private/ChannelMap.cpp:26-31`;
4.27 `AudioMixerChannelMaps.cpp:86-91`, *"Tables based on Ac-3 down-mixing"* `:70-72`):

```
            FL    FR    C      LFE   SL     SR     BL     BR
Left   =   1.0   0.0   0.707  0.0   0.707  0.0    0.707  0.0
Right  =   0.0   1.0   0.707  0.0   0.0    0.707  0.0    0.707
```

**Identical in 4.27 and 5.8. HARD.** The plugin's `DownmixToStereo` takes channels 0 and 1 and
discards the rest, deliberately (`SubmixDsp.hpp:47-51`, `SubmixDsp.cpp:9`). That comment's
reasoning — *"folding centre/LFE/rears into a grip would smear a directional effect across
both hands"* — describes a design choice the engine does not make: the engine keeps SL/BL on
the left grip and SR/BR on the right, at −3 dB, and puts C on both. Where it matters: Stray
sends sounds into `VibrationEndpointSubmix` through **attenuation settings**
(`PS5VibrationAttenuation.SubmixSendSettings[0]`, HARD from the pak), i.e. *spatialised*
sources, and a spatialised source behind the cat is panned into BL/BR by the 3D panner. The
engine's endpoint would still shake the coils for it; the tap drops it to zero. **HARD**
that the two folds differ; **SOFT** how much Stray's content exercises the rear channels.

### 3.4 Threading and block size: who calls whom, on which thread

* `ProcessAudioIfNeccessary` runs the callback synchronously when the endpoint declared
  `EndpointRequiresCallback()` and did **not** start an async thread (`IAudioEndpoint.cpp:45-52`).
  `RunCallbackSynchronously` pops `GetDesiredNumFrames() * GetNumChannels()` samples at a time
  from the patch mixer, **as many whole blocks as are available**, and calls
  `OnAudioCallback(block, NumChannels, settings)` for each (`:106-130`). All of this is inside
  the mixer's render callback, i.e. **on the audio render thread**. **HARD.**
* An endpoint may instead call `StartRunningAsyncCallback()` to get its own
  `FMixerNullCallback` thread (`:86-99`); neither Epic implementation does.
* `OnAudioCallback` returning `false` disconnects every input (`:122-125`); the mixer then
  re-patches on the next callback (`AudioMixerSubmix.cpp:2097-2102`).
* Settings are posted with `SetNewSettings` and read under a critical section via
  `PollSettings` (`IAudioEndpoint.cpp:38-43`, `:75-79`); `FMixerSubmix::UpdateEndpointSettings`
  forwards them (`AudioMixerSubmix.cpp:2352-2358`).

The vtable of `IAudioEndpoint`, in declaration order and identical in both versions (needed
by §7 #2): dtor, `IsImplemented`, `GetSampleRate`, `GetNumChannels`,
`EndpointRequiresCallback`, `GetDesiredNumFrames`, `OnAudioCallback`
(`IAudioEndpoint.h:83-140`; 4.27 `:67-124`). `PatchNewInput`, `SetNewSettings`,
`ProcessAudioIfNeccessary`, `PopAudio`, `PollSettings`, `DisconnectAllInputs`,
`Start/StopRunningAsyncCallback`, `RunCallbackSynchronously` are **non-virtual engine code
operating on the object's own `FPatchMixer`, `FCriticalSection`, `FAlignedFloatBuffer` and
`TUniquePtr<FMixerNullCallback>` members** (`:180-192`). **HARD.**

---

## 4. Epic's own DualSense endpoints on Windows — the plugin, read in full

`Engine/Plugins/Runtime/Windows/WinDualShock/`, release 5.8.2. `EnabledByDefault: false`,
Win64 only, described as *"InputDevice plugin for the PS4 DualShock controller in Windows"*
(`WinDualShock.uplugin`). All **HARD** unless marked.

**Stray cooked this plugin.** `Hk_project_Config_DefaultGame.ini:385` stages
`Engine/Platforms/PS4/Plugins/Runtime/WinDualShock` → `Engine/Plugins/Runtime/WinDualShock`,
and `Hk_project_Config_DefaultEngine.ini:624-625` sets `[SonyController] bDSMotionEvents=True`
— a key read only by this plugin's constructor (`WinDualShock.cpp:73`; 4.27 `:46`). That is
the input half. The PC build is broken for haptics without the plugin in this repo precisely
because the 4.27 vintage has no audio half (§5); nothing about Stray's copy needs restoring.

### 4.1 Registration

* Two factories, one template: `FExternalDualShockEndpointFactory<PadSpeakers>` and
  `<Vibration>` (`WinDualShock.cpp:676-762`, members `:773-774`). Names: `"Vibration Output"`
  and `"Pad Speaker Output"` (`:696-713`).
* Registered in `StartupModule` **only if** `FGenericPlatformMisc::IsPreferredInputDevice
  ("WinDualShock")` (`:786-797`, added 2026-04-09 in `810fdfcc1d95`); `IsPreferredInputDevice`
  is `true` unless `[/Script/Engine.InputSettings] bEnablePreferredInputAPIPreferences` is on
  (`GenericPlatformMisc.cpp:1470-1496`).
* Settings class `UDualShockExternalEndpointSettings { int32 ControllerIndex; }`
  (`WinDualShockSettings.h:12-28`); proxy `FDualShockExternalEndpointSettings { ControllerIndex
  = INDEX_NONE; }` (`WinDualShockSettingsProxies.h:10-14`). The endpoint is keyed by
  `FDeviceKey{ InitInfo.AudioDevicePtr->DeviceID, Settings.ControllerIndex }`
  (`WinDualShock.cpp:715-731`, `WinDualShock.h:30-39`). The same header also declares
  `UDualShockSoundfieldEndpointSettings` and `UDualShockSpatializationSettings { Spread,
  Priority, Passthrough }` (`WinDualShockSettings.h:30-71`) — the soundfield (3D audio) endpoint
  settings for the same controller family, defined here but with no Windows implementation in
  this file set. **SOFT** that these are the PS5 3D-audio object parameters mirrored.
* **Only one endpoint instance per port type per device is honoured.**
  `IWinDualShockAudioDevice::AddEndpoint` returns `true` only for the first
  (`WinDualShock.h:79-83`); later ones are created but discarded with a warning (`:726-729`).
  Commit `6ed1d5f2a8a5`, 2021-11-29: *"Prevent multiple endpoint submix instances from
  overlapping on the same hardware device, which isn't supported."*

### 4.2 The endpoint object

`FExternalWinDualShockEndpoint<PortType>` (`WinDualShockExternalEndpoints.h:15-86`):

| override | value |
|---|---|
| `GetSampleRate` | `48000` (`EWinDualShockDefaults::SampleRate`, `WinDualShock.h:105-113`) |
| `GetNumChannels` | `2` for both ports (`VibrationChannels = 2`, `PadSpeakerChannels = 2`) |
| `EndpointRequiresCallback` | `true` |
| `GetDesiredNumFrames` | `256` |
| `OnAudioCallback` | `check(NumChannels == 2)`; `Device->PushAudio(PortType, InAudio, 2)`; return `true` |
| `IsImplemented` | `true` |

So the engine delivers **512-float interleaved stereo blocks (256 frames) at 48 kHz on the
render thread**, already downmixed by §3.3 and resampled if the mixer is not at 48 kHz. The
GameInput implementation, written later against the same contract, explains the two constants:
*"Return the engine sample rate so the UE mixer does NOT create a resampler — matching
WinDualShock's approach"* and *"The mixer configures the submix's channel layout once and never
re-reads this — match WinDualShock's VibrationChannels = 2"*
(`GameInputHapticAudioDevice.h:175-192`).

### 4.3 The device: one 4-channel XAudio2 stream on the pad's endpoint

`FWinDualShockAudioDeviceImpl` (`WinDualShock.cpp:245-674`):

* **Endpoint discovery is by container id, through the registry.** `SetupAudio` asks the
  Sony controller class for `GetContainerRegistryPath(UserIndex)` (`:372`, restricted; the
  warning at `:378` names the requirement: *"This version of libscepad doesn't provide a
  registry container query API"*), opens that key under `HKLM`, enumerates value names, and
  takes the ones starting `SWD\MMDEVAPI\{0.0.0.00000000}` (render) and
  `SWD\MMDEVAPI\{0.0.1.00000000}` (capture), stripping the `SWD\MMDEVAPI\` prefix to obtain
  the **WASAPI device-id strings** (`:384-429`).
* `XAudio2Create`; `CreateMasteringVoice(NumOutputChannels, 48000, 0, OutputDeviceId, nullptr,
  AudioCategory_GameEffects)` with **`NumOutputChannels = 4` for DualSense, 2 for DualShock4**
  (`:499-508`); one source voice, `WAVE_FORMAT_IEEE_FLOAT`, N channels, 48 kHz (`:532-544`).
* **Frame layout for the 4-channel case: `[padSpeakerL, padSpeakerR, vibrationL, vibrationR]`**
  (`SubmitBuffer<4>`, `:335-363`). That is FL/FR = speaker, RL/RR = coils — the mapping the
  plugin measured and uses (`SubmixDsp.hpp:84-90`).
* Buffering: two `TCircularQueue<float>` of `256 * NumOutputChannels * 4 + 1` floats
  (`:439-440`), one per port; each stores stereo pairs, so **~2048 frames ≈ 42.7 ms capacity
  per port**. The stream starts when a port has queued ≥ 256 floats (128 frames, 2.7 ms)
  (`:271-277`, `:311-317`); XAudio2 consumes 256-frame blocks and `OnBufferEnd` refills
  (`:469-478`). Underrun = the dequeue fails and the sample stays `0` (`:343-359`). Overflow:
  `Enqueue`'s return is ignored (`:292-293`, `:329-330`), so a full queue drops the **newest**
  samples (**SOFT** on `TCircularQueue` semantics, not re-read this session).
* **Gain.** Every pushed sample is multiplied by `OutputGain = 1 / clamp(GetPlatformAudioHeadroom(),
  tiny, 1)` (`:95`, `:292-293`, `:329-330`), i.e. the engine's `[Audio] PlatformHeadroomDB`
  (`AudioDevice.cpp:490-496`) is undone so the pad is not attenuated with the main mix.
  Stray's shipped config sets no `PlatformHeadroomDB` (grep of `docs/game-config/`), so the
  factor is **1.0** for this title. The `[SonyController] DSPadSpeakerGain / DSHeadphonesGain /
  DSMicrophoneGain` floats (default 1.0, `:81-89`) go to the restricted
  `FPlatformControllers::SetAudioGain` (`WinDualShockControllers.h:28-38`) — **SOFT** that this
  is where `scePadSetVolumeGain` is called.
* **No soft clip, no limiter, no other processing** anywhere in the path.
* Mono into the speaker port duplicates the sample to L and R (`:283-295`); the engine never
  actually sends mono because `GetNumChannels()` is 2.
* Audio is set up lazily from `SendControllerEvents → UpdateAudioDevices` once the Sony class
  reports `GetSupportsAudio(UserIndex)` for the device, and torn down when it stops (`:152`,
  `:209-232`), so a pad plugged in mid-session is picked up on the input tick.

### 4.4 What the Windows plugin cannot show about PS5

The only calls into Sony's library visible in the public tree are the includes `pad.h` and
`pad_audio.h` (`WinDualShock.cpp:29-30`) and the `SCE_USER_SERVICE_*` constants. Route
selection (`scePadSetAudioOutPath`), volume (`scePadSetVolumeGain`), the container query
(`scePadGetContainerIdInformation`) and the console's pad audio ports all live behind
`FPlatformControllers`. What `STRAY-DUALSENSE.md` §16 measured about those calls on the shipped
`libScePad.dll` stands as the only HARD record of that half.

### 4.5 The second public implementation: GameInput (Xbox / GDK on Windows)

`Engine/Plugins/Runtime/GameInput/Source/GameInputBase/Private/GameInputHaptic*` — same
contract, WASAPI instead of XAudio2: `IAudioClient2` on the endpoint id GameInput reports,
channel mask built from `GameInputHapticLocation` GUIDs, `AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM`
so the OS converts to whatever rate the controller negotiates (`GameInputHapticAudioDevice.h`
`:64-158`, `:175-181`); 48 kHz / 256 frames / depth 4 defaults (`:33-38`); a dedicated render
thread waiting on the WASAPI sample-ready event (`:47-62`); recovery on
`AUDCLNT_E_*_INVALIDATED` (`:123-125`). Registered only when `IsHapticSupportEnabled()` and
GameInput is the preferred API (`GameInputBaseModule.cpp:271-277`). It applies the same
platform-headroom compensation (`:99-102`, *"applies platform headroom gain compensation"*).
Useful as a second reference for the Windows back end; it says nothing further about PS5.

---

## 5. 4.27 versus UE5 in this exact area

| | 4.27 (`3abfe77d`) | 5.8.2 (`16d75d84`) | matters for a 4.27 game? |
|---|---|---|---|
| `IAudioEndpoint` / `IAudioEndpointFactory` | `IAudioEndpoint.h:64-240` | `:80-254` | **No change** except `AUDIOEXTENSIONS_API` placement and `AlignedFloatBuffer` → `FAlignedFloatBuffer`. Virtual order identical. |
| `IAudioEndpointFactory::Get` | first name match wins (`.cpp:155-175`) | also requires `bIsImplemented` (`.cpp:159-181`) | Only if registering a factory (§7 #2): in 4.27 the flag is not consulted. |
| `ProcessAudioAndSendToEndpoint` | `AudioMixerSubmix.cpp:1537-1642` | `:2052-2160` | **No change.** |
| `FMixerSubmix::Init` endpoint branch | `:315-324` | `:478-487` | **No change.** |
| `UEndpointSubmix::PostLoad` | none | resets unknown `EndpointType` to default (`SoundSubmix.cpp:1079-1093`) | Explains why Stray's assets keep `"Vibration Output"` at runtime; a UE5 title would have rewritten them. |
| `ISubmixBufferListener` | `AudioDevice.h:394-406`: one pure virtual, **no virtual dtor**, raw pointer | `AudioMixer/Public/ISubmixBufferListener.h:12-50`: `TSharedFromThis`, virtual dtor, `IsRenderingAudio()`, `GetListenerName()` | The plugin's hand-built one-slot vtable is 4.27-correct and would be wrong on 5.x (already documented in `SubmixTap.hpp`). |
| `RegisterSubmixBufferListener` | `(ISubmixBufferListener*, USoundSubmix* = nullptr)`, `AudioDevice.h:863`; mixer `AudioMixerDevice.cpp:2343` | `(TSharedRef<…>, USoundSubmix&)`, `AudioMixerDevice.cpp:3571`; unknown submix goes to a pending list (`:3597-3607`) | Different vtable slot shape on 5.x. |
| Listener invocation | raw loop, `AudioMixerSubmix.cpp:1377-1380` | weak-ptr pin, `:1845-1849`; listeners are **still called with a silent buffer when the submix auto-disables** (`:1409-1450`) | Auto-disable does not exist in 4.27 (no `bAutoDisable` in its `AudioMixerSubmix.cpp`). |
| `FMixerSubmix::AddPatch` (lock-free patch output, an alternative to listeners) | present, `:498-506` | present, `AudioMixerSubmix.h:195` | Available in 4.27 too; not used by the plugin. |
| Downmix matrices | `AudioMixerChannelMaps.cpp:86-91` | moved to `SignalProcessing/Private/ChannelMap.cpp:26-31` | **Same numbers.** |
| WinDualShock audio endpoints | **absent** — 141-line input-only plugin (`4.27:WinDualShock.cpp`), `Build.cs` without AudioMixer/AudioExtensions deps | present; `WinDualShockExternalEndpoints.h` blob `58fc9740…` identical on `5.0`, `5.1`, `5.2`; first commit `0c3be2b6ad45` 2021-11-18 | **The reason Stray's PC build has dead endpoint submixes.** Nothing in 4.27 could have rendered them on Windows. |
| Device properties | `FInputDeviceProperty { FName Name }` only (`IInputInterface.h:88-95`); no `InputDeviceProperties.h` | typed structs + `UInputDeviceProperty` assets (§6) | Stray's `SonyLightColor` / `PS5TriggerEffect` are the pre-typed licensee form. |

**HARD** throughout (each cell was read on both branches).

---

## 6. UE5's public device-property API, mapped to Stray's FName dispatch

### 6.1 The public surface (5.8.2)

`GenericPlatform/IInputInterface.h`, all **HARD**:

| struct | `Name` FName | payload |
|---|---|---|
| `FInputDeviceProperty` (`:128-135`) | caller-defined | base |
| `FInputDeviceLightColorProperty` (`:156-175`) | `"InputDeviceLightColor"` | `bool bEnable; FColor Color` |
| `FInputDeviceTriggerProperty` (`:178-186`) | — | `EInputDeviceTriggerMask AffectedTriggers` (`Left=0x01, Right=0x02`, `:98-104`) |
| `FInputDeviceTriggerResetProperty` (`:189-198`) | `"InputDeviceTriggerResetProperty"` | mask = All |
| `FInputDeviceTriggerFeedbackProperty` (`:201-212`) | `"InputDeviceTriggerFeedback"` | `int32 Position; int32 Strengh` |
| `FInputDeviceTriggerResistanceProperty` (`:215-234`) | `"InputDeviceTriggerResistance"` | `StartPosition, StartStrengh, EndPosition, EndStrengh` |
| `FInputDeviceTriggerVibrationProperty` (`:237-250`) | `"InputDeviceTriggerVibration"` | `TriggerPosition, VibrationFrequency, VibrationAmplitude` |

Dispatch: `IInputInterface::SetDeviceProperty(int32 ControllerId, const FInputDeviceProperty*)`
(`:360`); `FWindowsApplication::SetLightColor` builds `FInputDeviceLightColorProperty(true,
Color)` and forwards it, `ResetLightColor` sends `(false, Black)`
(`Windows/WindowsApplication.cpp:4333-4344`), and `SetDeviceProperty` fans out to **every**
`IInputDevice` (`:4347-4357`); each device is expected to switch on `Property->Name` and
ignore what it does not know (`IInputDevice.h:77`). The asset layer
(`GameFramework/InputDeviceProperties.h`) evaluates `UInputDeviceProperty` objects over time
and ends in the same call (`InputDeviceProperties.cpp:26-40`); trigger feedback is authored as
curves with the ranges *position 1-9, strength 1-8* (`.h:317-323`) and carries a
`DeviceOverrideData` map keyed by `HardwareDeviceIdentifier` (`.h:349-351`). The hardware
identifier for the pad comes from the plugin's `Input.ini`:
`HardwareDeviceIdentifier="DualSense", SupportedFeaturesMask=1996` = `Gamepad|Touch|Lights|
TriggerHaptics|ForceFeedback|AudioBasedVibrations|Acceleration` (0x7CC), and `"DualShock4"`
`332` = `Gamepad|Touch|Lights|ForceFeedback` (`InputSettings.h:369-433`).

`UInputDeviceAudioBasedVibrationProperty` ("Audio Based Vibration (Experimental)") is the
public bridge between the two halves of this document: it has **no** `FInputDeviceProperty`
at all — it just `ClientPlaySound`s a `USoundBase` whose submix sends point at the pad
endpoints (`InputDeviceProperties.h:481-523`, `.cpp:456-471`: *"This sound should have the
submix sends set that it wants, all we have to do is play it."*). That is exactly Stray's
`PS5VibrationAttenuation` → `VibrationEndpointSubmix` pattern, made an engine feature. **HARD.**

### 6.2 Stray's 4.27 names against the UE5 ones

| Stray (`STRAY-DUALSENSE.md` §2, §3, §13) | UE5 public equivalent | Sony call / mode | label |
|---|---|---|---|
| `SonyLightColor` `{+0x08 enable, +0x0c..0x0e rgb}` | `FInputDeviceLightColorProperty { bEnable, FColor }` | `scePadSetLightBar` / `scePadResetLightBar` (Stray's own dispatch, HARD) | mapping **SOFT** (same fields, same on/off semantics; the 4.27 Sony tree is unreachable) |
| `PS5TriggerEffect` `{+0x08 mask, +0x09 mode 0..3, +0x0a..0x0c params}` with game order `None, Weapon, Vibration, Feedback` | one struct per mode (below) | `scePadSetTriggerEffect` | game struct HARD; correspondence **SOFT** |
| game `Feedback` = 3, `{position, strength}` | `FInputDeviceTriggerFeedbackProperty { Position, Strengh }` | Sony mode **1** FEEDBACK `{position, strength}` | SOFT |
| game `Vibration` = 2, `{position, amplitude, frequency}` | `FInputDeviceTriggerVibrationProperty { TriggerPosition, VibrationFrequency, VibrationAmplitude }` | Sony mode **3** VIBRATION | SOFT |
| game `Weapon` = 1, `{start, end, strength}` | no exact public struct — `FInputDeviceTriggerResistanceProperty` has **four** fields `{StartPosition, StartStrengh, EndPosition, EndStrengh}`, the shape of a slope-feedback effect rather than WEAPON's single strength | Sony mode **2** WEAPON | SOFT; the extra field says UE5's resistance maps to a different Sony mode than WEAPON |
| `EPS5TriggersState::{None, Scratchable}` | n/a (game-level state) | — | — |

Two consequences for the plugin's translation layer:

* **The plugin's `sony_trigger_mode()` is doing what `FPlatformControllers::SetDeviceProperty`
  does on PS5** — translating an engine-side property into `ScePadTriggerEffectParam`. UE5's
  public structs confirm the shape of that translation (one engine struct → one Sony mode, a
  trigger mask, integer position/strength in Sony's ranges) without exposing the Sony code.
  **SOFT.**
* **Stray's dispatcher at RVA `0x9FC470` is, in all likelihood, `FPlatformControllers::
  SetDeviceProperty`** reached through `FWinDualShock::SetDeviceProperty(ControllerId, Property)
  { Controllers.SetDeviceProperty(...) }` (`4.27:WinDualShock.cpp:102-105`): same signature
  (controller id, property pointer), same FName-switch-then-return-silently shape, and Stray's
  config proves the plugin is in the exe (§4). **SOFT** — an identification by shape and
  config, not by symbol.

---

## 7. What the engine hands an endpoint, what the plugin does, and the gap

Plugin refs are `mods/StrayDualSense/src/*` on `dualsense-thin` @ `60c7567`.

| the engine's contract (§3, §4) | the plugin today | gap |
|---|---|---|
| Endpoint = an `IAudioEndpoint` created by a factory named `"Vibration Output"` / `"Pad Speaker Output"`; the submix is an **`ExternalEndpointSubmix`**, never mixed into the master | re-parents `Submix_vibrationMaster` / `Submix_controllerMaster` under `Submix_unused` (`Config.hpp:93`), re-registers via `FAudioDevice::RegisterSoundSubmix` slot 14 (`Runtime.cpp:335`), then `RegisterSubmixBufferListener` slot 16 on each (`:385-410`) with a leaked-page one-slot vtable (`SubmixTap.cpp`) | **Structural.** Works (measured, §15) but is a different mechanism: the subtree is now an ordinary child of a rendered submix, kept inaudible by the parent's volume rather than by the endpoint list; the tap sees the parent's accumulation; the reroute must be re-armed per level load (§17). |
| Rendered at device channel count, **downmixed 8→2 with the AC-3 stereo matrix** (§3.3) | `DownmixToStereo`: channels 0/1 only (`SubmixDsp.hpp:47-51`); measured input is `ch=8` | **HARD gap.** C/SL/SR/BL/BR content (spatialised sends) reaches the engine's endpoint at −3 dB and never reaches the coils through the tap. |
| Endpoint declares **48 000 Hz**; engine resamples **linearly** to it if the mixer differs | tap records the engine rate (`Runtime.cpp:849`), sink resamples linearly to the endpoint's rate (`LinearResampler`, `SubmixDsp.hpp:119`) | none — same rate, same method, opposite side of the boundary |
| **2 channels per port**, both ports | stereo per lane, `InterleaveLanes` (`SubmixDsp.cpp:49`) | none |
| Frame layout `[spkL, spkR, vibL, vibR]` on one 4-ch 48 kHz float stream | FL/FR speaker, RL/RR coils, one WASAPI shared-mode float client (`SubmixDsp.hpp:84-90`, `SubmixSink.cpp:144-161`, `Wasapi.cpp:172`) | none — **the mapping is Epic's** |
| Blocks of 256 frames on the render thread → lock-free queue → device callback thread | tap on the render thread → SPSC ring → worker polling WASAPI every 3 ms (`SubmixSink.cpp:19`, `:220`) | shape identical; Epic's device thread is event/callback driven, ours polls |
| Start streaming at 128 queued frames; ~43 ms queue capacity; XAudio2 block 5.3 ms | `submixQueueAheadMs = 40`, ring 250 ms (`Config.hpp:117-118`) | **SOFT gap.** ~40 ms lead versus Epic's ~3-11 ms floor; adds latency to every haptic. Tunable without code. |
| Overflow drops the **newest** samples | ring overwrites the **oldest** (`SubmixDsp.hpp:198`) | opposite policy; ours is the better one for "be current", but it is a divergence |
| Underrun → zeros | `Read` zero-fills (`SubmixDsp.hpp:202-204`) | none |
| Gain: `1 / PlatformAudioHeadroom` (1.0 for Stray); ini floats per port to Sony; **no clipping stage** | `submixGain = 1.0`, `speakerGain = 1.0`, then **`SoftClip` knee 0.75** (`SubmixDsp.hpp:65`, `SubmixDsp.cpp:37`) | the 1.0 gains are right (**HARD**, no headroom configured); the soft clip is an addition the engine does not make (**SOFT** whether it is audible on this content) |
| Endpoint identified by the pad's **container id → registry → WASAPI device id** | endpoint matched by **friendly name** substring `"DualSense"` (`Config.hpp:159`, `Wasapi.cpp:120`) | **SOFT gap.** Name matching is locale- and driver-dependent; two pads, or a renamed endpoint, break it. |
| Per-device key `{AudioDeviceID, ControllerIndex}` from the submix's `EndpointSettings` | `padUserId` from the ini (`Config.hpp:33`); the endpoint submixes' `EndpointSettings` are never read | SOFT gap; matters only for multi-pad |
| `IsImplemented()` true → `ProcessAudioAndSendToEndpoint` renders; dummy → subtree skipped | n/a (the reroute sidesteps the dummy) | — |
| Audio set up lazily per pad when the Sony class reports audio support; torn down on loss | sink retries the endpoint every 2 s (`SubmixSink.cpp` `kReopenDelayMs`) | equivalent |

---

## 8. Recommendations, ranked

1. **Fold 8→2 with the engine's matrix, not channels 0/1.** Replace `DownmixToStereo`'s
   `>2` branch with the AC-3 table of §3.3 (FL/FR ×1.0; C, SL/BL → L and C, SR/BR → R at
   0.707; LFE dropped). **HARD** that this is what the engine's endpoint receives; **SOFT**
   how often Stray's spatialised haptic sends land outside FL/FR. Cheap, pure, unit-testable,
   and it removes the one arithmetic divergence from the contract. Keep the old fold behind a
   key for A/B.

2. **Be the endpoint: register real factories and let `ProcessAudioAndSendToEndpoint` drive
   the sink.** This is the exact contract, and it deletes the reroute, the per-level re-arm,
   the listener vtable, and the "parent volume keeps it inaudible" dependency. The shape,
   every piece of which is HARD from §3 and the vtable order:
   * Obtain `IModularFeatures::Get()` (a `CORE_API` singleton; in Stray's monolithic exe that
     means a pattern scan for the accessor or for the `FModularFeatures` instance) and call
     the **virtual** `RegisterModularFeature(FName("External Audio Endpoint"), factory)`
     (`IModularFeatures.h:108` @ 4.27).
   * The factory object: `IModularFeature` has **no virtuals in 4.27**
     (`Features/IModularFeature.h:11-19` @ 4.27), so the vtable is `IAudioEndpointFactory`'s
     alone: dtor, `GetEndpointTypeName` (FName by value, 8 bytes in a shipping build),
     `CreateNewEndpointInstance`, `GetCustomSettingsClass`, `GetDefaultSettings`
     (`IAudioEndpoint.h:185-240` @ 4.27); `bool bIsImplemented` at the first data slot.
   * **Do not construct an `IAudioEndpoint` yourself.** `PatchNewInput`, `PopAudio`,
     `RunCallbackSynchronously` are non-virtual engine code over the object's own
     `FPatchMixer` and friends (§3.4). Instead call the dummy factory's virtual
     `CreateNewEndpointInstance` (`IAudioEndpointFactory::GetDummyFactory()`, or the base slot
     through any registered factory) to get an **engine-constructed** endpoint, then replace
     its vptr with a copy of the engine vtable whose seven slots are patched:
     `IsImplemented → true`, `GetSampleRate → 48000`, `GetNumChannels → 2`,
     `EndpointRequiresCallback → true`, `GetDesiredNumFrames → 256`, `OnAudioCallback → push
     512 floats into the lane's ring`. The engine then resamples, downmixes and calls us on
     its own schedule.
   * `GetDefaultSettings` must return a live `UAudioEndpointSettingsBase*` whose virtual
     `GetProxy()` works — the CDO of `UDummyEndpointSettings` (a UObject class UE4SS can find
     by name) is exactly that, and Stray's endpoint submixes appear to carry no
     `EndpointSettings` of their own (UNCONFIRMED — the probe read only `EndpointType`).
   * Timing: the factory must be registered **before** `FMixerSubmix::Init` runs for the two
     endpoint submixes, or they must be re-inited afterwards — the plugin's existing
     `RegisterSoundSubmix(submix, bInit = true)` call is that re-init
     (`AudioMixerDevice.cpp:1494` @ 4.27), and `RegisterSoundSubmix` re-files the submix into
     `ExternalEndpointSubmixes` (`:1590-1603` @ 4.27). 4.27 does not check `bIsImplemented` in
     `Get` (§5), but the endpoint's `IsImplemented()` must be true or the subtree is skipped.
   **UNCONFIRMED** as a whole: a design, never built. Its risks are the singleton lookup and
   getting one vtable slot wrong; both fail loudly at first callback rather than silently.

3. **Identify the pad's endpoint by container id, not by friendly name.** Epic's way needs
   the Sony registry-path query (`scePadGetContainerIdInformation`, which §16 records
   crashing on a guessed struct — do not repeat that). The equivalent without Sony:
   enumerate `IMMDeviceEnumerator` render endpoints, read `PKEY_Device_ContainerId`, and match
   it against the container id of the DualSense HID interface the plugin already opens
   (`HidMode`), via `SetupDiGetDeviceProperty(DEVPKEY_Device_ContainerId)` on the HID node.
   **SOFT** that this is robust under Wine's device tree; it is what Epic's registry walk
   resolves to on Windows (`SWD\MMDEVAPI\{0.0.0.00000000}.{guid}` is the render endpoint's
   device id).

4. **Lower `SubmixQueueAheadMs` toward Epic's operating point** (start at 128 frames ≈ 3 ms,
   block 256 ≈ 5 ms) once the pacing is trusted — try 10-15 ms, watch `under=` in the status
   line. **SOFT**: latency is felt on haptics; the number is Epic's, the box's tolerance is
   unmeasured.

5. **Make the soft clip opt-in, or measure it.** The engine ships none; a clean 1.0-gain path
   with the device clipping is the contract. Keep `SoftClip` available (the buzz argument in
   `SubmixDsp.hpp:55-64` is reasonable) but default it off for the A/B, or log how often the
   knee engages. **SOFT.**

6. **Read `EndpointSettings.ControllerIndex` from the two endpoint submixes** (if non-null) in
   preference to the ini `PadUserId`, mirroring `FDeviceKey`. **SOFT**; single-pad today.

7. **Already right, leave alone (HARD):** 48 kHz; 2 channels per lane; FL/FR = speaker,
   RL/RR = coils on one 4-channel float stream; linear resampling; render-thread producer
   that never blocks; zero-fill on underrun; 1.0 lane gains for this title's config; the
   per-lane single-instance model (Epic also refuses a second endpoint per port).

8. **For the triggers and light bar, keep translating in the enum space UE5 documents**
   (§6.2): one Sony mode per property, integer position/strength, a trigger mask. The
   `sony_trigger_mode()` table is the plugin-side stand-in for the restricted
   `FPlatformControllers::SetDeviceProperty`, and UE5's public structs are the closest thing to
   a specification of what that function consumes. **SOFT.**

---

## Appendix — files read this session

Epic `release` @ `16d75d84` unless noted:
`Engine/Plugins/Runtime/Windows/WinDualShock/{WinDualShock.uplugin, Config/Input.ini,
Source/WinDualShock/WinDualShock.Build.cs, Private/WinDualShock.{h,cpp},
Private/WinDualShockControllers.h, Private/WinDualShockExternalEndpoints.h,
Public/WinDualShockSettings.h, Public/WinDualShockSettingsProxies.h}` (and the `4.27` versions
of `WinDualShock.{h,cpp}`, `WinDualShock.Build.cs`);
`Engine/Source/Runtime/AudioExtensions/{Public/IAudioEndpoint.h, Private/IAudioEndpoint.cpp,
Public/ISoundfieldEndpoint.h, Public/IAudioExtensionPlugin.h}` (both branches for the first two);
`Engine/Source/Runtime/AudioMixer/{Private/AudioMixerSubmix.cpp, Public/AudioMixerSubmix.h,
Private/AudioMixerDevice.cpp, Private/AudioMixerChannelMaps.cpp, Public/ISubmixBufferListener.h}`
(both branches for the `.cpp`s); `Engine/Source/Runtime/SignalProcessing/Private/ChannelMap.cpp`;
`Engine/Source/Runtime/Engine/{Classes/Sound/SoundSubmix.h, Private/SoundSubmix.cpp,
Public/AudioDevice.h (4.27), Private/AudioDevice.cpp, Classes/GameFramework/InputDeviceProperties.h,
Private/GameFramework/InputDeviceProperties.cpp, Classes/GameFramework/InputSettings.h}`;
`Engine/Source/Runtime/ApplicationCore/{Public/GenericPlatform/IInputInterface.h (both),
Private/Windows/WindowsApplication.cpp}`; `Engine/Source/Runtime/InputDevice/Public/{IInputDevice.h
(both), IInputDeviceModule.h}`; `Engine/Source/Runtime/Core/{Private/GenericPlatform/GenericPlatformMisc.cpp,
Public/Features/IModularFeature.h (4.27), Public/Features/IModularFeatures.h (4.27)}`;
`Engine/Plugins/Runtime/GameInput/Source/GameInputBase/{Private/GameInputHapticEndpointFactory.{h,cpp},
Private/GameInputHapticAudioDevice.h, Public/GameInputHapticEndpointSettings.h,
Private/GameInputBaseModule.cpp}`; `Engine/Build/Build.version` on `release`, `5.8`, `ue5-main`.

This repo: `docs/STRAY-DUALSENSE.md` §1, §14, §16, §18 and `mods/StrayDualSense/src/{SubmixTap.*,
SubmixSink.*, SubmixDsp.hpp, Runtime.cpp, Config.{hpp,cpp}, Wasapi.cpp}` on `dualsense-thin` @
`60c7567`; `docs/game-config/Hk_project_Config_{DefaultEngine,DefaultGame}.ini`.
