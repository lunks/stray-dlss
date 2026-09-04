// The engine's own upscaler seam: finding UE 4.27's ITemporalUpscaler in the shipping
// executable, and correlating what it announces against the dispatches we intercept.
//
// WHY THIS EXISTS. Every part of src/core/taa_signature.hpp is a *behavioural* identification
// of Stray's TAA pass — a DXBC hash, a depth+stencil-SRV-over-one-resource signature, dispatch
// rect arithmetic, an aspect-ratio gate. All of it exists because the project began as a
// ReShade add-on that could see nothing but D3D12 descriptors. As a UE4SS plugin we run inside
// the engine's own address space, and the engine has a first-class extension point for exactly
// this: `ITemporalUpscaler::AddPasses` is the ONE call site of the primary temporal upscale
// (PostProcessing.cpp:559 desktop, :2005 mobile), and every documented look-alike reaches
// FTAAStandaloneCS by a DIFFERENT route that never touches the interface — DiaphragmDOF,
// LightShaftRendering, IndirectLightRendering, SingleLayerWaterRendering,
// FPostProcessing::ProcessPlanarReflection and the DVSM_RayTracingDebug view all call
// `AddTemporalAAPass` directly. So being called through the interface *is* the identification,
// with no archaeology at all.
//
// Everything in this file is pure: bytes in, verdict out. No Windows, no D3D12, no engine
// headers. src/engine_seam_hook.cpp is the live half that maps the module and installs the hook.
//
// PROVENANCE (CLAUDE.md §0.5). Every offset and constant below is HARD, read from the UE 4.27.2
// source in the AlexMercer-MA/UnrealEngine-4.27 mirror whose Build.version reads
// 4.27.2 / ++UE4+Release-4.27. The code SHAPES this file scans for were UNCONFIRMED until
// 2026-09-03, when the scan ran on the box against Stray-Win64-Shipping.exe and found exactly
// one candidate at every stage, validated by all three constants (facts §36). They are HARD on
// that executable now; a game update recooking the binary is what would make them a question
// again, and discovery still refuses loudly and dumps what it did not understand.
#pragma once

#include <cstddef>
#include <cstdint>

