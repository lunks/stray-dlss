// Deciding whether a compute dispatch is Stray's FTAAStandaloneCS.
//
// Identity is deliberately NOT hash-only. Stray's TAA hash tracks r.PostProcessAAQuality
// (currently 3, Medium), so a user changing the in-game AA setting would silently break a
// hash-only hook. The structural signature is what gates interception; the hash is a fast
// pre-filter, a confirmation, and an exclusion list. (CLAUDE.md §2.3)
//
// Pure logic: no Windows, no D3D, no ReShade. The caller translates real D3D12 formats and
// resource pointers into these types.
#pragma once

#include <cstdint>
#include <vector>

namespace stray_dlss {

// Only the formats this decision needs to distinguish.
enum class TexFormat
{
	unknown,
	r32_float_x8x24_typeless, // the depth view over the R32G8X24_TYPELESS resource
	x32_typeless_g8x24_uint,  // the stencil view over the SAME resource
	r16g16b16a16_unorm,       // UE4 velocity
	r16g16b16a16_float,       // scene colour, history, TAA output
	r16g16_float,             // our resolved motion vectors
	r32g32b32a32_float,       // eye adaptation (1x1)
};

struct BoundTexture
{
	std::uint32_t slot = 0;     // t or u register
	std::uint64_t resource = 0; // ID3D12Resource*, as an opaque identity
	TexFormat format = TexFormat::unknown;
	std::uint32_t width = 0;
	std::uint32_t height = 0;
};

struct DispatchSignature
{
	std::vector<BoundTexture> srvs;
	std::vector<BoundTexture> uavs;
	bool has_view_cb = false;
	std::uint32_t group_count_x = 0;
	std::uint32_t group_count_y = 0;
	std::uint32_t group_count_z = 0;
	std::uint64_t shader_hash = 0;
};

enum class MatchVerdict
{
	no_match,
	structural_only,     // the bindings fit but the hash is unfamiliar
	hash_and_structural, // both agree — the strongest signal
	excluded,            // a hash we have positively identified as NOT the TAA pass
};

struct MatchResult
{
	MatchVerdict verdict = MatchVerdict::no_match;
	const char *reason = "";

	std::uint32_t depth_srv = 0;
	std::uint32_t stencil_srv = 0;
	std::uint32_t velocity_srv = 0;
	std::uint32_t colour_srv_a = 0; // which is scene colour and which is history cannot be
	std::uint32_t colour_srv_b = 0; // decided here — it needs last frame's u0 pointer
	std::uint32_t output_uav = 0;
	bool has_downsample_uav = false;
};

// Measured shader identities. CLAUDE.md §2.3.
constexpr std::uint64_t kTaaMainHash = 0x1708ec956099e259ull;
constexpr std::uint64_t kSecondCandidateHash = 0x52101a15e1a0c5ccull;
constexpr std::uint64_t kKnownFalsePositiveHash = 0x901e041a7cadc9dbull;

// GTemporalAATileSizeX/Y. The dispatch is ceil(viewrect / 8). (docs/RESEARCH.md §4.1)
constexpr std::uint32_t kTaaTileSize = 8;

MatchResult match_taa_dispatch(const DispatchSignature &sig,
                               std::uint32_t view_width,
                               std::uint32_t view_height);

} // namespace stray_dlss
