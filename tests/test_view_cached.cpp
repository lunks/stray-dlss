// The View constant buffer from the engine's own CPU struct (src/core/view_cached.hpp).
//
// The scan is driven over a synthetic FViewInfo laid out in a byte vector, with a synthetic
// FViewUniformShaderParameters block planted behind a pointer at an offset the scan cannot know
// in advance. Every failure mode the live half can produce is expressible by leaving something
// out of the fake address space - an unmapped address is a REFUSAL here exactly as it is a
// refusal (never a fault) there. The latch and the decision are pure state and are driven
// directly, including with the stale-copy pair measured on the box (facts §36.20).
#include <doctest/doctest.h>

#include "core/ue4_view.hpp"
#include "core/view_cached.hpp"
#include "core/view_params.hpp"

#include <cstring>
#include <map>
#include <vector>

using namespace stray_dlss;

namespace {

constexpr std::uint64_t kViewAddr = 0x0000000140000000ull;   // the fake FViewInfo
constexpr std::uint64_t kParamsAddr = 0x0000000150000000ull; // the fake cached struct
constexpr std::uint64_t kStaleAddr = 0x0000000150010000ull;  // a second block, last frame's bytes
constexpr std::uint64_t kDecoyAddr = 0x0000000150020000ull;  // an ordinary heap object
constexpr std::uint64_t kUnmappedAddr = 0x0000000160000000ull;

// The measured layout this fixture places the field at. DISCOVERED at runtime; the test picks a
// value the scan cannot know, which is the whole point of scanning.
constexpr std::uint32_t kTrueOffset = 0x1A48;

constexpr std::uint32_t kRenderW = 1920;
constexpr std::uint32_t kRenderH = 1080;
constexpr std::uint32_t kOutW = 3840;
constexpr std::uint32_t kOutH = 2160;
constexpr std::uint32_t kBufferW = 2560; // the scene buffer left oversized from an earlier res
constexpr std::uint32_t kBufferH = 1440;

// A View prefix with a clean row 135 and the given rects. `frame` perturbs exactly the fields
// the box measured differing between the stale and the current ring copy: ClipToPrevClip,
// jitter and CameraCut (facts §36.20).
std::vector<unsigned char> make_view_prefix(float frame, float w = kRenderW, float h = kRenderH,
                                            float bw = kBufferW, float bh = kBufferH)
{
	std::vector<unsigned char> buf(ue4::kViewPrefixBytes, 0);
	auto put4 = [&](std::uint32_t row, float x, float y, float z, float ww) {
		const float v[4] = { x, y, z, ww };
		std::memcpy(buf.data() + ue4::row_to_byte(row), v, sizeof(v));
	};
	// Identity with a per-frame translation in the last row, so only row 125 moves.
	for (std::uint32_t r = 0; r < 3; ++r)
		put4(ue4::ViewRow::kClipToPrevClip + r, r == 0 ? 1.0f : 0.0f, r == 1 ? 1.0f : 0.0f,
			r == 2 ? 1.0f : 0.0f, 0.0f);
	put4(ue4::ViewRow::kClipToPrevClip + 3, 0.001f * frame, 0.0f, 0.0f, 1.0f);
	put4(ue4::ViewRow::kTemporalAAJitter, 0.0001f * frame, -0.0002f * frame, 0.0003f, -0.0004f);
	put4(ue4::ViewRow::kViewRectMin, 0.0f, 0.0f, 0.0f, 0.0f);
	put4(ue4::ViewRow::kViewSizeAndInvSize, w, h, 1.0f / w, 1.0f / h);
	put4(ue4::ViewRow::kBufferSizeAndInvSize, bw, bh, 1.0f / bw, 1.0f / bh);
	put4(ue4::ViewRow::kTemporalAAParams, frame, 8.0f, 0.05f * frame, -0.125f);
	// (denormal, P, 1/P, 0): the self-check from one read.
	float denormal = 0.0f;
	const std::uint32_t one = 1;
	std::memcpy(&denormal, &one, sizeof(denormal));
	put4(ue4::ViewRow::kPreExposureRow, denormal, 0.45194f, 1.0f / 0.45194f, 0.0f);
	put4(ue4::ViewRow::kNearPlaneRow, 10.0f, 0.0f, 0.0f, 0.0f);
	put4(ue4::ViewRow::kDeltaTimeRow, 0.016f, 0.0f, 0.0f, 0.0f);
	put4(ue4::ViewRow::kCameraCutRow, frame >= 100.0f ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f);
	return buf;
}

// A sparse address space: readable blocks by base address, nothing else mapped.
struct World
{
	std::map<std::uint64_t, std::vector<unsigned char>> blocks;

