#include "app/diff_observer.hpp"
#include "core/heap_math.hpp"
#include "core/root_signature_walk.hpp"
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
