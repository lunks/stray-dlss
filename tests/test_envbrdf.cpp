// The G-buffer resolve math, pinned in CI (DLSS-RR phase 2).
//
// The expected values below were derived OFFLINE, by an independent high-precision
// implementation of the Streamline §4.2.1 listing (Python, double precision, written
// directly from the fetched coefficient matrices — the derivation is reproduced in each
// case's comment). The C++ under test must reproduce them in float; a coefficient typo,
// a transposed matrix, a swapped swizzle (the listing's denominators use X.xyw vs X.xzw
// asymmetrically) or a missing clamp lands outside the tolerances. The shader
// (shaders/gbuffer_resolve.hlsl) carries the same formulas by comment lockstep — CI
// cannot execute it, so this file is the only executable proof of the math.

#include <doctest/doctest.h>

#include "core/envbrdf.hpp"
#include "core/ring.hpp"

#include <cmath>

using namespace stray_dlss::gbr;

namespace {

constexpr float kTol = 2e-5f; // float32 vs double reference, well above rounding noise

bool near(float a, float b, float tol = kTol)
{
	return std::fabs(a - b) <= tol;
}

} // namespace

TEST_CASE("EnvBRDFApprox2 reproduces the offline reference vectors")
{
	// f0=0.04 (dielectric), roughness 0.5 -> alpha 0.25, NoV 0.7:
	// reference out = 0.040368143, scale = 0.876603123, bias = 0.005304018.
	{
		const Float3 out = env_brdf_approx2(Float3{ 0.04f, 0.04f, 0.04f }, 0.25f, 0.7f);
		CHECK(near(out.x, 0.040368143f));
		CHECK(near(out.y, 0.040368143f));
		CHECK(near(out.z, 0.040368143f));
	}
	// f0=1, alpha=1, NoV=1: reference out = 0.327395279 (scale 0.327212053, bias
	// 0.000183226) — the fully-rough mirror loses most of its reflectance.
	{
		const Float3 out = env_brdf_approx2(Float3{ 1.0f, 1.0f, 1.0f }, 1.0f, 1.0f);
		CHECK(near(out.x, 0.327395279f));
	}
	// f0=(0.9,0.6,0.3), alpha=0.5, NoV=0.5: reference (0.626855902, 0.421015553,
	// 0.215175204) — pins the full matrix arithmetic on an asymmetric colour.
	{
		const Float3 out = env_brdf_approx2(Float3{ 0.9f, 0.6f, 0.3f }, 0.5f, 0.5f);
		CHECK(near(out.x, 0.626855902f));
		CHECK(near(out.y, 0.421015553f));
		CHECK(near(out.z, 0.215175204f));
	}
}

TEST_CASE("the listing's green-channel hack kills the bias at f0.g == 0")
{
	// f0=(0.5, 0, 0.2), alpha=0.09, NoV=0.3: bias is scaled by saturate(f0.g*50) = 0, so
	// out = f0 * scale exactly — reference scale 0.804659511.
	const Float3 out = env_brdf_approx2(Float3{ 0.5f, 0.0f, 0.2f }, 0.09f, 0.3f);
	CHECK(near(out.x, 0.402329756f));
	CHECK(near(out.y, 0.0f));
	CHECK(near(out.z, 0.160931902f));
}

TEST_CASE("the max(0, bias) clamp is load-bearing at alpha=0, NoV=1")
{
	// Reference: raw bias = -0.004652508 there. Without the listing's clamp the result
	// would be 0.04*1.041647843 - 0.004652508 = 0.037013; with it, 0.041665914.
	const Float3 out = env_brdf_approx2(Float3{ 0.04f, 0.04f, 0.04f }, 0.0f, 1.0f);
	CHECK(near(out.x, 0.041665914f));
	CHECK(out.x > 0.04f); // scale > 1 and bias clamped away, never subtracted
}

TEST_CASE("EnvBRDFApprox2 takes |NoV| like the listing")
{
	const Float3 a = env_brdf_approx2(Float3{ 0.3f, 0.3f, 0.3f }, 0.4f, 0.6f);
	const Float3 b = env_brdf_approx2(Float3{ 0.3f, 0.3f, 0.3f }, 0.4f, -0.6f);
	CHECK(near(a.x, b.x));
}