namespace stray_dlss::seam {

// ---------------------------------------------------------------------------------------
// Layout, all HARD from UE 4.27.2
// ---------------------------------------------------------------------------------------

// ITemporalUpscaler's vtable, in declaration order — which for MSVC is slot order.
// TemporalAA.h:160-172 declares, in this order: ~ITemporalUpscaler, GetDebugName, AddPasses,
// GetMinUpsampleResolutionFraction, GetMaxUpsampleResolutionFraction. There is no other
// virtual and no virtual base, so a derived class that overrides all four keeps the layout.
// FDefaultTemporalUpscaler (TemporalAA.cpp:1523-1574) declares no destructor of its own and
// no data members, so slot 0 is the compiler-generated deleting destructor.
constexpr unsigned kSlotDestructor = 0;
constexpr unsigned kSlotGetDebugName = 1;
constexpr unsigned kSlotAddPasses = 2;
constexpr unsigned kSlotGetMinFraction = 3;
constexpr unsigned kSlotGetMaxFraction = 4;
constexpr unsigned kVtableSlots = 5;

// The literal FDefaultTemporalUpscaler::GetDebugName returns (TemporalAA.cpp:1527-1530). It is
// a `TEXT()` wide literal, so it lives in the image as UTF-16LE. It cannot be stripped: the
// function is reachable only through the vtable, and the vtable of an instantiated class is
// emitted whole.
constexpr char kDefaultUpscalerName[] = "FDefaultTemporalUpscaler";

// What FDefaultTemporalUpscaler's two fraction accessors return — a compile-time constant each,
// FSceneViewScreenPercentageConfig::kMin/kMaxTAAUpsampleResolutionFraction, SceneView.h:1438-1439.
// These are the self-check: a vtable found by any other means must reproduce BOTH exactly, and
// both are exactly representable in binary32 so the comparison can be ==.
constexpr float kMinTaaUpsampleResolutionFraction = 0.5f;
constexpr float kMaxTaaUpsampleResolutionFraction = 2.0f;

// ITemporalUpscaler::FPassInputs (TemporalAA.h:150-157) under the MSVC x64 ABI:
//   bool         bAllowDownsampleSceneColor;   // 1 byte at 0, 3 bytes of padding
//   EPixelFormat DownsampleOverrideFormat;     // unscoped enum, int underlying -> 4 at 4
//   FRDGTextureRef SceneColorTexture;          // pointer, 8-aligned -> 8
//   FRDGTextureRef SceneDepthTexture;          //                    -> 16
//   FRDGTextureRef SceneVelocityTexture;       //                    -> 24
// The three pointers are read for IDENTITY only. NEVER dereference one: FRDGTexture is an
// RDG-internal type whose layout this project has not established, and at the moment AddPasses
// runs the graph has not executed, so a transient texture has no RHI resource yet.
constexpr std::size_t kPassInputsAllowDownsample = 0;
constexpr std::size_t kPassInputsDownsampleFormat = 4;
constexpr std::size_t kPassInputsSceneColor = 8;
constexpr std::size_t kPassInputsSceneDepth = 16;
constexpr std::size_t kPassInputsSceneVelocity = 24;
constexpr std::size_t kPassInputsSize = 32;

// FIntRect = { FIntPoint Min, Max }; FIntPoint = { int32 X, Y }. No bases, no virtuals.
constexpr std::size_t kIntRectMinX = 0;
constexpr std::size_t kIntRectMinY = 4;
constexpr std::size_t kIntRectMaxX = 8;
constexpr std::size_t kIntRectMaxY = 12;
constexpr std::size_t kIntRectSize = 16;

// GTemporalAATileSizeX. The main pass dispatches
// GetGroupCount(PracticableDestRect.Size(), GTemporalAATileSizeX) (TemporalAA.cpp:958), and
// PracticableDestRect is DivideAndRoundUp(Inputs.OutputViewRect, ResolutionDivisor)
// (TemporalAA.cpp:654-656) with ResolutionDivisor == 1 for every Main* config. So the group
// count is ceil(the announced rect / 8) exactly.
constexpr std::uint32_t kTaaTileSize = 8;

// ---------------------------------------------------------------------------------------
// L1: FRDGTexture -> ID3D12Resource, all [derived] and UNCONFIRMED until the box
// ---------------------------------------------------------------------------------------
//
// The chain, HARD from UE 4.27.2 source (docs/RESEARCH-ENGINE-TAA-HOOK.md §4.2):
//   FRDGTextureRef == FRDGTexture*                         (RenderGraphDefinitions.h:555)
//   FRDGResource: vptr@0, `const TCHAR* Name`@8, `FRHIResource* ResourceRHI`@16
//                                              (RenderGraphResources.h:121, Shipping RDG_ENABLE_DEBUG=0)
//   FRHITexture::GetNativeResource() virtual returning ID3D12Resource*  (RHIResources.h:997)
//   D3D12: TD3D12Texture2D::GetNativeResource -> FD3D12Resource::GetResource -> ID3D12Resource*
//
// ResourceRHI is null at AddPasses time for a graph-allocated texture (assigned in
// FRDGBuilder::Execute's CollectPassResources loop); it is non-null at DISPATCH time, which is
// when the seam claims. So the resolve happens at claim time, never at the AddPasses thunk.
// NOT A LAYOUT GUESS - it is `GetRHI()`, spelled as an offset. UE 4.27's accessor is
// `FRHIResource* GetRHI() const { ValidateRHIAccess(); return ResourceRHI; }`, inline in
// RenderGraphResources.h, and in Shipping (RDG_ENABLE_DEBUG == 0) ValidateRHIAccess() is a
// no-op — so it compiles to this single load and there is no out-of-line symbol to call
// instead. NVIDIA's own DLSS plugin reads the same field the same way from inside an RDG pass
// lambda; we read it from inside AddPasses because we cannot author a pass (report §14).
constexpr std::size_t kRdgResourceRhiOffset = 16;

// FRDGResource::Name — `const TCHAR* const Name` at +8, UNCONDITIONAL in Shipping (it is not
// under RDG_ENABLE_DEBUG; RenderGraphResources.h:61 @ 4.27). Read at the seam from the OUTPUT
// texture the engine hands back in *OutSceneColorTexture, it must be the pool name
// TemporalAA.cpp:554-562 gives every Main* config: L"TemporalAA". One guarded pointer read
// that validates the RDG layout L1 relies on (`Name` is the qword BEFORE `ResourceRHI`), for
// free, on every announcement. docs/RESEARCH-U0-EXTERNAL-PRIOR-ART.md §2.4.
constexpr std::size_t kRdgResourceNameOffset = 8;
constexpr char kTaaOutputName[] = "TemporalAA";

// FRHITexture's vtable slot for GetNativeResource in a Shipping build (ENABLE_RHI_VALIDATION=0,
// so one vptr). FRHIResource declares one virtual (its dtor); FRHITexture then declares
// GetTexture2D, GetTexture2DArray, GetTexture3D, GetTextureCube, GetTextureReference,
// GetSizeXYZ, GetNativeResource -> slot 7. [derived] from the MSVC ABI, not measured.
constexpr unsigned kRhiGetNativeResourceSlot = 7;

// A guarded memory reader the live half supplies; the tests supply an in-memory fake. `read`
// returns false when the address is not safely readable, so the pure resolver never assumes a
// dereference succeeded. `is_code` is true when the address is inside an executable section of
// a loaded module — the guard that stops us calling a garbage `GetNativeResource` pointer.
struct RdgReader
{
	bool (*read_u64)(void *ctx, std::uint64_t va, std::uint64_t *out) = nullptr;
	bool (*is_code)(void *ctx, std::uint64_t va) = nullptr;
	void *ctx = nullptr;
};

enum class RhiChain : std::uint8_t
{
	ok = 0,
	null_rdg,      // the FRDGTexture pointer itself was 0 (the engine passed no texture)
	rhi_null,      // ResourceRHI is 0 — the texture is transient and not yet allocated
	rhi_unreadable,// ResourceRHI or its vtable could not be read
	fn_not_code,   // the resolved GetNativeResource slot does not point into code
	count
};
const char *rhi_chain_name(RhiChain c);

// Walks FRDGTexture -> FRHITexture -> the GetNativeResource function pointer, WITHOUT calling
// it (the call is the live half's job — a pure function cannot invoke an engine method). Every
// dereference goes through `r.read_u64`, so a bad pointer is a status, never a fault. On `ok`,
// `out_rhi` is the FRHITexture* and `out_fn` is the address to call with it.
RhiChain resolve_rhi_fn(const RdgReader &r, std::uint64_t rdg,
                        std::uint64_t *out_rhi, std::uint64_t *out_fn);

// Why an engine-announced frame did not reach a DLSS evaluate. Continuous counters (not
// once-per-pass): a rate that climbs is the "DLSS flip" the user sees, and the breakdown says
// which gate to fix. `no_dispatch` is the ledger's `unclaimed` (the matcher rejected the real
// dispatch, so it never claimed); the rest are a claimed engine dispatch failing downstream.
enum class SeamRefusal : std::uint8_t
{
	none = 0,       // SR evaluated — the good outcome
	view_unreadable,// claimed, but no bound constant buffer decoded as a plausible View
	dead_inputs,    // claimed, but depth/velocity not live AND the engine inputs did not resolve
	role_unresolved,// claimed, live, but colour or output could not be identified
	// Claimed, but the render rect we read is not a shape DLSS can upscale to the engine's
	// announced output rect (`primary_view_shape_ok`). The engine's word covers the DISPATCH
	// and the OUTPUT rect; the render rect still comes from the slot-order View CB search, so
	// this counts how often that search hands us another view's size. It was 37 spurious
	// feature creations per session before it was a refusal at all.
	bad_render_rect,
	mv_failed,      // claimed, the motion-vector resolve did not record
	create_failed,  // claimed, NGX ensure_feature failed
	eval_failed,    // claimed, EvaluateFeature returned failure
	count
};
const char *seam_refusal_name(SeamRefusal r);

// ---------------------------------------------------------------------------------------
// Mode, and the gate decision
// ---------------------------------------------------------------------------------------

// [STRAYDLSS] EngineSeam. Each level includes the ones below it.
enum class Mode : std::uint8_t
{
	off = 0,       // nothing scanned, nothing patched
	discover,      // scan + validate + log; install nothing
	observe,       // stand in for AddPasses and cross-check the heuristic; the heuristic gates
	authoritative, // the engine's announcement gates DLSS; hash and signature are assertions
};
Mode mode_from_level(int level);
const char *mode_name(Mode m);

// What decides whether DLSS may run on a dispatch this frame.
enum class Gate : std::uint8_t
{
	heuristic,           // the pre-seam path: cooked-hash whitelist + structural signature + pin
	engine,              // the engine announced this dispatch; run on its word
	refuse_not_announced,// authoritative, seam live, and the engine announced nothing this fits
	refuse_no_seam,      // authoritative, the seam is NOT live, and the fallback is disallowed
};
const char *gate_name(Gate g);

struct GateInputs
{
	Mode mode = Mode::off;
	bool hooked = false;           // the AddPasses stand-in is installed
	bool fallback_allowed = true;  // [STRAYDLSS] EngineSeamFallback
	bool announced = false;        // this dispatch claimed an announcement
};
// Pure. The heuristic is the answer in every mode below `authoritative`; in `authoritative`
// the seam is the answer, and a missing seam is either the heuristic (fallback allowed, said
// loudly by the caller) or a refusal (fallback disallowed). Never a silent downgrade.
Gate decide(const GateInputs &in);

// THE PRE-RESOLVE GATE. Asked BEFORE the descriptor resolve, for every size-gated dispatch,
// with only what the ledger already knows. Under the authoritative gate a dispatch whose group
// counts fit NO pending announcement cannot be claimed whatever the resolve finds - the
// resolve (the table walk over the descriptor shadow), the View-CB search (a 2448-byte read per
// bound root CBV) and the structural matcher would all run only to be told `not-announced`.
// Measured: ~7 size-gated dispatches a frame reach the resolve and ONE of them is the primary
// upscale (the `resolve 0.539ms (7.0)` line, docs/RESEARCH-RESHADE-SHAPE-SWEEP.md 1.2; the
// two known look-alikes ask every frame, facts 36.3).
//
// `skip` is allowed on exactly one combination, and each clause protects a counter:
//   * authoritative AND hooked - below that the heuristic gates and must see every dispatch;
//   * at least one announcement PENDING - so an ORPHAN (a candidate arriving with nothing
//     announced, the wrong-pass class) still runs the full path and is still counted;
//   * NONE of the pending announcements expects these group counts - so a NEAR MISS (the real
//     pass, refused by the matcher) still runs the full path and is still counted.
// Everything else runs. `claim`, `unclaimed`, `orphans` and `nearMiss` therefore keep their
// exact meaning; only `lookalikesRefused` shrinks, and what it no longer counts is counted as
// `preSkipped` by the caller.
enum class PreGate : std::uint8_t { run, skip };
struct PreGateInputs
{
	Mode mode = Mode::off;
	bool hooked = false;
	bool pending = false; // Ledger::pending() != 0
	bool expects = false; // Ledger::expects(group_x, group_y)
};
PreGate pre_gate_decide(const PreGateInputs &in);

// ---------------------------------------------------------------------------------------
// The module image
// ---------------------------------------------------------------------------------------

// One mapped section of the executable, with the address it is mapped at. `bytes` is the live
// mapping in the discovery build and a synthetic buffer in the tests — the scan cannot tell
// the difference, which is the point.
struct Region
{
	std::uint64_t va = 0;
	const unsigned char *bytes = nullptr;
	std::size_t size = 0;
	bool executable = false;
	const char *name = "";
};

struct Image
{
	const Region *regions = nullptr;
	std::size_t count = 0;

