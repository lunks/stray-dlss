// The dataflow walk, proven on synthetic Unreal-shaped frames.
//
// These tests are the correctness proof for the pass finder: the developer cannot run the
// game, so every discrimination the walk must make — TAA in, SSR denoiser out, motion blur
// out, the 0xd2e4d8c23c362ed1 composite out — is pinned here instead. Each frame below is
// shaped like UE 4.27's real post chain (CLAUDE.md §2.3): base pass, SSR denoiser with its
// own history, lighting composite, TAA, motion blur, bloom, tonemapper with the 3D
// colour-grading LUT, Slate composite into the back buffer.

#include <doctest/doctest.h>

#include "core/pass_walk.hpp"

#include <cstring>
#include <string>

using namespace stray_dlss;

namespace {

// Resource identities. Values are arbitrary; only equality matters.
constexpr std::uint64_t kEyeRes = 0x200;         // 1x1 eye adaptation, bound every frame
constexpr std::uint64_t kLutRes = 0x300;         // the 3D colour-grading LUT
constexpr std::uint64_t kGBufferRes = 0xA00;
constexpr std::uint64_t kDepthRes = 0xD00;       // depth+stencil, ONE resource, two views
constexpr std::uint64_t kVelocityRes = 0xE00;
constexpr std::uint64_t kSsrHistRes = 0x510;
constexpr std::uint64_t kSsrOutRes = 0x500;
constexpr std::uint64_t kSceneColourRes = 0x600;
constexpr std::uint64_t kTaaHistRes = 0x700;
constexpr std::uint64_t kTaaOutRes = 0x710;
constexpr std::uint64_t kTaaHalfRes = 0x720;
constexpr std::uint64_t kMbOutRes = 0x800;
constexpr std::uint64_t kBloomRes = 0x900;
constexpr std::uint64_t kLdrRes = 0x1000;
constexpr std::uint64_t kBackBufferRes = 0x1100;
constexpr std::uint64_t kDummyRes = 0x9999;      // the 1x1 BlackDummy

// Pipeline identities. Arbitrary except that none collides with the exclusion list.
constexpr std::uint64_t kSsrHash = 0x5511551155115511ull;
constexpr std::uint64_t kCompositeHash = 0xC0C0C0C0C0C0C0C0ull;
constexpr std::uint64_t kTaaHash = 0x7AA17AA17AA17AA1ull;
constexpr std::uint64_t kMbHash = 0xB10CB10CB10CB10Cull;
constexpr std::uint64_t kBloomHash = 0xB100B100B100B100ull;

// The measured configuration: 1920x1080 render, 3840x2160 output. (CLAUDE.md §2.3.1)
constexpr std::uint32_t kRW = 1920, kRH = 1080;
constexpr std::uint32_t kOW = 3840, kOH = 2160;

FrameEvent event(FrameEvent::Kind kind, std::uint64_t seq, std::uint64_t hash,
                 std::vector<BoundTexture> srvs, std::vector<BoundTexture> outputs)
{
	FrameEvent e;
	e.kind = kind;
	e.sequence = seq;
	e.pipeline_hash = hash;
	e.srvs = std::move(srvs);
	e.outputs = std::move(outputs);
	return e;
}

// A realistic frame. Sequence numbers are spaced so tests can splice events in between.
std::vector<FrameEvent> realistic_frame()
{
	std::vector<FrameEvent> f;

	// Base pass: writes the gbuffer, velocity (r.BasePassOutputsVelocity=True, §2.3.1) and
	// depth. A draw, with thousands of irrelevant material SRVs elided.
	f.push_back(event(FrameEvent::Kind::draw, 10, 0, {},
		{
			{ 0, kGBufferRes, TexFormat::r8g8b8a8_unorm, kRW, kRH },
			{ 1, kVelocityRes, TexFormat::r16g16b16a16_unorm, kRW, kRH },
			{ 2, kDepthRes, TexFormat::r32_float_x8x24_typeless, kRW, kRH },
		}));

	// SSR denoiser: reads depth, velocity and its OWN history, writes a full-res HDR
	// intermediate. Exactly the look-alike shape that defeated the structural signature.
	f.push_back(event(FrameEvent::Kind::dispatch, 20, kSsrHash,
		{
			{ 0, kDepthRes, TexFormat::r32_float_x8x24_typeless, kRW, kRH },
			{ 1, kVelocityRes, TexFormat::r16g16b16a16_unorm, kRW, kRH },
			{ 2, kSsrHistRes, TexFormat::r16g16b16a16_float, kRW, kRH },
		},
		{
			{ 0, kSsrOutRes, TexFormat::r16g16b16a16_float, kRW, kRH },
		}));

	// Lighting composite: folds the SSR result into scene colour. No velocity, no history.
	f.push_back(event(FrameEvent::Kind::dispatch, 30, kCompositeHash,
		{
			{ 0, kGBufferRes, TexFormat::r8g8b8a8_unorm, kRW, kRH },
			{ 1, kSsrOutRes, TexFormat::r16g16b16a16_float, kRW, kRH },
			{ 2, kDepthRes, TexFormat::r32_float_x8x24_typeless, kRW, kRH },
		},
		{
			{ 0, kSceneColourRes, TexFormat::r16g16b16a16_float, kRW, kRH },
		}));

	// The TAA: the measured §2.3 binding set — eye adaptation, scene colour, the
	// depth+stencil pair over one resource, velocity, history — upsampling into the full-res
	// output plus the optional half-res companion.
	f.push_back(event(FrameEvent::Kind::dispatch, 40, kTaaHash,
		{
			{ 0, kEyeRes, TexFormat::r32g32b32a32_float, 1, 1 },
			{ 1, kSceneColourRes, TexFormat::r16g16b16a16_float, kRW, kRH },
			{ 2, kDepthRes, TexFormat::r32_float_x8x24_typeless, kRW, kRH },
			{ 3, kVelocityRes, TexFormat::r16g16b16a16_unorm, kRW, kRH },
			{ 4, kDepthRes, TexFormat::x32_typeless_g8x24_uint, kRW, kRH },
			{ 5, kTaaHistRes, TexFormat::r16g16b16a16_float, kRW, kRH },
		},
		{
			{ 0, kTaaOutRes, TexFormat::r16g16b16a16_float, kOW, kOH },
			{ 1, kTaaHalfRes, TexFormat::r16g16b16a16_float, kRW, kRH },
		}));

	// Motion blur: full-res HDR compute, DOWNSTREAM of the TAA, reads velocity — but never
	// its own previous output. (PostProcessMotionBlur.cpp:366)
	f.push_back(event(FrameEvent::Kind::dispatch, 50, kMbHash,
		{
			{ 0, kTaaOutRes, TexFormat::r16g16b16a16_float, kOW, kOH },
			{ 1, kVelocityRes, TexFormat::r16g16b16a16_unorm, kRW, kRH },
			{ 2, kDepthRes, TexFormat::r32_float_x8x24_typeless, kRW, kRH },
		},
		{
			{ 0, kMbOutRes, TexFormat::r16g16b16a16_float, kOW, kOH },
		}));

	// Bloom: reads the motion-blurred scene, writes a small HDR chain.
	f.push_back(event(FrameEvent::Kind::dispatch, 60, kBloomHash,
		{
			{ 0, kMbOutRes, TexFormat::r16g16b16a16_float, kOW, kOH },
		},
		{
			{ 0, kBloomRes, TexFormat::r11g11b10_float, 960, 540 },
		}));

	// Tonemap: the anchor. The ONLY event binding a 3D texture SRV — the colour-grading LUT
	// (PostProcessTonemap.cpp:533,765). Its largest HDR input is the motion-blurred scene.
	f.push_back(event(FrameEvent::Kind::draw, 70, 0,
		{
			{ 0, kMbOutRes, TexFormat::r16g16b16a16_float, kOW, kOH },
			{ 1, kBloomRes, TexFormat::r11g11b10_float, 960, 540 },
			{ 2, kLutRes, TexFormat::unknown, 32, 32, 0, true }, // is_3d
			{ 3, kEyeRes, TexFormat::r32g32b32a32_float, 1, 1 },
		},
		{
			{ 0, kLdrRes, TexFormat::r10g10b10a2_unorm, kOW, kOH },
		}));

	// Slate/UI composite into the back buffer.
	f.push_back(event(FrameEvent::Kind::draw, 80, 0,
		{
			{ 0, kLdrRes, TexFormat::r10g10b10a2_unorm, kOW, kOH },
		},
		{
			{ 0, kBackBufferRes, TexFormat::r10g10b10a2_unorm, kOW, kOH },
		}));

	return f;
}

bool default_history(std::uint64_t hash)
{
	// Both temporal passes have demonstrated the round-trip — the whole point is that the
	// walk must still tell them apart.
	return hash == kTaaHash || hash == kSsrHash;
}

bool chain_contains(const WalkResult &r, std::uint64_t hash)
{
	for (const auto &hop : r.chain)
		if (hop.pipeline_hash == hash)
			return true;
	return false;
}

bool reason_contains(const WalkResult &r, const char *needle)
{
	return std::string(r.reason).find(needle) != std::string::npos;
}

} // namespace

