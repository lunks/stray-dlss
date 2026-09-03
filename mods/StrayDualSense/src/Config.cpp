#include "Config.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
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

Config::HapticSource ParseHapticSource(const std::string& v, Config::HapticSource fallback)
{
    const std::string l = Lower(Trim(v));
    if (l == "assets"  || l == "asset")   return Config::HapticSource::Assets;
    if (l == "measure" || l == "measure-only" || l == "probe") return Config::HapticSource::Measure;
    if (l == "submix")                    return Config::HapticSource::Submix;
    SDS_LOG_WARN("config: HapticSource='%s' is not one of assets|measure|submix; keeping the "
                 "previous value.", v.c_str());
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

float ParseFloat(const std::string& v, float fallback)
{
    char* end = nullptr;
    const double d = std::strtod(v.c_str(), &end);
    return (end == v.c_str()) ? fallback : static_cast<float>(d);
}

// Accepts "0x00", "00", "0", "252".
int ParseByte(const std::string& v, int fallback)
{
    char* end = nullptr;
    const long n = std::strtol(v.c_str(), &end, 0);
    if (end == v.c_str() || n < 0 || n > 255) return fallback;
    return static_cast<int>(n);
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

        if      (key == "enabled")                enabled                = ParseBool(val, enabled);
        else if (key == "loglevel")               logLevel               = ParseLevel(val, logLevel);
        else if (key == "paduserid")              padUserId              = std::clamp(std::atoi(val.c_str()), 0, 4);
        else if (key == "padpollseconds")         padPollSeconds         = ParseFloat(val, padPollSeconds);
        else if (key == "triggers")               triggers               = ParseBool(val, triggers);
        else if (key == "triggerreadback")        triggerReadback        = ParseBool(val, triggerReadback);
        else if (key == "haptics")                haptics                = ParseBool(val, haptics);
        else if (key == "hapticvalidflag0")       hapticValidFlag0       = ParseByte(val, hapticValidFlag0);
        else if (key == "hapticreassertseconds")  hapticReassertSeconds  = ParseFloat(val, hapticReassertSeconds);
        else if (key == "hapticsource")           hapticSource           = ParseHapticSource(val, hapticSource);
        else if (key == "submixpath")             submixPath             = val;
        else if (key == "submixprobemaster")      submixProbeMaster      = ParseBool(val, submixProbeMaster);
        else if (key == "submixregisterslot")     submixRegisterSlot     = std::clamp(std::atoi(val.c_str()), 0, 31);
        else if (key == "submixdevicesource")     submixDeviceSource     = Lower(val);
        else if (key == "submixscanbytes")        submixScanBytes        = std::clamp(std::atoi(val.c_str()), 0x800, 0x80000);
        else if (key == "submixdumpwords")        submixDumpWords        = std::clamp(std::atoi(val.c_str()), 0, 4096);
        else if (key == "submixgain")             submixGain             = std::clamp(ParseFloat(val, submixGain), 0.0f, 8.0f);
        else if (key == "submixqueueaheadms")     submixQueueAheadMs     = std::clamp(std::atoi(val.c_str()), 5, 500);
        else if (key == "submixringms")           submixRingMs           = std::clamp(std::atoi(val.c_str()), 20, 2000);
        else if (key == "submixstatusseconds")    submixStatusSeconds    = ParseFloat(val, submixStatusSeconds);
        else if (key == "submixstatusfile")       submixStatusFile       = val;
        else if (key == "speaker")                speaker                = ParseBool(val, speaker);
        else if (key == "endpointmatch")          endpointMatch          = val;
        else if (key == "hapticdir")              hapticDir              = val;
        else if (key == "spkdir")                 spkDir                 = val;
        else if (key == "hapticloopsfile")        hapticLoopsFile        = val;
        else if (key == "spkloopsfile")           spkLoopsFile           = val;
        else if (key == "hookretryseconds")       hookRetrySeconds       = ParseFloat(val, hookRetrySeconds);
        else if (key == "statusseconds")          statusSeconds          = ParseFloat(val, statusSeconds);
        else if (key == "configreloadseconds")    configReloadSeconds    = ParseFloat(val, configReloadSeconds);
        else SDS_LOG_WARN("config: ignoring unknown key '%s' (the envelope-era keys HapticGain, "
                          "HapticLoop, SpeakerBoost, VibeDir, TriggerPosition/Strength no longer "
                          "exist)", key.c_str());
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

    Config fresh;
    fresh.m_lastWriteTime = m_lastWriteTime;
    if (!fresh.Load(path))
        return false;

    if (fresh.padUserId != padUserId || fresh.endpointMatch != endpointMatch ||
        fresh.hapticDir != hapticDir || fresh.spkDir != spkDir ||
        fresh.hapticLoopsFile != hapticLoopsFile || fresh.spkLoopsFile != spkLoopsFile)
    {
        SDS_LOG_WARN("config: PadUserId / EndpointMatch / HapticDir / SpkDir / *LoopsFile "
                     "changed but are NOT hot-reloadable. Relaunch the game for those.");
    }
    // The submix tap binds ONCE, on the game thread, into a listener the engine then holds
    // for the session. Re-reading its shape at runtime would mean a second registration
    // against a live audio render thread, which is exactly the kind of half-applied change
    // this function exists to refuse.
    if (fresh.hapticSource != hapticSource || fresh.submixPath != submixPath ||
        fresh.submixRegisterSlot != submixRegisterSlot ||
        fresh.submixDeviceSource != submixDeviceSource ||
        fresh.submixProbeMaster != submixProbeMaster)
    {
        SDS_LOG_WARN("config: HapticSource / SubmixPath / SubmixRegisterSlot / "
                     "SubmixDeviceSource / SubmixProbeMaster changed but are NOT "
                     "hot-reloadable - the listener is registered once and the engine keeps "
                     "it. Relaunch the game.");
    }

    // Scalars only: the strings above are read by running threads without a lock.
    enabled               = fresh.enabled;
    logLevel              = fresh.logLevel;
    padPollSeconds        = fresh.padPollSeconds;
    triggers              = fresh.triggers;
    triggerReadback       = fresh.triggerReadback;
    haptics               = fresh.haptics;
    hapticValidFlag0      = fresh.hapticValidFlag0;
    hapticReassertSeconds = fresh.hapticReassertSeconds;
    speaker               = fresh.speaker;
    submixGain            = fresh.submixGain;      // live: it is one atomic on the sink
    submixStatusSeconds   = fresh.submixStatusSeconds;
    statusSeconds         = fresh.statusSeconds;
    configReloadSeconds   = fresh.configReloadSeconds;
    m_lastWriteTime       = fresh.m_lastWriteTime;

    Log::SetMinLevel(logLevel);
    LogSummary("reloaded");
    return true;
}

void Config::LogSummary(const char* what) const
{
    SDS_LOG_INFO("config %s: enabled=%d triggers=%d(readback=%d) haptics=%d(validFlag0=0x%02X "
                 "reassert=%.1fs) speaker=%d endpoint='%s' padUserId=%d hapticDir='%s' "
                 "spkDir='%s' loops='%s'/'%s'",
                 what, enabled ? 1 : 0, triggers ? 1 : 0, triggerReadback ? 1 : 0,
                 haptics ? 1 : 0, static_cast<unsigned>(hapticValidFlag0),
                 static_cast<double>(hapticReassertSeconds), speaker ? 1 : 0,
                 endpointMatch.c_str(), padUserId, hapticDir.c_str(), spkDir.c_str(),
                 hapticLoopsFile.c_str(), spkLoopsFile.c_str());
    SDS_LOG_INFO("config %s: HapticSource=%s submixPath='%s' probeMaster=%d slot=%d "
                 "deviceSource=%s gain=%.3f queueAhead=%dms ring=%dms scan=0x%X dump=%d",
                 what, HapticSourceName(), submixPath.c_str(), submixProbeMaster ? 1 : 0,
                 submixRegisterSlot, submixDeviceSource.c_str(),
                 static_cast<double>(submixGain), submixQueueAheadMs, submixRingMs,
                 static_cast<unsigned>(submixScanBytes), submixDumpWords);
}

const char* Config::HapticSourceName() const
{
    switch (hapticSource)
    {
    case HapticSource::Measure: return "measure";
    case HapticSource::Submix:  return "submix";
    case HapticSource::Assets:
    default:                    return "assets";
    }
}

} // namespace sds
