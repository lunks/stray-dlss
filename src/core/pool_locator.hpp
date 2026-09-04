// Naming UE 4.27's pooled render targets from outside the engine, by locating
// FRenderTargetPool::FindFreeElement the way UEVR does: anchor on a CALLER's string literal,
// resolve the rip-relative reference that loads it, and take the direct CALL that follows.
//
// WHY THIS EXISTS, and why docs/RESEARCH-U0-IDENTITY.md §4.1/§4.2 said it could not.
//
// §4.1 refused "name the resource" because no D3D12 OBJECT carries an engine name in a
// Shipping build — RHIBindDebugLabelName is inside `#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)`
// and NAME_OBJECTS is 0. Both halves are true and neither is the point. The name is a LIVE
// FUNCTION ARGUMENT: `FindFreeElementInternal` does `CreateInfo.DebugName = InDebugName`
// (RenderTargetPool.cpp:415 @ 4.27/306a7e9) outside any #if, and `FindFreeElement` takes it as
// `const TCHAR* InDebugName` (RenderTargetPool.h:268-274). A hook on that function is handed
// the name on every call. praydog/UEVR ships exactly this across hundreds of shipping UE
// titles (src/mods/vr/RenderTargetPoolHook.cpp), which is the field confirmation.
//
// §4.2 refused the pool because the function has "no self-validating constant, no
// shipping-surviving string anchor". It looked for a literal INSIDE the function. UEVR looks
// for a literal in the CALLER and decodes forward to the CALL — and 4.27's
// SceneRenderTargets.cpp alone has 25 such call sites, each with a distinct literal, spread
// over SIX distinct enclosing functions (see kNames). The self-validating constant is the
// argument itself, checked on EVERY call rather than once at install.
//
// PROVENANCE. Every name and line number below is HARD, read from
// AlexMercer-MA/UnrealEngine-4.27 @ 306a7e9 (Build.version 4.27.2) on 2026-09-04. That any of
// it holds on Stray-Win64-Shipping.exe is UNCONFIRMED until the scan runs on the box — which
// is why nothing here executes anything it finds, and why the verdict is a refusal by default.
//
// WINDOWS-PORTABLE ONLY. This file is pure: bytes in, verdict out. It knows the PE exception
// directory (via u0::FunctionTable) and MSVC's x86-64 encodings for `lea reg,[rip+d]` and
// `call rel32`, and nothing else. Nothing here consults vkd3d, DXVK or Wine.
#pragma once

#include "core/engine_seam.hpp"
#include "core/u0_rhi_uav.hpp"

#include <cstddef>
#include <cstdint>

