// The frame seams (src/core/frame_seams.hpp): the FRHIRenderPassInfo decode, the candidate
// set, the per-frame pass ledger and its shape verdict, the back-buffer latch, the identity
// judgement, the replayed state ledger and the HUD-less copy decision.
//
// As everywhere in this project the effort goes on REFUSALS: an engine answer outside the
// candidate set must never select a frame, a barrier recorded before a submitted list's own
// pending barrier must not be applied first, and the copy must never be taken before the
// frame's shape has proven itself.
#include "core/frame_seams.hpp"

#include <doctest/doctest.h>

#include <cstring>
#include <map>
#include <string>

using namespace stray_dlss::fseam;
using stray_dlss::u0::ObjectReader;

namespace {

struct FakeMemory
{
	std::map<std::uint64_t, std::uint64_t> qwords;
	static bool read(void *ctx, std::uint64_t va, std::uint64_t *out)
	{
		auto *m = static_cast<FakeMemory *>(ctx);
		const auto it = m->qwords.find(va);
		if (it == m->qwords.end())
			return false;
		*out = it->second;
		return true;
	}
	ObjectReader reader() { return ObjectReader{ &FakeMemory::read, this }; }
};

// Lays a FRHIRenderPassInfo's first colour entry into fake memory: RenderTarget at +0, and the
// {ArraySlice, MipIndex, Action} qword at +16 with Action at byte 21.
void lay_color0(FakeMemory &m, std::uint64_t info, std::uint64_t rt, std::uint8_t action, std::int32_t slice = -1, std::uint8_t mip = 0)
{
	m.qwords[info + 0] = rt;
	m.qwords[info + 8] = 0; // ResolveTarget
	std::uint64_t tail = static_cast<std::uint32_t>(slice);
	tail |= static_cast<std::uint64_t>(mip) << 32;
	tail |= static_cast<std::uint64_t>(action) << 40;
	m.qwords[info + 16] = tail;
}

} // namespace

TEST_CASE("frame_seams: ERenderTargetActions decodes as (Load << 2) | Store (RHIResources.h:2487)")
{
	CHECK(load_action(kActionLoadStore) == LoadAction::load);
	CHECK(load_action(kActionClearStore) == LoadAction::clear);
	CHECK(load_action(kActionDontLoadStore) == LoadAction::no_action);
	CHECK(kActionLoadStore == 5);
	CHECK(kActionClearStore == 9);
	CHECK(kActionDontLoadStore == 1);
	// Load_DontStore = (1<<2)|0 = 4 is still a LOAD; the store half does not matter here.
	CHECK(load_action(4) == LoadAction::load);
	CHECK(std::string(load_action_name(LoadAction::load)) == "Load");
	CHECK(std::string(load_action_name(LoadAction::invalid)) == "invalid");
}

TEST_CASE("frame_seams: the FColorEntry layout is 24 bytes with Action at +21, and the depth entry at 192")
{
	CHECK(kColorEntrySize == 24);
	CHECK(kColorEntryAction == 21);
	CHECK(kDepthStencilEntryOffset == 192);
	CHECK(kMaxSimultaneousRenderTargets == 8);
}

TEST_CASE("decode_color0: reads RenderTarget and Action, refuses an unreadable info")
{
	FakeMemory m;
	const std::uint64_t info = 0x7000;
	lay_color0(m, info, 0xABCDEF00ull, kActionLoadStore, -1, 0);
	Color0 c;
	REQUIRE(decode_color0(m.reader(), info, &c));
	CHECK(c.render_target == 0xABCDEF00ull);
	CHECK(c.action == kActionLoadStore);
	// A different mip/slice must not leak into the action byte.
	lay_color0(m, info, 0x1234ull, kActionClearStore, 3, 5);
	REQUIRE(decode_color0(m.reader(), info, &c));
	CHECK(c.render_target == 0x1234ull);
	CHECK(c.action == kActionClearStore);
	CHECK_FALSE(decode_color0(m.reader(), 0x9000, &c));
	CHECK_FALSE(decode_color0(m.reader(), 0, &c));
	ObjectReader none;
	CHECK_FALSE(decode_color0(none, info, &c));
}

TEST_CASE("Candidates: set / contains / index_of, zero ids dropped, capacity bounded")
{
	Candidates c;
	const std::uint64_t ids[] = { 0x10, 0, 0x20, 0x30 };
	c.set(ids, 4, 7);
	CHECK(c.count == 3);
	CHECK(c.epoch == 7);
	CHECK(c.contains(0x10));
	CHECK(c.contains(0x30));
	CHECK_FALSE(c.contains(0));
	CHECK_FALSE(c.contains(0x40));
	CHECK(c.index_of(0x20) == 1);
	CHECK(c.index_of(0x99) == -1);
	std::uint64_t many[kMaxCandidates + 4];
	for (unsigned i = 0; i < kMaxCandidates + 4; ++i)
		many[i] = 0x1000 + i;
	c.set(many, kMaxCandidates + 4, 8);
	CHECK(c.count == kMaxCandidates);
}

