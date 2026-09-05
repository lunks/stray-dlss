// The engine's own route to the TAA output UAV `u0`: UE 4.27's IRHIComputeContext, on the RHI
// thread, at the moment the pass binds it.
//
// WHY THIS EXISTS. docs/RESEARCH-U0-IDENTITY.md concluded that `u0` has no engine route: its
// `ResourceRHI` is assigned inside `FRDGBuilder::Execute()` (after our AddPasses seam) and the
// `FRDGTexture` wrapper is freed before the D3D12 Dispatch our hook sees. Both ENDS of that
// window were checked. The MIDDLE was not. Between them, on the RHI thread, the pass lambda's
// `SetShaderParameters` binds every resource through per-parameter RHI virtuals, and for a
// `UBMT_RDG_TEXTURE_UAV` that is (all HARD, UE 4.27.2 @ AlexMercer-MA/UnrealEngine-4.27 306a7e9):
//
//   TemporalAA.cpp:950        FComputeShaderUtils::AddPass(..., ComputeShader, PassParameters, GroupCount)
//   RenderGraphUtils.h:305    the pass lambda -> FComputeShaderUtils::Dispatch(...)
//   RenderGraphUtils.h:247    SetComputeShader; SetShaderParameters; DispatchComputeShader; UnsetShaderUAVs
//   ShaderParameterStruct.h:157  RHICmdList.SetUAVParameter(ShadeRHI, BaseIndex, GraphUAV->GetRHI())
//   RHICommandList.h:2390     FORCEINLINE: Bypass() ? GetComputeContext().RHISetUAVParameter(...)
//                                                  : ALLOC_COMMAND(FRHICommandSetUAVParameter<FRHIComputeShader>)
//   RHICommandListCommandExecutes.inl:175  Execute -> CmdList.GetComputeContext().RHISetUAVParameter(Shader, UAVIndex, UAV)
//   RHIContext.h:200          virtual void RHISetUAVParameter(FRHIComputeShader*, uint32, FRHIUnorderedAccessView*) = 0
//   D3D12Commands.cpp:680     FD3D12CommandContext::RHISetUAVParameter — the override
//   D3D12RHI.cpp:282-298      single GPU: RHIGetDefaultContext() IS the FD3D12CommandContext (no redirector)
//
// `OutComputeTex[0]` is `u0` (TemporalAA.cpp:217, :921; CLAUDE.md §2.3), so the call with
// UAVIndex == 0 on the bound compute shader, on the thread that then records the D3D12
// Dispatch, names the output the TAA pass is about to write — by the engine, before the
// dispatch, while the object is alive. And `FTAAStandaloneCS` is SHADER_USE_PARAMETER_STRUCT
// (TemporalAA.cpp:149), not a root-parameter struct, so it takes exactly this path.
//
// Everything in this file is pure: bytes in, verdict out. src/u0_rhi_hook.cpp is the live half.
//
// TWO DISCOVERIES, BOTH SELF-VALIDATING, NEITHER DERIVED FROM A LAYOUT GUESS ALONE:
//
//  1. The FD3D12CommandContext vtable. There is no name literal on the context, so the seed is a
//     RETURN ADDRESS: our own ID3D12GraphicsCommandList::Dispatch hook is called from
//     FD3D12CommandContext::RHIDispatchComputeShader (D3D12Commands.cpp:120, `CommandListHandle
//     ->Dispatch(...)`, followed by ConditionalFlushCommandList so it is never a tail call), and
//     the PE's exception directory (.pdata, RUNTIME_FUNCTION entries, present in every x64 image
//     because the unwinder needs them) maps that return address to the function's start. A
//     qword equal to that start in a read-only section is a vtable slot; RHIDispatchComputeShader
//     is IRHIComputeContext's slot 3 (dtor, SetComputeShader, SetComputePipelineState, then it).
//     The candidate is then held to a table of PREDICTIONS about IRHIComputeContext's other
//     slots (kSlotExpectations): every slot in the checked range must be code in the module,
//     and the virtuals whose only implementation on this class is an empty body must begin
//     with `ret`. A run of qwords that satisfies the seed at index 3 AND six `ret`s at six
//     predicted indices is not a coincidence.
//
//  2. The FRHIUnorderedAccessView* -> ID3D12Resource* hop. NOT through FD3D12View::Resource
//     (a second base at an unknown offset, then four members, then an FD3D12Resource — three
//     derived constants, the shape docs/RESEARCH-ENGINE-TAA-HOOK.md §15.1 refuses). Instead:
//     TD3D12ViewDescriptorHandle::CreateViewWithCounter calls ID3D12Device::
//     CreateUnorderedAccessView(resource, counter, &desc, Handle) (D3D12View.h:668-679) — a
//     real D3D12 call on the device we already hook, with the ID3D12Resource* and the CPU
//     descriptor handle as explicit arguments, and the descriptor shadow records exactly that
//     pair. So the object handed to RHISetUAVParameter is SCANNED for a qword our shadow knows
//     as a UAV's CPU handle. A hit locates the offset AND yields the resource, by two
//     bookkeepers agreeing on a 64-bit value; zero hits or two hits is a refusal, never a pick.
//
// WINDOWS-PORTABILITY. Nothing here depends on vkd3d-proton, DXVK or Wine: the exception
// directory is PE, the vtable is the game's own .rdata, the descriptor handle is an opaque
// 64-bit key our shadow recorded from a real ID3D12Device call, and the resource registry is
// fed by real ID3D12Device creation hooks. The predictions hold on any x64 MSVC build of the
// engine.
//
// PROVENANCE. The slot map below is [derived] from RHIContext.h:124-330 under the MSVC ABI
// (WITH_MGPU=1 on desktop, MultiGPU.h:14; ENABLE_RHI_VALIDATION=0 in Shipping) — with ONE
// caveat that is the reason two UAV slots are probed rather than one: MSVC lays out
// consecutively-declared overloads in REVERSE declaration order, so the 3-argument
// RHISetUAVParameter (:200) and the 4-argument InitialCount overload (:209) occupy slots 16
// and 17 in an order this file does not assume. Both are hooked with a thunk that forwards all
// five registers/stack args, and the box says which one carries `u0`.
#pragma once

