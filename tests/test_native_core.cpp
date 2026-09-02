#include "app/diff_observer.hpp"
#include "core/heap_math.hpp"
#include "core/root_signature_walk.hpp"
#include "core/present_plan.hpp"
#include "core/va_map.hpp"

#include <doctest/doctest.h>

#include <string>

using namespace stray_dlss;

TEST_CASE("VaMap: end-exclusive lookup, offsets, and placed-resource overwrite at the same start")
{
	core::VaMap m;
	m.insert(0x1000, 0x800, 1);
	m.insert(0x4000, 0x100, 2);
	std::uint64_t id = 0, off = 0;
	CHECK(m.find(0x1234, id, off)); CHECK(id == 1); CHECK(off == 0x234);
	CHECK(m.find(0x1000, id, off)); CHECK(id == 1); CHECK(off == 0);
	CHECK_FALSE(m.find(0x1800, id, off)); // end is exclusive
	CHECK_FALSE(m.find(0x0fff, id, off));
	CHECK(m.find(0x4000, id, off)); CHECK(id == 2); CHECK(off == 0);
	CHECK(m.find(0x40ff, id, off)); CHECK(id == 2);
	CHECK_FALSE(m.find(0x4100, id, off));
	// A placed resource re-created over the same range replaces the old entry (insert_or_assign).
	m.insert(0x1000, 0x10, 3);
	CHECK(m.find(0x1004, id, off)); CHECK(id == 3);
	CHECK_FALSE(m.find(0x1234, id, off)); // the old, larger range is gone with its entry
	m.erase(3);
	CHECK_FALSE(m.find(0x1004, id, off));
	CHECK(m.size() == 1);
	m.insert(0x9000, 0, 9); // zero-sized: ignored
	CHECK(m.size() == 1);
}

TEST_CASE("heap math: GPU handle -> CPU handle within one heap, and the rejections")
{
	core::HeapSpan h;
	h.cpu_base = 0x100; h.gpu_base = 0x9000; h.increment = 32; h.count = 8;
	std::uint64_t cpu = 0;
	CHECK(core::gpu_to_cpu(0x9000 + 3 * 32, h, cpu)); CHECK(cpu == 0x100 + 3 * 32);
	CHECK(core::gpu_to_cpu(0x9000, h, cpu)); CHECK(cpu == 0x100);
	CHECK_FALSE(core::gpu_to_cpu(0x9000 + 8 * 32, h, cpu)); // past the end
	CHECK_FALSE(core::gpu_to_cpu(0x9000 + 5, h, cpu));      // unaligned
	CHECK_FALSE(core::gpu_to_cpu(0x8fff, h, cpu));          // below the base
	std::uint32_t index = 0;
	CHECK(core::cpu_to_index(0x100 + 7 * 32, h, index)); CHECK(index == 7);
	CHECK_FALSE(core::cpu_to_index(0x100 + 8 * 32, h, index));
	core::HeapSpan bad;
	CHECK_FALSE(core::gpu_to_cpu(0, bad, cpu));
}

