#include "ngx_backend.hpp"

#include "core/dlss_quality.hpp"
#include "core/feature_recreate.hpp"
#include "ext_unhook.hpp"

#include "log.hpp"

#include <d3d12.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

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
void set_preset(int) {}
void set_recreate_stable_frames(unsigned int) {}
void recreate_counters(unsigned long long &waits, unsigned long long &restarts)
{
	waits = 0;
	restarts = 0;
}
void release_feature() {}
const char *last_error() { return "<ngx disabled>"; }

namespace { RRStatus g_rr_status; }
void set_rr_mode(int) {}
int rr_mode() { return 0; }
void set_exposure_mode(exposure::Mode) {}
exposure::Mode exposure_mode() { return exposure::Mode::automatic; }
void set_exposure_value_multiplier(float) {}
float exposure_value_multiplier() { return 1.0f; }
int preset() { return 0; }
void set_snippet_path(const char *) {}
void set_exposure_scale(float) {}
float exposure_scale() { return 1.0f; }
const RRStatus &rr_status() { return g_rr_status; }
bool ensure_feature_rr(ID3D12GraphicsCommandList *, const FeatureDesc &) { return false; }
bool evaluate_rr(ID3D12GraphicsCommandList *, const EvaluateInputsRR &) { return false; }
void release_feature_rr() {}

} // namespace stray_dlss::ngx

#else

#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers.h>
// DLSSD (Ray Reconstruction). Vendored at the same SDK tag as the import library
// (v310.7.0); nvsdk_ngx_params_dlssd.h was fetched from that tag alongside.
#include <nvsdk_ngx_helpers_dlssd.h>

namespace stray_dlss::ngx {
namespace {
// [STRAYDLSS] NgxPreset via set_preset: 10=J 11=K 12=L 13=M, 0=driver default. K is the
// transformer preset and the default; J is often calmer on specular sparkle when a game's
// denoisers were tuned for the engine's own TAA. (CLAUDE.md §5)
int g_preset = NVSDK_NGX_DLSS_Hint_Render_Preset_K;
} // namespace
void set_preset(int preset)
{
	if (preset == 0 || (preset >= 10 && preset <= 13))
		g_preset = preset;
	else
		STRAY_LOG_WARN("NgxPreset %d invalid (0, 10=J, 11=K, 12=L, 13=M); keeping %d.",
			preset, g_preset);
}
} // namespace stray_dlss::ngx

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

// THE RE-CREATION DEBOUNCE (src/core/feature_recreate.hpp). A feature is torn down only for a
// rect that has SETTLED, because a scripted scene transition animates the view rect every few
// frames and chasing it created six to ten features per transition — each one a lost temporal
// history and a full CreateFeature stall. Pure decision, unit-tested; this is only its state.
core::RecreateState g_recreate_state;
unsigned int g_recreate_stable_frames = core::kDefaultRecreateStableFrames;

// [STRAYDLSS] NgxExposure (ngx_backend.hpp). Creation-time property.
exposure::Mode g_exposure_mode = exposure::Mode::automatic;

// NVSDK_NGX_DLSS_Feature_Flags_AutoExposure is duplicated into the core library so it builds
// without the NGX headers. If NVIDIA ever renumbers it, this fails the build rather than
// silently creating the feature with the wrong flag.
static_assert(exposure::kAutoExposureFlag ==
	static_cast<unsigned int>(NVSDK_NGX_DLSS_Feature_Flags_AutoExposure),
	"exposure::kAutoExposureFlag no longer matches nvsdk_ngx_defs.h");

// --- NGX snippet search paths (ngx_backend.hpp: set_snippet_path) ---
//
// ALL FILE-STATIC ON PURPOSE. NVSDK_NGX_PathListInfo holds `wchar_t const* const* Path` — a
// borrowed array of borrowed strings — and NVSDK_NGX_FeatureCommonInfo additionally carries an
// InternalData pointer that NGX writes into, so NGX plainly retains the struct. Passing any of
// this from the stack is the classic lifetime trap with this API: the paths are read long after
// Init returns and a stack temporary is a dangling read.
constexpr unsigned int kMaxSnippetPaths = 3;
constexpr std::size_t kSnippetPathChars = 512;
wchar_t g_snippet_paths[kMaxSnippetPaths][kSnippetPathChars] = {};
const wchar_t *g_snippet_path_ptrs[kMaxSnippetPaths] = {};
unsigned int g_snippet_path_count = 0;
char g_snippet_path_override[kSnippetPathChars] = "";
NVSDK_NGX_FeatureCommonInfo g_common_info = {};

// Strips the file name from a full module path, leaving the directory (no trailing slash).
void strip_to_directory(wchar_t *path)
{
	wchar_t *last = nullptr;
	for (wchar_t *p = path; *p != 0; ++p)
		if (*p == L'\\' || *p == L'/')
			last = p;
	if (last != nullptr)
		*last = 0;
}

bool add_snippet_path(const wchar_t *dir)
{
	if (dir == nullptr || dir[0] == 0 || g_snippet_path_count >= kMaxSnippetPaths)
		return false;
	// Skip duplicates: a repeated directory just wastes a search slot.
	for (unsigned int i = 0; i < g_snippet_path_count; ++i)
		if (::lstrcmpiW(g_snippet_paths[i], dir) == 0)
			return false;
	std::size_t n = 0;
	while (dir[n] != 0 && n + 1 < kSnippetPathChars)
	{
		g_snippet_paths[g_snippet_path_count][n] = dir[n];
		++n;
	}
	g_snippet_paths[g_snippet_path_count][n] = 0;
	g_snippet_path_ptrs[g_snippet_path_count] = g_snippet_paths[g_snippet_path_count];
	++g_snippet_path_count;
	return true;
}

