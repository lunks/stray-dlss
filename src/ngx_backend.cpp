#include "ngx_backend.hpp"

#include "log.hpp"

#include <d3d12.h>

#if !defined(STRAY_DLSS_ENABLE_NGX)
#define STRAY_DLSS_ENABLE_NGX 1
#endif

#if !STRAY_DLSS_ENABLE_NGX

// Diagnostic build: NGX is not linked at all. Everything else in the add-on is identical, so
// if this variant loads and the NGX one does not, the static NGX library's global
// constructors are what fault at DLL entry.
namespace stray_dlss::ngx {

namespace { Status g_status; }

const char *result_name(unsigned int) { return "<ngx disabled>"; }
const Status &status() { return g_status; }

Status initialise(ID3D12Device *)
{
	g_status = Status{};
	STRAY_LOG_WARN("NGX is NOT linked in this build (diagnostic variant).");
	return g_status;
}

void shutdown(ID3D12Device *) { g_status = Status{}; }

} // namespace stray_dlss::ngx

#else

#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers.h>

namespace stray_dlss::ngx {
namespace {

// A self-generated GUID. NVIDIA does NOT need to register this: Init_with_ProjectID plus
// NVSDK_NGX_ENGINE_TYPE_CUSTOM works for anyone, and there is no application whitelist.
// (docs/RESEARCH.md §3.7)
constexpr const char *kProjectId = "6f2d1c84-9b3e-4a17-8e55-1d0c7a3f6b92";
constexpr const char *kEngineVersion = "0.1.0";

Status g_status;
NVSDK_NGX_Parameter *g_capability_params = nullptr;

// NGX's own logs are far more informative than our inference about why it failed, so route
// them into the same sink the user will paste back. (docs/RESEARCH.md §3.7)
void NVSDK_CONV ngx_log_callback(const char *message, NVSDK_NGX_Logging_Level level, NVSDK_NGX_Feature source)
{
	(void)source;
	const auto mapped = (level == NVSDK_NGX_LOGGING_LEVEL_VERBOSE) ? log::Level::debug : log::Level::info;
	log::writef(mapped, "  ngx: %s", message);
}

} // namespace

const char *result_name(unsigned int result)
{
	switch (static_cast<NVSDK_NGX_Result>(result))
	{
	case NVSDK_NGX_Result_Success:                        return "Success";
	case NVSDK_NGX_Result_Fail:                           return "Fail";
	case NVSDK_NGX_Result_FAIL_FeatureNotSupported:       return "FAIL_FeatureNotSupported";
	case NVSDK_NGX_Result_FAIL_PlatformError:             return "FAIL_PlatformError";
	case NVSDK_NGX_Result_FAIL_FeatureAlreadyExists:      return "FAIL_FeatureAlreadyExists";
	case NVSDK_NGX_Result_FAIL_FeatureNotFound:           return "FAIL_FeatureNotFound";
	case NVSDK_NGX_Result_FAIL_InvalidParameter:          return "FAIL_InvalidParameter";
	case NVSDK_NGX_Result_FAIL_ScratchBufferTooSmall:     return "FAIL_ScratchBufferTooSmall";
	case NVSDK_NGX_Result_FAIL_NotInitialized:            return "FAIL_NotInitialized";
	case NVSDK_NGX_Result_FAIL_UnsupportedInputFormat:    return "FAIL_UnsupportedInputFormat";
	case NVSDK_NGX_Result_FAIL_RWFlagMissing:             return "FAIL_RWFlagMissing";
	case NVSDK_NGX_Result_FAIL_MissingInput:              return "FAIL_MissingInput";
	case NVSDK_NGX_Result_FAIL_UnableToInitializeFeature: return "FAIL_UnableToInitializeFeature";
	case NVSDK_NGX_Result_FAIL_OutOfDate:                 return "FAIL_OutOfDate";
	case NVSDK_NGX_Result_FAIL_OutOfGPUMemory:            return "FAIL_OutOfGPUMemory";
	case NVSDK_NGX_Result_FAIL_UnsupportedFormat:         return "FAIL_UnsupportedFormat";
	case NVSDK_NGX_Result_FAIL_UnableToWriteToAppDataPath:return "FAIL_UnableToWriteToAppDataPath";
	case NVSDK_NGX_Result_FAIL_UnsupportedParameter:      return "FAIL_UnsupportedParameter";
	case NVSDK_NGX_Result_FAIL_Denied:                    return "FAIL_Denied";
	case NVSDK_NGX_Result_FAIL_NotImplemented:            return "FAIL_NotImplemented";
	default:                                              return "<unknown>";
	}
}

const Status &status() { return g_status; }

Status initialise(ID3D12Device *device)
{
	g_status = Status{};

	if (device == nullptr)
	{
		STRAY_LOG_ERROR("NGX init skipped: no D3D12 device");
		return g_status;
	}

	NVSDK_NGX_LoggingInfo logging_info = {};
	logging_info.LoggingCallback = ngx_log_callback;
	logging_info.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_ON;
	logging_info.DisableOtherLoggingSinks = false;

	NVSDK_NGX_FeatureCommonInfo common_info = {};
	common_info.LoggingInfo = logging_info;

	const NVSDK_NGX_Result init_result = NVSDK_NGX_D3D12_Init_with_ProjectID(
		kProjectId,
		NVSDK_NGX_ENGINE_TYPE_CUSTOM,
		kEngineVersion,
		L".",
		device,
		&common_info,
		NVSDK_NGX_Version_API);

	g_status.init_result = static_cast<unsigned int>(init_result);

	if (NVSDK_NGX_FAILED(init_result))
	{
		STRAY_LOG_ERROR("NVSDK_NGX_D3D12_Init_with_ProjectID failed: 0x%08x (%s)",
			g_status.init_result, result_name(g_status.init_result));
		STRAY_LOG_ERROR("  On Proton this usually means nvngx.dll/_nvngx.dll are missing from the");
		STRAY_LOG_ERROR("  prefix system32, the HKLM NGXCore\\FullPath key is absent, or nvidia_uvm");
		STRAY_LOG_ERROR("  is not loaded (see docs/RESEARCH.md section 1.3).");
		return g_status;
	}

	g_status.initialised = true;
	STRAY_LOG_INFO("NVSDK_NGX_D3D12_Init_with_ProjectID succeeded");

	const NVSDK_NGX_Result caps_result = NVSDK_NGX_D3D12_GetCapabilityParameters(&g_capability_params);
	if (NVSDK_NGX_FAILED(caps_result) || g_capability_params == nullptr)
	{
		STRAY_LOG_ERROR("NVSDK_NGX_D3D12_GetCapabilityParameters failed: 0x%08x (%s)",
			static_cast<unsigned int>(caps_result), result_name(static_cast<unsigned int>(caps_result)));
		return g_status;
	}

	// Deliberately NOT gated on NVSDK_NGX_D3D12_GetFeatureRequirements: Proton's _nvngx.dll
	// does not implement it and reports unsupported even when DLSS works fine.
	int available = 0;
	g_capability_params->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &available);
	g_status.super_sampling_available = available > 0;