TEST_CASE("root-signature walk: UE4's compute shape expands to registers; APPEND and unbounded ranges")
{
	core::RootLayout l;
	// [0] table { SRV t0..t5 }, [1] root CBV b1, [2] table { UAV u0..u1 }, [3] constants b0 x4
	core::RootParam p0; p0.kind = core::ParamKind::table;
	p0.ranges.push_back(core::RootRange{ core::RangeKind::srv, 0, 6, 0, 0 });
	core::RootParam p1; p1.kind = core::ParamKind::cbv; p1.shader_register = 1;
	core::RootParam p2; p2.kind = core::ParamKind::table;
	p2.ranges.push_back(core::RootRange{ core::RangeKind::uav, 0, 2, 0, core::kRangeOffsetAppend });
	core::RootParam p3; p3.kind = core::ParamKind::constants; p3.shader_register = 0; p3.num_32bit_values = 4;
	l.params = { p0, p1, p2, p3 };

	const core::WalkResult w = core::walk_tables(l);
	REQUIRE(w.slots.size() == 8);
	for (std::uint32_t i = 0; i < 6; ++i)
	{
		CHECK(w.slots[i].param == 0);
		CHECK(w.slots[i].table_index == i);
		CHECK(w.slots[i].kind == core::RangeKind::srv);
		CHECK(w.slots[i].reg == i);
	}
	CHECK(w.slots[6].param == 2); CHECK(w.slots[6].table_index == 0); CHECK(w.slots[6].kind == core::RangeKind::uav); CHECK(w.slots[6].reg == 0);
	CHECK(w.slots[7].reg == 1);
	CHECK(w.unbounded_ranges == 0);
	CHECK(l.params[1].kind == core::ParamKind::cbv);
	CHECK(l.params[3].kind == core::ParamKind::constants);
	CHECK(l.params[3].num_32bit_values == 4);

	// A table of three ranges: explicit offset, APPEND after it, then a CBV range at an
	// explicit gap. APPEND follows the PREVIOUS range, D3D12's rule.
	core::RootLayout l2;
	core::RootParam t; t.kind = core::ParamKind::table;
	t.ranges.push_back(core::RootRange{ core::RangeKind::srv, 2, 2, 0, 4 });                     // slots 4,5 -> t2,t3
	t.ranges.push_back(core::RootRange{ core::RangeKind::uav, 0, 1, 0, core::kRangeOffsetAppend }); // slot 6 -> u0
	t.ranges.push_back(core::RootRange{ core::RangeKind::cbv, 4, 1, 0, 10 });                    // slot 10 -> b4
	l2.params = { t };
	core::WalkResult w2;
	CHECK(core::walk_table(l2, 0, w2));
	REQUIRE(w2.slots.size() == 4);
	CHECK(w2.slots[0].table_index == 4); CHECK(w2.slots[0].reg == 2);
	CHECK(w2.slots[1].table_index == 5); CHECK(w2.slots[1].reg == 3);
	CHECK(w2.slots[2].table_index == 6); CHECK(w2.slots[2].kind == core::RangeKind::uav); CHECK(w2.slots[2].reg == 0);
	CHECK(w2.slots[3].table_index == 10); CHECK(w2.slots[3].kind == core::RangeKind::cbv); CHECK(w2.slots[3].reg == 4);
	CHECK_FALSE(core::walk_table(l2, 1, w2)); // no such param
	CHECK_FALSE(core::walk_table(l, 1, w2));  // a root CBV is not a table

	// Unbounded: reported, not expanded.
	core::RootLayout l3;
	core::RootParam u; u.kind = core::ParamKind::table;
	u.ranges.push_back(core::RootRange{ core::RangeKind::srv, 0, 1, 0, 0 });
	u.ranges.push_back(core::RootRange{ core::RangeKind::srv, 1, core::kUnboundedCount, 0, core::kRangeOffsetAppend });
	l3.params = { u };
	const core::WalkResult w3 = core::walk_tables(l3);
	CHECK(w3.slots.size() == 1);
	CHECK(w3.unbounded_ranges == 1);
}

