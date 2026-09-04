// THE VIEW CONSTANT BUFFER FROM THE ENGINE'S OWN CPU STRUCT, instead of by search.
//
// Pure logic: no Windows, no D3D12, no engine headers. Every dereference goes through a
// caller-supplied reader that may refuse, so a wrong offset is a STATUS and never a fault. The
// live half (src/view_params_hook.cpp) supplies VirtualQuery + SEH readers; the tests supply a
// synthetic FViewInfo laid out in a byte vector.
//
// ---------------------------------------------------------------------------------------
// WHY THIS EXISTS, AND WHY NO STRUCTURAL TEST COULD REPLACE IT
// ---------------------------------------------------------------------------------------
//
// The View CB is located today by SEARCH: try every bound root CBV in slot order, keep the first
// that decodes as a plausible `View` (src/taa_hook.cpp). Measured on the box (facts §36.20), it
// is right 99.67% of the time, and the residue is a STALE RING COPY OF THE SAME VIEW:
//
//     VIEW CB AMBIGUITY ON A CLAIMED DISPATCH: 480x270 groups (covers 3840x2160 px).
//     We used b3 = 1920x1080 (fraction 0.500, engine minimum 0.5 -> OK).
//     All plausible candidates: b3=1920x1080 b4=1920x1080
//
// Both candidates are the real render rect for the SAME view. Both pass extent, plausibility,
// row 135 and the engine's own minimum resolution fraction - and they still disagree on
// `ClipToPrevClip`, jitter and `CameraCut`. UE4 sub-allocates the View uniform buffer from a
// constant ring, a previous frame's copy is still bound on a lower root parameter, and the search
// walks slot order, so it takes the stale one. `ambClaimed=36` of ~10 800 claimed dispatches.
//
// **No structural test can separate those two, because both genuinely ARE this view's uniform
// buffer.** They differ only in WHEN they were written, and nothing in the bytes says when.
//
// ---------------------------------------------------------------------------------------
// THE ROUTE: ONE pointer in the FViewInfo we are already handed
// ---------------------------------------------------------------------------------------
//
// HARD, UE 4.27.2 (AlexMercer-MA/UnrealEngine-4.27 @ 306a7e9, read 2026-09-04):
//
//   SceneRendering.h, class FViewInfo : public FSceneView, right after `FIntRect ViewRect` and
//   `FSceneViewState* ViewState`:
//       /** Cached view uniform shader parameters, to allow recreating the view uniform buffer
//           without having to fill out the entire struct. */
//       TUniquePtr<FViewUniformShaderParameters> CachedViewUniformShaderParameters;
//
//   SceneRendering.cpp, FViewInfo::InitRHIResources():
//       CachedViewUniformShaderParameters = MakeUnique<FViewUniformShaderParameters>();
//       ... SetupUniformBufferParameters(..., *CachedViewUniformShaderParameters);
//       ViewUniformBuffer = TUniformBufferRef<FViewUniformShaderParameters>::
//           CreateUniformBufferImmediate(*CachedViewUniformShaderParameters, UniformBuffer_SingleFrame);
//
//   TemporalAA.cpp:767:  PassParameters->ViewUniformBuffer = View.ViewUniformBuffer;
//
// So the 2448-byte prefix `read_view_cb` maps out of the D3D12 upload ring is a COPY OF THIS
// STRUCT, made by the engine from this struct. The only other writer is
// `FViewInfo::UpdateLateLatchData()` (VR late-latching, `bLateLatchingEnabled`), which copies the
// struct INTO the buffer again - the struct is always the source, never the buffer. And
// `CreateSnapshot()` memcpys a NULL TUniquePtr into a snapshot view, so a snapshot carries none:
// that reads as `empty`, never as a wrong answer.
//
// TUniquePtr<T> with the default deleter is one pointer (the deleter is an empty base), so the
// route is ONE offset into `FViewInfo` whose pointee is the 2448-byte prefix we already parse.
// `AddPasses` hands us `const FViewInfo&`; we already stand in for it (src/engine_seam_hook.cpp).
//
// ---------------------------------------------------------------------------------------
// DISCOVERED, NOT DERIVED - and what makes the discovery safe
// ---------------------------------------------------------------------------------------
//
// `sizeof(FSceneView)` is unobservable from outside (FFinalPostProcessSettings alone is a few
// KB of `#if`-conditioned members), so the offset is SCANNED for over a bounded window and a
// candidate must pass, in order:
//
//  1. it is a readable heap pointer whose pointee is readable for kViewPrefixBytes;   (reader)
//  2. the pointee parses and passes `view_params_plausible`;                           (contents)
//  3. row 135 passes: y*z == 1.0 exactly, x denormal, w == 0 (CLAUDE.md §2.6);        (self-check)
//  4. its ViewSizeAndInvSize fits the announcement's own output rect, from above AND
//     from below (`view_fits_dispatch`, `view_fraction_plausible`);                     (engine rect)
//  5. its BufferSizeAndInvSize (row 132) equals the extent of the depth texture L1
//     resolved for the same announcement - HARD that row 132 is
//     `SceneContext.GetBufferSizeXY()` (SetupUniformBufferParameters), [derived] that
//     SceneDepthZ is allocated at that size;                                          (L1's extent)
//  6. AT CLAIM, its bytes are IDENTICAL to what the search read out of the bound
//     constant buffer for the dispatch this announcement matched.                     (THE ONE)
//
// Prediction 6 is the discriminating validator: it ties the candidate to a buffer the engine
// demonstrably BOUND. A wrong offset that lands on some other pointer cannot produce 2448 bytes
// equal to the bound View buffer; and once it has done so kLatchAgreements times running, the
// offset is the field. After the latch, a byte DISAGREEMENT is the search reading a stale ring
// copy - the 0.33% - and the counter it lands in is `disagree`, never a demotion.
//
// Nothing else in `FViewInfo` points at a block that passes 2-5: `ViewUniformBuffer` is an RHI
// object whose bytes live in the upload heap, not in the object; `PrevViewInfo` holds textures.
// If two candidates ever survive on one announcement the latch REFUSES for the session rather
// than picking by scan order - which would be this file's own bug, one level down.
#pragma once

