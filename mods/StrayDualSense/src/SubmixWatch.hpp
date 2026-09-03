// StrayDualSense — correlate the engine's own submix level with the game's own vibration
// starts.
//
// WHY THIS EXISTS. The submix tap reports `peak` over the LAST STATUS WINDOW and then resets
// it (SubmixDsp.hpp, LevelMeter::Take). One pasted line therefore says only "the submix was
// silent for the last second", which is exactly what a working tap looks like whenever
// nothing is playing. On 2026-09-03 a single such line — `peak=0.00000` on a session whose
// `live=1` proved the tap HAD carried a signal — was read as "the submix delivers nothing",
// and everything downstream of that reading was reasoning about the wrong failure.
//
// The honest question is not "is the submix silent now" but "was the submix silent WHILE THE
// GAME WAS ASKING FOR A VIBRATION". That is what this answers: a StartPS5Vibration opens a
// watch, every status window's peak is folded into it, and when the window closes it prints
// ONE line naming the asset and what the engine put in the submix for it.
//
// Pure: no Windows, no UE4SS, no threads. The caller does the locking (Start comes from a
// UFunction hook on the game thread; Sample/Poll from UE4SS's update thread).
#pragma once

#include <cstdint>
#include <string>

namespace sds {

// What one watch CONCLUDED — three states, not two.
//
// CORRECTED 2026-09-03, and it is §15's own lesson recurring inside the instrument built to
// fix §15. This enum used to be the bool `carried`, so a watch that folded in ZERO windows
// reported `carried = false`, which `ReportWatch` printed as "the engine mixed NOTHING". That
// sentence is a claim about the MIXER. A watch with no windows has not measured the mixer at
// all — the tap handed it no frames — and the two worlds have completely different causes:
//
//   NoData  the tap delivered no audio while the watch was open. The subtree is not being
//           rendered, so the suspect is the REROUTE or the binding, NOT the game's mixing.
//           Measured live in 03_Slums: "peak 0.00000 over 1.2s (0 window(s))".
//   Silent  the tap delivered frames and their peak stayed under the threshold. THIS is the
//           real negative, and only this one licenses "the engine mixed nothing".
//   Mixed   the peak reached the threshold.
enum class WatchResult
{
    NoData,
    Silent,
    Mixed,
};

struct WatchVerdict
{
    std::string   asset;              // the short asset name, e.g. "Scratch_VIBE"
    float         peak    = 0.0f;     // the highest window peak seen while the watch was open
    int           windows = 0;        // how many status windows were folded in
    std::uint64_t frames  = 0;        // total frames the tap delivered across those windows
    std::uint64_t ms      = 0;        // how long the watch was open, in ms
    WatchResult   result  = WatchResult::NoData;

    // Kept so existing call sites and tests read naturally; it is now derived, and it is
    // deliberately FALSE for NoData — "we did not see it mixed" is true there, while
    // "the engine mixed nothing" is not.
    bool Carried() const { return result == WatchResult::Mixed; }
};

class SubmixWatch
{
public:
    // A StartPS5Vibration. Opens a watch that closes `windowMs` later.
    //
    // A start while a watch is already open CLOSES the old one and reports it through
    // `closed` (returning true), because the two are different questions and one must not
    // silently swallow the other. The replaced watch is reported with the frames it actually
    // got, however few — `windows` says so.
    bool Start(const std::string& asset, std::uint64_t nowMs, std::uint64_t windowMs,
               float threshold, WatchVerdict& closed);

    // One status window: its peak AND how many frames the tap actually delivered. No-op when
    // no watch is open.
    //
    // CALL THIS EVEN WHEN `frames` IS ZERO. The caller used to skip the call entirely on a
    // frameless window, which is precisely what made "no data" indistinguishable from
    // "silence" — the watch could not count what it was never told about.
    //
    // KNOWN AND DELIBERATE: the first window folded in straddles the start, so it can carry
    // signal from just BEFORE it. That biases the reading towards "the engine mixed it",
    // which is the safe direction — it can only make a NEGATIVE verdict harder to reach.
    void Sample(float peak, std::uint64_t frames);

    // Closes and reports the watch once `windowMs` has elapsed. Returns false otherwise.
    bool Poll(std::uint64_t nowMs, WatchVerdict& out);

    bool Open() const { return m_open; }

private:
    WatchVerdict  Close();

    bool          m_open      = false;
    std::string   m_asset;
    float         m_peak      = 0.0f;
    float         m_threshold = 0.0f;
    int           m_windows   = 0;
    std::uint64_t m_frames    = 0;
    std::uint64_t m_startMs   = 0;
    std::uint64_t m_endMs     = 0;
};

} // namespace sds