#include "core/engine_seam.hpp"

#include <cstddef>
#include <cstdint>

namespace stray_dlss::u0 {

// ---------------------------------------------------------------------------------------
// The PE exception directory: return address -> function start
// ---------------------------------------------------------------------------------------

// IMAGE_RUNTIME_FUNCTION_ENTRY, x64: three RVAs. Sorted by `begin` in the image.
struct RuntimeFunction
{
	std::uint32_t begin = 0;
	std::uint32_t end = 0;
	std::uint32_t unwind = 0;
};

struct FunctionTable
{
	const RuntimeFunction *entries = nullptr;
	std::size_t count = 0;
	std::uint64_t image_base = 0;
	// For chained unwind info (UNW_FLAG_CHAININFO): the UNWIND_INFO is read through the image
	// so a synthetic table in the tests reads synthetic bytes. May be null (chains then stop).
	const seam::Image *image = nullptr;
};

// The VA of the START of the function containing `va`, following chained entries (MSVC splits
// a function into several RUNTIME_FUNCTIONs for hot/cold and mid-function prolog changes; the
// chained one points at the primary). 0 if `va` is in no entry.
std::uint64_t function_start(const FunctionTable &t, std::uint64_t va);

// ---------------------------------------------------------------------------------------
// IRHIComputeContext's vtable, RHIContext.h:124-330, MSVC declaration order
// ---------------------------------------------------------------------------------------

constexpr unsigned kSlotDestructor = 0;
constexpr unsigned kSlotSetComputeShader = 1;        // RHIContext.h:134
constexpr unsigned kSlotSetComputePipelineState = 2; // :136
constexpr unsigned kSlotDispatchComputeShader = 3;   // :145 — THE SEED
constexpr unsigned kSlotDispatchIndirect = 4;        // :147
constexpr unsigned kSlotSetAsyncComputeBudget = 5;   // :149 `{}`; FD3D12CommandContextBase overrides with `{}` too
constexpr unsigned kSlotBeginTransitions = 6;
constexpr unsigned kSlotEndTransitions = 7;
constexpr unsigned kSlotClearUAVFloat = 8;
constexpr unsigned kSlotClearUAVUint = 9;
constexpr unsigned kSlotBeginUAVOverlap = 10;        // :177 `{}`, not overridden by D3D12
constexpr unsigned kSlotEndUAVOverlap = 11;          // :178 `{}`
constexpr unsigned kSlotBeginUAVOverlapView = 12;    // :180 `{}`
constexpr unsigned kSlotEndUAVOverlapView = 13;      // :181 `{}`
constexpr unsigned kSlotSetShaderTexture = 14;       // :184 (FRHIComputeShader*, uint32, FRHITexture*)
constexpr unsigned kSlotSetShaderSampler = 15;       // :192 (FRHIComputeShader*, uint32, FRHISamplerState*)
constexpr unsigned kSlotSetUAVParameterA = 16;       // :200 / :209 — one of the two overloads
constexpr unsigned kSlotSetUAVParameterB = 17;       // the other; MSVC reverses adjacent overloads
constexpr unsigned kSlotSetShaderResourceView = 18;  // :211 (FRHIComputeShader*, uint32, FRHIShaderResourceView*)
constexpr unsigned kSlotSetShaderUniformBuffer = 19; // :213 (FRHIComputeShader*, uint32, FRHIUniformBuffer*)
constexpr unsigned kSlotSetShaderParameter = 20;     // :215 — SIX arguments; never probed
constexpr unsigned kSlotSetGlobalUniformBuffers = 21;
constexpr unsigned kSlotPushEvent = 22;
constexpr unsigned kSlotPopEvent = 23;
constexpr unsigned kSlotSubmitCommandsHint = 24;
constexpr unsigned kSlotInvalidateCachedState = 25;  // :235 `{}`, not overridden by D3D12
constexpr unsigned kSlotCopyToStagingBuffer = 26;
constexpr unsigned kSlotWriteGPUFence = 27;
constexpr unsigned kSlotSetGPUMask = 28;             // D3D12CommandContext.h:439: `ensure(...)` only
constexpr unsigned kSlotWaitForTemporalEffect = 29;  // WITH_MGPU
constexpr unsigned kSlotBroadcastTemporalEffectA = 30;
constexpr unsigned kSlotBroadcastTemporalEffectB = 31;
constexpr unsigned kSlotTransferTextures = 32;       // :284 `{}`, overridden only by the redirector
constexpr unsigned kSlotBuildASGeometry = 33;        // :290 checkNoEntry() — nothing in Shipping
constexpr unsigned kSlotBuildASs = 34;
constexpr unsigned kSlotBuildASScene = 35;
constexpr unsigned kSlotGetNativeCommandBuffer = 36; // :326 `return nullptr`
constexpr unsigned kSlotPostExternalCommandsReset = 37; // :327 `{}`
constexpr unsigned kSlotsChecked = 38;               // every slot below this must be code

// The window of slots whose signature is (this, FRHIComputeShader*, uint32, T*[, uint32]) —
// safe to forward through ONE thunk shape — and that brackets both UAV overloads with a
// margin of two on each side. Which of them carries `u0` is MEASURED, not assumed.
constexpr unsigned kProbeFirstSlot = kSlotSetShaderTexture;      // 14
constexpr unsigned kProbeLastSlot = kSlotSetShaderUniformBuffer; // 19
constexpr unsigned kProbeCount = kProbeLastSlot - kProbeFirstSlot + 1;

enum class Expect : std::uint8_t
{
	code,   // inside an executable section of the module
	ret,    // an empty body, and nothing else on this class overrides it. TWO encodings: C3, and
	        // MSVC's `ret 0` C2 00 00 — which is the one Stray's exe actually uses (facts §37.2)
	xorret, // 33 C0 C3: `return nullptr`
	seed,   // equals the function the Dispatch hook's return address resolved to
};

struct SlotExpectation
{
	unsigned slot;
	Expect expect;
	// Required: a candidate failing it is refused. Reported: logged pass/fail, never refused
	// on — kept out of the gate because its codegen (`ensure`, `checkNoEntry`, `return
	// nullptr`) is one build flag away from being a real body.
	bool required;
};

// In slot order. Every slot < kSlotsChecked is implicitly required to be `code`.
constexpr SlotExpectation kSlotExpectations[] = {
	{ kSlotDispatchComputeShader, Expect::seed, true },
	{ kSlotSetAsyncComputeBudget, Expect::ret, true },
	{ kSlotBeginUAVOverlap, Expect::ret, true },
	{ kSlotEndUAVOverlap, Expect::ret, true },
	{ kSlotBeginUAVOverlapView, Expect::ret, true },
	{ kSlotEndUAVOverlapView, Expect::ret, true },
	{ kSlotInvalidateCachedState, Expect::ret, true },
	{ kSlotSetGPUMask, Expect::ret, false },
	{ kSlotTransferTextures, Expect::ret, false },
	{ kSlotBuildASGeometry, Expect::ret, false },
	{ kSlotGetNativeCommandBuffer, Expect::xorret, false },
	{ kSlotPostExternalCommandsReset, Expect::ret, false },
};
constexpr std::size_t kSlotExpectationCount = sizeof(kSlotExpectations) / sizeof(kSlotExpectations[0]);

enum class CtxStatus : std::uint8_t
{
	ok = 0,
	no_seed,          // the caller handed no function start
	seed_not_code,    // the seed is not inside an executable section of the image
	no_vtable,        // no read-only qword equals the seed
	ambiguous,        // more than one candidate survived every required prediction
	slot_not_code,    // a slot below kSlotsChecked does not point into code
	prediction_failed,// a required Expect::ret slot is not an empty body (neither C3 nor C2 00 00)
	count
};
const char *ctx_status_text(CtxStatus s);

struct CtxDiscovery
{
	CtxStatus status = CtxStatus::no_seed;
	std::uint64_t seed = 0;
	std::uint64_t vtable_va = 0;             // slot 0's address
	std::uint32_t qword_hits = 0;            // read-only qwords equal to the seed
	std::uint32_t survivors = 0;             // candidates that passed every required prediction
	std::uint64_t slot[kSlotsChecked] = {};
	// For the refused case: which candidate failed what.
	std::uint32_t failed_slot = 0;
	// Reported predictions, bit i = kSlotExpectations[i] held (required ones are all set on ok).
	std::uint32_t expectation_mask = 0;
	// How many of the six required `ret` slots are the SAME address — /OPT:ICF folds identical
	// empty bodies, so 6 means the linker folded them and 1 means it did not. Information only.
	std::uint32_t ret_fold = 0;
};

// Finds the FD3D12CommandContext vtable from the seed (the start of the function that called
// ID3D12GraphicsCommandList::Dispatch). Executes nothing; reads only inside `image`.
CtxDiscovery discover_context_vtable(const seam::Image &image, std::uint64_t seed);

// ---------------------------------------------------------------------------------------
// The GRAPHICS half: IRHICommandContext's own virtuals, RHIContext.h:361-748
// ---------------------------------------------------------------------------------------
//
// `class IRHICommandContext : public IRHIComputeContext` (RHIContext.h:361) — single
// inheritance, so its NEW virtuals are appended to the same vtable after IRHIComputeContext's
// 38, in declaration order. A re-declaration of a base virtual (the dtor at :364,
// RHIDispatchComputeShader at :368, the compute overloads of SetShaderTexture / SetShaderSampler
// / SetUAVParameter / SetShaderResourceViewParameter / SetShaderUniformBuffer / SetShaderParameter
// at :492/:500/:525/:534/:536/:542/:546, RHIBuildAccelerationStructure(s) at :656-670) is an
// OVERRIDE and takes no new slot. `FD3D12CommandContextBase : public IRHICommandContext, public
// FD3D12AdapterChild` (D3D12CommandContext.h:32) and `FD3D12CommandContext :
// FD3D12CommandContextBase, FD3D12DeviceChild` (:69) keep IRHICommandContext as the PRIMARY
// base, so this is the very vtable the compute discovery found — one discovery, two halves.
//
// The two seams this half is built for, all HARD from the source:
//   RHIBeginDrawingViewport / RHIEndDrawingViewport (:419/:422) — `final override` on
//     FD3D12CommandContextBase (D3D12CommandContext.h:38-39), bodies D3D12Viewport.cpp:980/1014.
//     Slate's FSlateRHIRenderer::DrawWindow_RenderThread brackets its whole UI draw with them
//     (SlateRHIRenderer.cpp:845, :1153), and the SCENE never opens one of its own on this path:
//     FSceneViewport::BeginRenderFrame / EndRenderFrame, rendering directly to the window,
//     only fetch and release the back-buffer reference (SceneViewport.cpp:1734-1772). ONE
//     bracket per presented frame, and it is the UI's.
//   RHIBeginRenderPass / RHIEndRenderPass (:584/:586) — virtual on FD3D12CommandContext
//     (D3D12CommandContext.h:322/:336, not final; the redirector's override is not in the path
//     on one GPU). Every RDG raster pass reaches it (RenderGraphBuilder.cpp:1562) and so does
//     Slate (SlateRHIRenderer.cpp:885), each with `const FRHIRenderPassInfo&` whose
//     ColorRenderTargets[0].RenderTarget is the FRHITexture* and .Action the load/store pair
//     (RHIResources.h:2565-2575) — the two facts src/core/frame_seams.hpp decodes.
//
// Predictions in the same discipline as the compute half: every slot up to kGfxSlotsChecked must
// be code, and the virtuals whose only body on FD3D12CommandContext is the base's empty one must
// begin with `ret`. Checked against D3D12CommandContext.h's override list (:257-411): none of
// RHIResummarizeHTile, RHIPollOcclusionQueries, RHIDiscardRenderTargets, the four
// RHIBegin/EndUpdateMultiFrameResource, RHISetStereoViewport, RHIBegin/EndLateLatching or
// RHINextSubpass is overridden. RHISetStencilRef / RHISetBlendFactor ARE (:302-303) though the
// base is `{}`, so they are predicted code, not ret.
//
// ONE OVERLOAD PAIR IS ADJACENT AND REVERSED: RHICalibrateTimers() (:399, `{}`, not overridden)
// and RHICalibrateTimers(FRHITimestampCalibrationQuery*) (:404, overridden at
// D3D12CommandContext.h:288). MSVC lays consecutive overloads out in reverse declaration order,
// so slots 43/44 hold them in an order this file does not assume: the required prediction is
// "exactly one of the two begins with ret", reported per slot.

constexpr unsigned kGfxSlotSetMultipleViewports = 38;           // RHIContext.h:375
constexpr unsigned kGfxSlotCopyToResolveTarget = 39;            // :384
constexpr unsigned kGfxSlotResummarizeHTile = 40;               // :390 `{}`
constexpr unsigned kGfxSlotBeginRenderQuery = 41;               // :395
constexpr unsigned kGfxSlotEndRenderQuery = 42;                 // :397
constexpr unsigned kGfxSlotCalibrateTimersA = 43;               // :399 / :404 — one of the pair
constexpr unsigned kGfxSlotCalibrateTimersB = 44;               // the other (MSVC reverses)
constexpr unsigned kGfxSlotPollOcclusionQueries = 45;           // :410 `{}`
constexpr unsigned kGfxSlotDiscardRenderTargets = 46;           // :416 `{}`
constexpr unsigned kGfxSlotBeginDrawingViewport = 47;           // :419 — A SEAM
constexpr unsigned kGfxSlotEndDrawingViewport = 48;             // :422 — A SEAM
constexpr unsigned kGfxSlotBeginFrame = 49;                     // :425
constexpr unsigned kGfxSlotEndFrame = 50;                       // :428
constexpr unsigned kGfxSlotBeginScene = 51;                     // :436
constexpr unsigned kGfxSlotEndScene = 52;                       // :442
constexpr unsigned kGfxSlotBeginUpdateMultiFrameTex = 53;       // :447 `{}`
constexpr unsigned kGfxSlotEndUpdateMultiFrameTex = 54;         // :452 `{}`
constexpr unsigned kGfxSlotBeginUpdateMultiFrameUav = 55;       // :457 `{}`
constexpr unsigned kGfxSlotEndUpdateMultiFrameUav = 56;         // :462 `{}`
constexpr unsigned kGfxSlotSetStreamSource = 57;                // :467
constexpr unsigned kGfxSlotSetViewport = 58;                    // :473
constexpr unsigned kGfxSlotSetStereoViewport = 59;              // :475 `{}`
constexpr unsigned kGfxSlotSetScissorRect = 60;                 // :484
constexpr unsigned kGfxSlotSetGraphicsPipelineState = 61;       // :486
constexpr unsigned kGfxSlotSetShaderTextureGfx = 62;            // :489 (the compute one at :492 overrides slot 14)
constexpr unsigned kGfxSlotSetShaderSamplerGfx = 63;            // :508
constexpr unsigned kGfxSlotSetUAVParameterPixel = 64;           // :516
constexpr unsigned kGfxSlotSetShaderResourceViewGfx = 65;       // :538
constexpr unsigned kGfxSlotSetShaderUniformBufferGfx = 66;      // :540
constexpr unsigned kGfxSlotSetShaderParameterGfx = 67;          // :544
constexpr unsigned kGfxSlotSetStencilRef = 68;                  // :548 `{}` in the base, overridden by D3D12 (:302)
constexpr unsigned kGfxSlotSetBlendFactor = 69;                 // :550 `{}` in the base, overridden by D3D12 (:303)
constexpr unsigned kGfxSlotDrawPrimitive = 70;                  // :552
constexpr unsigned kGfxSlotDrawPrimitiveIndirect = 71;          // :554
constexpr unsigned kGfxSlotDrawIndexedIndirect = 72;            // :556
constexpr unsigned kGfxSlotDrawIndexedPrimitive = 73;           // :559
constexpr unsigned kGfxSlotDrawIndexedPrimitiveIndirect = 74;   // :561
constexpr unsigned kGfxSlotSetDepthBounds = 75;                 // :569
constexpr unsigned kGfxSlotSetShadingRate = 76;                 // :571, overridden (D3D12CommandContext.h:312)
constexpr unsigned kGfxSlotSetShadingRateImage = 77;            // :577, overridden (:313)
constexpr unsigned kGfxSlotUpdateTextureReference = 78;         // :582
constexpr unsigned kGfxSlotBeginRenderPass = 79;                // :584 — A SEAM
constexpr unsigned kGfxSlotEndRenderPass = 80;                  // :586 — A SEAM
constexpr unsigned kGfxSlotBeginLateLatching = 81;              // :593 `{}`
constexpr unsigned kGfxSlotEndLateLatching = 82;                // :601 `{}`
constexpr unsigned kGfxSlotNextSubpass = 83;                    // :606 `{}`
constexpr unsigned kGfxSlotCopyTexture = 84;                    // :610, overridden (:283)
constexpr unsigned kGfxSlotCopyBufferRegion = 85;               // :639, overridden (:376)
constexpr unsigned kGfxSlotsChecked = 86;                       // every slot in [kSlotsChecked, this) must be code
// Beyond 85 the declarations are conditional (`#if RHI_RAYTRACING` at :644, two platform
// defines at :732/:739), so nothing past kGfxSlotsChecked is predicted or read.

// In slot order. Every slot in [kSlotsChecked, kGfxSlotsChecked) is implicitly required to be
// code. The CalibrateTimers pair is handled by discover_graphics_half's own rule, not here.
constexpr SlotExpectation kGfxSlotExpectations[] = {
	{ kGfxSlotResummarizeHTile, Expect::ret, true },
	{ kGfxSlotPollOcclusionQueries, Expect::ret, true },
	{ kGfxSlotDiscardRenderTargets, Expect::ret, true },
	{ kGfxSlotBeginUpdateMultiFrameTex, Expect::ret, true },
	{ kGfxSlotEndUpdateMultiFrameTex, Expect::ret, true },
	{ kGfxSlotBeginUpdateMultiFrameUav, Expect::ret, true },
	{ kGfxSlotEndUpdateMultiFrameUav, Expect::ret, true },
	{ kGfxSlotSetStereoViewport, Expect::ret, true },
	{ kGfxSlotBeginLateLatching, Expect::ret, true },
	{ kGfxSlotEndLateLatching, Expect::ret, true },
	{ kGfxSlotNextSubpass, Expect::ret, true },
};
constexpr std::size_t kGfxSlotExpectationCount = sizeof(kGfxSlotExpectations) / sizeof(kGfxSlotExpectations[0]);

enum class GfxStatus : std::uint8_t
{
	ok = 0,
	no_vtable,          // the compute discovery did not succeed, so there is nothing to extend
	slot_not_code,      // a slot in [38, 86) does not point into code
	prediction_failed,  // a required `ret` slot is not an empty body
	calibrate_pair,     // neither or both of slots 43/44 is an empty body
	count
};
const char *gfx_status_text(GfxStatus s);

struct GfxDiscovery
{
	GfxStatus status = GfxStatus::no_vtable;
	std::uint64_t vtable_va = 0;
	std::uint64_t slot[kGfxSlotsChecked] = {}; // the whole table, both halves
	std::uint32_t failed_slot = 0;
	std::uint32_t expectation_mask = 0; // bit i = kGfxSlotExpectations[i] held
	unsigned calibrate_ret_slot = 0;    // which of 43/44 is the `{}` body (0 until ok)
	std::uint32_t ret_fold = 0;         // how many of the 11 required rets share slot 40's address
};

// Extends a successful compute discovery to the graphics half: reads slots [38, 86) of the
// same vtable and holds them to the predictions above. Executes nothing.
GfxDiscovery discover_graphics_half(const seam::Image &image, const CtxDiscovery &ctx);

// ---------------------------------------------------------------------------------------
// The object scan: FRHIUnorderedAccessView* -> a CPU descriptor handle our shadow knows
// ---------------------------------------------------------------------------------------

enum class HandleKind : std::uint8_t
{
	none = 0, // not a recorded handle
	uav,
	srv,
	other,    // cbv / rtv / dsv — recorded, but not what a compute bind slot carries
};

// What the live half knows about a 64-bit value: is it a CPU descriptor handle the shadow has
// recorded, of what kind, over which resource, and has that resource died since.
struct HandleLookup
{
	bool (*probe)(void *ctx, std::uint64_t value, HandleKind *kind, std::uint64_t *resource,
	              bool *dead) = nullptr;
	void *ctx = nullptr;
};

struct ObjectReader
{
	bool (*read_u64)(void *ctx, std::uint64_t va, std::uint64_t *out) = nullptr;
	void *ctx = nullptr;
};

// Qword 0 is the vptr; the handle sits inside FD3D12View's first member, which is a few qwords
// past the FRHIUnorderedAccessView base (24 bytes: vptr, NumRefs, MarkedForDelete, two bools).
// 40 qwords is 320 bytes, past the whole FD3D12View<UAV desc> (~120 bytes) with room to spare.
constexpr unsigned kScanFirstQword = 1;
constexpr unsigned kScanQwords = 40;

enum class ScanStatus : std::uint8_t
{
	ok = 0,
	no_hit,     // no qword in the window is a recorded handle
	ambiguous,  // two or more are — refused, never picked
	dead,       // one hit, and its resource has died (the shadow's tombstone)
	unreadable, // the object itself could not be read
	count
};
const char *scan_status_text(ScanStatus s);

struct ScanResult
{
	ScanStatus status = ScanStatus::unreadable;
	unsigned offset = 0;     // bytes from the object base to the handle qword
	HandleKind kind = HandleKind::none;
	std::uint64_t handle = 0;
	std::uint64_t resource = 0;
	unsigned hits = 0;       // recorded handles found in the window (any kind)
};

// Scans `object + 8*[kScanFirstQword, kScanQwords)` through `r` and `l`. Only `uav` and `srv`
// hits count (an FRHITexture may legitimately hold an SRV handle; a sampler holds nothing).
ScanResult scan_object_for_handle(const ObjectReader &r, const HandleLookup &l,
                                  std::uint64_t object);

// Reads ONLY the latched offset — the per-call path once the scan has settled. `ok` requires
// the qword there to be a recorded handle of `want`.
ScanResult read_handle_at(const ObjectReader &r, const HandleLookup &l, std::uint64_t object,
                          unsigned offset, HandleKind want);

// The offset is latched after `kLatchAgreements` consecutive scans agree on it; a scan that
// disagrees resets the run. Pure so the rule is testable.
constexpr unsigned kLatchAgreements = 3;
class OffsetLatch
{
public:
	// Returns true the moment the latch closes.
	bool observe(unsigned offset);
	bool latched() const { return m_latched; }
	unsigned offset() const { return m_offset; }
	unsigned run() const { return m_run; }
	void reset();

private:
	bool m_latched = false;
	unsigned m_offset = 0;
	unsigned m_run = 0;
};

// ---------------------------------------------------------------------------------------
// What each probed slot turned out to carry — the vtable index MEASURED rather than counted
// ---------------------------------------------------------------------------------------

enum class SlotRole : std::uint8_t
{
	unknown = 0, // not enough classified calls yet
	uav,         // the objects it is handed hold a UAV handle: an RHISetUAVParameter overload
	srv,         // ...an SRV handle: RHISetShaderResourceViewParameter (or a texture's SRV)
	silent,      // classified calls saw no recorded handle: samplers, uniform buffers
};
const char *slot_role_name(SlotRole r);

struct SlotCensus
{
	std::uint64_t calls = 0;
	std::uint64_t null_object = 0;   // the object argument was null (UnsetShaderUAVs, etc.)
	std::uint64_t index0 = 0;        // calls with the register index == 0
	std::uint64_t classified = 0;    // non-null objects the scan was run on
	std::uint64_t uav_hits = 0;
	std::uint64_t srv_hits = 0;
	std::uint64_t no_hits = 0;
	std::uint64_t ambiguous = 0;
};
// A slot is classified once `kClassifySamples` scans have run: the majority kind wins, and a
// slot whose scans found nothing is `silent`.
constexpr std::uint64_t kClassifySamples = 16;
SlotRole classify_slot(const SlotCensus &c);

// ---------------------------------------------------------------------------------------
// The assertion: the engine's u0 against the descriptor walk's u0
// ---------------------------------------------------------------------------------------

enum class Verdict : std::uint8_t
{
	agree = 0,      // same ID3D12Resource*
	disagree,       // both resolved, different resources — the finding this exists for
	no_bind,        // the dispatch had no UAVIndex==0 bind on its thread (the hook never saw it)
	unresolved,     // a bind was seen and its object did not resolve (scan refused)
	not_live,       // resolved to a pointer our registry does not call live
	walk_absent,    // the descriptor walk produced no u0 to compare against
	desc_mismatch,  // resolved, live, and not a 2D UAV-capable HDR texture covering the rect
	count
};
const char *verdict_name(Verdict v);

struct DescFacts
{
	bool live = false;
	bool is_buffer = false;
	bool is_3d = false;
	bool allow_uav = false;
	bool hdr_colour = false;
	std::uint32_t width = 0;
	std::uint32_t height = 0;
};

struct Judgement
{
	Verdict verdict = Verdict::no_bind;
	bool extent_equal = false; // the texture is EXACTLY the announced rect, not merely >= it
};

Judgement judge(bool bind_present, ScanStatus chain, std::uint64_t rhi_u0,
                std::uint64_t walk_u0, const DescFacts &d,
                std::uint32_t out_width, std::uint32_t out_height);

// ---------------------------------------------------------------------------------------
// The rest of the bind stream: the SRV registers and the View constant buffer's register
// ---------------------------------------------------------------------------------------
//
// The same bracket - RHISetComputeShader ... RHIDispatchComputeShader - carries every other
// parameter of the pass with its register attached (docs/RESEARCH-ENGINE-AWARE-REPLAN.md §1):
// for FTAAStandaloneCS, t0 EyeAdaptation, t1 colour, t2 depth, t3 velocity and t5 history
// arrive at RHISetShaderTexture as bare FRHITexture* (SHADER_PARAMETER_RDG_TEXTURE,
// TemporalAA.cpp:188-203), t4 stencil at RHISetShaderResourceViewParameter as an
// FRHIShaderResourceView* (:206), and the View uniform buffer at RHISetShaderUniformBuffer
// with its `b` register (:207, SHADER_PARAMETER_STRUCT_REF). That is CLAUDE.md §2.3's register
// map, from the engine. Each is asserted against the descriptor walk's answer for the same
// register; the walk stays authoritative.

constexpr unsigned kMaxTexRegs = 8; // t0..t7 recorded; TAA uses t0..t5
constexpr unsigned kMaxUavRegs = 4; // u0..u3 recorded; TAA uses u0 (u1 in one config)
constexpr unsigned kMaxCbRegs = 16; // b0..b15 as a bitmask

enum class RegVerdict : std::uint8_t
{
	agree = 0,
	disagree,
	engine_absent, // the engine bound nothing at this register in the bracket
	walk_absent,   // the descriptor walk resolved nothing at this register
	unresolved,    // the engine bound an object we could not resolve to a live resource
	count
};
const char *reg_verdict_name(RegVerdict v);
RegVerdict judge_register(bool engine_bound, std::uint64_t engine_res, std::uint64_t walk_res);

// The View constant buffer's register. FTAAStandaloneCS references exactly ONE uniform buffer
// struct (ViewUniformBuffer), so a bracket with exactly one RHISetShaderUniformBuffer names
// the View CB's `b` register outright - which is what the slot-order View-CB search has been
// guessing at (CLAUDE.md §2.6). Two or more bound is a bracket this rule does not cover.
enum class ViewRegVerdict : std::uint8_t
{
	agree = 0,
	disagree,
	none_bound,     // no uniform buffer was bound in the bracket
	multiple_bound, // more than one; the rule cannot pick
	walk_absent,    // the walk found no plausible View CB this dispatch
	count
};
const char *view_reg_verdict_name(ViewRegVerdict v);
ViewRegVerdict judge_view_register(std::uint32_t ub_mask, bool walk_valid, unsigned walk_reg);

} // namespace stray_dlss::u0
