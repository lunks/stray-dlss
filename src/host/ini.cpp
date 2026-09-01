#include "host/ini.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <system_error>

namespace stray_dlss::host {
namespace {

std::string trim(const std::string &s)
{
	const size_t b = s.find_first_not_of(" \t\r\n");
	if (b == std::string::npos)
		return {};
	const size_t e = s.find_last_not_of(" \t\r\n");
	return s.substr(b, e - b + 1);
}

std::string lower(std::string s)
{
	std::transform(s.begin(), s.end(), s.begin(),
		[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return s;
}

std::uint64_t file_write_time(const std::string &path)
{
	std::error_code ec;
	const auto t = std::filesystem::last_write_time(path, ec);
	if (ec)
		return 0;
	return static_cast<std::uint64_t>(t.time_since_epoch().count());
}

} // namespace

bool ini_parse_bool(const std::string &v, bool fallback)
{
	const std::string l = lower(trim(v));
	if (l == "1" || l == "true" || l == "yes" || l == "on")
		return true;
	if (l == "0" || l == "false" || l == "no" || l == "off")
		return false;
	return fallback;
}

int ini_parse_int(const std::string &v, int fallback)
{
	const std::string t = trim(v);
	char *end = nullptr;
	const long n = std::strtol(t.c_str(), &end, 0);
	return end == t.c_str() ? fallback : static_cast<int>(n);
}

float ini_parse_float(const std::string &v, float fallback)
{
	const std::string t = trim(v);
	char *end = nullptr;
	const double d = std::strtod(t.c_str(), &end);
	return end == t.c_str() ? fallback : static_cast<float>(d);
}

std::string IniFile::norm(const char *s)
{
	return lower(trim(s != nullptr ? s : ""));
}

bool IniFile::load(const std::string &utf8_path)
{
	m_values.clear();
	m_path = utf8_path;
	std::FILE *f = nullptr;
#ifdef _WIN32
	if (fopen_s(&f, utf8_path.c_str(), "r") != 0)
		f = nullptr;
#else
	f = std::fopen(utf8_path.c_str(), "r");
#endif
	if (f == nullptr)
		return false;

	std::string section;
	char line[1024];
	while (std::fgets(line, sizeof(line), f) != nullptr)
	{
		const std::string s = trim(line);
		if (s.empty() || s[0] == '#' || s[0] == ';')
			continue;
		if (s[0] == '[')
		{
			const size_t close = s.find(']');
			section = lower(trim(s.substr(1, close == std::string::npos ? std::string::npos : close - 1)));
			continue;
		}
		const size_t eq = s.find('=');
		if (eq == std::string::npos)
			continue;
		const std::string key = lower(trim(s.substr(0, eq)));
		const std::string val = trim(s.substr(eq + 1));
		if (key.empty())
			continue;
		m_values[section + '\x1f' + key] = val;
	}
	std::fclose(f);
	m_last_write_time = file_write_time(utf8_path);
	return true;
}

bool IniFile::reload_if_changed()
{
	if (m_path.empty())
		return false;
	const std::uint64_t t = file_write_time(m_path);
	if (t == 0 || t == m_last_write_time)
		return false;
	IniFile fresh;
	if (!fresh.load(m_path))
		return false;
	m_values.swap(fresh.m_values);
	m_last_write_time = fresh.m_last_write_time;
	return true;
}

bool IniFile::get(const char *section, const char *key, std::string &out) const
{
	const auto it = m_values.find(norm(section) + '\x1f' + norm(key));
	if (it == m_values.end())
		return false;
	out = it->second;
	return true;
}

void IniFile::set_for_test(const char *section, const char *key, const char *value)
{
	m_values[norm(section) + '\x1f' + norm(key)] = trim(value != nullptr ? value : "");
}

} // namespace stray_dlss::host