TEST_CASE("the UE plugin's NoV remap")
{
	// x/(x+(1-NoV)^2): 0.5 -> 0.5, 0.8 -> 0.941176471, 1 -> 1, 0 -> 0.
	CHECK(near(nov_remap(0.5f), 0.5f));
	CHECK(near(nov_remap(0.8f), 0.941176471f));
	CHECK(near(nov_remap(1.0f), 1.0f));
	CHECK(near(nov_remap(0.0f), 0.0f));
}

TEST_CASE("ComputeF0: dielectric 0.08*Specular, metals take base colour")
{
	// Metallic 0: F0 = 0.08 * Specular = 0.04 at the UE default Specular 0.5.
	const Float3 dielectric = compute_f0(0.5f, Float3{ 0.8f, 0.6f, 0.4f }, 0.0f);
	CHECK(near(dielectric.x, 0.04f));
	CHECK(near(dielectric.y, 0.04f));
	CHECK(near(dielectric.z, 0.04f));

	// Metallic 1: F0 = BaseColor.
	const Float3 metal = compute_f0(0.5f, Float3{ 0.8f, 0.6f, 0.4f }, 1.0f);
	CHECK(near(metal.x, 0.8f));
	CHECK(near(metal.y, 0.6f));
	CHECK(near(metal.z, 0.4f));

	// Halfway: lerp(0.04, base, 0.5).
	const Float3 half = compute_f0(0.5f, Float3{ 0.8f, 0.6f, 0.4f }, 0.5f);
	CHECK(near(half.x, 0.42f));
}

TEST_CASE("DiffuseColor = BaseColor - BaseColor * Metallic")
{
	const Float3 d0 = diffuse_colour(Float3{ 0.8f, 0.6f, 0.4f }, 0.0f);
	CHECK(near(d0.x, 0.8f));
	const Float3 d1 = diffuse_colour(Float3{ 0.8f, 0.6f, 0.4f }, 1.0f);
	CHECK(near(d1.x, 0.0f));
	const Float3 dh = diffuse_colour(Float3{ 0.8f, 0.6f, 0.4f }, 0.25f);
	CHECK(near(dh.y, 0.45f));
}

TEST_CASE("GBufferA normal decode: N*2-1, renormalised, never NaN")
{
	// +Z encodes as (0.5, 0.5, 1.0).
	const Float3 up = decode_gbuffer_a_normal(Float3{ 0.5f, 0.5f, 1.0f });
	CHECK(near(up.x, 0.0f));
	CHECK(near(up.y, 0.0f));
	CHECK(near(up.z, 1.0f));

	// A quantised off-axis value renormalises to unit length.
	const Float3 n = decode_gbuffer_a_normal(Float3{ 0.75f, 0.5f, 0.75f });
	const float len = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
	CHECK(near(len, 1.0f));
	CHECK(near(n.x, n.z)); // decoded (0.5, 0, 0.5) keeps its direction

	// The degenerate encode (exactly 0.5 everywhere -> zero vector) yields +Z, not NaN —
	// sky pixels under a cleared G-buffer hit this.
	const Float3 deg = decode_gbuffer_a_normal(Float3{ 0.5f, 0.5f, 0.5f });
	CHECK(near(deg.z, 1.0f));
	CHECK(deg.x == deg.x); // not NaN
}

TEST_CASE("the shading model id lives in GBufferB.a's low nibble")
{
	CHECK(decode_shading_model_id(0.0f) == kShadingModelUnlit);
	// DEFAULT_LIT = 1 encodes as 1/255.
	CHECK(decode_shading_model_id(1.0f / 255.0f) == 1);
	// A selective-output mask in the high nibble must not leak into the id: 0x31 -> 1.
	CHECK(decode_shading_model_id(0x31 / 255.0f) == 1);
	// SUBSURFACE = 2 with mask 0x50 -> 2.
	CHECK(decode_shading_model_id(0x52 / 255.0f) == 2);
	// Out-of-range input must not UB the cast.
	CHECK(decode_shading_model_id(-1.0f) == 0);
	CHECK(decode_shading_model_id(2.0f) == 0xF);
}

TEST_CASE("sRGB decode follows IEC 61966-2-1")
{
	// Offline reference: srgb(0.5) = 0.214041140, srgb(0.04) = 0.003095975 (linear
	// segment: 0.04/12.92), endpoints exact.
	CHECK(near(srgb_to_linear(0.5f), 0.214041140f));
	CHECK(near(srgb_to_linear(0.04f), 0.003095975f));
	CHECK(near(srgb_to_linear(0.0f), 0.0f));
	CHECK(near(srgb_to_linear(1.0f), 1.0f));
}

