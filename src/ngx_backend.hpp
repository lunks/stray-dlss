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

} // namespace stray_dlss::ngx
