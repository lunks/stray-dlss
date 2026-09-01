// StrayDualSense — adaptive triggers.
//
// The game drives EACH SIDE SEPARATELY: SetPS5TriggerActivated fires twice per event, ~0.2 ms
// apart, as (State, Side=Left) then (State, Side=Right) (docs/STRAY-DUALSENSE.md §8). Carrying
// a single "current side" makes the second call overwrite the first and only one trigger ever
// hardens — so L and R are tracked independently here and both stay in the trigger mask.
//
// The hook only stores two bools and wakes this worker. scePadSetTriggerEffect is a USB HID
// write and the optional readback sleeps 120 ms; neither belongs on the game thread, which is
// where the hook fires.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

namespace sds {

struct Config;
class ScePad;

enum class TriggerSide : int { Left = 0, Right = 1 };   // EPS5TriggersSide, from the exe

class TriggerEngine
{
public:
    ~TriggerEngine();

    void Start(ScePad& pad, const Config& config);
    void Shutdown();

    // ---- called from a UFunction hook, i.e. the GAME thread ---------------------------
    void SetSide(TriggerSide side, bool on);
    void ReleaseAll();

    bool Left() const  { return m_left.load(std::memory_order_relaxed); }
    bool Right() const { return m_right.load(std::memory_order_relaxed); }
    unsigned long Transmits() const { return m_transmits.load(); }

private:
    void WorkerMain();

    ScePad*       m_pad    = nullptr;
    const Config* m_config = nullptr;

    std::thread             m_worker;
    std::atomic<bool>       m_running{false};
    std::mutex              m_mutex;
    std::condition_variable m_cv;
    std::atomic<uint64_t>   m_epoch{0};   // bumped on every requested change

    std::atomic<bool> m_left{false};
    std::atomic<bool> m_right{false};

    std::atomic<unsigned long> m_transmits{0};
};

} // namespace sds
