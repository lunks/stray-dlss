// [STRAYDLSS] U0Hook level 3 (src/core/u0_authority.hpp): which source fills the TAA pass's
// register map, every named reason the bracket may NOT, and the one-way rule for switching
// the descriptor shadow's copy half off.
//
// As with the seam's tests, the effort goes on REFUSALS. A bracket missing ANY of the seven
// registers CLAUDE.md §2.3 names must send the frame to the walk with the register named; a
// skip must never arm on a session that has fallen back; and the shipped default must stay
// where it is until someone moves it on purpose.
#include "core/u0_authority.hpp"

#include <doctest/doctest.h>

#include <string>

using namespace stray_dlss::u0auth;

namespace {

// A bracket that answers everything, so each test can spoil exactly one thing.
BracketFacts complete()
{
	BracketFacts f;
	f.hooked = true;
	f.faulted = false;
	f.bracket_open = true;
	for (unsigned r = 0; r < kTaaTexRegs; ++r)
		f.t[r] = RegState::ok;
	f.u0 = RegState::ok;
	f.u0_is_buffer = false;
	f.u0_is_3d = false;
	f.u0_allow_uav = true;
	f.u0_hdr_colour = true;
	return f;
}

SkipInputs ready_to_arm()
{
	SkipInputs in;
	in.key = true;
	in.mode = Mode::authoritative;
	in.drive = true;
	in.hooked = true;
	in.faulted = false;
	in.already_armed = false;
	in.clean_streak = kSkipArmClaims;
	return in;
}

} // namespace

TEST_CASE("u0auth: the ladder maps levels the way the seam's does, and 3 is authoritative")
{
	CHECK(mode_from_level(-1) == Mode::off);
	CHECK(mode_from_level(0) == Mode::off);
	CHECK(mode_from_level(1) == Mode::discover);
	CHECK(mode_from_level(2) == Mode::observe);
	CHECK(mode_from_level(3) == Mode::authoritative);
	CHECK(mode_from_level(99) == Mode::authoritative);
	CHECK(std::string(mode_name(Mode::authoritative)) == "authoritative");
	CHECK(std::string(mode_name(Mode::observe)) == "observe");
}

TEST_CASE("u0auth: THE SHIPPED DEFAULT IS 1 (discover) - level 3 is built and UNCONFIRMED on the box")
{
	// Moving this is a deliberate edit against this test, never a one-character change.
	CHECK(kDefaultLevel == 1);
	CHECK(mode_from_level(kDefaultLevel) == Mode::discover);
}

TEST_CASE("u0auth: a complete bracket at level 3 is the source; below level 3 the walk is, uncounted")
{
	const BracketFacts f = complete();
	const Decision d3 = decide_source(Mode::authoritative, f);
	CHECK(d3.source == Source::bracket);
	CHECK(d3.fallback == Fallback::none);
	CHECK(d3.reg == 0);

	for (Mode m : { Mode::off, Mode::discover, Mode::observe })
	{
		const Decision d = decide_source(m, f);
		CHECK(d.source == Source::walk);
		// Not a fallback: below 3 the walk IS the design, and the [u0] line must not count it as
		// the bracket failing.
		CHECK(d.fallback == Fallback::level_below_3);
	}
}

TEST_CASE("u0auth: the session-level refusals come first, in the order they would be diagnosed")
{
	BracketFacts f = complete();
	f.hooked = false;
	CHECK(decide_source(Mode::authoritative, f).fallback == Fallback::hook_off);

	f = complete();
	f.faulted = true;
	CHECK(decide_source(Mode::authoritative, f).fallback == Fallback::faulted);

	f = complete();
	f.bracket_open = false;
	CHECK(decide_source(Mode::authoritative, f).fallback == Fallback::no_bracket);

	// hook_off outranks faulted outranks no_bracket: a session with all three set reports the
	// one that explains the other two.
	f = complete();
	f.hooked = false;
	f.faulted = true;
	f.bracket_open = false;
	CHECK(decide_source(Mode::authoritative, f).fallback == Fallback::hook_off);
}

TEST_CASE("u0auth: u0 is judged before any texture register, by state and then by shape")
{
	BracketFacts f = complete();
	f.u0 = RegState::absent;
	f.t[1] = RegState::absent; // a texture hole too - u0 must still be the named reason
	Decision d = decide_source(Mode::authoritative, f);
	CHECK(d.source == Source::walk);
	CHECK(d.fallback == Fallback::u0_no_bind);

	f = complete();
	f.u0 = RegState::unresolved;
	CHECK(decide_source(Mode::authoritative, f).fallback == Fallback::u0_unresolved);

	f = complete();
	f.u0 = RegState::dead;
	CHECK(decide_source(Mode::authoritative, f).fallback == Fallback::u0_not_live);

	// The shape test: a buffer, a volume, a texture without ALLOW_UNORDERED_ACCESS (the
	// black-output-with-no-error class, CLAUDE.md §0.2) and a non-HDR format are each refused.
	f = complete();
	f.u0_is_buffer = true;
	CHECK(decide_source(Mode::authoritative, f).fallback == Fallback::u0_desc_mismatch);
	f = complete();
	f.u0_is_3d = true;
	CHECK(decide_source(Mode::authoritative, f).fallback == Fallback::u0_desc_mismatch);
	f = complete();
	f.u0_allow_uav = false;
	CHECK(decide_source(Mode::authoritative, f).fallback == Fallback::u0_desc_mismatch);
	f = complete();
	f.u0_hdr_colour = false;
	CHECK(decide_source(Mode::authoritative, f).fallback == Fallback::u0_desc_mismatch);
}

