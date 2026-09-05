// FIND, DON'T SEARCH: the back buffer's identity, its D3D12 state and the HUD-less seam, all
// taken from the engine's own statements on the RHI thread rather than from models of it.
//
// Pure: bytes in, verdicts out. No Windows, no D3D12, no engine headers. The live halves are
// src/rhi_gfx_hook.cpp (the IRHICommandContext thunks), src/backend_native/backbuffer_state.cpp
// (the ResourceBarrier / ExecuteCommandLists ledger) and src/hudless.cpp (the copy).
//
// THREE MODELS THIS RETIRES TO THE RANK OF ASSERTION, and the engine statement that replaces each:
//
//  1. WHICH replacement back buffer the game rendered this frame. Today core::fg::GameIndexMirror
//     REPRODUCES FD3D12Viewport::CurrentBackBufferIndex_RHIThread (+1 per Present, 0 per Resize);
//     a mirror error is a stale presented frame that only the crop gate would notice. The engine
//     STATES the buffer twice per frame, on the RHI thread:
//       (a) IRHICommandContext::RHIBeginRenderPass(const FRHIRenderPassInfo&, const TCHAR*)
//           (RHIContext.h:584) inside Slate's RHIBeginDrawingViewport..RHIEndDrawingViewport
//           bracket (SlateRHIRenderer.cpp:845-1153) names ColorRenderTargets[0].RenderTarget —
//           the viewport's back-buffer reference texture — and
//           FD3D12TextureBase::GetNativeResource() on THAT object (FRHITexture vtable slot 7,
//           HARD since L1) resolves the reference to the CURRENT back buffer's ID3D12Resource
//           (D3D12Texture.h:312-331: a texture with no resource of its own goes through
//           GetD3D12TextureFromRHITexture, which swaps a TexCreate_Presentable dummy for
//           Viewport->GetBackBuffer_RHIThread(), :566-583 / D3D12Texture.cpp:3235). Under FG that
//           ID3D12Resource is one of OUR replacements (the game fetched them through the hooked
//           GetBuffer).
//       (b) FD3D12Viewport::Present transitions that same resource to D3D12_RESOURCE_STATE_PRESENT
//           and flushes before IDXGISwapChain::Present (D3D12Viewport.cpp:820-829, :868) — the
//           last barrier on a swapchain-class resource before our Present hook names it again.
//     Two engine routes, one identity; the mirror is the third and is now the ASSERTION.
//
//     NOTE ON WHAT THE TASK ASSUMED. RHIBeginDrawingViewport's RenderTargetRHI argument is NULL on
//     every 4.27 caller (UnrealClient.cpp:1418, SlateRHIRenderer.cpp:845) and
//     FDynamicRHI::RHIGetViewportBackBuffer returns the render thread's DUMMY reference texture
//     (D3D12Viewport.cpp:1161-1177), not the buffer — so the identity is NOT in that argument.
//     It is in the render pass's texture, one GetNativeResource hop deeper, and that hop is the
//     engine's own (item a). HARD, read from the source cited.
//
//  2. THE BACK BUFFER'S STATE AT PRESENT. The NR stage ASSUMES D3D12_RESOURCE_STATE_PRESENT
//     ([STRAYDLSS] NgxNRStageBackBufferState). The engine STATES every transition it makes
//     through ID3D12GraphicsCommandList::ResourceBarrier, and the GPU order of those statements
//     is the ExecuteCommandLists order — which matters, because UE 4.27 resolves a resource's
//     FIRST transition on a command list at submission time onto a separate barrier list that
//     executes BEFORE the list (D3D12CommandList.cpp:14, `AddPendingResourceBarrier`;
//     D3D12CommandListHandle::ResolvePendingResourceBarriers), so a ledger that applied barriers
//     in RECORDING order would end a frame believing RENDER_TARGET where the GPU has PRESENT. The
//     StateLedger below therefore queues per list and replays at execute.
//
//  3. THE HUD-LESS FRAME. Slate draws the UMG HUD onto the back buffer in ONE render pass
//     (SlateRHIRenderer.cpp:850 `FRHIRenderPassInfo RPInfo(BackBuffer, ERenderTargetActions::
//     Load_Store)`, :885 `BeginRenderPass(RPInfo, TEXT("SlateBatches"))`), inside the one
//     drawing-viewport bracket of the frame, with a LOAD action because the scene must survive
//     under the UI. The scene's own passes onto the back buffer (the tonemapper's) run BEFORE the
//     bracket — FSceneViewport::BeginRenderFrame, rendering directly to the window, never opens
//     one (SceneViewport.cpp:1734-1747). So "the first LOAD pass on the back buffer INSIDE the
//     bracket" is the HUD pass by the engine's own arguments — not by its name, which is
//     UNCONFIRMED in this exe's .rdata and is deliberately not consulted.
//
// Every offset below is HARD from the cited header under the MSVC x64 ABI; every rule is a pure
// function pinned by tests/test_frame_seams.cpp.
#pragma once

