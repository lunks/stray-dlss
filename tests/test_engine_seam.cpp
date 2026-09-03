// Finding UE 4.27's ITemporalUpscaler in a module image, and correlating what it announces
// against the dispatches we intercept (src/core/engine_seam.hpp).
//
// The whole point of the seam is to stop guessing which compute dispatch is the TAA pass, so a
// scan that reports a WRONG address is strictly worse than one that reports none. These tests
// therefore spend most of their effort on the refusals: a near-miss vtable, an unfamiliar
// codegen shape, the literal present with no accessor, fractions that decode to the wrong
// numbers. Discovery is built here out of bytes we lay down ourselves, exactly as the live
// scan sees them - there is no way for the scan to tell a synthetic image from a mapped one.
#include "core/engine_seam.hpp"

#include <doctest/doctest.h>

#include <cstring>
#include <vector>

using namespace stray_dlss::seam;

namespace {

// A hand-built image shaped like the part of a UE 4.27 shipping executable this scan cares
// about: one executable section holding five tiny functions, one read-only section holding the
// wide literal, the float pool and the vtable.
struct FakeModule
{
	static constexpr std::uint64_t kTextVa = 0x0000000140001000ull;
	static constexpr std::uint64_t kDataVa = 0x0000000141000000ull;

	// Offsets inside .text.
	static constexpr std::size_t kDtor = 0x00;
	static constexpr std::size_t kGetDebugName = 0x10;
	static constexpr std::size_t kAddPasses = 0x20;
	static constexpr std::size_t kMinFraction = 0x30;
	static constexpr std::size_t kMaxFraction = 0x40;
	// Offsets inside .rdata.
	static constexpr std::size_t kLiteral = 0x000;
	static constexpr std::size_t kPoolMin = 0x080;
	static constexpr std::size_t kPoolMax = 0x084;
	static constexpr std::size_t kVtable = 0x0C0;

	std::vector<unsigned char> text{ std::vector<unsigned char>(0x100, 0xCC) };
	std::vector<unsigned char> data{ std::vector<unsigned char>(0x200, 0x00) };

	static void put_i32(std::vector<unsigned char> &v, std::size_t at, std::int32_t x)
	{
		std::memcpy(v.data() + at, &x, sizeof(x));
	}
	static void put_u64(std::vector<unsigned char> &v, std::size_t at, std::uint64_t x)
	{
		std::memcpy(v.data() + at, &x, sizeof(x));
	}
	static void put_f32(std::vector<unsigned char> &v, std::size_t at, float x)
	{
		std::memcpy(v.data() + at, &x, sizeof(x));
	}

	// UTF-16LE, NUL-terminated, the way TEXT("...") lands in the image.
	void write_literal(std::size_t at, const char *ascii)
	{
		std::size_t n = at;
		for (const char *p = ascii; *p != '\0'; ++p)
		{
			data[n++] = static_cast<unsigned char>(*p);
			data[n++] = 0;
		}
		data[n++] = 0;
		data[n++] = 0;
	}

	// lea rax, [rip+d] ; ret  -> the address of `target`
	void write_lea_ret(std::size_t at, std::uint64_t target)
	{
		text[at + 0] = 0x48;
		text[at + 1] = 0x8D;
		text[at + 2] = 0x05;
		const std::uint64_t next = kTextVa + at + 7;
		put_i32(text, at + 3, static_cast<std::int32_t>(static_cast<std::int64_t>(target - next)));
		text[at + 7] = 0xC3;
	}

	// movss xmm0, [rip+d] ; ret
	void write_movss_ret(std::size_t at, std::uint64_t pool)
	{
		text[at + 0] = 0xF3;
		text[at + 1] = 0x0F;
		text[at + 2] = 0x10;
		text[at + 3] = 0x05;
		const std::uint64_t next = kTextVa + at + 8;
		put_i32(text, at + 4, static_cast<std::int32_t>(static_cast<std::int64_t>(pool - next)));
		text[at + 8] = 0xC3;
	}

