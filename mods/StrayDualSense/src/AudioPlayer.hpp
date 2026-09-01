// StrayDualSense — one waveform player, two routes.
//
// The pad's endpoint is 4ch: FL FR RL RR. Both of the game's asset families are float32 PCM
// at 48 kHz on disk (tools/dualsense/wavegen.sh) and both go to the SAME endpoint, so one
// player serves both, parameterised by a route:
//
//   haptics  <gamedir>/haptic/<name>.f32  stereo   L -> RL (left grip coil), R -> RR   gain 1.0
//   speaker  <gamedir>/spk/<name>.f32     mono     -> FL and FR                        gain +5 dB
//
// The VIBE assets are stereo BECAUSE there are two coils, one per grip (§12); collapsing them
// to one amplitude is what made the old rumble path feel wrong. The speaker trim is the game's
// own SBFX_Boost (§10), a named constant rather than a knob.
//
// THREADING (§11, learned the hard way): the thread that RECEIVES a request never PLAYS it.
// A hook only writes the request slot and bumps a sequence number; this worker drains it. A
// newer request supersedes whatever is in flight, so a Stop lands within one buffer top-up
// (~100 ms) even under a looping asset.
//
// FADES are honoured here: FadeInTime ramps the gain up from the first frame, and a Stop with
// FadeOutTime ramps it down before the stream ends. Both are plain gain ramps over the frames
// we generate (Fade.hpp).
//
// Win32/COM only. No UE4SS types.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace sds {

// SBFX_Boost on Submix_controllerPre: InputGainDb=+5.0, Ratio 1:1, Threshold 0 — a dynamics
// preset used purely as a level trim (§10). 10^(5/20).
constexpr float kSpeakerBoost = 1.7783f;

constexpr uint32_t kAssetSampleRate = 48000;   // what wavegen.sh writes; also the endpoint's

struct AudioRoute
{
    const char* tag;              // log prefix
    uint32_t    sourceChannels;   // 1 (mono) or 2 (stereo interleaved)
    uint32_t    outputMask[2];    // endpoint channels fed by source channel 0 / 1 (bit per channel)
    float       gain;             // constant trim applied on top of the game's level
};

// FL=0 FR=1 RL=2 RR=3, the measured channel order.
constexpr AudioRoute kCoilRoute    { "haptics", 2, { 1u << 2, 1u << 3 }, 1.0f };
constexpr AudioRoute kSpeakerRoute { "speaker", 1, { (1u << 0) | (1u << 1), 0u }, kSpeakerBoost };

class AudioPlayer
{
public:
    // Called on the worker immediately before a waveform starts. The coil route uses it to
    // re-assert valid_flag0 (HidMode) so the coils are in waveform mode when the first frame
    // lands, whatever libScePad wrote in between.
    using BeforePlayFn = std::function<void()>;

    ~AudioPlayer();

    // `dir` is absolute and ends in a separator. `endpointMatch` names the WASAPI endpoint.
    void Start(const AudioRoute& route, std::string endpointMatch, std::wstring dir,
               BeforePlayFn beforePlay);
    void Shutdown();

    // ---- from a UFunction hook, i.e. the GAME thread: record and wake, never block --------
    // `level` is 0..1 as the game supplies it; `loop` is the ASSET's own flag, resolved by
    // the caller from the loop list; `fadeInSeconds` is the game's FadeInTime.
    void Play(const std::string& name, float level, float fadeInSeconds, bool loop);
    // `fadeOutSeconds` is the game's FadeOutTime; 0 stops within one top-up.
    void Stop(float fadeOutSeconds);
    // SetPS5VibrationLevel fires at ~60 Hz. One atomic store.
    void SetLevel(float level);

    // ---- status --------------------------------------------------------------------------
    unsigned long Started() const  { return m_started.load(); }
    unsigned long Finished() const { return m_finished.load(); }
    unsigned long Missing() const  { return m_missing.load(); }
    unsigned long Failures() const { return m_failures.load(); }
    bool          EndpointFound() const { return m_endpointFound.load(); }
    std::string   CurrentName() const;
    bool          Playing() const { return m_playing.load(std::memory_order_relaxed); }

private:
    struct Request
    {
        std::string name;
        float       fadeIn  = 0.0f;
        float       fadeOut = 0.0f;
        bool        loop    = false;
        bool        isStop  = true;
        uint64_t    seq     = 0;
    };

    void WorkerMain();
    void PlayOne(const Request& req);
    bool LoadAsset(const std::string& name, std::vector<float>& out, size_t& outFrames);

    AudioRoute   m_route{};
    std::string  m_endpointMatch;
    std::wstring m_dir;
    BeforePlayFn m_beforePlay;

    std::thread             m_worker;
    std::atomic<bool>       m_running{false};
    mutable std::mutex      m_mutex;
    std::condition_variable m_cv;
    Request                 m_request;       // guarded by m_mutex
    uint64_t                m_servedSeq = 0; // guarded by m_mutex
    std::string             m_currentName;   // guarded by m_mutex

    std::atomic<float> m_level{1.0f};
    std::atomic<bool>  m_playing{false};

    std::atomic<unsigned long> m_started{0};
    std::atomic<unsigned long> m_finished{0};
    std::atomic<unsigned long> m_missing{0};
    std::atomic<unsigned long> m_failures{0};
    std::atomic<bool>          m_endpointFound{false};
    bool                       m_endpointsListed = false;   // worker only
};

} // namespace sds