	// Reads `n` bytes at `va`, from whichever region covers the whole range. False if none
	// does — a pointer into unmapped memory is a failed discovery, never a fault.
	bool read(std::uint64_t va, void *out, std::size_t n) const;
	// True when `va` lands inside a region marked executable. Used to reject a run of qwords
	// that looks like a vtable but does not point at code.
	bool is_code(std::uint64_t va) const;
};

// ---------------------------------------------------------------------------------------
// Discovery
// ---------------------------------------------------------------------------------------

enum class SeamStatus : std::uint8_t
{
	ok = 0,
	no_regions,          // the caller handed us nothing to scan
	name_not_found,      // no UTF-16LE "FDefaultTemporalUpscaler" in the image
	debug_name_not_found,// nothing in code returns that literal in the shape we recognise
	vtable_not_found,    // no plausible 5-slot run of code pointers holds that function
	fraction_shape,      // slot 3 or 4 is not a shape we can decode without executing it
	fraction_mismatch,   // decoded, and it is not 0.5 / 2.0 — so this is not the class
	count
};
const char *status_text(SeamStatus s);

struct Discovery
{
	SeamStatus status = SeamStatus::no_regions;

	std::uint64_t name_va = 0;          // the UTF-16LE literal
	std::uint32_t name_hits = 0;        // >1 is not fatal; the literal may be pooled twice
	std::uint64_t get_debug_name_va = 0;
	std::uint32_t debug_name_hits = 0;  // >1 means the shape scan is ambiguous — reported
	std::uint64_t vtable_va = 0;        // address of slot 0
	std::uint32_t vtable_hits = 0;      // how many candidate runs passed the full validation
	std::uint64_t slot[kVtableSlots] = {};