TEST_CASE("a realistic frame selects the TAA and nothing else")
{
	const auto frame = realistic_frame();
	const auto r = find_temporal_pass(frame, kBackBufferRes, default_history);

	CHECK(r.taa_hash == kTaaHash);

	// The chain is anchor -> motion blur -> TAA: the walk went THROUGH motion blur (velocity
	// but no history) and stopped at the TAA without ever visiting anything upstream of it.
	REQUIRE(r.chain.size() == 3);
	CHECK(r.chain[0].sequence == 70); // the tonemap anchor
	CHECK(r.chain[1].pipeline_hash == kMbHash);
	CHECK(r.chain[2].pipeline_hash == kTaaHash);

	// The SSR denoiser has a history round-trip and reads depth and velocity, yet it is
	// nowhere in the chain: the walk terminated at the TAA before reaching it. Direction
	// plus early termination is the discrimination.
	CHECK_FALSE(chain_contains(r, kSsrHash));
	CHECK_FALSE(chain_contains(r, kCompositeHash));
}

TEST_CASE("motion blur is a hop, never the selection")
{
	// Full-res HDR compute, downstream of the TAA, reads velocity — everything the
	// structural signature liked. What it lacks is the history round-trip.
	const auto frame = realistic_frame();
	const auto r = find_temporal_pass(frame, kBackBufferRes, default_history);

	CHECK(r.taa_hash != kMbHash);
	CHECK(chain_contains(r, kMbHash)); // visited, walked through, reported
}

