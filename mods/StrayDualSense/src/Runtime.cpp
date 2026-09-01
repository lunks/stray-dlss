#include "Runtime.hpp"

#include "AssetName.hpp"
#include "Log.hpp"
#include "Version.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <chrono>

namespace sds {
namespace {

uint64_t NowMs()
{
    return static_cast<uint64_t>(::GetTickCount64());
}

bool DirectoryExists(const std::wstring& path)
{
    const DWORD a = ::GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

std::wstring Widen(const std::string& s)
{
    if (s.empty()) return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<size_t>(n - 1), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

// Resolve a configured relative directory against the game's Binaries/Win64 first (where the
// assets are generated) and the mod's own directory second. Never hardcoded; always logged,
// because "the envelope was not found" and "we looked in the wrong place" are the same
// symptom otherwise.
std::wstring ResolveAssetDir(const std::string& configured, const std::wstring& gameDir,
                             const std::wstring& modDir, const char* what)
{
    const std::wstring rel = Widen(configured);
    const bool absolute = rel.size() > 1 && (rel[1] == L':' || rel[0] == L'\\' || rel[0] == L'/');
    if (absolute)
    {
        std::wstring p = rel;
        if (!p.empty() && p.back() != L'\\' && p.back() != L'/') p += L'\\';
        SDS_LOG_INFO("assets: %s -> %ls (absolute, exists=%d)", what, p.c_str(),
                     DirectoryExists(p) ? 1 : 0);
        return p;
    }

    const std::wstring a = gameDir + rel + L"\\";
    const std::wstring b = modDir  + rel + L"\\";
    const bool aOk = DirectoryExists(a);
    const bool bOk = DirectoryExists(b);
    SDS_LOG_INFO("assets: %s candidates: [game] %ls exists=%d | [mod] %ls exists=%d",
                 what, a.c_str(), aOk ? 1 : 0, b.c_str(), bOk ? 1 : 0);
    if (aOk) return a;
    if (bOk) return b;
    SDS_LOG_ERROR("assets: neither candidate for '%s' exists. Generate them with "
                  "tools/dualsense/extract_assets.sh and envgen.sh.", what);
    return a;   // keep the game-dir path so the per-asset error names the expected location
}

} // namespace

Runtime& Rt()
{
    static Runtime instance;
    return instance;
}

void Runtime::Startup(void* addressInsideThisModule)
{
    if (m_started.load(std::memory_order_acquire))
        return;

    m_gameDir = GameBinariesDir();
    m_modDir  = ModuleDir(addressInsideThisModule);

    // The log goes next to the game binaries, alongside scepad_shim.log's old home and
    // stray-dlss.log, because that is the directory the user already collects from.
    const std::wstring logDir = m_gameDir.empty() ? m_modDir : m_gameDir;
    Log::Open(logDir + L"stray-dualsense.log", LogLevel::Info);

    SDS_LOG_INFO("StrayDualSense %s starting", SDS_VERSION_STRING);
    SDS_LOG_INFO("  game binaries dir: %ls", m_gameDir.c_str());
    SDS_LOG_INFO("  mod dir          : %ls", m_modDir.c_str());

    // Config next to the mod DLL first (where an installer would put it), then the game dir.
    m_configPath = m_modDir + L"StrayDualSense.ini";
    if (!m_config.Load(m_configPath))
    {
        const std::wstring alt = logDir + L"StrayDualSense.ini";
        if (m_config.Load(alt))
        {
            m_configPath = alt;
            SDS_LOG_INFO("config: loaded %ls", m_configPath.c_str());
        }
        else
        {
            SDS_LOG_INFO("config: no StrayDualSense.ini found (looked next to the mod DLL and "
                         "in the game directory); using built-in defaults, which are the "
                         "values the working two-part mod shipped");
        }
    }
    else
    {
        SDS_LOG_INFO("config: loaded %ls", m_configPath.c_str());
    }
    Log::SetMinLevel(m_config.logLevel);
    m_config.LogSummary("loaded");

    if (!m_config.enabled)
    {
        SDS_LOG_WARN("Enabled=0 in the config: hooks will still register but nothing will be "
                     "sent to the pad.");
    }

    m_vibeDir = ResolveAssetDir(m_config.vibeDir, m_gameDir, m_modDir, "vibe");
    m_spkDir  = ResolveAssetDir(m_config.spkDir,  m_gameDir, m_modDir, "spk");

    m_triggers.Start(m_pad, m_config);
    m_haptics.Start(m_pad, m_config, m_vibeDir);
    m_speaker.Start(m_config, m_spkDir);

    m_padThreadRunning.store(true, std::memory_order_release);
    m_padThread = std::thread(&Runtime::PadThreadMain, this);

    m_lastStatusMs = NowMs();
    m_lastReloadMs = m_lastStatusMs;
    m_started.store(true, std::memory_order_release);
}

void Runtime::Shutdown()
{
    if (!m_started.exchange(false, std::memory_order_acq_rel))
        return;
    SDS_LOG_INFO("StrayDualSense shutting down");

    m_padThreadRunning.store(false, std::memory_order_release);
    if (m_padThread.joinable())
        m_padThread.join();

    m_haptics.Shutdown();
    m_speaker.Shutdown();
    m_triggers.Shutdown();   // last: it releases the triggers on the way out

    LogStatus();
    Log::Close();
}

void Runtime::PadThreadMain()
{
    // libScePad is DELAY-loaded (import #9), so at mod init it is usually not mapped yet.
    // Poll until it is; log the wait so "nothing happened" is never mysterious.
    int  waited = 0;
    bool announced = false;
    while (m_padThreadRunning.load(std::memory_order_acquire))
    {
        if (!m_pad.IsBound())
        {
            if (m_pad.Bind())
            {
                m_pad.SelectPad(m_config.padUserId);
            }
            else
            {
                waited += 500;
                if (!announced && waited >= 30000)
                {
                    announced = true;
                    SDS_LOG_WARN("libScePad.dll has not been loaded by the game after 30 s. It "
                                 "is a DELAY-LOAD import, so it only maps when the game first "
                                 "touches the pad API - if the DualSense is not visible to the "
                                 "prefix as a HID device that may never happen. Check that "
                                 "Steam's *global* PlayStation Controller Support is OFF.");
                }
                ::Sleep(500);
                continue;
            }
        }
        else if (m_config.padPollSeconds > 0.0f)
        {
            m_pad.RefreshIfLost();
        }

        const DWORD sleepMs = static_cast<DWORD>(
            std::max(0.25f, m_config.padPollSeconds) * 1000.0f);
        for (DWORD slept = 0; slept < sleepMs && m_padThreadRunning.load(std::memory_order_acquire);
             slept += 100)
            ::Sleep(100);
    }
    SDS_LOG_INFO("pad watcher exiting");
}

void Runtime::Tick()
{
    if (!m_started.load(std::memory_order_acquire))
        return;
    const uint64_t now = NowMs();

    if (m_config.configReloadSeconds > 0.0f &&
        now - m_lastReloadMs >= static_cast<uint64_t>(m_config.configReloadSeconds * 1000.0f))
    {
        m_lastReloadMs = now;
        m_config.ReloadIfChanged(m_configPath);
    }

    if (m_config.statusSeconds > 0.0f &&
        now - m_lastStatusMs >= static_cast<uint64_t>(m_config.statusSeconds * 1000.0f))
    {
        m_lastStatusMs = now;
        LogStatus();
    }
}

void Runtime::LogStatus()
{
    SDS_LOG_INFO("STATUS pad=%s(user=%d handle=0x%X) trig[events=%lu transmits=%lu ok=%lu "
                 "fail=%lu L=%d R=%d] vibe[starts=%lu played=%lu done=%lu missing=%lu "
                 "capped=%lu now='%s'] spk[starts=%lu missing=%lu fail=%lu endpoint=%d]",
                 m_pad.HasPad() ? "yes" : "NO", m_pad.UserId(),
                 static_cast<unsigned>(m_pad.Handle()),
                 m_triggerEvents.load(), m_triggers.Transmits(), m_pad.TriggerOk(),
                 m_pad.TriggerFail(), m_triggers.Left() ? 1 : 0, m_triggers.Right() ? 1 : 0,
                 m_vibrationStarts.load(), m_haptics.Started(), m_haptics.Finished(),
                 m_haptics.Missing(), m_haptics.Capped(), m_haptics.CurrentName().c_str(),
                 m_speakerStarts.load(), m_speaker.Missing(), m_speaker.Failures(),
                 m_speaker.EndpointFound() ? 1 : 0);
}

// ---- game intent ----------------------------------------------------------------------

void Runtime::OnTriggerActivated(bool state, int side)
{
    m_triggerEvents.fetch_add(1, std::memory_order_relaxed);
    if (side != static_cast<int>(TriggerSide::Left) && side != static_cast<int>(TriggerSide::Right))
    {
        // EPS5TriggersSide is 0/1. Anything else means the parameter was read wrongly, which
        // is exactly the class of mistake that silently hardens one trigger forever.
        SDS_LOG_ERROR("SetPS5TriggerActivated: side=%d is not 0(Left) or 1(Right). The "
                      "parameter read is WRONG; ignoring.", side);
        return;
    }
    SDS_LOG_DEBUG("SetPS5TriggerActivated state=%d side=%d", state ? 1 : 0, side);
    if (!m_config.enabled || !m_config.triggers)
        return;
    m_triggers.SetSide(static_cast<TriggerSide>(side), state);
}

void Runtime::OnUseStarted()
{
    SDS_LOG_DEBUG("_OnUseStarted");
}

void Runtime::OnAfterUseDone()
{
    SDS_LOG_DEBUG("_OnAfterUseDone -> release both triggers");
    m_triggers.ReleaseAll();
}

void Runtime::OnStartVibration(const std::string& assetFullName, float level, bool levelSeen)
{
    m_vibrationStarts.fetch_add(1, std::memory_order_relaxed);
    const std::string name = ShortAssetName(assetFullName);
    // An absent Level and a Level of 0 need different fixes, so they are distinguished
    // rather than both collapsing to a fallback (the old Lua mod's `lvSeen`).
    const float useLevel = levelSeen ? level : 1.0f;
    SDS_LOG_INFO("StartPS5Vibration asset='%s' -> '%s' level=%.3f (seen=%d)",
                 assetFullName.c_str(), name.c_str(), static_cast<double>(useLevel),
                 levelSeen ? 1 : 0);
    if (!m_config.enabled || !m_config.haptics)
        return;
    if (name.empty())
    {
        SDS_LOG_ERROR("StartPS5Vibration: could not derive an asset name from '%s'",
                      assetFullName.c_str());
        return;
    }
    m_haptics.Play(name, useLevel, m_config.hapticLoop);
}

void Runtime::OnStopVibration()
{
    SDS_LOG_INFO("StopPS5Vibration");
    m_haptics.Stop();
}

void Runtime::OnSetVibrationLevel(float level)
{
    // ~60 Hz. Never log it per call.
    m_haptics.SetLevel(level);
}

void Runtime::OnStartControllerSound(const std::string& assetFullName, float level, bool levelSeen)
{
    m_speakerStarts.fetch_add(1, std::memory_order_relaxed);
    const std::string name = ShortAssetName(assetFullName);
    const float useLevel = levelSeen ? level : 1.0f;
    SDS_LOG_INFO("StartPS5ControllerSound asset='%s' -> '%s' level=%.3f (seen=%d)",
                 assetFullName.c_str(), name.c_str(), static_cast<double>(useLevel),
                 levelSeen ? 1 : 0);
    if (!m_config.enabled || !m_config.speaker)
        return;
    if (name.empty())
    {
        SDS_LOG_ERROR("StartPS5ControllerSound: could not derive an asset name from '%s'",
                      assetFullName.c_str());
        return;
    }
    m_speaker.Play(name, useLevel, m_config.speakerLoop);
}

void Runtime::OnStopControllerSound()
{
    SDS_LOG_INFO("StopPS5ControllerSound");
    m_speaker.Stop();
}

void Runtime::OnSetControllerSoundLevel(float level)
{
    m_speaker.SetLevel(level);
}

void Runtime::OnPadVibrationEnabled(bool enabled)
{
    m_haptics.SetPadVibrationEnabled(enabled);
}

void Runtime::NoteHookRegistered(const char* name)
{
    SDS_LOG_INFO("hook registered: %s", name);
}

void Runtime::NoteHookMissing(const char* name)
{
    // A hook that never registers is the single most likely reason for "nothing happens", and
    // it is otherwise indistinguishable from "the game never called it".
    SDS_LOG_ERROR("hook NEVER REGISTERED: %s - that path of the mod is dead this session", name);
}

} // namespace sds