	float min_fraction = 0.0f;
	float max_fraction = 0.0f;

	// The bytes at whichever accessor could not be decoded, so an unfamiliar MSVC codegen
	// costs one log line rather than one round trip of guessing.
	std::uint64_t undecoded_va = 0;
	unsigned char undecoded[16] = {};
};

// Scans the image and returns the verdict. Never executes anything it finds, never
// dereferences an address outside the regions it was handed.
Discovery discover(const Image &image);

// Exposed for the tests, and because each is independently useful in a diagnostic.
// Returns the number of hits; writes at most `max_out` of them.
std::size_t find_utf16_literal(const Image &image, const char *ascii,
                               std::uint64_t *out, std::size_t max_out);
// `lea rax, [rip+d]; ret` — 48 8D 05 <int32> C3 — whose rip-relative target is `target_va`.
// This is what MSVC x64 /O2 emits for `return TEXT("...")`.
std::size_t find_lea_ret_to(const Image &image, std::uint64_t target_va,
                            std::uint64_t *out, std::size_t max_out);
// `movabs rax, imm64; ret` — 48 B8 <uint64> C3 — with `target_va` as the immediate. The other
// way x86-64 can materialise an absolute address, tried only when the rip-relative form finds
// nothing. It exists because a missed codegen shape costs a whole round trip to the box, and
// this project's most expensive resource is the round trip (CLAUDE.md §0.1).
std::size_t find_movabs_ret_to(const Image &image, std::uint64_t target_va,
                               std::uint64_t *out, std::size_t max_out);
// Every 8-byte-aligned qword in a non-executable region equal to `value`.
std::size_t find_qword(const Image &image, std::uint64_t value,
                       std::uint64_t *out, std::size_t max_out);
// Decodes a function that does nothing but return a float constant. Recognises the three
// shapes MSVC x64 emits for `return <constant>;`:
//   movss  xmm0, [rip+d] ; ret        F3 0F 10 05 <d32> C3
//   vmovss xmm0, [rip+d] ; ret        C5 FA 10 05 <d32> C3
//   mov eax, imm32 ; movd xmm0, eax ; ret   B8 <imm32> 66 0F 6E C0 C3
// False when the bytes are none of those, so the caller can dump them instead of guessing.
bool read_float_constant_return(const Image &image, std::uint64_t fn_va, float *out);

// ---------------------------------------------------------------------------------------
// The ledger: what the engine announced, against what we intercepted
// ---------------------------------------------------------------------------------------

// One `ITemporalUpscaler::AddPasses` call. The rect is the engine's own OutputViewRect, read
// out of the out-parameter AFTER the forwarded call has written it, so it is measured rather
// than derived.
struct Announcement
{
	std::uint64_t frame = 0;
	std::uint64_t sequence = 0;
	std::uint32_t out_width = 0;
	std::uint32_t out_height = 0;
	// FRDGTexture identities from FPassInputs. Compared, never dereferenced OUTSIDE AddPasses.
	std::uint64_t colour_rdg = 0;
	std::uint64_t depth_rdg = 0;
	std::uint64_t velocity_rdg = 0;
	// The SAME textures, already resolved to ID3D12Resource* ON THE RENDER THREAD INSIDE
	// AddPasses, where the FRDGBuilder is provably alive because we are inside its setup.
	// This is the whole architecture (report §12.9): `FRDGBuilder::Execute()` ends with
	// `Clear()` -> `Allocator.ReleaseAll()`, which frees every FRDGTexture, and it neither
	// flushes nor waits for the RHI thread - while `FRHICommandList` is "definitions for
	// queueing up & executing later" and a pass lambda's DispatchComputeShader does
	// ALLOC_COMMAND rather than calling the RHI. So the D3D12 Dispatch we intercept happens
	// AFTER the arena is freed, on another thread. Resolving there read freed memory by
	// construction; resolving here cannot.
	std::uint64_t colour_res = 0;
	std::uint64_t depth_res = 0;
	std::uint64_t velocity_res = 0;
	RhiChain colour_status = RhiChain::null_rdg;
	RhiChain depth_status = RhiChain::null_rdg;
	RhiChain velocity_status = RhiChain::null_rdg;
	// The OS thread that announced. An FRDGTexture is owned by the FRDGBuilder that made it,
	// and that builder lives on one thread's stack for the length of one Execute — so a claim
	// from another thread is a claim against memory that thread may already have recycled.
	std::uint64_t thread = 0;
	bool consumed = false;
};

// WHETHER AN ANNOUNCEMENT'S FRDGTexture POINTERS MAY STILL BE DEREFERENCED.
//
// This is the guard L1 shipped without, and its absence is what crashed the game
// (docs/RESEARCH-ENGINE-TAA-HOOK.md §12). The ledger deliberately holds an announcement for up
// to `kRetireAfterAnnouncements` newer announcements or `kRetireAfterFrames` presents, because
// CORRELATION wants that slack: a dispatch that arrives late still names the right rect, and
// `unclaimed` stays honest. IDENTITY survives that slack; POINTERS do not. `FRDGTexture` is
// allocated from the frame's `FRDGAllocator`, which the builder resets when Execute finishes,
// so the instant the announcing graph is gone the pointer addresses recycled arena memory —
// where `+16` is no longer `ResourceRHI` but whatever the next allocation put there.
//
// >>> THIS IS NO LONGER A GATE. It is a PIPELINE-DEPTH DIAGNOSTIC. <<<
//
// It was a gate for exactly one build, and the box retired it: `stale=4147` of 4147 claims with
// `resolved=0`, because the steady state IS "not the newest, and the frame has turned over" —
// by one, every frame. UE 4.27 runs RDG setup on the render thread and drains the RHI command
// list on another, one frame behind, so by the time our hook sees a dispatch the render thread
// has already built the next graph (report §12.9).
//
// The right conclusion was NOT a narrower gate. It was that resolving at claim time is
// unsafe *whatever* this returns — `FRDGBuilder::Execute()` frees the arena before the
// commands ever run — so the dereference moved into `AddPasses`, and the announcement now
// carries plain `ID3D12Resource*` that no allocator owns. Nothing about lifetime is decided
// here any more; liveness at claim is the check.
//
// Kept because the number is worth reading: it measures how far the RHI thread lags, and a
// sudden change in it means the engine's threading moved under us.
//
// THREAD IDENTITY IS NOT PART OF THE RULE EITHER, and a version that required it also shipped
// and also made L1 inert (§12.8). Thread identity governs OWNERSHIP, not VALIDITY: memory held
// by a live stack frame is readable from any thread. The two ids below are carried so the
// caller can REPORT the pair and notice if it ever changes; nothing looks at them.
struct Freshness
{
	std::uint64_t announce_sequence = 0; // Announcement::sequence
	std::uint64_t ledger_sequence = 0;   // Ledger::sequence(), i.e. the newest announcement
	std::uint64_t announce_frame = 0;    // Announcement::frame
	std::uint64_t current_frame = 0;     // the ledger's frame at claim time
	std::uint64_t announce_thread = 0;   // Announcement::thread — REPORTED, never tested
	std::uint64_t current_thread = 0;    // the dispatch-recording thread — REPORTED, never tested
};
bool announcement_is_fresh(const Freshness &f);

// Whether L1 may dereference this frame's FPassInputs, and if not, why. Pure so that the
// off-switch is TESTED rather than asserted: `[STRAYDLSS] EngineSeamInputs=0` must leave the
// plugin behaving exactly as it did before L1 existed, and `off` is the only outcome it can
// produce. Every other outcome except `resolve` also falls back to the heuristic — the
// difference is only what the [seam] line counts and what it says once.
enum class L1Gate : std::uint8_t
{
	off = 0,   // EngineSeamInputs=0, not authoritative, seam not live, or nothing announced
	faulted,   // a guarded read or call already faulted; L1 is off for the session
	resolve,   // proceed
};
const char *l1_gate_name(L1Gate g);

struct L1GateInputs
{
	bool inputs_enabled = false; // [STRAYDLSS] EngineSeamInputs
	Mode mode = Mode::off;
	bool hooked = false;
	bool announced = false;      // announce: always true; claim: this dispatch claimed one
	bool faulted = false;        // the session has already seen a guarded read fault
};
// Used at BOTH ends: inside AddPasses to decide whether to resolve at all, and at claim to
// decide whether to hand the resolved resources on. There is deliberately no freshness term —
// see `announcement_is_fresh`, which is now a DIAGNOSTIC rather than a gate.
L1Gate l1_gate(const L1GateInputs &in);

struct LedgerCounters
{
	std::uint64_t announced = 0;
	std::uint64_t claimed = 0;
	// Announced, then retired without any dispatch claiming it. THE error metric: the engine
	// ran its primary upscale and we did not intercept it, so that frame ran the engine's own
	// TAA. A steady non-zero rate is a correlation bug, not the engine skipping its upscale.
	std::uint64_t unclaimed = 0;
	// A candidate dispatch arrived and the engine had announced NOTHING pending. Under the
	// heuristic this was the wrong-pass class; under the authoritative gate it is a refusal.
	std::uint64_t orphans = 0;
	// A candidate dispatch arrived, an announcement was pending, and no rect agreed. MEASURED
	// 2026-09-03: this is the look-alikes — 0xe3ddca4be9830076 at 240x135 groups and
	// 0x42af595f8ff91038 at 120x68 — being asked and correctly refused, once each per frame.
	// It is expected to be non-zero and is NOT an error; `unclaimed` is.
	std::uint64_t rect_mismatch = 0;
	std::uint64_t overflow = 0; // more concurrent announcements than the ring holds
	// A dispatch arrived with EXACTLY the group counts a pending announcement expects, and it
	// never reached claim() because the matcher refused it first. This is the instrument that
	// tells the two causes of `unclaimed` apart: non-zero means the real dispatch IS there and
	// a gate is rejecting it (fixable, and the reason is logged); zero means the engine
	// announced an upscale no dispatch ever followed (not a loss - nothing to intercept).
	std::uint64_t near_misses = 0;
};

// When an unclaimed announcement is retired. Announcement and dispatch both happen on the
// render thread within one frame — RDG setup, then FRDGBuilder::Execute — so the engine's own
// AddPasses count is the clock to key on: once `kRetireAfterAnnouncements` NEWER announcements
// exist, this one's dispatch has been and gone. The present counter is only a backstop for a
// session that stops announcing altogether (a loading screen with no TAA), and it is
// deliberately generous: nothing about correlation depends on how presents relate to frames,
// which is what makes the ledger immune to frame generation presenting twice per game frame.
constexpr std::uint64_t kRetireAfterAnnouncements = 4;
constexpr std::uint64_t kRetireAfterFrames = 8;

class Ledger
{
public:
	static constexpr std::size_t kCapacity = 8;

