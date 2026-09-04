#include "host/ini_edit.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdio>

namespace stray_dlss::host {
namespace {

std::string lower(std::string s)
{
	std::transform(s.begin(), s.end(), s.begin(),
		[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return s;
}

std::string trim(const std::string &s)
{
	const std::size_t b = s.find_first_not_of(" \t\r\n");
	if (b == std::string::npos)
		return {};
	const std::size_t e = s.find_last_not_of(" \t\r\n");
	return s.substr(b, e - b + 1);
}

std::string norm(const std::string &s)
{
	return lower(trim(s));
}

// One physical line plus the exact bytes that ended it ("\n", "\r\n", or "" at EOF). Keeping
// the terminator with the line is what lets a rewrite preserve a CRLF file — the plugin's ini
// lives in a Wine prefix and is edited from both sides.
struct Line
{
	std::string text;
	std::string eol;
};

std::vector<Line> split_lines(const std::string &text)
{
	std::vector<Line> out;
	std::size_t i = 0;
	while (i < text.size())
	{
		const std::size_t nl = text.find('\n', i);
		if (nl == std::string::npos)
		{
			out.push_back({ text.substr(i), std::string() });
			return out;
		}
		std::size_t end = nl;
		std::string eol = "\n";
		if (end > i && text[end - 1] == '\r')
		{
			--end;
			eol = "\r\n";
		}
		out.push_back({ text.substr(i, end - i), eol });
		i = nl + 1;
	}
	return out;
}

bool is_section_header(const std::string &line, std::string &name)
{
	const std::string t = trim(line);
	if (t.empty() || t[0] != '[')
		return false;
	const std::size_t close = t.find(']');
	name = norm(t.substr(1, close == std::string::npos ? std::string::npos : close - 1));
	return true;
}

bool is_comment_or_blank(const std::string &line)
{
	const std::string t = trim(line);
	return t.empty() || t[0] == ';' || t[0] == '#';
}

// Splits `key = value` at the FIRST '=', the way ini.cpp does. `lhs` keeps its original
// spelling and spacing so a rewrite is invisible in a diff except for the value.
bool split_kv(const std::string &line, std::string &lhs, std::string &key)
{
	if (is_comment_or_blank(line))
		return false;
	const std::size_t eq = line.find('=');
	if (eq == std::string::npos)
		return false;
	// lhs keeps everything up to and including the '=' PLUS the run of spaces or tabs that
	// followed it, so `Key = value` stays `Key = value` and a save is invisible in a diff
	// except for the number.
	std::size_t vs = eq + 1;
	while (vs < line.size() && (line[vs] == ' ' || line[vs] == '\t'))
		++vs;
	lhs = line.substr(0, vs);
	key = norm(line.substr(0, eq));
	return !key.empty();
}

} // namespace

std::string ini_apply_edits(const std::string &text, const char *section,
	const std::vector<IniEdit> &edits)
{
	if (edits.empty())
		return text;
	const std::string want_section = norm(section != nullptr ? section : "");

	std::vector<Line> lines = split_lines(text);

	// Which edits have been applied to an EXISTING key. An edit may hit several duplicate
	// lines (see the header); each is rewritten, and the edit still counts as applied.
	std::vector<bool> applied(edits.size(), false);

	std::string current;
	bool in_section = false;
	// The index one past the section's last non-blank, non-comment-only line: where a new key
	// is inserted so it lands with the section's own keys rather than after its trailing
	// commentary or blank lines.
	std::size_t insert_at = 0;
	bool section_seen = false;

	for (std::size_t i = 0; i < lines.size(); ++i)
	{
		std::string name;
		if (is_section_header(lines[i].text, name))
		{
			current = name;
			in_section = (current == want_section);
			if (in_section)
			{
				section_seen = true;
				insert_at = i + 1;
			}
			continue;
		}
		if (!in_section)
			continue;
		std::string lhs, key;
		if (!split_kv(lines[i].text, lhs, key))
		{
			// A comment or a blank inside the section: it may be the header comment of a key
			// that follows, so it does not on its own move the insertion point.
			continue;
		}
		insert_at = i + 1;
		for (std::size_t e = 0; e < edits.size(); ++e)
		{
			if (norm(edits[e].key) != key)
				continue;
			lines[i].text = lhs + edits[e].value;
			applied[e] = true;
		}
	}

	// New keys, in the order given, inserted together at the section's insertion point.
	std::vector<Line> appended;
	for (std::size_t e = 0; e < edits.size(); ++e)
	{
		if (applied[e])
			continue;
		appended.push_back({ trim(edits[e].key) + "=" + edits[e].value, std::string() });
	}

	if (!appended.empty())
	{
		// Every inserted line needs a terminator. Reuse the file's prevailing one; a file with
		// no newline at all gets "\n".
		std::string eol = "\n";
		for (const Line &l : lines)
			if (!l.eol.empty())
			{
				eol = l.eol;
				break;
			}
		for (Line &l : appended)
			l.eol = eol;

		if (!section_seen)
		{
			// No such section: give the file a blank separator, the header, then the keys.
			if (!lines.empty() && !lines.back().eol.empty() && !trim(lines.back().text).empty())
				lines.push_back({ std::string(), eol });
			else if (!lines.empty() && lines.back().eol.empty())
				lines.back().eol = eol; // terminate the last line before appending after it
			lines.push_back({ "[" + trim(section != nullptr ? section : "") + "]", eol });
			insert_at = lines.size();
		}
		else if (insert_at > 0 && insert_at == lines.size() && lines.back().eol.empty())
		{
			// Appending after an unterminated final line: terminate it first.
			lines.back().eol = eol;
		}
		lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(insert_at),
			appended.begin(), appended.end());
	}

	std::string out;
	out.reserve(text.size() + 64 * appended.size());
	for (const Line &l : lines)
	{
		out += l.text;
		out += l.eol;
	}
	return out;
}

std::string ini_format_float(float v)
{
	char buf[48];
	std::snprintf(buf, sizeof(buf), "%.6g", static_cast<double>(v));
	return buf;
}

std::string ini_format_int(int v)
{
	char buf[24];
	std::snprintf(buf, sizeof(buf), "%d", v);
	return buf;
}

std::string ini_format_bool(bool v)
{
	return v ? "1" : "0";
}

} // namespace stray_dlss::host