TEST_CASE("FrameLedger: scene passes before the bracket, the first LOAD inside it is the HUD pass, later ones are not")
{
	FrameLedger f;
	// Tonemap onto the back buffer, before Slate's bracket.
	CHECK(f.classify(true, 0xBB, kActionClearStore) == PassClass::bb_outside_bracket);
	CHECK(f.classify(false, 0, kActionLoadStore) == PassClass::not_back_buffer);
	f.begin_bracket();
	CHECK(f.in_bracket);
	CHECK(f.brackets == 1);
	// SlateBatches: Load_Store on the back buffer, inside the bracket -> the HUD pass.
	CHECK(f.classify(true, 0xBB, kActionLoadStore) == PassClass::bb_hud);
	CHECK(f.hud_seen);
	CHECK(f.frame_res == 0xBB);
	CHECK(f.first_inside_action == kActionLoadStore);
	// A second LOAD pass on the back buffer inside the bracket (a composition pass) is not.
	CHECK(f.classify(true, 0xBB, kActionLoadStore) == PassClass::bb_in_bracket_later);
	f.end_bracket();
	CHECK_FALSE(f.in_bracket);
	CHECK(f.bb_outside == 1);
	CHECK(f.bb_inside == 2);
	CHECK(judge_frame(f) == FrameShape::ok);
	f.reset();
	CHECK(f.brackets == 0);
	CHECK_FALSE(f.hud_seen);
}

TEST_CASE("FrameLedger: Slate's Clear path inside the bracket is NOT the HUD pass; a frame with no LOAD reads no_hud")
{
	FrameLedger f;
	f.classify(true, 0xBB, kActionLoadStore); // the tonemapper may LOAD too (ScreenPass output): outside the bracket it is a scene pass
	f.begin_bracket();
	CHECK(f.classify(true, 0xBB, kActionClearStore) == PassClass::bb_in_bracket_no_load);
	CHECK_FALSE(f.hud_seen);
	f.end_bracket();
	CHECK(judge_frame(f) == FrameShape::no_hud);
}

TEST_CASE("judge_frame: every departure from the predicted shape is named")
{
	FrameLedger f;
	CHECK(judge_frame(f) == FrameShape::no_bracket);
	f.begin_bracket();
	f.end_bracket();
	CHECK(judge_frame(f) == FrameShape::no_scene_pass); // no back-buffer pass before the bracket
	FrameLedger g;
	g.classify(true, 0xBB, kActionClearStore);
	g.begin_bracket();
	g.end_bracket();
	g.begin_bracket();
	g.end_bracket();
	CHECK(judge_frame(g) == FrameShape::many_brackets);
	CHECK(std::string(frame_shape_name(FrameShape::no_scene_pass)) == "noScenePass");
	CHECK(std::string(pass_class_name(PassClass::bb_hud)) == "bbHud");
}

TEST_CASE("PtrLatch: closes after kLatchAgreements agreeing observations; a change restarts; zero breaks the run")
{
	PtrLatch l;
	CHECK_FALSE(l.observe(0xA));
	CHECK_FALSE(l.observe(0xA));
	CHECK(l.observe(0xA));
	CHECK(l.latched());
	CHECK(l.value() == 0xA);
	CHECK_FALSE(l.observe(0xA)); // already latched: no second "closed" edge
	l.reset();
	CHECK_FALSE(l.latched());
	CHECK_FALSE(l.observe(0xA));
	CHECK_FALSE(l.observe(0xB)); // disagreement restarts at 1
	CHECK_FALSE(l.observe(0xB));
	CHECK(l.observe(0xB));
	PtrLatch z;
	z.observe(0xC);
	z.observe(0);
	CHECK_FALSE(z.observe(0xC));
	CHECK_FALSE(z.observe(0xC));
	CHECK(z.observe(0xC));
}

TEST_CASE("level_from_int: 0 off, 1 observe, >= 2 authoritative")
{
	CHECK(level_from_int(-1) == Level::off);
	CHECK(level_from_int(0) == Level::off);
	CHECK(level_from_int(1) == Level::observe);
	CHECK(level_from_int(2) == Level::authoritative);
	CHECK(level_from_int(9) == Level::authoritative);
	CHECK(std::string(level_name(Level::observe)) == "observe");
}