	// The present backstop. Call once per GAME present (the present owner delivers exactly
	// that: our own frame-generation presents are skipped under OwnCodeScope).
	void begin_frame(std::uint64_t frame);
	void announce(const Announcement &a);
	// Called only for a dispatch the existing matcher already believes is the TAA pass.
	// Returns the announcement it corresponds to, or nullptr — and counts why not.
	const Announcement *claim(std::uint32_t group_x, std::uint32_t group_y);

	std::size_t pending() const;
	const LedgerCounters &counters() const { return m_counters; }
	std::uint64_t sequence() const { return m_sequence; }
	std::uint64_t frame() const { return m_frame; }

	// ceil(extent / kTaaTileSize), the group count UE 4.27 issues for a Main* config.
	static std::uint32_t expected_groups(std::uint32_t extent);

	// Does any UNCONSUMED announcement expect exactly these group counts? Asked for a dispatch
	// the matcher REFUSED, so that "the real pass arrived and a gate rejected it" and "no
	// dispatch ever came" stop being the same number. Counts a near miss when true.
	bool note_unmatched(std::uint32_t group_x, std::uint32_t group_y);
	// The same question with NO side effect: consumes nothing, counts nothing. This is what the
	// pre-resolve gate asks (pre_gate_decide) before any descriptor is looked up.
	bool expects(std::uint32_t group_x, std::uint32_t group_y) const;

private:
	void retire_stale();

	Announcement m_slots[kCapacity]{};
	std::size_t m_count = 0;
	std::uint64_t m_frame = 0;
	std::uint64_t m_sequence = 0;
	LedgerCounters m_counters{};
};

} // namespace stray_dlss::seam
