// The engine's route to the TAA output UAV (src/core/u0_rhi_uav.hpp): the .pdata lookup that
// turns a return address into a function start, the vtable discovery held to its slot
// predictions, the object scan that finds a recorded CPU descriptor handle, the latch, the
// slot census and the assertion verdict.
//
// As with the seam's tests, most of the effort goes on REFUSALS: a vtable found by any means
// that fails one predicted empty slot must be turned down, two vtables must be turned down,
// and an object holding two recorded handles must be declined rather than picked.
#include "core/u0_rhi_uav.hpp"

#include <doctest/doctest.h>

#include <cstring>
#include <map>
#include <string>
#include <vector>

using namespace stray_dlss::u0;
using stray_dlss::seam::Image;
using stray_dlss::seam::Region;

namespace {

// A synthetic module: one executable section holding tiny functions, one read-only section
// holding the vtable and a .pdata-like table. Nothing here can tell it from a mapped image.
struct FakeModule
{
	static constexpr std::uint64_t kBase = 0x0000000140000000ull;
	static constexpr std::uint64_t kTextVa = kBase + 0x1000;
	static constexpr std::uint64_t kDataVa = kBase + 0x100000;

	std::vector<unsigned char> text{ std::vector<unsigned char>(0x1000, 0xCC) };
	std::vector<unsigned char> data{ std::vector<unsigned char>(0x1000, 0x00) };
	std::vector<RuntimeFunction> pdata;

	// Function bodies. Each is laid down at its own 16-byte cell.
	static constexpr std::size_t kCell = 0x20;
	std::size_t fn(unsigned index) const { return kCell * index; }
	std::uint64_t fn_va(unsigned index) const { return kTextVa + fn(index); }

	void put_u64(std::size_t at, std::uint64_t x) { std::memcpy(data.data() + at, &x, sizeof(x)); }

	// A "real" function: mov rax,rcx ; ret, and a pdata entry covering its cell.
	void write_code(unsigned index)
	{
		const std::size_t at = fn(index);
		text[at + 0] = 0x48;
		text[at + 1] = 0x8B;
		text[at + 2] = 0xC1;
		text[at + 3] = 0xC3;
		pdata.push_back({ static_cast<std::uint32_t>(kTextVa - kBase + at),
			static_cast<std::uint32_t>(kTextVa - kBase + at + kCell), 0 });
	}
	void write_ret(unsigned index)
	{
		text[fn(index)] = 0xC3;
		pdata.push_back({ static_cast<std::uint32_t>(kTextVa - kBase + fn(index)),
			static_cast<std::uint32_t>(kTextVa - kBase + fn(index) + kCell), 0 });
	}
	void write_xorret(unsigned index)
	{
		const std::size_t at = fn(index);
		text[at + 0] = 0x33;
		text[at + 1] = 0xC0;
		text[at + 2] = 0xC3;
		pdata.push_back({ static_cast<std::uint32_t>(kTextVa - kBase + at),
			static_cast<std::uint32_t>(kTextVa - kBase + at + kCell), 0 });
	}

	static constexpr std::size_t kVtable = 0x200;
	// One function per slot, shaped as the expectations say; index == slot.
	void build_vtable(bool fold_rets = false)
	{
		for (unsigned s = 0; s < kSlotsChecked; ++s)
		{
			bool is_ret = false;
			bool is_xorret = false;
			for (const SlotExpectation &e : kSlotExpectations)
			{
				if (e.slot != s)
					continue;
				if (e.expect == Expect::ret)
					is_ret = true;
				if (e.expect == Expect::xorret)
					is_xorret = true;
			}
			if (is_ret)
				write_ret(s);
			else if (is_xorret)
				write_xorret(s);
			else
				write_code(s);
			std::uint64_t target = fn_va(s);
			if (fold_rets && is_ret)
				target = fn_va(kSlotSetAsyncComputeBudget);
			put_u64(kVtable + 8u * s, target);
		}
	}

	Region regions[2]{};
	Image image()
	{
		regions[0] = Region{ kTextVa, text.data(), text.size(), true, ".text" };
		regions[1] = Region{ kDataVa, data.data(), data.size(), false, ".rdata" };
		return Image{ regions, 2 };
	}
	FunctionTable table()
	{
		FunctionTable t;
		t.entries = pdata.data();
		t.count = pdata.size();
		t.image_base = kBase;
		return t;
	}
	std::uint64_t seed() const { return fn_va(kSlotDispatchComputeShader); }
	std::uint64_t vtable_va() const { return kDataVa + kVtable; }
};

} // namespace