TEST_CASE("judge_identity: agree / disagree against the mirror and the PRESENT barrier, and every absence named")
{
	IdentityInputs in;
	in.engine_res = 0xBB;
	in.engine_is_candidate = true;
	in.mirror_res = 0xBB;
	in.barrier_res = 0xBB;
	IdentityJudgement j = judge_identity(in);
	CHECK(j.vs_mirror == IdVerdict::agree);
	CHECK(j.vs_barrier == IdVerdict::agree);

	in.mirror_res = 0xCC;
	j = judge_identity(in);
	CHECK(j.vs_mirror == IdVerdict::disagree);
	CHECK(j.vs_barrier == IdVerdict::agree);

	in.barrier_res = 0;
	j = judge_identity(in);
	CHECK(j.vs_barrier == IdVerdict::other_absent);

	in.engine_is_candidate = false;
	j = judge_identity(in);
	CHECK(j.vs_mirror == IdVerdict::not_candidate);
	CHECK(j.vs_barrier == IdVerdict::not_candidate);

	in.engine_res = 0;
	j = judge_identity(in);
	CHECK(j.vs_mirror == IdVerdict::engine_absent);
	CHECK(std::string(id_verdict_name(IdVerdict::not_candidate)) == "notCandidate");
}

TEST_CASE("select_game_frame: the mirror below level 2; the engine at level 2 when it answered inside the candidate set; else the mirror, COUNTED as a fallback")
{
	IdentityInputs in;
	in.engine_res = 0xBB;
	in.engine_is_candidate = true;
	in.mirror_res = 0xCC;
	Selection s = select_game_frame(Level::observe, in);
	CHECK(s.source == FrameSource::mirror);
	CHECK(s.res == 0xCC);
	CHECK_FALSE(s.fell_back);

	s = select_game_frame(Level::authoritative, in);
	CHECK(s.source == FrameSource::engine);
	CHECK(s.res == 0xBB);
	CHECK_FALSE(s.fell_back);

	// An engine answer OUTSIDE the candidate set never selects: refused, and the mirror is used.
	in.engine_is_candidate = false;
	s = select_game_frame(Level::authoritative, in);
	CHECK(s.source == FrameSource::mirror);
	CHECK(s.res == 0xCC);
	CHECK(s.fell_back);

	in.engine_res = 0;
	s = select_game_frame(Level::authoritative, in);
	CHECK(s.fell_back);
	CHECK(s.res == 0xCC);
}

TEST_CASE("StateLedger: barriers are queued per list and applied at execute, in EXECUTE order — UE's pending-barrier list executes first")
{
	Candidates c;
	const std::uint64_t ids[] = { 0xBB0, 0xBB1, 0xBB2 };
	c.set(ids, 3, 1);
	StateLedger l;
	l.set_tracked(c);
	bool known = true;
	CHECK(l.state_of(0xBB0, &known) == kStatePresent);
	CHECK_FALSE(known);

	// Frame: the main list A records the HUD-less marker (assuming RT), then RT -> PRESENT from
	// FD3D12Viewport::Present. At submission the engine records PRESENT -> RT onto a separate
	// barrier list B and executes B, then A. Recording order is A's events first, B's second;
	// GPU order is B then A. A recording-order ledger would end at RT; this one ends at PRESENT.
	const std::uint64_t A = 0xA, B = 0xB;
	l.on_marker(A, 0xBB0, kStateRenderTarget);
	l.on_barrier(A, 0xBB0, kStateRenderTarget, kStatePresent);
	l.on_barrier(B, 0xBB0, kStatePresent, kStateRenderTarget);
	CHECK(l.pending_lists() == 2);

	ExecuteResult rb = l.on_execute(B);
	CHECK(rb.applied == 1);
	CHECK(rb.before_mismatch == 0); // unknown before: not a mismatch
	CHECK(l.state_of(0xBB0, &known) == kStateRenderTarget);
	CHECK(known);

	ExecuteResult ra = l.on_execute(A);
	CHECK(ra.applied == 1);
	CHECK(ra.markers_ok == 1);
	CHECK(ra.markers_bad == 0);
	CHECK(ra.before_mismatch == 0);
	CHECK(ra.present_res == 0xBB0);
	CHECK(l.state_of(0xBB0, &known) == kStatePresent);
	CHECK(l.last_present_resource() == 0xBB0);
	// Executed queues are drained.
	CHECK(l.on_execute(A).applied == 0);
}