	// mov eax, imm32 ; movd xmm0, eax ; ret
	void write_movd_ret(std::size_t at, float value)
	{
		std::uint32_t bits = 0;
		std::memcpy(&bits, &value, sizeof(bits));
		text[at + 0] = 0xB8;
		std::memcpy(text.data() + at + 1, &bits, sizeof(bits));
		text[at + 5] = 0x66;
		text[at + 6] = 0x0F;
		text[at + 7] = 0x6E;
		text[at + 8] = 0xC0;
		text[at + 9] = 0xC3;
	}

	// The whole, correct arrangement. Individual tests then break exactly one thing.
	void build(float min_fraction = 0.5f, float max_fraction = 2.0f,
	           const char *literal = "FDefaultTemporalUpscaler")
	{
		write_literal(kLiteral, literal);
		put_f32(data, kPoolMin, min_fraction);
		put_f32(data, kPoolMax, max_fraction);

		text[kDtor] = 0xC3;
		write_lea_ret(kGetDebugName, kDataVa + kLiteral);
		text[kAddPasses] = 0xC3;
		write_movss_ret(kMinFraction, kDataVa + kPoolMin);
		write_movss_ret(kMaxFraction, kDataVa + kPoolMax);

		put_u64(data, kVtable + 0x00, kTextVa + kDtor);
		put_u64(data, kVtable + 0x08, kTextVa + kGetDebugName);
		put_u64(data, kVtable + 0x10, kTextVa + kAddPasses);
		put_u64(data, kVtable + 0x18, kTextVa + kMinFraction);
		put_u64(data, kVtable + 0x20, kTextVa + kMaxFraction);
	}

	Region regions[2]{};
	Image image()
	{
		regions[0] = Region{ kTextVa, text.data(), text.size(), true, ".text" };
		regions[1] = Region{ kDataVa, data.data(), data.size(), false, ".rdata" };
		return Image{ regions, 2 };
	}
};

} // namespace

TEST_CASE("discover finds the vtable and validates it against 0.5 and 2.0")
{
	FakeModule m;
	m.build();
	const Discovery d = discover(m.image());

	REQUIRE(d.status == SeamStatus::ok);
	CHECK(d.name_va == FakeModule::kDataVa + FakeModule::kLiteral);
	CHECK(d.name_hits == 1);
	CHECK(d.get_debug_name_va == FakeModule::kTextVa + FakeModule::kGetDebugName);
	CHECK(d.debug_name_hits == 1);
	CHECK(d.vtable_va == FakeModule::kDataVa + FakeModule::kVtable);
	CHECK(d.vtable_hits == 1);
	CHECK(d.slot[kSlotAddPasses] == FakeModule::kTextVa + FakeModule::kAddPasses);
	CHECK(d.min_fraction == 0.5f);
	CHECK(d.max_fraction == 2.0f);
}

TEST_CASE("the mov/movd codegen for a float constant decodes too")
{
	FakeModule m;
	m.build();
	// Same two constants, the other encoding MSVC reaches for.
	std::fill(m.text.begin() + FakeModule::kMinFraction,
	          m.text.begin() + FakeModule::kMinFraction + 16, static_cast<unsigned char>(0xCC));
	std::fill(m.text.begin() + FakeModule::kMaxFraction,
	          m.text.begin() + FakeModule::kMaxFraction + 16, static_cast<unsigned char>(0xCC));
	m.write_movd_ret(FakeModule::kMinFraction, 0.5f);
	m.write_movd_ret(FakeModule::kMaxFraction, 2.0f);

	const Discovery d = discover(m.image());
	CHECK(d.status == SeamStatus::ok);
	CHECK(d.min_fraction == 0.5f);
	CHECK(d.max_fraction == 2.0f);
}

