// The resolve pass logs through src/log.hpp, whose real implementation pulls in ReShade. The
// WARP harness deliberately has no ReShade in it — the point is to exercise our D3D12 usage in
// isolation — so logging is satisfied here by printing to stdout, where CI can read it.
#include "log.hpp"

#include <cstdarg>
#include <cstdio>

namespace stray_dlss::log {

void init_file_sink() {}
void init_file_sink_path(const char *) {}
void shutdown_file_sink() {}
void set_external_sink(ExternalSink) {}
Stats stats() { return Stats{}; }
void set_threshold(Threshold) {}
Threshold threshold() { return Threshold::debug; }
bool enabled(Level) { return true; }

void write(Level level, const char *message)
{
	const char *tag = level == Level::error ? "ERROR" : level == Level::warning ? "WARN " : "INFO ";
	std::printf("    [%s] %s\n", tag, message);
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
