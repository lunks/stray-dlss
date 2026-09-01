#include "Log.hpp"

#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <mutex>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

namespace sds {
namespace {

std::mutex          g_mutex;
FILE*               g_file        = nullptr;
LogLevel            g_minLevel    = LogLevel::Info;
std::vector<std::string> g_mirror;
size_t              g_mirrorDropped = 0;

// The mirror only exists to reach UE4SS's log. If nobody is draining it the game is
// probably already gone; keeping the last N lines is more useful than keeping the first N.
constexpr size_t kMirrorCap = 512;

const char* LevelName(LogLevel level)
{
    switch (level)
    {
    case LogLevel::Debug: return "DEBUG";
    case LogLevel::Info:  return "INFO ";
    case LogLevel::Warn:  return "WARN ";
    case LogLevel::Error: return "ERROR";
    }
    return "?????";
}

// "12:34:56.789" — wall clock, because the user correlates this against UE4SS.log,
// ReShade's log and `dmesg` by timestamp.
void FormatTimestamp(char* out, size_t outSize)
{
#ifdef _WIN32
    SYSTEMTIME st{};
    GetLocalTime(&st);
    std::snprintf(out, outSize, "%02u:%02u:%02u.%03u",
                  st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
#else
    std::time_t     now = std::time(nullptr);
    const std::tm*  tm  = std::localtime(&now);
    if (tm != nullptr)
        std::snprintf(out, outSize, "%02d:%02d:%02d.000", tm->tm_hour, tm->tm_min, tm->tm_sec);
    else
        std::snprintf(out, outSize, "??:??:??.???");
#endif
}

} // namespace

void Log::Open(const std::wstring& path, LogLevel minLevel)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_minLevel = minLevel;

    if (g_file != nullptr)
    {
        std::fclose(g_file);
        g_file = nullptr;
    }
    if (path.empty())
        return;

#ifdef _WIN32
    // Truncate: one file per session. A previous session's lines matching a grep is a trap
    // this project has already been caught by (CLAUDE.md §5, "confirm the timestamps are
    // from the NEW session").
    if (_wfopen_s(&g_file, path.c_str(), L"w") != 0)
        g_file = nullptr;
#else
    (void)path;
#endif
}

void Log::Close()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file != nullptr)
    {
        std::fclose(g_file);
        g_file = nullptr;
    }
}

void Log::SetMinLevel(LogLevel level)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_minLevel = level;
}

LogLevel Log::MinLevel()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_minLevel;
}

void Log::Write(LogLevel level, const char* fmt, ...)
{
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (level < g_minLevel)
            return;
    }

    char body[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(body, sizeof(body), fmt, args);
    va_end(args);

    char stamp[32];
    FormatTimestamp(stamp, sizeof(stamp));

    char line[1200];
    std::snprintf(line, sizeof(line), "[%s] [%s] %s", stamp, LevelName(level), body);

    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file != nullptr)
    {
        std::fputs(line, g_file);
        std::fputc('\n', g_file);
        std::fflush(g_file);
    }

    if (g_mirror.size() >= kMirrorCap)
    {
        g_mirror.erase(g_mirror.begin());
        ++g_mirrorDropped;
    }
    g_mirror.emplace_back(line);
}

std::vector<std::string> Log::TakeMirrorLines()
{
    std::vector<std::string> out;
    std::lock_guard<std::mutex> lock(g_mutex);
    out.swap(g_mirror);
    if (g_mirrorDropped != 0)
    {
        char note[128];
        std::snprintf(note, sizeof(note),
                      "[log] %zu mirror lines dropped (nobody drained the mirror)",
                      g_mirrorDropped);
        out.emplace_back(note);
        g_mirrorDropped = 0;
    }
    return out;
}

} // namespace sds