	int needs_driver = 0;
	g_capability_params->Get(NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needs_driver);
	g_status.needs_updated_driver = needs_driver > 0;

	unsigned int major = 0, minor = 0;
	g_capability_params->Get(NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMajor, &major);
	g_capability_params->Get(NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMinor, &minor);
	g_status.min_driver_major = major;
	g_status.min_driver_minor = minor;

	unsigned int feature_init = 0;
	g_capability_params->Get(NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult, &feature_init);
	g_status.feature_init_result = feature_init;

	STRAY_LOG_INFO("DLSS SR available=%d needs_updated_driver=%d min_driver=%u.%u feature_init=0x%08x (%s)",
		g_status.super_sampling_available ? 1 : 0,
		g_status.needs_updated_driver ? 1 : 0,
		g_status.min_driver_major,
		g_status.min_driver_minor,
		g_status.feature_init_result,
		result_name(g_status.feature_init_result));

	return g_status;
}

void shutdown(ID3D12Device *device)
{
	// No features are created yet, so there is nothing to ReleaseFeature. When there is, it
	// must happen here first, after a GPU-idle wait and before DestroyParameters.
	if (g_capability_params != nullptr)
	{
		NVSDK_NGX_D3D12_DestroyParameters(g_capability_params);
		g_capability_params = nullptr;
	}

	if (g_status.initialised && device != nullptr)
	{
		const NVSDK_NGX_Result result = NVSDK_NGX_D3D12_Shutdown1(device);
		STRAY_LOG_INFO("NVSDK_NGX_D3D12_Shutdown1: 0x%08x (%s)",
			static_cast<unsigned int>(result), result_name(static_cast<unsigned int>(result)));
	}

	g_status = Status{};
}

} // namespace stray_dlss::ngx

#endif // STRAY_DLSS_ENABLE_NGX
