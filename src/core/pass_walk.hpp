// Dataflow identification of the temporal-AA pass: a bounded, anchored walk backwards from
// the presented image, over one frame's recorded dispatches, draws and copies.
//
// Why this exists — every simpler identification has been defeated in the field:
//
//   * DXBC hash          — fingerprints a compiled PERMUTATION. Changing screen percentage
//                          flips AA_UPSAMPLE and the hash no longer exists. (CLAUDE.md §2.3)
//   * structural match   — depth SRV + velocity SRV + HDR colour UAV matches 32 passes;
//                          every temporal effect in a deferred renderer has that shape.
//   * history round-trip — measured necessary but NOT sufficient: ~8 passes rebind their own
//                          previous output.
//   * suppression        — proves a pass is on the DISPLAY CHAIN, not that it is the TAA. It
//                          selected 0xd2e4d8c23c362ed1, a composite writing two colour
//                          targets plus depth plus stencil; replacing it broke the frame.
//                          (CLAUDE.md §1, "0xd2e4d8c23c362ed1 is NOT the TAA")
//
// What actually discriminates is DIRECTION plus EARLY TERMINATION. Walk from the tonemapper
// (anchored by the 3D colour-grading LUT SRV that nothing else in an Unreal frame binds)
// backwards through per-frame last-writers, and stop at the first compute pass that both
// reads a velocity-format SRV and owns the temporal history. Reachability alone proves
// nothing — every upstream pass reaches the display THROUGH the TAA — but stopping at the
// first candidate is exactly what rejects the SSR denoiser (upstream, shielded by the TAA)
// and motion blur (downstream of the TAA, full-res HDR compute, but with no history).
//
// One more gate, learned the hard way: a pass may only be REPLACED if every output it
// writes is one we reproduce. FTAAStandaloneCS writes colour — one full-res target,
// optionally a half-res companion — and nothing else. A candidate that also writes depth,
// stencil, or a second full-res target is reported but never selected.
//
// Pure logic: no D3D12, no ReShade, no game. The add-on records FrameEvents (pass_finder);
// CI proves the walk on synthetic Unreal-shaped frames (tests/test_pass_walk.cpp).
#pragma once

#include "taa_signature.hpp"

#include <cstdint>
#include <functional>
#include <vector>

namespace stray_dlss {

// One recorded GPU operation that writes somewhere a later operation can read.
struct FrameEvent
{
	enum class Kind
	{
		dispatch, // outputs = UAVs
		draw,     // outputs = render targets + depth-stencil view
		copy,     // srvs = { source }, outputs = { destination }
	};

	Kind kind = Kind::dispatch;
	// Total order on the queue: (execute-order of the command list << 32) | record-order
	// within the list. Only the relative order matters to the walk.
	std::uint64_t sequence = 0;
	// DXBC hash for dispatches (and for draws where known); 0 for copies.
	std::uint64_t pipeline_hash = 0;
	std::vector<BoundTexture> srvs;
	std::vector<BoundTexture> outputs;
};

// One step of the walk, rich enough that the add-on can log op / hash / resource / format /
// dims per hop without re-deriving anything.
struct WalkHop
{
	std::uint64_t sequence = 0;
	FrameEvent::Kind kind = FrameEvent::Kind::dispatch;
	std::uint64_t pipeline_hash = 0;
	// The resource whose last writer this event is — i.e. what we followed to get here.
	std::uint64_t resource = 0;
	TexFormat format = TexFormat::unknown;
	std::uint32_t width = 0;
	std::uint32_t height = 0;
	const char *note = "";
};

struct WalkResult
{
	// 0 when nothing was selected; `reason` then says why, and the chain still records every
	// hop taken, because the chain is the diagnostic even when the verdict is "not found".
	std::uint64_t taa_hash = 0;
	std::vector<WalkHop> chain;
	const char *reason = "";
};

// The hop budget. The UE4 post chain between the TAA and the tonemapper is short — DOF /
// motion blur / bloom apply, at most a copy in between — so the real TAA sits within three
// hops of the anchor. Six leaves slack for a licensee pass or an extra copy while still
// terminating long before the walk could wander into the base pass.
constexpr std::uint32_t kMaxWalkHops = 6;

// The write-set gate: true when `outputs` is a write set DLSS can fully reproduce — exactly
// one full-res colour target in a DLSS-accepted HDR format, optionally companions no larger
// than half-res in each dimension (FTAAStandaloneCS's optional half-res OutComputeTex_1),
// and nothing else. 1x1 bindings are dummies, not outputs. Any depth write, stencil write,
// non-HDR-colour target or second full-res target fails the gate: replacing such a pass
// leaves outputs unwritten and everything downstream reads stale data — the measured
// 0xd2e4d8c23c362ed1 failure.
//
// `why`, when non-null, receives a static string naming the failing rule.
bool is_replaceable_write_set(const std::vector<BoundTexture> &outputs, const char **why = nullptr);

// Walks backwards from the presented image and returns the first replaceable temporal pass.
//
//   frame              — every recorded event of ONE frame, in any order (sequences decide).
//   presented_resource — the swapchain back buffer this frame presented, used only as the
//                        fallback anchor when no tonemapper (3D LUT SRV) is found. 0 = none.
//   owns_history       — whether this pipeline hash has demonstrated the history round-trip
//                        (its output rebound as its own SRV on a later frame). That signal
//                        accumulates ACROSS frames, which is why it is the caller's, not
//                        derivable from one frame. A null function means "no proof for
//                        anything" and the walk will select nothing.
//
// The walk never guesses: every failure path degrades to taa_hash == 0 with a `reason` that
// names the failing step, because a clear "not found" costs one more observation run and a
// wrong answer costs a broken frame and a week. (CLAUDE.md §0.2)
WalkResult find_temporal_pass(const std::vector<FrameEvent> &frame,
                              std::uint64_t presented_resource,
                              const std::function<bool(std::uint64_t)> &owns_history);

} // namespace stray_dlss
