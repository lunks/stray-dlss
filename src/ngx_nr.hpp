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
struct ID3D12CommandQueue;
struct ID3D12Resource;

namespace stray_dlss::nr {


// [STRAYDLSS] NgxNR, and the overlay checkbox. Callable from ANY thread, and 1 -> 0 destroys
// nothing on the caller's: it queues a teardown that on_present() executes once the GPU has
// passed the last evaluate. Flipping back to 1 before that teardown has run cancels it, so a
// toggle loop neither leaks nor frees anything in flight.
void set_enabled(bool enabled);
bool enabled();
// Empty string = the default beside the game executable (where the operator staged it).
void set_dll_path(const char *utf8_path);
void set_tuning(float intensity, float local_tone_strength, float local_structure_strength);

// The rest of what RenoDX sets and we did not. Names verified present in the 310.8.0 runtime by
// exact string search; RenoDX's seven other names (InputWidth/Height, OutputWidth/Height,
// Output.Width/Height, Upscaling) are ABSENT from this build and are deliberately not written.
// Defaults are RenoDX's own shipped [RenoDX.DLSS5] values.
void set_renodx_tuning(float skin_structure_strength, unsigned int preset,
	unsigned int use_auto_mask, unsigned int ui_correction);
// Overrides the motion-vector scale handed to NR; <= 0 means "derive from the topology".
void set_mvec_scale_override(float scale);

// DLSSNR.Style — confirmed present in the 310.8.0 runtime by exact string search (one
// occurrence, "DLSSNR.Style", docs/RESEARCH-DLSSNR-STYLES.md), and independently written by two
// other DLSSNR callers (RenoDX and the OptiScaler_DLSSNR fork). It is NOT DLSSNR.Hint.Render.
// Preset — that parameter selects a different TRAINED WEIGHT SET (the runtime logs "preset=%d ->
// %s" against embedded config codenames like WEIGHTS_HT); Style is a small integer with no
// weight-set switch behind it. 0/1/2 only (values above 2 are meaningless to us; the OptiScaler
// fork clamps its own UI the same way). Community-tested labels — "Default (standard)", "Natural"
// and "Cinematic" for 0/1/2 respectively — are the ONLY names attached to these values anywhere
// we have found; the runtime binary itself carries none (docs/RESEARCH-DLSSNR-STYLES.md). Default
// here is 0, matching what an unset parameter has always behaved as, so NgxNRStyle=0 changes
// nothing from today.
//
// Style is in the runtime's own control-change list (docs/RESEARCH-RENODX-DLSS5.md's "DLSSNR's
// structure controls" section), so changing it forces the snippet to wipe feature 18's temporal
// history for one frame — judge the result a second or two after moving this, never on the frame
// it changes.
void set_style(unsigned int style);

// [STRAYDLSS] NgxNRPreload (default ON): LoadLibrary the staged nvngx_dlssnr.dll, resolve its
// exports and patch its GetModuleFileNameW import, at device init. This is the CHEAP half only
// — it never calls Init_Ext and never touches the GPU. It matches RenoDX's own description,
// "signed NR runtime (nvngx_dlssnr.dll) pre-loaded at device init"
// (docs/RESEARCH-RENODX-DLSS5.md §3.2). Logs the exact path tried and whether the load
// succeeded — a missing or blocked DLL must be unmistakable. Returns false and records
// last_error() on failure; NR then stays off for the session.
//
// Why the split, and why init is lazy: initialising a leaked pre-release NGX snippet during
// D3D12 device creation is both a measurable startup stall (a 165 MB DLL plus a GPU-touching
// init) and the leading suspect for two measured GPU losses (GPU_IS_LOST, host power-cycle
// each time). Device-init is the least settled moment in the D3D12 lifecycle; RenoDX's own
// string says only that the runtime is "pre-loaded" there, never that it is INITIALISED there.
// Deferring the init to a demonstrably healthy steady state is strictly safer and costs
// nothing, so device init does the memory work only and never contacts the GPU.
bool preload();

// How many successful SR/RR evaluates must pass before the NR runtime is initialised
// ([STRAYDLSS] NgxNRWarmupFrames, default 60; set 0 to reproduce RenoDX exactly). apply() is
// only reached after a successful evaluate, so this counts exactly the frames in which the
// device, queue and swapchain have demonstrably worked.
void set_warmup_frames(unsigned int frames);






struct ApplyInputs
{
	// The image to improve: our staging copy of the back buffer. It is NR's Color directly —
	// display-referred already, so nothing sits between it and the network.
	ID3D12Resource *image = nullptr;
	ID3D12Resource *depth = nullptr;          // render res, reversed-Z
	ID3D12Resource *motion_vectors = nullptr; // our dense RG16_FLOAT, render-res pixels

