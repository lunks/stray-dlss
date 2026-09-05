#include "core/frame_seams.hpp"

namespace stray_dlss::fseam {

const char *load_action_name(LoadAction a)
{
	switch (a)
	{
	case LoadAction::no_action: return "DontLoad";
	case LoadAction::load: return "Load";
	case LoadAction::clear: return "Clear";
	default: return "invalid";
	}
}

bool decode_color0(const u0::ObjectReader &r, std::uint64_t info, Color0 *out)
{
	if (out == nullptr || info == 0 || r.read_u64 == nullptr)
		return false;
	std::uint64_t rt = 0;
	std::uint64_t tail = 0; // qword 16..23: ArraySlice (int32), MipIndex (u8), Action (u8), pad
	if (!r.read_u64(r.ctx, info + kColorEntryRenderTarget, &rt))
		return false;
	if (!r.read_u64(r.ctx, info + 16, &tail))
		return false;
	out->render_target = rt;
	out->action = static_cast<std::uint8_t>((tail >> (8u * (kColorEntryAction - 16))) & 0xFFu);
	return true;
}

// ---------------------------------------------------------------------------------------

void Candidates::set(const std::uint64_t *ids, unsigned n, std::uint64_t new_epoch)
{
	count = 0;
	for (unsigned i = 0; i < n && count < kMaxCandidates; ++i)
		if (ids != nullptr && ids[i] != 0)
			res[count++] = ids[i];
	epoch = new_epoch;
}

bool Candidates::contains(std::uint64_t r) const { return index_of(r) >= 0; }

int Candidates::index_of(std::uint64_t r) const
{
	if (r == 0)
		return -1;
	for (unsigned i = 0; i < count; ++i)
		if (res[i] == r)
			return static_cast<int>(i);
	return -1;
}

// ---------------------------------------------------------------------------------------

const char *pass_class_name(PassClass c)
{
	switch (c)
	{
	case PassClass::not_back_buffer: return "notBackBuffer";
	case PassClass::bb_outside_bracket: return "bbScene";
	case PassClass::bb_hud: return "bbHud";
	case PassClass::bb_in_bracket_later: return "bbInBracketLater";
	case PassClass::bb_in_bracket_no_load: return "bbInBracketNoLoad";
	default: return "?";
	}
}

void FrameLedger::begin_bracket()
{
	in_bracket = true;
	++brackets;
}

void FrameLedger::end_bracket() { in_bracket = false; }

void FrameLedger::reset() { *this = FrameLedger{}; }

PassClass FrameLedger::classify(bool is_back_buffer, std::uint64_t resolved, std::uint8_t action)
{
	if (!is_back_buffer)
		return PassClass::not_back_buffer;
	if (!in_bracket)
	{
		++bb_outside;
		if (frame_res_outside == 0)
			frame_res_outside = resolved;
		return PassClass::bb_outside_bracket;
	}
	if (bb_inside == 0)
		first_inside_action = action;
	++bb_inside;
	if (frame_res == 0)
		frame_res = resolved;
	if (load_action(action) != LoadAction::load)
		return PassClass::bb_in_bracket_no_load;
	if (hud_seen)
		return PassClass::bb_in_bracket_later;
	hud_seen = true;
	return PassClass::bb_hud;
}

const char *frame_shape_name(FrameShape s)
{
	switch (s)
	{
	case FrameShape::ok: return "ok";
	case FrameShape::no_bracket: return "noBracket";
	case FrameShape::many_brackets: return "manyBrackets";
	case FrameShape::no_scene_pass: return "noScenePass";
	case FrameShape::no_hud: return "noHud";
	default: return "?";
	}
}

FrameShape judge_frame(const FrameLedger &f)
{
	if (f.brackets == 0)
		return FrameShape::no_bracket;
	if (f.brackets > 1)
		return FrameShape::many_brackets;
	if (f.bb_outside == 0)
		return FrameShape::no_scene_pass;
	if (!f.hud_seen)
		return FrameShape::no_hud;
	return FrameShape::ok;
}

// ---------------------------------------------------------------------------------------

bool PtrLatch::observe(std::uint64_t value)
{
	if (value == 0)
	{
		m_run = 0;
		return false;
	}
	if (m_latched)
		return false;
	if (m_run != 0 && value == m_value)
		++m_run;
	else
	{
		m_value = value;
		m_run = 1;
	}
	if (m_run >= kLatchAgreements)
	{
		m_latched = true;
		return true;
	}
	return false;
}

void PtrLatch::reset()
{
	m_latched = false;
	m_value = 0;
	m_run = 0;
}

// ---------------------------------------------------------------------------------------

Level level_from_int(int level)
{
	if (level <= 0)
		return Level::off;
	if (level == 1)
		return Level::observe;
	return Level::authoritative;
}

const char *level_name(Level l)
{
	switch (l)
	{
	case Level::off: return "off";
	case Level::observe: return "observe";
	case Level::authoritative: return "authoritative";
	default: return "?";
	}
}

const char *id_verdict_name(IdVerdict v)
{
	switch (v)
	{
	case IdVerdict::agree: return "agree";
	case IdVerdict::disagree: return "disagree";
	case IdVerdict::engine_absent: return "engineAbsent";
	case IdVerdict::not_candidate: return "notCandidate";
	case IdVerdict::other_absent: return "otherAbsent";
	default: return "?";
	}
}

namespace {

IdVerdict judge_one(const IdentityInputs &in, std::uint64_t other)
{
	if (in.engine_res == 0)
		return IdVerdict::engine_absent;
	if (!in.engine_is_candidate)
		return IdVerdict::not_candidate;
	if (other == 0)
		return IdVerdict::other_absent;
	return other == in.engine_res ? IdVerdict::agree : IdVerdict::disagree;
}

} // namespace

IdentityJudgement judge_identity(const IdentityInputs &in)
{
	IdentityJudgement j;
	j.vs_mirror = judge_one(in, in.mirror_res);
	j.vs_barrier = judge_one(in, in.barrier_res);
	return j;
}

Selection select_game_frame(Level level, const IdentityInputs &in)
{
	Selection s;
	s.res = in.mirror_res;
	if (level != Level::authoritative)
		return s;
	if (in.engine_res != 0 && in.engine_is_candidate)
	{
		s.source = FrameSource::engine;
		s.res = in.engine_res;
		return s;
	}
	s.fell_back = true;
	return s;
}

// ---------------------------------------------------------------------------------------

void StateLedger::set_tracked(const Candidates &c)
{
	m_tracked = c;
	for (unsigned i = 0; i < kMaxCandidates; ++i)
	{
		m_state[i] = kStatePresent;
		m_known[i] = false;
	}
	m_lists.clear();
	m_last_present = 0;
}

StateLedger::ListQueue *StateLedger::find(std::uint64_t list, bool create)
{
	for (ListQueue &q : m_lists)
		if (q.list == list)
			return &q;
	if (!create)
		return nullptr;
	ListQueue q;
	q.list = list;
	m_lists.push_back(q);
	return &m_lists.back();
}

void StateLedger::on_barrier(std::uint64_t list, std::uint64_t res, std::uint32_t before, std::uint32_t after)
{
	if (list == 0 || !m_tracked.contains(res))
		return;
	Event e;
	e.res = res;
	e.before = before;
	e.after = after;
	find(list, true)->events.push_back(e);
}

void StateLedger::on_marker(std::uint64_t list, std::uint64_t res, std::uint32_t assumed)
{
	if (list == 0 || !m_tracked.contains(res))
		return;
	Event e;
	e.res = res;
	e.before = assumed;
	e.after = assumed;
	e.marker = true;
	find(list, true)->events.push_back(e);
}

void StateLedger::on_reset(std::uint64_t list)
{
	if (ListQueue *q = find(list, false))
		q->events.clear();
}

ExecuteResult StateLedger::on_execute(std::uint64_t list)
{
	ExecuteResult r;
	ListQueue *q = find(list, false);
	if (q == nullptr)
		return r;
	for (const Event &e : q->events)
	{
		const int i = m_tracked.index_of(e.res);
		if (i < 0)
			continue;
		if (e.marker)
		{
			if (m_known[i] && m_state[i] == e.before)
				++r.markers_ok;
			else
				++r.markers_bad;
			continue;
		}
		if (m_known[i] && m_state[i] != e.before)
			++r.before_mismatch;
		m_state[i] = e.after;
		m_known[i] = true;
		++r.applied;
		if (e.after == kStatePresent)
		{
			r.present_res = e.res;
			m_last_present = e.res;
		}
	}
	q->events.clear();
	return r;
}

std::uint32_t StateLedger::state_of(std::uint64_t res, bool *known) const
{
	const int i = m_tracked.index_of(res);
	if (i < 0)
	{
		if (known != nullptr)
			*known = false;
		return kStatePresent;
	}
	if (known != nullptr)
		*known = m_known[i];
	return m_state[i];
}

const char *state_verdict_name(StateVerdict v)
{
	switch (v)
	{
	case StateVerdict::agree: return "agree";
	case StateVerdict::disagree: return "disagree";
	case StateVerdict::unknown: return "unknown";
	case StateVerdict::not_tracked: return "notTracked";
	default: return "?";
	}
}

StateVerdict judge_present_state(bool tracked, bool known, std::uint32_t recorded, std::uint32_t assumed)
{
	if (!tracked)
		return StateVerdict::not_tracked;
	if (!known)
		return StateVerdict::unknown;
	return recorded == assumed ? StateVerdict::agree : StateVerdict::disagree;
}

std::uint32_t present_state_to_use(Level level, bool tracked, bool known, std::uint32_t recorded, std::uint32_t assumed)
{
	if (level == Level::authoritative && tracked && known)
		return recorded;
	return assumed;
}

// ---------------------------------------------------------------------------------------

const char *copy_refusal_name(CopyRefusal r)
{
	switch (r)
	{
	case CopyRefusal::none: return "none";
	case CopyRefusal::level_off: return "levelOff";
	case CopyRefusal::not_hud_pass: return "notHudPass";
	case CopyRefusal::no_latch: return "noLatch";
	case CopyRefusal::shape_unproven: return "shapeUnproven";
	default: return "?";
	}
}

CopyRefusal decide_copy(const CopyInputs &in)
{
	if (in.level != Level::authoritative)
		return CopyRefusal::level_off;
	if (in.pass != PassClass::bb_hud)
		return CopyRefusal::not_hud_pass;
	if (!in.latched)
		return CopyRefusal::no_latch;
	if (in.shape_ok_streak < kShapeAgreements)
		return CopyRefusal::shape_unproven;
	return CopyRefusal::none;
}

} // namespace stray_dlss::fseam
