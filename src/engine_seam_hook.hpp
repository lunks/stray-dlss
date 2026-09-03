// The live half of the engine upscaler seam: mapping Stray's own module, running the pure
// scan over it, and — at level 2 — standing in for FDefaultTemporalUpscaler::AddPasses so the
// engine tells us which dispatch is the primary temporal upscale instead of us guessing.
//
// The reasoning, the provenance and the failure modes are in docs/RESEARCH-ENGINE-TAA-HOOK.md.
// The short version, because it decides how this file is allowed to behave:
//
//   * `ITemporalUpscaler::AddPasses` is the ONE call site of the primary temporal upscale
//     (PostProcessing.cpp:559). Every documented FTAAStandaloneCS look-alike — DOF, light
//     shafts, SSR/SSGI, single-layer water, planar reflections, the ray-tracing debug view —
//     calls `AddTemporalAAPass` directly and never touches the interface. So being called
//     through it IS the identification.
//   * NOTHING here has been run against Stray-Win64-Shipping.exe. Discovery is therefore
//     static, self-validating against three exact constants, and refuses loudly; and the
//     default level is 0, which does not even scan.
//   * The cross-check came back from the box on 2026-09-03 (facts §36): orphans=0 over 8570
//     announcements, and the two passes the structural signature let through were both caught
//     by the engine's answer. Level 3 makes that answer THE gate; the hash whitelist and the
//     structural signature become assertions, and the heuristic is the fallback only when the
//     seam is not live — loudly, and only if [STRAYDLSS] EngineSeamFallback allows it.
#pragma once

#include "core/engine_seam.hpp"

#include <cstddef>
#include <cstdint>

namespace stray_dlss::seamhook {

// [STRAYDLSS] EngineSeam (seam::Mode). Each level includes the ones below it.
//   0 — off. Nothing is scanned, nothing is patched.
//   1 — discover and statically validate the ITemporalUpscaler vtable, log the verdict, and
//       install nothing. One log line, cannot change a pixel.
//   2 — additionally stand in for AddPasses and cross-check every announcement against what
//       the heuristic matcher picks. The heuristic still gates DLSS.
//   3 — AUTHORITATIVE, the default: DLSS SR runs only on the dispatch the engine announced.
// `fallback_allowed` is [STRAYDLSS] EngineSeamFallback: at level 3 with no live seam, run the
// heuristic (true, said loudly) or refuse every frame (false).
void configure(int level, bool fallback_allowed, bool inputs_enabled);

seam::Mode mode();
bool fallback_allowed();
// True when level >= 1 and discovery succeeded.
bool discovered();
// True when the AddPasses stand-in is installed.
bool hooked();
// The gate for one candidate dispatch, from the live mode/hook state and `announced`.
seam::Gate gate(bool announced);

// Frame boundary for the ledger. Called from taa_hook::note_present.
void note_present(std::uint64_t frame);

// What the engine said about a dispatch the heuristic matcher has just accepted as the TAA
// pass. `announced` false with `active` true is the wrong-pass class, caught.
struct Verdict
{
	bool active = false;      // the seam is hooked, so the answer means something
	bool announced = false;   // an announcement matched this dispatch's group counts
	std::uint32_t out_width = 0;
	std::uint32_t out_height = 0;
	std::uint64_t sequence = 0;
	// The engine's own FPassInputs, for L1. Identities at this point; resolve_inputs turns
	// them into ID3D12Resource* and is the only thing allowed to dereference them.
	std::uint64_t colour_rdg = 0;
	std::uint64_t depth_rdg = 0;
	std::uint64_t velocity_rdg = 0;
	// May those three pointers be dereferenced? Only while the announcing FRDGBuilder lives:
	// newest announcement, same frame, same thread (seam::announcement_is_fresh). The ledger
	// claims IDENTITY with slack on purpose; POINTERS get none. False is a normal, counted
	// decline - L1 sits the frame out and the heuristic supplies the inputs.
	bool fresh = false;
	std::uint64_t announce_frame = 0;
	std::uint64_t current_frame = 0;
	std::uint64_t announce_thread = 0;
	std::uint64_t current_thread = 0;
	std::uint64_t ledger_sequence = 0;
};
Verdict claim(std::uint32_t group_x, std::uint32_t group_y);

// L1. The engine handed us its scene colour, depth and velocity; this turns each into the
// ID3D12Resource the D3D12 side already speaks. Call at CLAIM time (during graph execution):
// at AddPasses time a graph-allocated texture has no RHI resource yet.
//
// Every resolved pointer is validated against our own resource registry before it is returned,
// so a wrong offset yields `registry` (a pointer nothing knows) rather than a plausible lie.
// Anything not `ok` leaves that resource 0 and the caller falls back to the heuristic.
struct EngineInputs
{
	bool enabled = false;              // [STRAYDLSS] EngineSeamInputs and the seam are on
	std::uint64_t colour = 0;          // ID3D12Resource*, 0 when unresolved
	std::uint64_t depth = 0;
	std::uint64_t velocity = 0;
	seam::RhiChain colour_status = seam::RhiChain::null_rdg;
	seam::RhiChain depth_status = seam::RhiChain::null_rdg;
	seam::RhiChain velocity_status = seam::RhiChain::null_rdg;
	bool colour_registered = false;    // the resolved pointer is one our registry calls live
	bool depth_registered = false;
	bool velocity_registered = false;

	bool depth_ok() const { return depth != 0 && depth_registered; }
	bool velocity_ok() const { return velocity != 0 && velocity_registered; }
	bool colour_ok() const { return colour != 0 && colour_registered; }
};
EngineInputs resolve_inputs(const Verdict &v);

// Records what happened to a dispatch the ENGINE announced and we claimed. Continuous, not
// once-per-pass: the rate is the diagnosis. seam::SeamRefusal::none means SR evaluated.
void note_outcome(seam::SeamRefusal r);

// One WARN per session when the engine's resource and the heuristic's disagree. The engine
// wins; this exists so a wrong image has a first line to read.
void note_input_disagreement(const char *which, std::uint64_t engine, std::uint64_t heuristic);

// One line for the periodic report, and the same numbers for stray-dlss-status.txt.
// Writes at most `size` bytes including the terminator; returns the bytes written.
int format_report(char *buffer, std::size_t size);
// The same numbers into the log, so the verdict is readable without the status file (which
// is off in normal runs). `when` is the frame label the other periodic lines use.
void log_report(const char *when);

// Undoes the vtable patch. Must run before this DLL can be unloaded: a vtable slot pointing
// into a module that is gone is an address-0 crash on the next frame, which this project has
// already paid for once (docs/RESEARCH-UE4SS-MIGRATION.md, the device-recreate reload).
void shutdown();

} // namespace stray_dlss::seamhook