	const std::vector<unsigned char> *find(std::uint64_t va, std::size_t n, std::size_t &at) const
	{
		for (const auto &kv : blocks)
		{
			if (va >= kv.first && va + n <= kv.first + kv.second.size())
			{
				at = static_cast<std::size_t>(va - kv.first);
				return &kv.second;
			}
		}
		return nullptr;
	}
	void poke(std::uint64_t va, std::uint64_t q)
	{
		std::size_t at = 0;
		for (auto &kv : blocks)
		{
			if (va >= kv.first && va + 8 <= kv.first + kv.second.size())
			{
				at = static_cast<std::size_t>(va - kv.first);
				std::memcpy(kv.second.data() + at, &q, sizeof(q));
				return;
			}
		}
		REQUIRE(false);
	}
};

bool w_read_u64(void *ctx, std::uint64_t va, std::uint64_t *out)
{
	const auto *w = static_cast<const World *>(ctx);
	std::size_t at = 0;
	const auto *b = w->find(va, 8, at);
	if (b == nullptr)
		return false;
	std::memcpy(out, b->data() + at, 8);
	return true;
}

bool w_read_block(void *ctx, std::uint64_t va, std::size_t n, void *out)
{
	const auto *w = static_cast<const World *>(ctx);
	std::size_t at = 0;
	const auto *b = w->find(va, n, at);
	if (b == nullptr)
		return false;
	std::memcpy(out, b->data() + at, n);
	return true;
}

viewcached::Reader reader_for(World &w)
{
	viewcached::Reader r;
	r.read_u64 = &w_read_u64;
	r.read_block = &w_read_block;
	r.ctx = &w;
	return r;
}

// An FViewInfo-shaped object: 12 KB of noise in which pointer-shaped qwords point at things
// that are NOT the cached struct, plus the one real field.
World make_world(float frame = 4.0f)
{
	World w;
	std::vector<unsigned char> view(12288, 0);
	// Decoys the live scan will meet: a pointer to an ordinary heap object, a pointer into
	// unmapped memory, an FIntPoint pair that reads as one qword, a small integer, a pointer to
	// the MIDDLE of the real struct (misaligned rows), and a pointer to the stale block's
	// neighbour that is not a View at all.
	auto poke = [&](std::size_t at, std::uint64_t q) { std::memcpy(view.data() + at, &q, 8); };
	poke(0x0100, kDecoyAddr);
	poke(0x0200, kUnmappedAddr);
	poke(0x0300, 0x0000021c000003c0ull); // (960, 540) - the qword that crashed L1 once
	poke(0x0400, 7);
	poke(0x0500, kParamsAddr + 64);      // interior pointer: rows slip by 4
	poke(0x0600, kDecoyAddr + 16);
	poke(kTrueOffset, kParamsAddr);
	w.blocks[kViewAddr] = view;
	w.blocks[kParamsAddr] = make_view_prefix(frame);
	w.blocks[kDecoyAddr] = std::vector<unsigned char>(4096, 0x41);
	return w;
}

viewcached::Expectation expect_4k()
{
	viewcached::Expectation e;
	e.out_width = kOutW;
	e.out_height = kOutH;
	e.buffer_width = kBufferW;
	e.buffer_height = kBufferH;
	return e;
}

} // namespace

TEST_CASE("the scan finds the cached View struct at an offset it was not told, and only there")
{
	World w = make_world();
	const viewcached::Reader r = reader_for(w);
	viewcached::Candidate got[4];
	viewcached::StageCounts sc;
	const unsigned found = viewcached::scan(r, kViewAddr, expect_4k(), got, 4, &sc);
	REQUIRE(found == 1);
	CHECK(got[0].offset == kTrueOffset);
	CHECK(got[0].pointee == kParamsAddr);
	CHECK(got[0].params.view_size_and_inv_size.x == doctest::Approx(kRenderW));
	CHECK(got[0].params.temporal_aa_params.x == doctest::Approx(4.0f));
	CHECK(std::memcmp(got[0].prefix, w.blocks[kParamsAddr].data(), ue4::kViewPrefixBytes) == 0);

	// The stage counts tell the story: many qwords, a handful pointer-shaped, one survivor.
	CHECK(sc.qwords == viewcached::kScanWindowBytes / 8); // the whole window is walked
	CHECK(sc.pointers >= 5);
	CHECK(sc.probed == sc.pointers);
	CHECK(sc.readable >= 2);     // the real one and the interior pointer read; unmapped does not
	CHECK(sc.survivors == 1);
	CHECK_FALSE(sc.truncated);
}

