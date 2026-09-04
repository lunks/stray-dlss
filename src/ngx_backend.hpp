// NGX lifecycle. Skeleton stage: initialise, query capability, report, shut down.
// Feature creation and evaluation land in a later stage.
//
// Rules that must not be violated (docs/RESEARCH.md §3.7):
//   * NGX is NOT thread safe. Every call here happens on the thread that owns the ReShade
//     device/command-list callbacks.
//   * Initialise with the ORIGINAL vkd3d ID3D12Device from device::get_native(), never a
//     ReShade proxy.
//   * Do NOT gate availability on NVSDK_NGX_D3D12_GetFeatureRequirements — Proton's
//     _nvngx.dll does not implement it and reports unsupported even when DLSS works.
//   * Teardown order, GPU idle first: ReleaseFeature -> DestroyParameters -> Shutdown1.
#pragma once

#include "core/exposure_plan.hpp"

struct ID3D12Device;
struct ID3D12Resource;
struct ID3D12GraphicsCommandList;

namespace stray_dlss::ngx {

struct Status
{
	bool initialised = false;
	bool super_sampling_available = false;
	bool needs_updated_driver = false;
	unsigned int min_driver_major = 0;
	unsigned int min_driver_minor = 0;
	unsigned int init_result = 0;      // raw NVSDK_NGX_Result
	unsigned int feature_init_result = 0;
};

// [STRAYDLSS] NgxSnippetPath: an extra directory for NGX to search for feature DLLs
// ("snippets"). Must be set BEFORE initialise(). Empty = auto (the game executable's
// directory, then our own add-on's directory).
//
// Why this exists: NGX resolves its snippets INDEPENDENTLY of anything we LoadLibrary. The
// driver ships nvngx_dlss/nvngx_dlssd, so SR and RR resolve by default — but a
// side-loaded snippet like nvngx_dlssnr.dll exists only in the game directory, and if NGX
// does not search there, feature creation fails with FAIL_OutOfDate no matter what we have
// loaded into the process. NVSDK_NGX_FeatureCommonInfo::PathListInfo is the documented
// mechanism (nvsdk_ngx.h:133-135: "a list of paths where feature DLLs can be located, other
// than the default path (application directory)"). There is NO per-feature path override in
// the SDK — Snippet.OptLevel / Snippet.IsDevBranch are read-back diagnostics only — so this
// init-time list is the only lever.
void set_snippet_path(const char *utf8_path);

// Returns the status either way; a failed init is a reportable outcome, not an exception.
Status initialise(ID3D12Device *device);
void shutdown(ID3D12Device *device);

const Status &status();

// Human-readable NVSDK_NGX_Result. Results are a BITMASK over 0xBAD00000 — never compare
// against Success directly, use the NVSDK_NGX_SUCCEED/FAILED macros. (docs/RESEARCH.md §3.7)
const char *result_name(unsigned int result);

// --- feature lifecycle ---

struct FeatureDesc
{
	unsigned int render_width = 0;
	unsigned int render_height = 0;
	unsigned int output_width = 0;
	unsigned int output_height = 0;
};

// Creates the DLSS feature, or recreates it when the sizes change. Every output-resolution
// change needs a full ReleaseFeature + CreateFeature (CLAUDE.md §2.1), so this compares the
// requested description against the live one and rebuilds only on a real change.
//
// A RE-creation is additionally DEBOUNCED (`src/core/feature_recreate.hpp`): a differing rect
// must be asked for `NgxRecreateStableFrames` frames running before anything is torn down, and
// meanwhile this returns false so the engine's own TAA renders the frame. That is what stops a
// scripted scene transition — whose view rect animates every few frames — recreating the
// feature six to ten times in a second, each recreation a lost temporal history and a full
// CreateFeature stall. The FIRST creation is never debounced.
//
// Returns false and records last_error() on failure; the caller must then not evaluate.
bool ensure_feature(ID3D12GraphicsCommandList *cmd, const FeatureDesc &desc);

// [STRAYDLSS] NgxRecreateStableFrames. 0 restores the pre-2026-09-03 behaviour (recreate on the
// first differing frame) so the box can A/B it without a rebuild.
void set_recreate_stable_frames(unsigned int frames);

// The rect the LIVE feature was created for, or all-zero if there is none. The caller needs it
// to decide whether a differing request can be served by evaluating at THIS extent instead of
// rebuilding (`core::plan_letterbox_hold`) - which is what keeps DLSS SR, and therefore NR,
// running through a letterbox slide.
FeatureDesc live_feature_desc();

// FORCE `InReset` ON THE NEXT EVALUATE. DLSS reprojects its history assuming the motion vectors
// describe exactly ONE frame of motion, so resuming after a gap - the debounce declining a run
// of frames, or a fresh feature - reprojects across frames it never saw. That is the same error
// class as a wrong MVecScale (CLAUDE.md §5) and it compounds rather than costing one frame. The
// latch is set internally on a decline and on a creation; this is for callers that know of a
// gap the backend cannot see.
void force_reset_next_evaluate();

// How many evaluates carried a FORCED reset (a gap or a fresh feature), as opposed to a camera
// cut. It must be small: a rate that tracks the frame count means DLSS is being told to discard
// its history continuously, which looks like DLSS doing nothing at all.
unsigned long long forced_reset_count();

// `waits` counts frames declined by the debounce, `restarts` the times the requested rect
// changed AGAIN mid-debounce. A large `restarts` with `waits` climbing and no creations is an
// ANIMATING rect — the scripted-transition signature — and is the debounce doing its job.
void recreate_counters(unsigned long long &waits, unsigned long long &restarts);

// [STRAYDLSS] NgxExposure, as parsed by exposure::parse_mode:
//   auto    — the AutoExposure create flag, DLSS estimates exposure itself (default, and the
//             official plugin's own default: r.NGX.DLSS.AutoExposure defaults to 1).
//   texture — the flag is dropped and the ENGINE's eye-adaptation texture (TAA t0) is passed.
//   owned   — the flag is dropped and OUR OWN 1x1 R32_FLOAT is passed, carrying View row
//             135.y times [STRAYDLSS] NgxExposureValue. See src/exposure_texture.hpp for why
//             owning the resource is what makes the experiment decidable.
//
// CREATION-TIME property (the flag is), read once at init and A/B'd across launches.
//
// READ THE PRESET GATE BEFORE USING EITHER TEXTURE MODE. DLSS Programming Guide 310.6.0 §3.9:
// "Only supported by Presets J and K. Preset L always uses AutoExposure." Everything below is
// inert on a preset outside {J, K}, and the driver default resolves to M at this title's
// shipped 50% screen percentage. exposure::exposure_will_be_ignored() detects that combination
// and dlss_app warns at init.
void set_exposure_mode(exposure::Mode mode);
exposure::Mode exposure_mode();

// [STRAYDLSS] NgxExposureScale (default 1.0): InExposureScale on the SR evaluate under either
// texture mode. nvsdk_ngx_helpers.h:508 forwards it as DLSS.Exposure.Scale, with 0 rewritten
// to 1.0.
//
// DEMOTED FROM "the definitive consume test", deliberately, for two independent reasons.
// DLSS.Exposure.Scale has ZERO explanatory prose in the entire 84-page Programming Guide
// (revision 310.6.0) — the name appears only in the parameter listing, and whether it
// multiplies or divides into the texture's value is UNCONFIRMED from any primary source. AND
// the official UE plugin NEVER SETS IT: a repo-wide search of the plugin tree for
// `InExposureScale` and `ExposureScale` returns zero hits, so the field is left at whatever
// FMemory::Memzero left it (0, which helpers.h:508 rewrites to 1.0). The reference integration
// carries exposure through InPreExposure and the texture alone. A null result from a sweep is
// therefore not evidence about the exposure TEXTURE. NgxExposureValue under NgxExposure=owned
// is the sound consume test, because it perturbs the one quantity the guide does document.
void set_exposure_scale(float scale);
float exposure_scale();

// [STRAYDLSS] NgxExposureValue (default 1.0): multiplies the value written into the OWNED
// exposure texture. 1.0 is the honest value (the engine's own exposure). Anything else is the
// deliberately-wrong number of the consume test — if 0.25 and 4.0 produce the same image, the
// runtime is not reading the texture.
void set_exposure_value_multiplier(float multiplier);
float exposure_value_multiplier();

struct EvaluateInputs
{
	ID3D12Resource *color = nullptr;           // render-res scene colour, still pre-exposed
	ID3D12Resource *depth = nullptr;           // render-res depth, reversed-Z
	ID3D12Resource *motion_vectors = nullptr;  // our dense RG16_FLOAT field
	ID3D12Resource *output = nullptr;          // output-res UAV, ALLOW_UNORDERED_ACCESS
	// The exposure texture for this frame — the engine's eye-adaptation buffer (TAA
	// register t0) under NgxExposure=texture, our own 1x1 R32_FLOAT under
	// NgxExposure=owned, null under auto.
	//
	// SHAPE, HARD from UE 4.27.2's own source: eye adaptation is allocated
	// `Create2DDesc(FIntPoint(1, 1), PF_A32B32G32R32F, ...)`
	// (PostProcessEyeAdaptation.cpp:820) — 1x1 R32G32B32A32_FLOAT, exactly what
	// find_eye_adaptation_srv looks for and exactly what CLAUDE.md §2.3 measured at t0.
	//
	// FORMAT is not a constraint: DLSS Programming Guide 310.6.0 §3.9, "Only the first
	// channel is sampled in the texture so multiple formats will work but something such
	// as R16F is preferred". So the engine's RGBA32F and our R32_FLOAT are both legal.
	//
	// STATE: §3.4 requires D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, listing
	// exposure in the same input class as colour, depth and motion vectors.
	//
	// THE ONE PLACE WE DIVERGE FROM THE REFERENCE, and it is why `owned` exists. The
	// official plugin does not assume that state, it TRANSITIONS to it — the same
	// GetResidentD3D12Resource helper that resolves pInExposureTexture calls
	// `RHITransitionResource(CmdList, InTexture, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_
	// RESOURCE, ALL_SUBRESOURCES)` for every input texture (NGXD3D12RHI.cpp, plugin
	// v8.3.0-NGX310.4.0). UE's D3D12 RHI tracks the real current state, so that
	// transition is correct rather than guessed. WE CANNOT DO THAT for the game's
	// texture: D3D12 exposes no way to query a resource's state, and a barrier with a
	// guessed StateBefore is the exact hazard CLAUDE.md §5 documents. Under
	// NgxExposure=texture we therefore rely on the game's own dispatch having left t0 in
	// that state (true, and the same reliance we already place on colour and depth);
	// under NgxExposure=owned we transition it ourselves, which is the reference's
	// discipline reproduced rather than approximated.
	// VALUE SEMANTICS, the load-bearing point: texel .x is UE's smoothed exposure
	// multiplier — `OutColor.x = MiddleGreyExposureCompensation * SmoothedExposureScale`
	// (PostProcessEyeAdaptation.usf:95-112, 4.27.2 mirror, fetched 2026-08-31) — the value
	// the tonemapper multiplies scene colour by. The official
	// plugin passes this texture UNMODIFIED while ALSO passing View.PreExposure as
	// InPreExposure in BOTH exposure modes (DLSSUpscaler.cpp:1111-1115) — no reciprocal,
	// no combining, no double-apply at the integration layer: NGX separates the two
	// internally (pre-exposure names what is baked into the colour buffer; the exposure
	// texture names the tonemapper's multiplier). We reproduce that exactly: this texture
	// straight through, pre_exposure (row 135.y) straight through, unchanged in both modes.
	ID3D12Resource *exposure = nullptr;