	std::uint32_t render_width = 0;
	std::uint32_t render_height = 0;
	std::uint32_t output_width = 0;
	std::uint32_t output_height = 0;
	// Camera cut, the same OR of three signals SR gets (CLAUDE.md §2.8: View.CameraCut row
	// 145.x, TemporalAAJitter.zw == .xy, or a 1x1 history/velocity SRV). Feature 18 keeps its OWN
	// temporal accumulation — it consumes motion vectors and depth, and DLSSNR.Reset is
	// `settings.resetAccumulation` in the reference — so this must reach it at BOTH sites. A
	// camera cut that does not reset NR's history is the "flicker between a frozen image and fog"
	// class of bug this project has already been bitten by once.
	bool reset = false;


	// View row 135.z, `OneOverPreExposure`, already parsed by core/view_params.cpp. Used only by
	// the TAA site's codec, and only when NgxNRTrackExposure is on. <= 0 or non-finite means the
	// View constant buffer was not readable this frame, and the codec falls back to its static
	// scale rather than multiplying by a garbage value.
	float one_over_pre_exposure = 0.0f;
	// ue4::pre_exposure_plausible on the View CB this frame. False means keep the previous
	// smoothed exposure rather than walking it somewhere wrong on a stale read.
	bool pre_exposure_ok = false;

	// Colour/guide ratio for DLSSNR.MVecScaleX/Y. <= 0 means "derive from the topology", which is
	// what BOTH sites pass today and which reaches 1.0 — the value the user's own live A/B settled
	// on (CLAUDE.md, 2026-09-01), overturning a code-reading argument for the ratio. The present
	// stage's colour rect is the BACK BUFFER's, which is not necessarily the TAA output rect the
	// guides were sized against, so its gate COMPUTES the ratio and reports it in the periodic NR
	// STAGE line rather than sending it: the two numbers can then be compared on the box before
	// either is changed.
	float mvec_scale_x = 0.0f;
	float mvec_scale_y = 0.0f;
};

// Runs feature 18 and, once validated, copies the neural result back over `image`. Returns
// true only when the engine's image was actually replaced. Every failure path returns false,
// leaves the SR/RR image untouched, and counts a refusal reason.
bool apply(ID3D12Device *device, ID3D12GraphicsCommandList *cmd, const ApplyInputs &in);

// THE PRESENT BOUNDARY, and the only place anything NR owns is ever destroyed.
//
// `queue` is the swapchain's own command queue (icept::PresentContext::queue) and may be null
// under a host that does not report one. It is used for exactly one thing: signalling our own
// fence, so that "the CPU stopped using this" and "the GPU stopped reading it" can be told apart.
// Every deferred free — the neural output texture on a resolution change, the codec's proxy and
// descriptor heap on an NgxNR 1->0 toggle, the validation readback buffers, and ReleaseFeature
// itself — happens here and only once that fence has passed the last evaluate that used the
// resource. With no queue the decision falls back to a conservative present ring
// (core/nr_lifetime.hpp).
//
// Also drains the deferred validation readback. MUST be called every present, INCLUDING while NR
// is disabled: the disable is what queues the teardown.
void on_present(ID3D12CommandQueue *queue);

void shutdown();

const char *last_error();

// Telemetry for the periodic report: how often NR replaced the image versus refused, and why.
// NOTE: the count is duplicated in src/ngx_nr.cpp's kNrRefusalNames — change both together.
constexpr int kNrRefusalCount = 15;
extern const char *const kNrRefusalNames[kNrRefusalCount];
void counters(std::uint64_t &applied, std::uint64_t &refused, std::uint32_t out[kNrRefusalCount]);
bool validated();

// WHY EACH DLSSNR.Reset HAPPENED, by source.
//
// A reset discards feature 18's entire temporal accumulation, so a reset SOURCE that fires often
// is itself a flicker source — the accumulation never gets long enough to converge. This project
// has already been caught by exactly that once: a reset latch put on a continuously varying
// quantity fired 52 times and made the image worse (CLAUDE.md, "there it is a metronome").
//
// `frame_gap` is the one that still follows a continuous quantity — how reliably the TAA hook
// matches — so whether it is a rarity or a metronome is an empirical question. This is the
// counter that answers it in one session instead of one argument per round trip.
struct ResetCounts
{
	std::uint32_t frame_gap = 0;   // a frame NR declined, or was never asked about at all
	std::uint32_t guide_grid = 0;  // the render resolution moved under a fixed output rect
	std::uint32_t camera_cut = 0;  // the engine's own cut signal (CLAUDE.md §2.8)
	std::uint32_t new_feature = 0; // the first evaluate against a freshly created feature
};
ResetCounts reset_counters();

} // namespace stray_dlss::nr
