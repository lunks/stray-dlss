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

// Idempotent; safe to call every frame. Recreates its resources when the resolution changes.
bool initialise(ID3D12Device *device, std::uint32_t render_width, std::uint32_t render_height);
void shutdown();

bool is_ready();

// Records the resolve into the game's command list. The caller is responsible for restoring
// whatever state it needs afterwards — this deliberately does not, because the NGX evaluation
// that follows would clobber it again anyway.
bool record(ID3D12GraphicsCommandList *cmd, const ResolveInputs &in);

// R16G16_FLOAT at render resolution. Valid after a successful record().
ID3D12Resource *output();

const char *last_error();

} // namespace stray_dlss::mv
