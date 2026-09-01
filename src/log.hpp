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
void shutdown_file_sink();

// An optional second sink, installed by whichever host we run inside: the ReShade backend
// forwards into ReShade.log once register_addon has succeeded; the UE4SS plugin mirrors into
// UE4SS's console. Called after the file write, on the logging thread, under the log mutex —
// so it must not log back into here. nullptr removes it.
using ExternalSink = void (*)(Level level, const char *message);
void set_external_sink(ExternalSink sink);

void write(Level level, const char *message);

// printf-style. ReShade's own add-on-facing log takes no format arguments, so we format
// into a stack buffer ourselves. (docs/RESEARCH.md §2.3)
void writef(Level level, const char *format, ...);

} // namespace stray_dlss::log

#define STRAY_LOG_INFO(...) ::stray_dlss::log::writef(::stray_dlss::log::Level::info, __VA_ARGS__)
#define STRAY_LOG_WARN(...) ::stray_dlss::log::writef(::stray_dlss::log::Level::warning, __VA_ARGS__)
#define STRAY_LOG_ERROR(...) ::stray_dlss::log::writef(::stray_dlss::log::Level::error, __VA_ARGS__)
#define STRAY_LOG_DEBUG(...) ::stray_dlss::log::writef(::stray_dlss::log::Level::debug, __VA_ARGS__)
