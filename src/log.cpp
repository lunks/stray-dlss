#include "log.hpp"

#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace stray_dlss::log {
namespace {

std::mutex g_mutex;
std::FILE *g_file = nullptr;
ExternalSink g_external = nullptr;

const char *level_tag(Level level)
{
	switch (level)
	{
	case Level::error:   return "ERROR";
	case Level::warning: return "WARN ";
	case Level::info:    return "INFO ";
	case Level::debug:   return "DEBUG";
	}
	return "?????";
}

} // namespace

void init_file_sink()
{
	std::lock_guard<std::mutex> lock(g_mutex);
	if (g_file != nullptr)
		return;

	// Deliberately a fixed name next to the game executable's working directory rather than
	// anything clever: the user has to be able to find and paste it.
#ifdef _WIN32
	if (fopen_s(&g_file, "stray-dlss.log", "w") != 0)
		g_file = nullptr;
#else
	g_file = std::fopen("stray-dlss.log", "w");
#endif
}

void shutdown_file_sink()
{
	std::lock_guard<std::mutex> lock(g_mutex);
	if (g_file != nullptr)
	{
		std::fclose(g_file);
		g_file = nullptr;
	}
}

void set_external_sink(ExternalSink sink)
{
	std::lock_guard<std::mutex> lock(g_mutex);
	g_external = sink;
}

void write(Level level, const char *message)
{
	std::lock_guard<std::mutex> lock(g_mutex);

	if (g_file != nullptr)
	{
		std::fprintf(g_file, "[%s] %s\n", level_tag(level), message);
		std::fflush(g_file); // the interesting failures are the ones that crash next
	}

	if (g_external != nullptr)
		g_external(level, message);
}

void writef(Level level, const char *format, ...)
{
	char buffer[2048];

	va_list args;
	va_start(args, format);
	std::vsnprintf(buffer, sizeof(buffer), format, args);
	va_end(args);

	write(level, buffer);
}

} // namespace stray_dlss::log
