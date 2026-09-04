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
#include <map>
#include <set>
#include <string>
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

TEST_CASE("retirement is keyed on the engine's OWN announcement count, not on presents")
{
	// MEASURED 2026-09-03 with frame generation presenting twice per game frame: the hypothesis
	// that the present counter mis-pairs announcements was wrong (claimed/announced was 99%,
	// and the present owner counts GAME presents only), but the ledger must not even be
	// exposed to the question. So: an announcement lives until four newer ones exist.
	Ledger l;
	l.begin_frame(10);
	l.announce(made(10, 2560, 1440));
	// Many presents, no new announcements: the entry stays claimable well past the old
	// two-frame window (a loading screen presents without announcing).
	for (std::uint64_t f = 11; f < 10 + kRetireAfterFrames; ++f)
		l.begin_frame(f);
	CHECK(l.pending() == 1);
	CHECK(l.counters().unclaimed == 0);
	CHECK(l.claim(320, 180) != nullptr);

	// Now the engine's clock: four newer announcements retire an unclaimed older one, with no
	// present boundary at all in between.
	l.announce(made(20, 2560, 1440)); // the one we will never claim
	for (int i = 0; i < 3; ++i)
		l.announce(made(20, 3840, 2160));
	CHECK(l.counters().unclaimed == 0);
	l.announce(made(20, 3840, 2160)); // the fourth newer one
	CHECK(l.counters().unclaimed == 1);
	// The 2560x1440 one is gone; the four 4K ones remain claimable, in order.
	CHECK(l.claim(320, 180) == nullptr);
	CHECK(l.counters().rect_mismatch == 1);
	CHECK(l.pending() == 4);
}

TEST_CASE("the present backstop retires an announcement nobody follows up")
{
	Ledger l;
	l.begin_frame(100);
	l.announce(made(100, 3840, 2160));
	for (std::uint64_t f = 101; f < 100 + kRetireAfterFrames; ++f)
	{
		l.begin_frame(f);
		CHECK(l.pending() == 1);
	}
	l.begin_frame(100 + kRetireAfterFrames);
	CHECK(l.pending() == 0);
	CHECK(l.counters().unclaimed == 1);
}

TEST_CASE("a look-alike asking while the real announcement is pending is a rect_mismatch, and the real pass still claims")
{
	// The shape of the 2026-09-03 session: one 4K announcement per frame, then the SSD
	// look-alikes at 240x135 and 120x68 ask first, then the real 480x270 dispatch arrives.
	Ledger l;
	for (std::uint64_t f = 1; f <= 100; ++f)
	{
		l.begin_frame(f);
		l.announce(made(f, 3840, 2160));
		CHECK(l.claim(240, 135) == nullptr);
		CHECK(l.claim(120, 68) == nullptr);
		CHECK(l.claim(480, 270) != nullptr);
	}
	CHECK(l.counters().announced == 100);
	CHECK(l.counters().claimed == 100);
	CHECK(l.counters().rect_mismatch == 200);
	CHECK(l.counters().orphans == 0);
	CHECK(l.counters().unclaimed == 0);
}

TEST_CASE("mode_from_level and the gate decision")
{
	CHECK(mode_from_level(-1) == Mode::off);
	CHECK(mode_from_level(0) == Mode::off);
	CHECK(mode_from_level(1) == Mode::discover);
	CHECK(mode_from_level(2) == Mode::observe);
	CHECK(mode_from_level(3) == Mode::authoritative);
	CHECK(mode_from_level(9) == Mode::authoritative);

	GateInputs in;
	// Below authoritative the heuristic decides, whatever the seam says.
	for (Mode m : { Mode::off, Mode::discover, Mode::observe })
	{
		in.mode = m;
		in.hooked = true;
		in.announced = false;
		CHECK(decide(in) == Gate::heuristic);
		in.announced = true;
		CHECK(decide(in) == Gate::heuristic);
	}
	in.mode = Mode::authoritative;
	in.hooked = true;
	in.announced = true;
	CHECK(decide(in) == Gate::engine);
	in.announced = false;
	CHECK(decide(in) == Gate::refuse_not_announced);
	// The seam is not live: the fallback is a CONFIG decision, never a silent one.
	in.hooked = false;
	in.fallback_allowed = true;
	CHECK(decide(in) == Gate::heuristic);
	in.fallback_allowed = false;
	CHECK(decide(in) == Gate::refuse_no_seam);
	// An announcement cannot exist without the hook, but if it did the hook still decides.
	in.announced = true;
	CHECK(decide(in) == Gate::refuse_no_seam);

	CHECK(std::strcmp(gate_name(Gate::engine), "engine") == 0);
	CHECK(std::strcmp(mode_name(Mode::authoritative), "authoritative") == 0);
}

