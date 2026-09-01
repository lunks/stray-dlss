#include "host/config.hpp"

#include "host/ini.hpp"

#include <cstring>
#include <string>

namespace stray_dlss::host::cfg {
namespace {

Source *g_source = nullptr;

// One stack buffer for every scalar read. 256 is far beyond any scalar in the ini; a value
// longer than that is a string key and goes through get_string with the caller's buffer.
bool read(const char *key, std::string &out)
{
	if (g_source == nullptr || key == nullptr)
		return false;
	char buf[256] = {};
	std::size_t size = sizeof(buf);
	if (!g_source->get(key, buf, &size))
		return false;
	out.assign(buf);
	return true;
}

} // namespace

void set_source(Source *source) { g_source = source; }
Source *source() { return g_source; }

bool get_bool(const char *key, bool fallback)
{
	std::string v;
	return read(key, v) ? ini_parse_bool(v, fallback) : fallback;
}

int get_int(const char *key, int fallback)
{
	std::string v;
	return read(key, v) ? ini_parse_int(v, fallback) : fallback;
}

float get_float(const char *key, float fallback)
{
	std::string v;
	return read(key, v) ? ini_parse_float(v, fallback) : fallback;
}

bool get_string(const char *key, char *buf, std::size_t size)
{
	if (buf == nullptr || size == 0)
		return false;
	if (g_source == nullptr || key == nullptr)
	{
		buf[0] = '\0';
		return false;
	}
	std::size_t n = size;
	if (!g_source->get(key, buf, &n))
	{
		buf[0] = '\0';
		return false;
	}
	buf[size - 1] = '\0';
	return true;
}

bool has(const char *key)
{
	std::string v;
	return read(key, v);
}

} // namespace stray_dlss::host::cfg
