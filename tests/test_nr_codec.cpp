// The DLSS-NR HDR colour codec, pinned in CI.
//
// The shaders (shaders/nr_encode.hlsl, shaders/nr_decode.hlsl, shaders/nr_codec.hlsli) execute
// this math on the GPU and CI cannot run them, so src/core/nr_codec.cpp is their twin and this
// file is the only executable proof either is right. Everything here is a property of the
// design rather than a captured output, which is what makes the pins meaningful: a transposed
// branch, a dropped clamp, an x^2.2 "simplification" of the sRGB curve or a lerp written as
// std::lerp all land outside at least one of them.
//
// THE HEADLINE IS "the codec is an exact identity when the network changes nothing". That is
// not a nicety — it is the whole reason the transfer is written as a residual instead of the
// obvious `SrgbDecode(neural) / scale`. If it ever stops holding bit-for-bit, the pass has
// started quietly altering every pixel of every frame it touches, which on this project is the
// exact class of failure (a plausible wrong image, no error) that costs a week.

#include <doctest/doctest.h>

#include "core/nr_codec.hpp"

#include <cmath>
#include <initializer_list>
#include <limits>

using namespace stray_dlss::nrc;

namespace {

constexpr float kTol = 1e-5f;

bool near(float a, float b, float tol = kTol)
{
	return std::fabs(a - b) <= tol;
}

Float3 rgb(float r, float g, float b) { return Float3{ r, g, b }; }

// Bit-for-bit equality, deliberately not `near`. The identity property is exact or it is not a
// property.
bool same(Float3 a, Float3 b)
{
	return a.x == b.x && a.y == b.y && a.z == b.z;
}

} // namespace

TEST_CASE("sRGB is the exact piecewise curve, not an x^2.2 approximation")
{
	// The two published anchors. 0.5^(1/2.4) = 0.7491535, *1.055 - 0.055 = 0.7353569 — an
	// x^(1/2.2) approximation would give 0.7297, which is outside the tolerance by 60x.
	CHECK(near(srgb_encode_channel(0.5f), 0.7353569f));
	CHECK(near(srgb_decode_channel(0.7353569f), 0.5f));

	// Endpoints are exact.
	CHECK(srgb_encode_channel(0.0f) == 0.0f);
	CHECK(near(srgb_encode_channel(1.0f), 1.0f));
	CHECK(srgb_decode_channel(0.0f) == 0.0f);
	CHECK(near(srgb_decode_channel(1.0f), 1.0f));

	// The toe: linear with slope 12.92 below 0.0031308, and the two branch points agree
	// (12.92 * 0.0031308 = 0.04045, which is the decode's own threshold).
	CHECK(near(srgb_encode_channel(0.001f), 0.01292f));
	CHECK(near(srgb_encode_channel(0.0031308f), 0.04045f));
	CHECK(near(srgb_decode_channel(0.04045f), 0.0031308f));
	CHECK(near(srgb_decode_channel(0.02f), 0.02f / 12.92f));
}

TEST_CASE("sRGB round-trips across the whole domain")
{
	for (int i = 0; i <= 100; ++i)
	{
		const float v = static_cast<float>(i) / 100.0f;
		CHECK(near(srgb_decode_channel(srgb_encode_channel(v)), v, 2e-5f));
	}
	// Both directions saturate their input, so out-of-domain values are clamped, never
	// extrapolated into a negative or a NaN.
	CHECK(srgb_encode_channel(-3.0f) == 0.0f);
	CHECK(near(srgb_encode_channel(7.0f), 1.0f));
	CHECK(srgb_decode_channel(-3.0f) == 0.0f);
	CHECK(near(srgb_decode_channel(7.0f), 1.0f));
}

TEST_CASE("the soft clip is the identity at and below the knee")
{
	CHECK(soft_clip_channel(0.0f) == 0.0f);
	CHECK(soft_clip_channel(0.25f) == 0.25f);
	CHECK(soft_clip_channel(0.5f) == 0.5f);
	CHECK(soft_clip_channel(kSoftClipKnee) == kSoftClipKnee);
}

