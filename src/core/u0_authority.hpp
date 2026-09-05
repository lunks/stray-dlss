// [STRAYDLSS] U0Hook level 3: WHICH source names the TAA pass's registers for DLSS SR, WHEN
// the walk is the fallback, and WHEN the descriptor shadow's expensive half may stop being
// recorded at all. Pure: bytes in, verdict out; the live half is src/u0_rhi_hook.cpp.
//
// THE LADDER (docs/RESEARCH-U0-IDENTITY.md §10.4, the seam's own shape):
//   0 off        nothing mapped, nothing scanned
//   1 discover   find and validate the FD3D12CommandContext vtable; install nothing (DEFAULT)
//   2 observe    forwarding thunks; the RHI bind stream is ASSERTED against the descriptor walk
//   3 authoritative  the bind stream SUPPLIES u0 and t0..t5 for the TAA pass; the walk is the
//                fallback for the frame when the bracket has no complete answer, and stays the
//                assertion's other side whenever it still runs
//
// WHAT LEVEL 3 REPLACES, AND WHAT IT DOES NOT. The descriptor-table walk over the descriptor
// shadow (NativeBackend::resolve_compute_bindings) produces `DispatchBindings::srvs` and
// `::uavs` — the register map the matcher, the colour pick (t1, CLAUDE.md §2.3), the output UAV
// (u0), the eye-adaptation SRV (t0) and the history round-trip (t5) all read. The bracket the
// RHI thunks record carries the SAME registers with the engine's own objects
// (CLAUDE.md §2.9), so at level 3 those two vectors are FILLED FROM THE BRACKET on a dispatch a
// pending announcement expects. Nothing else moves: the View constant buffer comes from the
// ROOT CBVs (`constant_buffers`, registry::buffer_for_va, no shadow involved) and is owned by
// EngineSeamViewParams; depth and velocity are already the engine's through L1; the root
// shadow the state restore needs is untouched.
//
// THE COMPLETENESS RULE IS STRICT ON PURPOSE. Every FTAAStandaloneCS permutation the game
// ships declares t0..t5 and u0 (CLAUDE.md §5, "DLAA (1:1) is NOT blocked by the shader": 27
// permutations, two edge variants with fewer SRVs), and UE 4.27 binds every parameter the
// shader declares (ShaderParameterStruct.h:185-283). So a bracket missing any of the seven is
// not this pass, or not this build's pass, and the answer is the WALK for that frame — counted
// by register and by reason, never silent (CLAUDE.md §0.2). A partial substitution would hand
// the matcher a signature that fails "no depth+stencil SRV pair" and cost the frame with the
// blame on the wrong component.
//
// WHY SKIPPING THE WALK IS A SECOND KEY AND ONE-WAY. `U0HookSkipWalk=1` stops recording the
// shadow's COPY half (CopyDescriptors(Simple) -> note_copy_range, the measured `shadow-copy
// 1.644ms`) and the table half of the resolve (`walk` + `slots`), once level 3 has answered
// `kSkipArmClaims` claimed TAA dispatches running with zero fallbacks. Two things it must NOT
// do. It must not stop the WRITE half (Create*View -> note_view): the bind-stream hop itself is
// a CPU-handle cross-match against exactly those records (u0_rhi_uav.hpp, discovery 2), so
// shadow-write is level 3's own dependency, not its saving — the task brief's "shadow-write +
// shadow-copy" is corrected here. And it must not RESUME recording after a fallback: while
// recording is off the online heap's slots go stale in the shadow, and on resume a table the
// game did not re-copy would resolve to whatever the slot held before — a live, wrong resource
// with no error, the §5 stale-map class. Whether UE 4.27's descriptor cache re-copies every
// table after such a gap is SOFT (FD3D12DescriptorCache's unique-table reuse was not verified
// against 4.27.2 for this), so the skip is a one-way latch per session and a fallback under it
// costs the frame LOUDLY (`noWalk` on the [u0] line, one ERROR) rather than trusting a shadow
// that may lie. The user opts in only after reading `fellBack=0` over a session.
#pragma once

#include <cstdint>

