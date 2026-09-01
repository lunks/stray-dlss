#include "Runtime.hpp"

#include "AssetName.hpp"
#include "Log.hpp"
#include "Platform.hpp"
#include "Version.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <vector>

namespace sds {
namespace {

// Resolve a configured relative directory against the game's Binaries/Win64 first (where the
// assets are generated) and the mod's own directory second. Never hardcoded; always logged,
// because "the asset was not found" and "we looked in the wrong place" are the same symptom
// otherwise.
std::wstring ResolveDir(const std::string& configured, const std::wstring& gameDir,
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
    const std::wstring b = modDir + rel + L"\\";
    const bool aOk = DirectoryExists(a);
    const bool bOk = DirectoryExists(b);
    SDS_LOG_INFO("assets: %s candidates: [game] %ls exists=%d | [mod] %ls exists=%d",
                 what, a.c_str(), aOk ? 1 : 0, b.c_str(), bOk ? 1 : 0);
    if (aOk) return a;
    if (bOk) return b;
    SDS_LOG_ERROR("assets: neither candidate for '%s' exists. Generate them with "
                  "tools/dualsense/extract_assets.sh and wavegen.sh.", what);
    return a;   // keep the game-dir path so the per-asset error names the expected location
}

} // namespace

Runtime& Rt()
{
    static Runtime instance;
    return instance;
}

bool Runtime::LoadLoopList(LoopList& list, const std::string& fileName, const char* what)
{
    // Game dir first, mod dir second — the same order as the asset directories.
    for (const std::wstring& dir : { m_gameDir, m_modDir })
    {
        if (dir.empty()) continue;
        const std::wstring   path = dir + Widen(fileName);
        std::vector<uint8_t> bytes;
        if (!ReadWholeFile(path, bytes))
            continue;
        list.Parse(std::string(bytes.begin(), bytes.end()));
        SDS_LOG_INFO("loops: %s -> %ls (%zu looping asset(s))", what, path.c_str(), list.Count());
        return true;
    }
    return false;
}

void Runtime::LoadLoopLists()
{
    if (!LoadLoopList(m_hapticLoops, m_config.hapticLoopsFile, "haptic"))
    {
        // Loud: without the list every asset is a one-shot, so the purr and the rain stop after
        // one pass. That is the safe direction (a bump cannot buzz forever) but it is wrong.
        SDS_LOG_ERROR("loops: haptic '%s' not found in the game dir or the mod dir. EVERY haptic "
                      "asset will play as a ONE-SHOT; loops (purr, rain, scratch) will end after "
                      "one pass. Generate it with tools/dualsense/extract_assets.sh + wavegen.sh.",
                      m_config.hapticLoopsFile.c_str());
    }
    if (!LoadLoopList(m_spkLoops, m_config.spkLoopsFile, "spk"))
    {
        // extract_assets.sh writes ONE list covering every controller-class SoundWave, both
        // families; wavegen.sh splits it. Accept the combined list rather than silencing the
        // purr's loop over a missing split file.
        if (LoadLoopList(m_spkLoops, m_config.hapticLoopsFile, "spk (from the combined haptic list)"))
            SDS_LOG_INFO("loops: '%s' absent; the speaker is using '%s', which lists every "
                         "controller-class asset", m_config.spkLoopsFile.c_str(),
                         m_config.hapticLoopsFile.c_str());
        else
            SDS_LOG_ERROR("loops: spk '%s' not found (nor '%s'). EVERY speaker asset will play "
                          "as a ONE-SHOT; the purr will end after one pass.",
                          m_config.spkLoopsFile.c_str(), m_config.hapticLoopsFile.c_str());
    }
}

void Runtime::Startup(const void* addressInsideThisModule)
{
    if (m_started.load(std::memory_order_acquire))
        return;

    m_gameDir = GameBinariesDir();
    m_modDir  = ModuleDir(addressInsideThisModule);

    // The log goes next to the game binaries, alongside stray-dlss.log, because that is the
    // directory the user already collects from.
    const std::wstring logDir = m_gameDir.empty() ? m_modDir : m_gameDir;
    Log::Open(logDir + L"stray-dualsense.log", LogLevel::Info);

    SDS_LOG_INFO("StrayDualSense %s starting", SDS_VERSION_STRING);
    SDS_LOG_INFO("  game binaries dir: %ls", m_gameDir.c_str());
    SDS_LOG_INFO("  mod dir          : %ls", m_modDir.c_str());

    m_configPath = m_modDir + L"StrayDualSense.ini";
    if (!m_config.Load(m_configPath))
    {
        const std::wstring alt = logDir + L"StrayDualSense.ini";
        if (m_config.Load(alt))
            m_configPath = alt;
        else
            SDS_LOG_INFO("config: no StrayDualSense.ini next to the mod DLL or in the game "
                         "directory; using built-in defaults");
    }
    if (!m_configPath.empty())
        SDS_LOG_INFO("config: %ls", m_configPath.c_str());
    Log::SetMinLevel(m_config.logLevel);
    m_config.LogSummary("loaded");

    if (!m_config.enabled)
        SDS_LOG_WARN("Enabled=0: hooks will still register but nothing reaches the pad.");

    m_hapticDir = ResolveDir(m_config.hapticDir, m_gameDir, m_modDir, "haptic");
    m_spkDir    = ResolveDir(m_config.spkDir,    m_gameDir, m_modDir, "spk");
    LoadLoopLists();

    m_hidMode.Start(m_config);
    m_triggers.Start(m_pad, m_config);
    // The coil path re-asserts waveform mode right before every waveform: libScePad's own
    // trigger reports carry the same flag byte and may have flipped it back since the last
    // periodic write (§12).
    m_haptics.Start(kCoilRoute, m_config.endpointMatch, m_hapticDir,
                    [this] { m_hidMode.AssertNow("before waveform"); });
    m_speaker.Start(kSpeakerRoute, m_config.endpointMatch, m_spkDir, nullptr);

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
    m_triggers.Shutdown();   // releases the triggers on the way out
    m_hidMode.Shutdown();    // hands the coils back to rumble emulation

    LogStatus();
    Log::Close();
}

void Runtime::PadThreadMain()
{
    // libScePad is DELAY-loaded (import #9), so at mod init it is usually not mapped yet.
    // Poll until it is; log the wait so "nothing happened" is never mysterious.
    int  waited    = 0;
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

        const DWORD sleepMs = static_cast<DWORD>(std::max(0.25f, m_config.padPollSeconds) * 1000.0f);
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
    const TriggerEffect fx = m_triggers.Effect();
    SDS_LOG_INFO("STATUS pad=%s(user=%d handle=0x%X) hid[open=%d writes=%lu fail=%lu] "
                 "trig[events=%lu tx=%lu ok=%lu fail=%lu L=%d R=%d effect=%d/%u/%u/%u %s] "
                 "hap[starts=%lu played=%lu done=%lu missing=%lu fail=%lu endpoint=%d now='%s' "
                 "compStops ok=%lu ignored=%lu padVibe=%d] "
                 "spk[starts=%lu played=%lu missing=%lu fail=%lu endpoint=%d now='%s']",
                 m_pad.HasPad() ? "yes" : "NO", m_pad.UserId(), static_cast<unsigned>(m_pad.Handle()),
                 m_hidMode.Opened() ? 1 : 0, m_hidMode.Writes(), m_hidMode.Failures(),
                 m_triggerEvents.load(), m_triggers.Transmits(), m_pad.TriggerOk(),
                 m_pad.TriggerFail(), m_triggers.Left() ? 1 : 0, m_triggers.Right() ? 1 : 0,
                 fx.mode, fx.value1, fx.value2, fx.value3,
                 m_effectFromGame.load() ? "(game)" : "(FALLBACK)",
                 m_vibrationStarts.load(), m_haptics.Started(), m_haptics.Finished(),
                 m_haptics.Missing(), m_haptics.Failures(), m_haptics.EndpointFound() ? 1 : 0,
                 m_haptics.CurrentName().c_str(), m_componentStopsHonoured.load(),
                 m_componentStopsIgnored.load(), m_padVibrationEnabled.load() ? 1 : 0,
                 m_speakerStarts.load(), m_speaker.Started(), m_speaker.Missing(),
                 m_speaker.Failures(), m_speaker.EndpointFound() ? 1 : 0,
                 m_speaker.CurrentName().c_str());
}

// ---- game intent ----------------------------------------------------------------------

void Runtime::OnTriggerEffectRead(const TriggerEffect& effect, bool ok)
{
    if (!ok)
    {
        if (!m_effectFromGame.load())
            SDS_LOG_WARN("trigger effect: m_scratchablePS5TriggerEffect could not be read; "
                         "using the FALLBACK game-space effect %d(%s) v=%u/%u/%u",
                         kFallbackTriggerEffect.mode, GameModeName(kFallbackTriggerEffect.mode),
                         kFallbackTriggerEffect.value1, kFallbackTriggerEffect.value2,
                         kFallbackTriggerEffect.value3);
        return;
    }
    const TriggerEffect previous = m_triggers.Effect();
    if (!m_effectFromGame.exchange(true) || previous != effect)
    {
        SDS_LOG_INFO("trigger effect (authored by the game): mode=%d(%s) v1=%u v2=%u v3=%u -> "
                     "Sony %s%s", effect.mode, GameModeName(effect.mode), effect.value1,
                     effect.value2, effect.value3, SonyModeName(ToSonyMode(effect.mode)),
                     IsKnownGameMode(effect.mode) ? "" : "  [UNKNOWN game mode, mapped to Feedback]");
    }
    m_triggers.SetEffect(effect);
}

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
    SDS_LOG_INFO("SetPS5TriggerActivated state=%d side=%d(%s)", state ? 1 : 0, side,
                 side == 0 ? "Left" : "Right");
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

void Runtime::OnStartVibration(const VibrationStart& s)
{
    m_vibrationStarts.fetch_add(1, std::memory_order_relaxed);
    const std::string name         = ShortAssetName(s.soundFullName);
    const bool        viaComponent = !s.componentFullName.empty();

    // An absent Level and a Level of 0 need different fixes, so they are distinguished. And
    // component-attached vibrations arrive with Level=0.0: their level lives in the submix
    // send (PS5VibrationAttenuation: bAttenuate=False, send constant 1.0), so 0 on that path
    // means 1.0.
    float level = s.levelSeen ? s.level : 1.0f;
    if (viaComponent && level <= 0.0f)
        level = 1.0f;

    const bool loop = m_hapticLoops.Contains(name);   // the ASSET decides, never the caller
    SDS_LOG_INFO("StartPS5Vibration%s '%s' -> %s level=%.3f(seen=%d) fadeIn=%.2f loop=%d(asset)%s%s",
                 viaComponent ? "OnAudioComponent" : "", s.soundFullName.c_str(), name.c_str(),
                 static_cast<double>(level), s.levelSeen ? 1 : 0, static_cast<double>(s.fadeIn),
                 loop ? 1 : 0, viaComponent ? " component=" : "",
                 viaComponent ? s.componentFullName.c_str() : "");

    {
        std::lock_guard<std::mutex> lock(m_componentMutex);
        m_playingComponent = s.componentFullName;   // empty on the plain path
    }

    if (!m_config.enabled || !m_config.haptics)
        return;
    if (name.empty())
    {
        SDS_LOG_ERROR("StartPS5Vibration: could not derive an asset name from '%s'",
                      s.soundFullName.c_str());
        return;
    }
    if (!m_padVibrationEnabled.load(std::memory_order_relaxed))
    {
        SDS_LOG_INFO("haptics: '%s' suppressed, PadVibrationEnabled is off", name.c_str());
        m_haptics.Stop(0.0f);
        return;
    }
    m_haptics.Play(name, level, s.fadeIn, loop);
}

void Runtime::OnStopVibration(float fadeOut)
{
    SDS_LOG_INFO("StopPS5Vibration fadeOut=%.2f (global)", static_cast<double>(fadeOut));
    {
        std::lock_guard<std::mutex> lock(m_componentMutex);
        m_playingComponent.clear();
    }
    m_haptics.Stop(fadeOut);
}

void Runtime::OnStopVibrationOnComponent(const std::string& componentFullName, float fadeOut)
{
    // ~700 calls a session, most of them housekeeping for components that are not playing.
    // Only the owner of the haptic in flight may stop it.
    bool owner = false;
    {
        std::lock_guard<std::mutex> lock(m_componentMutex);
        owner = !m_playingComponent.empty() && m_playingComponent == componentFullName;
        if (owner)
            m_playingComponent.clear();
    }
    if (!owner)
    {
        m_componentStopsIgnored.fetch_add(1, std::memory_order_relaxed);
        SDS_LOG_DEBUG("StopPS5VibrationOnAudioComponent ignored: '%s' is not the playing "
                      "component", componentFullName.c_str());
        return;
    }
    m_componentStopsHonoured.fetch_add(1, std::memory_order_relaxed);
    SDS_LOG_INFO("StopPS5VibrationOnAudioComponent fadeOut=%.2f (owner) %s",
                 static_cast<double>(fadeOut), componentFullName.c_str());
    m_haptics.Stop(fadeOut);
}

void Runtime::OnSetVibrationLevel(float level)
{
    // ~60 Hz. Never log it per call.
    m_haptics.SetLevel(level);
}

void Runtime::OnStartControllerSound(const std::string& soundFullName, float level,
                                     bool levelSeen, float fadeIn)
{
    m_speakerStarts.fetch_add(1, std::memory_order_relaxed);
    const std::string name     = ShortAssetName(soundFullName);
    const float       useLevel = levelSeen ? level : 1.0f;
    const bool        loop     = m_spkLoops.Contains(name);
    SDS_LOG_INFO("StartPS5ControllerSound '%s' -> %s level=%.3f(seen=%d) fadeIn=%.2f loop=%d(asset)",
                 soundFullName.c_str(), name.c_str(), static_cast<double>(useLevel),
                 levelSeen ? 1 : 0, static_cast<double>(fadeIn), loop ? 1 : 0);
    if (!m_config.enabled || !m_config.speaker)
        return;
    if (name.empty())
    {
        SDS_LOG_ERROR("StartPS5ControllerSound: could not derive an asset name from '%s'",
                      soundFullName.c_str());
        return;
    }
    m_speaker.Play(name, useLevel, fadeIn, loop);
}

void Runtime::OnStopControllerSound(float fadeOut)
{
    SDS_LOG_INFO("StopPS5ControllerSound fadeOut=%.2f", static_cast<double>(fadeOut));
    m_speaker.Stop(fadeOut);
}

void Runtime::OnSetControllerSoundLevel(float level)
{
    m_speaker.SetLevel(level);
}

void Runtime::OnPadVibrationEnabled(bool enabled)
{
    const bool was = m_padVibrationEnabled.exchange(enabled, std::memory_order_relaxed);
    if (was == enabled)
        return;
    SDS_LOG_INFO("haptics: PadVibrationEnabled -> %d", enabled ? 1 : 0);
    if (!enabled)
        m_haptics.Stop(0.0f);
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
