// Diagnostic: dump the exact resources handed to NGX to disk, for offline inspection.
//
// The paint test proved the OUTPUT handle is the displayed buffer, which moves the fault into
// DLSS's inputs: the frozen scene means the colour we pass does not change between frames.
// This module produces the evidence. At fixed evaluate counts it copies the colour and depth
// inputs (before the evaluate) and the output (after it) into READBACK buffers, then a few
// presents later writes the raw bytes into the game directory. Two samples of the same input
// with identical bytes prove a stale texture; converting them to PNG shows what DLSS sees.
//
// [STRAYDLSS] NgxDumpInputs=1. Diagnostic only — 16-66 MB per capture, freed after the write.
#pragma once

#include <cstdint>

struct ID3D12Device;
struct ID3D12GraphicsCommandList;
struct ID3D12Resource;

namespace stray_dlss::input_dump {

void set_enabled(bool enabled);
bool enabled();

// True when `evaluate_count` is one of the dump points.
bool wants(std::uint64_t evaluate_count);

// Record a copy of `resource` into a fresh readback buffer on `cmd`. `state_before` is the
// D3D12_RESOURCE_STATES value the resource is in at the call (and is restored to). `name` tags
// the output file: straydlss_<name>_<count>.bin. Copies subresource/plane 0 only, which for
// the R32G8X24 depth resource is the R32 depth plane.
bool capture(ID3D12Device *device, ID3D12GraphicsCommandList *cmd, ID3D12Resource *resource,
             unsigned int state_before, const char *name, std::uint64_t evaluate_count);

// Call once per present: writes out any capture recorded >= 5 presents ago and frees it.
void on_present();

} // namespace stray_dlss::input_dump