TEST_CASE("check_offset names the stage that refused")
{
	const viewcached::Expectation exp = expect_4k();

	SUBCASE("the real offset is accepted")
	{
		World w = make_world();
		const viewcached::Reader r = reader_for(w);
		CHECK(viewcached::check_offset(r, kViewAddr, kTrueOffset, exp, nullptr) ==
			viewcached::Refusal::accepted);
	}
	SUBCASE("a qword that is not a pointer")
	{
		World w = make_world();
		const viewcached::Reader r = reader_for(w);
		CHECK(viewcached::check_offset(r, kViewAddr, 0x0400, exp, nullptr) ==
			viewcached::Refusal::not_a_pointer);
		// The (960, 540) FIntPoint that crashed L1 once: it PASSES every range test - aligned,
		// above 0x10000, canonical - which is exactly why the reader's readability check, not
		// the number, is what refuses it.
		CHECK(viewcached::check_offset(r, kViewAddr, 0x0300, exp, nullptr) ==
			viewcached::Refusal::unreadable);
	}
	SUBCASE("a pointer into unmapped memory is a refusal, never a fault")
	{
		World w = make_world();
		const viewcached::Reader r = reader_for(w);
		CHECK(viewcached::check_offset(r, kViewAddr, 0x0200, exp, nullptr) ==
			viewcached::Refusal::unreadable);
	}
	SUBCASE("an ordinary heap object is not a View")
	{
		World w = make_world();
		const viewcached::Reader r = reader_for(w);
		CHECK(viewcached::check_offset(r, kViewAddr, 0x0100, exp, nullptr) ==
			viewcached::Refusal::implausible);
	}
	SUBCASE("an interior pointer slips the rows and row 130 refuses it")
	{
		World w = make_world();
		const viewcached::Reader r = reader_for(w);
		// +64 = four rows: row 130 reads row 134's zeros, so plausibility fails first.
		CHECK(viewcached::check_offset(r, kViewAddr, 0x0500, exp, nullptr) !=
			viewcached::Refusal::accepted);
	}
	SUBCASE("a View with a broken row 135 is refused by the self-check")
	{
		World w = make_world();
		auto &p = w.blocks[kParamsAddr];
		const float bad[4] = { 0.0f, 2.0f, 3.0f, 0.0f }; // y*z = 6, not 1
		std::memcpy(p.data() + ue4::row_to_byte(ue4::ViewRow::kPreExposureRow), bad, sizeof(bad));
		const viewcached::Reader r = reader_for(w);
		CHECK(viewcached::check_offset(r, kViewAddr, kTrueOffset, exp, nullptr) ==
			viewcached::Refusal::row135_failed);
	}
	SUBCASE("a view larger than the announcement's own rect - the measured 4088x4088 impostor")
	{
		World w = make_world();
		w.blocks[kParamsAddr] = make_view_prefix(4.0f, 4088.0f, 4088.0f, 4096.0f, 4096.0f);
		const viewcached::Reader r = reader_for(w);
		CHECK(viewcached::check_offset(r, kViewAddr, kTrueOffset, exp, nullptr) ==
			viewcached::Refusal::does_not_fit);
	}
	SUBCASE("a view below the engine's 0.5 minimum fraction - the 64x41 capture view")
	{
		World w = make_world();
		w.blocks[kParamsAddr] = make_view_prefix(4.0f, 64.0f, 41.0f, 64.0f, 41.0f);
		const viewcached::Reader r = reader_for(w);
		CHECK(viewcached::check_offset(r, kViewAddr, kTrueOffset, exp, nullptr) ==
			viewcached::Refusal::fraction);
	}
	SUBCASE("row 132 must equal the depth extent L1 resolved")
	{
		World w = make_world();
		w.blocks[kParamsAddr] = make_view_prefix(4.0f, kRenderW, kRenderH, 1920.0f, 1080.0f);
		const viewcached::Reader r = reader_for(w);
		CHECK(viewcached::check_offset(r, kViewAddr, kTrueOffset, exp, nullptr) ==
			viewcached::Refusal::buffer_mismatch);
	}
	SUBCASE("with no rect and no extent to compare against, invent no refusal")
	{
		World w = make_world();
		w.blocks[kParamsAddr] = make_view_prefix(4.0f, 4088.0f, 4088.0f, 1.0f, 1.0f);
		const viewcached::Reader r = reader_for(w);
		viewcached::Expectation none;
		CHECK(viewcached::check_offset(r, kViewAddr, kTrueOffset, none, nullptr) ==
			viewcached::Refusal::accepted);
	}
	SUBCASE("a nonsense FViewInfo pointer and an unwired reader both refuse")
	{
		World w = make_world();
		const viewcached::Reader r = reader_for(w);
		CHECK(viewcached::check_offset(r, 0, kTrueOffset, exp, nullptr) ==
			viewcached::Refusal::not_a_pointer);
		CHECK(viewcached::check_offset(r, 0x3c0ull, kTrueOffset, exp, nullptr) ==
			viewcached::Refusal::not_a_pointer);
		viewcached::Reader broken;
		broken.ctx = &w;
		CHECK(viewcached::check_offset(broken, kViewAddr, kTrueOffset, exp, nullptr) ==
			viewcached::Refusal::unreadable);
		viewcached::Candidate got[2];
		CHECK(viewcached::scan(broken, kViewAddr, exp, got, 2, nullptr) == 0);
		CHECK(viewcached::scan(r, 0, exp, got, 2, nullptr) == 0);
	}
}

