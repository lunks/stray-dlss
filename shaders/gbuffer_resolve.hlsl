// The G-buffer resolve pass for DLSS Ray Reconstruction: reads UE 4.27's GBufferA/B/C and
// emits the four RR guide textures, using NVIDIA's own UE-plugin recipe.
//
// EVERY formula here has a scalar reference twin in src/core/envbrdf.cpp, pinned by
// tests/test_envbrdf.cpp — change one and you MUST change the other (CLAUDE.md §6: the
// developer cannot run the game, so CI is the only proof). Provenance for each constant
// and decode lives in src/core/envbrdf.hpp's header comment; the short form:
//
//   * EnvBRDFApprox2 — Streamline ProgrammingGuideDLSS_RR.md §4.2.1 verbatim, identical
//     to the UE plugin's EnvBRDFApproxRTG (GBufferResolve.usf), dxvk-remix and RTXPT.
//   * NoV remap + 0.75 scale — GBufferResolve.usf verbatim.
//   * G-buffer decode — UE 4.27.2 DeferredShadingCommon.ush / ShadingCommon.ush via the
//     mirror (docs/RESEARCH-RR-GBUFFER.md §2.4): GBufferA.rgb = N*0.5+0.5 (plain unit
//     vector, NOT octahedral — Stray's config selects stock PF_A2B10G10R10);
//     GBufferB = (Metallic, Specular, Roughness, ShadingModelID|Mask);
//     GBufferC.rgb = BaseColor, sRGB-stored.
//   * Sky/unlit → albedo 0.5 (RR guide PDF §3.4.2); albedo floor 0.05 (§2.5).
//
// Output contract (all four at RENDER resolution, top-left aligned, subrect base 0 —
// the input-rect convention the UE plugin ships, RESEARCH-RR-GBUFFER.md §2.4):
//   u0  RGBA16F  xyz = world-space signed unit normal, w = linear roughness
//   u1  R16F     linear roughness (standalone — keeps both RR roughness modes testable)
//   u2  RGBA8    diffuse albedo, LINEAR (never sRGB-encoded: guide §2.2)
//   u3  RGBA8    specular albedo, LINEAR
// RGBA8_UNORM for the albedos, deliberately: reflectance is [0,1] by construction, the
// format is on D3D12's GUARANTEED typed-UAV-store list (R11G11B10F, the UE plugin's RTV
// choice, is optional-support as a UAV store and would silently no-op where absent), and
// RR reads guides with formatted reads (§2.2). Remix ships 10-bit UNORM albedos.

cbuffer Params : register(b0)
{
	// TranslatedWorldToView rotation rows (upper 3x3; translation irrelevant for
	// directions). View CB rows 8-10 — the LAYOUT is [derived] (CLAUDE.md §2.6: the row
	// table was established from VIEW_UNIFORM_BUFFER_MEMBER_TABLE and reproduced all
	// measured anchors), but these rows are NOT among the measured anchors: Phase 3 must
	// log and sanity-check them (a rotation: rows orthonormal) before trusting NoV.
	float4 WorldToViewRow0;
	float4 WorldToViewRow1;
	float4 WorldToViewRow2;
	float2 RenderSize;          // View.ViewSizeAndInvSize.xy (row 130, measured)
	float2 ViewRectMin;         // View.ViewRectMin.xy (row 129, measured)
	float  Proj00;              // ViewToClipNoAA[0][0] (row 32, measured) — jitter-free
	float  Proj11;              // ViewToClipNoAA[1][1]
	uint   CViewDecodesSrgb;    // 1: the GBufferC SRV is *_SRGB and hardware delivers
	                            // linear; 0: raw view, decode in-shader (licensee format)
	uint   Padding;
};

Texture2D<float4>   GBufferA : register(t0); // RGB10A2: worldnormal*0.5+0.5, a = per-object data
Texture2D<float4>   GBufferB : register(t1); // 8-bit RGBA: metallic, specular, roughness, id|mask
Texture2D<float4>   GBufferC : register(t2); // 8-bit RGBA: base colour (.a is NOT AO here — §5.5)
RWTexture2D<float4> OutNormalRoughness : register(u0);
RWTexture2D<float>  OutRoughness       : register(u1);
RWTexture2D<float4> OutDiffuseAlbedo   : register(u2);
RWTexture2D<float4> OutSpecularAlbedo  : register(u3);

// Constants shared with src/core/envbrdf.hpp — keep in lockstep.
static const float kSkyAlbedo = 0.5f;
static const float kAlbedoFloor = 0.05f;
static const float kNovScale = 0.75f;
static const uint  kShadingModelUnlit = 0;

