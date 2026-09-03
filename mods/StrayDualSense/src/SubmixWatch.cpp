#include "SubmixWatch.hpp"

namespace sds {

WatchVerdict SubmixWatch::Close()
{
    WatchVerdict v;
    v.asset   = m_asset;
    v.peak    = m_peak;
    v.windows = m_windows;
    v.frames  = m_frames;
    v.ms      = m_endMs >= m_startMs ? (m_endMs - m_startMs) : 0;
    // NO FRAMES MEANS NO MEASUREMENT. Deciding this on `m_frames` rather than `m_windows` is
    // deliberate: a window can be folded in and still carry nothing, and it is the frames
    // that say whether the mixer was observed at all.
    if (m_frames == 0)
        v.result = WatchResult::NoData;
    else if (m_peak >= m_threshold)
        v.result = WatchResult::Mixed;
    else
        v.result = WatchResult::Silent;
    m_open    = false;
    m_asset.clear();
    m_peak    = 0.0f;
    m_windows = 0;
    m_frames  = 0;
    return v;
}

bool SubmixWatch::Start(const std::string& asset, std::uint64_t nowMs, std::uint64_t windowMs,
                        float threshold, WatchVerdict& closed)
{
    bool hadOne = false;
    if (m_open)
    {
        m_endMs = nowMs;
        closed  = Close();
        hadOne  = true;
    }
    m_open      = true;
    m_asset     = asset;
    m_peak      = 0.0f;
    m_threshold = threshold;
    m_windows   = 0;
    m_frames    = 0;
    m_startMs   = nowMs;
    m_endMs     = nowMs + windowMs;
    return hadOne;
}

void SubmixWatch::Sample(float peak, std::uint64_t frames)
{
    if (!m_open)
        return;
    ++m_windows;
    m_frames += frames;
    // A frameless window has no meter reading to contribute — its `peak` is whatever the
    // meter happened to hold — so it must not touch the peak. It still counts as a window,
    // which is how "we were looking and the tap gave us nothing" stays visible.
    if (frames == 0)
        return;
    // NaN never wins: the comparison is false for it, so a bad meter reading cannot fabricate
    // a "the engine mixed it" verdict.
    if (peak > m_peak)
        m_peak = peak;
}

bool SubmixWatch::Poll(std::uint64_t nowMs, WatchVerdict& out)
{
    if (!m_open || nowMs < m_endMs)
        return false;
    out = Close();
    return true;
}

// ---- the reroute watchdog ---------------------------------------------------------------

bool StallWatchdog::Observe(const std::uint64_t* callbacks, std::size_t lanes,
                            std::uint64_t nowMs, std::uint64_t stalledMs,
                            unsigned long maxRearms)
{
    if (callbacks == nullptr || stalledMs == 0 || m_gaveUp || m_pending)
        return false;
    if (lanes > kWatchdogMaxLanes)
        lanes = kWatchdogMaxLanes;

    for (std::size_t i = 0; i < lanes; ++i)
    {
        const std::uint64_t n = callbacks[i];
        if (n != m_last[i])
        {
            m_last[i]  = n;
            m_since[i] = nowMs;
            continue;
        }
        // Never before the first callback has EVER arrived on this lane: registration is
        // asynchronous, so a freshly bound lane legitimately reads 0 for a while, and
        // re-arming there would fight the bind instead of repairing it.
        if (n == 0 || m_since[i] == 0)
        {
            m_since[i] = nowMs;
            continue;
        }
        if (nowMs - m_since[i] < stalledMs)
            continue;

        // Stalled. Count the re-arm first so the cap is a count of ATTEMPTS, then decide.
        const unsigned long attempt = ++m_rearms;
        if (attempt > maxRearms)
        {
            m_gaveUp = true;
            return false;
        }
        m_stalledLane        = i;
        m_stalledForMs       = nowMs - m_since[i];   // BEFORE the reset, or it reads 0
        m_stalledAtCallbacks = n;
        m_since[i]           = nowMs;
        m_pending            = true;
        return true;
    }
    return false;
}

void StallWatchdog::Rearmed(const std::uint64_t* callbacks, std::size_t lanes,
                            std::uint64_t nowMs)
{
    if (lanes > kWatchdogMaxLanes)
        lanes = kWatchdogMaxLanes;
    for (std::size_t i = 0; i < lanes; ++i)
    {
        m_last[i]  = callbacks != nullptr ? callbacks[i] : 0;
        m_since[i] = nowMs;
    }
    m_pending = false;
}

// ---- the lane verdict -------------------------------------------------------------------

const char* LaneOwnerName(LaneOwner o)
{
    switch (o)
    {
    case LaneOwner::Submix: return "SUBMIX";
    case LaneOwner::Nobody:
    default:                return "NOBODY";
    }
}

namespace {

// Why the submix is not delivering, in the order a reader would want to rule things out.
const char* LaneShortfall(const LaneFacts& f)
{
    if (!f.tapCreated)  return "the listener could not even be allocated";
    if (f.tapRefused)   return "the listener registration was REFUSED (see the ERROR above), so the tap is dead for this session";
    if (!f.tapBound)    return "the tap is not registered yet (no game-thread hook has fired, the submixes have not loaded, or FAudioDevice was not found)";
    if (f.tapCallbacks == 0)
        return "the tap is registered but the engine has NEVER called it: this submix is not being rendered (the reroute is the suspect)";
    return "the tap is called but has only ever carried silence (no real signal yet)";
}

} // namespace

LaneVerdict JudgeLane(const LaneFacts& f)
{
    LaneVerdict v;
    if (!f.enabled)
    {
        v.owner    = LaneOwner::Nobody;
        v.headline = "NOBODY";
        v.detail   = "disabled in StrayDualSense.ini (Enabled=0, or this lane's own switch)";
        return v;
    }
    if (!f.gameSwitch)
    {
        v.owner    = LaneOwner::Nobody;
        v.headline = "NOBODY";
        v.detail   = "PadVibrationEnabled is OFF in the game's settings menu";
        return v;
    }
    if (f.tapLive)
    {
        v.owner    = LaneOwner::Submix;
        v.headline = "driven by the SUBMIX";
        v.detail   = "the engine's own mix, through the tap; there is no other source";
        return v;
    }
    v.owner    = LaneOwner::Nobody;
    v.warn     = true;
    v.headline = "NOBODY - SILENT, and there is no fallback";
    v.detail   = LaneShortfall(f);
    return v;
}

} // namespace sds
