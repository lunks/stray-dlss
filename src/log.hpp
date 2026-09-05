// Logging is this project's second product. The developer cannot run the game, so a single
// pasted log has to be enough to diagnose a failure without a second round-trip.
//
// Two sinks, deliberately:
//   * the host's log — ReShade.log or UE4SS's console, whichever host loaded us; installable,
//                      and only available once that host has accepted us.
//   * our own file   — written from DllMain onward, so "the add-on never loaded" and
//                      "register_addon returned false" are distinguishable. An imgui version
//                      mismatch fails exactly this way, silently. (docs/RESEARCH.md §2.8)
//
// THE FILE WRITE IS ASYNCHRONOUS since 2026-09-05. write() formats the line and enqueues it;
// one writer thread does the fputs + fflush in batches. The user correlated the on-screen
// flicker with the log line for line (a small blink on the 600-frame status block, a large one
// on the [stall] line) while the stall detector's own buckets accounted for ~2.6 ms of a 40 ms
// frame: the synchronous write on the present thread was the unmeasured rest. An ERROR line
// still reaches the disk before its caller continues (bounded wait), because the interesting
// failures are the ones that crash next. Details and the measurement in log.cpp.
//
// No framework header is included here or in log.cpp: this file is shared by every host.
#pragma once

namespace stray_dlss::log {

enum class Level
{
	error = 1,
	warning = 2,
	info = 3,
	debug = 4,
};

// Opens the side-channel file next to the add-on. Safe to call before register_addon.
void init_file_sink();
// The same sink at a narrow path, every platform (the unit tests use it).
void init_file_sink_path(const char *path);
#ifdef _WIN32
// The same sink at an explicit path (the UE4SS host writes <game>/stray-dlss-plugin.log so it
// never collides with a ReShade add-on's stray-dlss.log in the same directory).
void init_file_sink(const wchar_t *path);
#endif
// Stops the writer thread, draining everything queued, then closes the file.
void shutdown_file_sink();

// An optional second sink, installed by whichever host we run inside: the ReShade backend
// forwards into ReShade.log once register_addon has succeeded; the UE4SS plugin mirrors into
// UE4SS's console. Called from the WRITER thread after the file write (or from the caller,
// synchronously, when no writer is running) — so it must not log back into here. nullptr
// removes it.
using ExternalSink = void (*)(Level level, const char *message);
void set_external_sink(ExternalSink sink);

// What the logger cost. `caller_ns_max` is the longest any write() took on its calling thread
// (an ERROR waits for the drain and is expected to be the maximum); `dropped` must stay 0.
struct Stats
{
	unsigned long long lines = 0;          // write() calls
	unsigned long long written = 0;        // lines the writer put in the file
	unsigned long long dropped = 0;        // lines refused because the queue was full
	unsigned long long queued_now = 0;     // queue depth when stats() was called
	unsigned long long writer_batches = 0;
	unsigned long long batch_max = 0;      // largest batch drained at once
	unsigned long long writer_ns_sum = 0;  // time the writer thread spent in fputs/fflush
	unsigned long long writer_ns_max = 0;  // slowest single batch
	unsigned long long caller_ns_max = 0;  // slowest write() as seen by a caller
};
Stats stats();

void write(Level level, const char *message);

// printf-style. ReShade's own add-on-facing log takes no format arguments, so we format
// into a stack buffer ourselves. (docs/RESEARCH.md §2.3)
void writef(Level level, const char *format, ...);

} // namespace stray_dlss::log

#define STRAY_LOG_INFO(...) ::stray_dlss::log::writef(::stray_dlss::log::Level::info, __VA_ARGS__)
#define STRAY_LOG_WARN(...) ::stray_dlss::log::writef(::stray_dlss::log::Level::warning, __VA_ARGS__)
#define STRAY_LOG_ERROR(...) ::stray_dlss::log::writef(::stray_dlss::log::Level::error, __VA_ARGS__)
#define STRAY_LOG_DEBUG(...) ::stray_dlss::log::writef(::stray_dlss::log::Level::debug, __VA_ARGS__)
