#include "Triggers.hpp"

#include "Config.hpp"
#include "Log.hpp"
#include "ScePad.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <chrono>

namespace sds {

TriggerEngine::~TriggerEngine()
{
    Shutdown();
}

void TriggerEngine::Start(ScePad& pad, const Config& config)
{
    m_pad    = &pad;
    m_config = &config;
    m_running.store(true, std::memory_order_release);
    m_worker = std::thread(&TriggerEngine::WorkerMain, this);
    SDS_LOG_INFO("triggers: worker started (readback=%d); effect until the game's is read: "
                 "game mode %d(%s) v=%u/%u/%u", config.triggerReadback ? 1 : 0,
                 kFallbackTriggerEffect.mode, GameModeName(kFallbackTriggerEffect.mode),
                 kFallbackTriggerEffect.value1, kFallbackTriggerEffect.value2,
                 kFallbackTriggerEffect.value3);
}

void TriggerEngine::Shutdown()
{
    if (!m_running.exchange(false, std::memory_order_acq_rel))
        return;
    ReleaseAll();
    m_cv.notify_all();
    if (m_worker.joinable())
        m_worker.join();
    // Leaving a trigger stiff after the mod goes away would be the worst possible exit.
    if (m_pad != nullptr && m_pad->HasPad())
    {
        uint8_t param[kTriggerParamSize];
        const TriggerEffect effect = Effect();
        BuildTriggerParam(false, false, effect, param);
        m_pad->SetTriggers(param, false, false, effect);
    }
}

void TriggerEngine::SetSide(TriggerSide side, bool on)
{
    std::atomic<bool>& slot = (side == TriggerSide::Left) ? m_left : m_right;
    if (slot.exchange(on, std::memory_order_relaxed) == on)
        return;   // idempotent: resending identical params every frame buzzes audibly
    m_epoch.fetch_add(1, std::memory_order_release);
    m_cv.notify_all();
}

void TriggerEngine::ReleaseAll()
{
    const bool l = m_left.exchange(false, std::memory_order_relaxed);
    const bool r = m_right.exchange(false, std::memory_order_relaxed);
    if (!l && !r)
        return;
    m_epoch.fetch_add(1, std::memory_order_release);
    m_cv.notify_all();
}

void TriggerEngine::SetEffect(const TriggerEffect& effect)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_effect == effect)
            return;
        m_effect = effect;
    }
    m_epoch.fetch_add(1, std::memory_order_release);
    m_cv.notify_all();
}

TriggerEffect TriggerEngine::Effect() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_effect;
}

void TriggerEngine::WorkerMain()
{
    uint64_t      seen = 0;
    bool          transmitted = false;
    bool          lastL = false, lastR = false;
    TriggerEffect lastEffect;

    while (m_running.load(std::memory_order_acquire))
    {
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait_for(lock, std::chrono::milliseconds(250), [this, seen] {
                return !m_running.load(std::memory_order_acquire) ||
                       m_epoch.load(std::memory_order_acquire) != seen;
            });
        }
        if (!m_running.load(std::memory_order_acquire))
            break;
        seen = m_epoch.load(std::memory_order_acquire);

        if (m_config == nullptr || !m_config->triggers || m_pad == nullptr || !m_pad->HasPad())
            continue;

        const bool          l      = m_left.load(std::memory_order_relaxed);
        const bool          r      = m_right.load(std::memory_order_relaxed);
        const TriggerEffect effect = Effect();
        if (transmitted && l == lastL && r == lastR && effect == lastEffect)
            continue;
        // An effect change while both sides are off changes nothing on the pad.
        if (transmitted && !l && !r && !lastL && !lastR)
        {
            lastEffect = effect;
            continue;
        }
        transmitted = true;
        lastL = l; lastR = r; lastEffect = effect;

        uint8_t param[kTriggerParamSize];
        BuildTriggerParam(l, r, effect, param);
        m_pad->SetTriggers(param, l, r, effect);
        m_transmits.fetch_add(1, std::memory_order_relaxed);

        if (m_config->triggerReadback)
        {
            // The pad's own input report, not an API echo (§5). The FIRST engage after open
            // always reads 0/0 because no report has arrived yet — never judge on one sample.
            ::Sleep(120);
            uint32_t sl = 0, sr = 0;
            const bool ok = m_pad->GetTriggerState(sl, sr);
            SDS_LOG_INFO("triggers: hardware readback ok=%d L2state=%u R2state=%u "
                         "(first sample after open is expected to read 0/0)",
                         ok ? 1 : 0, sl, sr);
        }
    }
    SDS_LOG_INFO("triggers: worker exiting");
}

} // namespace sds