TEST_CASE("a copy on the chain is walked through")
{
	auto frame = realistic_frame();

	// The tonemapper reads a COPY of the motion-blur output rather than the output itself.
	constexpr std::uint64_t kCopyRes = 0x1234;
	frame.push_back(event(FrameEvent::Kind::copy, 65, 0,
		{ { 0, kMbOutRes, TexFormat::r16g16b16a16_float, kOW, kOH } },
		{ { 0, kCopyRes, TexFormat::r16g16b16a16_float, kOW, kOH } }));
	for (auto &e : frame)
		if (e.sequence == 70)
			e.srvs[0] = { 0, kCopyRes, TexFormat::r16g16b16a16_float, kOW, kOH };

	const auto r = find_temporal_pass(frame, kBackBufferRes, default_history);
	CHECK(r.taa_hash == kTaaHash);
	REQUIRE(r.chain.size() == 4); // anchor -> copy -> motion blur -> TAA
	CHECK(r.chain[1].kind == FrameEvent::Kind::copy);
}

TEST_CASE("the 0xd2e4d8c23c362ed1 composite is rejected by the write-set gate")
{
	// The measured failure (CLAUDE.md §1): the pass that drives the display chain at 1:1
	// writes TWO full-res colour targets plus depth plus stencil. Replacing it with a
	// single-output DLSS evaluation left depth and stencil unwritten and broke the frame.
	// The walk must refuse it however well its inputs match.
	constexpr std::uint64_t kBadCompositeHash = 0xD2E4D8C23C362ED1ull;
	constexpr std::uint64_t kBadOutRes = 0x2000;
	constexpr std::uint64_t kBadOut2Res = 0x2001;

	std::vector<FrameEvent> frame;
	frame.push_back(realistic_frame()[0]); // base pass

	frame.push_back(event(FrameEvent::Kind::dispatch, 40, kBadCompositeHash,
		{
			{ 0, kSceneColourRes, TexFormat::r16g16b16a16_float, kRW, kRH },
			{ 1, kVelocityRes, TexFormat::r16g16b16a16_unorm, kRW, kRH },
			{ 2, kDepthRes, TexFormat::r32_float_x8x24_typeless, kRW, kRH },
		},
		{
			{ 0, kBadOutRes, TexFormat::r16g16b16a16_float, kRW, kRH },
			{ 1, kDummyRes, TexFormat::unknown, 1, 1 },
			{ 2, kBadOut2Res, TexFormat::r11g11b10_float, kRW, kRH },
			{ 3, kDepthRes, TexFormat::r32_float_x8x24_typeless, kRW, kRH },
			{ 5, kDepthRes, TexFormat::x32_typeless_g8x24_uint, kRW, kRH },
		}));

	frame.push_back(event(FrameEvent::Kind::draw, 70, 0,
		{
			{ 0, kBadOutRes, TexFormat::r16g16b16a16_float, kRW, kRH },
			{ 1, kLutRes, TexFormat::unknown, 32, 32, 0, true },
		},
		{
			{ 0, kLdrRes, TexFormat::r10g10b10a2_unorm, kOW, kOH },
		}));

	const auto history = [](std::uint64_t h) { return h == kBadCompositeHash; };
	const auto r = find_temporal_pass(realistic_frame(), kBackBufferRes, history);
	// In the realistic frame that hash is absent, so nothing is history-owning: not found.
	CHECK(r.taa_hash == 0);

	const auto r2 = find_temporal_pass(frame, kBackBufferRes, history);
	CHECK(r2.taa_hash == 0);
	CHECK(reason_contains(r2, "write set"));
	// The chain still names the pass, because "found it but cannot replace it" is the
	// diagnostic that changes the plan.
	CHECK(chain_contains(r2, kBadCompositeHash));
}

