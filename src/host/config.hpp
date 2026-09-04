// The configuration seam. Every [STRAYDLSS] key is read through these getters, and WHERE the
// value comes from is one installable Source. Only one remains: our own hot-reloaded
// StrayDLSS.ini under the UE4SS plugin host. The ReShade ini source went with the add-on.
//
// The getters coerce the way ReShade's typed get_config_value did — bool from 0/1/true/false,
// int via strtol (so 0x-prefixed hex works), float via strtod — so a key reads the same value
// under either host. The section is always STRAYDLSS; the Source hides that.
#pragma once

#include <cstddef>

namespace stray_dlss::host::cfg {

struct Source
{
	virtual ~Source() = default;
	// ReShade's char-overload contract, kept on purpose so its source is a one-liner: copies
	// the value into `buf` and returns true if the key exists and fits; if it exists but does
	// not fit, sets *size to the required size (including the NUL) and returns false; if it
	// does not exist, returns false and leaves *size alone.
	virtual bool get(const char *key, char *buf, std::size_t *size) = 0;
};

// Exactly one at a time. nullptr makes every getter return its fallback.
void set_source(Source *source);
Source *source();

bool get_bool(const char *key, bool fallback);
int get_int(const char *key, int fallback);
float get_float(const char *key, float fallback);
// Copies at most `size-1` chars, NUL-terminates, returns true if the key existed (and fit).
bool get_string(const char *key, char *buf, std::size_t size);
// Whether the key exists at all, whatever its value.
bool has(const char *key);

} // namespace stray_dlss::host::cfg