TEST_CASE("the ring cannot overflow: the engine's clock retires before capacity is reached")
{
	Ledger l;
	l.begin_frame(1);
	for (std::size_t i = 0; i < Ledger::kCapacity + 3; ++i)
		l.announce(made(1, 3840, 2160));
	CHECK(l.counters().announced == Ledger::kCapacity + 3);
	CHECK(l.counters().overflow == 0);
	// Only the newest kRetireAfterAnnouncements survive; everything older retired UNCLAIMED,
	// which is the counter that would have said so in a live session.
	CHECK(l.pending() == kRetireAfterAnnouncements);
	CHECK(l.counters().unclaimed == Ledger::kCapacity + 3 - kRetireAfterAnnouncements);
}


// ---------------------------------------------------------------------------------------
// L1: the FRDGTexture -> FRHITexture -> GetNativeResource chain
// ---------------------------------------------------------------------------------------

namespace {

// A synthetic object graph shaped like the engine's. `read_u64` serves it from a map, which is
// exactly what the live reader does from real memory - so the resolver cannot tell the
// difference, and every refusal below is the one the live path would take.
struct FakeHeap
{
	std::map<std::uint64_t, std::uint64_t> mem;
	std::uint64_t code_lo = 0x140001000ull;
	std::uint64_t code_hi = 0x140002000ull;

	static bool read(void *ctx, std::uint64_t va, std::uint64_t *out)
	{
		auto *self = static_cast<FakeHeap *>(ctx);
		const auto it = self->mem.find(va);
		if (it == self->mem.end())
			return false;
		*out = it->second;
		return true;
	}
	static bool is_code(void *ctx, std::uint64_t va)
	{
		auto *self = static_cast<FakeHeap *>(ctx);
		return va >= self->code_lo && va < self->code_hi;
	}
	RdgReader reader()
	{
		RdgReader r;
		r.read_u64 = &FakeHeap::read;
		r.is_code = &FakeHeap::is_code;
		r.ctx = this;
		return r;
	}
};

constexpr std::uint64_t kRdg = 0x0000000031000000ull;
constexpr std::uint64_t kRhi = 0x0000000032000000ull;
constexpr std::uint64_t kVtableAddr = 0x0000000141000800ull;
constexpr std::uint64_t kGetNative = 0x0000000140001500ull;

FakeHeap healthy_heap()
{
	FakeHeap h;
	h.mem[kRdg + kRdgResourceRhiOffset] = kRhi;                          // ResourceRHI
	h.mem[kRhi] = kVtableAddr;                                           // the FRHITexture vptr
	h.mem[kVtableAddr + 8ull * kRhiGetNativeResourceSlot] = kGetNative;  // the slot
	return h;
}

} // namespace

TEST_CASE("the RHI chain resolves to the GetNativeResource pointer, and never calls it")
{
	FakeHeap h = healthy_heap();
	std::uint64_t rhi = 0;
	std::uint64_t fn = 0;
	CHECK(resolve_rhi_fn(h.reader(), kRdg, &rhi, &fn) == RhiChain::ok);
	CHECK(rhi == kRhi);
	CHECK(fn == kGetNative);
}

