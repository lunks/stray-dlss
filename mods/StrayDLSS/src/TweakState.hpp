// The live-tuning MODEL: which [STRAYDLSS] knobs the in-game tab exposes, what each one is
// worth right now, how to change it, and how to write the whole set back to StrayDLSS.ini.
//
// It contains NO ImGui and NO UE4SS, on purpose: TweakUi.cpp is the only file that draws, and
// this half is therefore compiled by the workflow's two-minute mingw lane along with Host.cpp
// rather than only by the twenty-minute plugin lane.
//
// THREAD. Everything here is called from UE4SS's GUI thread (register_tab's render callback) or
// its ~200 Hz update thread (Host::Tick's ini reload) - never the render thread. It writes only
// through the modules' own public setters, which is exactly the discipline Host.cpp already
// uses for NgxNR and for the same reason: those setters store plain scalars that the render
// path re-reads on its next evaluate, so a change lands on the following frame with no lock on
// the per-frame path and no feature recreation. It adds no new synchronisation, and that is a
// deliberate choice rather than an omission - a mutex here would have to be taken by the
// evaluate as well, which is the one place this project will not put one.
//
// WHAT IT WILL NOT DO. It never releases a feature, never resizes anything, and never touches
// a knob whose value is consumed only at CreateFeature time (NgxPreset is the notable one:
// CLAUDE.md §5, "Set all five hint keys BEFORE CreateFeature; setting them after has no
// effect"). Those are shown read-only, because a slider that silently does nothing is worse
// than no slider.
#pragma once

#include <cstddef>
#include <string>

namespace stray_dlss::plugin::tweak {

enum class Kind
{
	boolean,
	integer,
	real,
	combo, // an integer with named values
};

enum class Group
{
	nr = 0,
	sr,
	fg,
	diagnostics,
	count,
};

struct Knob
{
	const char *ini_key;   // the [STRAYDLSS] key this saves to
	const char *label;     // what the tab shows
	const char *help;      // the annotation; hovering shows it, and it is NOT optional prose -
	                       // the NR structure sliders each wipe feature 18's temporal history,
	                       // and a user who does not know that mis-reads the whole screen
	                       // changing as the knob's own effect (CLAUDE.md §5, "the reset that
	                       // fooled us").
	Kind kind;
	float min_value;       // real/integer/combo range (combo: 0..count-1)
	float max_value;
	// Changing this fires CG2R_ResetTemporalHistoryOnControlChange inside the DLSSNR runtime,
	// which discards feature 18's whole accumulation for as long as the value keeps moving.
	// Measured from the disassembly (CLAUDE.md §5, "DLSSNR's structure controls").
	bool resets_nr_history;
	// True when the value is consumed once (at feature creation) and a live change cannot
	// matter this session. Drawn disabled, with `help` saying so.
	bool read_only;
	const char *const *combo_items;
	int combo_count;
};

// The registry. `count` receives the number of knobs in the group.
const Knob *knobs(Group g, int &count);
const char *group_name(Group g);

// Current live values, read from the modules themselves (app::nr_live(), fg::config(), ...)
// rather than from a shadow copy, so the tab cannot drift from what the frame is using.
float value_of(const Knob &k);
// Applies `v` through the owning module's public setter. A no-op for a read_only knob.
void set_value(const Knob &k, float v);

// ---- the ini save ----

// Rewrites `ini_path` so that every knob above carries its CURRENT live value, preserving every
// comment, every blank line and every key the tab does not know about (src/host/ini_edit.hpp).
// Returns false and fills `error` on any failure; the file is replaced atomically-ish (written
// whole, after a successful read), so a failed write leaves the old file intact.
bool save_to_ini(const std::string &ini_path, std::string &error);

// ---- the ini as a control channel ----

// Re-reads every knob above from the config source and applies it. Called from Host::Tick when
// StrayDLSS.ini changed on disk, so hand-editing the file stays a working control path whether
// or not the debug GUI can be opened on this box. Returns how many values actually moved.
int apply_from_config();

// ---- read-only status, for the tab's header ----

// One line each, NUL-terminated. Written for a screenshot: whoever reads them is diagnosing.
int format_sr_status(char *buf, std::size_t size);
int format_nr_status(char *buf, std::size_t size);
int format_fg_status(char *buf, std::size_t size);

} // namespace stray_dlss::plugin::tweak
