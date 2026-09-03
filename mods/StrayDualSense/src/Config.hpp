// StrayDualSense — configuration.
//
// INI, not JSON: it is hand-edited on a machine that is mid-session, over ssh, by someone who
// wants to flip one switch and relaunch. It is hot-reloaded because this mod cannot be built
// or tested locally — the only tuning loop is the user editing a value on the box.
//
// There are deliberately FEW knobs. Everything the game authors (trigger effect, loop flags,
// asset levels, the speaker's +5 dB trim) is read from the game or is a named constant, not a
// setting; a knob whose best value equals something the engine already knows is a missing
// wire, not a feature (CLAUDE.md, "a hand-tuned constant that works is a bug report").
//
// No UE4SS types here.
#pragma once

#include <cstdint>
#include <string>

#include "CoilOwner.hpp"
#include "Log.hpp"
#include "PadAudio.hpp"

namespace sds {

struct Config
{
    // ---- lifecycle -------------------------------------------------------------------
    bool     enabled  = true;
    LogLevel logLevel = LogLevel::Info;

    // ---- pad selection ---------------------------------------------------------------
    // 0 = probe user slots 1..4 and adopt the one whose `connected` byte is set. MEASURED:
    // every slot answers with a positive handle and a success code, so anything else is a
    // guess. Non-zero forces that slot and SKIPS the probe (escape hatch only).
    int   padUserId      = 0;
    float padPollSeconds = 2.0f;   // "is the pad still there" cadence on the pad watcher

    // ---- adaptive triggers -----------------------------------------------------------
    bool triggers        = true;
    // Diagnostic hardware readback after each change; costs a 120 ms sleep ON THE WORKER,
    // never the game thread. The first engage after open always reads 0/0 (§5).
    bool triggerReadback = false;

    // ---- haptics (waveforms on the coils) ----------------------------------------------
    bool haptics = true;
    // DualSense USB output report 0x02, byte 1 = valid_flag0. Bits are VALIDITY CLAIMS, not a
    // mode: bit0 COMPATIBLE_VIBRATION, bit1 HAPTICS_SELECT, bit2/3 trigger FFB, bit4/5 audio,
    // bit6/7 mic. 0x00 claims NOTHING — and thereby stops re-asserting compatible-vibration,
    // which is all it takes for the coils to take the waveform (§12). 0xFC is WRONG: it claims
    // trigger data while supplying zeros, which first killed the adaptive triggers and then
    // latched them on. This exists to reproduce that measurement, not to be tuned.
    int   hapticValidFlag0     = 0x00;
    // libScePad's own output reports (triggers, rumble) carry the same flag byte and undo a
    // single write; re-assert on this cadence AND immediately before each waveform.
    float hapticReassertSeconds = 2.0f;

    // ---- haptics: where the waveform COMES FROM (the submix spike) ---------------------
    // The modes and the rule each imposes are defined ONCE, in CoilOwner.hpp, next to the pure
    // function that turns them plus the tap's facts into "who drives the coils". Read that.
    //
    //   assets           the shipped behaviour: <gamedir>/haptic/<name>.f32, one playback slot
    //   measure          tap the engine's vibration submix and REPORT; assets drive the coils
    //   submix-fallback  assets drive the coils until the tap carries a real signal, and every
    //                    status line and a periodic WARN say so - the 2026-09-03 session read
    //                    `bound=1` plus a vibrating pad as "the submix works" when it was this
    //                    fallback, and that must never be possible again
    //   submix           the submix or NOTHING: the asset path never plays, so anything felt
    //                    in this mode came from the submix. A silent submix is a silent pad.
    HapticSource hapticSource = HapticSource::Assets;

    // How often the WARN repeats while a submix mode is configured and the submix is not
    // delivering (unbound, never called, or silent). 0 disables the cadence (the status line
    // still says who drives the coils).
    float submixWarnSeconds = 10.0f;

    // The submix to listen on. The path measured in the box's own UE4SS object dump; the
    // literal "master" registers on the engine's MASTER submix instead, which needs no
    // USoundSubmix object at all (a nullptr submix means master —
    // AudioMixerDevice.cpp:2350) and is the surest proof that the tap MECHANISM works.
    std::string submixPath = "/Game/Sound/tools/settings/Submix_vibrationMaster.Submix_vibrationMaster";

    // Register a SECOND, meter-only listener on the master submix. It is what turns a null
    // result into a diagnosis: master firing while the vibration submix stays silent means
    // the tap works and the GAME is not playing haptics into it; neither firing means the
    // tap is broken. Costs one extra downmix of the game's output per audio callback.
    bool submixProbeMaster = true;

    // FAudioDevice::RegisterSubmixBufferListener's vtable index. 16 for stock UE 4.27.2, and
    // the count is unusually safe because no FAudioDevice virtual sits inside a preprocessor
    // conditional — but Stray is a LICENSEE build, so this is a knob and it is logged.
    // See src/SubmixDiscovery.hpp for the full derivation.
    int submixRegisterSlot = 16;