TEST_CASE("the movabs codegen for GetDebugName is the fallback, and it works")
{
	FakeModule m;
	m.build();
	// Replace lea+ret with `movabs rax, <literal address>; ret`. If MSVC ever materialises the
	// literal absolutely rather than rip-relative, discovery must still find the function
	// rather than costing a round trip to the box to discover a NOT-FOUND.
	std::fill(m.text.begin() + FakeModule::kGetDebugName,
	          m.text.begin() + FakeModule::kGetDebugName + 16, static_cast<unsigned char>(0xCC));
	const std::size_t at = FakeModule::kGetDebugName;
	m.text[at] = 0x48;
	m.text[at + 1] = 0xB8;
	const std::uint64_t target = FakeModule::kDataVa + FakeModule::kLiteral;
	std::memcpy(m.text.data() + at + 2, &target, sizeof(target));
	m.text[at + 10] = 0xC3;

	const Discovery d = discover(m.image());
	REQUIRE(d.status == SeamStatus::ok);
	CHECK(d.get_debug_name_va == FakeModule::kTextVa + FakeModule::kGetDebugName);
	CHECK(d.vtable_va == FakeModule::kDataVa + FakeModule::kVtable);
}

TEST_CASE("every refusal is distinct, named, and stops short of reporting an address")
{
	SUBCASE("nothing to scan")
	{
		const Discovery d = discover(Image{});
		CHECK(d.status == SeamStatus::no_regions);
		CHECK(d.vtable_va == 0);
	}

	SUBCASE("not a UE 4.27 image")
	{
		FakeModule m;
		m.build(0.5f, 2.0f, "FSomeOtherUpscaler");
		const Discovery d = discover(m.image());
		CHECK(d.status == SeamStatus::name_not_found);
		CHECK(d.vtable_va == 0);
	}

	SUBCASE("the literal is there but GetDebugName is a shape we do not know")
	{
		FakeModule m;
		m.build();
		std::fill(m.text.begin() + FakeModule::kGetDebugName,
		          m.text.begin() + FakeModule::kGetDebugName + 16, static_cast<unsigned char>(0x90));
		const Discovery d = discover(m.image());
		CHECK(d.status == SeamStatus::debug_name_not_found);
		CHECK(d.name_va != 0); // it got that far, and says so
		CHECK(d.vtable_va == 0);
	}

	SUBCASE("GetDebugName is found but no run of five code pointers holds it")
	{
		FakeModule m;
		m.build();
		// Blank the vtable: the pointer to GetDebugName now exists nowhere in data.
		std::fill(m.data.begin() + FakeModule::kVtable,
		          m.data.begin() + FakeModule::kVtable + 0x28, static_cast<unsigned char>(0x00));
		const Discovery d = discover(m.image());
		CHECK(d.status == SeamStatus::vtable_not_found);
		CHECK(d.get_debug_name_va != 0);
		CHECK(d.vtable_va == 0);
	}

	SUBCASE("a slot that does not point at code is not a vtable")
	{
		FakeModule m;
		m.build();
		// Slot 4 points into .rdata. Plausible-looking, and wrong.
		FakeModule::put_u64(m.data, FakeModule::kVtable + 0x20, FakeModule::kDataVa);
		const Discovery d = discover(m.image());
		CHECK(d.status == SeamStatus::vtable_not_found);
	}

	SUBCASE("an undecodable accessor is reported with its bytes, not guessed at")
	{
		FakeModule m;
		m.build();
		std::fill(m.text.begin() + FakeModule::kMaxFraction,
		          m.text.begin() + FakeModule::kMaxFraction + 16, static_cast<unsigned char>(0x5A));
		const Discovery d = discover(m.image());
		REQUIRE(d.status == SeamStatus::fraction_shape);
		CHECK(d.undecoded_va == FakeModule::kTextVa + FakeModule::kMaxFraction);
		CHECK(d.undecoded[0] == 0x5A);
		// It located a candidate, so the diagnostic can say where - but the caller must not
		// hook it, and that is the status's job.
		CHECK(d.vtable_va == FakeModule::kDataVa + FakeModule::kVtable);
	}

	SUBCASE("the wrong constants mean the wrong class, and it is refused")
	{
		FakeModule m;
		// UE 5's ITemporalUpscaler has different fractions; so would any other implementation
		// of the same shape. Both must match or we are hooking something else.
		m.build(0.5f, 4.0f);
		const Discovery d = discover(m.image());
		REQUIRE(d.status == SeamStatus::fraction_mismatch);
		CHECK(d.min_fraction == 0.5f);
		CHECK(d.max_fraction == 4.0f);
	}
}