#include "core/u0_rhi_uav.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace stray_dlss::fseam {

// ---------------------------------------------------------------------------------------
// FRHIRenderPassInfo (RHIResources.h:2565-2584), MSVC x64
// ---------------------------------------------------------------------------------------
//
//   struct FColorEntry { FRHITexture* RenderTarget; FRHITexture* ResolveTarget; int32 ArraySlice;
//                        uint8 MipIndex; ERenderTargetActions Action; };   // 8+8+4+1+1 -> 24
//   FColorEntry ColorRenderTargets[MaxSimultaneousRenderTargets];          // 8 entries (RHIDefinitions.h:775)
//   FDepthStencilEntry DepthStencilRenderTarget;                            // at 192
constexpr std::size_t kColorEntrySize = 24;
constexpr std::size_t kColorEntryRenderTarget = 0;
constexpr std::size_t kColorEntryAction = 21;
constexpr unsigned kMaxSimultaneousRenderTargets = 8;
constexpr std::size_t kDepthStencilEntryOffset = kColorEntrySize * kMaxSimultaneousRenderTargets;

// ERenderTargetActions : uint8 (RHIResources.h:2487-2520): (Load << 2) | Store, with
// ERenderTargetLoadAction { ENoAction=0, ELoad=1, EClear=2 } (RHIDefinitions.h:1505-1518) and
// ERenderTargetStoreAction { ENoAction=0, EStore=1, EMultisampleResolve=2 } (:1524-1537).
enum class LoadAction : std::uint8_t
{
	no_action = 0,
	load = 1,
	clear = 2,
	invalid = 3, // 3 is not a value of the enum
};
constexpr unsigned kLoadOpShift = 2; // ERenderTargetActions::LoadOpMask
constexpr LoadAction load_action(std::uint8_t action) { return static_cast<LoadAction>((action >> kLoadOpShift) & 3u); }
constexpr std::uint8_t kActionLoadStore = (1u << kLoadOpShift) | 1u;    // Load_Store   = 5 — Slate's HUD pass
constexpr std::uint8_t kActionClearStore = (2u << kLoadOpShift) | 1u;   // Clear_Store  = 9
constexpr std::uint8_t kActionDontLoadStore = 1u;                        // DontLoad_Store = 1
const char *load_action_name(LoadAction a);

struct Color0
{
	std::uint64_t render_target = 0; // FRHITexture*, identity only until resolved by the live half
	std::uint8_t action = 0;         // ERenderTargetActions
};
// Reads ColorRenderTargets[0].{RenderTarget, Action} of the FRHIRenderPassInfo at `info` through
// the guarded reader. False when either qword could not be read.
bool decode_color0(const u0::ObjectReader &r, std::uint64_t info, Color0 *out);

// ---------------------------------------------------------------------------------------
// The swapchain-class candidate set: what an engine-named back buffer must resolve to
// ---------------------------------------------------------------------------------------
//
// Under FG the game holds our REPLACEMENT buffers (fg_present.hpp); otherwise the real swapchain
// buffers. Either way a small fixed set the live half registers, and an engine answer outside
// it is REFUSED — that membership is the independent prediction every resolve is held to.
constexpr unsigned kMaxCandidates = 16; // DXGI allows 16 buffers; UE4 asks for 3 (facts §32.4)

struct Candidates
{
	std::uint64_t res[kMaxCandidates] = {};
	unsigned count = 0;
	std::uint64_t epoch = 0; // bumped on every re-registration (resize, FG arm/disarm)

	void set(const std::uint64_t *ids, unsigned n, std::uint64_t new_epoch);
	bool contains(std::uint64_t r) const;
	int index_of(std::uint64_t r) const; // -1 when absent
};

// ---------------------------------------------------------------------------------------
// The per-frame render-pass ledger, on the RHI thread
// ---------------------------------------------------------------------------------------

enum class PassClass : std::uint8_t
{
	not_back_buffer = 0,    // RT0 is some other texture (or unresolved / not a candidate)
	bb_outside_bracket,     // a scene pass onto the back buffer, before RHIBeginDrawingViewport
	bb_hud,                 // INSIDE the bracket, LOAD, and the first such this frame: the HUD pass
	bb_in_bracket_later,    // inside the bracket, LOAD, but a HUD pass was already seen this frame
	bb_in_bracket_no_load,  // inside the bracket with Clear / DontLoad — Slate's bClear path, or
	                        // a composition pass; never the copy point
	count
};
const char *pass_class_name(PassClass c);