TEST_CASE("the soft clip is continuous at the knee and asymptotic to exactly 1.0")
{
	// C0: the two branches meet.
	CHECK(near(soft_clip_channel(kSoftClipKnee + 1e-6f), kSoftClipKnee, 1e-5f));

	// Asymptote: no radiance, however large, leaves [0,1] — which is what makes the sRGB
	// encode's domain safe without a clamp of its own. (In exact arithmetic the bound is the
	// half-open [0,1); float32 rounds onto 1.0 from ~3.474 up, pinned in its own case below.)
	CHECK(soft_clip_channel(2.0f) < 1.0f);
	CHECK(soft_clip_channel(100.0f) <= 1.0f);
	CHECK(soft_clip_channel(1.0e6f) <= 1.0f);
	CHECK(soft_clip_channel(1.0e6f) > 0.99999f);

	// Non-decreasing everywhere, and STRICTLY increasing up to the point where float32 rounds
	// the shoulder onto 1.0. It never folds two distinct radiances onto different proxy values in
	// the wrong order, which is what would make the decode's subtraction meaningless.
	float previous = -1.0f;
	for (int i = 0; i <= 400; ++i)
	{
		const float v = static_cast<float>(i) * 0.05f;
		const float c = soft_clip_channel(v);
		CHECK(c >= previous);
		if (v < 3.0f)
			CHECK(c > previous);
		previous = c;
	}
}

TEST_CASE("the soft clip saturates to exactly 1.0 at about 3.474 in float32")
{
	// MEASURED, and it is the practical ceiling on how bright a pixel the network can still tell
	// apart: above ~3.5x display white every channel encodes to pure white, so a paper white that
	// leaves most of the frame up there hands the network a flat clipped image. Pinned because it
	// is the thing to reason about when the diagnostic line says the proxy is blown out.
	CHECK(soft_clip_channel(3.0f) < 1.0f);
	CHECK(soft_clip_channel(3.0f) > 0.9999f);
	CHECK(soft_clip_channel(3.5f) == 1.0f);
	CHECK(soft_clip_channel(1.0e6f) == 1.0f);
	// Never ABOVE 1.0, which is the invariant the sRGB encode's domain actually depends on.
	CHECK(soft_clip_channel(1.0e6f) <= 1.0f);
	CHECK(soft_clip_channel(3.4739f) < 1.0f);
	CHECK(soft_clip_channel(3.4741f) == 1.0f);
}

TEST_CASE("the soft clip is C0 but NOT C1, deliberately")
{
	// 0.25 * 5.770780 = 1.442695 = 1/ln(2): the slope jumps from 1.0 to ~1.4427 at the knee.
	// This is reproduced from the known working deployment rather than "corrected", so pin it —
	// a future smoothing would be a silent change to what the network sees.
	CHECK(near((1.0f - kSoftClipKnee) * kSoftClipShoulder, 1.442695f, 1e-5f));

	const float h = 1e-4f;
	const float slope_below =
		(soft_clip_channel(kSoftClipKnee - h) - soft_clip_channel(kSoftClipKnee - 2.0f * h)) / h;
	const float slope_above =
		(soft_clip_channel(kSoftClipKnee + 2.0f * h) - soft_clip_channel(kSoftClipKnee + h)) / h;
	CHECK(near(slope_below, 1.0f, 1e-3f));
	CHECK(near(slope_above, 1.442695f, 1e-2f));
}

