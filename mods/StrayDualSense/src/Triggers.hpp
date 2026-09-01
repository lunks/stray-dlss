// StrayDualSense — adaptive triggers.
//
// MEASURED (docs/STRAY-DUALSENSE.md §13): the game calls SetPS5TriggerActivated(State, Side)
// once per side in the same instant — (true, Left) then (true, Right) — then both false
// together, with NOTHING in between. It hardens BOTH triggers for the whole scratch and does
// not alternate per paw. So each call updates ONE side and the state ACCUMULATES; treating a
// call as authoritative lets the second of the pair win and only the right trigger fires.
//
// The effect itself is the game's authored PS5TriggerEffectData, handed in by the hook that
// read it (in the GAME's enum space; TriggerEffect.hpp translates).
//
// The hook only stores two bools and wakes this worker. scePadSetTriggerEffect is a USB HID
// write and the optional readback sleeps 120 ms; neither belongs on the game thread.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

#include "TriggerEffect.hpp"

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

    // ---- from a UFunction hook, i.e. the GAME thread ---------------------------------
    void SetSide(TriggerSide side, bool on);
    void ReleaseAll();
    // The authored effect. Re-transmitted if it changes while a side is engaged.
    void SetEffect(const TriggerEffect& effect);

    bool          Left() const  { return m_left.load(std::memory_order_relaxed); }
    bool          Right() const { return m_right.load(std::memory_order_relaxed); }
    unsigned long Transmits() const { return m_transmits.load(); }
    TriggerEffect Effect() const;

private:
    void WorkerMain();

    ScePad*       m_pad    = nullptr;
    const Config* m_config = nullptr;

    std::thread             m_worker;
    std::atomic<bool>       m_running{false};
    mutable std::mutex      m_mutex;
    std::condition_variable m_cv;
    std::atomic<uint64_t>   m_epoch{0};   // bumped on every requested change

    std::atomic<bool> m_left{false};
    std::atomic<bool> m_right{false};
    TriggerEffect     m_effect = kFallbackTriggerEffect;   // guarded by m_mutex

    std::atomic<unsigned long> m_transmits{0};
};

} // namespace sds
