// The dense motion-vector resolve pass.
//
// Written in raw D3D12 rather than through ReShade's abstraction. We have to save and restore
// command-list state around NGX anyway (NGX clobbers descriptor heaps, root signature and PSO
// and the game will not re-set them), so owning our own heap and root signature is simpler and
// more predictable than interleaving with ReShade's heap management. (docs/RESEARCH.md §3.5)
#pragma once

#include "core/mv_census.hpp"

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

// Transitions our motion-vector texture between the state our resolve writes it in
// (UNORDERED_ACCESS) and the state a consumer must read it in (NON_PIXEL_SHADER_RESOURCE).
//
// NGX requires its inputs in NON_PIXEL_SHADER_RESOURCE and its output as a UAV; NVIDIA's guide
// is explicit that wrong resource usage can yield a black image "without further indication",
// and under vkd3d — which validates none of this — it faults instead.
void transition_output(ID3D12GraphicsCommandList *cmd, bool to_shader_resource);

// Diagnostic: clear `target` to solid magenta.
//
// The decisive test for "is our captured output handle actually the buffer this frame
// displays". DLSS reports evaluate OK yet the scene stays frozen, which is consistent with
// writing a resource nothing reads. Paint is unambiguous: a magenta screen proves the handle is
// right and moves the fault into the evaluate; a still-frozen scene proves the handle is wrong
// and everything downstream of it was noise.
bool paint(ID3D12GraphicsCommandList *cmd, ID3D12Resource *target);

// Per-branch motion-vector sign. The resolve has two paths — decoded object velocity for the
// pixels UE4's SPARSE velocity buffer actually wrote, and reconstructed camera motion for the
// rest — and their conventions were derived separately, so one can be inverted while the other
// is right. That failure is branch-selective: it shows on moving objects while static geometry
// stays clean, which is not decidable by reading either path. Every DLSS injector ships this as
// a switch for the same reason. (1,1)/(1,1) is the long-standing behaviour.
void set_signs(float sparse_x, float sparse_y, float camera_x, float camera_y);

// A/B for the row_major fix in shaders/mv_resolve.hlsl. TRUE reproduces the old, transposed
// camera branch (HLSL's default column-major packing reading UE's row-major upload), so the
// fix can be confirmed live in one session. Judge it while MOVING: with a still camera
// ClipToPrevClip is ~identity and a transposed identity is still the identity, which is exactly
// why the bug was invisible for so long.
void set_legacy_transposed_clip(bool legacy);

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

// --- The census: LEVEL 1, measures and changes nothing ---
//
// [STRAYDLSS] MvStats (default 0 = OFF, i.e. byte-identical to before this existed). When on,
// shaders/mv_resolve.hlsl additionally reduces sixteen per-pixel counters through LDS into a
// root UAV; the vector it writes to the output texture is unchanged either way. Every
// `window` dispatches the counters are copied to a readback buffer and zeroed on the GPU, and
// the result is folded in a few presents later by on_present().
//
// The number this exists to produce: what fraction of the motion field UE 4.27 actually WROTE
// versus what we reconstruct from depth. It bounds how much the reconstruction's quality can
// matter, and nobody has ever measured it. (src/core/mv_census.hpp)
void set_stats(bool enabled, std::uint32_t window_frames);
bool stats_enabled();

// Drains any completed census readback. Must be called once per present, from the same place
// input_dump::on_present() is called — the readback is fence-free and paced in presents, the
// same conservative latency every other readback in this project uses.
void on_present();

// Everything folded in so far, plus the most recent window on its own. Both are handed to
// mvcensus::format_report / format_status, which REFUSE to print a split when the counters
// contradict the shader's own construction.
const mvcensus::Census &census_total();
const mvcensus::Census &census_last();

const char *last_error();

} // namespace stray_dlss::mv