	// TemporalAAParams.zw, passed straight through: already render-resolution pixels in
	// [-0.5, +0.5]. No negation, no scaling. (CLAUDE.md §2.7)
	float jitter_x = 0.0f;
	float jitter_y = 0.0f;

	unsigned int render_width = 0;
	unsigned int render_height = 0;
	bool reset = false;                        // camera cut / invalid history
	float pre_exposure = 1.0f;
	// ue4::pre_exposure_plausible on the View CB this frame: row 135 must read
	// (denormal, P, 1/P, 0.0) with P*(1/P) == 1 exactly (CLAUDE.md §2.6). FALSE means we do
	// not know the exposure, and InPreExposure is then sent as a deliberate 1.0 rather than
	// as whatever the row happened to hold. The NR path has always gated this row; the SR
	// path did not, which let a misread reach DLSS silently.
	bool pre_exposure_ok = true;
};

bool evaluate(ID3D12GraphicsCommandList *cmd, const EvaluateInputs &in);

// Releases the feature. The GPU must already be idle for it.
// [STRAYDLSS] NgxPreset: 0=driver default, 10=J, 11=K (default), 12=L, 13=M. J is often
// calmer on specular sparkle. Takes effect at the next feature (re)creation.
void set_preset(int preset);

// The preset hint currently configured (0 = driver default). Needed because exposure input is
// PRESET-GATED: DLSS Programming Guide 310.6.0 §3.9, "Only supported by Presets J and K.
// Preset L always uses AutoExposure."
int preset();

void release_feature();

// The most recent failure, for reporting without another round trip.
const char *last_error();

// --- Ray Reconstruction (DLSSD, NVSDK_NGX_Feature_RayReconstruction = 13) ---
//
// Staged behind [STRAYDLSS] NgxRR (0 = off, SR unchanged; 1 = PROBE; 2 = FULL). The probe
// answers "does DLSSD exist under vkd3d on this stack" in one run: it reads the
// SuperSamplingDenoising.* capability keys (nvsdk_ngx_defs_dlssd.h — the DLSSD siblings
// of the SR availability keys; NOT GetFeatureRequirements, which Proton does not
// implement) and attempts one NGX_D3D12_CREATE_DLSSD_EXT on the same command-list path SR
// feature creation uses, releasing the throwaway feature once the GPU has certainly
// executed its creation work. SR keeps running throughout.

struct RRStatus
{
	bool available = false;             // SuperSamplingDenoising.Available
	bool needs_updated_driver = false;
	unsigned int min_driver_major = 0;
	unsigned int min_driver_minor = 0;
	unsigned int feature_init_result = 0; // SuperSamplingDenoising.FeatureInitResult
	bool probed = false;                // the create probe has run (either way)
	bool probe_create_ok = false;
	unsigned int probe_create_result = 0;
};

void set_rr_mode(int mode); // 0 off, 1 probe, 2 full — set once at init, like set_preset
int rr_mode();
const RRStatus &rr_status();

// Creates (or size-recreates) the RR feature. Same contract as ensure_feature, plus a
// failure latch: a description that failed to create is not retried every frame — the
// caller falls back to SR and the log says so once.
bool ensure_feature_rr(ID3D12GraphicsCommandList *cmd, const FeatureDesc &desc);

struct EvaluateInputsRR
{
	// The SR input set, identical semantics (colour/depth/MV/output, jitter straight from
	// TemporalAAParams.zw, render rect, reset, pre-exposure).
	EvaluateInputs base;

