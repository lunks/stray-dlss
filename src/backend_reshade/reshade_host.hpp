// What the ReShade backend offers the add-on entry point: its config source and its log sink.
#pragma once

#include "host/config.hpp"
#include "log.hpp"

namespace stray_dlss::rsb {

host::cfg::Source *reshade_config_source();
void reshade_log_sink(log::Level level, const char *message);

} // namespace stray_dlss::rsb