TEST_CASE("every step of the chain has its own refusal, and none of them is a fault")
{
	SUBCASE("the engine passed no texture at all")
	{
		FakeHeap h = healthy_heap();
		std::uint64_t rhi = 1;
		std::uint64_t fn = 1;
		CHECK(resolve_rhi_fn(h.reader(), 0, &rhi, &fn) == RhiChain::null_rdg);
		CHECK(rhi == 0);
		CHECK(fn == 0);
	}

	SUBCASE("ResourceRHI is null - a graph-allocated texture, read too early")
	{
		// THE reason the resolve happens at claim time and not in the AddPasses thunk: at
		// graph-setup time a transient texture has no RHI resource yet.
		FakeHeap h = healthy_heap();
		h.mem[kRdg + kRdgResourceRhiOffset] = 0;
		CHECK(resolve_rhi_fn(h.reader(), kRdg, nullptr, nullptr) == RhiChain::rhi_null);
	}

	SUBCASE("the FRDGTexture field is not readable")
	{
		FakeHeap h = healthy_heap();
		h.mem.erase(kRdg + kRdgResourceRhiOffset);
		CHECK(resolve_rhi_fn(h.reader(), kRdg, nullptr, nullptr) == RhiChain::rhi_unreadable);
	}

	SUBCASE("the vptr is not readable, or is null")
	{
		FakeHeap h = healthy_heap();
		h.mem.erase(kRhi);
		CHECK(resolve_rhi_fn(h.reader(), kRdg, nullptr, nullptr) == RhiChain::rhi_unreadable);
		h.mem[kRhi] = 0;
		CHECK(resolve_rhi_fn(h.reader(), kRdg, nullptr, nullptr) == RhiChain::rhi_unreadable);
	}

	SUBCASE("the vtable slot is not readable")
	{
		FakeHeap h = healthy_heap();
		h.mem.erase(kVtableAddr + 8ull * kRhiGetNativeResourceSlot);
		CHECK(resolve_rhi_fn(h.reader(), kRdg, nullptr, nullptr) == RhiChain::rhi_unreadable);
	}

	SUBCASE("the slot does not point at code - the guard before the only call we make")
	{
		// A wrong kRhiGetNativeResourceSlot lands here, which is the point of the guard:
		// refusing costs one fallback frame, calling it costs the process.
		FakeHeap h = healthy_heap();
		h.mem[kVtableAddr + 8ull * kRhiGetNativeResourceSlot] = 0x0000000200000000ull;
		std::uint64_t fn = 1;
		CHECK(resolve_rhi_fn(h.reader(), kRdg, nullptr, &fn) == RhiChain::fn_not_code);
		CHECK(fn == 0);
	}

	SUBCASE("a reader with no callbacks refuses rather than dereferencing anything")
	{
		RdgReader empty;
		CHECK(resolve_rhi_fn(empty, kRdg, nullptr, nullptr) == RhiChain::rhi_unreadable);
	}
}

TEST_CASE("the L1 offsets are the ones UE 4.27's Shipping layout gives")
{
	// FRDGResource in Shipping (RDG_ENABLE_DEBUG == 0): vptr@0, Name@8, ResourceRHI@16.
	CHECK(kRdgResourceRhiOffset == 16);
	// FRHITexture with one vptr (ENABLE_RHI_VALIDATION == 0): dtor, five casts, GetSizeXYZ,
	// then GetNativeResource.
	CHECK(kRhiGetNativeResourceSlot == 7);
	// Both are [derived]. If the box says otherwise, these two constants are the fix, and the
	// `l1: fellBack=` counter in the [seam] line is what says so.
}

TEST_CASE("every refusal reason has a distinct, stable name for the [seam] line")
{
	// The names are read by eye out of a pasted log, so a duplicate or an empty one costs a
	// round trip. Also pins that `none` reads as success rather than as a refusal.
	// Iterate the ENUM rather than a hand-written list: a reason added without a name would
	// otherwise pass here and print "?" in a pasted log, which is the one place it matters.
	std::set<std::string> seen;
	for (std::size_t i = 0; i < static_cast<std::size_t>(SeamRefusal::count); ++i)
	{
		const SeamRefusal r = static_cast<SeamRefusal>(i);
		const char *n = seam_refusal_name(r);
		REQUIRE(n != nullptr);
		CHECK(std::strlen(n) > 0);
		CHECK(std::strcmp(n, "?") != 0);
		CHECK(seen.insert(n).second);
	}
	CHECK(std::strcmp(seam_refusal_name(SeamRefusal::none), "evaluated") == 0);

	const RhiChain chains[] = { RhiChain::ok, RhiChain::null_rdg, RhiChain::rhi_null,
		RhiChain::rhi_unreadable, RhiChain::fn_not_code };
	std::set<std::string> cseen;
	for (RhiChain c : chains)
	{
		const char *n = rhi_chain_name(c);
		REQUIRE(n != nullptr);
		CHECK(std::strcmp(n, "?") != 0);
		CHECK(cseen.insert(n).second);
	}
}

