#include "Haptics.hpp"

#include "Config.hpp"
#include "Log.hpp"
#include "ScePad.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <timeapi.h>

#include <algorithm>
#include <cstdio>

namespace sds {
namespace {

std::wstring Widen(const std::string& s)
{
    if (s.empty()) return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<size_t>(n - 1), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

// A UFunction argument can name anything; a name reaching the filesystem must not be able to
// escape the asset directory. Refuse rather than sanitise, so a surprising name is visible.
bool NameIsSafe(const std::string& s)
{
    if (s.empty() || s.size() > 96) return false;
    for (const char c : s)
    {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
        if (!ok) return false;
    }
    return s.find("..") == std::string::npos;
}

} // namespace

HapticEngine::~HapticEngine()
{
    Shutdown();
}

void HapticEngine::Start(ScePad& pad, const Config& config, std::wstring vibeDir)
{
    m_pad     = &pad;
    m_config  = &config;
    m_vibeDir = std::move(vibeDir);
    m_running.store(true, std::memory_order_release);
    m_worker = std::thread(&HapticEngine::WorkerMain, this);
    SDS_LOG_INFO("haptics: worker started, envelopes from %ls", m_vibeDir.c_str());
}

void HapticEngine::Shutdown()
{
    if (!m_running.exchange(false, std::memory_order_acq_rel))
        return;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        ++m_requestSeq;
        m_requestIsStop = true;
    }
    m_cv.notify_all();
    if (m_worker.joinable())
        m_worker.join();
    if (m_pad != nullptr)
        m_pad->SetVibration(0, 0);
}

void HapticEngine::Play(const std::string& name, float level, bool loop)
{
    if (!NameIsSafe(name))
    {
        SDS_LOG_ERROR("haptics: refusing unsafe asset name '%s'", name.c_str());
        return;
    }
    m_level.store(std::clamp(level, 0.0f, 1.0f), std::memory_order_relaxed);
    if (!m_padVibrationEnabled.load(std::memory_order_relaxed))
    {
        SDS_LOG_INFO("haptics: '%s' suppressed, PadVibrationEnabled is off", name.c_str());
        Stop();
        return;
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_requestName   = name;
        m_requestLoop   = loop;
        m_requestIsStop = false;
        ++m_requestSeq;
    }
    m_cv.notify_all();
}

void HapticEngine::Stop()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_requestIsStop = true;
        ++m_requestSeq;
    }
    m_cv.notify_all();
}

void HapticEngine::SetLevel(float level)
{
    m_level.store(std::clamp(level, 0.0f, 1.0f), std::memory_order_relaxed);
}

void HapticEngine::SetPadVibrationEnabled(bool on)
{
    const bool was = m_padVibrationEnabled.exchange(on, std::memory_order_relaxed);
    if (was == on)
        return;
    SDS_LOG_INFO("haptics: PadVibrationEnabled -> %d", on ? 1 : 0);
    if (!on)
        Stop();
}

std::string HapticEngine::CurrentName() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_currentName;
}

const std::vector<uint8_t>* HapticEngine::LoadEnvelope(const std::string& name)
{
    {
        std::lock_guard<std::mutex> lock(m_cacheMutex);
        const auto it = m_cache.find(name);
        if (it != m_cache.end())
            return it->second.empty() ? nullptr : &it->second;
    }

    const std::wstring path = m_vibeDir + Widen(name) + L".env";
    std::vector<uint8_t> data;

    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") == 0 && f != nullptr)
    {
        std::fseek(f, 0, SEEK_END);
        const long n = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        if (n > 0)
        {
            data.resize(static_cast<size_t>(n));
            const size_t got = std::fread(data.data(), 1, data.size(), f);
            data.resize(got);
        }
        std::fclose(f);
    }

    if (data.empty())
    {
        m_missing.fetch_add(1, std::memory_order_relaxed);
        // Loud: a silent miss here is indistinguishable from "the game never asked".
        SDS_LOG_ERROR("haptics: envelope MISSING or empty: %ls  (run tools/dualsense/"
                      "extract_assets.sh + envgen.sh to populate the vibe/ directory)",
                      path.c_str());
    }
    else
    {
        SDS_LOG_INFO("haptics: loaded %s (%zu steps, %.2fs)", name.c_str(), data.size(),
                     static_cast<double>(data.size()) *
                         (m_config != nullptr ? m_config->envelopeStepMs : 5) / 1000.0);
    }

    std::lock_guard<std::mutex> lock(m_cacheMutex);
    auto& slot = m_cache[name];
    slot = std::move(data);
    return slot.empty() ? nullptr : &slot;
}