// ---------------------------------------------------------------------------------------
// .pdata
// ---------------------------------------------------------------------------------------

TEST_CASE("function_start: a return address inside a function resolves to its start")
{
	FakeModule m;
	m.build_vtable();
	// pdata must be sorted by begin; build_vtable pushes in slot order, which is ascending.
	const FunctionTable t = m.table();
	// A return address a few bytes into slot 3's cell.
	CHECK(function_start(t, m.seed() + 9) == m.seed());
	// The first byte of the function is inside it too.
	CHECK(function_start(t, m.seed()) == m.seed());
	// The byte after the cell belongs to the next function.
	CHECK(function_start(t, m.seed() + FakeModule::kCell) == m.fn_va(kSlotDispatchComputeShader + 1));
}

TEST_CASE("function_start: outside every entry, or outside the image, is 0")
{
	FakeModule m;
	m.build_vtable();
	const FunctionTable t = m.table();
	CHECK(function_start(t, FakeModule::kBase) == 0);
	CHECK(function_start(t, FakeModule::kBase - 8) == 0);
	CHECK(function_start(t, m.fn_va(kSlotsChecked) + 0x800) == 0);
	FunctionTable empty;
	CHECK(function_start(empty, m.seed()) == 0);
}

TEST_CASE("function_start: a chained entry follows UNW_FLAG_CHAININFO to the primary")
{
	FakeModule m;
	// One primary function at cell 0 covering [0, 0x20), and a cold split at cell 5 whose
	// unwind info is chained back to it.
	m.write_code(0);
	// The chained UNWIND_INFO lives in .rdata at 0x400: Version 1 | Flags CHAININFO (4) -> byte0
	// = (4 << 3) | 1 = 0x21; SizeOfProlog 0; CountOfCodes 1 (padded to 2 codes = 4 bytes);
	// FrameRegister/Offset 0; then 2 codes; then the chained RUNTIME_FUNCTION.
	const std::size_t unwind = 0x400;
	m.data[unwind + 0] = 0x21;
	m.data[unwind + 1] = 0x00;
	m.data[unwind + 2] = 0x01;
	m.data[unwind + 3] = 0x00;
	const std::size_t chained = unwind + 4 + 2 * 2;
	const RuntimeFunction parent{ static_cast<std::uint32_t>(FakeModule::kTextVa - FakeModule::kBase),
		static_cast<std::uint32_t>(FakeModule::kTextVa - FakeModule::kBase + FakeModule::kCell), 0 };
	std::memcpy(m.data.data() + chained, &parent, sizeof(parent));
	m.pdata.push_back({ static_cast<std::uint32_t>(FakeModule::kTextVa - FakeModule::kBase + m.fn(5)),
		static_cast<std::uint32_t>(FakeModule::kTextVa - FakeModule::kBase + m.fn(5) + FakeModule::kCell),
		static_cast<std::uint32_t>(FakeModule::kDataVa - FakeModule::kBase + unwind) });
	const Image img = m.image();
	FunctionTable t = m.table();
	t.image = &img;
	CHECK(function_start(t, m.fn_va(5) + 3) == m.fn_va(0));
	// Without an image the chain cannot be read and the split's own start is returned.
	t.image = nullptr;
	CHECK(function_start(t, m.fn_va(5) + 3) == m.fn_va(5));
}

// ---------------------------------------------------------------------------------------
// The vtable
// ---------------------------------------------------------------------------------------

TEST_CASE("discover_context_vtable: the correct arrangement is found once, with every prediction")
{
	FakeModule m;
	m.build_vtable();
	const CtxDiscovery d = discover_context_vtable(m.image(), m.seed());
	REQUIRE(d.status == CtxStatus::ok);
	CHECK(d.vtable_va == m.vtable_va());
	CHECK(d.qword_hits == 1);
	CHECK(d.survivors == 1);
	CHECK(d.slot[kSlotDispatchComputeShader] == m.seed());
	CHECK(d.slot[kSlotSetComputeShader] == m.fn_va(kSlotSetComputeShader));
	CHECK(d.slot[kSlotSetUAVParameterA] == m.fn_va(kSlotSetUAVParameterA));
	CHECK(d.slot[kSlotSetUAVParameterB] == m.fn_va(kSlotSetUAVParameterB));
	// Every expectation held, required and reported alike.
	for (std::size_t k = 0; k < kSlotExpectationCount; ++k)
		CHECK((d.expectation_mask & (1u << k)) != 0);
	CHECK(d.ret_fold == 1); // six distinct ret bodies: only the reference slot matches itself
}