namespace stray_dlss::pool {

// ---------------------------------------------------------------------------------------
// The names
// ---------------------------------------------------------------------------------------

// Every pooled render target this plugin has a reason to know, in the order the report prints
// them. `Target::count` is a bound, not a claim that Stray allocates all of them.
enum class Target : std::uint8_t
{
	gbuffer_a = 0,     // RR: base colour / specular colour, depending on the GBuffer format
	gbuffer_b,         // RR: metallic / specular / roughness
	gbuffer_c,         // RR: diffuse colour + indirect irradiance
	gbuffer_d,         // RR: custom data
	gbuffer_e,         // RR: precomputed shadow factors
	gbuffer_f,         // RR: anisotropy (4.27's AllocateAnisotropyTarget)
	scene_depth_z,     // the depth L1 already resolves from FPassInputs — the ORACLE pair
	scene_depth_aux,
	gbuffer_velocity,  // the velocity L1 already resolves — the second ORACLE pair
	scene_color_deferred,
	small_depth_z,
	screen_space_ao,
	light_accumulation,
	directional_occlusion,
	quad_overdraw_buffer,
	sky_sh_irradiance_map,
	reflection_scratch_0,
	reflection_scratch_1,
	diffuse_irradiance_0,
	diffuse_irradiance_1,
	count
};

// One pooled render target we know by name.
//
// `anchor` is whether the literal is passed AT the call site, which is what makes it usable to
// locate the function. SceneColorDeferred is not: AllocSceneColor passes
// `GetSceneColorTargetName(CurrentShadingPath)`, which RETURNS the literal from a table
// (SceneRenderTargets.cpp:315-324), so the reference to it lives in a different function
// entirely. It is still recorded at the hook — it just cannot anchor the scan.
//
// `source_fn` is the enclosing function in 4.27's source, recorded so a reader can see the
// independence the scan measures; the scan itself derives grouping from the exe's own .pdata
// and never consults this string.
struct Name
{
	const char *text = "";
	Target target = Target::count;
	bool anchor = false;
	const char *source_fn = "";
};

// HARD, SceneRenderTargets.cpp @ 4.27/306a7e9, line numbers in the comments.
extern const Name kNames[];
constexpr std::size_t kNameCount = static_cast<std::size_t>(Target::count);
static_assert(kNameCount <= 32, "the candidate name mask is a uint32");

// The name index (0..kNameCount-1) for an ASCII string, or -1. Exact match; a longer name
// that merely starts with one of ours is not a hit.
int match_name(const char *ascii);
const char *target_name(Target t);

// ---------------------------------------------------------------------------------------
// The ladder, [STRAYDLSS] PoolNames
// ---------------------------------------------------------------------------------------

enum class Level : std::uint8_t
{
	off = 0,
	discover,   // scan the module, log the verdict, INSTALL NOTHING
	observe,    // additionally install the forwarding recorder and assert; image byte-identical
	supply,     // DECLARED, NOT BUILT — feeding RR its guides is a separate decision
};
Level level_from_int(int v);
const char *level_name(Level l);

// Default 1: discovery only. This is the project's first inline hook into ENGINE CODE, and
// §9's rule is that nothing is installed on a guess. Pinned by a test so moving the rung is a
// deliberate edit (the discipline tests/test_nr_history_plan.cpp applies to NgxNRRestoreHistory).
constexpr int kDefaultLevel = 1;

// ---------------------------------------------------------------------------------------
// Locating FRenderTargetPool::FindFreeElement
// ---------------------------------------------------------------------------------------

// How far past the `lea` that loads the name we look for the CALL. MSVC /O2 stores a stack
// argument close to its call, but the desc construction inlined around it can be long, so this
// is generous: correctness comes from the .pdata filter and cross-anchor agreement, not from
// the window being tight.
constexpr std::size_t kForwardBytes = 0x100;
constexpr std::size_t kMaxCallsPerSite = 16;
constexpr std::size_t kMaxLiteralHitsPerName = 4;   // string pooling can duplicate a literal
constexpr std::size_t kMaxLeaSites = 128;
constexpr std::size_t kMaxCandidates = 64;
constexpr std::size_t kMaxGroupsPerCandidate = 12;

// THE ACCEPTANCE BAR, and it is the equivalent of the seam's three constants. A candidate must
// be reached from at least this many DISTINCT enclosing functions (derived from the exe's own
// .pdata, not from kNames::source_fn) and this many DISTINCT name literals, and it must beat
// the runner-up on groups OUTRIGHT. A tie is ambiguity and ambiguity is a refusal.
constexpr unsigned kMinAgreeingGroups = 3;
constexpr unsigned kMinAgreeingNames = 4;

enum class LocateStatus : std::uint8_t
{
	ok = 0,
	no_regions,       // nothing to scan
	no_literals,      // not one of the UTF-16 name literals is in the image
	no_references,    // literals present, but nothing loads them with lea reg,[rip+d]
	no_candidates,    // references present, but no rel32 CALL in any window lands on a .pdata function start
	insufficient,     // a best candidate exists and it does not clear kMinAgreeing*
	ambiguous,        // two candidates tied on distinct enclosing functions — refuse, never pick
	count
};
const char *locate_status_text(LocateStatus s);

struct Candidate
{
	std::uint64_t target = 0;
	std::uint32_t name_mask = 0;
	unsigned name_count = 0;
	std::uint64_t groups[kMaxGroupsPerCandidate] = {};
	unsigned group_count = 0;
	unsigned sites = 0;
	std::uint32_t min_distance = 0xFFFFFFFFu;
};

struct Locate
{
	LocateStatus status = LocateStatus::no_regions;