    // both | world | engine. "both" demands the same FAudioDevice pointer be found
    // independently inside the UWorld and the UEngine object; the single-object modes are
    // escape hatches and say so in the log.
    std::string submixDeviceSource = "both";

    // How far into UWorld/UEngine to scan for the audio device. 0x2000 was NOT ENOUGH:
    // UEngine declares 268 UPROPERTYs before MainAudioDeviceHandle (Engine.h:1735).
    int   submixScanBytes    = 0x8000;
    // 32-bit words of each non-UObject candidate to dump when NOTHING is accepted. The
    // sample-rate test is the one check that cannot self-check, so a refusal must leave behind
    // enough to find the rate by eye and measure its offset.
    int   submixDumpWords    = 96;

    float submixGain         = 1.0f;   // applied before the soft clip
    int   submixQueueAheadMs = 40;     // WASAPI lead on the coil stream; raise if it crackles
    int   submixRingMs       = 250;    // ring capacity; ~12 engine callbacks at the default
    float submixStatusSeconds = 1.0f;  // how often the numbers proof is written

    // THE CORRELATION INSTRUMENT (src/SubmixWatch.hpp). Every StartPS5Vibration opens a watch
    // this many seconds wide; when it closes, ONE line says whether the engine put anything in
    // the submix for that asset. Without it the only reading available is a per-second peak,
    // which is 0.00000 whenever nothing happens to be playing - and on 2026-09-03 exactly one
    // such line was read as "the submix delivers nothing" on a session that had already
    // handed the coils over on a real signal. 0 disables the watch.
    float submixWatchSeconds = 3.0f;

    // What counts as a REAL SIGNAL: the peak, in the tap's own units, at or above which the
    // submix takes the coils over from the asset path (Runtime::StartSinkAtHandover).
    //
    // 1e-4 is -80 dBFS. It is deliberately low so a quiet asset cannot be missed, and the
    // cost of that choice is stated rather than hidden: in STRICT `submix` the handover
    // DISABLES the asset path for the rest of the session, so a single -80 dBFS tail is
    // enough to make `live=1` while the pad is, in practice, silent. If a session reports
    // `live=1` with a `FIRST REAL SIGNAL` peak that is a small fraction of the ~0.7 a real
    // VIBE asset measures, raise this rather than believing the handover.
    float submixLiveThreshold = 1.0e-4f;
    // One line, rewritten in place, next to the log — so the numbers can be read with `cat`
    // over ssh without the game overlay.
    std::string submixStatusFile = "stray-dualsense-submix.txt";

    // ---- the reroute: make the engine RENDER the vibration submix on PC --------------------
    // MEASURED 2026-09-03 (docs/STRAY-DUALSENSE.md §14): VibrationEndpointSubmix is a
    // UEndpointSubmix whose EndpointType is "Vibration Output". UE 4.27 has no factory for
    // that name on Windows, so IAudioEndpointFactory::Get hands it the DUMMY factory
    // (IAudioEndpoint.cpp:174) and FMixerSubmix::ProcessAudioAndSendToEndpoint returns before
    // processing a single child (AudioMixerSubmix.cpp, IsDummyEndpointSubmix) - the whole
    // vibration subtree is never rendered, which is why the tap saw ZERO callbacks.
    //
    // The reroute re-parents Submix_vibrationMaster under a submix that IS rendered every
    // callback (SubmixRerouteParent), whose OutputVolume is first set to 0 so nothing leaks
    // into the speakers, and asks the engine to rebuild the links by calling
    // FAudioDevice::RegisterSoundSubmix(submix, true) - a virtual two slots below the one the
    // tap already calls (AudioDevice.h:854, slot 14). The listener on Submix_vibrationMaster
    // then sees the full mix (listeners run after the submix's OWN volume, which stays 1.0,
    // and before the parent's). Default OFF: it writes two UPROPERTYs and calls one more
    // derived vtable slot.
    bool        submixReroute       = false;
    std::string submixRerouteMaster = "/Game/Sound/tools/settings/Submix_vibrationMaster.Submix_vibrationMaster";
    std::string submixRerouteParent = "/Game/Sound/tools/settings/Submix_unused.Submix_unused";
    int         submixRegisterSoundSubmixSlot = 14;

    // BP_HKPlayerController_C.DebugPS5Haptic (MEASURED in the object dump, offset 0x778):
    // a bool beside the PS5 platform gate in every StartPS5Vibration Blueprint. When on, the
    // plugin sets it TRUE on the player controller from inside its own hooks (game thread),
    // so the Blueprint body past the gate runs on PC. Default OFF until the probe proves
    // what the gate is.
    bool forcePS5HapticPath = false;

    // ---- button glyphs -------------------------------------------------------------------
    // /Script/Hk_project.InputSubsystem:GetGameControllerType(_forceGamepad: bool) returns
    // EGameControllerType (MEASURED: 0 Unknown, 1 XBOX, 2 PS4, 3 PS5, 4 SwitchPro,
    // 5 KeyboardMouse). UMG_KeyIcon's Set Key calls it to pick the prompt texture. With the
    // pad presented as an X360 device the game answers XBOX; a post-hook rewrites the return.
    // -1 = leave the game's answer alone.
    int glyphControllerType = 3;