TEST_CASE("the scan reports MORE THAN ONE survivor rather than taking the first")
{
	// A second field pointing at a second, equally valid block. Reporting 2 is what lets the
	// latch refuse; returning the first would be this file's own bug one level down.
	World w = make_world();
	constexpr std::uint32_t kSecondOffset = 0x2000;
	w.poke(kViewAddr + kSecondOffset, kStaleAddr);
	w.blocks[kStaleAddr] = make_view_prefix(3.0f);
	const viewcached::Reader r = reader_for(w);
	viewcached::Candidate got[4];
	viewcached::StageCounts sc;
	CHECK(viewcached::scan(r, kViewAddr, expect_4k(), got, 4, &sc) == 2);
	CHECK(sc.survivors == 2);
	CHECK(got[0].offset == kTrueOffset);
	CHECK(got[1].offset == kSecondOffset);
}

TEST_CASE("the scan is bounded and says so")
{
	// A window made entirely of pointer-shaped qwords into a readable decoy exhausts the probe
	// budget; the result must be marked truncated rather than reported as a clean `absent`.
	World w;
	std::vector<unsigned char> view(8 * (viewcached::kMaxProbesPerScan + 64), 0);
	for (std::size_t o = 0; o + 8 <= view.size(); o += 8)
		std::memcpy(view.data() + o, &kDecoyAddr, 8);
	w.blocks[kViewAddr] = view;
	w.blocks[kDecoyAddr] = std::vector<unsigned char>(4096, 0);
	const viewcached::Reader r = reader_for(w);
	viewcached::Candidate got[1];
	viewcached::StageCounts sc;
	CHECK(viewcached::scan(r, kViewAddr, expect_4k(), got, 1, &sc, view.size()) == 0);
	CHECK(sc.truncated);
	CHECK(sc.probed == viewcached::kMaxProbesPerScan);
}