TEST_CASE("discover_context_vtable: ICF-folded empty bodies are reported, not refused")
{
	FakeModule m;
	m.build_vtable(/*fold_rets=*/true);
	const CtxDiscovery d = discover_context_vtable(m.image(), m.seed());
	REQUIRE(d.status == CtxStatus::ok);
	CHECK(d.ret_fold == 6);
}

// MEASURED on Stray-Win64-Shipping.exe 2026-09-04 (facts §37.2). Every slot the design predicts
// to be an empty body — 5, 10, 11, 12, 13, 25, and the reported 28/32/37 — is MSVC's `ret 0`,
// C2 00 00, all folded by ICF onto one address, while slot 36 is `33 C0 C3` exactly as predicted.
// Accepting only C3 refused that vtable at slot 5 (`survivors=0 failedSlot=5`) even though every
// other prediction held, which is what this case pins.
TEST_CASE("discover_context_vtable: MSVC's `ret 0` (C2 00 00) is an empty body too - Stray's own shape")
{
	FakeModule m;
	m.build_vtable(/*fold_rets=*/true);
	for (const SlotExpectation &e : kSlotExpectations)
	{
		if (e.expect != Expect::ret)
			continue;
		const std::size_t at = m.fn(e.slot);
		m.text[at + 0] = 0xC2;
		m.text[at + 1] = 0x00;
		m.text[at + 2] = 0x00;
	}
	const CtxDiscovery d = discover_context_vtable(m.image(), m.seed());
	REQUIRE(d.status == CtxStatus::ok);
	CHECK(d.vtable_va == m.vtable_va());
	CHECK(d.survivors == 1);
	CHECK(d.ret_fold == 6);
	// The reported predictions hold on this shape too, slot 36's xor eax,eax; ret included.
	for (std::size_t k = 0; k < kSlotExpectationCount; ++k)
		CHECK((d.expectation_mask & (1u << k)) != 0);
}

TEST_CASE("discover_context_vtable: a `ret imm16` that is not `ret 0` is NOT an empty body")
{
	FakeModule m;
	m.build_vtable();
	// C2 08 00 pops 8 bytes: not a body-less return, and no x64 empty body compiles to it.
	m.text[m.fn(kSlotSetAsyncComputeBudget) + 0] = 0xC2;
	m.text[m.fn(kSlotSetAsyncComputeBudget) + 1] = 0x08;
	m.text[m.fn(kSlotSetAsyncComputeBudget) + 2] = 0x00;
	const CtxDiscovery d = discover_context_vtable(m.image(), m.seed());
	CHECK(d.status == CtxStatus::prediction_failed);
	CHECK(d.failed_slot == kSlotSetAsyncComputeBudget);
	CHECK(d.survivors == 0);
}

TEST_CASE("discover_context_vtable: no seed, a seed outside code, a seed no qword holds")
{
	FakeModule m;
	m.build_vtable();
	CHECK(discover_context_vtable(m.image(), 0).status == CtxStatus::no_seed);
	CHECK(discover_context_vtable(m.image(), m.vtable_va()).status == CtxStatus::seed_not_code);
	// A real function nothing points at.
	m.write_code(kSlotsChecked + 2);
	CHECK(discover_context_vtable(m.image(), m.fn_va(kSlotsChecked + 2)).status == CtxStatus::no_vtable);
}

TEST_CASE("discover_context_vtable: one required empty-body prediction failing refuses the candidate")
{
	for (const SlotExpectation &e : kSlotExpectations)
	{
		if (!e.required || e.expect != Expect::ret)
			continue;
		FakeModule m;
		m.build_vtable();
		// Replace that slot's body with real code.
		m.text[m.fn(e.slot)] = 0x48;
		m.text[m.fn(e.slot) + 1] = 0x8B;
		m.text[m.fn(e.slot) + 2] = 0xC1;
		m.text[m.fn(e.slot) + 3] = 0xC3;
		const CtxDiscovery d = discover_context_vtable(m.image(), m.seed());
		CHECK(d.status == CtxStatus::prediction_failed);
		CHECK(d.failed_slot == e.slot);
		CHECK(d.survivors == 0);
	}
}

