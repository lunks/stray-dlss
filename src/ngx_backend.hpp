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

struct EvaluateInputs
{
	ID3D12Resource *color = nullptr;           // render-res scene colour, still pre-exposed
	ID3D12Resource *depth = nullptr;           // render-res depth, reversed-Z
	ID3D12Resource *motion_vectors = nullptr;  // our dense RG16_FLOAT field
	ID3D12Resource *output = nullptr;          // output-res UAV, ALLOW_UNORDERED_ACCESS

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
	// View.TranslatedWorldToView (rows 8-11 — DERIVED layout, caller must have passed
	// world_to_view_rotation_plausible); ViewToClip is ViewToClipNoAA (row 32, measured —
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
