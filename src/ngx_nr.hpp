// DLSS Neural Rendering (NGX feature 18) — the "DLSS 5" neural post-pass.
//
// Feature 18 is NVSDK_NGX_Feature_Reserved18 in the public SDK (nvsdk_ngx_defs.h:224); NVIDIA's
// leaked pre-release DLSSNR runtime occupies that reserved slot. Everything here is derived from
// our own study of the RenoDX "DLSS5 Generic" addon, which reverse-engineered the mechanism:
// docs/RESEARCH-RENODX-DLSS5.md §2.1-§2.3 (feature id, the DLSSNR.* parameter namespace, the
// SR-shaped contract) and §3.2 (the runtime must be LoadLibrary'd at device init).
//
// Why we are better placed than that addon: it Detours the NGX exports process-wide to graft
// itself onto a game-created DLSS feature (and in Stray reaches "HOOKS ARMED - NO DLSS CREATE
// SEEN" and does nothing, §0.3). We already OWN a working native-device NGX integration, so we
// create feature 18 directly, and ext_unhook already defuses the ReShade vtable trap that breaks
// descriptor handles on this stack.
//
// NO RR-style guide buffers are required: DLSS 5 infers materials from the frame itself
// (§2.2) — the input contract is the colour/depth/motion/jitter set our SR path already builds.
//
// SAFETY, because the runtime is a leak and the study warns it misbehaves: the NR result is
// written to OUR OWN texture, never straight over the engine's output. It is copied back only
// after a one-shot readback proves the neural output is not degenerate — the study records
// "the DLSSNR proxy output can be degenerate/empty for some HDR inputs" and the addon's own
// shader carries a `if (neural_y <= 1e-5) return original;` black-frame guard (§4.1). So a
// degenerate runtime costs us zero black frames: NR simply never engages, loudly.
//
// [STRAYDLSS] NgxNR=0 (default) is byte-identical to today.
#pragma once

#include <cstdint>

struct ID3D12Device;
struct ID3D12GraphicsCommandList;
struct ID3D12Resource;

namespace stray_dlss::nr {

// How feature 18 is wired. Both readings of the study are plausible, so both are reachable.
enum class Topology
{
	// (a) DEFAULT. Post-process on the already-upscaled image: Color = our SR/RR output
	// (output res), Output = our NR texture (output res, 1:1 subrects), Depth/MVec at render
	// res with MVecScale mapping them onto the output rect. This is the shape the study's
	// headline describes ("a post-process on top of the game's existing DLSS output", §0.1).
	post_process,
	// (b) SR-shaped: Color/Depth/MVec at render res -> Output at output res, i.e. feature 18
	// does the upscale itself, exactly mirroring the SR contract the addon snapshots off the
	// game (§2.3). Selectable because §2.3 and §0.1 read differently and only a live run can
	// settle which the runtime actually wants.
	sr_shaped,
};

void set_enabled(bool enabled);
bool enabled();
// Empty string = the default beside the game executable (where the operator staged it).
void set_dll_path(const char *utf8_path);
void set_topology(Topology topology);
void set_tuning(float intensity, float local_tone_strength, float local_structure_strength);
// Overrides the motion-vector scale handed to NR; <= 0 means "derive from the topology".
void set_mvec_scale_override(float scale);

// LoadLibrary the staged nvngx_dlssnr.dll. MUST be called at device init, BEFORE
// NVSDK_NGX_D3D12_Init_* (§3.2: the addon pre-loads it at device creation). Logs the exact
// path tried and whether the load succeeded — a missing or blocked DLL must be unmistakable.
// Returns false and records last_error() on failure; NR then stays off for the session.
bool load_runtime();

struct ApplyInputs
{
	// The image to improve: whatever our SR/RR evaluate just wrote (the engine's output
	// resource, u0). Under post_process this is NR's Color; under sr_shaped it is only the
	// copy-back destination and `render_color` is NR's Color.
	ID3D12Resource *image = nullptr;
	// Render-resolution scene colour — used as NR's Color under sr_shaped only.
	ID3D12Resource *render_color = nullptr;
	ID3D12Resource *depth = nullptr;          // render res, reversed-Z
	ID3D12Resource *motion_vectors = nullptr; // our dense RG16_FLOAT, render-res pixels

	std::uint32_t render_width = 0;
	std::uint32_t render_height = 0;
	std::uint32_t output_width = 0;
	std::uint32_t output_height = 0;
	bool reset = false; // camera cut, same signal SR gets
};

// Runs feature 18 and, once validated, copies the neural result back over `image`. Returns
// true only when the engine's image was actually replaced. Every failure path returns false,
// leaves the SR/RR image untouched, and counts a refusal reason.
bool apply(ID3D12Device *device, ID3D12GraphicsCommandList *cmd, const ApplyInputs &in);

// Drains the deferred validation readback. Call once per present.
void on_present();

void shutdown();

const char *last_error();

// Telemetry for the periodic report: how often NR replaced the image versus refused, and why.
constexpr int kNrRefusalCount = 7;
extern const char *const kNrRefusalNames[kNrRefusalCount];
void counters(std::uint64_t &applied, std::uint64_t &refused, std::uint32_t out[kNrRefusalCount]);
bool validated();

} // namespace stray_dlss::nr