TEST_CASE("discover_context_vtable: a reported prediction failing does NOT refuse, and is visible")
{
	FakeModule m;
	m.build_vtable();
	// RHIGetNativeCommandBuffer compiled to something other than xor eax,eax; ret.
	m.text[m.fn(kSlotGetNativeCommandBuffer)] = 0x48;
	const CtxDiscovery d = discover_context_vtable(m.image(), m.seed());
	REQUIRE(d.status == CtxStatus::ok);
	std::size_t idx = 0;
	for (; idx < kSlotExpectationCount; ++idx)
		if (kSlotExpectations[idx].slot == kSlotGetNativeCommandBuffer)
			break;
	REQUIRE(idx < kSlotExpectationCount);
	CHECK((d.expectation_mask & (1u << idx)) == 0);
}

TEST_CASE("discover_context_vtable: a slot outside code refuses; two surviving vtables refuse")
{
	{
		FakeModule m;
		m.build_vtable();
		m.put_u64(FakeModule::kVtable + 8u * kSlotPushEvent, FakeModule::kDataVa + 0x10);
		const CtxDiscovery d = discover_context_vtable(m.image(), m.seed());
		CHECK(d.status == CtxStatus::slot_not_code);
		CHECK(d.failed_slot == kSlotPushEvent);
	}
	{
		FakeModule m;
		m.build_vtable();
		// A byte-identical second vtable elsewhere in .rdata.
		std::memcpy(m.data.data() + 0x800, m.data.data() + FakeModule::kVtable, 8u * kSlotsChecked);
		const CtxDiscovery d = discover_context_vtable(m.image(), m.seed());
		CHECK(d.status == CtxStatus::ambiguous);
		CHECK(d.qword_hits == 2);
		CHECK(d.survivors == 2);
	}
}

TEST_CASE("discover_context_vtable: a qword hit that is NOT slot 3 of a vtable is refused, not shifted")
{
	FakeModule m;
	m.build_vtable();
	// A stray copy of the seed sitting in a data table, with garbage around it.
	m.put_u64(0x900, m.seed());
	const CtxDiscovery d = discover_context_vtable(m.image(), m.seed());
	// The real vtable still wins alone: the stray hit fails `slot_not_code` and is dropped.
	REQUIRE(d.status == CtxStatus::ok);
	CHECK(d.qword_hits == 2);
	CHECK(d.survivors == 1);
	CHECK(d.vtable_va == m.vtable_va());
}

TEST_CASE("the slot map matches RHIContext.h's declaration order, and the probe window brackets both UAV overloads")
{
	// A pin, so a re-count that moves anything fails here rather than on the box.
	CHECK(kSlotDispatchComputeShader == 3);
	CHECK(kSlotSetShaderTexture == 14);
	CHECK(kSlotSetUAVParameterA == 16);
	CHECK(kSlotSetUAVParameterB == 17);
	CHECK(kSlotSetShaderResourceView == 18);
	CHECK(kSlotSetShaderUniformBuffer == 19);
	CHECK(kSlotSetShaderParameter == 20); // six arguments: must stay OUTSIDE the probe window
	CHECK(kProbeFirstSlot < kSlotSetUAVParameterA);
	CHECK(kProbeLastSlot > kSlotSetUAVParameterB);
	CHECK(kProbeLastSlot < kSlotSetShaderParameter);
	CHECK(kProbeCount == 6);
	CHECK(kSlotGetNativeCommandBuffer == 36);
	CHECK(kSlotsChecked == 38);
}

// ---------------------------------------------------------------------------------------
// The object scan
// ---------------------------------------------------------------------------------------

namespace {

struct FakeObject
{
	// 48 qwords of "object", readable; the reader refuses anything outside.
	std::uint64_t qwords[48] = {};
	static constexpr std::uint64_t kVa = 0x0000020000001000ull;

	static bool read(void *ctx, std::uint64_t va, std::uint64_t *out)
	{
		auto *o = static_cast<FakeObject *>(ctx);
		if (va < kVa || va + 8 > kVa + sizeof(o->qwords) || (va & 7) != 0)
			return false;
		*out = o->qwords[(va - kVa) / 8];
		return true;
	}
	ObjectReader reader() { return ObjectReader{ &FakeObject::read, this }; }
};

struct FakeShadow
{
	struct Entry
	{
		HandleKind kind;
		std::uint64_t resource;
		bool dead;
	};
	std::map<std::uint64_t, Entry> entries;

	static bool probe(void *ctx, std::uint64_t value, HandleKind *kind, std::uint64_t *resource, bool *dead)
	{
		auto *s = static_cast<FakeShadow *>(ctx);
		const auto it = s->entries.find(value);
		if (it == s->entries.end())
			return false;
		*kind = it->second.kind;
		*resource = it->second.resource;
		*dead = it->second.dead;
		return true;
	}
	HandleLookup lookup() { return HandleLookup{ &FakeShadow::probe, this }; }
};

} // namespace