	std::uint64_t target = 0;      // FRenderTargetPool::FindFreeElement, on success
	unsigned names = 0;            // distinct name literals that reached it
	unsigned groups = 0;           // distinct enclosing functions that reached it
	unsigned sites = 0;            // lea sites that reached it
	std::uint32_t name_mask = 0;   // which of kNames, so the log can list them
	std::uint32_t min_distance = 0;

	std::uint64_t runner_up = 0;
	unsigned runner_up_names = 0;
	unsigned runner_up_groups = 0;

	// Stage counts, so a failure names the stage that refused rather than needing a round trip.
	unsigned literals_found = 0;   // names whose UTF-16 literal is in the image
	unsigned refs_found = 0;       // names with at least one lea reference
	unsigned lea_sites = 0;        // total lea sites examined
	unsigned candidates = 0;       // distinct call targets scored
	bool lea_sites_overflowed = false;
	bool candidates_overflowed = false;

	// The first 16 bytes at the winner, so a human can eyeball a function prologue in the log
	// instead of trusting the scan's word for it.
	unsigned char entry[16] = {};
	bool entry_read = false;

	// THE RESIDUAL docs/RESEARCH-U0-EXTERNAL-PRIOR-ART.md §2.5 flags, SETTLED rather than
	// assumed. `FindFreeElement` (what the anchors reach) and `FindFreeElementForRDG` (what
	// u0/TemporalAA goes through) are two functions that both forward to
	// `FindFreeElementInternal`. The cheapest falsifiable test is whether this image still
	// carries `%d MB, NewRT %s %s` — the UE_LOG format string INSIDE FindFreeElementInternal
	// (RenderTargetPool.cpp:403). Present means USE_LOGGING_IN_SHIPPING and a unique
	// in-function anchor for the internal; absent closes that route and says so.
	std::uint64_t internal_log_literal_va = 0;
	unsigned internal_log_hits = 0;
	std::uint64_t internal_fn = 0;       // the function containing the reference to it
	unsigned internal_fn_candidates = 0; // distinct such functions (1 is the clean answer)
	// Does our winner CALL it? RenderTargetPool.cpp:703 says FindFreeElement's last act is
	// `FindFreeElementInternal(...)`, so when the literal survives this is a fourth, entirely
	// independent confirmation of the winner. False with internal_fn==0 means "not testable".
	bool internal_called_by_target = false;
};

// The whole scan. Never executes anything it finds; never reads outside `image`.
Locate locate(const seam::Image &image, const u0::FunctionTable &table);

// Exposed because each is independently useful in a diagnostic, and because the tests drive
// them directly.
//
// One pass over every readable region for ALL of kNames at once — 15 separate scans of a
// ~200 MB shipping image on the device-creation thread is a startup stall this project has
// paid for before. `out[i][j]` receives up to kMaxLiteralHitsPerName VAs for name i.
void find_name_literals(const seam::Image &image,
                        std::uint64_t out[][kMaxLiteralHitsPerName], unsigned *hits);

// One pass over every EXECUTABLE region decoding `lea reg,[rip+disp32]` (REX.W 8D /r with
// mod=00 rm=101, i.e. 48/4C 8D <05|0D|15|1D|25|2D|35|3D> <disp32>) and reporting each whose
// rip-relative target is one of `targets`. Returns the number of sites written.
struct LeaSite
{
	std::uint64_t va = 0;      // the lea itself
	std::uint32_t which = 0;   // index into `targets`
};
std::size_t find_lea_rip_refs(const seam::Image &image,
                              const std::uint64_t *targets, const std::uint32_t *owners,
                              std::size_t target_count, LeaSite *out, std::size_t max_out);

// Every `E8 rel32` in [from+7, from+kForwardBytes) whose target is a function START per the
// image's own .pdata AND lies in executable memory. `distance` is bytes from `from`.
struct CallHit
{
	std::uint64_t target = 0;
	std::uint32_t distance = 0;
};
std::size_t collect_calls_after(const seam::Image &image, const u0::FunctionTable &table,
                                std::uint64_t from, CallHit *out, std::size_t max_out);

// True when `container`'s .pdata extent holds an `E8 rel32` targeting `callee`.
bool function_calls(const seam::Image &image, const u0::FunctionTable &table,
                    std::uint64_t container, std::uint64_t callee);

// ---------------------------------------------------------------------------------------
// What the hook records, and how a wrong answer is refused
// ---------------------------------------------------------------------------------------

// IPooledRenderTarget, RendererInterface.h:477-525 @ 4.27/306a7e9: one vptr (it declares a
// virtual destructor and eight more virtuals, no virtual bases) and exactly one data member,
// `FSceneRenderTargetItem RenderTargetItem`, whose first two members are
// `FTextureRHIRef TargetableTexture` and `FTextureRHIRef ShaderResourceTexture`
// (RendererInterface.h:462-465). A TRefCountPtr is one pointer. So:
constexpr std::size_t kPooledTargetableTexture = 8;
constexpr std::size_t kPooledShaderResourceTexture = 16;
// HARD from the header, and corroborated in the field by UEVR's own field-tested
// `struct IPooledRenderTarget : IRefCountedObject { virtual bool IsFree(); virtual void*
// GetDesc(); FSceneRenderTargetItem item; }` (shared/sdk/StereoStuff.hpp), which is the same
// +8 across many UE builds.

// Why a recorded name did not become an ID3D12Resource. Every one is counted; none is silent.
enum class RecordStatus : std::uint8_t
{
	ok = 0,
	unknown_name,      // a plausible wide string we do not have a Target for — census only
	name_unreadable,   // the const TCHAR* argument did not read as a wide string AT ALL
	out_null,          // the TRefCountPtr& is null after the call (bDeferTextureAllocation)
	pooled_unreadable,
	rhi_null,          // TargetableTexture is null — deferred allocation, expected for SceneDepthZ
	rhi_unreadable,
	fn_not_code,       // the GetNativeResource slot does not point into code
	not_registered,    // the ID3D12Resource is not one our registry calls live — REFUSED
	count
};
const char *record_status_text(RecordStatus s);

// The level-2 assertion, one per oracle pair. Pure so the classification is tested rather than
// asserted: `absent` is not a disagreement and must never be counted as one.
enum class AssertVerdict : std::uint8_t
{
	absent = 0,   // one side had nothing this frame — not evidence either way
	agree,
	disagree,
	count
};
const char *assert_verdict_text(AssertVerdict v);
// `pool_a`/`pool_b` are the pooled target's Targetable and ShaderResource resources: for a
// non-MSAA target they are the same object, and either matching is agreement.
AssertVerdict judge_resource(std::uint64_t pool_a, std::uint64_t pool_b, std::uint64_t other);

// The extent assertion. GBufferA-E, SceneDepthZ and GBufferVelocity are all allocated at the
// SCENE BUFFER extent (SceneRenderTargets.cpp uses GetBufferSizeXY() for each), which is
// exactly View row 132 `BufferSizeAndInvSize` — a number that reaches us from the engine's own
// CachedViewUniformShaderParameters by a completely different route (report §19). `expected_*`
// of 0 means we do not know it this frame, which is `absent`.
AssertVerdict judge_extent(std::uint32_t got_w, std::uint32_t got_h,
                           std::uint32_t expected_w, std::uint32_t expected_h);
// Which targets are predicted to sit at the scene-buffer extent. Everything else is recorded
// and reported but predicted nothing, because predicting what we have not read is how a
// counter starts lying.
bool extent_is_predicted(Target t);

} // namespace stray_dlss::pool
