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

// ---------------------------------------------------------------------------------------
// THE REROUTE WATCHDOG, over N tapped submixes.
//
// MEASURED 2026-09-03 (docs/STRAY-DUALSENSE.md §17): the reroute that makes the engine render
// the pad subtrees is submitted at bind time, and a LEVEL LOAD rebuilds the submix graph
// underneath it - the identical build carried peak 0.708 in BaseMap and delivered zero
// callbacks in 03_Slums for the rest of the session. So: once a lane has been called at least
// once, its callbacks stopping for `stalledMs` means the subtree is no longer rendered, and
// the reroute must be re-submitted. ONE re-arm covers every lane, because every rerouted
// master is re-registered under the same parent in one pass.
//
// Rules, each of which is a measured trap:
//   * never fire before a lane's FIRST callback - registration is asynchronous (the engine
//     runs it on the audio thread), so a freshly bound lane legitimately reads 0 for a while;
//   * fire on ANY lane that once rendered and then stalled - the speaker subtree can die
//     without the coil one if a map only re-creates part of the graph;
//   * after a re-arm is due, stay quiet until the caller reports it SUBMITTED (Rearmed):
//     the re-submission happens on the game thread, asynchronously to this check;
//   * cap the re-arms and say so, rather than re-submitting forever over a structural fault.
//
// Pure: no clock, no threads. The caller supplies the time and the counters.
// ---------------------------------------------------------------------------------------
constexpr std::size_t kWatchdogMaxLanes = 4;

class StallWatchdog
{
public:
    // Fold in every lane's current callback count. Returns true exactly when a re-arm has
    // become due on this observation; the caller then re-submits the reroute and calls
    // Rearmed(). `stalledMs` 0 disables the watchdog (always false).
    bool Observe(const std::uint64_t* callbacks, std::size_t lanes, std::uint64_t nowMs,
                 std::uint64_t stalledMs, unsigned long maxRearms);

    // The re-arm has been submitted; start watching afresh from these counts.
    void Rearmed(const std::uint64_t* callbacks, std::size_t lanes, std::uint64_t nowMs);

    unsigned long Rearms() const   { return m_rearms; }
    bool          Pending() const  { return m_pending; }
    // True once the cap was exceeded: the watchdog has stopped for good and said so.
    bool          GaveUp() const   { return m_gaveUp; }
    // Set by the Observe() that returned true: which lane stalled, and for how long.
    std::size_t   StalledLane() const  { return m_stalledLane; }
    std::uint64_t StalledForMs() const { return m_stalledForMs; }
    std::uint64_t StalledAt() const    { return m_stalledAtCallbacks; }

private:
    std::uint64_t m_last[kWatchdogMaxLanes]  = {};
    std::uint64_t m_since[kWatchdogMaxLanes] = {};   // 0 = not yet timed
    unsigned long m_rearms       = 0;
    bool          m_pending      = false;
    bool          m_gaveUp       = false;
    std::size_t   m_stalledLane  = 0;
    std::uint64_t m_stalledForMs = 0;
    std::uint64_t m_stalledAtCallbacks = 0;
};

// ---------------------------------------------------------------------------------------
// WHO DRIVES THIS PAIR OF CHANNELS RIGHT NOW, in one line. One instance per lane: the coils
// (Submix_vibrationMaster -> RL/RR) and the speaker (Submix_controllerMaster -> FL/FR).
//
// There is exactly one source - the engine's own mix through the tap - and no fallback, so
// the verdict is "the SUBMIX" or "NOBODY, and here is why". The reason is the load-bearing
// part: with no asset path to fall back on, a silent pad has to name the failing step (the
// listener page, the registration, a subtree the engine never renders, a subtree that only
// carries silence) or it is indistinguishable from a working pad in a quiet moment.
//
// Written 2026-09-03 after a session in which `submix bound=1` next to a vibrating pad was
// read as "the submix works" while every waveform felt came from the asset path. The asset
// path is gone; the sentence stays.
// ---------------------------------------------------------------------------------------
enum class LaneOwner : std::uint8_t
{
    Submix = 0,   // the engine's own mix, through the tap and the sink
    Nobody = 1,   // the pair is silent, and the verdict says why
};

const char* LaneOwnerName(LaneOwner o);

struct LaneFacts
{
    bool          enabled      = true;    // Config: Enabled && (Haptics | Speaker)
    bool          gameSwitch   = true;    // the game's own switch (PadVibrationEnabled); always
                                          // true for the speaker, which has none
    bool          tapCreated   = false;   // Tap::Create succeeded (pages allocated)
    bool          tapBound     = false;   // registration handed to the engine
    bool          tapRefused   = false;   // registration refused; the tap is dead
    std::uint64_t tapCallbacks = 0;       // OnNewSubmixBuffer count since bind
    bool          tapLive      = false;   // a real signal has been seen on this lane
};

struct LaneVerdict
{
    LaneOwner   owner    = LaneOwner::Nobody;
    // A periodic WARN is due: the lane is enabled and the submix is not delivering.
    bool        warn     = false;
    // The sentence after "COILS: " / "SPEAKER: ". Never empty.
    const char* headline = "";
    // Why, for the same line.
    const char* detail   = "";
};

LaneVerdict JudgeLane(const LaneFacts& f);

} // namespace sds
