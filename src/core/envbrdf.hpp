// The pure math of the G-buffer resolve pass (DLSS-RR phase 2): UE 4.27 G-buffer decode,
// NVIDIA's sanctioned specular-albedo approximation, and the NoV geometry — as scalar
// reference functions, so CI pins every formula the shader executes
// (tests/test_envbrdf.cpp) and shaders/gbuffer_resolve.hlsl is kept in lockstep by
// comment cross-reference. The developer cannot run the game; this is the §6 discipline.
//
// Provenance, all primary-source:
//
//   * `env_brdf_approx2` — the EXACT listing NVIDIA sanctions for the Specular Albedo
//     guide: Streamline ProgrammingGuideDLSS_RR.md §4.2.1 (`EnvBRDFApprox2`, "[Ray
//     Tracing Gems, Chapter 32]"), fetched verbatim 2026-08-31; byte-identical
//     coefficients in the official UE plugin's `EnvBRDFApproxRTG`
//     (Plugins/DLSS/Shaders/Private/GBufferResolve.usf, v3.7.3 mirror, fetched verbatim
//     2026-08-31) and, per docs/RESEARCH-RR-GBUFFER.md §2.3, in dxvk-remix
//     (brdf.slangh:676-705) and RTXPT (Microfacet.hlsli:282). HARD. Note the final
//     clamps — `mad(F0, max(0,scale), max(0,bias))` — are part of the listing: at
//     alpha=0, NoV=1 the raw bias is NEGATIVE and the clamp is load-bearing (pinned in
//     the tests).
//   * `nov_remap` and the 0.75 NoV scale — the UE plugin's two empirical tweaks, copied
//     per RESEARCH-RR-GBUFFER.md §2.3/§2.4: `x = NoV*NoV; NoV = x/(x+(1-NoV)*(1-NoV));`
//     then `EnvBRDFApproxRTG(F0, Square(Roughness), NoV * 0.75f)` (GBufferResolve.usf,
//     fetched). HARD.
//   * G-buffer decode — UE 4.27.2 via the mirror (RESEARCH-RR-GBUFFER.md §2.4, §6;
//     HARD-via-mirror): GBufferA.rgb = WorldNormal*0.5+0.5 (DeferredShadingCommon.ush
//     :121-124, used :446 — the plain unit-vector encoding, NOT octahedral: octahedral
//     belongs to the high-precision G-buffer paths, and Stray's config selects stock
//     PF_A2B10G10R10, §1.2); GBufferB = (Metallic, Specular, Roughness,
//     ShadingModelID|SelectiveOutputMask) (:459-462); DiffuseColor =
//     BaseColor − BaseColor·Metallic (:608); F0 = ComputeF0(Specular, BaseColor,
//     Metallic) = lerp((0.08·Specular).xxx, BaseColor, Metallic)
//     (ShadingCommon.ush:77-97). The shading-model id lives in the LOW nibble of
//     GBufferB.a with the selective-output mask above it (encode/decode helpers in the
//     :419-480 region the research doc cites).
//   * Sky/unlit defaults and the albedo floor — RR Integration Guide: default sky albedo
//     (0.5, 0.5, 0.5) (PDF §3.4.2); near-zero albedo guides make RR hallucinate
//     brightness, RTXPT floors both albedos at 0.05 (RESEARCH-RR-GBUFFER.md §2.5). HARD.
//   * `srgb_to_linear` — IEC 61966-2-1 (the sRGB standard transfer function). Used only
//     when the GBufferC view cannot decode sRGB in hardware; the guide forbids
//     sRGB-encoded albedo inputs ("sRGB formats are not supported", §2.2).
//
// Pure logic: no Windows, no D3D, no ReShade.
#pragma once

#include <cstdint>

namespace stray_dlss::gbr {

struct Float3
{
	float x = 0.0f;
	float y = 0.0f;
	float z = 0.0f;
};

// --- constants the recipe hangs on (each cited above) ---

// PDF §3.4.2: sky/no-geometry pixels default both albedo guides to mid-grey.
constexpr float kSkyAlbedo = 0.5f;
// RESEARCH-RR-GBUFFER.md §2.5: floor both albedos so RR never sees a near-zero guide.
constexpr float kAlbedoFloor = 0.05f;
// GBufferResolve.usf: the empirical NoV scale at the EnvBRDF call site.
constexpr float kNovScale = 0.75f;
// ShadingCommon.ush: SHADINGMODELID_UNLIT — sky, cleared G-buffer, emissive-only.
constexpr std::uint32_t kShadingModelUnlit = 0;

// The Ray Tracing Gems ch. 32 preintegrated-GGX approximation, exactly as the Streamline
// guide lists it. `f0` is specular reflectance at normal incidence, `alpha` is PERCEPTUAL
// ROUGHNESS SQUARED, `nov` the (remapped, scaled) N·V.
Float3 env_brdf_approx2(const Float3 &f0, float alpha, float nov);

// The UE plugin's NoV remap: x = NoV²; NoV = x / (x + (1−NoV)²).
float nov_remap(float nov);

// ComputeF0: lerp((0.08·Specular).xxx, BaseColor, Metallic).
Float3 compute_f0(float specular, const Float3 &base_colour, float metallic);

// DiffuseColor = BaseColor − BaseColor·Metallic.
Float3 diffuse_colour(const Float3 &base_colour, float metallic);

// GBufferA.rgb → world normal: N*2−1, renormalised (the 10-bit quantisation denormalises
// slightly, and RR requires "Shading Normals (Normalized)", §2.2). A degenerate encoded
// value (all-zero vector after decode) falls back to +Z rather than NaN.
Float3 decode_gbuffer_a_normal(const Float3 &encoded);

// GBufferB.a → shading model id (low nibble; the selective-output mask sits above it).
std::uint32_t decode_shading_model_id(float gbuffer_b_alpha);

// IEC 61966-2-1 decode, one channel.
float srgb_to_linear(float c);

// N·V for a pixel, from verified View-CB data only: `screen` is the NDC position
// ((2u−1, 1−2v), Y-up), `proj00`/`proj11` are ViewToClipNoAA[0][0]/[1][1] (View CB row
// 32, measured anchor — jitter-free on purpose), and `n_view` is the world-space normal
// already rotated into view space. The view-space ray through the pixel is
// (screen.x/P00, screen.y/P11, 1) — its direction does not depend on depth, which is why
// no depth read is needed — and V is its negation. Assumes a centred projection
// (ViewToClipNoAA[2][0]==[2][1]==0, true for Stray's main view; an off-centre projection
// would need those terms). Returns saturate(dot(V, normalize(n_view))).
float nov_from_screen(float screen_x, float screen_y, float proj00, float proj11,
                      const Float3 &n_view);

} // namespace stray_dlss::gbr
