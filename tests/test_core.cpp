// Unit tests for everything that can be proven without a GPU. These build and run on both
// the Windows and Linux CI runners.
//
// The point is not coverage for its own sake: the developer cannot run the game, so every
// convention that could silently produce a wrong image has to be pinned here instead.

#include <doctest/doctest.h>

#include "core/fnv1a.hpp"
#include "core/ue4_view.hpp"

#include <cstdint>
#include <cstring>
#include <string>

using namespace stray_dlss;

TEST_CASE("fnv1a64 matches the reference vectors")
{
	// Canonical FNV-1a 64-bit test vectors.
	CHECK(fnv1a64("", 0) == 0xcbf29ce484222325ull);
	CHECK(fnv1a64("a", 1) == 0xaf63dc4c8601ec8cull);
	CHECK(fnv1a64("foobar", 6) == 0x85944171f73967e8ull);
}

TEST_CASE("fnv1a64 is stable for the identities we hook on")
{
	// The measured hashes in CLAUDE.md §2.3 are only meaningful if this function never
	// changes. Hash a fixed blob and pin the answer so a refactor cannot silently move it.
	std::uint8_t blob[256];
	for (std::size_t i = 0; i < sizeof(blob); ++i)
		blob[i] = static_cast<std::uint8_t>(i);

	CHECK(fnv1a64(blob, sizeof(blob)) == 0x4242dc5249c33625ull);
}

TEST_CASE("velocity decode reproduces the constants measured in Stray's own DXBC")
{
	// CLAUDE.md §2.5. These bit patterns were located in the shipped shader bytecode, so a
	// drift here means we no longer agree with the game.
	std::uint32_t bits = 0;

	std::memcpy(&bits, &ue4::kVelocityInvDiv, sizeof(bits));
	CHECK(bits == 0x408041ABu); // 4.00801611f

	std::memcpy(&bits, &ue4::kVelocityDecodeBias, sizeof(bits));
	CHECK(bits == 0x4000412Bu); // 2.00397754f, appears negated as 0xC000412B in the MAD

	std::memcpy(&bits, &ue4::kVelocityEncodeBias, sizeof(bits));
	CHECK(bits == 0x3EFFFF00u); // 32767/65535, NOT 0.5
}

TEST_CASE("velocity encode and decode round-trip")
{
	const ue4::Float2 inputs[] = {
		{ 0.0f, 0.0f }, { 0.1f, -0.1f }, { -0.25f, 0.25f }, { 0.4f, 0.4f }, { -0.49f, 0.49f },
	};

	for (const auto &v : inputs)
	{
		const ue4::Float2 decoded = ue4::decode_velocity(ue4::encode_velocity(v));
		CHECK(decoded.x == doctest::Approx(v.x).epsilon(1e-4));
		CHECK(decoded.y == doctest::Approx(v.y).epsilon(1e-4));
	}
}

TEST_CASE("velocity validity test is a strict red-channel comparison against zero")
{
	// UE 4.27 clears the velocity target to 0 and the TAA shader tests `EncodedVelocity.x >
	// 0.0` — red channel only, strictly greater. A magnitude test would be wrong, and so
	// would testing both components. CLAUDE.md §2.5.
	CHECK_FALSE(ue4::has_object_velocity({ 0.0f, 0.0f }));
	CHECK_FALSE(ue4::has_object_velocity({ 0.0f, 0.9f })); // green set, red clear -> still invalid
	CHECK(ue4::has_object_velocity({ 0.0001f, 0.0f }));

	// Anything the encoder produces for a real velocity is ~0.5 and therefore valid.
	CHECK(ue4::has_object_velocity(ue4::encode_velocity({ 0.0f, 0.0f })));
	CHECK(ue4::has_object_velocity(ue4::encode_velocity({ -0.4f, 0.0f })));
}

TEST_CASE("View constant buffer offsets match the measured layout")
{
	// CLAUDE.md §2.6. Byte offsets, not rows, because that is what a reader of the mapped
	// upload buffer actually indexes with.
	CHECK(ue4::row_to_byte(ue4::ViewRow::kViewToClip) == 448);
	CHECK(ue4::row_to_byte(ue4::ViewRow::kViewToClipNoAA) == 512);
	CHECK(ue4::row_to_byte(ue4::ViewRow::kClipToPrevClip) == 1952);
	CHECK(ue4::row_to_byte(ue4::ViewRow::kTemporalAAJitter) == 2016);
	CHECK(ue4::row_to_byte(ue4::ViewRow::kViewRectMin) == 2064);
	CHECK(ue4::row_to_byte(ue4::ViewRow::kViewSizeAndInvSize) == 2080);
	CHECK(ue4::row_to_byte(ue4::ViewRow::kLightProbeSizeRatio) == 2096); // the (1,1,1,1) decoy
	CHECK(ue4::row_to_byte(ue4::ViewRow::kBufferSizeAndInvSize) == 2112);
	CHECK(ue4::row_to_byte(ue4::ViewRow::kTemporalAAParams) == 2432);

	// Scalars, addressed as row byte + component * 4.
	CHECK(ue4::row_to_byte(ue4::ViewRow::kPreExposureRow) + ue4::kPreExposureComponent * 4 == 2164);
	CHECK(ue4::row_to_byte(ue4::ViewRow::kNearPlaneRow) + ue4::kNearPlaneComponent * 4 == 2272);
	CHECK(ue4::row_to_byte(ue4::ViewRow::kDeltaTimeRow) + ue4::kDeltaTimeComponent * 4 == 2288);
	CHECK(ue4::row_to_byte(ue4::ViewRow::kCameraCutRow) + ue4::kCameraCutComponent * 4 == 2320);

	// One contiguous read covers everything verified. Rows past 152 are unverified.
	CHECK(ue4::kViewPrefixBytes == 2448);
	CHECK(ue4::kViewPrefixBytes > ue4::row_to_byte(ue4::ViewRow::kTemporalAAParams) + 15);
}