bool HapticEngine::PlayEnvelope(const std::string& name, bool loop, uint64_t seq)
{
    const std::vector<uint8_t>* env = LoadEnvelope(name);
    if (env == nullptr)
        return false;

    const int stepMs = m_config != nullptr ? m_config->envelopeStepMs : 5;
    const int cap    = m_config != nullptr ? m_config->maxEnvelopeSteps : 12000;
    const int gain   = m_config != nullptr ? m_config->hapticGain : 255;

    m_started.fetch_add(1, std::memory_order_relaxed);
    SDS_LOG_INFO("haptics: play %s (%zu steps, loop=%d, gain=%d)", name.c_str(), env->size(),
                 loop ? 1 : 0, gain);

    // 5 ms steps need better than the default ~15.6 ms timer. libScePad already raises this
    // for its own timing (§7), but do not depend on someone else's side effect.
    ::timeBeginPeriod(1);

    bool superseded = false;
    bool capped     = false;
    long long steps = 0;

    // Deadline-based rather than a bare Sleep(5) per step: a Sleep that overshoots by 1 ms
    // 4000 times stretches a 20 s purr by four seconds.
    LARGE_INTEGER freq{};
    ::QueryPerformanceFrequency(&freq);
    LARGE_INTEGER start{};
    ::QueryPerformanceCounter(&start);

    do
    {
        for (size_t i = 0; i < env->size(); ++i)
        {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (m_requestSeq != seq) { superseded = true; break; }
            }
            if (!m_running.load(std::memory_order_acquire)) { superseded = true; break; }
            if (steps > cap) { capped = true; break; }

            // Rounded, not truncated: the old shim's (a*b*c + 32512) / 65025.
            const int lvl = static_cast<int>(m_level.load(std::memory_order_relaxed) * 255.0f + 0.5f);
            const int amp = ((*env)[i] * std::clamp(lvl, 0, 255) * std::clamp(gain, 0, 255) + 32512) / 65025;
            const uint8_t a = static_cast<uint8_t>(std::clamp(amp, 0, 255));
            if (m_pad != nullptr)
                m_pad->SetVibration(a, a);

            ++steps;
            LARGE_INTEGER now{};
            ::QueryPerformanceCounter(&now);
            const double elapsedMs =
                static_cast<double>(now.QuadPart - start.QuadPart) * 1000.0 /
                static_cast<double>(freq.QuadPart);
            const double dueMs = static_cast<double>(steps) * stepMs;
            if (dueMs > elapsedMs)
                ::Sleep(static_cast<DWORD>(dueMs - elapsedMs));
        }
    } while (loop && !superseded && !capped);

    ::timeEndPeriod(1);

    if (m_pad != nullptr)
        m_pad->SetVibration(0, 0);

    if (capped)
    {
        m_capped.fetch_add(1, std::memory_order_relaxed);
        SDS_LOG_ERROR("haptics: '%s' hit the %d-step runaway cap. A Stop went MISSING — the "
                      "game asked for a loop and never asked for it to end.", name.c_str(), cap);
    }
    m_finished.fetch_add(1, std::memory_order_relaxed);
    SDS_LOG_INFO("haptics: '%s' ended (steps=%lld superseded=%d capped=%d)", name.c_str(),
                 steps, superseded ? 1 : 0, capped ? 1 : 0);
    return true;
}

void HapticEngine::WorkerMain()
{
    while (m_running.load(std::memory_order_acquire))
    {
        std::string name;
        bool        loop = false;
        uint64_t    seq  = 0;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait(lock, [this] {
                return !m_running.load(std::memory_order_acquire) || m_requestSeq != m_servedSeq;
            });
            if (!m_running.load(std::memory_order_acquire))
                break;
            m_servedSeq = m_requestSeq;
            seq         = m_requestSeq;
            if (m_requestIsStop)
            {
                m_currentName.clear();
                lock.unlock();
                if (m_pad != nullptr)
                    m_pad->SetVibration(0, 0);
                continue;
            }
            name          = m_requestName;
            loop          = m_requestLoop;
            m_currentName = name;
        }
        PlayEnvelope(name, loop, seq);
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_requestSeq == seq)
                m_currentName.clear();
        }
    }
    if (m_pad != nullptr)
        m_pad->SetVibration(0, 0);
    SDS_LOG_INFO("haptics: worker exiting");
}

} // namespace sds