#include "view_params.hpp"

#include <cstddef>
#include <cstdint>

namespace stray_dlss::viewcached {

// ---------------------------------------------------------------------------------------
// Layout constants
// ---------------------------------------------------------------------------------------

// The window scanned over the FViewInfo, in bytes from its start. FSceneView is several KB
// (two FViewMatrices, FFinalPostProcessSettings, a convex volume) and the field sits at the
// start of FViewInfo's own members, so 32 KB is generous on purpose: an `absent` verdict must
// mean "not there", not "window too small". Reading past the object's end lands in the
// FSceneRenderer's Views array or heap slack, which the reader's page check governs.
constexpr std::size_t kScanWindowBytes = 32768;

// A pointer-shaped qword whose pointee cannot be probed is skipped; probing costs a VirtualQuery
// in the live half, so the number of probes per scan is bounded and the bound is REPORTED.
//
// THE BUDGET IS THE WHOLE WINDOW, and it has to be, because the claim this scan makes is
// "EXACTLY ONE offset survived" - a claim about the entire window, which a scan that stopped
// short of the end cannot make. That matters precisely because the latch REFUSES on two
// survivors rather than picking one: a second survivor hiding in the unjudged tail is the one
// reading that would change what we do, and it is the one a truncated scan cannot see.
// MEASURED 2026-09-04 on the box (facts §36.22) at the old budget of 2048:
//
//     candidates by stage: qwords=3973 pointer-shaped=2049 probed=2048 readable=1986
//       plausible=1 row135=1 fitsRect=1 aboveMinFraction=1 bufferSize=1 survivors=1
//       <- TRUNCATED: the probe budget ran out before the window was fully judged
//
// It broke at qword 3973 of 4096 with one survivor already found, so the answer stood - but only
// 97% of the window had been judged and the other 3% was unexamined. At the observed pointer
// density (2049 pointer-shaped qwords in 3973, 0.516) the unjudged tail holds about 63 more
// pointer-shaped qwords, so a full-window scan probes ~2112 rather than 2048: **about 64 more
// probes, ~156 KB more guarded reading, per scan.** A probe that gets past the range test costs
// one region-cache lookup plus a kViewPrefixBytes (2448) memcpy under SEH, so a whole-window
// scan is ~5.2 MB of guarded copying - and it runs ONLY while `searching`, which the same launch
// measured ending after 8 claimed announcements. Once latched an announcement costs one guarded
// qword and one guarded 2448-byte read, whatever this constant says.
//
// Deriving it from the window rather than picking a round number is what makes `truncated` mean
// something again: with the default window it CANNOT fire, so if it ever does, someone widened
// the window without raising the budget, which is exactly what the flag should catch.
constexpr unsigned kMaxProbesPerScan =
	static_cast<unsigned>(kScanWindowBytes / sizeof(std::uint64_t)); // 4096

// ---------------------------------------------------------------------------------------
// The reader contract
// ---------------------------------------------------------------------------------------

struct Reader
{
	// One aligned qword at `va`. False when it is not safely readable.
	bool (*read_u64)(void *ctx, std::uint64_t va, std::uint64_t *out) = nullptr;
	// `n` bytes at `va`, into `out`. False when any part is not safely readable.
	bool (*read_block)(void *ctx, std::uint64_t va, std::size_t n, void *out) = nullptr;
	void *ctx = nullptr;
};

// What the announcement already knows, so the candidate can be checked against the ENGINE's
// answers rather than against shape alone. Zero means "not known - do not invent a refusal".
struct Expectation
{
	std::uint32_t out_width = 0;    // the engine's own OutputViewRect for this AddPasses
	std::uint32_t out_height = 0;
	std::uint32_t buffer_width = 0; // the extent of the depth texture L1 resolved (row 132)
	std::uint32_t buffer_height = 0;
};

enum class Refusal : std::uint8_t
{
	accepted = 0,
	not_a_pointer,   // the qword is not pointer-shaped
	unreadable,      // pointer-shaped, but the pointee is not readable for the prefix
	implausible,     // parsed, but `view_params_plausible` refused it
	row135_failed,   // a View read at a slipped offset, or not a View at all
	does_not_fit,    // ViewSize larger than the announcement's output rect
	fraction,        // ViewSize below the engine's own 0.5 minimum fraction of that rect
	buffer_mismatch, // row 132 disagrees with the depth extent L1 resolved
	count
};
const char *refusal_name(Refusal r);

struct Candidate
{
	std::uint32_t offset = 0;   // byte offset into the FViewInfo
	std::uint64_t pointee = 0;  // the FViewUniformShaderParameters* it held
	ue4::ViewParams params{};
	unsigned char prefix[ue4::kViewPrefixBytes] = {};
};

// How many candidates reached each stage of one scan. Printed on the first scan and on an
// `absent` verdict, so "no survivor" names the stage that refused rather than costing a round
// trip of guessing.
struct StageCounts
{
	std::uint32_t qwords = 0;      // window / 8
	std::uint32_t pointers = 0;    // pointer-shaped
	std::uint32_t probed = 0;      // pointee probed (bounded by kMaxProbesPerScan)
	std::uint32_t readable = 0;    // pointee readable for the prefix
	std::uint32_t plausible = 0;   // view_params_plausible
	std::uint32_t row135 = 0;      // pre_exposure_plausible
	std::uint32_t fits = 0;        // view_fits_dispatch
	std::uint32_t fraction = 0;    // view_fraction_plausible
	std::uint32_t buffer = 0;      // row 132 == depth extent (or extent unknown)
	std::uint32_t survivors = 0;
	bool truncated = false;        // kMaxProbesPerScan was hit; the window was not fully judged
};

// Runs predictions 1-5 on ONE offset. `out` may be null.
Refusal check_offset(const Reader &r, std::uint64_t view, std::uint32_t offset,
                     const Expectation &exp, Candidate *out);

// Scans every 8-aligned offset in [0, window) and returns the number of survivors, writing at
// most `max_out` of them. Keeps going after the first: knowing there is MORE THAN ONE is the
// point, because the latch refuses ambiguity rather than taking the first.
unsigned scan(const Reader &r, std::uint64_t view, const Expectation &exp,
              Candidate *out, unsigned max_out, StageCounts *counts,
              std::size_t window = kScanWindowBytes);

// ---------------------------------------------------------------------------------------
// Prediction 6: the bytes against what the search read
// ---------------------------------------------------------------------------------------

// Which fields two decodes of the same view DIFFER in. Only the fields a temporal consumer
// receives are named; `any` is the whole-prefix verdict.
struct FieldDiff
{
	bool any = false;              // memcmp over the prefix found a difference
	std::uint32_t first_row = 0;   // the float4 row of the first differing byte
	bool clip_to_prev_clip = false;
	bool jitter = false;           // TemporalAAJitter (126) or TemporalAAParams (152)
	bool camera_cut = false;
	bool pre_exposure = false;
	bool rect = false;             // ViewRectMin / ViewSizeAndInvSize / BufferSizeAndInvSize
};
FieldDiff compare_prefix(const unsigned char *engine, const unsigned char *search,
                         std::size_t bytes = ue4::kViewPrefixBytes);
// "ClipToPrevClip jitter CameraCut" etc., for one log line. Returns bytes written.
int describe_diff(const FieldDiff &d, char *buffer, std::size_t size);

// ---------------------------------------------------------------------------------------
// The latch
// ---------------------------------------------------------------------------------------

enum class LatchState : std::uint8_t
{
	searching = 0, // no offset has proven itself by byte-equality often enough yet
	latched,       // one offset, kLatchAgreements byte-exact agreements running
	// BOTH OF THESE ARE TERMINAL FOR THE SESSION, IN THIS STATE MACHINE - not by fiat at the
	// call site. A caller that stops scanning on a terminal verdict must be modelling a latch
	// that would not re-arm either, or the tested model is more permissive than the shipped one.
	ambiguous,     // more than one candidate survived on one announcement - REFUSED
	absent,        // no candidate survived for long enough that the field is not in the window
};
const char *latch_state_name(LatchState s);

// One announcement's scan result, joined at CLAIM with the search's verdict for the dispatch
// that claimed it. The scan happens on the render thread and the comparison on the recording
// thread, so the observation is assembled by the caller and fed here in one call.
struct Observation
{
	unsigned survivors = 0;     // at announce: how many offsets passed predictions 1-5
	std::uint32_t offset = 0;   // the sole survivor's offset (meaningful when survivors == 1)
	bool compared = false;      // the search found a View for this dispatch, so bytes were compared
	bool bytes_equal = false;   // prediction 6
};

// A wrong offset must NEVER latch, and a rare stale-search frame must only DELAY the latch: so
// before the latch a byte disagreement resets the run, and after it a disagreement is counted
// as the search's error - which, once the offset is proven, is the only reading left.
constexpr unsigned kLatchAgreements = 8;
// After this many CONSECUTIVE announcements with no survivor while still searching, the field
// is structurally not in the window (or every reader refuses), and the verdict is `absent`.
constexpr std::uint64_t kAbsentAfter = 240;

class Latch
{
public:
	void observe(const Observation &o);