TEST_CASE("THE STALE RING COPY (facts §36.20): the struct against what the search read")
{
	// MEASURED ON THE BOX: 480x270 groups covering 3840x2160; two bound View buffers, b3 and b4,
	// BOTH 1920x1080, both passing plausibility, row 135, the fit bound and the 0.5 minimum
	// fraction, differing only in ClipToPrevClip, jitter and CameraCut. The search took b3 -
	// the lower register - which was the previous frame's ring copy.
	const auto current = make_view_prefix(4.0f);   // what the engine's struct holds (b4)
	const auto stale = make_view_prefix(3.0f);     // last frame's bytes still bound at b3

	SUBCASE("the two really are indistinguishable by every structural test")
	{
		ue4::ViewParams a{};
		ue4::ViewParams b{};
		REQUIRE(ue4::parse_view_params(current.data(), current.size(), a));
		REQUIRE(ue4::parse_view_params(stale.data(), stale.size(), b));
		CHECK(ue4::view_params_plausible(a));
		CHECK(ue4::view_params_plausible(b));
		CHECK(ue4::pre_exposure_plausible(a));
		CHECK(ue4::pre_exposure_plausible(b));
		CHECK(ue4::view_fits_dispatch(a, kOutW, kOutH));
		CHECK(ue4::view_fits_dispatch(b, kOutW, kOutH));
		CHECK(ue4::view_fraction_plausible(a, kOutW, kOutH));
		CHECK(ue4::view_fraction_plausible(b, kOutW, kOutH));
		CHECK(ue4::views_differ_temporally(a, b)); // and yet DLSS would get different motion
	}
	SUBCASE("byte comparison separates them and names exactly the measured fields")
	{
		const viewcached::FieldDiff d = viewcached::compare_prefix(current.data(), stale.data());
		CHECK(d.any);
		CHECK(d.first_row == ue4::ViewRow::kClipToPrevClip + 3);
		CHECK(d.clip_to_prev_clip);
		CHECK(d.jitter);
		CHECK_FALSE(d.camera_cut);   // both frames are ordinary frames here
		CHECK_FALSE(d.pre_exposure);
		CHECK_FALSE(d.rect);         // the same view: rects identical, which is the whole trap
		char line[160];
		viewcached::describe_diff(d, line, sizeof(line));
		CHECK(std::strstr(line, "ClipToPrevClip") != nullptr);
		CHECK(std::strstr(line, "jitter") != nullptr);
		CHECK(std::strstr(line, "CameraCut") == nullptr);
	}
	SUBCASE("a cut frame against a non-cut copy also names CameraCut")
	{
		const auto cut = make_view_prefix(100.0f);
		const viewcached::FieldDiff d = viewcached::compare_prefix(cut.data(), stale.data());
		CHECK(d.camera_cut);
		CHECK(d.clip_to_prev_clip);
	}
	SUBCASE("identical bytes are identical")
	{
		const viewcached::FieldDiff d = viewcached::compare_prefix(current.data(), current.data());
		CHECK_FALSE(d.any);
		char line[32];
		viewcached::describe_diff(d, line, sizeof(line));
		CHECK(std::strcmp(line, "identical") == 0);
	}
	SUBCASE("a difference outside every consumer row says so rather than naming one")
	{
		auto other = current;
		other[ue4::row_to_byte(ue4::ViewRow::kNearPlaneRow)] ^= 0x01;
		const viewcached::FieldDiff d = viewcached::compare_prefix(current.data(), other.data());
		CHECK(d.any);
		CHECK_FALSE(d.clip_to_prev_clip);
		CHECK_FALSE(d.jitter);
		char line[160];
		viewcached::describe_diff(d, line, sizeof(line));
		CHECK(std::strstr(line, "no consumer field differs") != nullptr);
	}
	SUBCASE("null inputs are a difference, not a crash")
	{
		CHECK(viewcached::compare_prefix(nullptr, stale.data()).any);
		CHECK(viewcached::compare_prefix(current.data(), nullptr).any);
	}

	// And the latch, driven with the measured session: a stream of announcements in which one
	// in ~300 finds the search stale. The latch must land on the offset anyway, and after it
	// the stale frames must land in `disagree` - the search's error - never demote the latch.
	SUBCASE("the latch survives the 0.33% and files it under the search's name")
	{
		viewcached::Latch l;
		viewcached::Observation ok;
		ok.survivors = 1;
		ok.offset = kTrueOffset;
		ok.compared = true;
		ok.bytes_equal = true;
		viewcached::Observation stale_search = ok;
		stale_search.bytes_equal = false;

		// The very first dispatch happens to be a stale one: the run restarts, nothing latches.
		l.observe(stale_search);
		CHECK(l.state() == viewcached::LatchState::searching);
		CHECK(l.pre_latch_disagreements() == 1);
		for (unsigned i = 0; i < viewcached::kLatchAgreements; ++i)
			l.observe(ok);
		REQUIRE(l.state() == viewcached::LatchState::latched);
		CHECK(l.offset() == kTrueOffset);

		// 300 frames, one of them stale.
		for (unsigned i = 0; i < 299; ++i)
			l.observe(ok);
		l.observe(stale_search);
		CHECK(l.state() == viewcached::LatchState::latched);
		CHECK(l.agree() == 299);
		CHECK(l.disagree() == 1);
		CHECK(l.offset() == kTrueOffset);

		// Once latched, the decision hands DLSS the struct's View at level 2 and not at level 1.
		viewcached::DecisionInputs in;
		in.latch = l.state();
		in.latched_offset = l.offset();
		in.carried = true;
		in.carried_offset = kTrueOffset;
		in.mode = viewcached::Mode::authoritative;
		CHECK(viewcached::use_engine_view(in));
		in.mode = viewcached::Mode::discover;
		CHECK_FALSE(viewcached::use_engine_view(in));
	}
}