    // THE REROUTE WATCHDOG. The reroute is submitted ONCE at bind time, and both halves of it
    // — the glue's ParentSubmix/OutputVolume writes and our RegisterSoundSubmix call — are
    // gated on SubmixWantsBinding(), which latches false forever after the first bind. So a
    // submix graph rebuilt by a level load could never be repaired, and the pad went silent
    // for the rest of the session (measured 2026-09-03: worked in BaseMap, silent in
    // 03_Slums). If the tap is bound and its callbacks stop advancing for this long, re-arm
    // the reroute. 0 disables it. This is NOT a fallback — it never gives the coils back to
    // the asset path; it repairs the thing that makes strict mode unreliable.
    float submixRerouteWatchdogSeconds = 5.0f;

    // ---- controller speaker ----------------------------------------------------------
    bool speaker = true;

    // WHICH MECHANISM SELECTS THE PAD'S AUDIO ROUTING. Read PadAudio.hpp first: the pad's
    // default routing MUTES the internal speaker, so writing correct samples into its audio
    // endpoint — which we do, with zero failures and megabytes streamed — reaches nothing
    // until something selects a route.
    //
    //   sony   scePadSetAudioOutPath / scePadSetVolumeGain. What the retired libScePad shim
    //          did, on this hardware, when the speaker last worked.
    //   hid    the raw output-report claim. The fallback for a libScePad that refuses.
    //   both   sony, then hid unconditionally.
    //   auto   sony, escalating to hid ONLY if Sony's call failed. THE DEFAULT: the HID
    //          claim's coil safety is argued (a disjoint bit set) rather than measured, so it
    //          runs only in the world where the measured path is already broken.
    //   off    write nothing. The behaviour that left the speaker silent.
    PadSpeakerRoute padSpeakerRoute = PadSpeakerRoute::Auto;

    // ScePadAudioOutPath. 3 = SPEAKER is the shim's measured-working value; do not change it
    // on a reading of the kernel's OUTPUT_PATH_SEL table, which is a DIFFERENT enum.
    int padSpeakerPath = kSceAudioOutPathSpeaker;
    // ScePadVolumeGain speakerVolume and jackVolume. The shim passed 80 for both.
    int padSpeakerGain = kSceVolumeGainDefault;

    // Re-apply the routing on this cadence as well as on change. 0 = only on change, which is
    // what the shim did (one call, and the speaker worked). Raise it only if the routing is
    // observed being lost mid-session — libScePad writes its own reports for triggers and
    // rumble, and whether any of them resets the audio path is UNCONFIRMED.
    float padSpeakerReassertSeconds = 0.0f;

    // The HID fallback's own values (PadSpeakerRoute = hid/both, or auto after a refusal).
    // These are the KERNEL's numbering and have nothing to do with padSpeakerPath above.
    int padSpeakerHidPath   = kPadSpeakerPathDefault;     // 2: speaker on, headphones alive
    int padSpeakerHidVolume = kPadSpeakerVolumeDefault;   // 0x64
    int padSpeakerHidPreamp = kPadSpeakerPreampDefault;   // +6 dB

    // ---- the pad's WASAPI endpoint ---------------------------------------------------
    // Substring of the friendly name. MEASURED: "Speakers (DualSense Wireless Controller)",
    // 4ch / 48000 Hz / 32-bit float, FL FR = speaker, RL RR = the two coils.
    std::string endpointMatch = "DualSense";

    // ---- assets ----------------------------------------------------------------------
    // Relative paths resolve against the game's Binaries/Win64 first, the mod's dir second.
    std::string hapticDir       = "haptic";            // <name>.f32, stereo float32 @ 48 kHz
    std::string spkDir          = "spk";               // <name>.f32, mono float32 @ 48 kHz
    std::string hapticLoopsFile = "haptic_loops.txt";  // the game's bLooping, one name per line
    std::string spkLoopsFile    = "spk_loops.txt";

    // ---- diagnostics -----------------------------------------------------------------
    float hookRetrySeconds    = 3.0f;    // MEASURED: registration fails until the BP loads
    float statusSeconds       = 30.0f;
    float configReloadSeconds = 5.0f;    // 0 disables hot reload

    // Parse `path`. Missing keys keep their defaults; unknown keys are logged and ignored.
    // Returns false only if the file could not be opened.
    bool Load(const std::wstring& path);

    // Re-read if the file's mtime moved. Only the LIVE scalars are applied; paths, the pad
    // slot and the endpoint match are read once by threads already running and are logged
    // as needing a relaunch rather than half-applied.
    bool ReloadIfChanged(const std::wstring& path);

    void LogSummary(const char* what) const;
    const char* HapticSourceName() const { return ::sds::HapticSourceName(hapticSource); }
    bool SubmixTapWanted() const { return TapWanted(hapticSource); }
    bool SubmixDrivesCoils() const { return SubmixMayDriveCoils(hapticSource); }
    const char* GlyphName() const;

private:
    uint64_t m_lastWriteTime = 0;
};

} // namespace sds