TEST_CASE("u0auth: ANY missing texture register sends the frame to the walk, naming the FIRST hole")
{
	// Every FTAAStandaloneCS permutation declares t0..t5 (CLAUDE.md §5, 27 permutations), so a
	// bracket short of one is not a partial answer to be patched from the walk - it is a
	// different pass or a different build, and the whole frame goes to the walk.
	for (unsigned r = 0; r < kTaaTexRegs; ++r)
	{
		BracketFacts f = complete();
		f.t[r] = RegState::absent;
		const Decision d = decide_source(Mode::authoritative, f);
		CHECK(d.source == Source::walk);
		CHECK(d.fallback == Fallback::tex_no_bind);
		CHECK(d.reg == r);

		f = complete();
		f.t[r] = RegState::unresolved;
		CHECK(decide_source(Mode::authoritative, f).fallback == Fallback::tex_unresolved);
		CHECK(decide_source(Mode::authoritative, f).reg == r);

		f = complete();
		f.t[r] = RegState::dead;
		CHECK(decide_source(Mode::authoritative, f).fallback == Fallback::tex_not_live);
		CHECK(decide_source(Mode::authoritative, f).reg == r);
	}

	// Two holes: the lower register is the one named, so a log line reads in t-order.
	BracketFacts f = complete();
	f.t[4] = RegState::dead;
	f.t[2] = RegState::unresolved;
	const Decision d = decide_source(Mode::authoritative, f);
	CHECK(d.fallback == Fallback::tex_unresolved);
	CHECK(d.reg == 2);
}

TEST_CASE("u0auth: every fallback has a distinct printable name")
{
	for (unsigned i = 0; i < static_cast<unsigned>(Fallback::count); ++i)
	{
		const char *n = fallback_name(static_cast<Fallback>(i));
		CHECK(n != nullptr);
		CHECK(std::string(n) != "?");
		for (unsigned j = 0; j < i; ++j)
			CHECK(std::string(n) != fallback_name(static_cast<Fallback>(j)));
	}
	CHECK(std::string(fallback_name(Fallback::count)) == "?");
}

TEST_CASE("u0auth skip: the shipping value is OFF, and no counter can turn it on without the key")
{
	SkipInputs in = ready_to_arm();
	in.key = false;
	CHECK(skip_decide(in) == Skip::off_no_key);
	in.clean_streak = kSkipArmClaims * 1000;
	CHECK(skip_decide(in) == Skip::off_no_key);
}

TEST_CASE("u0auth skip: every precondition is a named refusal, in diagnostic order")
{
	SkipInputs in = ready_to_arm();
	in.mode = Mode::observe;
	CHECK(skip_decide(in) == Skip::off_mode);

	in = ready_to_arm();
	in.drive = false;
	CHECK(skip_decide(in) == Skip::off_not_drive);

	in = ready_to_arm();
	in.hooked = false;
	CHECK(skip_decide(in) == Skip::off_hook);

	in = ready_to_arm();
	in.faulted = true;
	CHECK(skip_decide(in) == Skip::off_faulted);
}

TEST_CASE("u0auth skip: arms only after kSkipArmClaims CONSECUTIVE bracket-answered claims")
{
	SkipInputs in = ready_to_arm();
	in.clean_streak = 0;
	CHECK(skip_decide(in) == Skip::arming);
	in.clean_streak = kSkipArmClaims - 1;
	CHECK(skip_decide(in) == Skip::arming);
	in.clean_streak = kSkipArmClaims;
	CHECK(skip_decide(in) == Skip::armed);
	in.clean_streak = kSkipArmClaims + 1;
	CHECK(skip_decide(in) == Skip::armed);
	// A fallback resets the streak in the live half; here that is a streak of 0 again.
	in.clean_streak = 0;
	CHECK(skip_decide(in) == Skip::arming);
}

TEST_CASE("u0auth skip: ARMED is one-way for the session, whatever the inputs say afterwards")
{
	// The copy half has stopped recording; a table the game did not re-copy after a resume
	// would resolve to a stale, live, wrong resource with no error (header comment). So no
	// input - a fault, a lost hook, the key going away, a fallback - re-enables it.
	SkipInputs in = ready_to_arm();
	in.already_armed = true;
	in.key = false;
	in.mode = Mode::off;
	in.drive = false;
	in.hooked = false;
	in.faulted = true;
	in.clean_streak = 0;
	CHECK(skip_decide(in) == Skip::armed);
}

TEST_CASE("u0auth skip: the arming bar is one report interval and every state has a name")
{
	CHECK(kSkipArmClaims == 600);
	for (unsigned i = 0; i < static_cast<unsigned>(Skip::count); ++i)
	{
		const char *n = skip_name(static_cast<Skip>(i));
		CHECK(n != nullptr);
		CHECK(std::string(n) != "?");
	}
}