TEST_CASE("the write-set gate itself")
{
	const BoundTexture full{ 0, 0x1, TexFormat::r16g16b16a16_float, kOW, kOH };
	const BoundTexture half{ 1, 0x2, TexFormat::r16g16b16a16_float, kOW / 2, kOH / 2 };
	const BoundTexture full2{ 2, 0x3, TexFormat::r11g11b10_float, kOW, kOH };
	const BoundTexture depth{ 3, 0x4, TexFormat::r32_float_x8x24_typeless, kOW, kOH };
	const BoundTexture stencil{ 4, 0x5, TexFormat::x32_typeless_g8x24_uint, kOW, kOH };
	const BoundTexture ldr{ 5, 0x6, TexFormat::r8g8b8a8_unorm, kOW, kOH };
	const BoundTexture mv{ 6, 0x7, TexFormat::r16g16_float, kOW, kOH };
	const BoundTexture dummy{ 7, 0x8, TexFormat::unknown, 1, 1 };

	const char *why = "";
	CHECK(is_replaceable_write_set({ full }, &why));
	CHECK(is_replaceable_write_set({ full, half }, &why));
	CHECK(is_replaceable_write_set({ full, dummy, dummy }, &why));

	CHECK_FALSE(is_replaceable_write_set({}, &why));
	CHECK_FALSE(is_replaceable_write_set({ full, full2 }, &why));   // second full-res target
	CHECK_FALSE(is_replaceable_write_set({ full, depth }, &why));   // writes depth
	CHECK_FALSE(is_replaceable_write_set({ full, stencil }, &why)); // writes stencil
	CHECK_FALSE(is_replaceable_write_set({ full, ldr }, &why));     // non-HDR target
	CHECK_FALSE(is_replaceable_write_set({ full, mv }, &why));      // non-colour target
	CHECK_FALSE(is_replaceable_write_set({ dummy }, &why));         // writes nothing real
}

TEST_CASE("a camera-cut frame degrades to a clear reason, never a wrong answer")
{
	// On a cut UE4 substitutes the 1x1 BlackDummy for the TAA's velocity and history
	// (CLAUDE.md §2.3). The TAA then reads no live velocity, so it cannot be CONFIRMED this
	// frame — and hopping through it would put the SSR denoiser, a genuine history-owning
	// velocity-reading pass, next on the chain. The walk must stop and say why instead.
	auto frame = realistic_frame();
	for (auto &e : frame)
	{
		if (e.pipeline_hash == kTaaHash)
		{
			e.srvs[3] = { 3, kDummyRes, TexFormat::r8g8b8a8_unorm, 1, 1 }; // velocity
			e.srvs[5] = { 5, kDummyRes, TexFormat::r8g8b8a8_unorm, 1, 1 }; // history
		}
	}

	const auto r = find_temporal_pass(frame, kBackBufferRes, default_history);
	CHECK(r.taa_hash == 0);
	CHECK(reason_contains(r, "camera cut"));
	CHECK_FALSE(chain_contains(r, kSsrHash));
}

TEST_CASE("a history-owning DRAW is walked through: only a compute pass can be the TAA")
{
	auto frame = realistic_frame();
	for (auto &e : frame)
		if (e.pipeline_hash == kMbHash)
			e.kind = FrameEvent::Kind::draw;

	const auto history = [](std::uint64_t h) { return h == kTaaHash || h == kMbHash; };
	const auto r = find_temporal_pass(frame, kBackBufferRes, history);
	CHECK(r.taa_hash == kTaaHash);
}

TEST_CASE("an excluded hash is never selected, even at the stop position")
{
	auto frame = realistic_frame();
	for (auto &e : frame)
		if (e.pipeline_hash == kTaaHash)
			e.pipeline_hash = kDenoiserLookalikeHash;

	const auto history = [](std::uint64_t h) { return h == kDenoiserLookalikeHash; };
	const auto r = find_temporal_pass(frame, kBackBufferRes, history);
	CHECK(r.taa_hash == 0);
	CHECK(reason_contains(r, "exclusion"));
}