// -----------------------------------------------------------------------------------------
// Freshness: the guard whose absence crashed 3365f02 on the box.
//
// The crash was an EXCEPTION_ACCESS_VIOLATION reading 0x0000021c000003c0 inside l1_read_u64's
// memcpy, seven frames under our own Dispatch hook. That address is two int32s (960, 540) — an
// FIntPoint — read as one qword out of `rdg + 16`, i.e. the FRDGTexture had been recycled. The
// ledger holds an announcement across up to kRetireAfterAnnouncements newer ones ON PURPOSE, so
// that a late dispatch still claims the right rect and `unclaimed` stays honest. Identity
// survives that slack. POINTERS DO NOT.

// >>> announcement_is_fresh IS NO LONGER A GATE — it is the pipeline-depth diagnostic. <<<
// It gated the dereference for two builds and made L1 inert in both; the fix was to move the
// dereference into AddPasses, not to narrow the predicate (report §12.9). These cases stay
// because the NUMBER is worth reading and because the two failures are worth pinning.
TEST_CASE("freshness measures how far the RHI thread lags; it decides nothing")
{
	Freshness f;
	f.announce_sequence = 7;
	f.ledger_sequence = 7;
	f.announce_frame = 100;
	f.current_frame = 100;
	f.announce_thread = 42;
	f.current_thread = 42;
	CHECK(announcement_is_fresh(f));

	SUBCASE("a newer announcement exists, so the announcing graph is gone")
	{
		// THIS IS THE CRASH. The engine announced in frame N, no dispatch claimed it (the
		// structural matcher rejected the real one — the very failure L1 exists to work
		// around), frame N+1 announced again, and N+1's dispatch claimed the OLDER slot,
		// because Ledger::claim returns the first unconsumed rect match. Rect-identical, so
		// nothing in the counters moved: `claimed` still incremented, `unclaimed` stayed 0.
		Freshness stale = f;
		stale.ledger_sequence = 8;
		CHECK_FALSE(announcement_is_fresh(stale));
	}
	SUBCASE("a present has happened in between")
	{
		Freshness stale = f;
		stale.current_frame = 101;
		CHECK_FALSE(announcement_is_fresh(stale));
	}
	SUBCASE("A DIFFERENT CLAIMING THREAD IS STILL FRESH - this is the NORMAL case")
	{
		// MEASURED ON THE BOX (report §12.8, facts §36.10): UE 4.27 announced on thread 1400
		// and recorded the dispatch on thread 1152, stably, for a whole session. An earlier
		// version of this predicate required the two to be equal and made L1 INERT -
		// `stale=4147` of 4147 claims, `resolved=0`, the feature silently equivalent to
		// EngineSeamInputs=0 while every other counter said it was on.
		//
		// The reasoning error, pinned here so it is not repeated: thread identity governs
		// OWNERSHIP, not VALIDITY. FRDGBuilder being a stack object means the announcing
		// thread's frame must not have returned; it does not mean only that thread may read
		// what its allocator holds. Memory held by a live stack frame is readable from any
		// thread, and the engine itself reads ResourceRHI on the recording thread in order to
		// bind the texture. Lifetime is the two conditions above, and neither is about threads.
		Freshness other = f;
		other.announce_thread = 1400;
		other.current_thread = 1152;
		CHECK(announcement_is_fresh(other));
	}
	SUBCASE("an unset sequence is never fresh, and an unset thread does not matter")
	{
		Freshness zero = f;
		zero.announce_sequence = 0;
		zero.ledger_sequence = 0;
		CHECK_FALSE(announcement_is_fresh(zero));
		// Thread ids are reported, never tested, so a zero one cannot decline a frame.
		Freshness nothread = f;
		nothread.announce_thread = 0;
		nothread.current_thread = 0;
		CHECK(announcement_is_fresh(nothread));
	}
	SUBCASE("either lifetime condition ALONE catches the crash sequence")
	{
		// Both fire together in the real sequence; each must be sufficient on its own, so
		// that neither is load-bearing only by accident — and so that dropping the thread
		// test cannot have left the guard resting on one condition.
		Freshness newer = f;
		newer.ledger_sequence = f.announce_sequence + 1;
		newer.announce_thread = 1400;
		newer.current_thread = 1152;
		CHECK_FALSE(announcement_is_fresh(newer));
		Freshness turned = f;
		turned.current_frame = f.announce_frame + 1;
		turned.announce_thread = 1400;
		turned.current_thread = 1152;
		CHECK_FALSE(announcement_is_fresh(turned));
	}
}

