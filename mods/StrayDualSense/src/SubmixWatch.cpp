#include "SubmixWatch.hpp"

namespace sds {

WatchVerdict SubmixWatch::Close()
{
    WatchVerdict v;
    v.asset   = m_asset;
    v.peak    = m_peak;
    v.windows = m_windows;
    v.ms      = m_endMs >= m_startMs ? (m_endMs - m_startMs) : 0;
    v.carried = m_peak >= m_threshold;
    m_open    = false;
    m_asset.clear();
    m_peak    = 0.0f;
    m_windows = 0;
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
    m_startMs   = nowMs;
    m_endMs     = nowMs + windowMs;
    return hadOne;
}

void SubmixWatch::Sample(float peak)
{
    if (!m_open)
        return;
    ++m_windows;
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