	// The four RR guides. Their producer (gbuffer_resolve) was deleted 2026-09-03 with the
	// heuristic finder; these formats are the contract a future engine-sourced guide pass must
	// meet, and nothing on the SR/NR/FG path touches them:
	ID3D12Resource *diffuse_albedo = nullptr;    // RGBA8_UNORM, linear
	ID3D12Resource *specular_albedo = nullptr;   // RGBA8_UNORM, linear
	ID3D12Resource *normals_roughness = nullptr; // RGBA16F, xyz signed world normal
	ID3D12Resource *roughness = nullptr;         // R16F (Roughness_Mode_Unpacked reads this)

	// Row-major matrices for the DLSSD helper's void-pointer parameters. WorldToView is
	// View.TranslatedWorldToView as stored (rows 12-15, mirror-verified; caller must have
	// passed world_to_view_rotation_plausible — NGX's expected convention is undocumented,
	// so the matrix goes through unmodified and only affects the specular-MV path we do
	// not feed); ViewToClip is ViewToClipNoAA (row 32, measured —
	// jitter reaches NGX separately, so the unjittered projection is the consistent one).
	// Optional: the official UE plugin ships NEITHER matrix, so absence only degrades
	// reflection reprojection, never the evaluate.
	float world_to_view[16] = {};
	float view_to_clip[16] = {};
	bool have_matrices = false;

	// View.DeltaTime in milliseconds, when known; the header says it helps the DLL scale
	// denoising with object speed. 0 = not provided.
	float frame_time_delta_ms = 0.0f;
};

bool evaluate_rr(ID3D12GraphicsCommandList *cmd, const EvaluateInputsRR &in);

void release_feature_rr();

} // namespace stray_dlss::ngx
