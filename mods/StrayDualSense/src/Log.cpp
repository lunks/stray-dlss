#include "Log.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <thread>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

namespace sds {
namespace {

// THE FILE WRITE IS ASYNCHRONOUS (2026-09-05). It used to be fputs + fflush on the calling
// thread under the sink mutex. In StrayDLSS the user correlated the on-screen flicker with the
// log, line for line (the same fputs + fflush shape); this logger is called from six threads,
// two of them the trigger and haptic workers whose timing IS the product, so it gets the same
// writer: callers format and enqueue, one thread writes and flushes per batch. Kept from the old
// contract: complete lines in enqueue order, an ERROR line on disk before its caller continues
// (bounded wait for the drain), the in-memory mirror for UE4SS, and synchronous writes before
// Open and after Close. Close waits for the drain and DETACHES the thread rather than joining
// it, because the mod is torn down at process exit where the loader lock may be held.
struct Writer
{
    std::condition_variable  cv;
    std::condition_variable  drained;
    std::thread              thread;
    std::vector<std::string> queue;
    bool                     stop     = false;
    bool                     draining = false;
};

std::mutex               g_mutex;
FILE*                    g_file        = nullptr;
LogLevel                 g_minLevel    = LogLevel::Info;
std::vector<std::string> g_mirror;
size_t                   g_mirrorDropped = 0;
Writer*                  g_writer      = nullptr;
Log::Stats               g_stats       = {};
constexpr size_t         kMaxQueue     = 4096;
constexpr auto           kErrorDrainTimeout = std::chrono::milliseconds(200);

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

uint64_t NowNs()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

// Mutex NOT held: the batch was swapped out under it.
void WriteBatch(const std::vector<std::string>& batch, FILE* file)
{
    if (file == nullptr)
        return;
    for (const std::string& line : batch)
    {
        std::fputs(line.c_str(), file);
        std::fputc('\n', file);
    }
    std::fflush(file);
}

void WriterMain(Writer* w)
{
    std::vector<std::string> batch;
    for (;;)
    {
        FILE* file;
        {
            std::unique_lock<std::mutex> lock(g_mutex);
            w->cv.wait(lock, [&] { return w->stop || !w->queue.empty(); });
            if (w->stop && w->queue.empty())
                return;
            batch.swap(w->queue);
            w->queue.clear();
            w->draining = true;
            file = g_file;
        }
        const uint64_t t0 = NowNs();
        WriteBatch(batch, file);
        const uint64_t dt = NowNs() - t0;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_stats.written += batch.size();
            if (dt > g_stats.writerMaxNs)
                g_stats.writerMaxNs = dt;
            w->draining = false;
        }
        batch.clear();
        w->drained.notify_all();
    }
}

// Mutex held.
void StartWriterLocked()
{
    if (g_writer != nullptr)
        return;
    g_writer = new Writer();
    g_writer->thread = std::thread(WriterMain, g_writer);
}

// Mutex held by the caller, released and re-taken around the drain wait.
void StopWriterLocked(std::unique_lock<std::mutex>& lock)
{
    Writer* w = g_writer;
    g_writer = nullptr; // new writes go synchronous from here
    if (w == nullptr)
        return;
    w->stop = true;
    w->cv.notify_all();
    w->drained.wait_for(lock, std::chrono::milliseconds(500), [&] { return w->queue.empty() && !w->draining; });
    if (!w->queue.empty())
    {
        // The writer did not get to it in time: write the rest here, synchronously.
        std::vector<std::string> rest;
        rest.swap(w->queue);
        lock.unlock();
        WriteBatch(rest, g_file);
        lock.lock();
    }
    if (w->thread.joinable())
        w->thread.detach();
    // `w` is intentionally leaked: the detached thread may still be on its way out of
    // WriterMain and touches w->cv / w->stop as it leaves. A few hundred bytes at exit.
}

void OpenLocked(FILE* file, std::unique_lock<std::mutex>& lock)
{
    StopWriterLocked(lock);
    if (g_file != nullptr)
    {
        std::fclose(g_file);
        g_file = nullptr;
    }
    if (file == nullptr)
        return;
    g_file = file;
    StartWriterLocked();
}

} // namespace

void Log::Open(const std::wstring& path, LogLevel minLevel)
{
    std::unique_lock<std::mutex> lock(g_mutex);
    g_minLevel = minLevel;
    FILE* f = nullptr;
#ifdef _WIN32
    // Truncate: one file per session. A previous session's lines matching a grep is a trap
    // this project has already been caught by (CLAUDE.md §5, "confirm the timestamps are
    // from the NEW session").
    if (!path.empty() && _wfopen_s(&f, path.c_str(), L"w") != 0)
        f = nullptr;
#else
    (void)path;
#endif
    OpenLocked(f, lock);
}

void Log::OpenPath(const char* path, LogLevel minLevel)
{
    std::unique_lock<std::mutex> lock(g_mutex);
    g_minLevel = minLevel;
    FILE* f = nullptr;
    if (path != nullptr && *path != '\0')
    {
#ifdef _WIN32
        if (fopen_s(&f, path, "w") != 0)
            f = nullptr;
#else
        f = std::fopen(path, "w");
#endif
    }
    OpenLocked(f, lock);
}

void Log::Close()
{
    std::unique_lock<std::mutex> lock(g_mutex);
    StopWriterLocked(lock);
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

Log::Stats Log::GetStats()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    Stats s = g_stats;
    s.queuedNow = g_writer != nullptr ? g_writer->queue.size() : 0;
    return s;
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

    const uint64_t t0 = NowNs();
    std::unique_lock<std::mutex> lock(g_mutex);
    ++g_stats.lines;

    if (g_mirror.size() >= kMirrorCap)
    {
        g_mirror.erase(g_mirror.begin());
        ++g_mirrorDropped;
    }
    g_mirror.emplace_back(line);

    if (g_writer == nullptr)
    {
        // No writer (before Open, or after Close): synchronous, as it always was.
        if (g_file != nullptr)
        {
            std::fputs(line, g_file);
            std::fputc('\n', g_file);
            std::fflush(g_file);
        }
        return;
    }
    if (g_writer->queue.size() >= kMaxQueue)
    {
        ++g_stats.dropped;
        return;
    }
    g_writer->queue.emplace_back(line);
    Writer* w = g_writer;
    if (level == LogLevel::Error)
    {
        // Bounded wait for the drain: an ERROR is worth a stall, a lost ERROR is not.
        w->cv.notify_one();
        w->drained.wait_for(lock, kErrorDrainTimeout, [&] { return w->queue.empty() && !w->draining; });
        const uint64_t dt = NowNs() - t0;
        if (dt > g_stats.callerMaxNs)
            g_stats.callerMaxNs = dt;
        return;
    }
    const uint64_t dt = NowNs() - t0;
    if (dt > g_stats.callerMaxNs)
        g_stats.callerMaxNs = dt;
    lock.unlock();
    w->cv.notify_one();
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
