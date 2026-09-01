// Driving an NGX "snippet" DLL DIRECTLY, through its own exported NGX API.
//
// Why this exists, and why asking the NGX core was always going to fail: a snippet like the
// leaked nvngx_dlssnr.dll exports the WHOLE NGX D3D12 surface itself — measured on the staged
// build: NVSDK_NGX_D3D12_Init_Ext, CreateFeature, EvaluateFeature, GetFeatureRequirements and
// friends, 55 exports in total. The NGX CORE, meanwhile, resolves features from the snippets
// the driver ships; it knows nothing about a pre-release DLL sitting in the game directory, so
// asking the core to create feature 18 returns FAIL_OutOfDate no matter what we have
// LoadLibrary'd. Measured exactly that: 0xbad0000c. So we bypass the core and call the
// snippet's own exports. (The same shape RTX Remix ships as remix_nvngx.dll, a trampoline
// whose one custom export points NGX at a chosen snippet.)
//
// The identity hook. The staged snippet IMPORTS GetModuleFileNameW (also ...A and
// GetModuleHandleW), i.e. it interrogates module identity, and RenoDX — which drives this same
// runtime successfully on this same machine — patches exactly that import, with error strings
// that spell the method out: "failed to make signed-feature IAT writable", "signed feature has
// no GetModuleFileNameW import; ... not a valid signed NGX runtime". We do not know what
// identity the runtime wants, so this module does not guess: it patches the import, LOGS EVERY
// CALL (which module was asked about, what we returned), and lets [STRAYDLSS] NgxNRIdentity
// select what to answer. The default is pass-through, so the first run OBSERVES the runtime's
// questions without changing a single answer — the call trace is the diagnostic.
#pragma once

#include <cstdint>

struct ID3D12Device;
struct ID3D12GraphicsCommandList;

namespace stray_dlss::snippet {

// THE IDENTITY CHECK, SOLVED (measured 2026-09, live):
//
// The runtime requires the module path it is told about to CONTAIN THE SUBSTRING "nvngx.dll".
// Evidence: with pass-through (our real 'stray-dlss.addon64') it made exactly ONE query and
// failed immediately with FAIL_PlatformError; answering with a path ending in "\nvngx.dll" it
// makes MULTIPLE queries, goes on to interrogate a SECOND module, and proceeds with no
// PlatformError.
//
// This is the answer to "why can't a ReShade add-on just drive DLSS Neural Rendering": the
// snippet refuses to run unless it believes it was loaded by NVIDIA's own nvngx host. It is
// also exactly why RTX Remix names its shim remix_nvngx.dll — that filename CONTAINS
// "nvngx.dll" — and why naming a path after the snippet itself does NOT work:
// "nvngx_dlssnr.dll" does not contain the substring "nvngx.dll".
//
// Hence nvngx_like is the DEFAULT. The other modes are kept for diagnosis.
enum class Identity
{
	passthrough,  // forward to the real API, change nothing, just log (diagnostic)
	snippet_path, // the snippet's own full path — does NOT satisfy the check; diagnostic only
	nvngx_like,   // DEFAULT: the snippet's directory but named "nvngx.dll"
	exe_path,     // the game executable's path (diagnostic)
};

void set_identity(Identity identity);
// "passthrough" | "snippet" | "nvngx" | "exe"; unknown values keep the current setting.
void set_identity_from_string(const char *name);
const char *identity_name();

// LoadLibrary the snippet, resolve its NGX exports, and patch its GetModuleFileNameW import.
// `utf8_path` empty = "nvngx_dlssnr.dll" (resolved by the loader's normal search). Logs the
// resolved module path, every export that did and did not resolve, and the IAT patch outcome.
// Returns true only when the required exports are all present, i.e. the direct path is usable.
bool load(const char *utf8_path);

// True when load() succeeded and the four REQUIRED exports resolved (Init_Ext,
// CreateFeature, EvaluateFeature, ReleaseFeature).
bool available();

// Which OPTIONAL helpers this snippet provides. The parameter block may be allocated by the
// NGX core instead and handed to the snippet — an NVSDK_NGX_Parameter is just an interface
// object, so the allocator and the consumer need not be the same runtime. Whatever allocates
// it MUST also destroy it, so callers pair these two.
bool has_allocate_parameters();
bool has_destroy_parameters();
bool has_shutdown1();

const char *last_error();

// Dumps how many identity queries the snippet made and what it asked about. Safe any time.
void log_identity_calls();

// --- the snippet's own NGX entry points. All return FAIL_NotInitialized when unavailable. ---
// Signatures mirror nvsdk_ngx.h exactly; the opaque NGX types stay void* here so this header
// costs nothing to include.
std::uint32_t init_ext(unsigned long long app_id, const wchar_t *app_data_path,
                       ID3D12Device *device, unsigned int sdk_version, const void *params);
std::uint32_t allocate_parameters(void **out_params);
std::uint32_t get_capability_parameters(void **out_params);
std::uint32_t destroy_parameters(void *params);
std::uint32_t create_feature(ID3D12GraphicsCommandList *cmd, unsigned int feature_id,
                             void *params, void **out_handle);
std::uint32_t evaluate_feature(ID3D12GraphicsCommandList *cmd, const void *handle,
                               const void *params);
std::uint32_t release_feature(void *handle);
std::uint32_t shutdown1(ID3D12Device *device);

} // namespace stray_dlss::snippet