TEST_CASE("proxy_scale is 1/paperWhite, clamped exactly like the reference")
{
	// `calcProxyScale`: staticExposure(1) / max(paperWhiteScale, 0.01).
	CHECK(proxy_scale(1.0f, 1.0f) == 1.0f);
	CHECK(near(proxy_scale(2.0f, 1.0f), 0.5f));
	CHECK(near(proxy_scale(1.605f, 1.0f), 1.0f / 1.605f)); // RenoDX's own shipped value
	CHECK(near(proxy_scale(16.0f, 1.0f), 0.0625f));

	// Values BELOW 1.0 are legal and raise the scale — the direction a pre-exposed, already
	// small input needs to reach the 0.75 knee.
	CHECK(near(proxy_scale(0.5f, 1.0f), 2.0f));
	CHECK(near(proxy_scale(0.1f, 1.0f), 10.0f));

	// The paper-white floor, so the reciprocal cannot run away.
	CHECK(near(proxy_scale(0.0f, 1.0f), 1.0f / kMinPaperWhite));
	CHECK(near(proxy_scale(-5.0f, 1.0f), 1.0f / kMinPaperWhite));
	CHECK(near(proxy_scale(0.000001f, 1.0f), 1.0f / kMinPaperWhite));

	// The reference RTX_OPTION's own ceiling. Without it a paper white of 1e30 would reach the
	// scale's [1e-6, 1e6] backstop, and the decode divides by the scale — so every delta would
	// come back amplified a millionfold. The backstop stays, as a backstop.
	CHECK(near(proxy_scale(1.0e30f, 1.0f), 1.0f / kMaxPaperWhite));
	CHECK(near(proxy_scale(1000.0f, 1.0f), 1.0f / kMaxPaperWhite));
	CHECK(proxy_scale(1.0e30f, 1.0f) > kScaleMin);

	// A non-finite knob falls back to the caller's default rather than poisoning the frame.
	// Falling back beats clamping here: an infinite paper white clamped to the floor would give
	// scale 1e-6, which the decode turns into a millionfold amplification of every delta —
	// strictly worse than simply ignoring a nonsensical setting.
	CHECK(near(proxy_scale(std::numeric_limits<float>::quiet_NaN(), 4.0f), 0.25f));
	CHECK(near(proxy_scale(std::numeric_limits<float>::infinity(), 1.0f), 1.0f));
	CHECK(near(proxy_scale(-std::numeric_limits<float>::infinity(), 2.0f), 0.5f));
	// A non-finite FALLBACK cannot poison it either.
	CHECK(near(proxy_scale(std::numeric_limits<float>::quiet_NaN(),
		std::numeric_limits<float>::quiet_NaN()), 1.0f));
}

TEST_CASE("the encode always lands in [0,1] and flushes broken pixels to black")
{
	const float scales[] = { kScaleMin, 0.01f, 1.0f, 17.5f, 1000.0f, kScaleMax };
	const float radiances[] = { 0.0f, 1e-6f, 0.01f, 0.5f, 1.0f, 12.0f, 5000.0f, 60000.0f };
	for (float s : scales)
	{
		for (float r : radiances)
		{
			const Float3 p = encode(rgb(r, r * 0.5f, r * 2.0f), s);
			CHECK(p.x >= 0.0f);
			CHECK(p.x <= 1.0f);
			CHECK(p.y >= 0.0f);
			CHECK(p.y <= 1.0f);
			CHECK(p.z >= 0.0f);
			CHECK(p.z <= 1.0f);
		}
	}

	// A negative channel is clamped rather than fed to pow().
	CHECK(encode(rgb(-1.0f, -0.0f, -1e9f), 1.0f).x == 0.0f);

	// NaN/Inf must never reach the network. The decode leaves the matching original alone, so
	// the two halves agree about what a broken pixel means.
	const float nan = std::numeric_limits<float>::quiet_NaN();
	const float inf = std::numeric_limits<float>::infinity();
	CHECK(same(encode(rgb(nan, 0.5f, 0.5f), 1.0f), rgb(0.0f, 0.0f, 0.0f)));
	CHECK(same(encode(rgb(0.5f, inf, 0.5f), 1.0f), rgb(0.0f, 0.0f, 0.0f)));
	CHECK(same(encode(rgb(0.5f, 0.5f, -inf), 1.0f), rgb(0.0f, 0.0f, 0.0f)));
}