TEST_CASE("jitter conversion carries the negative Y factor")
{
	// CLAUDE.md §2.7. The negative Y is real in the DERIVATION from TemporalAAJitter. It is
	// NOT applied again when feeding NGX — TemporalAAParams.zw goes across untouched.
	constexpr float w = 2560.0f;
	constexpr float h = 1440.0f;

	const ue4::Float2 px = ue4::jitter_pixels_from_ndc({ 0.001f, 0.001f }, w, h);
	CHECK(px.x > 0.0f);
	CHECK(px.y < 0.0f); // the sign that ruins images if dropped

	// Round-trip both ways.
	const ue4::Float2 original_px{ 0.3f, -0.2f };
	const ue4::Float2 ndc = ue4::jitter_ndc_from_pixels(original_px, w, h);
	const ue4::Float2 back = ue4::jitter_pixels_from_ndc(ndc, w, h);
	CHECK(back.x == doctest::Approx(original_px.x));
	CHECK(back.y == doctest::Approx(original_px.y));
}

TEST_CASE("camera-cut jitter heuristic")
{
	// On a cut UE4 assigns PrevViewMatrices = ViewMatrices, so jitter .zw becomes .xy.
	CHECK(ue4::jitter_indicates_camera_cut({ 0.25f, -0.125f }, { 0.25f, -0.125f }));
	CHECK_FALSE(ue4::jitter_indicates_camera_cut({ 0.25f, -0.125f }, { 0.25f, -0.124f }));
	CHECK_FALSE(ue4::jitter_indicates_camera_cut({ 0.25f, -0.125f }, { 0.24f, -0.125f }));
}

TEST_CASE("jitter phase count follows NVIDIA's formula and meets the documented minimums")
{
	// Total Phases = 8 * (target / render)^2. docs/RESEARCH.md §3.3.
	CHECK(ue4::jitter_phase_count(2160, 2160) == 8); // DLAA, minimum 8

	// Quality is 2/3 scale -> 8 * 2.25 = 18, exactly the documented Quality minimum.
	CHECK(ue4::jitter_phase_count(1440, 2160) == 18);

	// Performance is 1/2 scale -> 8 * 4 = 32, the documented Performance minimum.
	CHECK(ue4::jitter_phase_count(1080, 2160) == 32);

	// Ultra Performance is 1/3 scale -> 8 * 9 = 72.
	CHECK(ue4::jitter_phase_count(720, 2160) == 72);

	// Never below base, even if render somehow exceeds output.
	CHECK(ue4::jitter_phase_count(4320, 2160) == 8);
	CHECK(ue4::jitter_phase_count(0, 2160) == 8);
}

TEST_CASE("mip LOD bias")
{
	// -1 at native resolution, since TAA/DLAA is still running.
	CHECK(ue4::mip_lod_bias(2160, 2160) == doctest::Approx(-1.0f));
	// Half resolution -> log2(0.5) - 1 = -2.
	CHECK(ue4::mip_lod_bias(1080, 2160) == doctest::Approx(-2.0f));
	// Render above output clamps the ratio to 1.
	CHECK(ue4::mip_lod_bias(4320, 2160) == doctest::Approx(-1.0f));
}

TEST_CASE("halton stays inside the pixel-space window DLSS expects")
{
	// Base 2 for x, base 3 for y, returning [-0.5, 0.5].
	for (unsigned int i = 0; i < 128; ++i)
	{
		const float x = ue4::halton(i, 2);
		const float y = ue4::halton(i, 3);
		CHECK(x >= -0.5f);
		CHECK(x < 0.5f);
		CHECK(y >= -0.5f);
		CHECK(y < 0.5f);
	}

	// Index 0 must not sit at the origin, which is why the implementation adds 1.
	CHECK(ue4::halton(0, 2) == doctest::Approx(0.0f));  // 1/2 - 0.5
	CHECK(ue4::halton(1, 2) == doctest::Approx(-0.25f)); // 1/4 - 0.5
	CHECK(ue4::halton(0, 3) == doctest::Approx(-1.0f / 6.0f)); // 1/3 - 0.5
}
