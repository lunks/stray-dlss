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

} // namespace stray_dlss::ngx
