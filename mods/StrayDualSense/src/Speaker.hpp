// StrayDualSense — the controller speaker.
//
// MEASURED (docs/STRAY-DUALSENSE.md §10): under GE-Proton with
// PROTON_SONY_WINDOWS_DEVICE_NAMES=1 and PROTON_KEEP_SONY_AUDIO_ENDPOINT_VISIBLE=1 the pad
// enumerates as an ordinary WASAPI render endpoint named "Speakers (DualSense Wireless
// Controller)", 4ch / 48000 Hz / 32-bit float, channel order FL FR RL RR.
//
// Write FL/FR ONLY. RL/RR are the haptic voice coils; writing them never reached them in
// testing and the rumble path covers that side anyway.
//
// Sony's own audio API is NOT used and cannot be: scePadSetAudioOutPath returns 0x80920007
// because libScePad never finds the pad's sibling USB audio interfaces in the Wine device
// tree. Nothing here touches ALSA either — both paths were verified working with the pad's
// ALSA `PCM` control muted.
//
// Same threading rule as the haptics: the calling thread only records a request.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace sds {

struct Config;

class SpeakerEngine
{
public:
    ~SpeakerEngine();

    // `spkDir` must already be an absolute directory ending in a separator.
    void Start(const Config& config, std::wstring spkDir);
    void Shutdown();

    // ---- called from a UFunction hook, i.e. the GAME thread ---------------------------
    void Play(const std::string& name, float level, bool loop);
    void Stop();
    void SetLevel(float level);

    unsigned long Started() const  { return m_started.load(); }
    unsigned long Missing() const  { return m_missing.load(); }
    unsigned long Failures() const { return m_failures.load(); }
    bool          EndpointFound() const { return m_endpointFound.load(); }

private:
    void WorkerMain();
    void PlayFile(const std::string& name, bool loop, uint64_t seq);
    const std::vector<float>* LoadMono(const std::string& name);

    const Config* m_config = nullptr;
    std::wstring  m_spkDir;

    std::thread             m_worker;
    std::atomic<bool>       m_running{false};
    std::mutex              m_mutex;
    std::condition_variable m_cv;

    std::string m_requestName;
    bool        m_requestLoop   = false;
    bool        m_requestIsStop = true;
    uint64_t    m_requestSeq    = 0;
    uint64_t    m_servedSeq     = 0;

    std::atomic<float> m_level{1.0f};

    std::mutex m_cacheMutex;
    std::unordered_map<std::string, std::vector<float>> m_cache;

    std::atomic<unsigned long> m_started{0};
    std::atomic<unsigned long> m_missing{0};
    std::atomic<unsigned long> m_failures{0};
    std::atomic<bool>          m_endpointFound{false};
};

} // namespace sds