TEST_CASE("scan_object_for_handle: exactly one recorded UAV handle locates the offset and the resource")
{
	FakeObject o;
	FakeShadow s;
	o.qwords[0] = 0x0000000140ABC000ull; // the vptr
	o.qwords[5] = 0x11223344ull;          // the CPU handle, at +40 as FD3D12View's Descriptor would put it
	o.qwords[7] = 0x0000020000009000ull;  // some heap pointer
	s.entries[0x11223344ull] = { HandleKind::uav, 0xBEEF0000ull, false };
	const ScanResult r = scan_object_for_handle(o.reader(), s.lookup(), FakeObject::kVa);
	CHECK(r.status == ScanStatus::ok);
	CHECK(r.offset == 40);
	CHECK(r.kind == HandleKind::uav);
	CHECK(r.handle == 0x11223344ull);
	CHECK(r.resource == 0xBEEF0000ull);
	CHECK(r.hits == 1);
}

TEST_CASE("scan_object_for_handle: zero hits, two hits, a dead resource, an unreadable object")
{
	FakeObject o;
	FakeShadow s;
	o.qwords[5] = 0x1111ull;
	o.qwords[9] = 0x2222ull;
	CHECK(scan_object_for_handle(o.reader(), s.lookup(), FakeObject::kVa).status == ScanStatus::no_hit);

	s.entries[0x1111ull] = { HandleKind::uav, 0xA, false };
	s.entries[0x2222ull] = { HandleKind::srv, 0xB, false };
	const ScanResult two = scan_object_for_handle(o.reader(), s.lookup(), FakeObject::kVa);
	CHECK(two.status == ScanStatus::ambiguous);
	CHECK(two.hits == 2);
	CHECK(two.resource == 0);

	s.entries.erase(0x2222ull);
	s.entries[0x1111ull].dead = true;
	CHECK(scan_object_for_handle(o.reader(), s.lookup(), FakeObject::kVa).status == ScanStatus::dead);

	CHECK(scan_object_for_handle(o.reader(), s.lookup(), FakeObject::kVa + 0x10000).status == ScanStatus::unreadable);
	CHECK(scan_object_for_handle(o.reader(), s.lookup(), 0).status == ScanStatus::unreadable);
}

TEST_CASE("scan_object_for_handle: the vptr qword is never a candidate, and cbv/rtv/dsv handles do not count")
{
	FakeObject o;
	FakeShadow s;
	o.qwords[0] = 0x5555ull;
	o.qwords[3] = 0x6666ull;
	s.entries[0x5555ull] = { HandleKind::uav, 0xA, false };
	s.entries[0x6666ull] = { HandleKind::other, 0xB, false };
	CHECK(scan_object_for_handle(o.reader(), s.lookup(), FakeObject::kVa).status == ScanStatus::no_hit);
}

TEST_CASE("read_handle_at: the latched offset must still hold a handle of the wanted kind")
{
	FakeObject o;
	FakeShadow s;
	o.qwords[5] = 0x1111ull;
	s.entries[0x1111ull] = { HandleKind::uav, 0xA, false };
	CHECK(read_handle_at(o.reader(), s.lookup(), FakeObject::kVa, 40, HandleKind::uav).status == ScanStatus::ok);
	CHECK(read_handle_at(o.reader(), s.lookup(), FakeObject::kVa, 40, HandleKind::uav).resource == 0xA);
	CHECK(read_handle_at(o.reader(), s.lookup(), FakeObject::kVa, 40, HandleKind::srv).status == ScanStatus::no_hit);
	CHECK(read_handle_at(o.reader(), s.lookup(), FakeObject::kVa, 48, HandleKind::uav).status == ScanStatus::no_hit);
	s.entries[0x1111ull].dead = true;
	const ScanResult d = read_handle_at(o.reader(), s.lookup(), FakeObject::kVa, 40, HandleKind::uav);
	CHECK(d.status == ScanStatus::dead);
	CHECK(d.resource == 0);
}

TEST_CASE("OffsetLatch: closes after kLatchAgreements consecutive agreements, and a disagreement restarts the run")
{
	OffsetLatch l;
	CHECK(!l.observe(40));
	CHECK(!l.observe(40));
	CHECK(!l.observe(48)); // disagreement: run restarts at 48
	CHECK(l.run() == 1);
	CHECK(!l.observe(48));
	CHECK(l.observe(48));
	CHECK(l.latched());
	CHECK(l.offset() == 48);
	CHECK(!l.observe(40)); // latched: further observations change nothing
	CHECK(l.offset() == 48);
	l.reset();
	CHECK(!l.latched());
}

