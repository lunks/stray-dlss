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
	// The GAME's depth and velocity RESOURCES. We build our own SRVs for them rather than
	// copying the game's descriptors.
	//
	// Copying was the original design, on the assumption that the handles we capture live in
	// UE4's offline heaps. Measured in the live game they do not: the source heap is
	// `type=0 NumDescriptors=500000 SHADER_VISIBLE=YES`. D3D12 forbids a shader-visible copy
	// SOURCE — "D3D12 ERROR #654: SrcDescriptorRangeStart points to a descriptor heap type that
	// is CPU write only, so reading it is invalid", reproduced in our own CI. vkd3d-proton has
	// no debug layer to object, so the illegal copy silently produced a descriptor the GPU
	// rejected: MvDispatch=0 survived because nothing read it, and a single 1x1 dispatch hung
	// the GPU with Xid 109.
	//
	// The old comment here warned that recreating views means dereferencing resource pointers
	// ReShade may hold stale. That risk is unchanged by this: describe() already asks ReShade
	// for get_resource_desc on the same resource, so we depend on its validity either way.
	std::uint64_t depth_resource = 0;
	std::uint64_t velocity_resource = 0;
	std::uint32_t render_width = 0;
	std::uint32_t render_height = 0;
	const ue4::ViewParams *view = nullptr;
};

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
