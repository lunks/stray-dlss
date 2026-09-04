// The DLSS SR bias-current-colour mask — the live D3D12 half.
//
// The gate, the neutral value and the alternation are pure and live in src/core/mv_mask_plan.hpp;
// read that first, it carries the argument for why this is the honest treatment for the
// reflection problem rather than a correction to the motion vectors.
//
// STAGE 1 IS A CONSTANT FILL and there is deliberately no shader here at all: the fill is a
// `ClearUnorderedAccessViewFloat`, exactly as `mv::paint` already does for the magenta
// diagnostic, so this module adds no PSO, no root signature and no HLSL to the one path in the
// frame that is currently known-good. A content-driven mask is level 3 and is declared, not
// built — see the "What one launch should answer" note in the branch's commit message.
#pragma once

#include "core/mv_mask_plan.hpp"

#include <cstddef>
#include <cstdint>

struct ID3D12Device;
struct ID3D12GraphicsCommandList;
struct ID3D12Resource;

namespace stray_dlss::mvmask {

// [STRAYDLSS] MvMaskFormat. The DLSS guide does not state a required format for this input and
// the runtime validates nothing, so this is a knob rather than a constant. 0 is R8_UNORM, which
// is what the one third-party integration known to drive this input successfully uses
// (docs/RESEARCH-DLSS5-FEEDER-OPTIONS.md §7).
constexpr int kFormatR8Unorm = 0;
constexpr int kFormatR16Float = 1;
constexpr int kFormatRgba8Unorm = 2;
constexpr int kDefaultFormat = kFormatR8Unorm;

// All defaults are the behaviour from before this existed: disabled, neutral, no alternation.
void configure(bool enabled, float value, std::uint32_t alternate_period, int format);
bool enabled();

// Idempotent; grow-only, the same policy mv_resolve uses and for the same reason (a flapping
// render resolution during a load must not churn GPU allocations).
bool initialise(ID3D12Device *device, std::uint32_t width, std::uint32_t height);
void shutdown();

// Records whatever fill this frame needs onto the game's list and returns the resource to hand
// to NGX, or nullptr when the plan refused. The returned resource is left in
// NON_PIXEL_SHADER_RESOURCE, which is the state DLSS requires of every input.
//
// `frame` drives the alternation and must be the same monotonic counter across a session.
ID3D12Resource *record(ID3D12GraphicsCommandList *cmd, std::uint32_t render_width,
	std::uint32_t render_height, std::uint64_t frame);

struct Stats
{
	std::uint64_t bound = 0;  // frames the mask was handed to NGX
	std::uint64_t fills = 0;  // clears actually recorded (only when the value changes)
	std::uint64_t refused[static_cast<int>(mvmaskplan::Refusal::count)] = {};
	bool phase_on = false;    // the alternation's current half
	float value = 0.0f;       // what the texture currently holds
	std::uint32_t width = 0;
	std::uint32_t height = 0;
	int format = kDefaultFormat;
};

const Stats &stats();

// One line for the periodic report and for stray-dlss-status.txt.
int format_report(char *buf, std::size_t size);
int format_status(char *buf, std::size_t size);

const char *last_error();

} // namespace stray_dlss::mvmask
