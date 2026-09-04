// Writing a value BACK into a hand-maintained INI, without destroying the file.
//
// StrayDLSS.ini is how the box is driven: 380 lines of keys with the reasoning for each one
// written above it as a comment, and CLAUDE.md's own rule is that a deploy writes only the keys
// it is passed and stale keys persist. A save that rewrote the file from the parsed key/value
// map would silently delete every comment and every key the writer did not happen to know
// about — which is the same class of quiet damage this project keeps paying for elsewhere.
//
// So the editor works on the file's TEXT, not on IniFile's map. It preserves, byte for byte,
// every line it does not have to change; the only lines it touches are the ones whose key it
// was asked to set. It is pure (no filesystem, no Windows) and is tested on Linux in
// tests/test_ini_edit.cpp.
//
// The matching rules deliberately mirror src/host/ini.cpp's parser, because a save that the
// parser then reads differently would be worse than no save at all:
//   * sections and keys compare case-insensitively, after trimming;
//   * a line's value is EVERYTHING after the first '=' — ini.cpp strips no inline comment, so
//     rewriting `key=value` loses nothing;
//   * a full-line comment is ';' or '#' in the first non-space column;
//   * when a key appears more than once in a section, ini.cpp's map keeps the LAST. This
//     editor rewrites EVERY occurrence, so the value the parser reads back is the value that
//     was asked for whichever duplicate it lands on.
#pragma once

#include <string>
#include <vector>

namespace stray_dlss::host {

struct IniEdit
{
	std::string key;
	std::string value;
};

// Returns `text` with each edit applied inside `section`.
//
//   * a key already in the section  -> its value is replaced, keeping the original key
//                                      spelling, the original spacing around '=' and the
//                                      original line ending;
//   * a key not in the section      -> appended after the section's last non-blank line, ahead
//                                      of the next `[header]`;
//   * the section not in the file   -> appended at the end, with its header.
//
// Everything else — comments, blank lines, key order, other sections, keys not named in
// `edits` — is returned unchanged.
std::string ini_apply_edits(const std::string &text, const char *section,
	const std::vector<IniEdit> &edits);

// Value formatting, so a saved value round-trips through ini_parse_* to the same number.
// Floats use %.6g: short enough to stay hand-editable, wide enough that a float's decimal
// representation reads back as the same float for every value this project's knobs carry.
std::string ini_format_float(float v);
std::string ini_format_int(int v);
std::string ini_format_bool(bool v);

} // namespace stray_dlss::host