// Builds the search list, in descending order of preference:
//   1. [STRAYDLSS] NgxSnippetPath, when the operator set one (testing / non-standard staging).
//   2. The GAME EXECUTABLE's directory — where the operator stages nvngx_dlssnr.dll, and what
//      the header calls the "application directory". Resolved from GetModuleFileNameW(nullptr)
//      rather than the CWD, which under Proton is not reliably the exe directory, and rather
//      than hardcoding anything.
//   3. Our own add-on's directory, in case a snippet is staged beside the add-on instead.
void build_snippet_paths()
{
	g_snippet_path_count = 0;

	if (g_snippet_path_override[0] != 0)
	{
		wchar_t wide[kSnippetPathChars] = {};
		if (::MultiByteToWideChar(CP_UTF8, 0, g_snippet_path_override, -1, wide,
				static_cast<int>(kSnippetPathChars) - 1) > 0)
			add_snippet_path(wide);
	}

	wchar_t exe[kSnippetPathChars] = {};
	if (::GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(kSnippetPathChars)) > 0)
	{
		strip_to_directory(exe);
		add_snippet_path(exe);
	}

	HMODULE self = nullptr;
	if (::GetModuleHandleExW(
			GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCWSTR>(&build_snippet_paths), &self) && self != nullptr)
	{
		wchar_t own[kSnippetPathChars] = {};
		if (::GetModuleFileNameW(self, own, static_cast<DWORD>(kSnippetPathChars)) > 0)
		{
			strip_to_directory(own);
			add_snippet_path(own);
		}
	}
}
// [STRAYDLSS] NgxExposureScale (ngx_backend.hpp): InExposureScale under a texture mode.
float g_exposure_scale = 1.0f;
// [STRAYDLSS] NgxExposureValue (ngx_backend.hpp): the owned texel's consume-test multiplier.
float g_exposure_value_multiplier = 1.0f;

// --- Ray Reconstruction state ([STRAYDLSS] NgxRR) ---
int g_rr_mode = 0;
RRStatus g_rr_status;
NVSDK_NGX_Handle *g_rr_feature = nullptr;
NVSDK_NGX_Parameter *g_rr_params = nullptr;
FeatureDesc g_rr_desc;
// A description that failed CreateFeature is latched and never retried per-frame: retrying
// a failing create every frame would hitch the game for nothing and flood the log.
FeatureDesc g_rr_failed_desc;
bool g_rr_create_latched = false;
// The probe's throwaway feature. Its CREATION work was recorded onto the game's command
// list; releasing it immediately could free memory that recorded-but-unexecuted work still
// references, so the release is deferred until the keep-alive ring says the GPU is past it.
NVSDK_NGX_Handle *g_probe_feature = nullptr;
NVSDK_NGX_Parameter *g_probe_params = nullptr;
std::uint64_t g_probe_eval_frame = 0;
// The matrices handed to the DLSSD helper live here because it passes POINTERS
// (SetVoidPointer): the storage must outlive the EvaluateFeature call, and file statics
// also cover a DLL that reads them later than the call itself.
float g_rr_world_to_view[16] = {};
float g_rr_view_to_clip[16] = {};

// Resources handed to NGX, kept alive until the GPU has certainly finished with them.
//
// NGX holds NO references to what we pass it — the manual says so, and it makes us responsible
// (CLAUDE.md §5). EvaluateFeature only RECORDS work; the GPU runs it later. UE4 rotates and
// frees these buffers constantly, so between recording and execution the game can drop the last
// reference to the colour, depth or output texture and the resource dies underneath in-flight
// work. That matches the observed failure exactly: our own cycle completes, the log stops
// mid-frame, and the process dies with an access violation and no GPU fault at all.
//
// So take a reference for the duration and drop it a few frames later.
constexpr std::size_t kKeepAliveFrames = 6;
struct KeepAlive
{
	// 8 slots: SR uses four (colour/depth/MV/output); RR adds the four guide textures.
	// The three G-buffer INPUTS are not here: whatever future pass produces the guides owns
	// their keep-alive. (gbuffer_resolve did, until it was deleted 2026-09-03 with the finder.)
	ID3D12Resource *resources[8] = {};
	std::uint64_t frame = 0;
};
std::vector<KeepAlive> g_keep_alive;
std::uint64_t g_eval_frame = 0;

