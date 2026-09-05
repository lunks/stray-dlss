// The camera-cut reset plan (src/core/reset_plan.hpp): which of the three §2.8 signals decides
// DLSS's InReset (and DLSSG.Reset / DLSSNR.Reset with it), and how the heuristic's own
// contribution is counted.
#include "core/reset_plan.hpp"

#include <doctest/doctest.h>

#include <string>

using namespace stray_dlss::resetplan;

TEST_CASE("resetplan: the ladder and the shipped default (1 = observe)")
{
	CHECK(mode_from_level(-1) == Mode::off);
	CHECK(mode_from_level(0) == Mode::off);
	CHECK(mode_from_level(1) == Mode::observe);
	CHECK(mode_from_level(2) == Mode::engine);
	CHECK(mode_from_level(7) == Mode::engine);
	CHECK(kDefaultLevel == 1);
	CHECK(mode_from_level(kDefaultLevel) == Mode::observe);
	CHECK(std::string(mode_name(Mode::engine)) == "engine");
}

TEST_CASE("resetplan: combo_index is bit0 cut, bit1 jitter, bit2 1x1, and every name is distinct")
{
	Signals s;
	CHECK(combo_index(s) == 0);
	s.camera_cut = true;
	CHECK(combo_index(s) == 1);
	s.camera_cut = false;
	s.jitter_equal = true;
	CHECK(combo_index(s) == 2);
	s.history_1x1 = true;
	CHECK(combo_index(s) == 6);
	s.camera_cut = true;
	CHECK(combo_index(s) == 7);
	for (unsigned a = 0; a < kCombos; ++a)
		for (unsigned b = a + 1; b < kCombos; ++b)
			CHECK(std::string(combo_name(a)) != std::string(combo_name(b)));
}

TEST_CASE("resetplan: below level 2 the decision is the three-way OR, whatever supplied the View")
{
	for (Mode m : { Mode::off, Mode::observe })
	{
		for (unsigned combo = 0; combo < kCombos; ++combo)
		{
			Signals s;
			s.camera_cut = (combo & 1u) != 0;
			s.jitter_equal = (combo & 2u) != 0;
			s.history_1x1 = (combo & 4u) != 0;
			s.engine_view = true;
			const Decision d = decide(m, s);
			CHECK(d.reset == (combo != 0));
			CHECK(d.source == Source::heuristic_or);
			CHECK_FALSE(d.fell_back);
			CHECK(d.jitter_only == (combo == 2));
		}
	}
}

TEST_CASE("resetplan: at level 2 with the engine's View, CameraCut | 1x1 decides and jitter-equality alone does NOT reset")
{
	Signals s;
	s.engine_view = true;
	s.jitter_equal = true;
	Decision d = decide(Mode::engine, s);
	CHECK_FALSE(d.reset);
	CHECK(d.source == Source::engine);
	CHECK(d.jitter_only);
	CHECK_FALSE(d.fell_back);

	s.camera_cut = true;
	d = decide(Mode::engine, s);
	CHECK(d.reset);
	CHECK_FALSE(d.jitter_only);

	Signals h;
	h.engine_view = true;
	h.history_1x1 = true;
	d = decide(Mode::engine, h);
	CHECK(d.reset); // the 1x1 half of bCameraCut never reaches the View; it is kept at every level
	CHECK(d.source == Source::engine);
}

TEST_CASE("resetplan: at level 2 WITHOUT the engine's View the OR decides, counted as a fallback")
{
	Signals s;
	s.engine_view = false;
	s.jitter_equal = true;
	const Decision d = decide(Mode::engine, s);
	CHECK(d.reset);
	CHECK(d.source == Source::heuristic_or);
	CHECK(d.fell_back);
	CHECK(d.jitter_only);
}

TEST_CASE("resetplan: Counters separate the jitter-only resets the engine would not have taken from the ones the OR took")
{
	Counters c;
	Signals s;
	s.engine_view = true;
	s.jitter_equal = true;
	c.note(s, decide(Mode::engine, s));
	c.note(s, decide(Mode::observe, s));
	Signals cut;
	cut.engine_view = true;
	cut.camera_cut = true;
	c.note(cut, decide(Mode::engine, cut));
	Signals none;
	none.engine_view = false;
	c.note(none, decide(Mode::engine, none));

	CHECK(c.frames == 4);
	CHECK(c.combo[2] == 2);
	CHECK(c.combo[1] == 1);
	CHECK(c.combo[0] == 1);
	CHECK(c.resets == 2);              // observe's jitter-only, and the cut
	CHECK(c.from_engine == 2);         // the level-2 jitter-only (suppressed) and the cut
	CHECK(c.fell_back == 1);           // the frame with no engine View
	CHECK(c.jitter_only == 2);
	CHECK(c.jitter_only_suppressed == 1);
	CHECK(c.jitter_only_fired == 1);
}