struct FrameLedger
{
	bool in_bracket = false;          // between RHIBeginDrawingViewport and RHIEndDrawingViewport
	unsigned brackets = 0;            // RHIBeginDrawingViewport calls this frame (predict 1)
	unsigned bb_outside = 0;          // back-buffer passes before the bracket (the scene's)
	unsigned bb_inside = 0;           // back-buffer passes inside it
	bool hud_seen = false;            // the bb_hud pass was classified this frame
	std::uint8_t first_inside_action = 0xFF;
	std::uint64_t frame_res = 0;      // the ID3D12Resource the in-bracket passes resolved to (0 = none)
	std::uint64_t frame_res_outside = 0; // ...and the outside ones (should be the same buffer)

	void begin_bracket();            // RHIBeginDrawingViewport
	void end_bracket();              // RHIEndDrawingViewport
	void reset();                    // after the present that closes the frame
	// Classifies a render pass whose RT0 did / did not resolve to a candidate back buffer, and
	// records it. `resolved` is the ID3D12Resource (0 when not a back buffer).
	PassClass classify(bool is_back_buffer, std::uint64_t resolved, std::uint8_t action);
};

// The per-frame SHAPE the HUD-less copy requires, and what a departure means. Predicted from the
// source (item 3 above): one bracket, at least one back-buffer pass before it, exactly one HUD
// pass inside it. A frame with no HUD pass (no UI batches, SlateRHIRenderer.cpp:883) is
// `no_hud`, and FG/NR simply proceed as today.
enum class FrameShape : std::uint8_t
{
	ok = 0,
	no_bracket,        // no RHIBeginDrawingViewport this frame — the hooks are not seeing the RHI thread
	many_brackets,     // >1: a viewport we do not model is drawing too
	no_scene_pass,     // nothing drew onto the back buffer before the bracket: the scene is NOT
	                   // rendered directly to the window, and the HUD pass would carry the scene too
	no_hud,            // bracket seen, no LOAD pass inside it
	count
};
const char *frame_shape_name(FrameShape s);
FrameShape judge_frame(const FrameLedger &f);

// ---------------------------------------------------------------------------------------
// The back-buffer FRHITexture* latch
// ---------------------------------------------------------------------------------------
//
// The reference texture the engine names is ONE object per viewport, recreated only on a resize
// (WindowsD3D12Viewport.cpp:392), so once three frames' resolves agree the per-pass cost is a
// pointer compare; the resolve is then repeated only for the periodic assertion.
constexpr unsigned kLatchAgreements = 3;
class PtrLatch
{
public:
	bool observe(std::uint64_t value); // true the moment the latch closes
	bool latched() const { return m_latched; }
	std::uint64_t value() const { return m_value; }
	void reset();

private:
	bool m_latched = false;
	std::uint64_t m_value = 0;
	unsigned m_run = 0;
};

// ---------------------------------------------------------------------------------------
// Deliverable 1: the identity verdict per present, and which answer selects the frame
// ---------------------------------------------------------------------------------------

enum class Level : std::uint8_t
{
	off = 0,
	observe,       // 1: hooks installed, everything asserted and counted, nothing selected
	authoritative, // 2: the engine's answer selects; the model is the assertion
};
Level level_from_int(int level);
const char *level_name(Level l);

enum class IdVerdict : std::uint8_t
{
	agree = 0,
	disagree,
	engine_absent,   // no in-bracket back-buffer pass resolved this frame
	not_candidate,   // the engine's resource is not in the candidate set — refused, never used
	other_absent,    // the other side (mirror / barrier) had no answer
	count
};
const char *id_verdict_name(IdVerdict v);

struct IdentityInputs
{
	std::uint64_t engine_res = 0;   // from the render pass (item 1a); 0 = none this frame
	bool engine_is_candidate = false;
	std::uint64_t barrier_res = 0;  // the last candidate transitioned to PRESENT (item 1b); 0 = none
	std::uint64_t mirror_res = 0;   // core::fg::GameIndexMirror's answer; 0 = FG not armed
};
struct IdentityJudgement
{
	IdVerdict vs_mirror = IdVerdict::engine_absent;
	IdVerdict vs_barrier = IdVerdict::engine_absent;
};
IdentityJudgement judge_identity(const IdentityInputs &in);

enum class FrameSource : std::uint8_t
{
	mirror = 0,   // the model chose (level < 2, or the engine had no usable answer)
	engine,       // the engine's render-pass resource chose
};
struct Selection
{
	FrameSource source = FrameSource::mirror;
	std::uint64_t res = 0;
	bool fell_back = false; // level 2 asked the engine and it had nothing usable this frame
};
Selection select_game_frame(Level level, const IdentityInputs &in);

// ---------------------------------------------------------------------------------------
// Deliverable 3: the state ledger — barriers queued per list, replayed at execute
// ---------------------------------------------------------------------------------------