// ---------------------------------------------------------------------------------------
// Slot roles
// ---------------------------------------------------------------------------------------

TEST_CASE("classify_slot: unknown until enough samples; then the majority kind, or silent")
{
	SlotCensus c;
	CHECK(classify_slot(c) == SlotRole::unknown);
	c.classified = kClassifySamples - 1;
	c.uav_hits = c.classified;
	CHECK(classify_slot(c) == SlotRole::unknown);
	c.classified = kClassifySamples;
	c.uav_hits = kClassifySamples;
	CHECK(classify_slot(c) == SlotRole::uav);

	SlotCensus srv;
	srv.classified = kClassifySamples;
	srv.srv_hits = 12;
	srv.no_hits = 4;
	CHECK(classify_slot(srv) == SlotRole::srv);

	SlotCensus silent;
	silent.classified = kClassifySamples;
	silent.no_hits = kClassifySamples;
	CHECK(classify_slot(silent) == SlotRole::silent);

	SlotCensus tie;
	tie.classified = kClassifySamples;
	tie.uav_hits = 8;
	tie.srv_hits = 8;
	CHECK(classify_slot(tie) == SlotRole::unknown);
}

// ---------------------------------------------------------------------------------------
// The assertion
// ---------------------------------------------------------------------------------------

TEST_CASE("judge: agree / disagree on two live, well-shaped resources")
{
	DescFacts d;
	d.live = true;
	d.allow_uav = true;
	d.hdr_colour = true;
	d.width = 3840;
	d.height = 2160;
	CHECK(judge(true, ScanStatus::ok, 0x10, 0x10, d, 3840, 2160).verdict == Verdict::agree);
	CHECK(judge(true, ScanStatus::ok, 0x10, 0x10, d, 3840, 2160).extent_equal);
	CHECK(judge(true, ScanStatus::ok, 0x10, 0x20, d, 3840, 2160).verdict == Verdict::disagree);
	// A pooled element larger than the rect is still the right kind of thing.
	d.width = 4096;
	d.height = 2304;
	const Judgement j = judge(true, ScanStatus::ok, 0x10, 0x10, d, 3840, 2160);
	CHECK(j.verdict == Verdict::agree);
	CHECK(!j.extent_equal);
}

TEST_CASE("judge: every refusal in order, and none of them reaches a comparison")
{
	DescFacts good;
	good.live = true;
	good.allow_uav = true;
	good.hdr_colour = true;
	good.width = 3840;
	good.height = 2160;
	CHECK(judge(false, ScanStatus::ok, 0x10, 0x10, good, 3840, 2160).verdict == Verdict::no_bind);
	CHECK(judge(true, ScanStatus::no_hit, 0x10, 0x10, good, 3840, 2160).verdict == Verdict::unresolved);
	CHECK(judge(true, ScanStatus::ambiguous, 0, 0x10, good, 3840, 2160).verdict == Verdict::unresolved);
	CHECK(judge(true, ScanStatus::ok, 0, 0x10, good, 3840, 2160).verdict == Verdict::unresolved);
	DescFacts dead = good;
	dead.live = false;
	CHECK(judge(true, ScanStatus::ok, 0x10, 0x10, dead, 3840, 2160).verdict == Verdict::not_live);
	DescFacts buffer = good;
	buffer.is_buffer = true;
	CHECK(judge(true, ScanStatus::ok, 0x10, 0x10, buffer, 3840, 2160).verdict == Verdict::desc_mismatch);
	DescFacts no_uav = good;
	no_uav.allow_uav = false;
	CHECK(judge(true, ScanStatus::ok, 0x10, 0x10, no_uav, 3840, 2160).verdict == Verdict::desc_mismatch);
	DescFacts ldr = good;
	ldr.hdr_colour = false;
	CHECK(judge(true, ScanStatus::ok, 0x10, 0x10, ldr, 3840, 2160).verdict == Verdict::desc_mismatch);
	DescFacts small = good;
	small.height = 2073;
	CHECK(judge(true, ScanStatus::ok, 0x10, 0x10, small, 3840, 2160).verdict == Verdict::desc_mismatch);
	CHECK(judge(true, ScanStatus::ok, 0x10, 0, good, 3840, 2160).verdict == Verdict::walk_absent);
}

