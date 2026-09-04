// Whether this frame's ENGINE-NAMED G-buffer set may feed DLSS Ray Reconstruction.
//
// WHAT THIS REPLACES, and why the shape matters more than the code. RR's guides used to come
// from a heuristic finder that identified GBufferA-E by descriptor SHAPE — format, extent, draw
// pattern — and it was deleted on 2026-09-03 for being the same class of guessing the engine
// seam retired for the TAA pass (docs/RESEARCH-ENGINE-TAA-HOOK.md §13). The replacement is the
// engine's own name: FRenderTargetPool::FindFreeElement takes `const TCHAR* InDebugName` as a
// live argument and our forwarding recorder reads it (src/core/pool_locator.hpp,
// src/pool_name_hook.hpp). So identity is no longer inferred at all.
//
// IDENTITY IS NOT LIFETIME, AND THIS FILE IS WHERE THE TWO ARE KEPT APART. The name record is
// written when the engine ALLOCATES the target; the resolve reads the resource much later in the
// same frame. This project has already crashed once dereferencing a pointer whose IDENTITY was
// right and whose LIFETIME had ended (CLAUDE.md §2.3, the FRDGTexture arena), and once faulted
// creating a view on a resource ReShade's map still named (CLAUDE.md §5, hazard 2). So:
//
//   * IDENTITY comes from the record and is warranted by the engine's own argument. It is what
//     `epoch` and `name_missing` police: a set whose three members were not written by ONE
//     allocation cycle is not a set, whatever each member individually is.
//   * LIVENESS comes from OUR OWN resource registry, re-checked at CONSUME time on every frame,
//     and is passed in here as `Record::live`. It is never inferred from the record's age, its
//     status, or the fact that the engine named it a moment ago.
//   * FRESHNESS is a bound on how long ago the record was written, and it gates because a
//     pointer the engine has not re-affirmed for several frames is one whose element the pool
//     may have handed elsewhere. It is COUNTED separately from every other refusal precisely so
//     that a session in which the engine does not re-allocate every frame says so in one line
//     instead of looking like a broken mechanism.
//
// Pure logic: no Windows, no D3D, no engine. Everything here is decided from numbers the caller
// has already read, and pinned by tests/test_rr_guides.cpp.
#pragma once

#include <cstddef>
#include <cstdint>

namespace stray_dlss::rrguides {

// Why a frame's guide set did not reach Ray Reconstruction. Every one is counted and named on
// the periodic line: CLAUDE.md's rule is that a pass which never reaches DLSS must never be
// indistinguishable from a pass that was not there.
enum class Refusal : std::uint8_t
{
	none = 0,
	not_supplying,     // [STRAYDLSS] PoolNames < 3 — the map exists but nothing publishes it
	not_hooked,        // the forwarding recorder is not installed (discovery refused, or level 1)
	name_missing,      // GBufferA, B or C has never been named by the engine
	record_bad,        // the name was seen but its chain did not end in a registered resource
	epoch_split,       // A, B and C were not written by ONE allocation cycle
	stale,             // the set's cycle is older than kMaxSetAgeFrames
	not_live,          // our own resource registry does not call one of them live — REFUSED
	extent_disagree,   // A, B and C are not all at one extent
	extent_mismatch,   // that extent is not the scene-buffer extent View row 132 reports
	extent_too_small,  // the set does not cover the render rect the resolve would dispatch over
	format_a,          // GBufferA is not the RGB10A2 family our N*2-1 decode assumes
	format_bc,         // GBufferB or GBufferC is not the 8-bit RGBA family
	count
};
const char *refusal_name(Refusal r);
constexpr std::size_t kRefusalCount = static_cast<std::size_t>(Refusal::count);

// One named pooled render target, as the caller has already resolved it.
struct Record
{
	bool seen = false;        // the engine has named it at least once
	bool status_ok = false;   // pool::RecordStatus::ok — the chain reached a registered resource
	bool live = false;        // OUR registry, re-checked at consume time. Never assumed.
	std::uint64_t epoch = 0;  // the allocation cycle that wrote this record (0 = never)
	std::uint64_t frame = 0;  // the present frame at which it was written
	std::uint64_t resource = 0;
	std::uint32_t width = 0;
	std::uint32_t height = 0;
	std::uint32_t dxgi_format = 0;
};

struct Set
{
	Record a;
	Record b;
	Record c;
};

struct Expect
{
	std::uint64_t frame_now = 0;
	// View row 132 BufferSizeAndInvSize (CLAUDE.md §2.6). 0 means we do not know it this frame,
	// and an unknown is not a disagreement — it simply skips that one check.
	std::uint32_t buffer_width = 0;
	std::uint32_t buffer_height = 0;
	// The rect the resolve would dispatch over. The G-buffers are BUFFER-sized
	// (docs/RESEARCH-RR-GBUFFER.md §1.1), so they must COVER this, never equal it.
	std::uint32_t render_width = 0;
	std::uint32_t render_height = 0;
	bool supplying = false;   // [STRAYDLSS] PoolNames == 3
	bool hooked = false;      // the recorder is installed
};

// How many present frames a name record may be stale before the set is refused. The engine
// releases the G-buffer refcount at the end of every Render and re-acquires it the next frame
// (docs/RESEARCH-RR-GBUFFER.md §1.1), so in steady state the age at the TAA hook is 0 or 1;
// this is generous rather than tight because the value the counter reports is more useful than
// the value we guessed. SOFT: that FindFreeElement is reached every frame for these names is
// derived from the alloc/release pair, not measured.
constexpr std::uint64_t kMaxSetAgeFrames = 4;

// GBufferA carries WorldNormal*0.5+0.5 in an RGB10A2 target under Stray's shipped configuration
// (docs/RESEARCH-RR-GBUFFER.md §1.2: GetGBufferAFormat returns PF_A2B10G10R10 because
// r.GBufferFormat is not >= 3). The high-precision paths use an OCTAHEDRAL encoding that our
// N*2-1 decode would silently mis-read, so any other format is a REFUSAL, never a fallback.
bool gbuffer_a_format_ok(std::uint32_t dxgi_format);
// GBufferB and GBufferC are 8-bit RGBA (PF_B8G8R8A8, C additionally TexCreate_SRGB). Both
// channel orders and all three typed/typeless/sRGB spellings are accepted: the SRV the resolve
// creates handles the ordering, and the sRGB decode is decided from the resource's own format.
bool gbuffer_bc_format_ok(std::uint32_t dxgi_format);

// The verdict. Pure; order of the checks is the order of the enum, so the FIRST thing wrong is
// what gets reported and a log line never has to be read as a set.
Refusal judge(const Set &set, const Expect &expect);

} // namespace stray_dlss::rrguides
