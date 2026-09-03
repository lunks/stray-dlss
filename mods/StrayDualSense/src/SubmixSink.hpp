// StrayDualSense — the two rings -> the pad's one 4-channel stream.
//
// One thread, ONE IAudioClient on the pad's endpoint, pulling stereo frames from two rings
// the taps left behind and writing them to the endpoint's two channel pairs:
//
//   Submix_vibrationMaster  -> coil ring    -> RL/RR   (left grip, right grip)   §12
//   Submix_controllerMaster -> speaker ring -> FL/FR   (the internal speaker)    §10/§16
//
// The engine has already mixed, faded, looped and levelled both, and has already run the
// speaker through its own SBFX_Boost chain, so nothing here is a level: the two gains exist
// for the game's PadVibrationEnabled switch and for an ini A/B (SpeakerGain), and the soft
// clip is there because a live mix of several concurrent haptics has no authored headroom
// and a square corner on a voice coil buzzes.
//
// Sony's own libScePad selects what the pad DOES with these channels (scePadSetAudioOutPath,
// Runtime::ApplySpeakerRoute); this file only puts the samples where the hardware reads them.
//
// The HID waveform mode is re-asserted on the coil lane's SILENT -> SIGNAL transition
// (rate-limited), on top of HidMode's own 2 s cadence. Without valid_flag0 = 0x00 the coils
// are busy emulating rumble and every sample goes nowhere (§12) — that is the single most
// important byte in this plugin, so it is asserted from both places.
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

    // Both rings are owned by the caller and must outlive this object. A lane whose tap is
    // not attached yet simply reads as silence (and counts underruns) until it is.
    void Start(SubmixRing* coilRing, SubmixRing* speakerRing, std::string endpointMatch,
               const Config& config, BeforePlayFn beforeCoils);
    void Shutdown();

    // The engine's own sample rate, learned from the first tap callback. Anything other than
    // the endpoint's rate engages the resamplers rather than silently changing the pitch.
    void SetSourceRate(std::uint32_t rate);
    void SetCoilGain(float gain);
    void SetSpeakerGain(float gain);

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

    SubmixRing*  m_coilRing    = nullptr;
    SubmixRing*  m_speakerRing = nullptr;
    std::string  m_endpointMatch;
    BeforePlayFn m_beforeCoils;
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

    std::atomic<float> m_coilGain{1.0f};
    std::atomic<float> m_speakerGain{1.0f};

    mutable std::mutex m_nameMutex;
    std::string        m_endpointName;

    // Worker only. One resampler per lane: it is stateful across buffers.
    LinearResampler    m_coilResampler;
    LinearResampler    m_speakerResampler;
    std::vector<float> m_coilPull;
    std::vector<float> m_speakerPull;
    std::vector<float> m_coilOut;
    std::vector<float> m_speakerOut;
};

} // namespace submix
} // namespace sds
