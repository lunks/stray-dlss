// The dense motion-vector resolve pass.
//
// Written in raw D3D12 rather than through ReShade's abstraction. We have to save and restore
// command-list state around NGX anyway (NGX clobbers descriptor heaps, root signature and PSO
// and the game will not re-set them), so owning our own heap and root signature is simpler and
// more predictable than interleaving with ReShade's heap management. (docs/RESEARCH.md §3.5)
#pragma once

#include <cstdint>

struct ID3D12Device;
struct ID3D12GraphicsCommandList;
struct ID3D12Resource;

namespace stray_dlss::ue4 { struct ViewParams; }

namespace stray_dlss::mv {

struct ResolveInputs
{
	// The GAME's CPU descriptor handles for its depth and velocity SRVs, copied into our heap
	// rather than recreated. Recreating them would mean dereferencing ID3D12Resource pointers
	// that ReShade may have stale (it never calls destroy_resource_view on D3D12), which is an
	// access violation the moment the game rotates a buffer. (docs/RESEARCH.md §2.7)
	std::uint64_t depth_descriptor = 0;
	std::uint64_t velocity_descriptor = 0;
	std::uint32_t render_width = 0;
	std::uint32_t render_height = 0;
	const ue4::ViewParams *view = nullptr;
};

// Names the device to use for DESCRIPTOR work — creating our heap and copying the game's SRVs
// into it — when it differs from the device that owns our resources.
//
// Under ReShade this MUST be its proxy device. ReShade's descriptor-heap proxy returns
// bit-packed synthetic CPU handles rather than real addresses, so the game's descriptors are in
// ReShade's space; copying them with the native device reads near-null garbage and the GPU
// faults as soon as a dispatch samples the result. The proxy converts both sides back.
//
// Call before initialise(). Leaving it unset keeps everything native, which is correct when
// there is no ReShade in the process.
void set_descriptor_device(ID3D12Device *device);

// Idempotent; safe to call every frame. Recreates its resources when the resolution changes.
bool initialise(ID3D12Device *device, std::uint32_t render_width, std::uint32_t render_height);
void shutdown();

bool is_ready();

// Records the resolve into the game's command list. The caller is responsible for restoring
// whatever state it needs afterwards — this deliberately does not, because the NGX evaluation
// that follows would clobber it again anyway.
// `dispatch` false records every state change but skips the Dispatch itself, which separates
// "our state changes break the game" from "our GPU work breaks the game" in one run.
// dispatch_mode: 0 skips the Dispatch entirely, 1 issues a single 8x8 group, 2 covers the
// frame. One group still exercises the root signature, heap and shader without touching most
// of the image, which separates "dispatching at all is wrong" from "the workload is wrong".
bool record(ID3D12GraphicsCommandList *cmd, const ResolveInputs &in, int dispatch_mode);

// R16G16_FLOAT at render resolution. Valid after a successful record().
ID3D12Resource *output();

// Allocation accounting. The GPU ran out of memory during a real run, and resource churn in
// initialise() is the prime suspect: the render resolution is taken from whichever dispatch
// matched, so a flapping size reallocates the heap, constant buffer and output texture. These
// counters make that measurable rather than theoretical, and are asserted in the WARP test.
struct Stats
{
	std::uint64_t resource_sets_created = 0;  // times create_resources() ran
	std::uint64_t resource_sets_retired = 0;  // times a set was handed to the retirement list
	std::uint64_t resource_sets_released = 0; // times a retired set was actually freed
	std::uint32_t live_retired = 0;           // retired sets still holding GPU memory
	std::uint64_t bytes_live = 0;             // approximate output-texture bytes currently held
};

const Stats &stats();

const char *last_error();

} // namespace stray_dlss::mv