TEST_CASE("EXACT IDENTITY: neural == proxy returns the original bit for bit")
{
	// The property the whole design exists for. Note the decode reads BOTH the proxy and the
	// neural answer out of textures, so on the GPU each has been quantised to FP16 — modelling
	// that here would change nothing, because what matters is that the two are the SAME value,
	// and their difference is then exactly +0.0 whatever they are. Feeding the encode's own
	// output as the "neural" texel reproduces exactly that.
	const float paper_whites[] = { 0.01f, 0.1f, 0.5f, 1.0f, 1.605f, 4.0f, 16.0f, 64.0f };
	const Float3 radiances[] = {
		rgb(0.0f, 0.0f, 0.0f),          // black — the chroma valve's faded region
		rgb(1e-7f, 0.0f, 3e-7f),        // near-black, hue is noise
		rgb(0.01f, 0.02f, 0.03f),       // deep shadow
		rgb(0.2f, 0.4f, 0.6f),          // well below the knee
		rgb(0.74f, 0.75f, 0.76f),       // straddling the knee itself
		rgb(1.0f, 1.0f, 1.0f),
		rgb(12.0f, 3.0f, 0.5f),         // far above the knee, strongly chromatic
		rgb(900.0f, 900.0f, 900.0f),    // deep HDR headroom
		rgb(60000.0f, 1.0f, 0.0f),      // the top of what FP16 can hold, fully saturated
	};
	const float strengths[] = { 0.0f, 0.35f, 1.0f };

	for (float pw : paper_whites)
	{
		const float s = proxy_scale(pw, 1.0f);
		for (const Float3 &o : radiances)
		{
			const Float3 proxy = encode(o, s);
			for (float cs : strengths)
			{
				for (float ts : strengths)
				{
					Float3 out = rgb(-1.0f, -1.0f, -1.0f);
					REQUIRE(decode(o, proxy, proxy, s, cs, ts, out));
					CHECK(same(out, o));
				}
			}
		}
	}
}

TEST_CASE("transferStrength 0 is an exact bypass even when the network disagrees")
{
	// The honest A/B against "NR off": the network's answer is computed and then discarded
	// exactly, so any difference on screen at ts=0 is somebody else's bug.
	const Float3 o = rgb(3.0f, 1.25f, 0.4f);
	const float s = proxy_scale(1.0f, 1.0f);
	const Float3 proxy = encode(o, s);
	const Float3 neural = rgb(0.1f, 0.9f, 0.35f); // nothing like the proxy

	Float3 out = rgb(-1.0f, -1.0f, -1.0f);
	REQUIRE(decode(o, proxy, neural, s, 1.0f, 0.0f, out));
	CHECK(same(out, o));
}

TEST_CASE("the network's answer is carried through, and it is not a no-op")
{
	// Sanity in the other direction: a codec that always returned the original would pass every
	// identity test above and be useless.
	const Float3 o = rgb(0.3f, 0.3f, 0.3f);
	const float s = 1.0f;
	const Float3 proxy = encode(o, s);
	const Float3 neural = rgb(proxy.x + 0.1f, proxy.y + 0.1f, proxy.z + 0.1f);

	Float3 out = rgb(0.0f, 0.0f, 0.0f);
	REQUIRE(decode(o, proxy, neural, s, 1.0f, 1.0f, out));
	CHECK(out.x > o.x);
	CHECK(out.y > o.y);
	CHECK(out.z > o.z);
}

TEST_CASE("below the knee the result is the network's answer decoded straight back")
{
	// Property 4 of the derivation: there p == d == o*s, so o + (n - p)/s reduces to n/s.
	const float s = 2.0f;
	const Float3 o = rgb(0.2f, 0.15f, 0.1f); // o*s = 0.4/0.3/0.2, all under the 0.75 knee
	const Float3 proxy = encode(o, s);
	const Float3 neural = rgb(0.6f, 0.5f, 0.4f);

	Float3 out = rgb(0.0f, 0.0f, 0.0f);
	REQUIRE(decode(o, proxy, neural, s, 1.0f, 1.0f, out));

	const Float3 expected = srgb_decode(neural);
	CHECK(near(out.x, expected.x / s, 1e-4f));
	CHECK(near(out.y, expected.y / s, 1e-4f));
	CHECK(near(out.z, expected.z / s, 1e-4f));
}

TEST_CASE("in clipped highlights the change is additive, never multiplicative")
{
	// Property 5: o = 100 with the network answering 0.7 against a proxy pinned near 1.0 must
	// give ~99.7, not 70. A multiplicative carry-back would have amplified a display-referred
	// change by 100x and blown the highlight out — and HDR headroom must survive (property 2).
	const float s = 1.0f;
	const Float3 o = rgb(100.0f, 100.0f, 100.0f);
	const Float3 proxy = encode(o, s);
	const Float3 neural = rgb(0.7f, 0.7f, 0.7f);

	Float3 out = rgb(0.0f, 0.0f, 0.0f);
	REQUIRE(decode(o, proxy, neural, s, 1.0f, 1.0f, out));

	// SrgbDecode(0.7) ~ 0.4479; the proxy decodes to just under 1.0, so the delta is about
	// -0.55 and the result stays deep in HDR.
	CHECK(out.x > 99.0f);
	CHECK(out.x < 100.0f);
}

