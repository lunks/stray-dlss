// StrayDualSense — WHO IS DRIVING THE VOICE COILS RIGHT NOW, stated in one line.
//
// Written 2026-09-03 after the first bound-tap session misled the user. The log said
// `submix bound=1`, the pad vibrated, and the natural reading was "the submix works". It did
// not: the tap had ZERO callbacks all session and every waveform the user felt came from the
// ASSET path, which stays in charge until the tap carries a real signal. Two true facts on
// two different lines added up to one false conclusion, because nothing anywhere said, in
// plain words, which path owned the coils.
//
// This module is that sentence. It is pure — a struct of facts in, a verdict out — so CI
// proves every combination, and every status line, warning and gate in the runtime asks it
// rather than re-deriving the answer from `bound=` and `total=`.
//
// The modes, and the rule each one imposes:
//
//   assets           the shipped path. No tap. Assets own the coils.
//   measure          the tap REPORTS; assets own the coils; nothing new reaches the pad.
//   submix-fallback  assets own the coils until the tap carries a real signal, then the
//                    submix takes over. The handover is LOUD, and so is every second before
//                    it: this mode can never again feel like a working submix.
//   submix           the submix or NOTHING. The asset path never plays. A silent submix
//                    means a silent pad, by configuration, and the log says so on a cadence.
//                    This is the honest mode for proving the engine's mix reaches the coils:
//                    if the user feels anything, it came from the submix.
//
// No Windows, no UE4SS, no engine. Portable.
#pragma once

#include <cstdint>

namespace sds {

enum class HapticSource : std::uint8_t
{
    Assets         = 0,
    Measure        = 1,
    SubmixFallback = 2,
    Submix         = 3,
};

const char* HapticSourceName(HapticSource s);

// True for every mode that creates a listener.
constexpr bool TapWanted(HapticSource s) { return s != HapticSource::Assets; }
// True for every mode in which the submix MAY feed the coils.
constexpr bool SubmixMayDriveCoils(HapticSource s)
{
    return s == HapticSource::Submix || s == HapticSource::SubmixFallback;
}

enum class CoilOwner : std::uint8_t
{
    Assets = 0,   // the shipped one-slot asset player
    Submix = 1,   // the engine's own mix, through the tap and the sink
    Nobody = 2,   // the pad is silent, and the verdict says why
};

const char* CoilOwnerName(CoilOwner o);

struct CoilFacts
{
    HapticSource  mode             = HapticSource::Assets;
    bool          hapticsEnabled   = true;    // Config: Enabled && Haptics
    bool          padVibration     = true;    // HKGameUserSettings.PadVibrationEnabled
    bool          tapCreated       = false;   // Tap::Create succeeded (pages allocated)
    bool          tapBound         = false;   // registration handed to the engine
    bool          tapRefused       = false;   // registration refused; the tap is dead
    std::uint64_t tapCallbacks     = 0;       // OnNewSubmixBuffer count since bind
    bool          tapLive          = false;   // a real signal has been seen (the handover)
};

struct CoilVerdict
{
    CoilOwner   owner           = CoilOwner::Assets;
    // May the asset player start a waveform right now? False whenever the owner is not the
    // asset path — including "Nobody", so a strict submix session never quietly falls back.
    bool        assetPathActive = true;
    // A periodic WARN is due: the configuration asked for the submix and the submix is not
    // delivering. Never true in assets or measure mode.
    bool        warn            = false;
    // One line a person reads in a glance. Always starts with "COILS:".
    const char* headline        = "";
    // The reason behind the headline, for the same line.
    const char* detail          = "";
};

CoilVerdict JudgeCoils(const CoilFacts& f);

} // namespace sds
