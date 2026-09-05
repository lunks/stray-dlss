#include "core/reset_plan.hpp"

namespace stray_dlss::resetplan {

Mode mode_from_level(int level)
{
	if (level <= 0)
		return Mode::off;
	if (level == 1)
		return Mode::observe;
	return Mode::engine;
}

const char *mode_name(Mode m)
{
	switch (m)
	{
	case Mode::off: return "off";
	case Mode::observe: return "observe";
	case Mode::engine: return "engine";
	default: return "?";
	}
}

const char *combo_name(unsigned combo)
{
	switch (combo & 7u)
	{
	case 0: return "none";
	case 1: return "cut";
	case 2: return "jitter";
	case 3: return "cut+jitter";
	case 4: return "1x1";
	case 5: return "cut+1x1";
	case 6: return "jitter+1x1";
	default: return "cut+jitter+1x1";
	}
}

Decision decide(Mode mode, const Signals &s)
{
	Decision d;
	d.jitter_only = s.jitter_equal && !s.camera_cut && !s.history_1x1;
	const bool heuristic = s.camera_cut || s.jitter_equal || s.history_1x1;
	if (mode == Mode::engine && s.engine_view)
	{
		d.source = Source::engine;
		d.reset = s.camera_cut || s.history_1x1;
		return d;
	}
	d.source = Source::heuristic_or;
	d.fell_back = mode == Mode::engine;
	d.reset = heuristic;
	return d;
}

void Counters::note(const Signals &s, const Decision &d)
{
	++frames;
	++combo[combo_index(s)];
	if (d.reset)
		++resets;
	if (d.source == Source::engine)
		++from_engine;
	if (d.fell_back)
		++fell_back;
	if (d.jitter_only)
	{
		++jitter_only;
		if (d.reset)
			++jitter_only_fired;
		else
			++jitter_only_suppressed;
	}
}

} // namespace stray_dlss::resetplan