// Streamline §4.2.1, verbatim (reference: env_brdf_approx2, src/core/envbrdf.cpp).
float3 EnvBRDFApprox2(float3 SpecularColor, float alpha, float NoV)
{
	NoV = abs(NoV);
	// [Ray Tracing Gems, Chapter 32]
	float4 X;
	X.x = 1.f;
	X.y = NoV;
	X.z = NoV * NoV;
	X.w = NoV * X.z;

	float4 Y;
	Y.x = 1.f;
	Y.y = alpha;
	Y.z = alpha * alpha;
	Y.w = alpha * Y.z;

	float2x2 M1 = float2x2(0.99044f, -1.28514f, 1.29678f, -0.755907f);
	float3x3 M2 = float3x3(1.f, 2.92338f, 59.4188f, 20.3225f, -27.0302f, 222.592f,
	                       121.563f, 626.13f, 316.627f);
	float2x2 M3 = float2x2(0.0365463f, 3.32707f, 9.0632f, -9.04756f);
	float3x3 M4 = float3x3(1.f, 3.59685f, -1.36772f, 9.04401f, -16.3174f, 9.22949f,
	                       5.56589f, 19.7886f, -20.2123f);

	float bias = dot(mul(M1, X.xy), Y.xy) * rcp(dot(mul(M2, X.xyw), Y.xyw));
	float scale = dot(mul(M3, X.xy), Y.xy) * rcp(dot(mul(M4, X.xzw), Y.xyw));

	// This is a hack for specular reflectance of 0 — the listing's own comment.
	bias *= saturate(SpecularColor.g * 50);

	return mad(SpecularColor, max(0, scale), max(0, bias));
}

// IEC 61966-2-1 (reference: srgb_to_linear). Only taken when the C view is a raw UNORM.
float3 SrgbToLinear3(float3 c)
{
	const float3 lo = c / 12.92f;
	// abs() is a no-op for UNORM input; it exists to silence fxc's X3571 (pow of a
	// possibly-negative base), which /WX would turn into a build break.
	const float3 hi = pow(abs((c + 0.055f) / 1.055f), 2.4f);
	return float3(c.x <= 0.04045f ? lo.x : hi.x,
	              c.y <= 0.04045f ? lo.y : hi.y,
	              c.z <= 0.04045f ? lo.z : hi.z);
}

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
	if (tid.x >= (uint)RenderSize.x || tid.y >= (uint)RenderSize.y)
		return;

	// The G-buffers are BUFFER-sized (allocated at FSceneRenderTargets::BufferSize, like
	// depth and velocity — RESEARCH-RR-GBUFFER.md §1.1), so index with absolute buffer
	// coordinates, output with view-local ones. Same convention as mv_resolve.hlsl.
	const uint2 buffer_pos = uint2(ViewRectMin) + tid.xy;

	const float4 a = GBufferA.Load(int3(buffer_pos, 0));
	const float4 b = GBufferB.Load(int3(buffer_pos, 0));
	const float4 c = GBufferC.Load(int3(buffer_pos, 0));

	// GBufferB.a low nibble (reference: decode_shading_model_id).
	const uint shading_model = (uint)round(saturate(b.a) * 255.0f) & 0xFu;

	// GBufferA decode (reference: decode_gbuffer_a_normal): N*2-1, renormalised. A
	// degenerate encode (the cleared-to-0.5 case) falls back to +Z rather than NaN.
	float3 n = a.xyz * 2.0f - 1.0f;
	const float n_len2 = dot(n, n);
	n = n_len2 < 1e-8f ? float3(0.0f, 0.0f, 1.0f) : n * rsqrt(n_len2);

	float roughness = b.b;
	float3 diffuse;
	float3 specular_albedo;

	if (shading_model == kShadingModelUnlit)
	{
		// Sky, cleared G-buffer, emissive-only surfaces: no BRDF to describe. Default
		// albedos per the RR guide; roughness 1 (no coherent specular lobe).
		diffuse = kSkyAlbedo.xxx;
		specular_albedo = kSkyAlbedo.xxx;
		roughness = 1.0f;
	}
	else
	{
		const float metallic = b.r;
		const float specular = b.g;
		const float3 base = CViewDecodesSrgb != 0 ? c.rgb : SrgbToLinear3(c.rgb);

		// DiffuseColor / F0 (references: diffuse_colour, compute_f0).
		diffuse = base * (1.0f - metallic);
		const float3 f0 = lerp((0.08f * specular).xxx, base, metallic);

		// NoV from the pixel's view-space ray — depth-independent — and the world-space
		// normal rotated into view space (reference: nov_from_screen). Then the UE
		// plugin's remap and 0.75 scale (reference: nov_remap, kNovScale).
		const float2 uv = (float2(tid.xy) + 0.5f) / RenderSize;
		const float2 screen_pos = float2(2.0f * uv.x - 1.0f, 1.0f - 2.0f * uv.y);
		const float3 n_view = float3(dot(WorldToViewRow0.xyz, n),
		                             dot(WorldToViewRow1.xyz, n),
		                             dot(WorldToViewRow2.xyz, n));
		const float3 ray = float3(screen_pos.x / Proj00, screen_pos.y / Proj11, 1.0f);
		float nov = saturate(dot(-normalize(ray), normalize(n_view)));
		const float x = nov * nov;
		nov = x / (x + (1.0f - nov) * (1.0f - nov));

		specular_albedo = EnvBRDFApprox2(f0, roughness * roughness, nov * kNovScale);

		// Near-zero albedo guides make RR hallucinate brightness (§2.5): floor both.
		diffuse = max(diffuse, kAlbedoFloor.xxx);
		specular_albedo = max(specular_albedo, kAlbedoFloor.xxx);
	}

	OutNormalRoughness[tid.xy] = float4(n, roughness);
	OutRoughness[tid.xy] = roughness;
	OutDiffuseAlbedo[tid.xy] = float4(diffuse, 1.0f);
	OutSpecularAlbedo[tid.xy] = float4(specular_albedo, 1.0f);
}