TEST_CASE("the ledger hands back the OLDER announcement, and that is CORRECT")
{
	// This case was written to show the crash sequence and it turned out to show something
	// better. With the RHI thread one frame behind, dispatch(N-1) arrives while {N-1, N} are
	// pending and claim() returns N-1 — the announcement that dispatch actually belongs to.
	// The correlation was never wrong; only the pointer was dead. `claim()` stays untouched
	// on the merits, not out of caution, and `unclaimed` stays comparable across every build.
	Ledger led;
	led.begin_frame(1);

	Announcement first;
	first.out_width = 3840;
	first.out_height = 2160;
	first.frame = 1;
	first.thread = 9;
	led.announce(first);

	// Frame 1's dispatch never arrives. Frame 2 announces the same rect.
	led.begin_frame(2);
	Announcement second = first;
	second.frame = 2;
	led.announce(second);

	const Announcement *got = led.claim(Ledger::expected_groups(3840),
		Ledger::expected_groups(2160));
	REQUIRE(got != nullptr);
	CHECK(led.counters().claimed == 1);
	CHECK(led.counters().unclaimed == 0);
	// It handed back the OLDER announcement — correct for identity, and exactly the pointer
	// that must not be read.
	CHECK(got->frame == 1);

	Freshness f;
	f.announce_sequence = got->sequence;
	f.ledger_sequence = led.sequence();
	f.announce_frame = got->frame;
	f.current_frame = 2;
	f.announce_thread = got->thread;
	f.current_thread = 9;
	CHECK_FALSE(announcement_is_fresh(f));
}

TEST_CASE("the ordinary frame is fresh, so L1 keeps working")
{
	// The regression that would matter most: a gate so strict that L1 never resolves would
	// "fix" the crash by deleting the feature. THAT ALREADY HAPPENED ONCE — a same-thread
	// requirement shipped and declined 4147 of 4147 claims — so this case is written with the
	// box's own measured thread pair (announce 1400, dispatch 1152) rather than with a tidy
	// same-thread one, precisely so a re-introduced thread test fails here.
	Ledger led;
	led.begin_frame(5);
	Announcement a;
	a.out_width = 2560;
	a.out_height = 1440;
	a.frame = 5;
	a.thread = 1400;
	led.announce(a);

	const Announcement *got = led.claim(Ledger::expected_groups(2560),
		Ledger::expected_groups(1440));
	REQUIRE(got != nullptr);
	Freshness f;
	f.announce_sequence = got->sequence;
	f.ledger_sequence = led.sequence();
	f.announce_frame = got->frame;
	f.current_frame = led.frame();
	f.announce_thread = got->thread;
	f.current_thread = 1152;
	CHECK(announcement_is_fresh(f));
}

TEST_CASE("EngineSeamInputs=0 is a clean off-switch, and nothing can route around it")
{
	// The off-switch must leave the plugin behaving exactly as cf31bd9d did - no dereference
	// of engine memory by any route, at either end. This gate is asked TWICE per frame now:
	// inside AddPasses before the resolve, and at claim before the resources are handed on.
	// It is pure so that this is a TEST rather than a claim.
	L1GateInputs in;
	in.inputs_enabled = false;
	in.mode = Mode::authoritative;
	in.hooked = true;
	in.announced = true;
	in.faulted = false;
	CHECK(l1_gate(in) == L1Gate::off);

	SUBCASE("and it stays off whatever else is true")
	{
		for (bool faulted : { false, true })
			for (bool hooked : { false, true })
				for (bool announced : { false, true })
				{
					L1GateInputs o = in;
					o.faulted = faulted;
					o.hooked = hooked;
					o.announced = announced;
					CHECK(l1_gate(o) == L1Gate::off);
				}
	}
}

