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
//   * At level 2 this observes and counts. It does NOT gate DLSS. The heuristic matcher in
//     src/core/taa_signature.cpp remains the mechanism until the cross-check has come back
//     from the box saying the two agree.
#pragma once

#include <cstddef>
#include <cstdint>

namespace stray_dlss::seamhook {

// [STRAYDLSS] EngineSeam
//   0 — off. Nothing is scanned, nothing is patched. The default.
//   1 — discover and statically validate the ITemporalUpscaler vtable, log the verdict, and
//       install nothing. One log line answers the whole feasibility question and cannot
//       change a pixel.
//   2 — additionally stand in for AddPasses and cross-check every announcement against what
//       the heuristic matcher picks. Still changes no pixel: the thunk forwards.
void configure(int level);

// True when level >= 1 and discovery succeeded.
bool discovered();
// True when the AddPasses stand-in is installed.
bool hooked();

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

// Undoes the vtable patch. Must run before this DLL can be unloaded: a vtable slot pointing
// into a module that is gone is an address-0 crash on the next frame, which this project has
// already paid for once (docs/RESEARCH-UE4SS-MIGRATION.md, the device-recreate reload).
void shutdown();

} // namespace stray_dlss::seamhook