// ---------------------------------------------------------------------------------------
// The rest of the bind stream
// ---------------------------------------------------------------------------------------

TEST_CASE("judge_register: the five verdicts, in refusal order")
{
	CHECK(judge_register(false, 0, 0x10) == RegVerdict::engine_absent);
	CHECK(judge_register(false, 0x10, 0x10) == RegVerdict::engine_absent);
	CHECK(judge_register(true, 0, 0x10) == RegVerdict::unresolved);
	CHECK(judge_register(true, 0x10, 0) == RegVerdict::walk_absent);
	CHECK(judge_register(true, 0x10, 0x10) == RegVerdict::agree);
	CHECK(judge_register(true, 0x10, 0x20) == RegVerdict::disagree);
}

TEST_CASE("judge_view_register: exactly one bound uniform buffer names the View register")
{
	CHECK(judge_view_register(0, true, 4) == ViewRegVerdict::none_bound);
	CHECK(judge_view_register((1u << 3) | (1u << 4), true, 4) == ViewRegVerdict::multiple_bound);
	CHECK(judge_view_register(1u << 4, false, 4) == ViewRegVerdict::walk_absent);
	CHECK(judge_view_register(1u << 4, true, 4) == ViewRegVerdict::agree);
	// The measured bug (facts §36.18): the search took b3, the engine bound View at b4.
	CHECK(judge_view_register(1u << 4, true, 3) == ViewRegVerdict::disagree);
	CHECK(judge_view_register(1u << 1, true, 1) == ViewRegVerdict::agree);
}

// ---------------------------------------------------------------------------------------
// The graphics half (IRHICommandContext, RHIContext.h:361-748)
// ---------------------------------------------------------------------------------------

namespace {

// Lays every slot of BOTH halves as the predictions say: the compute half exactly as
// build_vtable does, then slots 38..85 with the graphics expectations, the reversed
// RHICalibrateTimers pair as (43 code, 44 ret) unless told otherwise.
void build_gfx_vtable(FakeModule &m, unsigned calibrate_ret_slot = kGfxSlotCalibrateTimersB)
{
	m.build_vtable();
	for (unsigned s = kSlotsChecked; s < kGfxSlotsChecked; ++s)
	{
		bool is_ret = s == calibrate_ret_slot;
		for (const SlotExpectation &e : kGfxSlotExpectations)
			if (e.slot == s && e.expect == Expect::ret)
				is_ret = true;
		if (is_ret)
			m.write_ret(s);
		else
			m.write_code(s);
		m.put_u64(FakeModule::kVtable + 8u * s, m.fn_va(s));
	}
}

} // namespace

TEST_CASE("the graphics slot map matches RHIContext.h's declaration order and brackets the four seams with predicted empty bodies")
{
	// New virtuals of IRHICommandContext start right after the compute half.
	CHECK(kGfxSlotSetMultipleViewports == kSlotsChecked);
	// The viewport pair is preceded by two `{}` (PollOcclusionQueries, DiscardRenderTargets) and
	// followed by four code slots then four `{}` (the MultiFrameResource quartet).
	CHECK(kGfxSlotBeginDrawingViewport == kGfxSlotDiscardRenderTargets + 1);
	CHECK(kGfxSlotEndDrawingViewport == kGfxSlotBeginDrawingViewport + 1);
	CHECK(kGfxSlotBeginUpdateMultiFrameTex == kGfxSlotEndScene + 1);
	// The render-pass pair is followed by three `{}` (Begin/EndLateLatching, NextSubpass).
	CHECK(kGfxSlotEndRenderPass == kGfxSlotBeginRenderPass + 1);
	CHECK(kGfxSlotBeginLateLatching == kGfxSlotEndRenderPass + 1);
	CHECK(kGfxSlotNextSubpass == kGfxSlotEndLateLatching + 1);
	CHECK(kGfxSlotsChecked == 86);
	// Every expectation names a slot inside the checked range and above the compute half.
	for (const SlotExpectation &e : kGfxSlotExpectations)
	{
		CHECK(e.slot >= kSlotsChecked);
		CHECK(e.slot < kGfxSlotsChecked);
		CHECK(e.expect == Expect::ret);
		CHECK(e.required);
	}
	CHECK(kGfxSlotExpectationCount == 11);
}