TEST_CASE("the latch needs a RUN of byte-exact agreements, and refuses ambiguity for the session")
{
	viewcached::Observation a;
	a.survivors = 1;
	a.offset = kTrueOffset;
	a.compared = true;
	a.bytes_equal = true;
	viewcached::Observation b = a;
	b.offset = kTrueOffset + 8;

	SUBCASE("one agreement is not enough; kLatchAgreements is")
	{
		viewcached::Latch l;
		for (unsigned i = 0; i < viewcached::kLatchAgreements - 1; ++i)
		{
			l.observe(a);
			CHECK(l.state() == viewcached::LatchState::searching);
		}
		l.observe(a);
		CHECK(l.state() == viewcached::LatchState::latched);
		CHECK(l.offset() == kTrueOffset);
	}
	SUBCASE("a different single answer restarts the run rather than averaging them")
	{
		viewcached::Latch l;
		for (unsigned i = 0; i < viewcached::kLatchAgreements - 1; ++i)
			l.observe(a);
		l.observe(b);
		CHECK(l.state() == viewcached::LatchState::searching);
		CHECK(l.agreements() == 1);
	}
	SUBCASE("an uncompared announcement neither extends nor resets the run")
	{
		viewcached::Latch l;
		for (unsigned i = 0; i < viewcached::kLatchAgreements - 1; ++i)
			l.observe(a);
		viewcached::Observation u = a;
		u.compared = false;
		l.observe(u);
		CHECK(l.state() == viewcached::LatchState::searching);
		CHECK(l.agreements() == viewcached::kLatchAgreements - 1);
		CHECK(l.uncompared() == 1);
		l.observe(a);
		CHECK(l.state() == viewcached::LatchState::latched);
	}
	SUBCASE("a wrong offset that never matches the bound bytes never latches")
	{
		viewcached::Latch l;
		viewcached::Observation wrong = a;
		wrong.bytes_equal = false;
		for (unsigned i = 0; i < 10 * viewcached::kLatchAgreements; ++i)
			l.observe(wrong);
		CHECK(l.state() == viewcached::LatchState::searching);
		CHECK(l.agreements() == 0);
		CHECK(l.pre_latch_disagreements() == 10 * viewcached::kLatchAgreements);
	}
	SUBCASE("two survivors on ONE announcement is sticky ambiguity, never a coin flip")
	{
		viewcached::Latch l;
		viewcached::Observation two = a;
		two.survivors = 2;
		l.observe(two);
		CHECK(l.state() == viewcached::LatchState::ambiguous);
		for (unsigned i = 0; i < 4 * viewcached::kLatchAgreements; ++i)
			l.observe(a);
		CHECK(l.state() == viewcached::LatchState::ambiguous);
		CHECK(l.ambiguous_observations() == 1);
	}
	SUBCASE("a latched offset that is then reported from elsewhere is ambiguity arriving late")
	{
		viewcached::Latch l;
		for (unsigned i = 0; i < viewcached::kLatchAgreements; ++i)
			l.observe(a);
		REQUIRE(l.state() == viewcached::LatchState::latched);
		l.observe(b);
		CHECK(l.state() == viewcached::LatchState::ambiguous);
	}
	SUBCASE("no survivor for long enough says ABSENT rather than rolling on silently")
	{
		viewcached::Latch l;
		viewcached::Observation none;
		for (std::uint64_t i = 0; i < viewcached::kAbsentAfter; ++i)
			l.observe(none);
		CHECK(l.state() == viewcached::LatchState::absent);
		CHECK(l.empty_observations() == viewcached::kAbsentAfter);
	}
	SUBCASE("ABSENT counts CONSECUTIVE empties: a survivor in between restarts the count")
	{
		viewcached::Latch l;
		viewcached::Observation none;
		for (std::uint64_t i = 0; i < viewcached::kAbsentAfter - 1; ++i)
			l.observe(none);
		l.observe(a);
		for (std::uint64_t i = 0; i < viewcached::kAbsentAfter - 1; ++i)
			l.observe(none);
		CHECK(l.state() == viewcached::LatchState::searching);
	}
	SUBCASE("ABSENT is STICKY IN THE STATE MACHINE - it never re-arms within a session")
	{
		// The caller stops scanning once the verdict is terminal, so a state machine that quietly
		// re-armed here would mean the TESTED model and the SHIPPED behaviour disagreed, with the
		// tested one being the more permissive - the correction from the ecd3d2b audit.
		viewcached::Latch l;
		viewcached::Observation none;
		for (std::uint64_t i = 0; i < viewcached::kAbsentAfter; ++i)
			l.observe(none);
		REQUIRE(l.state() == viewcached::LatchState::absent);
		for (unsigned i = 0; i < 4 * viewcached::kLatchAgreements; ++i)
			l.observe(a);
		CHECK(l.state() == viewcached::LatchState::absent);
		CHECK(l.offset() == 0);
	}
	SUBCASE("an empty announcement AFTER latching is `unverified`, not a demotion")
	{
		viewcached::Latch l;
		for (unsigned i = 0; i < viewcached::kLatchAgreements; ++i)
			l.observe(a);
		viewcached::Observation none;
		for (std::uint64_t i = 0; i < 2 * viewcached::kAbsentAfter; ++i)
			l.observe(none);
		CHECK(l.state() == viewcached::LatchState::latched);
		CHECK(l.unverified() == 2 * viewcached::kAbsentAfter);
	}
}