TEST_CASE("diff: identical inputs agree; one format difference is one mismatch naming the slot; a missing slot is unknown")
{
	icept::DispatchBindings a, b;
	a.srvs.push_back(BoundTexture{ 0, 0x10, TexFormat::r32g32b32a32_float, 1, 1, 0xa0 });
	a.srvs.push_back(BoundTexture{ 1, 0x11, TexFormat::r16g16b16a16_float, 1920, 1080, 0xa1 });
	a.srvs.push_back(BoundTexture{ 2, 0x12, TexFormat::r32_float_x8x24_typeless, 1920, 1080, 0xa2 });
	a.srvs.push_back(BoundTexture{ 4, 0x12, TexFormat::x32_typeless_g8x24_uint, 1920, 1080, 0xa4 });
	a.uavs.push_back(BoundTexture{ 0, 0x20, TexFormat::r16g16b16a16_float, 3840, 2160, 0xb0 });
	a.constant_buffers.emplace_back(1u, icept::BufferRange{ 0x99, 256, icept::kUnknownSize });
	a.constant_buffers.emplace_back(2u, icept::BufferRange{ 0x99, 4096, icept::kUnknownSize });
	a.view_cb = icept::BufferRange{ 0x77, 0, 2448 }; a.view_cb_valid = true; a.view_cb_register = 4;
	a.heaps[0] = reinterpret_cast<ID3D12DescriptorHeap *>(0x5000); a.heap_count = 1;
	b = a;
	// The oracle keys root CBVs by root parameter, the native side may key differently.
	b.constant_buffers[0].first = 7;
	// Descriptor handles are not compared: the same slot resolved from two handle spaces.
	b.srvs[1].descriptor = 0xdead;
	CHECK(diff::compare(a, b).agree());

	// One format difference on t1.
	icept::DispatchBindings c = a;
	c.srvs[1].format = TexFormat::r11g11b10_float;
	diff::Result r = diff::compare(a, c);
	REQUIRE(r.mismatches.size() == 1);
	CHECK(r.mismatches[0].rfind("t1:", 0) == 0);
	CHECK(r.unknown.empty());
	CHECK(r.extra.empty());

	// A slot the native side could not resolve is UNKNOWN, not a mismatch; an extra one is EXTRA.
	icept::DispatchBindings d = a;
	d.srvs.erase(d.srvs.begin() + 2);
	d.uavs.push_back(BoundTexture{ 1, 0x21, TexFormat::r16g16b16a16_float, 1920, 1080, 0xb1 });
	r = diff::compare(a, d);
	CHECK(r.mismatches.empty());
	REQUIRE(r.unknown.size() == 1);
	CHECK(r.unknown[0].rfind("t2:", 0) == 0);
	REQUIRE(r.extra.size() == 1);
	CHECK(r.extra[0].rfind("u1:", 0) == 0);

	// Constant buffers compare as a multiset of (buffer, offset); the View CB and the heaps too.
	icept::DispatchBindings e = a;
	e.constant_buffers.pop_back();
	e.view_cb_register = 5;
	e.heaps[0] = reinterpret_cast<ID3D12DescriptorHeap *>(0x6000);
	r = diff::compare(a, e);
	CHECK(r.unknown.size() == 1); // the missing cb
	CHECK(r.heap_identity.size() == 1); // the heap differs, reported apart, not a disagreement
	CHECK(r.mismatches.size() == 1); // view_cb register
	CHECK(r.mismatches[0].rfind("view_cb:", 0) == 0);
}

TEST_CASE("diff live machinery: publish/consume is per thread and per list, and counts")
{
	diff::set_enabled(true);
	icept::DispatchBindings a;
	a.srvs.push_back(BoundTexture{ 0, 0x10, TexFormat::r16g16b16a16_float, 8, 8, 0xa0 });
	int list_a = 0, list_b = 0;
	const diff::Summary before = diff::summary();
	// Nothing published: not observed.
	CHECK_FALSE(diff::consume_and_compare(&list_a, a, 0));
	diff::publish_expected(&list_a, 0x901e041a7cadc9db, 480, 270, a);
	// The wrong list does not consume it.
	CHECK_FALSE(diff::consume_and_compare(&list_b, a, 0));
	CHECK(diff::consume_and_compare(&list_a, a, 0));
	CHECK_FALSE(diff::consume_and_compare(&list_a, a, 0)); // consumed once
	icept::DispatchBindings wrong = a;
	wrong.srvs[0].width = 9;
	diff::publish_expected(&list_a, 0x1234, 40, 20, a);
	CHECK(diff::consume_and_compare(&list_a, wrong, 3));
	// A publish over an unconsumed one counts as unconsumed.
	diff::publish_expected(&list_a, 1, 40, 20, a);
	diff::publish_expected(&list_a, 2, 40, 20, a);
	CHECK(diff::consume_and_compare(&list_a, a, 3));
	const diff::Summary after = diff::summary();
	CHECK(after.dispatches - before.dispatches == 3);
	CHECK(after.agree - before.agree == 2);
	CHECK(after.mismatch - before.mismatch == 1);
	CHECK(after.taa_dispatches - before.taa_dispatches == 1);
	CHECK(after.taa_disagree - before.taa_disagree == 0);
	CHECK(after.unconsumed - before.unconsumed == 1);
	diff::set_enabled(false);
	diff::publish_expected(&list_a, 1, 40, 20, a);
	CHECK_FALSE(diff::consume_and_compare(&list_a, a, 0)); // disabled: inert
}