void release_keep_alive(bool all)
{
	for (auto it = g_keep_alive.begin(); it != g_keep_alive.end();)
	{
		if (all || it->frame + kKeepAliveFrames <= g_eval_frame)
		{
			for (ID3D12Resource *r : it->resources)
				if (r != nullptr)
					r->Release();
			it = g_keep_alive.erase(it);
		}
		else
		{
			++it;
		}
	}
}

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

	// Undo ReShade's ext-vtable patch before NGX first touches the extension interface.
	ext_unhook::repair();

	NVSDK_NGX_LoggingInfo logging_info = {};
	logging_info.LoggingCallback = ngx_log_callback;
	logging_info.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_ON;
	logging_info.DisableOtherLoggingSinks = false;

	// PathListInfo: extra directories NGX searches for feature DLLs, "other than the default
	// path (application directory)" (nvsdk_ngx.h:133-135). Additive — the driver-shipped
	// SR/RR snippets keep resolving exactly as before; this only ADDS places to look, which
	// is what a side-loaded snippet needs. g_common_info is FILE-STATIC because NGX retains
	// it (it writes through InternalData) and the path array is borrowed, not copied.
	build_snippet_paths();
	g_common_info = NVSDK_NGX_FeatureCommonInfo{};
	g_common_info.LoggingInfo = logging_info;
	g_common_info.PathListInfo.Path = g_snippet_path_ptrs;
	g_common_info.PathListInfo.Length = g_snippet_path_count;

	for (unsigned int i = 0; i < g_snippet_path_count; ++i)
		STRAY_LOG_INFO("NGX snippet search path[%u] = '%ls'", i, g_snippet_paths[i]);
	if (g_snippet_path_count == 0)
		STRAY_LOG_WARN("NGX snippet search list is EMPTY; only NGX's own default (the "
			"application directory) will be searched.");

	// InApplicationDataPath stays L"." deliberately: the header defines it as the directory
	// "to store logs and other temporary files" (nvsdk_ngx.h:125-127), NOT a snippet search
	// path, so changing it cannot affect feature resolution and would only move NGX's log
	// files. Left alone to keep SR/RR byte-identical.
	const NVSDK_NGX_Result init_result = NVSDK_NGX_D3D12_Init_with_ProjectID(
		kProjectId,
		NVSDK_NGX_ENGINE_TYPE_CUSTOM,
		kEngineVersion,
		L".",
		device,
		&g_common_info,
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

	// The DLSSD (Ray Reconstruction) capability keys — the SuperSamplingDenoising.*
	// siblings of the SR keys (nvsdk_ngx_defs_dlssd.h). Same discipline as SR: never
	// GetFeatureRequirements (Proton does not implement it); Available plus a successful
	// create are the only trustworthy signals. Queried and logged unconditionally — one
	// line, and it answers "does this stack even claim RR" without a dedicated run.
	{
		int rr_available = 0;
		g_capability_params->Get(NVSDK_NGX_Parameter_SuperSamplingDenoising_Available,
			&rr_available);
		g_rr_status.available = rr_available > 0;

		int rr_needs_driver = 0;
		g_capability_params->Get(
			NVSDK_NGX_Parameter_SuperSamplingDenoising_NeedsUpdatedDriver, &rr_needs_driver);
		g_rr_status.needs_updated_driver = rr_needs_driver > 0;

		unsigned int rr_major = 0, rr_minor = 0;
		g_capability_params->Get(
			NVSDK_NGX_Parameter_SuperSamplingDenoising_MinDriverVersionMajor, &rr_major);
		g_capability_params->Get(
			NVSDK_NGX_Parameter_SuperSamplingDenoising_MinDriverVersionMinor, &rr_minor);
		g_rr_status.min_driver_major = rr_major;
		g_rr_status.min_driver_minor = rr_minor;

		unsigned int rr_init = 0;
		g_capability_params->Get(
			NVSDK_NGX_Parameter_SuperSamplingDenoising_FeatureInitResult, &rr_init);
		g_rr_status.feature_init_result = rr_init;

		STRAY_LOG_INFO("DLSS RR (DLSSD) available=%d needs_updated_driver=%d min_driver=%u.%u "
			"feature_init=0x%08x (%s)  [NgxRR=%d]",
			g_rr_status.available ? 1 : 0, g_rr_status.needs_updated_driver ? 1 : 0,
			g_rr_status.min_driver_major, g_rr_status.min_driver_minor,
			g_rr_status.feature_init_result, result_name(g_rr_status.feature_init_result),
			g_rr_mode);
		if (!g_rr_status.available && g_rr_mode != 0)
			STRAY_LOG_WARN("  RR unavailable per the capability key. On this stack that "
				"usually means nvngx_dlssd.dll is not staged: the operator must place "
				"nvngx_dlssd.dll next to the game executable (same place as nvngx_dlss.dll); "
				"driver >= 535 is also required. Availability here can also be a false "
				"negative under Proton - the create probe (NgxRR=1) is the real answer.");
	}

	return g_status;
}

const char *last_error() { return g_last_error; }

void release_feature()
{
	// Everything we were holding for NGX goes first: after this the feature is gone and there
	// is nothing left that could still be reading them.
	release_keep_alive(/*all=*/true);

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
	// No feature means nothing to protect, so the next request creates immediately.
	g_recreate_state.pending = core::FeatureRect{};
	g_recreate_state.pending_count = 0;
}