TEST_CASE("a resource reused later in the frame does not divert the walk")
{
	// UE4's transient allocator reuses render targets within a frame. If the last-writer
	// lookup were frame-global rather than position-aware, the late scratch pass below would
	// steal the motion-blur output's writer slot and derail the chain.
	auto frame = realistic_frame();
	frame.push_back(event(FrameEvent::Kind::dispatch, 90, 0x5C1A7C4ull,
		{ { 0, kGBufferRes, TexFormat::r8g8b8a8_unorm, kRW, kRH } },
		{ { 0, kMbOutRes, TexFormat::r16g16b16a16_float, kOW, kOH } }));

	const auto r = find_temporal_pass(frame, kBackBufferRes, default_history);
	CHECK(r.taa_hash == kTaaHash);
	REQUIRE(r.chain.size() == 3);
	CHECK(r.chain[1].pipeline_hash == kMbHash); // not the seq-90 scratch pass
}

TEST_CASE("a chain that leaves the recorded frame degrades to a clear reason")
{
	// Recording can start mid-frame, so the anchor's input may have no recorded writer.
	auto frame = realistic_frame();
	frame.erase(frame.begin(), frame.begin() + 5); // drop everything up to and incl. MB

	const auto r = find_temporal_pass(frame, kBackBufferRes, default_history);
	CHECK(r.taa_hash == 0);
	CHECK(reason_contains(r, "chain broke"));
}

TEST_CASE("no anchor degrades to a clear reason")
{
	auto frame = realistic_frame();
	for (auto &e : frame)
		for (auto &t : e.srvs)
			t.is_3d = false; // no LUT anywhere

	// And nothing wrote the claimed back buffer either.
	const auto r = find_temporal_pass(frame, 0xFFFF, default_history);
	CHECK(r.taa_hash == 0);
	CHECK(reason_contains(r, "no anchor"));
}

TEST_CASE("an empty frame degrades to a clear reason")
{
	const auto r = find_temporal_pass({}, kBackBufferRes, default_history);
	CHECK(r.taa_hash == 0);
	CHECK(reason_contains(r, "empty frame"));
}

TEST_CASE("without the LUT anchor the walk starts from the back buffer")
{
	// The fallback: anchor on whatever wrote the presented image (the Slate composite) and
	// walk through it. Its input is LDR, so the first hop follows the largest input of any
	// format until the chain reaches HDR territory.
	auto frame = realistic_frame();
	for (auto &e : frame)
		for (auto &t : e.srvs)
			t.is_3d = false;

	const auto r = find_temporal_pass(frame, kBackBufferRes, default_history);
	CHECK(r.taa_hash == kTaaHash);
	REQUIRE(r.chain.size() == 4); // slate -> tonemap -> motion blur -> TAA
	CHECK(r.chain[0].sequence == 80);
}

TEST_CASE("the hop budget terminates a chain with no candidate on it")
{
	// A long chain of HDR passes, none temporal: the walk must stop after kMaxWalkHops with
	// a clear reason rather than wandering into the base pass.
	std::vector<FrameEvent> frame;
	const std::uint32_t chain_len = kMaxWalkHops + 3;
	for (std::uint32_t i = 0; i < chain_len; ++i)
	{
		frame.push_back(event(FrameEvent::Kind::dispatch, 10 + i, 0x1000 + i,
			{ { 0, 0x4000ull + i, TexFormat::r16g16b16a16_float, kOW, kOH } },
			{ { 0, 0x4001ull + i, TexFormat::r16g16b16a16_float, kOW, kOH } }));
	}
	frame.push_back(event(FrameEvent::Kind::draw, 100, 0,
		{
			{ 0, 0x4000ull + chain_len, TexFormat::r16g16b16a16_float, kOW, kOH },
			{ 1, kLutRes, TexFormat::unknown, 32, 32, 0, true },
		},
		{ { 0, kLdrRes, TexFormat::r10g10b10a2_unorm, kOW, kOH } }));

	const auto r = find_temporal_pass(frame, 0, default_history);
	CHECK(r.taa_hash == 0);
	CHECK(reason_contains(r, "hop budget"));
	CHECK(r.chain.size() == kMaxWalkHops + 1); // the anchor plus exactly the budget
}

TEST_CASE("a null history oracle selects nothing")
{
	// No proof, no selection: with no round-trip data the walk must degrade, not guess.
	const auto r = find_temporal_pass(realistic_frame(), kBackBufferRes, nullptr);
	CHECK(r.taa_hash == 0);
}
