// ReShade.log as the external log sink. Level values are numerically ReShade's
// (log.hpp's enum was defined against them), so the cast is a reinterpretation, not a map.
#include "reshade_all.hpp"

#include "log.hpp"

#include <cstdio>

namespace stray_dlss::rsb {

void reshade_log_sink(log::Level level, const char *message)
{
	char prefixed[2048];
	std::snprintf(prefixed, sizeof(prefixed), "[stray-dlss] %s", message);
	reshade::log::message(static_cast<reshade::log::level>(level), prefixed);
}

} // namespace stray_dlss::rsb
