// StrayDualSense — the ring -> voice coils half of the submix spike.
//
// One thread, one IAudioClient of its own on the pad's endpoint (so it MIXES with the
// speaker path rather than displacing it, §10), pulling stereo frames the tap left in the
// ring and writing them to RL/RR — left channel to the left grip, right to the right, the
// same routing the asset path uses (AudioPlayer's kCoilRoute, docs/STRAY-DUALSENSE.md §12).
//
// Differences from the asset path, all deliberate:
//
//  * It is a CONTINUOUS stream, not a per-asset one. The engine mixes whenever it wants, so
//    there is no "start" to hang a stream on; an underrun is silence, which is the right
//    answer for a haptic.
//  * SOFT clip, not the asset path's hard clamp. The engine's mix of several concurrent
//    haptics has no authored headroom guarantee, and a square corner on a voice coil buzzes.
//  * The HID waveform mode is re-asserted on the SILENT -> SIGNAL transition (rate-limited),
//    on top of HidMode's own 2 s cadence. Without valid_flag0 = 0x00 the coils are busy
//    emulating rumble and every sample goes nowhere (§12) — that is the single most
//    important byte in this plugin, so it is asserted from both places.
//
// Win32/COM only. No UE4SS types.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "SubmixDsp.hpp"

namespace sds {

struct Config;

namespace submix {

class SubmixSink
{
public:
    using BeforePlayFn = std::function<void()>;

    ~SubmixSink();

    // `ring` is owned by the caller and must outlive this object.
    void Start(SubmixRing* ring, std::string endpointMatch, const Config& config,
               BeforePlayFn beforePlay);
    void Shutdown();

    // The engine's own sample rate, learned from the first tap callback. Anything other than
    // the endpoint's rate engages the resampler rather than silently changing the pitch.
    void SetSourceRate(std::uint32_t rate);
    void SetGain(float gain);

    bool          StreamOpen() const   { return m_streamOpen.load(std::memory_order_relaxed); }
    std::uint32_t EndpointRate() const { return m_endpointRate.load(std::memory_order_relaxed); }
    std::uint32_t EndpointChannels() const { return m_endpointChannels.load(std::memory_order_relaxed); }
    std::uint64_t FramesWritten() const{ return m_framesWritten.load(std::memory_order_relaxed); }
    std::uint64_t Restarts() const     { return m_restarts.load(std::memory_order_relaxed); }
    std::uint64_t Failures() const     { return m_failures.load(std::memory_order_relaxed); }
    std::string   EndpointName() const;

private:
    void WorkerMain();
    bool RunStream();      // returns false when the worker should stop entirely

    SubmixRing*  m_ring = nullptr;
    std::string  m_endpointMatch;
    BeforePlayFn m_beforePlay;
    std::uint32_t m_queueAheadMs = 40;

    std::thread       m_worker;
    std::atomic<bool> m_running{false};

    std::atomic<std::uint32_t> m_sourceRate{kSubmixDefaultRate};
    std::atomic<std::uint32_t> m_endpointRate{0};
    std::atomic<std::uint32_t> m_endpointChannels{0};
    std::atomic<bool>          m_streamOpen{false};
    std::atomic<std::uint64_t> m_framesWritten{0};
    std::atomic<std::uint64_t> m_restarts{0};
    std::atomic<std::uint64_t> m_failures{0};

    std::atomic<float> m_gain{1.0f};

    mutable std::mutex m_nameMutex;
    std::string        m_endpointName;

    LinearResampler    m_resampler;   // worker only
    std::vector<float> m_pull;        // worker only
    std::vector<float> m_out;         // worker only
};

} // namespace submix
} // namespace sds