TEST_CASE("the decision never uses the struct without a latch, a carry and the latched offset")
{
	viewcached::DecisionInputs in;
	in.mode = viewcached::Mode::authoritative;
	in.latch = viewcached::LatchState::latched;
	in.latched_offset = kTrueOffset;
	in.carried = true;
	in.carried_offset = kTrueOffset;
	CHECK(viewcached::use_engine_view(in));

	SUBCASE("below authoritative the search is the source")
	{
		in.mode = viewcached::Mode::discover;
		CHECK_FALSE(viewcached::use_engine_view(in));
		in.mode = viewcached::Mode::off;
		CHECK_FALSE(viewcached::use_engine_view(in));
	}
	SUBCASE("before the latch, or after a terminal verdict, the search is the source")
	{
		in.latch = viewcached::LatchState::searching;
		CHECK_FALSE(viewcached::use_engine_view(in));
		in.latch = viewcached::LatchState::ambiguous;
		CHECK_FALSE(viewcached::use_engine_view(in));
		in.latch = viewcached::LatchState::absent;
		CHECK_FALSE(viewcached::use_engine_view(in));
	}
	SUBCASE("an announcement that carried nothing falls back to the search")
	{
		in.carried = false;
		CHECK_FALSE(viewcached::use_engine_view(in));
	}
	SUBCASE("a carry from an offset other than the latched one is refused")
	{
		in.carried_offset = kTrueOffset + 8;
		CHECK_FALSE(viewcached::use_engine_view(in));
	}
}

TEST_CASE("the mode ladder declares level 3 and refuses to pretend it exists")
{
	CHECK(viewcached::mode_from_level(0) == viewcached::Mode::off);
	CHECK(viewcached::mode_from_level(-3) == viewcached::Mode::off);
	CHECK(viewcached::mode_from_level(1) == viewcached::Mode::discover);
	CHECK(viewcached::mode_from_level(2) == viewcached::Mode::authoritative);
	CHECK(viewcached::mode_from_level(3) == viewcached::Mode::exclusive);
	CHECK(viewcached::mode_from_level(9) == viewcached::Mode::exclusive);

	CHECK(viewcached::mode_is_implemented(viewcached::Mode::off));
	CHECK(viewcached::mode_is_implemented(viewcached::Mode::discover));
	CHECK(viewcached::mode_is_implemented(viewcached::Mode::authoritative));
	CHECK_FALSE(viewcached::mode_is_implemented(viewcached::Mode::exclusive));

	CHECK(std::strcmp(viewcached::mode_name(viewcached::Mode::authoritative), "authoritative") == 0);
	CHECK(std::strcmp(viewcached::latch_state_name(viewcached::LatchState::absent), "absent") == 0);
	CHECK(std::strcmp(viewcached::refusal_name(viewcached::Refusal::buffer_mismatch),
		"buffer-size-mismatch") == 0);
}