TEST_CASE("a decoy vtable next to the real one does not win")
{
	FakeModule m;
	m.build();
	// A second five-pointer run holding the same GetDebugName, whose fraction accessors are
	// the destructor stub - i.e. `ret`, which decodes as nothing.
	const std::size_t decoy = FakeModule::kVtable + 0x40;
	FakeModule::put_u64(m.data, decoy + 0x00, FakeModule::kTextVa + FakeModule::kDtor);
	FakeModule::put_u64(m.data, decoy + 0x08, FakeModule::kTextVa + FakeModule::kGetDebugName);
	FakeModule::put_u64(m.data, decoy + 0x10, FakeModule::kTextVa + FakeModule::kDtor);
	FakeModule::put_u64(m.data, decoy + 0x18, FakeModule::kTextVa + FakeModule::kDtor);
	FakeModule::put_u64(m.data, decoy + 0x20, FakeModule::kTextVa + FakeModule::kDtor);

	const Discovery d = discover(m.image());
	REQUIRE(d.status == SeamStatus::ok);
	CHECK(d.vtable_va == FakeModule::kDataVa + FakeModule::kVtable);
	CHECK(d.vtable_hits == 1);
}

TEST_CASE("Image::read never leaves the regions it was given")
{
	FakeModule m;
	m.build();
	const Image img = m.image();

	std::uint64_t v = 0;
	CHECK(img.read(FakeModule::kDataVa + FakeModule::kVtable, &v, sizeof(v)));
	CHECK(v == FakeModule::kTextVa + FakeModule::kDtor);

	// One byte past the end of a region is a miss, not a straddled read.
	CHECK_FALSE(img.read(FakeModule::kDataVa + m.data.size() - 4, &v, sizeof(v)));
	CHECK_FALSE(img.read(FakeModule::kDataVa - 1, &v, sizeof(v)));
	CHECK_FALSE(img.read(0, &v, sizeof(v)));

	CHECK(img.is_code(FakeModule::kTextVa));
	CHECK(img.is_code(FakeModule::kTextVa + m.text.size() - 1));
	CHECK_FALSE(img.is_code(FakeModule::kTextVa + m.text.size()));
	CHECK_FALSE(img.is_code(FakeModule::kDataVa));
}

TEST_CASE("the struct offsets are the ones MSVC gives UE 4.27's own declarations")
{
	// FPassInputs: bool, then a 4-byte enum at 4, then three pointers. If any of these moves,
	// the hook reads a garbage FRDGTexture identity and the correlation silently degrades to
	// the heuristic it was meant to replace - so they are pinned here rather than commented.
	CHECK(kPassInputsAllowDownsample == 0);
	CHECK(kPassInputsDownsampleFormat == 4);
	CHECK(kPassInputsSceneColor == 8);
	CHECK(kPassInputsSceneDepth == 16);
	CHECK(kPassInputsSceneVelocity == 24);
	CHECK(kPassInputsSize == 32);

	CHECK(kIntRectMinX == 0);
	CHECK(kIntRectMinY == 4);
	CHECK(kIntRectMaxX == 8);
	CHECK(kIntRectMaxY == 12);
	CHECK(kIntRectSize == 16);

	// Declaration order in TemporalAA.h:160-172 is the MSVC slot order.
	CHECK(kSlotDestructor == 0);
	CHECK(kSlotGetDebugName == 1);
	CHECK(kSlotAddPasses == 2);
	CHECK(kSlotGetMinFraction == 3);
	CHECK(kSlotGetMaxFraction == 4);
	CHECK(kVtableSlots == 5);
}

// ---------------------------------------------------------------------------------------
// The ledger
// ---------------------------------------------------------------------------------------

namespace {
Announcement made(std::uint64_t frame, std::uint32_t w, std::uint32_t h)
{
	Announcement a;
	a.frame = frame;
	a.out_width = w;
	a.out_height = h;
	a.colour_rdg = 0x1111;
	a.depth_rdg = 0x2222;
	a.velocity_rdg = 0x3333;
	return a;
}
} // namespace

