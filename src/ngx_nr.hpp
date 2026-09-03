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

// [STRAYDLSS] NgxNR, and the overlay checkbox. Callable from ANY thread, and 1 -> 0 destroys
// nothing on the caller's: it queues a teardown that on_present() executes once the GPU has
// passed the last evaluate. Flipping back to 1 before that teardown has run cancels it, so a
// toggle loop neither leaks nor frees anything in flight.
void set_enabled(bool enabled);
bool enabled();
// Empty string = the default beside the game executable (where the operator staged it).
void set_dll_path(const char *utf8_path);
void set_topology(Topology topology);
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

// The HDR colour codec's three knobs. The codec is NOT optional: feature 18 is a
// display-referred image network and we hook a raw linear HDR image, so without the
// encode/decode pair the network sees an out-of-domain signal and answers near-black (measured:
// neural output max luminance 0.0026 over the centre crop, red noise on screen). The runtime
// carries no HDR, colour-space or exposure parameter of its own — established by exhaustive
// string search over nvngx_dlssnr.dll — so the conversion has to be in our pixels. Math and
// provenance: src/core/nr_codec.hpp.
//
//  paper_white       [STRAYDLSS] NgxNRPaperWhiteScale, default 1.0. The post-exposure value
//                    treated as display white; the shader's multiplier is 1/paper_white
//                    (`calcProxyScale` in the reference tree). Values BELOW 1.0 are legal and
//                    are the likely direction here: Stray's scene colour at our hook point
//                    already carries UE4's pre-exposure (CLAUDE.md §2.6 row 135.y, ~0.056
//                    measured live), so it is already small, and our symptom is a near-black
//                    neural output. Raise it if the proxy looks blown out, lower it if the
//                    proxy looks black — and read the codec's own input/proxy/output luminance
//                    line before choosing, which is exactly what it is there for.
//  color_strength    [STRAYDLSS] NgxNRColorStrength, default 1.0. 0 keeps the ORIGINAL's
//                    chromaticity and transfers only the network's luminance change; 1 takes
//                    the network's colour too. Lower it for a colour cast.
//  transfer_strength [STRAYDLSS] NgxNRTransferStrength, default 1.0. A global lerp back toward
//                    the untouched original; 0 is an EXACT bypass, bit for bit.
void set_codec_tuning(float paper_white, float color_strength, float transfer_strength);

// [STRAYDLSS] NgxNRExposureSmoothing, default 0.05: per-frame weight of the new exposure sample
// when NgxNRTrackExposure is on. 1.0 disables smoothing (the old behaviour). NOT cosmetic — see
// nrc::smooth_exposure_factor: NR keeps its own temporal history, so a scale that moves frame to
// frame leaves that history in units the current proxy no longer matches.
void set_exposure_smoothing(float rate);

// [STRAYDLSS] NgxNRScaleResetTolerance, default 0.15: how far the codec scale may drift, as a
// RATIO, before NR's temporal history is discarded with a forced DLSSNR.Reset. The scale defines
// the display-referred units that history is accumulated in, so a change invalidates it as
// surely as a guide-grid change does. 0 disables the latch.
void set_scale_reset_tolerance(float tolerance);

// [STRAYDLSS] NgxNRTrackExposure, default ON — the reference's `trackAutoExposure`
// (rtx_neural_rendering.h:137-140), which defaults to true and which we dropped in the port.
//
// With it on, the codec's effective scale is the static proxy_scale(paperWhite) MULTIPLIED by the
// engine's OneOverPreExposure (View row 135.z), so the soft-clip knee follows scene brightness
// instead of sitting wherever a constant put it. Stray's scene colour at the TAA hook carries
// UE4's pre-exposure (measured 0.056 live), and the user hand-dialled NgxNRPaperWhiteScale to
// ~0.1 — an effective scale near 10x, the same order as 1/0.056 ~= 18. That is a person supplying
// this term manually, and it cannot be right in two differently-lit areas at once, because
// pre-exposure moves with the scene.
//
// TAA SITE ONLY. Post-tonemap the image is already display-referred with no pre-exposure left to
// undo, and the codec is bypassed there entirely. Math, clamps and the SR-exposure asymmetry:
// src/core/nr_codec.hpp above proxy_scale_tracked.
void set_track_exposure(bool enabled);

// WHERE the call comes from. One NR path, two call sites, and the difference between them is
// entirely the colour pipeline — so it is a parameter rather than a second copy of this module.
enum class Site
{
	// src/taa_hook.cpp, inside the intercepted TAA compute dispatch. `image` is the engine's
	// `u0`: raw, unbounded, PRE-EXPOSED LINEAR HDR, in D3D12_RESOURCE_STATE_UNORDERED_ACCESS
	// because the SR/RR evaluate has just written it. The HDR colour codec is MANDATORY here —
	// feature 18 is a display-referred network and this signal is out of its domain (measured:
	// neural output max luminance 0.0026, red noise on screen).
	taa_dispatch,
	// A post-tonemap site (REMOVED 2026-09-02 together with src/nr_hook.cpp; kept as the
	// codec-bypass case the codec tests pin).
	// `image` is a staging copy of the back buffer: ALREADY TONEMAPPED and display-referred, in
	// D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, and left in that state on exit.
	//
	// The HDR colour codec is BYPASSED here, deliberately and explicitly. The image is already in
	// the domain the network was trained on; encoding it a second time would apply the soft clip
	// and the sRGB curve on top of the game's own tone curve. The residual transfer has nothing
	// to carry either — with no encode there is no proxy to subtract — so on success the neural
	// answer is copied over `image` whole, which is correct precisely BECAUSE this site is
	// terminal: nothing downstream reads it back into the engine's temporal state.
	post_tonemap,
};

struct ApplyInputs
{
	// The image to improve. Under Site::taa_dispatch this is the engine's `u0` (and NR's Color
	// under post_process, or the copy-back destination under sr_shaped). Under
	// Site::post_tonemap it is our staging copy of the back buffer, and it is NR's Color
	// directly, with no codec in between.
	ID3D12Resource *image = nullptr;
	// Render-resolution scene colour — used as NR's Color under sr_shaped only.
	ID3D12Resource *render_color = nullptr;
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

	Site site = Site::taa_dispatch;

	// View row 135.z, `OneOverPreExposure`, already parsed by core/view_params.cpp. Used only by
	// the TAA site's codec, and only when NgxNRTrackExposure is on. <= 0 or non-finite means the
	// View constant buffer was not readable this frame, and the codec falls back to its static
	// scale rather than multiplying by a garbage value.
	float one_over_pre_exposure = 0.0f;
	// ue4::pre_exposure_plausible on the View CB this frame. False means keep the previous
	// smoothed exposure rather than walking it somewhere wrong on a stale read.
	bool pre_exposure_ok = false;

	// Colour/guide ratio for DLSSNR.MVecScaleX/Y. <= 0 means "derive from output/render", which
	// is what the TAA site does. The post-tonemap sites pass the ratio their own gate computed,
	// because their colour rect is the BACK BUFFER's, which is not necessarily the TAA output
	// rect the guides were sized against.
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

} // namespace stray_dlss::nr
