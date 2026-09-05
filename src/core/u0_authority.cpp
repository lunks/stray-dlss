#include "core/u0_authority.hpp"

namespace stray_dlss::u0auth {

Mode mode_from_level(int level)
{
	if (level <= 0)
		return Mode::off;
	if (level == 1)
		return Mode::discover;
	if (level == 2)
		return Mode::observe;
	return Mode::authoritative;
}

const char *mode_name(Mode m)
{
	switch (m)
	{
	case Mode::off: return "off";
	case Mode::discover: return "discover";
	case Mode::observe: return "observe";
	case Mode::authoritative: return "authoritative";
	default: return "?";
	}
}

const char *fallback_name(Fallback f)
{
	switch (f)
	{
	case Fallback::none: return "none";
	case Fallback::level_below_3: return "levelBelow3";
	case Fallback::hook_off: return "hookOff";
	case Fallback::faulted: return "faulted";
	case Fallback::no_bracket: return "noBracket";
	case Fallback::u0_no_bind: return "u0NoBind";
	case Fallback::u0_unresolved: return "u0Unresolved";
	case Fallback::u0_not_live: return "u0NotLive";
	case Fallback::u0_desc_mismatch: return "u0Desc";
	case Fallback::tex_no_bind: return "texNoBind";
	case Fallback::tex_unresolved: return "texUnresolved";
	case Fallback::tex_not_live: return "texNotLive";
	default: return "?";
	}
}

Decision decide_source(Mode mode, const BracketFacts &f)
{
	Decision d;
	d.source = Source::walk;
	if (mode != Mode::authoritative)
	{
		d.fallback = Fallback::level_below_3;
		return d;
	}
	if (!f.hooked)
	{
		d.fallback = Fallback::hook_off;
		return d;
	}
	if (f.faulted)
	{
		d.fallback = Fallback::faulted;
		return d;
	}
	if (!f.bracket_open)
	{
		d.fallback = Fallback::no_bracket;
		return d;
	}
	// u0 first: it is the one register whose wrong answer is a wrong IMAGE rather than a wrong
	// input, and the one the whole route exists for.
	switch (f.u0)
	{
	case RegState::absent: d.fallback = Fallback::u0_no_bind; return d;
	case RegState::unresolved: d.fallback = Fallback::u0_unresolved; return d;
	case RegState::dead: d.fallback = Fallback::u0_not_live; return d;
	default: break;
	}
	if (f.u0_is_buffer || f.u0_is_3d || !f.u0_allow_uav || !f.u0_hdr_colour)
	{
		d.fallback = Fallback::u0_desc_mismatch;
		return d;
	}
	// Then every texture register, in t-order, so `reg` names the FIRST hole.
	for (unsigned r = 0; r < kTaaTexRegs; ++r)
	{
		d.reg = r;
		switch (f.t[r])
		{
		case RegState::absent: d.fallback = Fallback::tex_no_bind; return d;
		case RegState::unresolved: d.fallback = Fallback::tex_unresolved; return d;
		case RegState::dead: d.fallback = Fallback::tex_not_live; return d;
		default: break;
		}
	}
	d.reg = 0;
	d.source = Source::bracket;
	d.fallback = Fallback::none;
	return d;
}

const char *skip_name(Skip s)
{
	switch (s)
	{
	case Skip::off_no_key: return "off(noKey)";
	case Skip::off_mode: return "off(levelBelow3)";
	case Skip::off_not_drive: return "off(notDrive)";
	case Skip::off_hook: return "off(hookOff)";
	case Skip::off_faulted: return "off(faulted)";
	case Skip::arming: return "arming";
	case Skip::armed: return "ARMED";
	default: return "?";
	}
}

Skip skip_decide(const SkipInputs &in)
{
	// One-way: the shadow's copy half has stopped recording and cannot be trusted again this
	// session (header comment). Whatever else the inputs say, the answer is what it was.
	if (in.already_armed)
		return Skip::armed;
	if (!in.key)
		return Skip::off_no_key;
	if (in.mode != Mode::authoritative)
		return Skip::off_mode;
	if (!in.drive)
		return Skip::off_not_drive;
	if (!in.hooked)
		return Skip::off_hook;
	if (in.faulted)
		return Skip::off_faulted;
	return in.clean_streak >= kSkipArmClaims ? Skip::armed : Skip::arming;
}

} // namespace stray_dlss::u0auth
