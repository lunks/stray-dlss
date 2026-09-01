// ReShade's ini as the configuration Source: the add-on's [STRAYDLSS] section of ReShade.ini.
#include "reshade_all.hpp"

#include "host/config.hpp"

namespace stray_dlss::rsb {

struct ReshadeConfigSource final : host::cfg::Source
{
	bool get(const char *key, char *buf, std::size_t *size) override
	{
		// The char overload. NOTE the documented trap (CLAUDE.md §5): comma lists come back as
		// element 0 only — unchanged here; lists still go through sidecar files.
		return reshade::get_config_value(nullptr, "STRAYDLSS", key, buf, size);
	}
};

host::cfg::Source *reshade_config_source()
{
	static ReshadeConfigSource s;
	return &s;
}

} // namespace stray_dlss::rsb