TEST_CASE("VaMap: a placed buffer nested inside a bigger one, and a sub-allocated ring resolve by RANGE")
{
	// UE4's constant/upload ring is ONE big buffer that root CBVs address at arbitrary interior
	// offsets (run F: offsets up to +5505024 into one resource), and placed resources can sit
	// INSIDE another buffer's VA range on the same heap. A lookup that only inspects the range
	// starting at or before the address (ReShade's shape) misses the outer buffer whenever an
	// inner one starts between them.
	core::VaMap m;
	m.insert(0x100000, 8u << 20, 1); // an 8 MB ring
	m.insert(0x102000, 0x100, 2);    // a small placed buffer inside it
	std::uint64_t id = 0, off = 0;
	CHECK(m.find(0x102050, id, off)); CHECK(id == 2); CHECK(off == 0x50);  // the innermost wins
	CHECK(m.find(0x103000, id, off)); CHECK(id == 1); CHECK(off == 0x3000); // past the inner one: the ring
	CHECK(m.find(0x100000 + 5505024, id, off)); CHECK(id == 1); CHECK(off == 5505024);
	CHECK(m.find(0x100000 + (8u << 20) - 1, id, off)); CHECK(id == 1);
	CHECK_FALSE(m.find(0x100000 + (8u << 20), id, off));
	m.erase(2);
	CHECK(m.find(0x102050, id, off)); CHECK(id == 1); CHECK(off == 0x2050);
	// Two rings side by side, the second registered lazily AFTER an interior address was
	// already being used: only the range matters, never the registration order.
	m.insert(0x900000, 8u << 20, 3);
	CHECK(m.find(0x900000 + 4706816, id, off)); CHECK(id == 3); CHECK(off == 4706816);
	CHECK(m.find(0x100000 + 4706816, id, off)); CHECK(id == 1);
}