// D3D12_RESOURCE_STATES values this file names (d3d12.h): PRESENT == COMMON == 0.
constexpr std::uint32_t kStatePresent = 0;
constexpr std::uint32_t kStateRenderTarget = 0x4;
constexpr std::uint32_t kStateCopySource = 0x800;
constexpr std::uint32_t kStateCopyDest = 0x400;

struct ExecuteResult
{
	unsigned applied = 0;        // transitions replayed onto tracked resources
	unsigned before_mismatch = 0;// the engine's StateBefore != the state this ledger held (an assertion)
	unsigned markers_ok = 0;     // HUD-less copies whose assumed StateBefore matched the replay
	unsigned markers_bad = 0;    // ...and did not: the copy ran with a wrong barrier
	std::uint64_t present_res = 0; // the last tracked resource this execute left in PRESENT, 0 = none
};

class StateLedger
{
public:
	// The tracked set. Replaces the previous one; every state becomes "unknown" (a swapchain
	// buffer is created in PRESENT, D3D12Viewport.cpp:241/:278, and our replacements in COMMON
	// which is the same value — but "unknown until the engine says" is the honest start).
	void set_tracked(const Candidates &c);
	bool tracked(std::uint64_t res) const { return m_tracked.contains(res); }

	// Recording-thread events, in the order the game issues them.
	void on_barrier(std::uint64_t list, std::uint64_t res, std::uint32_t before, std::uint32_t after);
	// The HUD-less copy was recorded on `list` assuming `res` was in `assumed`; judged at execute.
	void on_marker(std::uint64_t list, std::uint64_t res, std::uint32_t assumed);
	void on_reset(std::uint64_t list); // ID3D12GraphicsCommandList::Reset: the queued sequence is gone
	// ID3D12CommandQueue::ExecuteCommandLists, one list at a time in the order given: replays.
	ExecuteResult on_execute(std::uint64_t list);

	// The state of `res` as of the last executed transition; `known` false until the engine has
	// transitioned it once since set_tracked.
	std::uint32_t state_of(std::uint64_t res, bool *known) const;
	// The most recent tracked resource an execute left in PRESENT (item 1b); 0 = none yet.
	std::uint64_t last_present_resource() const { return m_last_present; }

	std::size_t pending_lists() const { return m_lists.size(); }

private:
	struct Event
	{
		std::uint64_t res = 0;
		std::uint32_t before = 0;
		std::uint32_t after = 0;
		bool marker = false;
	};
	struct ListQueue
	{
		std::uint64_t list = 0;
		std::vector<Event> events;
	};
	ListQueue *find(std::uint64_t list, bool create);

	Candidates m_tracked;
	std::uint32_t m_state[kMaxCandidates] = {};
	bool m_known[kMaxCandidates] = {};
	std::vector<ListQueue> m_lists;
	std::uint64_t m_last_present = 0;
};

enum class StateVerdict : std::uint8_t
{
	agree = 0,      // the ledger's recorded state equals the assumed constant
	disagree,       // it does not — at level 2 the recorded state is what the stage uses
	unknown,        // the engine has not transitioned this resource since it was registered
	not_tracked,    // the back buffer the stage was handed is not a registered candidate
	count
};
const char *state_verdict_name(StateVerdict v);
StateVerdict judge_present_state(bool tracked, bool known, std::uint32_t recorded, std::uint32_t assumed);
// What the NR stage should name as the back buffer's StateBefore: the recorded state at level 2
// when known, the assumed constant otherwise.
std::uint32_t present_state_to_use(Level level, bool tracked, bool known, std::uint32_t recorded, std::uint32_t assumed);

// ---------------------------------------------------------------------------------------
// Deliverable 4: whether THIS render pass is where the HUD-less copy is recorded
// ---------------------------------------------------------------------------------------

enum class CopyRefusal : std::uint8_t
{
	none = 0,        // copy here
	level_off,       // EngineSeamHudless < 2
	not_hud_pass,    // the pass is not bb_hud
	no_latch,        // the back-buffer texture has not been latched (identity still being proven)
	shape_unproven,  // the frame shape has not read `ok` for kShapeAgreements frames running
	count
};
const char *copy_refusal_name(CopyRefusal r);

// The copy is only ever recorded once the frame's shape has held for this many presented frames
// running: one bracket, scene passes before it, one HUD pass inside it (FrameShape::ok).
constexpr unsigned kShapeAgreements = 8;

struct CopyInputs
{
	Level level = Level::off;
	PassClass pass = PassClass::not_back_buffer;
	bool latched = false;
	unsigned shape_ok_streak = 0;
};
CopyRefusal decide_copy(const CopyInputs &in);

} // namespace stray_dlss::fseam