namespace {

// Releases the probe's throwaway feature once the keep-alive horizon says the GPU has
// executed the command list its creation work was recorded on. Called from evaluate()'s
// frame tick and unconditionally from release_feature_rr()/shutdown.
void release_probe_feature(bool force)
{
	if (g_probe_feature == nullptr && g_probe_params == nullptr)
		return;
	if (!force && g_eval_frame < g_probe_eval_frame + kKeepAliveFrames)
		return;
	if (g_probe_feature != nullptr)
	{
		NVSDK_NGX_D3D12_ReleaseFeature(g_probe_feature);
		g_probe_feature = nullptr;
		STRAY_LOG_INFO("RR probe feature released (deferred %s).",
			force ? "to teardown" : "past the keep-alive horizon");
	}
	if (g_probe_params != nullptr)
	{
		NVSDK_NGX_D3D12_DestroyParameters(g_probe_params);
		g_probe_params = nullptr;
	}
}

// The NgxRR=1 probe: one CREATE_DLSSD_EXT attempt on the same command-list path SR feature
// creation uses, released once the GPU is past it. Runs once per session, whatever the
// outcome — the log lines it emits are the whole product of a probe run.
void maybe_probe_rr(ID3D12GraphicsCommandList *cmd, const FeatureDesc &desc)
{
	if (g_rr_mode != 1 || g_rr_status.probed || cmd == nullptr)
		return;
	if (desc.render_width == 0 || desc.output_width == 0)
		return;
	g_rr_status.probed = true;

	NVSDK_NGX_Result result = NVSDK_NGX_D3D12_AllocateParameters(&g_probe_params);
	if (NVSDK_NGX_FAILED(result) || g_probe_params == nullptr)
	{
		g_rr_status.probe_create_result = static_cast<unsigned int>(result);
		STRAY_LOG_ERROR("RR PROBE: AllocateParameters failed: 0x%08x (%s)",
			static_cast<unsigned int>(result),
			result_name(static_cast<unsigned int>(result)));
		return;
	}

	NVSDK_NGX_DLSSD_Create_Params create = {};
	create.InDenoiseMode = NVSDK_NGX_DLSS_Denoise_Mode_DLUnified;
	create.InRoughnessMode = NVSDK_NGX_DLSS_Roughness_Mode_Unpacked;
	create.InUseHWDepth = NVSDK_NGX_DLSS_Depth_Type_HW;
	create.InWidth = desc.render_width;
	create.InHeight = desc.render_height;
	create.InTargetWidth = desc.output_width;
	create.InTargetHeight = desc.output_height;
	create.InPerfQualityValue =
		desc.render_width == desc.output_width && desc.render_height == desc.output_height
			? NVSDK_NGX_PerfQuality_Value_DLAA
			: NVSDK_NGX_PerfQuality_Value_MaxQuality;
	// SR's flags minus AutoExposure: RR does not support auto-exposure and Remix masks the
	// flag off (RESEARCH-RR-GBUFFER.md §2.1); InPreExposure still applies at evaluate.
	create.InFeatureCreateFlags =
		NVSDK_NGX_DLSS_Feature_Flags_IsHDR |
		NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
		NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;

	STRAY_LOG_WARN("RR PROBE: attempting NGX_D3D12_CREATE_DLSSD_EXT %ux%u -> %ux%u "
		"(DLUnified, Unpacked roughness, HW depth, flags=0x%x)...",
		create.InWidth, create.InHeight, create.InTargetWidth, create.InTargetHeight,
		static_cast<unsigned>(create.InFeatureCreateFlags));

	result = NGX_D3D12_CREATE_DLSSD_EXT(cmd, 1, 1, &g_probe_feature, g_probe_params, &create);
	g_rr_status.probe_create_result = static_cast<unsigned int>(result);
	g_rr_status.probe_create_ok = NVSDK_NGX_SUCCEED(result) && g_probe_feature != nullptr;
	g_probe_eval_frame = g_eval_frame;

	if (g_rr_status.probe_create_ok)
	{
		STRAY_LOG_WARN("RR PROBE: CreateFeature SUCCEEDED (0x%08x %s). DLSSD exists on this "
			"stack; the feature is a throwaway and will be released shortly. NgxRR=2 is the "
			"next step.", g_rr_status.probe_create_result,
			result_name(g_rr_status.probe_create_result));
	}
	else
	{
		STRAY_LOG_ERROR("RR PROBE: CreateFeature FAILED: 0x%08x (%s)",
			g_rr_status.probe_create_result,
			result_name(g_rr_status.probe_create_result));
		STRAY_LOG_ERROR("  FAIL_FeatureNotFound / FAIL_FeatureNotSupported here usually "
			"means the loader could not find nvngx_dlssd.dll - the operator must stage "
			"nvngx_dlssd.dll next to the game executable (beside nvngx_dlss.dll). "
			"FAIL_OutOfDate means the staged DLL or driver is too old (driver >= 535). "
			"Grep this log for 'ngx:' lines above for the DLL loader's own account.");
		if (g_probe_params != nullptr)
		{
			NVSDK_NGX_D3D12_DestroyParameters(g_probe_params);
			g_probe_params = nullptr;
		}
	}
}

} // namespace

void set_rr_mode(int mode)
{
	if (mode >= 0 && mode <= 2)
		g_rr_mode = mode;
	else
		STRAY_LOG_WARN("NgxRR %d invalid (0 off, 1 probe, 2 full); keeping %d.", mode, g_rr_mode);
}

int rr_mode() { return g_rr_mode; }
const RRStatus &rr_status() { return g_rr_status; }

void set_snippet_path(const char *utf8_path)
{
	if (utf8_path == nullptr)
		g_snippet_path_override[0] = 0;
	else
		std::snprintf(g_snippet_path_override, sizeof(g_snippet_path_override), "%s", utf8_path);
}

void set_exposure_mode(exposure::Mode mode) { g_exposure_mode = mode; }
exposure::Mode exposure_mode() { return g_exposure_mode; }
void set_exposure_scale(float scale) { g_exposure_scale = scale; }
float exposure_scale() { return g_exposure_scale; }
void set_exposure_value_multiplier(float m) { g_exposure_value_multiplier = m; }
float exposure_value_multiplier() { return g_exposure_value_multiplier; }
int preset() { return g_preset; }

void release_feature_rr()
{
	release_probe_feature(/*force=*/true);
	if (g_rr_feature != nullptr)
	{
		NVSDK_NGX_D3D12_ReleaseFeature(g_rr_feature);
		g_rr_feature = nullptr;
	}
	if (g_rr_params != nullptr)
	{
		NVSDK_NGX_D3D12_DestroyParameters(g_rr_params);
		g_rr_params = nullptr;
	}
	g_rr_desc = FeatureDesc{};
}