TEST_CASE("colorStrength 0 keeps the original's chromaticity")
{
	// The escape hatch for "everything's gone red". At cs=0 the result must sit on the
	// original's chromaticity line, carrying only the network's luminance change.
	const float s = 1.0f;
	const Float3 o = rgb(0.4f, 0.2f, 0.1f);
	const Float3 proxy = encode(o, s);
	const Float3 neural = rgb(0.1f, 0.6f, 0.9f); // a violently different colour

	Float3 out = rgb(0.0f, 0.0f, 0.0f);
	REQUIRE(decode(o, proxy, neural, s, 0.0f, 1.0f, out));

	// Same hue: the channel ratios are preserved.
	CHECK(near(out.y / out.x, o.y / o.x, 1e-4f));
	CHECK(near(out.z / out.x, o.z / o.x, 1e-4f));
	// ...and cs=1 does NOT preserve them, or the knob would be doing nothing.
	Float3 coloured = rgb(0.0f, 0.0f, 0.0f);
	REQUIRE(decode(o, proxy, neural, s, 1.0f, 1.0f, coloured));
	CHECK(!near(coloured.z / coloured.x, o.z / o.x, 1e-3f));
}

TEST_CASE("the chroma valve fades out rather than stepping, near black")
{
	// A hard `originalLuminance > 0` branch made a pixel at exactly zero take the network's
	// colour while one a denormal above took a fully saturated primary. Below the floor the
	// weight must reach 0 continuously.
	const float s = 1.0f;
	const Float3 neural = rgb(0.05f, 0.05f, 0.05f);

	for (int i = 1; i <= 8; ++i)
	{
		// Sweep the original's luminance up through kMinChromaLuminance / s.
		const float lum = kMinChromaLuminance * (static_cast<float>(i) / 8.0f);
		const Float3 o = rgb(lum, lum, lum);
		const Float3 proxy = encode(o, s);

		Float3 faded = rgb(0.0f, 0.0f, 0.0f);
		Float3 full = rgb(0.0f, 0.0f, 0.0f);
		REQUIRE(decode(o, proxy, neural, s, 0.0f, 1.0f, faded)); // chroma-preserving path
		REQUIRE(decode(o, proxy, neural, s, 1.0f, 1.0f, full));  // network's colour
		// Neutral grey in, so both paths agree here; what is pinned is that neither produces a
		// NaN or a discontinuity as the weight crosses in.
		CHECK(std::isfinite(faded.x));
		CHECK(std::isfinite(full.x));
		CHECK(std::fabs(faded.x - full.x) < 1e-3f);
	}

	// Exactly zero luminance must not produce 0/0.
	Float3 out = rgb(-1.0f, -1.0f, -1.0f);
	const Float3 black = rgb(0.0f, 0.0f, 0.0f);
	REQUIRE(decode(black, encode(black, s), neural, s, 0.0f, 1.0f, out));
	CHECK(std::isfinite(out.x));
	CHECK(std::isfinite(out.y));
	CHECK(std::isfinite(out.z));
}

TEST_CASE("a broken pixel is left exactly as it was")
{
	const float nan = std::numeric_limits<float>::quiet_NaN();
	const float inf = std::numeric_limits<float>::infinity();
	const Float3 sentinel = rgb(-7.0f, -7.0f, -7.0f);
	const Float3 neural = rgb(0.5f, 0.5f, 0.5f);
	const Float3 proxy = rgb(0.25f, 0.25f, 0.25f);

	Float3 out = sentinel;
	CHECK(!decode(rgb(nan, 1.0f, 1.0f), proxy, neural, 1.0f, 1.0f, 1.0f, out));
	CHECK(same(out, sentinel)); // untouched, so the caller's pixel survives
	CHECK(!decode(rgb(1.0f, inf, 1.0f), proxy, neural, 1.0f, 1.0f, 1.0f, out));
	CHECK(same(out, sentinel));

	// A non-finite neural texel cannot poison the frame either: sRGB decode saturates it away,
	// so the delta stays finite and the pixel is still written.
	CHECK(decode(rgb(1.0f, 1.0f, 1.0f), proxy, rgb(nan, 0.5f, 0.5f), 1.0f, 1.0f, 1.0f, out));
	CHECK(std::isfinite(out.x));
}

