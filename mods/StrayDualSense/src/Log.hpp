// StrayDualSense — logging.
//
// Deliberately free of every UE4SS type. SIX threads log here -- the game thread (UFunction
// hooks), UE4SS's own event-loop thread (on_update), and our trigger, haptic and speaker
// workers -- so the sink is mutex-guarded. Since 2026-09-05 the FILE WRITE IS ASYNCHRONOUS:
// Write() formats the line and enqueues it, one writer thread does the fputs + fflush per batch.
// The reason is measured in StrayDLSS, whose logger had the same fputs + fflush-per-line shape
// and whose user correlated the on-screen flicker with the log line for line; here two of the
// callers are the trigger and haptic workers, whose timing is the product. What is kept: an
// ERROR line is on disk before its caller continues (a bounded wait for the drain), because
// this project's only feedback loop is a user pasting a log back and a line lost at the moment
// of a crash is a line that never existed; lines stay complete and in enqueue order; before
// Open and after Close the write is synchronous.
//
// UE4SS's own Output::send is NOT called from here. The mod glue drains
// `Log::TakeMirrorLines()` from on_update and forwards on the thread UE4SS itself calls us
// on, so a worker never reaches into UE4SS's logging from an arbitrary thread.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sds {

enum class LogLevel : uint8_t { Debug = 0, Info = 1, Warn = 2, Error = 3 };

namespace Log {

// `path` may be empty, in which case only the in-memory mirror is kept.
void Open(const std::wstring& path, LogLevel minLevel);
// The same, with a narrow path on every platform (the tests use it).
void OpenPath(const char* path, LogLevel minLevel);
// Drains what is queued (bounded), then closes the file.
void Close();

void SetMinLevel(LogLevel level);
LogLevel MinLevel();

// printf-style. Formats on the caller, enqueues; an ERROR waits for the drain.
void Write(LogLevel level, const char* fmt, ...);

// What the logger cost. `callerMaxNs` is the longest any Write() took on its calling thread
// (an ERROR waits for the drain and is expected to be the maximum); `dropped` must stay 0.
struct Stats
{
    uint64_t lines       = 0;  // Write() calls that passed the level filter
    uint64_t written     = 0;  // lines the writer thread put in the file
    uint64_t dropped     = 0;  // lines refused because the queue was full
    uint64_t queuedNow   = 0;  // queue depth at the time of the call
    uint64_t writerMaxNs = 0;  // slowest batch on the writer thread
    uint64_t callerMaxNs = 0;  // slowest Write() as seen by its caller
};
Stats GetStats();

// Lines produced since the last call, for mirroring into UE4SS's own log from the game
// thread. Bounded: if nobody drains it, the oldest lines are dropped and a "dropped N"
// marker is emitted, so the mirror can never grow without bound.
std::vector<std::string> TakeMirrorLines();

} // namespace Log

#define SDS_LOG_DEBUG(...) ::sds::Log::Write(::sds::LogLevel::Debug, __VA_ARGS__)
#define SDS_LOG_INFO(...)  ::sds::Log::Write(::sds::LogLevel::Info,  __VA_ARGS__)
#define SDS_LOG_WARN(...)  ::sds::Log::Write(::sds::LogLevel::Warn,  __VA_ARGS__)
#define SDS_LOG_ERROR(...) ::sds::Log::Write(::sds::LogLevel::Error, __VA_ARGS__)

} // namespace sds