TEST_CASE("discover_graphics_half: the predicted arrangement is accepted and names the CalibrateTimers empty body")
{
	FakeModule m;
	build_gfx_vtable(m);
	const CtxDiscovery c = discover_context_vtable(m.image(), m.seed());
	REQUIRE(c.status == CtxStatus::ok);
	const GfxDiscovery g = discover_graphics_half(m.image(), c);
	CHECK(g.status == GfxStatus::ok);
	CHECK(g.vtable_va == m.vtable_va());
	CHECK(g.calibrate_ret_slot == kGfxSlotCalibrateTimersB);
	CHECK(g.expectation_mask == (1u << kGfxSlotExpectationCount) - 1u);
	CHECK(g.slot[kGfxSlotBeginRenderPass] == m.fn_va(kGfxSlotBeginRenderPass));
	CHECK(g.slot[kGfxSlotBeginDrawingViewport] == m.fn_va(kGfxSlotBeginDrawingViewport));
	CHECK(g.ret_fold == 1); // unfolded: only slot 40 equals itself
	// The other order of the reversed pair is equally acceptable, and reported as such.
	FakeModule m2;
	build_gfx_vtable(m2, kGfxSlotCalibrateTimersA);
	const GfxDiscovery g2 = discover_graphics_half(m2.image(), discover_context_vtable(m2.image(), m2.seed()));
	CHECK(g2.status == GfxStatus::ok);
	CHECK(g2.calibrate_ret_slot == kGfxSlotCalibrateTimersA);
}

TEST_CASE("discover_graphics_half: a failed compute discovery, a slot outside code, a required empty body that is not, and a wrong CalibrateTimers pair all refuse")
{
	FakeModule m;
	build_gfx_vtable(m);
	CtxDiscovery bad;
	bad.status = CtxStatus::no_vtable;
	CHECK(discover_graphics_half(m.image(), bad).status == GfxStatus::no_vtable);

	const CtxDiscovery c = discover_context_vtable(m.image(), m.seed());
	REQUIRE(c.status == CtxStatus::ok);
	{
		FakeModule x;
		build_gfx_vtable(x);
		x.put_u64(FakeModule::kVtable + 8u * kGfxSlotDrawPrimitive, FakeModule::kDataVa + 0x10); // .rdata, not code
		const GfxDiscovery g = discover_graphics_half(x.image(), discover_context_vtable(x.image(), x.seed()));
		CHECK(g.status == GfxStatus::slot_not_code);
		CHECK(g.failed_slot == kGfxSlotDrawPrimitive);
	}
	{
		FakeModule x;
		build_gfx_vtable(x);
		x.write_code(kGfxSlotNextSubpass); // predicted `{}`, given a body
		const GfxDiscovery g = discover_graphics_half(x.image(), discover_context_vtable(x.image(), x.seed()));
		CHECK(g.status == GfxStatus::prediction_failed);
		CHECK(g.failed_slot == kGfxSlotNextSubpass);
	}
	{
		FakeModule x;
		build_gfx_vtable(x);
		x.write_ret(kGfxSlotCalibrateTimersA); // now BOTH are empty
		const GfxDiscovery g = discover_graphics_half(x.image(), discover_context_vtable(x.image(), x.seed()));
		CHECK(g.status == GfxStatus::calibrate_pair);
	}
	{
		FakeModule x;
		build_gfx_vtable(x);
		x.write_code(kGfxSlotCalibrateTimersB); // now NEITHER is empty
		const GfxDiscovery g = discover_graphics_half(x.image(), discover_context_vtable(x.image(), x.seed()));
		CHECK(g.status == GfxStatus::calibrate_pair);
	}
	CHECK(std::string(gfx_status_text(GfxStatus::calibrate_pair)).find("CalibrateTimers") != std::string::npos);
}

TEST_CASE("discover_graphics_half: the compute half's ICF-folded ret and MSVC's `ret 0` are accepted in the graphics half too")
{
	FakeModule m;
	build_gfx_vtable(m);
	// Fold every graphics `{}` onto slot 40's address, and give slot 40 the `ret 0` encoding.
	m.text[m.fn(kGfxSlotResummarizeHTile)] = 0xC2;
	m.text[m.fn(kGfxSlotResummarizeHTile) + 1] = 0x00;
	m.text[m.fn(kGfxSlotResummarizeHTile) + 2] = 0x00;
	for (const SlotExpectation &e : kGfxSlotExpectations)
		m.put_u64(FakeModule::kVtable + 8u * e.slot, m.fn_va(kGfxSlotResummarizeHTile));
	const GfxDiscovery g = discover_graphics_half(m.image(), discover_context_vtable(m.image(), m.seed()));
	CHECK(g.status == GfxStatus::ok);
	CHECK(g.ret_fold == kGfxSlotExpectationCount);
}