TEST_CASE("diff adjudication: a stale oracle map is convicted by its own liveness, and the classes are named")
{
	// Liveness as the two trackers would answer it.
	//   0xdead: ReShade's destroy_resource fired (live=0) but its view->resource map still names
	//           it (CLAUDE.md §5); the registry saw it die too.            -> RESHADE-STALE
	//   0x7777: live per ReShade, NEVER registered by the registry.          -> NATIVE-BLIND
	//   0xbeef: dead per ReShade, LIVE per the registry's sentinel (the runtime's word):
	//           ReShade's event-fed liveness never carried it.        -> RESHADE-LIVENESS-GAP
	//   0xcafe: live per ReShade, seen by the registry but its sentinel FIRED. -> LIVENESS-CONFLICT
	//   everything else: live and seen on both sides.
	static const auto oracle_live = [](icept::ResourceId r) { return r != 0xdead && r != 0xbeef; };
	static const auto native_live = [](icept::ResourceId r) { return r != 0xdead && r != 0x7777 && r != 0xcafe; };
	static const auto native_seen = [](icept::ResourceId r) { return r != 0x7777; };
	diff::Adjudicator adj;
	adj.oracle_live = oracle_live;
	adj.native_live = native_live;
	adj.native_seen = native_seen;

	icept::DispatchBindings oracle, native;
	oracle.srvs.push_back(BoundTexture{ 6, 0xdead, TexFormat::unknown, 0, 0, 0xa6 });  // stale in ReShade
	native.srvs.push_back(BoundTexture{ 6, 0x1111, TexFormat::unknown, 0, 0, 0xa6 });
	oracle.srvs.push_back(BoundTexture{ 8, 0x2222, TexFormat::unknown, 0, 0, 0xa8 });  // both live, differ
	native.srvs.push_back(BoundTexture{ 8, 0x3333, TexFormat::unknown, 0, 0, 0xa8 });
	oracle.srvs.push_back(BoundTexture{ 9, 0x4444, TexFormat::unknown, 0, 0, 0xa9 });  // native has nothing
	oracle.srvs.push_back(BoundTexture{ 10, 0x7777, TexFormat::unknown, 0, 0, 0xaa }); // the registry never saw it
	native.uavs.push_back(BoundTexture{ 2, 0x5555, TexFormat::unknown, 0, 0, 0xb2 });  // oracle has nothing
	oracle.uavs.push_back(BoundTexture{ 3, 0x6666, TexFormat::unknown, 0, 0, 0xb3 });
	native.uavs.push_back(BoundTexture{ 3, 0xbeef, TexFormat::unknown, 0, 0, 0xb3 });  // native names a ReShade-dead, sentinel-live one
	oracle.uavs.push_back(BoundTexture{ 4, 0xcafe, TexFormat::unknown, 0, 0, 0xb4 });  // oracle names one the sentinel saw die
	native.uavs.push_back(BoundTexture{ 4, 0x1234, TexFormat::unknown, 0, 0, 0xb4 });
	oracle.constant_buffers.emplace_back(1u, icept::BufferRange{ 0xdead, 4096, icept::kUnknownSize });

	const diff::Result r = diff::compare(oracle, native, &adj);
	REQUIRE(r.mismatches.size() == 4);
	REQUIRE(r.unknown.size() == 3);
	REQUIRE(r.extra.size() == 1);
	CHECK(r.mismatches[0].find("=> RESHADE-STALE") != std::string::npos);
	CHECK(r.mismatches[0].find("oracle-res live rs=0 reg=0 seen=1") != std::string::npos);
	CHECK(r.mismatches[1].find("=> BOTH-LIVE") != std::string::npos);
	CHECK(r.mismatches[2].find("=> RESHADE-LIVENESS-GAP") != std::string::npos); // 0xbeef
	CHECK(r.mismatches[3].find("=> LIVENESS-CONFLICT") != std::string::npos);   // 0xcafe
	CHECK(r.unknown[0].find("=> NATIVE-MISSED") != std::string::npos);
	CHECK(r.unknown[1].find("=> NATIVE-BLIND") != std::string::npos);
	CHECK(r.unknown[2].rfind("cb:", 0) == 0);
	CHECK(r.unknown[2].find("=> RESHADE-STALE") != std::string::npos); // the cb into a dead buffer
	CHECK(r.extra[0].find("=> ORACLE-MISSED") != std::string::npos);
	CHECK(r.verdicts[static_cast<int>(diff::Verdict::reshade_stale)] == 2);
	CHECK(r.verdicts[static_cast<int>(diff::Verdict::native_blind)] == 1);
	CHECK(r.verdicts[static_cast<int>(diff::Verdict::both_live)] == 1);
	CHECK(r.verdicts[static_cast<int>(diff::Verdict::liveness_conflict)] == 1);
	CHECK(r.verdicts[static_cast<int>(diff::Verdict::reshade_liveness_gap)] == 1);
	CHECK(r.verdicts[static_cast<int>(diff::Verdict::native_missed)] == 1);
	CHECK(r.verdicts[static_cast<int>(diff::Verdict::oracle_missed)] == 1);
	CHECK(r.verdicts[static_cast<int>(diff::Verdict::unadjudicated)] == 0);
	// The refinements. A tombstone naming the oracle's resource convicts the oracle: the
	// resource died after the descriptor was written and its address was reused (both
	// liveness trackers say "live" — of the NEW resource). A slot never written since attach
	// is our blind spot. A copied slot whose SOURCE ReShade itself maps to the native answer
	// convicts ReShade's copy bookkeeping.
	static const auto all_live = [](icept::ResourceId) { return true; };
	static const auto view_map = [](icept::DescriptorId v) -> icept::ResourceId { return v == 0xa000 ? 0x3333 : v == 0xa001 ? 0x2222 : 0; };
	// The native shadow's entries: online slots b8xx copied from offline sources a00x; the
	// source a001 was RE-CREATED for 0x2222 at seq 50, after the copy at seq 10.
	static const auto slot_info = [](icept::DescriptorId slot, icept::ResourceId &res, std::uint64_t &seq, bool &copy,
	                                 icept::DescriptorId &src, bool &dead) {
		dead = false;
		if (slot == 0xb800) { res = 0x3333; seq = 10; copy = true; src = 0xa000; return true; }  // source agrees with native (ReShade's map)
		if (slot == 0xb801) { res = 0x3333; seq = 10; copy = true; src = 0xa001; return true; }  // source re-created for the oracle's resource
		if (slot == 0xb802) { res = 0x3333; seq = 10; copy = false; src = 0; return true; }
		if (slot == 0xa000) { res = 0x3333; seq = 5; copy = false; src = 0; return true; }
		if (slot == 0xa001) { res = 0x2222; seq = 50; copy = false; src = 0; return true; }
		if (slot == 0xc011) { res = 0x9999; seq = 10; copy = true; src = 0xa001; dead = true; return true; } // tombstone copied from the re-created source
		return false;
	};
	diff::Adjudicator full;
	full.oracle_live = all_live; full.native_live = all_live; full.native_seen = all_live;
	full.oracle_view_resource = view_map; full.native_slot = slot_info;
	icept::DispatchBindings o2, n2;
	o2.srvs.push_back(BoundTexture{ 1, 0x2222, TexFormat::unknown, 0, 0, 0xa001 });
	n2.srvs.push_back(BoundTexture{ 1, 0x3333, TexFormat::unknown, 0, 0, 0xb800 }); // copied from a slot ReShade maps to 0x3333
	o2.srvs.push_back(BoundTexture{ 2, 0x2222, TexFormat::unknown, 0, 0, 0xa001 });
	n2.srvs.push_back(BoundTexture{ 2, 0x3333, TexFormat::unknown, 0, 0, 0xb801 }); // copied from a slot ReShade maps to 0x2222
	o2.srvs.push_back(BoundTexture{ 3, 0x2222, TexFormat::unknown, 0, 0, 0xa001 });
	n2.srvs.push_back(BoundTexture{ 3, 0x3333, TexFormat::unknown, 0, 0, 0xb802 }); // written by a view creation
	o2.srvs.push_back(BoundTexture{ 10, 0x8144, TexFormat::unknown, 0, 0, 0xa002 }); // native: tombstone naming 0x8144
	o2.srvs.push_back(BoundTexture{ 11, 0x2222, TexFormat::unknown, 0, 0, 0xa001 }); // native: tombstone whose source was re-created for 0x2222
	o2.srvs.push_back(BoundTexture{ 12, 0x8146, TexFormat::unknown, 0, 0, 0xa004 }); // native: never written
	o2.srvs.push_back(BoundTexture{ 13, 0x8147, TexFormat::unknown, 0, 0, 0xa005 }); // native: not in the walk at all
	n2.unresolved.push_back(icept::DispatchBindings::Unresolved{ 't', 10, 0xc010, 2, 0x8144 });
	n2.unresolved.push_back(icept::DispatchBindings::Unresolved{ 't', 11, 0xc011, 2, 0x9999 });
	n2.unresolved.push_back(icept::DispatchBindings::Unresolved{ 't', 12, 0xc012, 1, 0 });
	const diff::Result r2 = diff::compare(o2, n2, &full);
	REQUIRE(r2.mismatches.size() == 3);
	REQUIRE(r2.unknown.size() == 4);
	CHECK(r2.mismatches[0].find("=> RESHADE-COPY-STALE") != std::string::npos);
	CHECK(r2.mismatches[0].find("ReShade's view map says 3333") != std::string::npos);
	CHECK(r2.mismatches[1].find("=> RESHADE-VIEW-RECREATED") != std::string::npos);
	CHECK(r2.mismatches[1].find("which now holds res 2222 written at seq 50 - AFTER the copy") != std::string::npos);
	CHECK(r2.mismatches[2].find("=> BOTH-LIVE") != std::string::npos);
	CHECK(r2.mismatches[2].find("written by a view creation") != std::string::npos);
	CHECK(r2.unknown[0].find("TOMBSTONE: res 8144 died after the slot was written; the oracle names that address") != std::string::npos);
	CHECK(r2.unknown[0].find("=> RESHADE-STALE") != std::string::npos);
	CHECK(r2.unknown[1].find("=> RESHADE-VIEW-RECREATED") != std::string::npos); // the tombstone's source was re-created for the oracle's resource
	CHECK(r2.unknown[2].find("never written since attach") != std::string::npos);
	CHECK(r2.unknown[2].find("=> NATIVE-BLIND") != std::string::npos);
	CHECK(r2.unknown[3].find("no such register") != std::string::npos);
	CHECK(r2.unknown[3].find("=> NATIVE-MISSED") != std::string::npos);
	CHECK(r2.verdicts[static_cast<int>(diff::Verdict::reshade_copy_stale)] == 1);
	CHECK(r2.verdicts[static_cast<int>(diff::Verdict::reshade_view_recreated)] == 2);
	CHECK(r2.verdicts[static_cast<int>(diff::Verdict::reshade_stale)] == 1);
	CHECK(r2.verdicts[static_cast<int>(diff::Verdict::native_blind)] == 1);
	CHECK(r2.verdicts[static_cast<int>(diff::Verdict::native_missed)] == 1);
	CHECK(r2.verdicts[static_cast<int>(diff::Verdict::both_live)] == 1);
	CHECK_FALSE(r2.oracle_wrong()); // NATIVE-BLIND and NATIVE-MISSED slots keep it unresolved
	// A disagreement made only of ReShade-convicting slots is the oracle's fault, whole.
	icept::DispatchBindings o3, n3;
	o3.srvs.push_back(BoundTexture{ 10, 0x8144, TexFormat::unknown, 0, 0, 0xa002 });
	o3.srvs.push_back(BoundTexture{ 11, 0x2222, TexFormat::unknown, 0, 0, 0xa001 });
	n3.unresolved.push_back(icept::DispatchBindings::Unresolved{ 't', 10, 0xc010, 2, 0x8144 });
	n3.unresolved.push_back(icept::DispatchBindings::Unresolved{ 't', 11, 0xc011, 2, 0x9999 });
	const diff::Result r3 = diff::compare(o3, n3, &full);
	CHECK(r3.oracle_wrong());
	CHECK_FALSE(diff::compare(o3, n3).oracle_wrong()); // unadjudicated never convicts
	// EXTRA: the oracle dropped a view (dead per its liveness) that is exactly the source
	// the native slot was copied from - the game released/re-created it after the copy.
	icept::DispatchBindings o4, n4;
	o4.unresolved.push_back(icept::DispatchBindings::Unresolved{ 't', 26, 0xa001, 4, 0x7777 });
	n4.srvs.push_back(BoundTexture{ 26, 0x3333, TexFormat::unknown, 0, 0, 0xb801 }); // copied from a001
	n4.srvs.push_back(BoundTexture{ 27, 0x3333, TexFormat::unknown, 0, 0, 0xb802 }); // a view creation; the oracle said nothing
	const diff::Result r4 = diff::compare(o4, n4, &full);
	REQUIRE(r4.extra.size() == 2);
	CHECK(r4.extra[0].find("oracle dropped view a001: its resource 7777 is dead per ReShade") != std::string::npos);
	CHECK(r4.extra[0].find("=> RESHADE-VIEW-RECREATED") != std::string::npos);
	CHECK(r4.extra[1].find("=> ORACLE-MISSED") != std::string::npos);
	CHECK_FALSE(r4.oracle_wrong());
	// Without an adjudicator every differing slot is unadjudicated and the lines are bare.
	const diff::Result bare = diff::compare(oracle, native);
	CHECK(bare.verdicts[static_cast<int>(diff::Verdict::unadjudicated)] == 8);
	CHECK(bare.mismatches[0].find("=>") == std::string::npos);
	// The live machinery accumulates verdicts into the summary.
	diff::set_enabled(true);
	const diff::Summary before = diff::summary();
	int list = 0;
	diff::publish_expected(&list, 0x1, 40, 20, oracle);
	CHECK(diff::consume_and_compare(&list, native, 0, &adj, "shadow rs=0 tables=0 root-cbv=0"));
	const diff::Summary after = diff::summary();
	CHECK(after.verdicts[static_cast<int>(diff::Verdict::reshade_stale)] - before.verdicts[static_cast<int>(diff::Verdict::reshade_stale)] == 2);
	diff::set_enabled(false);
}

TEST_CASE("present plan: the swapchain's queue by identity, else the first DIRECT queue (a proxy hid it), else none")
{
	std::vector<core::QueueRecord> q;
	CHECK(core::pick_present_queue(q, 0x10) == -1);
	q.push_back(core::QueueRecord{ 0x30, 3 }); // copy
	q.push_back(core::QueueRecord{ 0x20, 2 }); // compute
	CHECK(core::pick_present_queue(q, 0x99) == -1); // no direct queue at all
	q.push_back(core::QueueRecord{ 0x10, 0 }); // the game's direct queue
	q.push_back(core::QueueRecord{ 0x11, 0 }); // a second direct queue
	CHECK(core::pick_present_queue(q, 0x11) == 3); // identity wins
	CHECK(core::pick_present_queue(q, 0x10) == 2);
	CHECK(core::pick_present_queue(q, 0x99) == 2); // ReShade's proxy: the first direct one
	CHECK(core::pick_present_queue(q, 0) == 2);
	CHECK(core::pick_present_queue(q, 0x20) == 1); // an exact match, even a compute queue, is honoured
}
