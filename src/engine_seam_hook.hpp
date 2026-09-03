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
void configure(int level, bool fallback_allowed);

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
};
Verdict claim(std::uint32_t group_x, std::uint32_t group_y);

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
