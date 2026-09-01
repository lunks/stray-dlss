// StrayDLSS — logging. Copied verbatim from mods/StrayDualSense/src/Log.hpp (the sibling
// plugin, same box, same author); it is replaced by the shared stray_dlss::log sink once the
// mod links the native backend library (plan Task 16).
//
// Deliberately free of every UE4SS type. FIVE threads log here -- the game thread (UFunction
// hooks), UE4SS's own event-loop thread (on_update), and our trigger, haptic and speaker
// workers -- so the sink is mutex-guarded and the file is flushed on every line: this
// project's only feedback loop is a user pasting a log back, and a line buffered at the moment
// of a crash is a line that never existed.
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
void Close();

void SetMinLevel(LogLevel level);
LogLevel MinLevel();

// printf-style. Every call takes the sink mutex and flushes.
void Write(LogLevel level, const char* fmt, ...);

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
