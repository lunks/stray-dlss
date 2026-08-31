#include "ngx_backend.hpp"

#include "log.hpp"

#include <d3d12.h>

#include <cstdio>

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

bool ensure_feature(ID3D12GraphicsCommandList *, const FeatureDesc &) { return false; }
bool evaluate(ID3D12GraphicsCommandList *, const EvaluateInputs &) { return false; }
void release_feature() {}
const char *last_error() { return "<ngx disabled>"; }

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

// The live feature, and the description it was built for. A size change means a full
// ReleaseFeature + CreateFeature — DLSS features are not resizable. (CLAUDE.md §2.1)
NVSDK_NGX_Handle *g_feature = nullptr;
NVSDK_NGX_Parameter *g_feature_params = nullptr;
FeatureDesc g_feature_desc;
char g_last_error[256] = "";

void set_error(const char *what, NVSDK_NGX_Result result)
{
	std::snprintf(g_last_error, sizeof(g_last_error), "%s: 0x%08x (%s)", what,
		static_cast<unsigned int>(result), result_name(static_cast<unsigned int>(result)));
	STRAY_LOG_ERROR("NGX %s", g_last_error);
}

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

const char *last_error() { return g_last_error; }

void release_feature()
{
	if (g_feature != nullptr)
	{
		NVSDK_NGX_D3D12_ReleaseFeature(g_feature);
		g_feature = nullptr;
	}
	if (g_feature_params != nullptr)
	{
		NVSDK_NGX_D3D12_DestroyParameters(g_feature_params);
		g_feature_params = nullptr;
	}
	g_feature_desc = FeatureDesc{};
}

bool ensure_feature(ID3D12GraphicsCommandList *cmd, const FeatureDesc &desc)
{
	if (!g_status.initialised || !g_status.super_sampling_available || cmd == nullptr)
		return false;
	if (desc.render_width == 0 || desc.output_width == 0)
		return false;

	const bool same = g_feature != nullptr &&
		g_feature_desc.render_width == desc.render_width &&
		g_feature_desc.render_height == desc.render_height &&
		g_feature_desc.output_width == desc.output_width &&
		g_feature_desc.output_height == desc.output_height;
	if (same)
		return true;

	release_feature();

	NVSDK_NGX_Result result = NVSDK_NGX_D3D12_AllocateParameters(&g_feature_params);
	if (NVSDK_NGX_FAILED(result) || g_feature_params == nullptr)
	{
		set_error("AllocateParameters", result);
		return false;
	}

	// Render == output means DLAA. Stray currently renders 1:1, so that is the mode we get;
	// the same code covers the upsampling case when screen percentage is turned down.
	const bool is_dlaa = desc.render_width == desc.output_width &&
		desc.render_height == desc.output_height;
	const NVSDK_NGX_PerfQuality_Value quality = is_dlaa
		? NVSDK_NGX_PerfQuality_Value_DLAA
		: NVSDK_NGX_PerfQuality_Value_MaxQuality;

	// Preset hints must be set BEFORE CreateFeature; setting them afterwards has no effect.
	// Only 0, J, K, L and M are valid — A–D were removed and E/F are deprecated. K is the
	// transformer preset and the best image quality. (CLAUDE.md §5)
	constexpr int kPresetK = NVSDK_NGX_DLSS_Hint_Render_Preset_K;
	g_feature_params->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, kPresetK);
	g_feature_params->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality, kPresetK);
	g_feature_params->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced, kPresetK);
	g_feature_params->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance, kPresetK);
	g_feature_params->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance, kPresetK);

	NVSDK_NGX_DLSS_Create_Params create = {};
	create.Feature.InWidth = desc.render_width;
	create.Feature.InHeight = desc.render_height;
	create.Feature.InTargetWidth = desc.output_width;
	create.Feature.InTargetHeight = desc.output_height;
	create.Feature.InPerfQualityValue = quality;
	// IsHDR because scene colour is linear HDR and pre-exposed; MVLowRes because our motion
	// vectors are at render resolution; DepthInverted because UE4 uses reversed-Z; AutoExposure
	// because we supply no exposure texture. Never DoSharpening — deprecated and inert.
	create.InFeatureCreateFlags =
		NVSDK_NGX_DLSS_Feature_Flags_IsHDR |
		NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
		NVSDK_NGX_DLSS_Feature_Flags_DepthInverted |
		NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;

	result = NGX_D3D12_CREATE_DLSS_EXT(cmd, 1, 1, &g_feature, g_feature_params, &create);
	if (NVSDK_NGX_FAILED(result) || g_feature == nullptr)
	{
		set_error("CreateFeature", result);
		release_feature();
		return false;
	}

	g_feature_desc = desc;
	g_last_error[0] = 0;
	STRAY_LOG_INFO("DLSS feature created: %ux%u -> %ux%u, %s, preset K, flags=0x%x",
		desc.render_width, desc.render_height, desc.output_width, desc.output_height,
		is_dlaa ? "DLAA" : "MaxQuality", static_cast<unsigned>(create.InFeatureCreateFlags));
	return true;
}

bool evaluate(ID3D12GraphicsCommandList *cmd, const EvaluateInputs &in)
{
	if (g_feature == nullptr || g_feature_params == nullptr || cmd == nullptr)
		return false;
	if (in.color == nullptr || in.depth == nullptr || in.motion_vectors == nullptr ||
		in.output == nullptr)
	{
		std::snprintf(g_last_error, sizeof(g_last_error), "evaluate: a required resource is null");
		return false;
	}

	NVSDK_NGX_D3D12_DLSS_Eval_Params eval = {};
	eval.Feature.pInColor = in.color;
	eval.Feature.pInOutput = in.output;
	eval.Feature.InSharpness = 0.0f;   // sharpening is deprecated and does nothing
	eval.pInDepth = in.depth;
	eval.pInMotionVectors = in.motion_vectors;

	// Straight through from TemporalAAParams.zw. NVIDIA's own UE plugin passes these
	// unmodified, and the negative-Y factor in the derivation is already baked in.
	eval.InJitterOffsetX = in.jitter_x;
	eval.InJitterOffsetY = in.jitter_y;

	eval.InRenderSubrectDimensions.Width = in.render_width;
	eval.InRenderSubrectDimensions.Height = in.render_height;
	eval.InReset = in.reset ? 1 : 0;

	// Our resolve already emits pixel-space motion vectors, so no further scaling.
	eval.InMVScaleX = 1.0f;
	eval.InMVScaleY = 1.0f;
	eval.InPreExposure = in.pre_exposure;

	const NVSDK_NGX_Result result =
		NGX_D3D12_EVALUATE_DLSS_EXT(cmd, g_feature, g_feature_params, &eval);
	if (NVSDK_NGX_FAILED(result))
	{
		set_error("EvaluateFeature", result);
		return false;
	}
	return true;
}

void shutdown(ID3D12Device *device)
{
	// The feature goes first, before the capability parameters and the shutdown itself. The
	// caller is responsible for the GPU already being idle.
	release_feature();
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
