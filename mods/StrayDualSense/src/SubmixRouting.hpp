// StrayDualSense — WHICH submix each lane taps, and which assets can never reach a tap.
//
// Pure: no Windows, no UE4SS, no threads. Everything here is a fact read out of Stray's own
// cooked content or out of the UE 4.27.2 source, expressed so CI can pin it.
//
// ---------------------------------------------------------------------------------------
// WHY THE TAP TARGET IS NOT THE SUBMIX WE RE-PARENT
// ---------------------------------------------------------------------------------------
//
// MEASURED 2026-09-03 in the running game (docs/STRAY-DUALSENSE.md §20.1): with a listener on
// `Submix_vibrationMaster` and another on `Submix_controllerMaster`, the two lanes reported
// BIT-IDENTICAL `peak` and `rms` in 2,365 of 2,483 paired status periods — 95.2% — with the
// speaker lane strictly greater in 116 and the vibration lane strictly greater in 2. That is
// not two submixes that happen to agree. It is one buffer read twice.
//
// The cause is UE 4.27's buffer-listener contract, and it is not a bug in the engine:
//
//   FMixerSubmix::ProcessAudio(AlignedFloatBuffer& OutAudioBuffer)   AudioMixerSubmix.cpp
//     :1085   InputBuffer.Reset(NumSamples);
//     :1086   InputBuffer.AddZeroed(NumSamples);          // OUR scratch, zeroed ONCE
//     :1101       ChildSubmix->ProcessAudio(InputBuffer); // children accumulate into it
//             ... sources, effects, output volume, all on InputBuffer ...
//     :1364   Audio::MixInBufferFast(InputBuffer, OutAudioBuffer);   // into the PARENT
//     :1380   BufferListener->OnNewSubmixBuffer(SoundSubmix, OutAudioBuffer.GetData(), ...);
//
// The listener is handed `OutAudioBuffer` — the PARENT's buffer — AFTER this submix mixed into
// it, and the parent zeroes that buffer only once, before its child loop, never between
// siblings. So for children processed in order C1, C2: C1's listener sees {C1} and C2's sees
// {C1 + C2}. The reroute puts BOTH masters under one `Submix_unused`, which makes them
// siblings, which makes the second one processed read both.
//
// **The fix is to stop sharing rather than to compensate for it.** Each master has exactly one
// child (`Submix_vibration` under `Submix_vibrationMaster`, `Submix_controller` under
// `Submix_controllerMaster`), and a listener on that child is handed its MASTER's InputBuffer
// — which holds that master's subtree and nothing else, because a master is the only thing
// under `Submix_unused` that its own child mixes into. Clean by construction, no ordering
// assumption, and no second unused submix to find.
//
// WHAT THE CHILD TAP GIVES UP, stated so it is not discovered later: the master's own
// `OutputVolume` and effect chain are applied after the child's listener runs, so they are no
// longer in our samples. Measured (§14 probe, and confirmed from the pak in §20.10): NEITHER
// master serialises an `OutputVolume` and neither carries an effect chain, so today this is
// exactly a no-op. It would become a silent wrong level if the game ever gained one, which is
// why `TapPlan::givesUpMasterVolume` exists and is logged.
//
// `SBFX_Boost` is unaffected: it lives on `Submix_controllerPre`, BELOW `Submix_controller`,
// so it was never in the master's own chain in the first place. (Whether the `_CONTROL` assets
// reach it at all is a separate question — §20.10 Correction 2 says they do not, because the
// sound class routes them into `Submix_controller`, the boost's parent.)
#pragma once

#include <string>

namespace sds {
namespace submix {

// The two roles a lane's submix can play, and they are no longer the same object.
//
//   reroute target  the MASTER. Re-parented under `Submix_unused` and re-registered so the
//                   mixer renders the subtree at all (§14). Must stay the master: it is the
//                   node whose ParentSubmix we rewrote.
//   tap target      the CHILD. Where the buffer listener goes, per the analysis above.
struct TapPlan
{
    std::string target;                      // the path to register the listener on
    bool        tappingChild        = false; // false = the old, aliasing, master tap
    bool        givesUpMasterVolume = false; // true whenever tappingChild is true
    const char* why                 = "";    // one clause for the log
};

// Decide a lane's tap target. `tapPath` is the configured child; `masterPath` is the submix
// the reroute re-parents.
//
// An EMPTY `tapPath` deliberately means "tap the master", reproducing the pre-fix behaviour
// from one ini edit. That is not a courtesy: it is the A/B that proves the fix, because the
// lane-alias instrument below reports a completely different number in the two modes, in one
// session, with no rebuild.
//
// A `tapPath` equal to the master is the same thing said a different way, and is reported as
// such rather than silently treated as a child tap.
TapPlan PlanTap(const std::string& tapPath, const std::string& masterPath);

// ---------------------------------------------------------------------------------------
// ASSETS THAT CAN NEVER REACH A TAP, whatever we do.
//
// HARD, from the cooked assets (docs/STRAY-DUALSENSE.md §20.11): two `SoundWave`s serialise an
// explicit `SoundSubmixObject = VibrationEndpointSubmix`, overriding
// `SCLASS_controllerVibration.DefaultSubmix`. That names the `UEndpointSubmix` ROOT — which on
// Windows has no `IAudioEndpointFactory`, gets the dummy endpoint, and is skipped by
// `ProcessAudioAndSendToEndpoint` BEFORE its children are touched (§14). So these two bypass
// `Submix_vibration` entirely and land on the one submix in the tree the mixer never
// processes.
//
// WHY THIS IS IN THE CODE AND NOT ONLY IN THE DOC: without it the watch reports
// "the engine mixed NOTHING", whose printed advice points at the Blueprint gate, the level or
// the routing — three wrong answers. A silent asset that is silent BY CONSTRUCTION must say so,
// or it costs someone a session. This is prime directive 2: a loud, named failure beats a
// quiet wrong one.
//
// The check is on the SHORT asset name (`AssetName.hpp`), case-sensitively, because that is
// what the hook logs and what the pak records.
bool RoutesToDeadEndpoint(const std::string& shortAssetName);

// The sentence to print when it does. Never null.
const char* DeadEndpointReason();

} // namespace submix
} // namespace sds