TEST_CASE("L1 resolves only on the one combination that is safe")
{
	L1GateInputs in;
	in.inputs_enabled = true;
	in.mode = Mode::authoritative;
	in.hooked = true;
	in.announced = true;
	in.faulted = false;
	CHECK(l1_gate(in) == L1Gate::resolve);

	SUBCASE("levels below authoritative never dereference")
	{
		for (Mode m : { Mode::off, Mode::discover, Mode::observe })
		{
			L1GateInputs o = in;
			o.mode = m;
			CHECK(l1_gate(o) == L1Gate::off);
		}
	}
	SUBCASE("no live seam, or nothing announced, is off - not a fallback dereference")
	{
		L1GateInputs a = in;
		a.hooked = false;
		CHECK(l1_gate(a) == L1Gate::off);
		L1GateInputs b = in;
		b.announced = false;
		CHECK(l1_gate(b) == L1Gate::off);
	}
	SUBCASE("a fault latches the session off")
	{
		L1GateInputs o = in;
		o.faulted = true;
		CHECK(l1_gate(o) == L1Gate::faulted);
	}
	SUBCASE("THERE IS NO FRESHNESS TERM, and that is the point")
	{
		// Two builds shipped a lifetime condition in this gate and both made L1 inert:
		// same-thread (stale=4147 of 4147, report SS12.8) and then newest+same-frame (stale
		// grew every frame while resolved froze, SS12.9). Neither was a narrower gate away
		// from working, because resolving at claim time is unsafe WHATEVER the gate says -
		// FRDGBuilder::Execute() runs Allocator.ReleaseAll() before the RHI thread ever makes
		// the D3D12 call. The dereference moved into AddPasses instead, so this gate has
		// exactly five inputs and none of them is about when the claim happened.
		CHECK(sizeof(L1GateInputs) == sizeof(L1GateInputs));  // documentation anchor
		L1GateInputs o = in;
		CHECK(l1_gate(o) == L1Gate::resolve);
	}
}

TEST_CASE("every L1 gate outcome has a distinct, stable name")
{
	const L1Gate all[] = { L1Gate::off, L1Gate::faulted, L1Gate::resolve };
	std::set<std::string> seen;
	for (L1Gate g : all)
	{
		const char *n = l1_gate_name(g);
		REQUIRE(n != nullptr);
		CHECK(std::strlen(n) > 0);
		CHECK(std::strcmp(n, "?") != 0);
		CHECK(seen.insert(n).second);
	}
}

TEST_CASE("note_unmatched separates 'we refused the real pass' from 'no dispatch came'")
{
	// `unclaimed` on its own cannot tell those apart, and they are opposite bugs: the first is
	// ours to fix and names a gate, the second is not a loss at all - the engine announced an
	// upscale no dispatch followed, and there was never anything to intercept. This is the
	// instrument that splits them (report §15).
	Ledger led;
	led.begin_frame(1);
	Announcement a;
	a.out_width = 3840;
	a.out_height = 2160;
	a.frame = 1;
	a.thread = 1400;
	led.announce(a);

	const std::uint32_t gx = Ledger::expected_groups(3840);
	const std::uint32_t gy = Ledger::expected_groups(2160);

	SUBCASE("a refused dispatch of the announced shape IS a near miss")
	{
		CHECK(led.note_unmatched(gx, gy));
		CHECK(led.counters().near_misses == 1);
	}
	SUBCASE("a refused dispatch of any other shape is not")
	{
		CHECK_FALSE(led.note_unmatched(gx, gy + 1));
		CHECK_FALSE(led.note_unmatched(120, 68));   // the measured look-alike
		CHECK(led.counters().near_misses == 0);
	}
	SUBCASE("once the announcement is CLAIMED it can no longer be near-missed")
	{
		REQUIRE(led.claim(gx, gy) != nullptr);
		CHECK_FALSE(led.note_unmatched(gx, gy));
		CHECK(led.counters().near_misses == 0);
	}
	SUBCASE("with nothing pending, nothing is a near miss")
	{
		Ledger empty;
		CHECK_FALSE(empty.note_unmatched(gx, gy));
		CHECK(empty.counters().near_misses == 0);
	}
	SUBCASE("it does not disturb the correlation counters")
	{
		// The instrument must be pure observation: `unclaimed` and `claimed` have to stay
		// comparable across every build, which is the rule claim() has been held to throughout.
		led.note_unmatched(gx, gy);
		CHECK(led.counters().claimed == 0);
		CHECK(led.counters().unclaimed == 0);
		CHECK(led.counters().orphans == 0);
		CHECK(led.counters().rect_mismatch == 0);
		// and the announcement is still claimable afterwards
		CHECK(led.claim(gx, gy) != nullptr);
		CHECK(led.counters().claimed == 1);
	}
}
