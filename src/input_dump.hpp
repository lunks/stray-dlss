// Diagnostic: dump the exact resources handed to NGX to disk, for offline inspection.
//
// The paint test proved the OUTPUT handle is the displayed buffer, which moves the fault into
// DLSS's inputs: the frozen scene means the colour we pass does not change between frames.
// This module produces the evidence. At fixed evaluate counts it copies the colour and depth
// inputs (before the evaluate) and the output (after it) into READBACK buffers, then a few
// presents later writes the raw bytes into the game directory. Two samples of the same input
// with identical bytes prove a stale texture; converting them to PNG shows what DLSS sees.
//
// It also dumps the ENGINE'S OWN sparse velocity buffer (`velocity_raw`, R16G16B16A16_UNORM)
// and our resolved dense field (`mv`, R16G16_FLOAT). Those two answer a question neither the
// colour nor the resolved vectors can: which pixels UE4 actually wrote an object velocity for.
// mv_resolve.hlsl produces a plausible vector for every pixel — decoded object motion where
// `EncodedVelocity.x > 0`, reconstructed camera motion everywhere else — so the branch a pixel
// took is invisible downstream, and `R > 0` over the raw buffer is the only way to read it.
//
// [STRAYDLSS] NgxDumpInputs=1. Diagnostic only — 16-66 MB per capture, freed after the write.
// [STRAYDLSS] NgxDumpAt=<evaluate> moves the capture points; see set_points below.
#pragma once

#include <cstdint>

struct ID3D12Device;
struct ID3D12GraphicsCommandList;
struct ID3D12Resource;

namespace stray_dlss::input_dump {

void set_enabled(bool enabled);
bool enabled();

// [STRAYDLSS] NgxDumpAt: the FIRST evaluate to capture at; 0 keeps the shipped 600/900. The
// second point follows a fixed gap. See src/core/dump_plan.hpp for why this is configurable at
// all — a run still on a loading screen at evaluate 600 dumps a black frame and answers nothing.
void set_points(int configured);
std::uint64_t first_point();
std::uint64_t second_point();

// True when `evaluate_count` is one of the dump points.
bool wants(std::uint64_t evaluate_count);

// Record a copy of `resource` into a fresh readback buffer on `cmd`. `state_before` is the
// D3D12_RESOURCE_STATES value the resource is in at the call (and is restored to). `name` tags
// the output file: straydlss_<name>_<count>.bin. Copies subresource/plane 0 only, which for
// the R32G8X24 depth resource is the R32 depth plane.
bool capture(ID3D12Device *device, ID3D12GraphicsCommandList *cmd, ID3D12Resource *resource,
             unsigned int state_before, const char *name, std::uint64_t evaluate_count);

// Capture a 1x1 texel and LOG its four RGBA32F floats to the ReShade log a few presents
// later (instead of writing a file). For the eye-adaptation exposure diagnosis: shape says
// nothing about whether the VALUE is one DLSS will accept, and only a readback tells us.
// `state_before` is the D3D12_RESOURCE_STATES the resource is in at the call (restored
// after the copy). `label` tags the log line. Copies subresource 0.
bool capture_texel(ID3D12Device *device, ID3D12GraphicsCommandList *cmd,
                   ID3D12Resource *resource, unsigned int state_before, const char *label,
                   std::uint64_t evaluate_count);

// Call once per present: writes out any capture recorded >= 5 presents ago and frees it.
void on_present();

} // namespace stray_dlss::input_dump
