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
	r11g11b10_float,          // UE4's usual scene-colour format
	r10g10b10a2_unorm,        // the swapchain
	r32_float,
	r16_float,
	r8g8b8a8_unorm,
	b8g8r8a8_unorm,           // UE4's PF_B8G8R8A8 — stock GBufferB/C
};

// True for any format DLSS will accept as pInColor: an HDR-capable float colour target.
bool is_hdr_colour(TexFormat f);

struct BoundTexture
{
	std::uint32_t slot = 0;     // t or u register
	// Identity ONLY — for comparing bindings between frames (the history round-trip). Never
	// dereference it: ReShade does not call destroy_resource_view on D3D12, so its view->
	// resource map can hand back a pointer to a destroyed resource. (docs/RESEARCH.md §2.7)
	std::uint64_t resource = 0;
	TexFormat format = TexFormat::unknown;
	std::uint32_t width = 0;
	std::uint32_t height = 0;
	// The game's own CPU descriptor handle for this binding. Copying this into our heap is the
	// safe way to read the resource: it needs no resource pointer and no format guessing.
	// Last so the positional initialisers in the tests stay meaningful.
	std::uint64_t descriptor = 0;
	// True for a 3D (volume) texture view. The dataflow walk anchors on this: the tonemapper
	// is the only pass in an Unreal frame that binds a 3D texture SRV — the colour-grading
	// LUT (PostProcessTonemap.cpp:533,765). Appended after `descriptor` so the positional
	// initialisers above keep their meaning.
	bool is_3d = false;
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

	// The output RECT — the rect the dispatch actually writes (group count x 8, clamped to the
	// UAV), not the UAV's allocation. UE 4.27 dispatches over OutputViewRect while allocating
	// at FTAAPassParameters::GetOutputExtent(), which is only ever >= that rect, so the two are
	// different quantities and DLSS must be created for the rect.
	std::uint32_t output_width = 0;
	std::uint32_t output_height = 0;
	// The render resolution, taken from the depth SRV rather than the View buffer, so it is
	// available even on frames where the constant buffer cannot be read.
	std::uint32_t render_width = 0;
	std::uint32_t render_height = 0;
	bool is_upsampling = false;

	// True when velocity or the history colour is the 1x1 GSystemTextures::BlackDummy, which
	// UE4 substitutes on a camera cut or the first frame. Not a reason to reject the pass —
	// it IS the pass, resetting. It is, however, exactly what should drive NGX InReset.
	bool camera_cut_dummies = false;
};

// Shader identities, CORRECTED against the live game and the shipped bytecode. CLAUDE.md §2.3.
//
// The original measurement had these backwards, and the mistake is instructive. It scored
// 0x901e041a7cadc9db as a false positive on "colour=1 depth=2 velocity=0" — but that is
// exactly a camera-cut frame, where UE4 swaps velocity for the 1x1 BlackDummy, and depth
// counts twice because the depth SRV and the stencil SRV are two views of ONE resource. Both
// halves of the disqualifying score are in fact the TAA's own signature.
//
// Confirmed independently from the bytecode: every FTAAStandaloneCS permutation reads
// View.StateFrameIndexMod8 at row 144 and so must declare cb1[145]; 0x1708ec declares
// cb1[126] and therefore cannot be this shader at all. 0x901e additionally declares
// dcl_tgsm_structured stride 16 count 64 (float4[64]), which only compiles under
// AA_UPSAMPLE==1, and carries the upsample-only kernel constants 0.905 / -1.9.
constexpr std::uint64_t kTaaMainHash = 0x901e041a7cadc9dbull;

// True when `hash` is one of the cooked FTAAStandaloneCS permutations extracted offline from
// the game's own shader cache (src/core/taa_hashes.hpp, 27 entries). This is the hash half of
// identification: ANY permutation counts, so resolution / screen-percentage / AA-quality
// changes keep the strong verdict without live rediscovery.
bool is_known_taa_hash(std::uint64_t hash);

// Extend the known-permutation set at runtime with hashes from an operator-supplied override
// file (a game update recooks the shaders; the user regenerates the list offline and drops it
// beside the add-on — no rebuild). Replaces any previous extra set; capped at 64 entries.
void set_extra_taa_hashes(const std::uint64_t *hashes, std::size_t count);

// NAMED offline 2026-08-31 (shaderlib_extract.py --find-hash): FSSDTemporalAccumulationCS
// permutation #5 — UE 4.27's Screen-Space Denoiser temporal accumulation. It reads depth and
// velocity and reprojects with ClipToPrevClip, which is why its bindings look convincing and
// why it was mistaken for the TAA pass. cb1[126], eight float SRVs, no uint2 stencil view.
constexpr std::uint64_t kDenoiserLookalikeHash = 0x1708ec956099e259ull;

// FSSDTemporalAccumulationCS permutation #0 (same shader type as the above; the family has 9
// cooked permutations). Eleven SRVs, two UAVs, cb1[131].
constexpr std::uint64_t kSecondCandidateHash = 0x52101a15e1a0c5ccull;

// True when `hash` is a known FSSDTemporalAccumulationCS permutation — the SSD
// temporal-accumulation family whose dispatches READ the G-buffers as SRVs, which makes
// the first one each frame the content-alive trigger point for the RR guide resolve
// (measured 2026-08-31: at the TAA dispatch the G-buffer OBJECTS are alive but their
// CONTENT is already recycled — near-black, unlit fallback over ~95% of the frame).
//
// Nine permutations are cooked (the family was suppressed wholesale in the 3.4x
// denoiser-suppression experiment via the operator's dry-run file, whose list was never
// committed); the TWO named offline are baked here — permutation #5 is the one measured
// dispatching steadily in live gameplay, so the trigger works with the baked set — and
// set_extra_ssd_hashes extends the family at runtime from stray-dlss-ssd-hashes.txt,
// exactly like the TAA override, so the deploy tooling can supply all nine, no rebuild.
bool is_ssd_temporal_hash(std::uint64_t hash);
void set_extra_ssd_hashes(const std::uint64_t *hashes, std::size_t count);

// The eye-adaptation SRV of an intercepted TAA dispatch: register t0, 1x1
// R32G32B32A32_FLOAT, present EVERY frame (CLAUDE.md §2.3 — it is NOT one of the
// camera-cut dummies: those replace only the velocity and colour slots, and the BlackDummy
// is not RGBA32F). Returns the resource handle, or 0 when absent. Register t0 wins when it
// fits the signature; otherwise the only structural 1x1-RGBA32F binding is accepted, so a
// permutation with shifted registers still yields the texture. Used as DLSS's exposure
// texture under [STRAYDLSS] NgxExposure=texture — the official UE plugin passes exactly
// this texture, unmodified, as pInExposureTexture (DLSSUpscaler.cpp:1111-1115 /
// NGXD3D12RHI.cpp:275-276, plugin v8.3.0-NGX310.4.0, re-verified 2026-09-03).
std::uint64_t find_eye_adaptation_srv(const std::vector<BoundTexture> &srvs);

// GTemporalAATileSizeX/Y. The dispatch is ceil(viewrect / 8). (docs/RESEARCH.md §4.1)
constexpr std::uint32_t kTaaTileSize = 8;

// `view_width`/`view_height` are the RENDER rect (View.ViewSizeAndInvSize). The dispatch is
// sized over the OUTPUT rect, which under temporal upsampling is larger, so the match is made
// against the output UAV's own extent instead of against the render rect.
MatchResult match_taa_dispatch(const DispatchSignature &sig,
                               std::uint32_t view_width,
                               std::uint32_t view_height);

} // namespace stray_dlss