	LatchState state() const { return state_; }
	std::uint32_t offset() const { return offset_; }   // valid only in `latched`
	unsigned agreements() const { return agreements_; }
	std::uint64_t observations() const { return observations_; }
	// Counted forever, whichever state they happened in.
	std::uint64_t ambiguous_observations() const { return ambiguous_; }
	std::uint64_t empty_observations() const { return empty_; }
	std::uint64_t uncompared() const { return uncompared_; }
	// Byte-exact agreements and disagreements AFTER the latch: `disagree` is the number the
	// whole mechanism exists to move out of `ambClaimed` - the search reading a stale copy.
	std::uint64_t agree() const { return agree_; }
	std::uint64_t disagree() const { return disagree_; }
	// Disagreements BEFORE the latch. A few are the stale search delaying the latch; a stream
	// with no latch ever forming is the struct not being the bound buffer on this executable.
	std::uint64_t pre_latch_disagreements() const { return pre_disagree_; }
	// A latched offset that did not verify on some announcement (the pointee refused): the
	// announcement carried nothing and the search supplied that frame. Counted, never a demotion.
	std::uint64_t unverified() const { return unverified_; }

private:
	LatchState state_ = LatchState::searching;
	std::uint32_t offset_ = 0;
	unsigned agreements_ = 0;
	std::uint64_t consecutive_empty_ = 0;
	std::uint64_t observations_ = 0;
	std::uint64_t ambiguous_ = 0;
	std::uint64_t empty_ = 0;
	std::uint64_t uncompared_ = 0;
	std::uint64_t agree_ = 0;
	std::uint64_t disagree_ = 0;
	std::uint64_t pre_disagree_ = 0;
	std::uint64_t unverified_ = 0;
};

// ---------------------------------------------------------------------------------------
// The mode, and the decision
// ---------------------------------------------------------------------------------------

// [STRAYDLSS] EngineSeamViewParams. Same ladder shape as EngineSeam, for the same reason: a
// level that cannot change a pixel comes before a level that can.
enum class Mode : std::uint8_t
{
	off = 0,       // nothing scanned, nothing read
	discover,      // scan, validate at claim, latch, log. The search still supplies the View
	authoritative, // once latched, the struct supplies the View; the search is the assertion
	exclusive,     // DECLARED, NOT BUILT: the search is deleted. A separate decision after 2
};
Mode mode_from_level(int level);
const char *mode_name(Mode m);
bool mode_is_implemented(Mode m);

// THE SHIPPED DEFAULT, pinned here rather than left as a literal at the config call site, so
// that moving the ladder's default rung is a deliberate edit against a test and not a one-
// character change nobody reviews (the same discipline `tests/test_nr_history_plan.cpp` applies
// to `NgxNRRestoreHistory`'s default OFF).
//
// **2 (authoritative), since 2026-09-04.** Level 1 ran on the box (facts §36.22) and answered
// every question it was built to answer: exactly one candidate offset, byte-IDENTICAL to the
// bound buffer on the first comparison, latched after 8 claimed announcements, `faults=0`, and
// `disagree=4` matching `ambClaimed=4` event for event with every assertion WARN naming jitter /
// PreExposure / ClipToPrevClip - facts §36.20's stale-ring shape exactly. Level 2 is what turns
// that measurement into a fix: the struct supplies the View, the search becomes the assertion.
//
// It is a SAFE default in the strong sense - not "we think it will work" but "it cannot act
// before it has proven itself". `use_engine_view` requires the latch, and the latch requires 8
// byte-exact agreements with a buffer the engine bound; on an executable where the offset is
// wrong, or moved, or where any reader refuses, no latch forms, nothing is substituted, and the
// search supplies the View exactly as at level 0. A fault at any point disables the mechanism
// for the session at ERROR. So the worst case of shipping 2 rather than 1 is the behaviour of 1.
constexpr int kDefaultLevel = 2;

// Whether the struct's View is used for THIS dispatch. Pure, so the gate is tested rather than
// asserted: below `authoritative`, or before the latch, or when the announcement carried
// nothing, or when what it carried came from an offset other than the latched one, the answer
// is the search's - counted by the caller as a named fallback, never silent.
struct DecisionInputs
{
	Mode mode = Mode::off;
	LatchState latch = LatchState::searching;
	std::uint32_t latched_offset = 0;
	bool carried = false;         // the announcement carries a decoded View
	std::uint32_t carried_offset = 0;
};
bool use_engine_view(const DecisionInputs &in);

} // namespace stray_dlss::viewcached