bool ensure_feature_rr(ID3D12GraphicsCommandList *cmd, const FeatureDesc &desc)
{
	// Same preconditions and repair as the SR ensure. Availability is deliberately NOT a
	// gate: the capability key can be a false negative under Proton (the SR precedent),
	// so a create attempt decides — once, then the latch.
	ext_unhook::repair();
	if (!g_status.initialised || cmd == nullptr)
		return false;
	if (desc.render_width == 0 || desc.output_width == 0)
		return false;

	const bool same = g_rr_feature != nullptr &&
		g_rr_desc.render_width == desc.render_width &&
		g_rr_desc.render_height == desc.render_height &&
		g_rr_desc.output_width == desc.output_width &&
		g_rr_desc.output_height == desc.output_height;
	if (same)
		return true;

	const bool same_failed = g_rr_create_latched &&
		g_rr_failed_desc.render_width == desc.render_width &&
		g_rr_failed_desc.render_height == desc.render_height &&
		g_rr_failed_desc.output_width == desc.output_width &&
		g_rr_failed_desc.output_height == desc.output_height;
	if (same_failed)
		return false; // already failed at this size and logged; SR carries the frame

	release_feature_rr();

	NVSDK_NGX_Result result = NVSDK_NGX_D3D12_AllocateParameters(&g_rr_params);
	if (NVSDK_NGX_FAILED(result) || g_rr_params == nullptr)
	{
		set_error("RR AllocateParameters", result);
		return false;
	}

	// Same derivation as the SR path: NGX picks its reconstruction parameters from
	// InPerfQualityValue, so it must describe the rect we actually hand it. RR carried the same
	// hardcoded MaxQuality this file had on the SR side.
	const DlssQuality rr_quality = dlss_quality_for(desc.render_width, desc.render_height,
		desc.output_width, desc.output_height);

	NVSDK_NGX_DLSSD_Create_Params create = {};
	create.InDenoiseMode = NVSDK_NGX_DLSS_Denoise_Mode_DLUnified;
	// Unpacked: the guide producer must emit a standalone R16F roughness (and also pack .w of the
	// normals texture, so Packed is one enum flip away if the observation run prefers it).
	create.InRoughnessMode = NVSDK_NGX_DLSS_Roughness_Mode_Unpacked;
	// HW reversed-Z with DepthInverted, per the staged plan (RESEARCH-RR-GBUFFER.md §4.4
	// step 7); the UE plugin's linear-depth substitution is the documented fallback if RR
	// misbehaves on it.
	create.InUseHWDepth = NVSDK_NGX_DLSS_Depth_Type_HW;
	create.InWidth = desc.render_width;
	create.InHeight = desc.render_height;
	create.InTargetWidth = desc.output_width;
	create.InTargetHeight = desc.output_height;
	NVSDK_NGX_PerfQuality_Value rr_perf = NVSDK_NGX_PerfQuality_Value_MaxQuality;
	switch (rr_quality)
	{
	case DlssQuality::dlaa: rr_perf = NVSDK_NGX_PerfQuality_Value_DLAA; break;
	case DlssQuality::max_quality: rr_perf = NVSDK_NGX_PerfQuality_Value_MaxQuality; break;
	case DlssQuality::balanced: rr_perf = NVSDK_NGX_PerfQuality_Value_Balanced; break;
	case DlssQuality::max_performance: rr_perf = NVSDK_NGX_PerfQuality_Value_MaxPerf; break;
	case DlssQuality::ultra_performance:
		rr_perf = NVSDK_NGX_PerfQuality_Value_UltraPerformance;
		break;
	}
	create.InPerfQualityValue = rr_perf;
	// SR's flags minus AutoExposure (RR ignores auto-exposure; Remix masks it off —
	// RESEARCH-RR-GBUFFER.md §2.1). Preset: Default — the RR guide recommends games stick
	// to Preset_Default (the defs only bless D and E as named alternates), so no
	// RayReconstruction.Hint.Render.Preset.* key is set.
	create.InFeatureCreateFlags =
		NVSDK_NGX_DLSS_Feature_Flags_IsHDR |
		NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
		NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;

	result = NGX_D3D12_CREATE_DLSSD_EXT(cmd, 1, 1, &g_rr_feature, g_rr_params, &create);
	if (NVSDK_NGX_FAILED(result) || g_rr_feature == nullptr)
	{
		set_error("RR CreateFeature", result);
		STRAY_LOG_ERROR("  RR create failed at this size; latched - SR carries every frame "
			"until the size changes. If the result is FeatureNotFound/NotSupported, stage "
			"nvngx_dlssd.dll next to the game executable.");
		release_feature_rr();
		g_rr_failed_desc = desc;
		g_rr_create_latched = true;
		return false;
	}

	g_rr_desc = desc;
	g_rr_create_latched = false;
	g_last_error[0] = 0;
	STRAY_LOG_INFO("DLSS RR feature created: %ux%u -> %ux%u, %s, preset=Default, flags=0x%x "
		"(DLUnified, Unpacked roughness, HW depth)",
		desc.render_width, desc.render_height, desc.output_width, desc.output_height,
		dlss_quality_name(rr_quality),
		static_cast<unsigned>(create.InFeatureCreateFlags));
	return true;
}

