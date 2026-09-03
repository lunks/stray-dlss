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

} // namespace sds
