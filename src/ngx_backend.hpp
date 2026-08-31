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
// Returns false and records last_error() on failure; the caller must then not evaluate.
bool ensure_feature(ID3D12GraphicsCommandList *cmd, const FeatureDesc &desc);

// [STRAYDLSS] NgxExposure: false = "auto" (today's behaviour — the feature is created
// with NVSDK_NGX_DLSS_Feature_Flags_AutoExposure and DLSS estimates exposure itself);
// true = "texture" (the flag is dropped and the engine's own eye-adaptation texture is
// passed as pInExposureTexture at every evaluate). CREATION-TIME property: read once at
// init, A/B'd across launches. The official UE plugin implements exactly this pair
// (r.NGX.DLSS.AutoExposure; NGXRHI.cpp:537-546 sets the flag from it, NGXD3D12RHI.cpp:
// 267-269 nulls the texture under auto — v3.7.3 mirror, fetched 2026-08-31).
void set_exposure_from_texture(bool use_texture);
bool exposure_from_texture();

// [STRAYDLSS] NgxExposureScale (default 1.0): the InExposureScale passed to the SR
// evaluate under NgxExposure=texture — DLSS multiplies the exposure-texture value by this
// (nvsdk_ngx_helpers.h:508). The DEFINITIVE consume test: the on-screen indicator's
// "Auto Exposure" text is ambiguous, so feed a deliberately wrong scale (0.25 / 4.0) and
// watch the image — a brightness/bright-region response proves DLSS reads our exposure;
// no response at either extreme proves the texture never reaches DLSS's math. 1.0 is
// behaviourally identical to today (the helper maps our previous 0 to 1.0 anyway). Read
// once at init; only meaningful under NgxExposure=texture.
void set_exposure_scale(float scale);
float exposure_scale();

struct EvaluateInputs
{
	ID3D12Resource *color = nullptr;           // render-res scene colour, still pre-exposed
	ID3D12Resource *depth = nullptr;           // render-res depth, reversed-Z
	ID3D12Resource *motion_vectors = nullptr;  // our dense RG16_FLOAT field
	ID3D12Resource *output = nullptr;          // output-res UAV, ALLOW_UNORDERED_ACCESS
	// The engine's eye-adaptation texture (TAA register t0: 1x1 RGBA32F), consumed only
	// under NgxExposure=texture; null falls back to DLSS's default exposure of 1.0 for
	// that frame (the helper's InExposureScale 0->1 mapping, nvsdk_ngx_helpers.h:508).
	// VALUE SEMANTICS, the load-bearing point: texel .x is UE's smoothed exposure
	// multiplier — `OutColor.x = MiddleGreyExposureCompensation * SmoothedExposureScale`
	// (PostProcessEyeAdaptation.usf:95-112, 4.27.2 mirror, fetched 2026-08-31) — the value
	// the tonemapper multiplies scene colour by. The official
	// plugin passes this texture UNMODIFIED while ALSO passing View.PreExposure as
	// InPreExposure in BOTH exposure modes (DLSSUpscaler.cpp:1085-1089) — no reciprocal,
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
};

bool evaluate(ID3D12GraphicsCommandList *cmd, const EvaluateInputs &in);

// Releases the feature. The GPU must already be idle for it.
// [STRAYDLSS] NgxPreset: 0=driver default, 10=J, 11=K (default), 12=L, 13=M. J is often
// calmer on specular sparkle. Takes effect at the next feature (re)creation.
void set_preset(int preset);

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

	// The four RR guides, from gbuffer_resolve's outputs (formats documented there):
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
