// The G-buffer resolve pass for DLSS Ray Reconstruction (phase 2 of the RR milestone):
// reads the game's GBufferA/B/C — identified by the gbuffer finder — and emits the four
// RR guide textures (normals+roughness, standalone roughness, diffuse albedo, specular
// albedo) using NVIDIA's own UE-plugin recipe. docs/RESEARCH-RR-GBUFFER.md §2.4/§4.4;
// formula provenance in src/core/envbrdf.hpp; shader in shaders/gbuffer_resolve.hlsl.
//
// NOT WIRED INTO ANY FRAME PATH YET. This is the module and its CI-testable surface only;
// phase 3 connects it to the TAA hook alongside the DLSSD create/evaluate. Nothing calls
// record() in the shipping add-on today, so shipping this changes no behaviour.
//
// Written in raw D3D12 in the mv_resolve mould, and for the same reasons (mv_resolve.hpp):
// build-time-compiled DXBC (never D3DCompile at runtime — CLAUDE.md §6), our own
// descriptor heap and root signature, ring-buffered per-frame descriptors and constants
// (core/ring.hpp), SRVs recreated from resource pointers every frame (descriptor COPY out
// of the game's shader-visible heap is illegal — CLAUDE.md §5), grow-only allocation with
// retirement (the resolution-churn OOM lesson), and an AddRef keep-alive ring so the GPU
// never reads a freed input (the Xid-109 lesson). The caller owns liveness checks BEFORE
// handing resources in, exactly as it does for mv_resolve.
#pragma once

#include <cstdint>

struct ID3D12Device;
struct ID3D12GraphicsCommandList;
struct ID3D12Resource;

namespace stray_dlss::gbr {

struct ResolveInputs
{
	// The GAME's G-buffer resources, as identified by gbuffer_finder (slot 1 -> A,
	// slot 2 -> B, slot 3 -> C — RESEARCH-RR-GBUFFER.md §1.5). Liveness-checked by the
	// caller at capture time; this module AddRefs them across GPU execution.
	std::uint64_t gbuffer_a = 0;
	std::uint64_t gbuffer_b = 0;
	std::uint64_t gbuffer_c = 0;

	// The render rect. The G-buffers are BUFFER-sized (scene-buffer extent, §1.1), so the
	// shader indexes them at view_rect_min + thread id, same as mv_resolve.
	std::uint32_t render_width = 0;
	std::uint32_t render_height = 0;
	float view_rect_min[2] = { 0.0f, 0.0f };

	// NoV geometry. proj00/proj11 are ViewToClipNoAA[0][0] and [1][1] (View CB row 32 — a
	// measured anchor, CLAUDE.md §2.6; NoAA on purpose, jitter-free). world_to_view is the
	// TRANSPOSED upper 3x3 of View.TranslatedWorldToView (rows 12-15, mirror-verified) —
	// fill it with ue4::nov_rotation_rows, which owns the row-vector-to-dot(row,n)
	// convention flip, and gate on world_to_view_rotation_plausible first (a wrong
	// rotation biases specular albedo, silently).
	float proj00 = 1.0f;
	float proj11 = 1.0f;
	float world_to_view[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
};

// Idempotent; safe to call every frame. Grow-only across resolution changes, like
// mv_resolve: a smaller frame dispatches over part of the textures.
bool initialise(ID3D12Device *device, std::uint32_t render_width, std::uint32_t render_height);
void shutdown();

bool is_ready();

// The B/C content-check flip (RESEARCH-RR-GBUFFER.md §1.5, §5.4): GBufferB and GBufferC
// are format-indistinguishable and their identification rests on stock slot order alone.
// When the on-target content check (consumption cross-check, or a visual guide dump)
// says the order is swapped, this flips which captured resource feeds the B and C roles —
// at runtime, no rebuild. Swapping B and C poisons BOTH albedos and roughness, which is
// exactly the wrong-but-plausible image CLAUDE.md §0.2 exists to prevent; the flag state
// is logged at every use site so a pasted log always says which way it ran.
void set_bc_swapped(bool swapped);
bool bc_swapped();

// Records the resolve into the given command list. The caller restores state afterwards
// (the NGX evaluation that follows clobbers it anyway — the mv_resolve contract).
// dispatch_mode: 0 records every state change but skips the Dispatch, 1 issues a single
// 8x8 group, 2 covers the frame — the same three-way isolation knob as mv_resolve.
bool record(ID3D12GraphicsCommandList *cmd, const ResolveInputs &in, int dispatch_mode);

// The four guide textures, all at the allocated render extent, valid after a successful
// record(). Formats per the RR guide's table and D3D12's guaranteed typed-UAV-store list
// (rationale in shaders/gbuffer_resolve.hlsl):
ID3D12Resource *normals_roughness(); // RGBA16F: xyz signed world normal, w roughness
ID3D12Resource *roughness();         // R16F: standalone linear roughness
ID3D12Resource *diffuse_albedo();    // RGBA8_UNORM (linear, never sRGB)
ID3D12Resource *specular_albedo();   // RGBA8_UNORM (linear)

// Transitions all four outputs between UNORDERED_ACCESS (written by the resolve) and
// NON_PIXEL_SHADER_RESOURCE (read by NGX). Same rationale as mv_resolve::transition_output.
void transition_outputs(ID3D12GraphicsCommandList *cmd, bool to_shader_resource);

// Allocation accounting, mirroring mv_resolve::Stats — the churn that OOM'd the GPU once
// must stay measurable.
struct Stats
{
	std::uint64_t resource_sets_created = 0;
	std::uint64_t resource_sets_retired = 0;
	std::uint64_t resource_sets_released = 0;
	std::uint32_t live_retired = 0;
	std::uint64_t bytes_live = 0;
};

const Stats &stats();

const char *last_error();

} // namespace stray_dlss::gbr