TEST_CASE("expected_groups is ceil(extent / 8), which is what UE 4.27 dispatches")
{
	CHECK(Ledger::expected_groups(3840) == 480);
	CHECK(Ledger::expected_groups(2160) == 270);
	CHECK(Ledger::expected_groups(2560) == 320);
	CHECK(Ledger::expected_groups(1440) == 180);
	CHECK(Ledger::expected_groups(1) == 1);
	CHECK(Ledger::expected_groups(8) == 1);
	CHECK(Ledger::expected_groups(9) == 2);
	CHECK(Ledger::expected_groups(0) == 0);
}

TEST_CASE("a dispatch that matches the announced rect claims it, exactly once")
{
	Ledger l;
	l.begin_frame(100);
	l.announce(made(100, 3840, 2160));

	CHECK(l.pending() == 1);
	const Announcement *a = l.claim(480, 270);
	REQUIRE(a != nullptr);
	CHECK(a->out_width == 3840);
	CHECK(a->colour_rdg == 0x1111);
	CHECK(a->sequence == 1);
	CHECK(l.pending() == 0);

	// A second dispatch of the same shape is NOT the same pass. One announcement, one claim.
	CHECK(l.claim(480, 270) == nullptr);
	CHECK(l.counters().claimed == 1);
	CHECK(l.counters().orphans == 1);
}

TEST_CASE("a dispatch with no announcement is an ORPHAN - the wrong-pass class, named")
{
	Ledger l;
	l.begin_frame(1);
	// The engine ran no primary temporal upscale this frame (a loading screen, a menu, or a
	// pass that reaches FTAAStandaloneCS through DiaphragmDOF / LightShaft / SSR / the water
	// renderer / a planar reflection - none of which go through ITemporalUpscaler).
	CHECK(l.claim(480, 270) == nullptr);
	CHECK(l.counters().orphans == 1);
	CHECK(l.counters().rect_mismatch == 0);
	CHECK(l.counters().claimed == 0);
}

TEST_CASE("an announcement whose rect nobody matches is a rect_mismatch, not an orphan")
{
	Ledger l;
	l.begin_frame(1);
	l.announce(made(1, 3840, 2160));
	CHECK(l.claim(240, 135) == nullptr); // half-res: a denoiser, not the upscale
	CHECK(l.counters().rect_mismatch == 1);
	CHECK(l.counters().orphans == 0);
	CHECK(l.pending() == 1);
}

TEST_CASE("two views in one family are claimed independently, by rect")
{
	Ledger l;
	l.begin_frame(7);
	l.announce(made(7, 1920, 1080));
	l.announce(made(7, 3840, 2160));

	const Announcement *b = l.claim(480, 270);
	REQUIRE(b != nullptr);
	CHECK(b->out_width == 3840);
	const Announcement *a = l.claim(240, 135);
	REQUIRE(a != nullptr);
	CHECK(a->out_width == 1920);
	CHECK(l.counters().claimed == 2);
	CHECK(l.counters().rect_mismatch == 0);
}

TEST_CASE("an announcement survives the present boundary, then retires unclaimed")
{
	Ledger l;
	l.begin_frame(10);
	l.announce(made(10, 2560, 1440));

	// The recording can straddle Present when an RHI thread is in play, so the very next
	// frame must still be able to claim it.
	l.begin_frame(11);
	CHECK(l.pending() == 1);
	const Announcement *a = l.claim(320, 180);
	CHECK(a != nullptr);
	CHECK(l.counters().unclaimed == 0);

	// One that nobody ever claims is counted, once, when it retires.
	l.announce(made(11, 2560, 1440));
	l.begin_frame(12);
	CHECK(l.counters().unclaimed == 0);
	l.begin_frame(13);
	CHECK(l.counters().unclaimed == 1);
	CHECK(l.pending() == 0);
}

TEST_CASE("the ring is bounded, and says so rather than dropping in silence")
{
	Ledger l;
	l.begin_frame(1);
	for (std::size_t i = 0; i < Ledger::kCapacity + 3; ++i)
		l.announce(made(1, 3840, 2160));
	CHECK(l.counters().announced == Ledger::kCapacity + 3);
	CHECK(l.counters().overflow == 3);
	CHECK(l.pending() == Ledger::kCapacity);
}