bool evaluate_rr(ID3D12GraphicsCommandList *cmd, const EvaluateInputsRR &in)
{
	ext_unhook::repair();
	if (g_rr_feature == nullptr || g_rr_params == nullptr || cmd == nullptr)
		return false;
	if (in.base.color == nullptr || in.base.depth == nullptr ||
		in.base.motion_vectors == nullptr || in.base.output == nullptr ||
		in.diffuse_albedo == nullptr || in.specular_albedo == nullptr ||
		in.normals_roughness == nullptr || in.roughness == nullptr)
	{
		std::snprintf(g_last_error, sizeof(g_last_error),
			"evaluate_rr: a required resource is null");
		return false;
	}

	NVSDK_NGX_D3D12_DLSSD_Eval_Params eval = {};
	eval.pInColor = in.base.color;
	eval.pInOutput = in.base.output;
	eval.pInDepth = in.base.depth;
	eval.pInMotionVectors = in.base.motion_vectors;
	eval.pInDiffuseAlbedo = in.diffuse_albedo;
	eval.pInSpecularAlbedo = in.specular_albedo;
	eval.pInNormals = in.normals_roughness;
	eval.pInRoughness = in.roughness;

	// Identical semantics to the SR evaluate: jitter straight through, render-res subrect,
	// pixel-space MVs so scale stays 1 (the helper maps 0 to 1 itself, but explicit is
	// louder), pre-exposure from View row 135.y.
	eval.InJitterOffsetX = in.base.jitter_x;
	eval.InJitterOffsetY = in.base.jitter_y;
	eval.InRenderSubrectDimensions.Width = in.base.render_width;
	eval.InRenderSubrectDimensions.Height = in.base.render_height;
	eval.InReset = in.base.reset ? 1 : 0;
	eval.InMVScaleX = 1.0f;
	eval.InMVScaleY = 1.0f;
	// Guarded exactly as on the SR path: an unvalidated row 135 yields a deliberate 1.0, not
	// whatever the row happened to hold. RR takes no exposure TEXTURE, so InPreExposure is the
	// only exposure input it has and getting it wrong has nowhere else to be corrected.
	eval.InPreExposure = exposure::sr_pre_exposure(in.base.pre_exposure, in.base.pre_exposure_ok);
	eval.InFrameTimeDeltaInMsec = in.frame_time_delta_ms;
	// Deliberately NO pInExposureTexture here even under NgxExposure=texture: "Exposure,
	// Auto-Exposure, Sharpness ... are not supported by DLSS Ray Reconstruction" (RR guide
	// PDF §3.7, RESEARCH-RR-GBUFFER.md §2.1) — InPreExposure is the one exposure input RR
	// takes, and it is passed above. The exposure-texture fix is an SR-path change.

	if (in.have_matrices)
	{
		// The helper passes these as void POINTERS; file-static storage outlives the call
		// (and any later read the DLL might make). Row-major, straight from the View CB.
		std::memcpy(g_rr_world_to_view, in.world_to_view, sizeof(g_rr_world_to_view));
		std::memcpy(g_rr_view_to_clip, in.view_to_clip, sizeof(g_rr_view_to_clip));
		eval.pInWorldToViewMatrix = g_rr_world_to_view;
		eval.pInViewToClipMatrix = g_rr_view_to_clip;
	}

	const NVSDK_NGX_Result result =
		NGX_D3D12_EVALUATE_DLSSD_EXT(cmd, g_rr_feature, g_rr_params, &eval);
	if (NVSDK_NGX_FAILED(result))
	{
		set_error("RR EvaluateFeature", result);
		return false;
	}

	// Keep the SR quartet AND the four guides alive past GPU execution; the G-buffer
	// inputs are held by whatever produced the guides.
	KeepAlive ka;
	ka.frame = g_eval_frame;
	ka.resources[0] = in.base.color;
	ka.resources[1] = in.base.depth;
	ka.resources[2] = in.base.motion_vectors;
	ka.resources[3] = in.base.output;
	ka.resources[4] = in.diffuse_albedo;
	ka.resources[5] = in.specular_albedo;
	ka.resources[6] = in.normals_roughness;
	ka.resources[7] = in.roughness;
	for (ID3D12Resource *r : ka.resources)
		if (r != nullptr)
			r->AddRef();
	g_keep_alive.push_back(ka);

	++g_eval_frame;
	release_keep_alive(/*all=*/false);
	release_probe_feature(/*force=*/false);
	return true;
}