TEST_CASE("StateLedger: a StateBefore that disagrees with the ledger is counted; a marker whose assumption was wrong is counted; untracked resources are ignored; Reset drops the queue")
{
	Candidates c;
	const std::uint64_t ids[] = { 0xBB0 };
	c.set(ids, 1, 1);
	StateLedger l;
	l.set_tracked(c);
	const std::uint64_t A = 0xA;
	l.on_barrier(A, 0xBB0, kStatePresent, kStateRenderTarget);
	l.on_barrier(A, 0x999, kStatePresent, kStateRenderTarget); // not tracked: dropped
	CHECK(l.on_execute(A).applied == 1);
	// The engine now says "before = COPY_SOURCE" while we hold RENDER_TARGET: an assertion fires.
	l.on_barrier(A, 0xBB0, kStateCopySource, kStatePresent);
	ExecuteResult r = l.on_execute(A);
	CHECK(r.before_mismatch == 1);
	CHECK(r.applied == 1);
	CHECK(r.present_res == 0xBB0);
	// A marker assuming RT while the ledger holds PRESENT is a bad marker.
	l.on_marker(A, 0xBB0, kStateRenderTarget);
	r = l.on_execute(A);
	CHECK(r.markers_bad == 1);
	CHECK(r.markers_ok == 0);
	// Reset discards whatever was queued on the list.
	l.on_barrier(A, 0xBB0, kStatePresent, kStateRenderTarget);
	l.on_reset(A);
	CHECK(l.on_execute(A).applied == 0);
	bool known = false;
	CHECK(l.state_of(0xBB0, &known) == kStatePresent);
	CHECK(known);
	// A list nobody recorded on executes to nothing.
	CHECK(l.on_execute(0xF).applied == 0);
	// set_tracked forgets everything.
	l.set_tracked(c);
	CHECK(l.state_of(0xBB0, &known) == kStatePresent);
	CHECK_FALSE(known);
	CHECK(l.last_present_resource() == 0);
}

TEST_CASE("judge_present_state / present_state_to_use: the assumed constant below level 2 and whenever the ledger cannot answer")
{
	CHECK(judge_present_state(false, false, 0, 0) == StateVerdict::not_tracked);
	CHECK(judge_present_state(true, false, 0, 0) == StateVerdict::unknown);
	CHECK(judge_present_state(true, true, kStatePresent, kStatePresent) == StateVerdict::agree);
	CHECK(judge_present_state(true, true, kStateRenderTarget, kStatePresent) == StateVerdict::disagree);
	CHECK(present_state_to_use(Level::observe, true, true, kStateRenderTarget, kStatePresent) == kStatePresent);
	CHECK(present_state_to_use(Level::authoritative, true, true, kStateRenderTarget, kStatePresent) == kStateRenderTarget);
	CHECK(present_state_to_use(Level::authoritative, true, false, kStateRenderTarget, kStatePresent) == kStatePresent);
	CHECK(present_state_to_use(Level::authoritative, false, true, kStateRenderTarget, kStatePresent) == kStatePresent);
	CHECK(std::string(state_verdict_name(StateVerdict::disagree)) == "disagree");
}

TEST_CASE("decide_copy: only at level 2, only on the HUD pass, only once latched, only after the shape has held kShapeAgreements frames")
{
	CopyInputs in;
	in.level = Level::authoritative;
	in.pass = PassClass::bb_hud;
	in.latched = true;
	in.shape_ok_streak = kShapeAgreements;
	CHECK(decide_copy(in) == CopyRefusal::none);
	in.level = Level::observe;
	CHECK(decide_copy(in) == CopyRefusal::level_off);
	in.level = Level::authoritative;
	in.pass = PassClass::bb_in_bracket_later;
	CHECK(decide_copy(in) == CopyRefusal::not_hud_pass);
	in.pass = PassClass::bb_outside_bracket;
	CHECK(decide_copy(in) == CopyRefusal::not_hud_pass);
	in.pass = PassClass::bb_hud;
	in.latched = false;
	CHECK(decide_copy(in) == CopyRefusal::no_latch);
	in.latched = true;
	in.shape_ok_streak = kShapeAgreements - 1;
	CHECK(decide_copy(in) == CopyRefusal::shape_unproven);
	CHECK(std::string(copy_refusal_name(CopyRefusal::shape_unproven)) == "shapeUnproven");
}

TEST_CASE("frame_seams: D3D12 state constants are d3d12.h's (PRESENT == COMMON == 0)")
{
	CHECK(kStatePresent == 0);
	CHECK(kStateRenderTarget == 0x4);
	CHECK(kStateCopyDest == 0x400);
	CHECK(kStateCopySource == 0x800);
}
