#include "Config.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace sds {
namespace {

std::string Trim(const std::string& s)
{
    const size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    const size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::string Lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(::tolower(c)); });
    return s;
}

bool ParseBool(const std::string& v, bool fallback)
{
    const std::string l = Lower(Trim(v));
    if (l == "1" || l == "true"  || l == "yes" || l == "on")  return true;
    if (l == "0" || l == "false" || l == "no"  || l == "off") return false;
    return fallback;
}

LogLevel ParseLevel(const std::string& v, LogLevel fallback)
{
    const std::string l = Lower(Trim(v));
    if (l == "debug") return LogLevel::Debug;
    if (l == "info")  return LogLevel::Info;
    if (l == "warn")  return LogLevel::Warn;
    if (l == "error") return LogLevel::Error;
    return fallback;
}

uint64_t FileWriteTime(const std::wstring& path)
{
    WIN32_FILE_ATTRIBUTE_DATA d{};
    if (!::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &d))
        return 0;
    ULARGE_INTEGER u;
    u.LowPart  = d.ftLastWriteTime.dwLowDateTime;
    u.HighPart = d.ftLastWriteTime.dwHighDateTime;
    return u.QuadPart;
}

} // namespace

bool Config::Load(const std::wstring& path)
{
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"r") != 0 || f == nullptr)
        return false;

    char line[512];
    while (std::fgets(line, sizeof(line), f) != nullptr)
    {
        std::string s = Trim(line);
        if (s.empty() || s[0] == '#' || s[0] == ';' || s[0] == '[')
            continue;
        const size_t eq = s.find('=');
        if (eq == std::string::npos)
            continue;
        const std::string key = Lower(Trim(s.substr(0, eq)));
        const std::string val = Trim(s.substr(eq + 1));

        if      (key == "enabled")            enabled          = ParseBool(val, enabled);
        else if (key == "loglevel")           logLevel         = ParseLevel(val, logLevel);
        else if (key == "paduserid")          padUserId        = std::atoi(val.c_str());
        else if (key == "padpollseconds")     padPollSeconds   = static_cast<float>(std::atof(val.c_str()));
        else if (key == "triggers")           triggers         = ParseBool(val, triggers);
        else if (key == "triggerposition")    triggerPosition  = static_cast<uint8_t>(std::clamp(std::atoi(val.c_str()), 0, 9));
        else if (key == "triggerstrength")    triggerStrength  = static_cast<uint8_t>(std::clamp(std::atoi(val.c_str()), 0, 8));
        else if (key == "triggerreadback")    triggerReadback  = ParseBool(val, triggerReadback);
        else if (key == "haptics")            haptics          = ParseBool(val, haptics);
        else if (key == "hapticgain")         hapticGain       = std::clamp(std::atoi(val.c_str()), 0, 255);
        else if (key == "envelopestepms")     envelopeStepMs   = std::clamp(std::atoi(val.c_str()), 1, 100);
        else if (key == "maxenvelopesteps")   maxEnvelopeSteps = std::max(1, std::atoi(val.c_str()));
        else if (key == "hapticloop")         hapticLoop       = ParseBool(val, hapticLoop);
        else if (key == "speaker")            speaker          = ParseBool(val, speaker);
        else if (key == "speakerboost")       speakerBoost     = static_cast<float>(std::atof(val.c_str()));
        else if (key == "speakerdevicematch") speakerDeviceMatch = val;
        else if (key == "speakerassetrate")   speakerAssetRate = std::max(1, std::atoi(val.c_str()));
        else if (key == "speakerloop")        speakerLoop      = ParseBool(val, speakerLoop);
        else if (key == "vibedir")            vibeDir          = val;
        else if (key == "spkdir")             spkDir           = val;
        else if (key == "hookretryseconds")   hookRetrySeconds = static_cast<float>(std::atof(val.c_str()));
        else if (key == "statusseconds")      statusSeconds    = static_cast<float>(std::atof(val.c_str()));
        else if (key == "configreloadseconds") configReloadSeconds = static_cast<float>(std::atof(val.c_str()));
        else SDS_LOG_WARN("config: ignoring unknown key '%s'", key.c_str());
    }
    std::fclose(f);
    m_lastWriteTime = FileWriteTime(path);
    return true;
}

bool Config::ReloadIfChanged(const std::wstring& path)
{
    if (configReloadSeconds <= 0.0f)
        return false;
    const uint64_t t = FileWriteTime(path);
    if (t == 0 || t == m_lastWriteTime)
        return false;

    // Only the live fields are taken. Pad selection and the device match are read once at
    // startup by threads that are already running; silently "reloading" them would leave the
    // log claiming one thing and the runtime doing another.
    Config fresh;
    fresh.m_lastWriteTime = m_lastWriteTime;
    if (!fresh.Load(path))
        return false;

    if (fresh.padUserId != padUserId || fresh.speakerDeviceMatch != speakerDeviceMatch ||
        fresh.vibeDir != vibeDir || fresh.spkDir != spkDir)
    {
        SDS_LOG_WARN("config: PadUserId / SpeakerDeviceMatch / VibeDir / SpkDir changed but "
                     "are NOT hot-reloadable. Relaunch the game for those to take effect.");
    }

    enabled          = fresh.enabled;
    logLevel         = fresh.logLevel;
    triggers         = fresh.triggers;
    triggerPosition  = fresh.triggerPosition;
    triggerStrength  = fresh.triggerStrength;
    triggerReadback  = fresh.triggerReadback;
    haptics          = fresh.haptics;
    hapticGain       = fresh.hapticGain;
    hapticLoop       = fresh.hapticLoop;
    speaker          = fresh.speaker;
    speakerBoost     = fresh.speakerBoost;
    speakerLoop      = fresh.speakerLoop;
    statusSeconds    = fresh.statusSeconds;
    configReloadSeconds = fresh.configReloadSeconds;
    m_lastWriteTime  = fresh.m_lastWriteTime;

    Log::SetMinLevel(logLevel);
    LogSummary("reloaded");
    return true;
}

void Config::LogSummary(const char* what) const
{
    SDS_LOG_INFO("config %s: enabled=%d triggers=%d(pos=%u str=%u readback=%d) "
                 "haptics=%d(gain=%d step=%dms cap=%d loop=%d) speaker=%d(boost=%.4f loop=%d match='%s') "
                 "padUserId=%d vibeDir='%s' spkDir='%s'",
                 what, enabled ? 1 : 0, triggers ? 1 : 0, triggerPosition, triggerStrength,
                 triggerReadback ? 1 : 0, haptics ? 1 : 0, hapticGain, envelopeStepMs,
                 maxEnvelopeSteps, hapticLoop ? 1 : 0, speaker ? 1 : 0,
                 static_cast<double>(speakerBoost), speakerLoop ? 1 : 0,
                 speakerDeviceMatch.c_str(), padUserId, vibeDir.c_str(), spkDir.c_str());
}

} // namespace sds
