// StrayDualSense — configuration.
//
// INI, not JSON: it is hand-edited on a machine that is mid-session, over ssh, by someone who
// wants to flip one switch and relaunch. It is hot-reloaded because this mod cannot be built
// or tested locally — the only tuning loop is the user editing a value on the box.
//
// There are deliberately FEW knobs. Everything the game authors (trigger effect, loops, fades,
// levels, the speaker's +5 dB trim) happens inside the engine's own mix, which is what the
// taps carry; a knob whose best value equals something the engine already knows is a missing
// wire, not a feature (CLAUDE.md, "a hand-tuned constant that works is a bug report").
//
// No UE4SS types here.
#pragma once

#include <cstdint>
#include <string>

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

    // ---- the coils: the mode byte ----------------------------------------------------
    bool haptics = true;
    // DualSense USB output report 0x02, byte 1 = valid_flag0. Bits are VALIDITY CLAIMS, not a
    // mode (HidMode.hpp). 0x00 claims NOTHING — and thereby stops re-asserting
    // compatible-vibration, which is all it takes for the coils to take the waveform (§12).
    // 0xFC is WRONG: it claims trigger data while supplying zeros, which first killed the
    // adaptive triggers and then latched them on. This exists to reproduce that measurement,
    // not to be tuned.
    int   hapticValidFlag0     = 0x00;
    // libScePad's own output reports (triggers, rumble) carry the same flag byte and undo a
    // single write; re-assert on this cadence AND on the coil lane's silence -> signal edge.
    float hapticReassertSeconds = 2.0f;

    // ---- the speaker -----------------------------------------------------------------
    bool speaker = true;
    // Applied to the speaker lane before the soft clip. 1.0 on purpose: the samples come out
    // of the engine's own Submix_controllerPre -> controller -> controllerMaster chain, so the
    // game's +5 dB SBFX_Boost is already in them. The retired asset-replay path baked that
    // trim in as a constant (kSpeakerBoost = 1.7783) BECAUSE it bypassed the chain; carrying
    // it over here would apply it twice (docs §16). A knob so the box can A/B it in one
    // session, never a level to tune.
    float speakerGain = 1.0f;
    // THE PAD'S OWN ROUTING, through Sony's API (PadAudio.hpp). The pad's default routing
    // MUTES the internal speaker, so the samples above reach nothing until this is selected.
    // ScePadAudioOutPath: 3 = SPEAKER is the shim's measured-working value.
    int padSpeakerPath = kSceAudioOutPathSpeaker;
    // ScePadVolumeGain speakerVolume and jackVolume. The shim passed 80 for both.
    int padSpeakerGain = kSceVolumeGainDefault;
    // Re-apply the routing on this cadence as well as on change. 0 = only on change, which is
    // what the shim did (one call, and the speaker worked). Raise it only if the routing is
    // observed being lost mid-session — libScePad writes its own reports for triggers and
    // rumble, and whether any of them resets the audio path is UNCONFIRMED.
    float padSpeakerReassertSeconds = 0.0f;

    // ---- the two submixes, and the reroute that makes the engine render them ----------
    // By the paths measured in the box's own UE4SS object dump (docs §14). The vibration
    // master feeds the coils (RL/RR); the controller master feeds the speaker (FL/FR), tapped
    // POST-effects so the game's own speaker tuning is in the samples.
    //
    // MEASURED 2026-09-03 (docs §14/§16): both roots are UEndpointSubmixes ("Vibration
    // Output", "Pad Speaker Output") with no factory on Windows, so UE 4.27 hands them the
    // DUMMY endpoint and skips both subtrees - a plain tap reports zero callbacks forever.
    // The reroute re-parents each master under SubmixRerouteParent (Submix_unused: rendered
    // every callback, no children, no effects), after setting that parent's OutputVolume to 0
    // so nothing leaks into the speakers, and asks the engine to rebuild the links through
    // FAudioDevice::RegisterSoundSubmix (vtable slot SubmixRegisterSoundSubmixSlot; measured
    // working at 14). Listeners run after the submix's own volume (1.0) and before the
    // parent's (0), so the taps see the full mix. Always on: without it there is nothing to
    // tap, and there is no other source.
    std::string submixPath          = "/Game/Sound/tools/settings/Submix_vibrationMaster.Submix_vibrationMaster";
    std::string submixSpeakerPath   = "/Game/Sound/tools/settings/Submix_controllerMaster.Submix_controllerMaster";
    std::string submixRerouteParent = "/Game/Sound/tools/settings/Submix_unused.Submix_unused";

    // WHERE THE LISTENERS GO, which is NOT what we re-parent. See src/SubmixRouting.hpp for
    // the derivation and docs/STRAY-DUALSENSE.md §20.1 for the measurement.
    //
    // Both masters are re-parented under ONE `Submix_unused`, which makes them siblings, and
    // UE 4.27 hands a buffer listener the PARENT's accumulation buffer after this submix mixed
    // into it (AudioMixerSubmix.cpp:1364, :1380) — zeroed once per callback, not once per
    // child. So a listener on the second sibling processed reads BOTH lanes. Measured in the
    // running game: the two lanes' `peak` and `rms` were bit-identical in 95.2% of 2,483
    // non-trivial status periods, and every `_VIBE` the engine mixed was emitted on the pad
    // SPEAKER as well as the coils.
    //
    // Tapping each master's own CHILD removes the sharing instead of compensating for it: the
    // buffer handed to a child's listener is its master's InputBuffer, which holds that
    // master's subtree and nothing else.
    //
    // EMPTY means "tap the master", i.e. the old aliasing behaviour — kept deliberately,
    // because it is the A/B that proves the fix inside one session with no rebuild. The
    // `submix alias` line reports a completely different number in the two modes.
    std::string submixTapPath        = "/Game/Sound/tools/settings/Submix_vibration.Submix_vibration";
    std::string submixSpeakerTapPath = "/Game/Sound/tools/settings/Submix_controller.Submix_controller";
    int         submixRegisterSoundSubmixSlot = 14;

    // FAudioDevice::RegisterSubmixBufferListener's vtable index. 16 for stock UE 4.27.2 and
    // measured working; Stray is a LICENSEE build, so it stays a knob and the log dumps the
    // vtable. See src/SubmixDiscovery.hpp for the full derivation.
    int submixRegisterSlot = 16;

    // both | world | engine. Which objects may supply the FAudioDevice pointer; the ladder in
    // SubmixChoice decides. MEASURED: UWorld holds no audio device on this build, UEngine does.
    std::string submixDeviceSource = "both";
    // How far into UWorld/UEngine to scan. 0x2000 was NOT ENOUGH: UEngine declares 268
    // UPROPERTYs before MainAudioDeviceHandle (Engine.h:1735).
    int   submixScanBytes    = 0x8000;
    // 32-bit words of each non-UObject candidate to dump when NOTHING is accepted.
    int   submixDumpWords    = 96;

    // A second, meter-only listener on the engine's MASTER submix, never attached to the
    // pad. It turns a null result into a diagnosis: master firing while a lane stays silent
    // means the tap works and the engine is not rendering that subtree; neither firing means
    // the tap is broken. Costs one extra downmix of the game's output per audio callback.
    bool submixProbeMaster = true;

    float submixGain         = 1.0f;   // the coil lane, before the soft clip
    int   submixQueueAheadMs = 40;     // WASAPI lead on the pad stream; raise if it crackles
    int   submixRingMs       = 250;    // per-lane ring capacity; ~12 engine callbacks
    float submixStatusSeconds = 1.0f;  // how often the numbers proof is written

    // THE CORRELATION INSTRUMENT (src/SubmixWatch.hpp). Every StartPS5Vibration opens the coil
    // lane's watch and every StartPS5ControllerSound the speaker lane's, this many seconds
    // wide; when it closes, ONE line says what the engine put in that submix for that asset
    // (NoData / Silent / Mixed). 0 disables the watches.
    float submixWatchSeconds = 3.0f;

    // What counts as a REAL SIGNAL: the peak, in the tap's own units, at or above which a
    // lane is called live and the sink opens. 1e-4 is -80 dBFS, deliberately low so a quiet
    // asset cannot be missed; a real VIBE asset measures ~0.7 (docs §14). The handover line
    // prints the peak that tripped it — if that is a small fraction of 0.7, raise this.
    float submixLiveThreshold = 1.0e-4f;

    // How often the WARN repeats while a lane is enabled and the submix is not delivering
    // (unbound, never called, or silent). 0 disables the cadence; the COILS:/SPEAKER: lines
    // still say who drives each pair.
    float submixWarnSeconds = 10.0f;

    // THE REROUTE WATCHDOG (docs §17). A level load rebuilds the submix graph and drops the
    // re-parenting; if a bound lane's callbacks stop advancing for this long AFTER having
    // advanced at least once, the reroute is re-submitted for the parent and both masters.
    // 0 disables it. The listeners are NOT re-registered (UE appends without de-duplicating).
    float submixRerouteWatchdogSeconds = 5.0f;

    // One line per lane, rewritten in place, next to the log — so the numbers can be read
    // with `cat` over ssh without the game overlay.
    std::string submixStatusFile = "stray-dualsense-submix.txt";

    // ---- button glyphs -------------------------------------------------------------------
    // /Script/Hk_project.InputSubsystem:GetGameControllerType(_forceGamepad: bool) returns
    // EGameControllerType (MEASURED: 0 Unknown, 1 XBOX, 2 PS4, 3 PS5, 4 SwitchPro,
    // 5 KeyboardMouse). UMG_KeyIcon's Set Key calls it to pick the prompt texture. With the
    // pad presented as an X360 device the game answers XBOX; a post-hook rewrites the return.
    // -1 = leave the game's answer alone.
    int glyphControllerType = 3;

    // ---- the pad's WASAPI endpoint ---------------------------------------------------
    // Substring of the friendly name. MEASURED: "Speakers (DualSense Wireless Controller)",
    // 4ch / 48000 Hz / 32-bit float, FL FR = speaker, RL RR = the two coils.
    std::string endpointMatch = "DualSense";

    // ---- diagnostics -----------------------------------------------------------------
    float hookRetrySeconds    = 3.0f;    // MEASURED: registration fails until the BP loads
    float statusSeconds       = 30.0f;
    float configReloadSeconds = 5.0f;    // 0 disables hot reload

    // Parse `path`. Missing keys keep their defaults; keys retired in 0.4.0 are named and
    // ignored; unknown keys are logged and ignored. Returns false only if the file could not
    // be opened.
    bool Load(const std::wstring& path);

    // Re-read if the file's mtime moved. Only the LIVE scalars are applied; paths, the pad
    // slot and the endpoint match are read once by threads already running and are logged
    // as needing a relaunch rather than half-applied.
    bool ReloadIfChanged(const std::wstring& path);

    void LogSummary(const char* what) const;
    const char* GlyphName() const;

private:
    uint64_t m_lastWriteTime = 0;
};

} // namespace sds