namespace stray_dlss::u0auth {

// [STRAYDLSS] U0Hook. Same ladder shape as EngineSeam and EngineSeamViewParams.
enum class Mode : std::uint8_t
{
	off = 0,
	discover,
	observe,
	authoritative,
};
Mode mode_from_level(int level);
const char *mode_name(Mode m);

// THE SHIPPED DEFAULT, pinned here and by a test, so moving the rung is a deliberate edit
// (the discipline view_cached.hpp applies to EngineSeamViewParams). 1: discover, install
// nothing. Level 3 is BUILT and UNCONFIRMED on the box; the default does not move until the
// user has read a level-3 session's [u0] line.
constexpr int kDefaultLevel = 1;

// CLAUDE.md §2.3's register map: the SR path reads t0 (eye adaptation), t1 (scene colour),
// t2 (depth), t3 (velocity), t4 (stencil), t5 (history) and writes u0.
constexpr unsigned kTaaTexRegs = 6;
constexpr unsigned kOutputUavReg = 0;

// What the live half established about one register of the closed bracket.
enum class RegState : std::uint8_t
{
	absent = 0, // the engine bound nothing at this register in the bracket
	unresolved, // bound, and the object did not resolve (scan refused, vtable shape refused, fault)
	dead,       // resolved to a pointer the resource registry does not call live
	ok,         // resolved, live, described
};

struct BracketFacts
{
	bool hooked = false;       // the thunks are installed
	bool faulted = false;      // the guarded-read fault latch is set; nothing may be read
	bool bracket_open = false; // RHISetComputeShader was seen and at least one bind followed
	RegState t[kTaaTexRegs] = {};
	RegState u0 = RegState::absent;
	// u0's description from the registry, meaningful only when u0 == ok. The rect test is the
	// matcher's (the announced rect is not known before the claim); this is the shape test.
	bool u0_is_buffer = false;
	bool u0_is_3d = false;
	bool u0_allow_uav = false;
	bool u0_hdr_colour = false;
};

enum class Source : std::uint8_t
{
	walk = 0,
	bracket,
};

// Every way the bracket can fail to answer, each counted on the [u0] line as `fellBack:`.
enum class Fallback : std::uint8_t
{
	none = 0,
	level_below_3,   // not asked: U0Hook < 3 (never counted as a fallback; the walk is the design)
	hook_off,        // level 3 configured but the thunks never installed (discovery refused)
	faulted,         // the fault latch is set for the session
	no_bracket,      // no RHISetComputeShader ... bind sequence closed by this dispatch
	u0_no_bind,      // no UAVIndex==0 bind in the bracket
	u0_unresolved,   // the u0 object's handle scan refused
	u0_not_live,     // u0 resolved to a pointer the registry does not call live
	u0_desc_mismatch,// u0 is a buffer, 3D, not UAV-capable or not an HDR colour format
	tex_no_bind,     // a t0..t5 register had no bind (`reg` says which)
	tex_unresolved,  // ...bound, unresolved
	tex_not_live,    // ...resolved, not live
	count
};
const char *fallback_name(Fallback f);

struct Decision
{
	Source source = Source::walk;
	Fallback fallback = Fallback::level_below_3;
	unsigned reg = 0; // for the tex_* reasons: the first register that failed, in t-order
};

// Which source fills `DispatchBindings::srvs`/`::uavs` for THIS dispatch.
Decision decide_source(Mode mode, const BracketFacts &f);

// ---------------------------------------------------------------------------------------
// Skipping the walk: [STRAYDLSS] U0HookSkipWalk
// ---------------------------------------------------------------------------------------

// Claimed TAA dispatches — one per frame the engine announced — that must be answered by the
// bracket, consecutively and with no fallback between them, before recording stops. 600 is
// ~10 s at 60 fps and matches the periodic report interval, so the arming reads off one
// [u0] line. [derived]: a bar, not a measurement; a wrong-but-consistent bracket would pass
// it, which is why level 2's assertion must have run clean across GAMEPLAY first.
constexpr std::uint64_t kSkipArmClaims = 600;

enum class Skip : std::uint8_t
{
	off_no_key = 0, // U0HookSkipWalk is 0: the walk always runs (the shipping value)
	off_mode,       // the key is set but U0Hook < 3: nothing could answer without the walk
	off_not_drive,  // the native backend is not in drive mode (observe needs the walk to diff)
	off_hook,       // level 3 configured, thunks not installed
	off_faulted,    // the fault latch is set; the bracket cannot answer
	arming,         // everything is in place; counting clean claims towards kSkipArmClaims
	armed,          // recording is OFF for the rest of the session (one-way)
	count
};
const char *skip_name(Skip s);

struct SkipInputs
{
	bool key = false;           // [STRAYDLSS] U0HookSkipWalk
	Mode mode = Mode::off;
	bool drive = false;         // native::mode() == drive
	bool hooked = false;
	bool faulted = false;
	bool already_armed = false; // the latch closed earlier this session
	std::uint64_t clean_streak = 0; // consecutive claimed dispatches answered by the bracket
};

// `armed` is sticky: once returned it is returned for the rest of the session whatever else
// changes (see the header comment for why resuming is refused). Everything else is a live
// verdict, recomputed per claim.
Skip skip_decide(const SkipInputs &in);

} // namespace stray_dlss::u0auth