bool ensure_feature(ID3D12GraphicsCommandList *cmd, const FeatureDesc &desc)
{
	// CreateFeature builds NGX's cubin descriptor objects; undo ReShade's ext-vtable patch
	// first or they are built against mangled handles. (src/ext_unhook.hpp)
	ext_unhook::repair();
	if (!g_status.initialised || !g_status.super_sampling_available || cmd == nullptr)
		return false;
	if (desc.render_width == 0 || desc.render_height == 0 ||
		desc.output_width == 0 || desc.output_height == 0)
		return false;

	// The NgxRR=1 probe rides the SR ensure path: same command list, same real dimensions,
	// ext hook just repaired. One attempt per session; SR proceeds regardless.
	maybe_probe_rr(cmd, desc);

	const core::FeatureRect want{ desc.render_width, desc.render_height,
		desc.output_width, desc.output_height };
	const core::FeatureRect live{ g_feature_desc.render_width, g_feature_desc.render_height,
		g_feature_desc.output_width, g_feature_desc.output_height };
	const core::RecreateAction action = core::plan_recreate(g_recreate_state,
		g_feature != nullptr, live, want, g_recreate_stable_frames);
	if (action == core::RecreateAction::keep)
		return g_feature != nullptr;
	if (action == core::RecreateAction::wait)
	{
		// DECLINED, and the frame renders with the engine's own TAA. This is the correct
		// failure: the alternative is a feature built for a rect that has already moved on.
		// Logged sparsely because a transition produces one of these per frame for ~0.5 s.
		if (g_recreate_state.waits <= 3 || (g_recreate_state.waits % 600) == 0)
		{
			std::snprintf(g_last_error, sizeof(g_last_error),
				"ensure_feature: rect %ux%u -> %ux%u has not settled (%u/%u frames)",
				desc.render_width, desc.render_height, desc.output_width, desc.output_height,
				g_recreate_state.pending_count, g_recreate_stable_frames);
			STRAY_LOG_INFO("DLSS recreate DEFERRED: asked for %ux%u -> %ux%u while the live "
				"feature is %ux%u -> %ux%u, and that request has only stood for %u of %u "
				"frames. The engine's own TAA renders this frame. A scripted scene transition "
				"animates the view rect every few frames, and chasing it destroyed DLSS's "
				"temporal history six to ten times per transition; waiting for the rect to "
				"settle keeps the ORIGINAL feature alive across the whole animation. "
				"waits=%llu restarts=%llu ([STRAYDLSS] NgxRecreateStableFrames=0 restores the "
				"old behaviour). Logged 3 times, then every 600.",
				desc.render_width, desc.render_height, desc.output_width, desc.output_height,
				g_feature_desc.render_width, g_feature_desc.render_height,
				g_feature_desc.output_width, g_feature_desc.output_height,
				g_recreate_state.pending_count, g_recreate_stable_frames,
				static_cast<unsigned long long>(g_recreate_state.waits),
				static_cast<unsigned long long>(g_recreate_state.restarts));
		}
		return false;
	}

	release_feature();

	NVSDK_NGX_Result result = NVSDK_NGX_D3D12_AllocateParameters(&g_feature_params);
	if (NVSDK_NGX_FAILED(result) || g_feature_params == nullptr)
	{
		set_error("AllocateParameters", result);
		return false;
	}

	// Render == output means DLAA. Stray currently renders 1:1, so that is the mode we get;
	// the same code covers the upsampling case when screen percentage is turned down.
	// DERIVED from the actual ratio, never hardcoded. NGX picks its internal reconstruction
	// parameters from InPerfQualityValue, so declaring a mode whose ratio does not match the
	// rect we hand it tells DLSS to expect a different sample count than it gets. This was
	// MaxQuality at every ratio, which at 50% screen percentage (a true 2.0x = Performance) was
	// two modes off — and very nearly right at 70% (1.43x), consistent with the measured
	// observation that 70% looks sharper than 50%. (CLAUDE.md §5 required deriving it.)
	const DlssQuality derived = dlss_quality_for(desc.render_width, desc.render_height,
		desc.output_width, desc.output_height);
	NVSDK_NGX_PerfQuality_Value quality = NVSDK_NGX_PerfQuality_Value_MaxQuality;
	switch (derived)
	{
	case DlssQuality::dlaa: quality = NVSDK_NGX_PerfQuality_Value_DLAA; break;
	case DlssQuality::max_quality: quality = NVSDK_NGX_PerfQuality_Value_MaxQuality; break;
	case DlssQuality::balanced: quality = NVSDK_NGX_PerfQuality_Value_Balanced; break;
	case DlssQuality::max_performance: quality = NVSDK_NGX_PerfQuality_Value_MaxPerf; break;
	case DlssQuality::ultra_performance:
		quality = NVSDK_NGX_PerfQuality_Value_UltraPerformance;
		break;
	}

	const int preset = g_preset;

	// Preset hints must be set BEFORE CreateFeature; setting them afterwards has no effect.
	// Only 0, J, K, L and M are valid — A–D were removed and E/F are deprecated. K is the
	// transformer preset and the best image quality. (CLAUDE.md §5)
	g_feature_params->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, preset);
	g_feature_params->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality, preset);
	g_feature_params->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced, preset);
	g_feature_params->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance, preset);
	g_feature_params->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance, preset);

	NVSDK_NGX_DLSS_Create_Params create = {};
	create.Feature.InWidth = desc.render_width;
	create.Feature.InHeight = desc.render_height;
	create.Feature.InTargetWidth = desc.output_width;
	create.Feature.InTargetHeight = desc.output_height;
	create.Feature.InPerfQualityValue = quality;
	// IsHDR because scene colour is linear HDR and pre-exposed; MVLowRes because our motion
	// vectors are at render resolution; DepthInverted because UE4 uses reversed-Z. Never
	// DoSharpening — deprecated and inert. AutoExposure (1<<6 = 0x40, nvsdk_ngx_defs.h:297)
	// only under NgxExposure=auto: in texture mode the engine's eye-adaptation texture is
	// the exposure source and the flag must be ABSENT — exactly the official UE plugin's
	// pair (NGXRHI.cpp:550-565: `bUseAutoExposure ? Flags_AutoExposure : 0`).
	create.InFeatureCreateFlags = static_cast<int>(
		static_cast<unsigned int>(NVSDK_NGX_DLSS_Feature_Flags_IsHDR) |
		static_cast<unsigned int>(NVSDK_NGX_DLSS_Feature_Flags_MVLowRes) |
		static_cast<unsigned int>(NVSDK_NGX_DLSS_Feature_Flags_DepthInverted) |
		exposure::create_flag_bits(g_exposure_mode));

	// ONE line that carries the whole exposure configuration, unconditionally, every session.
	// The old line hardcoded the string "createFlags=0x0b" into the diagnostic and printed the
	// mode as a two-valued word — so a log could not distinguish "texture mode was configured"
	// from "texture mode was configured AND the runtime was ever going to honour it". That
	// ambiguity is how "the texture mode measured inert" became a belief with no measurement
	// behind it.
	const int resolved = exposure::resolved_preset(g_preset, derived);
	const exposure::PresetExposure support =
		exposure::preset_exposure_support(g_preset, derived);
	STRAY_LOG_INFO("DLSS EXPOSURE: mode=%s flags=0x%02x (AutoExposure %s) preset=%s(%d) "
		"quality=%s presetSupportsExposureInput=%s scale=%.4f valueMul=%.4f",
		exposure::mode_name(g_exposure_mode),
		static_cast<unsigned>(create.InFeatureCreateFlags),
		exposure::create_flag_bits(g_exposure_mode) != 0 ? "SET" : "dropped",
		exposure::preset_letter(resolved), resolved, dlss_quality_name(derived),
		support == exposure::PresetExposure::supported ? "yes"
			: (support == exposure::PresetExposure::not_supported ? "NO" : "unknown"),
		static_cast<double>(g_exposure_scale),
		static_cast<double>(g_exposure_value_multiplier));

	// DLSS Programming Guide 310.6.0 §3.9: "Only supported by Presets J and K. Preset L always
	// uses AutoExposure." A texture mode on any other preset is a configuration that cannot
	// work, and it is the single best explanation on record for the 2026-08-31 observation
	// (flag dropped, healthy texture passed every frame, indicator still reading Auto Exposure:
	// ON). We WARN rather than silently switching the preset: quietly repairing a config is how
	// a session ends up measuring something other than what it was asked to measure.
	if (exposure::exposure_will_be_ignored(g_exposure_mode, g_preset, derived))
		STRAY_LOG_WARN("NgxExposure=%s is INERT on preset %s: the DLSS Programming Guide "
			"(310.6.0 §3.9) states exposure input is \"Only supported by Presets J and K\" and "
			"that \"Preset L always uses AutoExposure\". This %s feature resolves to preset %s. "
			"Set [STRAYDLSS] NgxPreset=11 (K) to make the exposure texture reachable, or "
			"NgxExposure=auto to stop paying for a texture nothing reads.",
			exposure::mode_name(g_exposure_mode), exposure::preset_letter(resolved),
			dlss_quality_name(derived), exposure::preset_letter(resolved));

	result = NGX_D3D12_CREATE_DLSS_EXT(cmd, 1, 1, &g_feature, g_feature_params, &create);
	if (NVSDK_NGX_FAILED(result) || g_feature == nullptr)
	{
		set_error("CreateFeature", result);
		release_feature();
		return false;
	}

	g_feature_desc = desc;
	g_last_error[0] = 0;
	STRAY_LOG_INFO("DLSS feature created: %ux%u -> %ux%u, %s, preset=%d, flags=0x%x",
		desc.render_width, desc.render_height, desc.output_width, desc.output_height,
		dlss_quality_name(derived), preset,
		static_cast<unsigned>(create.InFeatureCreateFlags));
	return true;
}