TEST_CASE("the result never exceeds what R16G16B16A16_FLOAT can hold")
{
	// Writing past kMaxHalf turns the pixel into an infinity, which then survives every
	// downstream pass.
	const float s = kScaleMin; // 1e-6: the largest possible amplification of the delta
	const Float3 o = rgb(60000.0f, 60000.0f, 60000.0f);
	const Float3 proxy = rgb(0.0f, 0.0f, 0.0f);
	const Float3 neural = rgb(1.0f, 1.0f, 1.0f);

	Float3 out = rgb(0.0f, 0.0f, 0.0f);
	REQUIRE(decode(o, proxy, neural, s, 1.0f, 1.0f, out));
	CHECK(out.x == kMaxHalf);
	CHECK(out.y == kMaxHalf);
	CHECK(out.z == kMaxHalf);

	// And never below zero, whichever way the network pushed.
	const Float3 dark = rgb(0.001f, 0.001f, 0.001f);
	REQUIRE(decode(dark, rgb(1.0f, 1.0f, 1.0f), rgb(0.0f, 0.0f, 0.0f), s, 1.0f, 1.0f, out));
	CHECK(out.x >= 0.0f);
	CHECK(out.y >= 0.0f);
	CHECK(out.z >= 0.0f);
}

TEST_CASE("Rec.709 luminance uses the standard weights")
{
	CHECK(near(bt709_luminance(rgb(1.0f, 0.0f, 0.0f)), 0.2126f));
	CHECK(near(bt709_luminance(rgb(0.0f, 1.0f, 0.0f)), 0.7152f));
	CHECK(near(bt709_luminance(rgb(0.0f, 0.0f, 1.0f)), 0.0722f));
	CHECK(near(bt709_luminance(rgb(1.0f, 1.0f, 1.0f)), 1.0f));
	CHECK(bt709_luminance(rgb(0.0f, 0.0f, 0.0f)) == 0.0f);
}

TEST_CASE("tracked exposure folds the engine's OneOverPreExposure into the proxy scale")
{
	// THE MEASUREMENT THIS ENCODES. Stray's scene colour at the TAA hook carries UE4's
	// pre-exposure, measured live at 0.056 (CLAUDE.md §2.6, View row 135.y), so
	// OneOverPreExposure is about 17.86. With paper white left at its neutral 1.0 the static
	// scale is 1.0 and tracking alone lifts the signal by that factor — the same order of
	// magnitude as the ~10x the user reached by hand-dialling NgxNRPaperWhiteScale to 0.1.
	CHECK(near(proxy_scale_tracked(1.0f, 1.0f, 1.0f / 0.056f), 1.0f / 0.056f, 1e-3f));

	// It is a plain product with the static scale, so the knob still works on top of it.
	CHECK(near(proxy_scale_tracked(2.0f, 1.0f, 4.0f), 2.0f));
	CHECK(near(proxy_scale_tracked(0.1f, 1.0f, 2.0f), 20.0f, 1e-3f));

	// An exposure factor of exactly 1.0 must be the untracked answer, bit for bit — otherwise
	// turning tracking on would silently change a tuned configuration.
	for (const float pw : { 0.05f, 0.5f, 1.0f, 1.605f, 16.0f })
		CHECK(proxy_scale_tracked(pw, 1.0f, 1.0f) == proxy_scale(pw, 1.0f));
}

TEST_CASE("a frame with no readable View CB falls back to the static scale, never a wild one")
{
	// A non-finite or non-positive exposure means the View constant buffer was not readable this
	// frame. Multiplying by it would put the network at an arbitrary operating point with no
	// diagnostic at all — the silent-wrong-image class this project exists to avoid.
	const float expected = proxy_scale(0.5f, 1.0f);
	CHECK(proxy_scale_tracked(0.5f, 1.0f, 0.0f) == expected);
	CHECK(proxy_scale_tracked(0.5f, 1.0f, -3.0f) == expected);
	CHECK(proxy_scale_tracked(0.5f, 1.0f, std::numeric_limits<float>::quiet_NaN()) == expected);
	CHECK(proxy_scale_tracked(0.5f, 1.0f, std::numeric_limits<float>::infinity()) == expected);
}

