// The UE 4.27.2 View uniform buffer (register b1) as Stray presents it, plus the pure
// arithmetic we derive from it. Platform-independent: no Windows or D3D types, so this
// compiles and is unit-tested on Linux as well as MSVC.
//
// Every offset and formula here cites CLAUDE.md, which in turn cites either a measurement
// against the running game or docs/RESEARCH.md. Do not add a constant without a citation.
#pragma once

#include <cstddef>
#include <cstdint>

namespace stray_dlss::ue4 {

// ---------------------------------------------------------------------------------------
// View uniform buffer layout  (CLAUDE.md §2.6)
//
// Rows are float4 rows; byte offset is row * 16. The total buffer size varies across
// permutations (126 / 131 / 145 float4s observed in one session) but these offsets do not.
//
// Rows beyond 152 depend on GMaxGlobalDistanceFieldClipmaps / MAX_PHYSICS_FIELD_TARGETS
// and were never verified. Do not add any.
// ---------------------------------------------------------------------------------------
struct ViewRow
{
	// The seven-FMatrix block occupying rows 0-27, in member-table order — HARD-via-mirror
	// (SceneView.h VIEW_UNIFORM_BUFFER_MEMBER_TABLE, ~:1078, AlexMercer-MA 4.27.2 mirror,
	// fetched 2026-08-31): TranslatedWorldToClip 0, WorldToClip 4, CLIPTOWORLD 8,
	// TranslatedWorldToView 12, ViewToTranslatedWorld 16, TranslatedWorldToCameraView 20,
	// CameraViewToTranslatedWorld 24 — then ViewToClip at 28, which is the MEASURED anchor
	// that pins the count. The first shipped guess put TranslatedWorldToView at row 8
	// (forgetting ClipToWorld) and the live run refused every frame with rows-implausible:
	// an inverse view-projection is never orthonormal, so the plausibility gate caught the
	// wrong-row read exactly as designed. Consumers still sanity-check what they read
	// (world_to_view_rotation_plausible) — a licensee edit here stays survivable.
	static constexpr std::uint32_t kViewMatrixBlock = 0;             // 7 matrices, 28 rows
	static constexpr std::uint32_t kTranslatedWorldToView = 12;      // mirror-verified, 4 rows
	static constexpr std::uint32_t kViewToClip = 28;                 // measured
	static constexpr std::uint32_t kViewToClipNoAA = 32;             // measured
	static constexpr std::uint32_t kInvDeviceZToWorldZTransform = 65; // derived
	static constexpr std::uint32_t kClipToPrevClip = 122;            // measured, 4 rows
	static constexpr std::uint32_t kTemporalAAJitter = 126;          // measured
	static constexpr std::uint32_t kViewRectMin = 129;               // measured
	static constexpr std::uint32_t kViewSizeAndInvSize = 130;        // measured
	static constexpr std::uint32_t kLightProbeSizeRatio = 131;       // measured — DECOY, reads (1,1,1,1)
	static constexpr std::uint32_t kBufferSizeAndInvSize = 132;      // derived
	static constexpr std::uint32_t kPreExposureRow = 135;            // derived, .y
	static constexpr std::uint32_t kNearPlaneRow = 142;              // derived, .x
	static constexpr std::uint32_t kDeltaTimeRow = 143;              // derived, .x
	static constexpr std::uint32_t kCameraCutRow = 145;              // derived, .x
	static constexpr std::uint32_t kTemporalAAParams = 152;          // measured
};

// Everything we need lives in this prefix. Reading more is unverified territory.
constexpr std::size_t kViewPrefixBytes = (ViewRow::kTemporalAAParams + 1) * 16; // 2448

constexpr std::size_t row_to_byte(std::uint32_t row) noexcept { return std::size_t{ row } * 16; }

// Component indices within the packed rows that hold scalars.
constexpr std::uint32_t kPreExposureComponent = 1;      // 135.y -> byte 2164
constexpr std::uint32_t kOneOverPreExposureComponent = 2; // 135.z -> byte 2168
constexpr std::uint32_t kNearPlaneComponent = 0;        // 142.x -> byte 2272
constexpr std::uint32_t kDeltaTimeComponent = 0;        // 143.x -> byte 2288
constexpr std::uint32_t kCameraCutComponent = 0;        // 145.x -> byte 2320

// ---------------------------------------------------------------------------------------
// Velocity encoding  (CLAUDE.md §2.5, UE 4.27 Common.ush:1537-1570)
//
//   EncodedV.xy = V.xy * (0.499f * 0.5f) + 32767.0f / 65535.0f
//   V.xy        = EncodedV.xy * InvDiv - (32767.0f / 65535.0f) * InvDiv
//
// Both constants were located in Stray's own DXBC. The bias is NOT 0.5. Compute the folded
// term from the expression rather than hardcoding a rounded literal.
// ---------------------------------------------------------------------------------------
constexpr float kVelocityEncodeScale = 0.499f * 0.5f;
constexpr float kVelocityEncodeBias = 32767.0f / 65535.0f; // 0.49999237f, bit pattern 0x3EFFFF00
constexpr float kVelocityInvDiv = 1.0f / kVelocityEncodeScale; // 4.00801611f, 0x408041AB
constexpr float kVelocityDecodeBias = kVelocityEncodeBias * kVelocityInvDiv; // 2.00397754f, 0x4000412B

struct Float2
{
	float x = 0.0f;
	float y = 0.0f;
};

constexpr Float2 encode_velocity(Float2 v) noexcept
{
	return { v.x * kVelocityEncodeScale + kVelocityEncodeBias,
	         v.y * kVelocityEncodeScale + kVelocityEncodeBias };
}

constexpr Float2 decode_velocity(Float2 encoded) noexcept
{
	return { encoded.x * kVelocityInvDiv - kVelocityDecodeBias,
	         encoded.y * kVelocityInvDiv - kVelocityDecodeBias };
}

// UE 4.27 clears the velocity target to 0 and the TAA shader's validity test is literally
// `EncodedVelocity.x > 0.0` — strict, red channel only. Reproduce it exactly; a magnitude
// threshold is wrong. (CLAUDE.md §2.5)
constexpr bool has_object_velocity(Float2 encoded) noexcept { return encoded.x > 0.0f; }

// ---------------------------------------------------------------------------------------
// Jitter  (CLAUDE.md §2.7)
//
// TemporalAAParams is (JitterIndex, SequenceLength, JitterPixelsX, JitterPixelsY) and .zw
// IS TemporalJitterPixels, already in render-resolution pixels within [-0.5, +0.5].
// NGX takes it with NO sign flip — NVIDIA's own UE plugin passes it through unmodified.
//
// The derivation below exists only to cross-check a captured buffer; it is never the path
// used to feed NGX.
// ---------------------------------------------------------------------------------------
constexpr Float2 jitter_pixels_from_ndc(Float2 jitter_ndc, float view_width, float view_height) noexcept
{
	return { jitter_ndc.x * view_width * 0.5f, jitter_ndc.y * view_height * -0.5f };
}

constexpr Float2 jitter_ndc_from_pixels(Float2 jitter_px, float view_width, float view_height) noexcept
{
	return { jitter_px.x * 2.0f / view_width, jitter_px.y * -2.0f / view_height };
}

// ---------------------------------------------------------------------------------------
// Camera cut  (CLAUDE.md §2.8)
//
// TemporalAAJitter is (CurX, CurY, PrevX, PrevY). On a cut UE4 assigns
// PrevViewMatrices = ViewMatrices, so .zw becomes equal to .xy.
//
// This signal alone is NOT sufficient: bCameraCut is `!InputHistory.IsValid() ||
// View.bCameraCut`, and the history-invalid case never reaches the View buffer. The caller
// must OR this with View.CameraCut (row 145.x) and with "history or velocity SRV is 1x1".
// ---------------------------------------------------------------------------------------
constexpr bool jitter_indicates_camera_cut(Float2 current, Float2 previous) noexcept
{
	return current.x == previous.x && current.y == previous.y;
}

// ---------------------------------------------------------------------------------------
// DLSS sequencing helpers  (docs/RESEARCH.md §3.3)
// ---------------------------------------------------------------------------------------

// NVIDIA: Total Phases = Base * (Target / Render)^2, base 8. Documented minimums are
// DLAA 8, Quality 18, Balanced 24, Performance 32, UltraPerformance 72.
constexpr int kBaseJitterPhases = 8;

int jitter_phase_count(std::uint32_t render_height, std::uint32_t output_height) noexcept;

// Mip LOD bias for a given resolution scale, matching Luma's formula.
float mip_lod_bias(std::uint32_t render_height, std::uint32_t output_height) noexcept;

// Halton sequence, returning [-0.5, 0.5] in pixel space. Base 2 for x, base 3 for y.
// DLSS was trained on Halton. Note UE4 only emits plain Halton in TemporalUpscale mode;
// in the shipped non-upsampling mode it warps it through Box-Muller. (CLAUDE.md §2.7)
float halton(unsigned int index, unsigned int base) noexcept;

} // namespace stray_dlss::ue4
