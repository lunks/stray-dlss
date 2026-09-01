// A hand-edited, hot-reloadable INI. Lifted from mods/StrayDualSense/src/Config.cpp (Trim,
// Lower, ParseBool, ParseFloat, the mtime reload) and made section-aware, because this project
// carries 55 [STRAYDLSS] keys and, once it no longer runs inside ReShade, has to read them from
// a file of its own. Kept free of Windows so the parser is unit-tested on Linux.
//
// Why hot reload at all: this code cannot be built or tested where it is written — the only
// tuning loop is someone editing a value on the box, and making that not require a relaunch
// is worth the code (mods/StrayDualSense/src/Config.hpp says the same).
#pragma once

#include <cstdint>
#include <map>
#include <string>

namespace stray_dlss::host {

class IniFile
{
public:
	// Parses `utf8_path`. `[section]` headers, `key = value` lines, `;` and `#` comments. Keys
	// and sections are matched case-insensitively; values are trimmed, otherwise verbatim.
	// Returns false only if the file could not be opened (the map is then left EMPTY).
	bool load(const std::string &utf8_path);

	// Re-reads if the file's mtime moved since load()/the last reload. True when re-read.
	bool reload_if_changed();

	bool get(const char *section, const char *key, std::string &out) const;

	// For tests and for a backend that wants to seed values without a file.
	void set_for_test(const char *section, const char *key, const char *value);

	const std::string &path() const { return m_path; }
	std::size_t size() const { return m_values.size(); }

private:
	static std::string norm(const char *s);
	std::map<std::string, std::string> m_values; // "section\x1fkey" -> value
	std::string m_path;
	std::uint64_t m_last_write_time = 0;
};

// Coercions shared by every reader of a value. Pure; tested.
bool ini_parse_bool(const std::string &v, bool fallback);
int ini_parse_int(const std::string &v, int fallback);
float ini_parse_float(const std::string &v, float fallback);

} // namespace stray_dlss::host