TEST_CASE("the tracked scale is clamped to the same bounds as the static one")
{
	// The product can leave the range the static scale is bounded to, and the division in the
	// decode is by exactly this value — so the clamp has to survive the multiply.
	CHECK(proxy_scale_tracked(0.01f, 1.0f, 1e12f) == kScaleMax);
	CHECK(proxy_scale_tracked(64.0f, 1.0f, 1e-12f) == kScaleMin);
	// A product that overflows to infinity is not a clamp case, it is a "we have no answer"
	// case, and it falls back rather than pinning to the ceiling.
	CHECK(proxy_scale_tracked(0.01f, 1.0f, std::numeric_limits<float>::max()) ==
		proxy_scale(0.01f, 1.0f));
}

// The exposure factor is smoothed because DLSSNR keeps its own temporal history: a scale that
// moves frame to frame leaves that history in units that no longer match the current proxy.
// The user observed the flicker tracking paper white, with intensity amplifying it.
TEST_CASE("exposure smoothing is geometric, not arithmetic")
{
	// Exposure is multiplicative, so the halfway point between 1/16 and 16 is 1, not 8.
	const float mid = stray_dlss::nrc::smooth_exposure_factor(0.0625f, 16.0f, 0.5f);
	CHECK(mid == doctest::Approx(1.0f).epsilon(1e-4));
}

TEST_CASE("smoothing adopts the first sample rather than ramping from nothing")
{
	CHECK(stray_dlss::nrc::smooth_exposure_factor(0.0f, 3.0f, 0.1f) == doctest::Approx(3.0f));
	CHECK(stray_dlss::nrc::smooth_exposure_factor(-1.0f, 3.0f, 0.1f) == doctest::Approx(3.0f));
}

TEST_CASE("a bad sample never poisons the running value")
{
	const float nan = std::numeric_limits<float>::quiet_NaN();
	CHECK(stray_dlss::nrc::smooth_exposure_factor(2.0f, nan, 0.1f) == doctest::Approx(2.0f));
	CHECK(stray_dlss::nrc::smooth_exposure_factor(2.0f, 0.0f, 0.1f) == doctest::Approx(2.0f));
}

TEST_CASE("rate 1.0 disables smoothing and rate 0 freezes it")
{
	CHECK(stray_dlss::nrc::smooth_exposure_factor(2.0f, 5.0f, 1.0f) == doctest::Approx(5.0f));
	CHECK(stray_dlss::nrc::smooth_exposure_factor(2.0f, 5.0f, 0.0f) == doctest::Approx(2.0f));
}

// Feature 18 keeps its own temporal accumulation in display-referred units, and the codec scale
// defines those units. A scale change invalidates that history silently. Smoothing makes the
// mismatch gradual rather than absent — which is why a static camera recovers fast while
// movement keeps re-triggering it — so the scale is latched and a change forces one Reset.
TEST_CASE("the scale latch compares ratios, not differences")
{
	using stray_dlss::nrc::codec_scale_invalidates_history;
	// Same RELATIVE change at very different magnitudes must behave identically.
	CHECK(codec_scale_invalidates_history(1.0f, 1.2f, 0.1f));
	CHECK(codec_scale_invalidates_history(10.0f, 12.0f, 0.1f));
	// A large absolute difference that is a small relative one must NOT trip.
	CHECK_FALSE(codec_scale_invalidates_history(100.0f, 105.0f, 0.1f));
	// Symmetric: a drop of the same ratio trips too.
	CHECK(codec_scale_invalidates_history(1.2f, 1.0f, 0.1f));
}

TEST_CASE("the scale latch is inert without history or with bad input")
{
	using stray_dlss::nrc::codec_scale_invalidates_history;
	CHECK_FALSE(codec_scale_invalidates_history(0.0f, 5.0f, 0.1f));  // no history yet
	CHECK_FALSE(codec_scale_invalidates_history(5.0f, 0.0f, 0.1f));  // bad reading
	CHECK_FALSE(codec_scale_invalidates_history(5.0f, 50.0f, 0.0f)); // tolerance 0 = disabled
}
