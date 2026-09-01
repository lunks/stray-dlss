// StrayDualSense — haptics.
//
// Plays the game's own VIBE amplitude envelopes (one uint8 per 5 ms, produced by
// tools/dualsense/envgen.sh with ONE shared scale across the whole set) through
// scePadSetVibration.
//
// THREADING, and this is the lesson that cost the old mod a session
// (docs/STRAY-DUALSENSE.md §11): the thread that RECEIVES a request must never be the thread
// that PLAYS it. An inline looping playback made the old watcher deaf to `stop`, so the purr
// never stopped. Here the caller only ever bumps a sequence number; the worker drains
// it, and a newer request supersedes whatever is in flight.
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
class ScePad;

class HapticEngine
{
public:
    ~HapticEngine();

    // `vibeDir` must already be an absolute directory ending in a separator.
    void Start(ScePad& pad, const Config& config, std::wstring vibeDir);
    void Shutdown();

    // ---- called from a UFunction hook, i.e. the GAME thread ---------------------------
    // (SetPadVibrationEnabled may also arrive from there.) None of these block, allocate
    // unboundedly, touch the disk or call the pad.
    // `level` is 0..1 as the game supplies it (StartPS5Vibration's Level argument).
    void Play(const std::string& name, float level, bool loop);
    void Stop();
    // SetPS5VibrationLevel fires at ~60 Hz. Cheap: one atomic store.
    void SetLevel(float level);
    // HKGameUserSettings.PadVibrationEnabled. Turning it off stops anything playing.
    void SetPadVibrationEnabled(bool on);

    // ---- status ----------------------------------------------------------------------
    unsigned long Started() const  { return m_started.load(); }
    unsigned long Finished() const { return m_finished.load(); }
    unsigned long Missing() const  { return m_missing.load(); }
    unsigned long Capped() const   { return m_capped.load(); }
    std::string   CurrentName() const;

private:
    void WorkerMain();
    // Returns false if the envelope could not be loaded.
    bool PlayEnvelope(const std::string& name, bool loop, uint64_t seq);
    const std::vector<uint8_t>* LoadEnvelope(const std::string& name);

    ScePad*       m_pad    = nullptr;
    const Config* m_config = nullptr;
    std::wstring  m_vibeDir;

    std::thread             m_worker;
    std::atomic<bool>       m_running{false};
    mutable std::mutex      m_mutex;
    std::condition_variable m_cv;

    // The request slot. Guarded by m_mutex; the sequence number is what the worker watches.
    std::string          m_requestName;
    bool                 m_requestLoop = false;
    uint64_t             m_requestSeq  = 0;
    uint64_t             m_servedSeq   = 0;
    bool                 m_requestIsStop = true;
    std::string          m_currentName;

    std::atomic<float>   m_level{1.0f};
    std::atomic<bool>    m_padVibrationEnabled{true};

    std::mutex m_cacheMutex;
    std::unordered_map<std::string, std::vector<uint8_t>> m_cache;

    std::atomic<unsigned long> m_started{0};
    std::atomic<unsigned long> m_finished{0};
    std::atomic<unsigned long> m_missing{0};
    std::atomic<unsigned long> m_capped{0};
};

} // namespace sds
