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

// ps5 | ps4 | xbox | game. The numbers are EGameControllerType, read from the object dump.
int ParseGlyphs(const std::string& v, int fallback)
{
    const std::string l = Lower(Trim(v));
    if (l == "ps5" || l == "playstation" || l == "3") return 3;
    if (l == "ps4" || l == "2")                       return 2;
    if (l == "xbox" || l == "1")                      return 1;
    if (l == "game" || l == "off" || l == "none" || l == "-1" || l == "0") return -1;
    SDS_LOG_WARN("config: Glyphs='%s' is not one of ps5|ps4|xbox|game; keeping the previous "
                 "value.", v.c_str());
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

// Keys that existed before 0.4.0 and mean nothing now. Named rather than "unknown" so an ini
// left over from the spike days reads as "this was retired" and not as a typo — and so a
// value that used to CHANGE behaviour (HapticSource=assets, PadSpeakerRoute=hid) is seen to
// be doing nothing rather than silently doing something else.
struct RetiredKey { const char* key; const char* why; };
const RetiredKey kRetired[] = {
    { "hapticsource",        "there is one source, the engine's own submix through the tap; no asset path, no fallback" },
    { "submixreroute",       "the reroute is always on - without it there is nothing to tap" },
    { "submixreroutemaster", "the vibration master is SubmixPath" },
    { "forceps5hapticpath",  "the DebugPS5Haptic gate is always opened from the Start hooks" },
    { "padspeakerroute",     "the speaker route is Sony's scePadSetAudioOutPath and nothing else" },
    { "padspeakerhidpath",   "the raw HID audio claim is gone" },
    { "padspeakerhidvolume", "the raw HID audio claim is gone" },
    { "padspeakerhidpreamp", "the raw HID audio claim is gone" },
    { "hapticdir",           "no extracted assets are played" },
    { "spkdir",              "no extracted assets are played" },
    { "hapticloopsfile",     "the engine loops what the asset says; no loop list is read" },
    { "spkloopsfile",        "the engine loops what the asset says; no loop list is read" },
};

const char* RetiredWhy(const std::string& key)
{
    for (const RetiredKey& r : kRetired)
        if (key == r.key)
            return r.why;
    return nullptr;
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
        else if (key == "speaker")                speaker                = ParseBool(val, speaker);
        else if (key == "speakergain")            speakerGain            = std::clamp(ParseFloat(val, speakerGain), 0.0f, 8.0f);
        else if (key == "padspeakerpath")         padSpeakerPath         = std::clamp(std::atoi(val.c_str()), 0, 4);
        else if (key == "padspeakergain")         padSpeakerGain         = std::clamp(std::atoi(val.c_str()), 0, 255);
        else if (key == "padspeakerreassertseconds") padSpeakerReassertSeconds = std::clamp(ParseFloat(val, padSpeakerReassertSeconds), 0.0f, 600.0f);
        else if (key == "submixpath")             submixPath             = val;
        else if (key == "submixspeakerpath")      submixSpeakerPath      = val;
        else if (key == "submixrerouteparent")    submixRerouteParent    = val;
        else if (key == "submixregistersoundsubmixslot") submixRegisterSoundSubmixSlot = std::clamp(std::atoi(val.c_str()), 0, 31);
        else if (key == "submixregisterslot")     submixRegisterSlot     = std::clamp(std::atoi(val.c_str()), 0, 31);
        else if (key == "submixdevicesource")     submixDeviceSource     = Lower(val);
        else if (key == "submixscanbytes")        submixScanBytes        = std::clamp(std::atoi(val.c_str()), 0x800, 0x80000);
        else if (key == "submixdumpwords")        submixDumpWords        = std::clamp(std::atoi(val.c_str()), 0, 4096);
        else if (key == "submixprobemaster")      submixProbeMaster      = ParseBool(val, submixProbeMaster);
        else if (key == "submixgain")             submixGain             = std::clamp(ParseFloat(val, submixGain), 0.0f, 8.0f);
        else if (key == "submixqueueaheadms")     submixQueueAheadMs     = std::clamp(std::atoi(val.c_str()), 5, 500);
        else if (key == "submixringms")           submixRingMs           = std::clamp(std::atoi(val.c_str()), 20, 2000);
        else if (key == "submixstatusseconds")    submixStatusSeconds    = ParseFloat(val, submixStatusSeconds);
        else if (key == "submixwatchseconds")     submixWatchSeconds     = ParseFloat(val, submixWatchSeconds);
        else if (key == "submixlivethreshold")    submixLiveThreshold    = std::clamp(ParseFloat(val, submixLiveThreshold), 0.0f, 1.0f);
        else if (key == "submixwarnseconds")      submixWarnSeconds      = ParseFloat(val, submixWarnSeconds);
        else if (key == "submixreroutewatchdogseconds") submixRerouteWatchdogSeconds = std::clamp(ParseFloat(val, submixRerouteWatchdogSeconds), 0.0f, 600.0f);
        else if (key == "submixstatusfile")       submixStatusFile       = val;
        else if (key == "glyphs")                 glyphControllerType    = ParseGlyphs(val, glyphControllerType);
        else if (key == "endpointmatch")          endpointMatch          = val;
        else if (key == "hookretryseconds")       hookRetrySeconds       = ParseFloat(val, hookRetrySeconds);
        else if (key == "statusseconds")          statusSeconds          = ParseFloat(val, statusSeconds);
        else if (key == "configreloadseconds")    configReloadSeconds    = ParseFloat(val, configReloadSeconds);
        else if (const char* why = RetiredWhy(key))
            SDS_LOG_INFO("config: '%s' was retired in 0.4.0 (%s); ignored.", key.c_str(), why);
        else
            SDS_LOG_WARN("config: ignoring unknown key '%s'", key.c_str());
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

    if (fresh.padUserId != padUserId || fresh.endpointMatch != endpointMatch)
    {
        SDS_LOG_WARN("config: PadUserId / EndpointMatch changed but are NOT hot-reloadable. "
                     "Relaunch the game for those.");
    }
    // The taps bind ONCE, on the game thread, into listeners the engine then holds for the
    // session. Re-reading their shape at runtime would mean a second registration against a
    // live audio render thread, which is exactly the kind of half-applied change this
    // function exists to refuse.
    if (fresh.submixPath != submixPath || fresh.submixSpeakerPath != submixSpeakerPath ||
        fresh.submixRerouteParent != submixRerouteParent ||
        fresh.submixRegisterSlot != submixRegisterSlot ||
        fresh.submixRegisterSoundSubmixSlot != submixRegisterSoundSubmixSlot ||
        fresh.submixDeviceSource != submixDeviceSource ||
        fresh.submixProbeMaster != submixProbeMaster)
    {
        SDS_LOG_WARN("config: Submix*Path / SubmixRerouteParent / SubmixRegister*Slot / "
                     "SubmixDeviceSource / SubmixProbeMaster changed but are NOT hot-reloadable "
                     "- the listeners are registered once and the engine keeps them. Relaunch "
                     "the game.");
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
    speakerGain           = fresh.speakerGain;     // live: one atomic on the sink
    // LIVE, deliberately: Runtime re-applies the routing whenever these change.
    padSpeakerPath        = fresh.padSpeakerPath;
    padSpeakerGain        = fresh.padSpeakerGain;
    padSpeakerReassertSeconds = fresh.padSpeakerReassertSeconds;
    submixGain            = fresh.submixGain;      // live: one atomic on the sink
    submixStatusSeconds   = fresh.submixStatusSeconds;
    submixWatchSeconds    = fresh.submixWatchSeconds;
    submixLiveThreshold   = fresh.submixLiveThreshold;
    submixWarnSeconds     = fresh.submixWarnSeconds;
    submixRerouteWatchdogSeconds = fresh.submixRerouteWatchdogSeconds;
    glyphControllerType   = fresh.glyphControllerType;  // read on the game thread per call
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
                 "reassert=%.1fs) speaker=%d(gain=%.3f, sonyPath=%d(%s) sonyGain=%d "
                 "reassert=%.1fs) endpoint='%s' padUserId=%d Glyphs=%s(%d)",
                 what, enabled ? 1 : 0, triggers ? 1 : 0, triggerReadback ? 1 : 0,
                 haptics ? 1 : 0, static_cast<unsigned>(hapticValidFlag0),
                 static_cast<double>(hapticReassertSeconds), speaker ? 1 : 0,
                 static_cast<double>(speakerGain), padSpeakerPath,
                 SceAudioOutPathName(padSpeakerPath), padSpeakerGain,
                 static_cast<double>(padSpeakerReassertSeconds), endpointMatch.c_str(),
                 padUserId, GlyphName(), glyphControllerType);
    SDS_LOG_INFO("config %s: submix vibration='%s' speaker='%s' parent='%s' slots=%d/%d "
                 "deviceSource=%s scan=0x%X dump=%d probeMaster=%d gain=%.3f queueAhead=%dms "
                 "ring=%dms status=%.1fs watch=%.1fs liveThreshold=%.5f warnEvery=%.1fs "
                 "watchdog=%.1fs",
                 what, submixPath.c_str(), submixSpeakerPath.c_str(),
                 submixRerouteParent.c_str(), submixRegisterSlot, submixRegisterSoundSubmixSlot,
                 submixDeviceSource.c_str(), static_cast<unsigned>(submixScanBytes),
                 submixDumpWords, submixProbeMaster ? 1 : 0, static_cast<double>(submixGain),
                 submixQueueAheadMs, submixRingMs, static_cast<double>(submixStatusSeconds),
                 static_cast<double>(submixWatchSeconds),
                 static_cast<double>(submixLiveThreshold),
                 static_cast<double>(submixWarnSeconds),
                 static_cast<double>(submixRerouteWatchdogSeconds));
}

const char* Config::GlyphName() const
{
    switch (glyphControllerType)
    {
    case 3:  return "ps5";
    case 2:  return "ps4";
    case 1:  return "xbox";
    case -1: return "game";
    default: return "?";
    }
}

} // namespace sds
