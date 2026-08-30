// Dumping the game's compute-shader DXBC to disk.
//
// This exists because the developer cannot run the game. Getting Stray's actual TAA bytecode
// off the user's machine lets the binding layout be read directly out of the shader's
// `dcl_resource_*` declarations, which settles questions that would otherwise cost a field
// test each: what t1/t3/t4 really are, and whether the measured hash still holds.
//
// Off by default. Enable with `DumpShaders=1` under `[STRAYDLSS]` in ReShade.ini.
#pragma once

#include <cstddef>
#include <cstdint>

namespace stray_dlss::shader_dump {

// Reads `[STRAYDLSS] DumpShaders` and prepares the output directory.
void initialise();

bool enabled();

// Writes the bytecode once per distinct hash. Cheap to call for every pipeline.
void dump_compute_shader(std::uint64_t hash, const void *code, std::size_t code_size);

// Appends a line to the manifest recording how a shader was classified when we saw it
// dispatched. Written separately from the dump so it survives shaders seen but never used.
void note_dispatch(std::uint64_t hash, std::uint32_t x, std::uint32_t y, std::uint32_t z);

void finish();

} // namespace stray_dlss::shader_dump