bool evaluate(ID3D12GraphicsCommandList *cmd, const EvaluateInputs &in)
{
	// The game can re-install the patch with a single QueryInterface at any time; repair
	// immediately before NGX resolves its descriptors. (src/ext_unhook.hpp)
	ext_unhook::repair();
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
	// Every exposure decision comes from one pure function so it is pinned by CI rather than
	// re-derived here. In particular InPreExposure is now GUARDED: the SR path used to forward
	// View row 135.y with no plausibility check while the NR path gated the same row, so a
	// misread reached DLSS silently — and a misread of 0 becomes a literal 1.0 at
	// helpers.h:507, telling DLSS the colour buffer carries no pre-exposure when it carries
	// ~0.45. (src/core/exposure_plan.hpp)
	const exposure::EvalPlan plan = exposure::plan_evaluate(g_exposure_mode,
		in.exposure != nullptr, in.pre_exposure, in.pre_exposure_ok, g_exposure_scale);
	eval.InPreExposure = plan.pre_exposure;
	// NVSDK_NGX_Parameter_ExposureTexture (helpers.h:466). Guide 310.6.0 §3.9: "Only the first
	// channel is sampled in the texture so multiple formats will work" — so the engine's
	// RGBA32F and our own R32_FLOAT are both legal, and §3.4 puts it in the same
	// NON_PIXEL_SHADER_RESOURCE class as colour and depth.
	eval.pInExposureTexture = plan.pass_texture ? in.exposure : nullptr;
	eval.InExposureScale = plan.exposure_scale;

	if (plan.degraded)
	{
		// A creation-time flag cannot be un-set per frame, so this frame gets DLSS's default
		// exposure of 1.0 rather than the auto path. Count it: an exposure experiment whose
		// texture went missing half the time must not read as a clean negative.
		static unsigned int s_degraded = 0;
		if (++s_degraded <= 3 || (s_degraded % 600) == 0)
			STRAY_LOG_WARN("DLSS EXPOSURE degraded: NgxExposure=%s but no exposure texture this "
				"frame (%u so far). AutoExposure is a CREATE flag and cannot be restored per "
				"frame, so DLSS uses its default exposure of 1.0 for these frames.",
				exposure::mode_name(g_exposure_mode), s_degraded);
	}

	const NVSDK_NGX_Result result =
		NGX_D3D12_EVALUATE_DLSS_EXT(cmd, g_feature, g_feature_params, &eval);
	if (NVSDK_NGX_FAILED(result))
	{
		set_error("EvaluateFeature", result);
		return false;
	}

	// Hold the inputs and output alive past this frame. Evaluate only RECORDED the work.
	KeepAlive ka;
	ka.frame = g_eval_frame;
	ka.resources[0] = in.color;
	ka.resources[1] = in.depth;
	ka.resources[2] = in.motion_vectors;
	ka.resources[3] = in.output;
	ka.resources[4] = in.exposure; // null under NgxExposure=auto
	for (ID3D12Resource *r : ka.resources)
		if (r != nullptr)
			r->AddRef();
	g_keep_alive.push_back(ka);

	++g_eval_frame;
	release_keep_alive(/*all=*/false);
	release_probe_feature(/*force=*/false);
	return true;
}

void set_recreate_stable_frames(unsigned int frames)
{
	g_recreate_stable_frames = frames;
	if (frames == 0)
		STRAY_LOG_WARN("NgxRecreateStableFrames=0: a DLSS feature will be recreated on the "
			"FIRST frame that asks for a different rect, which is the pre-2026-09-03 behaviour "
			"and is what produced 6-10 creations per scripted scene transition.");
}

void recreate_counters(unsigned long long &waits, unsigned long long &restarts)
{
	waits = static_cast<unsigned long long>(g_recreate_state.waits);
	restarts = static_cast<unsigned long long>(g_recreate_state.restarts);
}

void shutdown(ID3D12Device *device)
{
	// The features go first, before the capability parameters and the shutdown itself. The
	// caller is responsible for the GPU already being idle.
	release_feature_rr();
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