TEST_CASE("NoV geometry: head-on surface at screen centre gives 1")
{
	// Screen centre: the ray is (0,0,1); a surface facing the camera has view-space
	// normal (0,0,-1) (UE view space looks down +Z). V = -ray, dot = 1.
	CHECK(near(nov_from_screen(0.0f, 0.0f, 1.2f, 2.1f, Float3{ 0.0f, 0.0f, -1.0f }), 1.0f));

	// The same normal seen off-centre makes a shallower angle: NoV < 1, and wider
	// projection terms (smaller fov -> larger P00) pull it back toward 1.
	const float edge = nov_from_screen(1.0f, 0.0f, 1.0f, 1.0f, Float3{ 0.0f, 0.0f, -1.0f });
	CHECK(near(edge, 1.0f / std::sqrt(2.0f), 1e-4f));
	const float narrow = nov_from_screen(1.0f, 0.0f, 2.0f, 2.0f, Float3{ 0.0f, 0.0f, -1.0f });
	CHECK(narrow > edge);

	// A back-facing normal clamps to 0 rather than going negative.
	CHECK(near(nov_from_screen(0.0f, 0.0f, 1.0f, 1.0f, Float3{ 0.0f, 0.0f, 1.0f }), 0.0f));

	// Unnormalised normals are handled (the rotation rows may carry scale error): only
	// the direction matters.
	CHECK(near(nov_from_screen(0.0f, 0.0f, 1.0f, 1.0f, Float3{ 0.0f, 0.0f, -2.5f }), 1.0f));

	// A degenerate normal yields 0, never NaN.
	const float deg = nov_from_screen(0.0f, 0.0f, 1.0f, 1.0f, Float3{ 0.0f, 0.0f, 0.0f });
	CHECK(near(deg, 0.0f));
	CHECK(deg == deg);
}

TEST_CASE("the guide constants match their citations")
{
	// PDF §3.4.2 sky albedo, §2.5 floor, GBufferResolve.usf's 0.75 NoV scale. A drive-by
	// "improvement" to any of these must fail here and cite a new source.
	CHECK(kSkyAlbedo == 0.5f);
	CHECK(kAlbedoFloor == 0.05f);
	CHECK(kNovScale == 0.75f);
	CHECK(kShadingModelUnlit == 0);
}

TEST_CASE("the full specular-albedo pipeline composes: UE default dielectric")
{
	// BaseColor 0.5 grey, Metallic 0, Specular 0.5, Roughness 0.5, head-on at screen
	// centre: F0 = 0.04; NoV = 1 remaps to 1, scaled to 0.75;
	// EnvBRDFApprox2(0.04, 0.25, 0.75) — the composition the shader executes per pixel.
	const Float3 base{ 0.5f, 0.5f, 0.5f };
	const Float3 f0 = compute_f0(0.5f, base, 0.0f);
	const float nov = nov_from_screen(0.0f, 0.0f, 1.0f, 1.0f, Float3{ 0.0f, 0.0f, -1.0f });
	const float remapped = nov_remap(nov) * kNovScale;
	const Float3 spec = env_brdf_approx2(f0, 0.5f * 0.5f, remapped);
	const Float3 expected = env_brdf_approx2(Float3{ 0.04f, 0.04f, 0.04f }, 0.25f, 0.75f);
	CHECK(near(spec.x, expected.x));
	// And the diffuse half: non-metal keeps its base colour, above the floor.
	const Float3 diff = diffuse_colour(base, 0.0f);
	CHECK(near(diff.x, 0.5f));
	CHECK(diff.x >= kAlbedoFloor);
}

TEST_CASE("ring: the generic descriptor offset matches the specialised one")
{
	// gbuffer_resolve packs 7 descriptors per frame slice; the generic arithmetic must
	// agree with the specialised mv_resolve form at its own width, and slices must not
	// overlap at width 7.
	namespace ring = stray_dlss::ring;
	for (std::uint32_t slot = 0; slot < ring::kFrameCount; ++slot)
		for (std::uint32_t i = 0; i < ring::kDescriptorsPerFrame; ++i)
			CHECK(ring::descriptor_offset_n(slot, ring::kDescriptorsPerFrame, i, 32) ==
				ring::descriptor_offset_in_slot(slot, i, 32));

	constexpr std::uint32_t kWide = 7;
	const auto end_of_slot0 = ring::descriptor_offset_n(0, kWide, kWide - 1, 32) + 32;
	CHECK(ring::descriptor_offset_n(1, kWide, 0, 32) == end_of_slot0);
}
