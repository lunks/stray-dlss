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

#include "Log.hpp"

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
    // assets  the shipped behaviour: <gamedir>/haptic/<name>.f32, one playback slot, so a
    //         second haptic supersedes the first (measured: touching the cat kills the rain).
    // measure tap the engine's own vibration submix and REPORT THE NUMBERS, but leave the
    //         coils entirely to the asset path. This is the honest first step: it answers
    //         "is the engine mixing haptics for us" without risking the pad.
    // submix  tap the submix and feed the coils from it; the asset haptic path is disabled.
    //         The speaker path is untouched in every mode.
    enum class HapticSource : uint8_t { Assets = 0, Measure = 1, Submix = 2 };
    HapticSource hapticSource = HapticSource::Assets;

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

    float submixGain         = 1.0f;   // applied before the soft clip
    int   submixQueueAheadMs = 40;     // WASAPI lead on the coil stream; raise if it crackles
    int   submixRingMs       = 250;    // ring capacity; ~12 engine callbacks at the default
    float submixStatusSeconds = 1.0f;  // how often the numbers proof is written
    // One line, rewritten in place, next to the log — so the numbers can be read with `cat`
    // over ssh without the game overlay.
    std::string submixStatusFile = "stray-dualsense-submix.txt";

    // ---- controller speaker ----------------------------------------------------------
    bool speaker = true;

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
    const char* HapticSourceName() const;
    bool SubmixTapWanted() const { return hapticSource != HapticSource::Assets; }
    bool SubmixDrivesCoils() const { return hapticSource == HapticSource::Submix; }

private:
    uint64_t m_lastWriteTime = 0;
};

} // namespace sds
