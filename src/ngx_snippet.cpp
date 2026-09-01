#include "ngx_snippet.hpp"

#include "log.hpp"

#include <Windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>

namespace stray_dlss::snippet {
namespace {

constexpr std::size_t kPathChars = 512;
// NGX result codes we synthesise when the direct path is unusable. 0xBAD00000 | 5 is
// FAIL_NotInitialized in the NGX bitmask scheme; we hardcode it so this file need not pull the
// NGX headers in.
constexpr std::uint32_t kFailNotInitialized = 0xBAD00005u;

using PFN_Init_Ext = std::uint32_t(__cdecl *)(unsigned long long, const wchar_t *,
	ID3D12Device *, unsigned int, const void *);
using PFN_AllocParams = std::uint32_t(__cdecl *)(void **);
using PFN_DestroyParams = std::uint32_t(__cdecl *)(void *);
using PFN_CreateFeature = std::uint32_t(__cdecl *)(ID3D12GraphicsCommandList *, unsigned int,
	void *, void **);
using PFN_EvaluateFeature = std::uint32_t(__cdecl *)(ID3D12GraphicsCommandList *, const void *,
	const void *, void *);
using PFN_ReleaseFeature = std::uint32_t(__cdecl *)(void *);
using PFN_Shutdown1 = std::uint32_t(__cdecl *)(ID3D12Device *);
using PFN_GetModuleFileNameW = DWORD(__stdcall *)(HMODULE, LPWSTR, DWORD);

HMODULE g_module = nullptr;
wchar_t g_module_path[kPathChars] = {};
wchar_t g_module_dir[kPathChars] = {};
wchar_t g_nvngx_like[kPathChars] = {};
wchar_t g_exe_path[kPathChars] = {};
char g_last_error[256] = "";
bool g_available = false;

Identity g_identity = Identity::passthrough;

PFN_Init_Ext g_init_ext = nullptr;
PFN_AllocParams g_alloc_params = nullptr;
PFN_AllocParams g_get_caps = nullptr;
PFN_DestroyParams g_destroy_params = nullptr;
PFN_CreateFeature g_create_feature = nullptr;
PFN_EvaluateFeature g_evaluate_feature = nullptr;
PFN_ReleaseFeature g_release_feature = nullptr;
PFN_Shutdown1 g_shutdown1 = nullptr;

// Identity-hook bookkeeping. The hook can be called from any thread at any time, so everything
// it touches is atomic and it never allocates.
std::atomic<std::uint64_t> g_identity_calls{ 0 };
std::atomic<std::uint64_t> g_identity_calls_self{ 0 };
std::atomic<std::uint64_t> g_identity_calls_null{ 0 };
std::atomic<std::uint64_t> g_identity_calls_other{ 0 };
std::atomic<int> g_identity_logged{ 0 };
constexpr int kIdentityLogBudget = 16;
PFN_GetModuleFileNameW *g_iat_slot = nullptr; // the patched thunk, for teardown/diagnostics

void set_error(const char *fmt, unsigned long value)
{
	std::snprintf(g_last_error, sizeof(g_last_error), fmt, value);
}

void strip_to_directory(wchar_t *path)
{
	wchar_t *last = nullptr;
	for (wchar_t *p = path; *p != 0; ++p)
		if (*p == L'\\' || *p == L'/')
			last = p;
	if (last != nullptr)
		*last = 0;
}

std::size_t wide_copy(wchar_t *dst, std::size_t cap, const wchar_t *src)
{
	std::size_t n = 0;
	while (src[n] != 0 && n + 1 < cap)
	{
		dst[n] = src[n];
		++n;
	}
	dst[n] = 0;
	return n;
}

// Our replacement for the snippet's GetModuleFileNameW import.
//
// It is deliberately OBSERVATIONAL by default: pass-through answers exactly what Windows would,
// so enabling the hook cannot by itself change the runtime's behaviour — only the log grows.
// The other modes exist because we do not know what identity the runtime demands, and the call
// trace this produces is the evidence that will tell us.
DWORD __stdcall hooked_GetModuleFileNameW(HMODULE module, LPWSTR out, DWORD size)
{
	g_identity_calls.fetch_add(1, std::memory_order_relaxed);
	const bool is_self = module == g_module;
	const bool is_null = module == nullptr;
	if (is_self)
		g_identity_calls_self.fetch_add(1, std::memory_order_relaxed);
	else if (is_null)
		g_identity_calls_null.fetch_add(1, std::memory_order_relaxed);
	else
		g_identity_calls_other.fetch_add(1, std::memory_order_relaxed);

	const wchar_t *answer = nullptr;
	switch (g_identity)
	{
	case Identity::snippet_path: answer = g_module_path; break;
	case Identity::nvngx_like:   answer = g_nvngx_like; break;
	case Identity::exe_path:     answer = g_exe_path; break;
	case Identity::passthrough:  break;
	}

	DWORD written = 0;
	if (answer != nullptr && answer[0] != 0 && out != nullptr && size > 0)
	{
		written = static_cast<DWORD>(wide_copy(out, size, answer));
	}
	else
	{
		// Pass-through (or no answer configured): what Windows itself would say. This calls
		// OUR import, not the snippet's patched one, so there is no recursion.
		written = ::GetModuleFileNameW(module, out, size);
	}

	if (g_identity_logged.fetch_add(1, std::memory_order_relaxed) < kIdentityLogBudget)
		STRAY_LOG_INFO("NR identity: snippet asked GetModuleFileNameW(%s%p) -> '%ls' "
			"(mode=%s, %lu chars). This trace is the diagnostic for what the runtime checks.",
			is_self ? "SELF " : (is_null ? "NULL/exe " : "other "), static_cast<void *>(module),
			out != nullptr ? out : L"<null>", identity_name(),
			static_cast<unsigned long>(written));
	return written;
}

// Walks the snippet's import descriptors and swaps the GetModuleFileNameW thunk for ours.
// Mirrors ext_unhook's discipline: every failure is named, with the address and the OS error.
bool patch_identity_import()
{
	auto *base = reinterpret_cast<unsigned char *>(g_module);
	auto *dos = reinterpret_cast<IMAGE_DOS_HEADER *>(base);
	if (dos->e_magic != IMAGE_DOS_SIGNATURE)
	{
		std::snprintf(g_last_error, sizeof(g_last_error), "snippet has no MZ header");
		return false;
	}
	auto *nt = reinterpret_cast<IMAGE_NT_HEADERS64 *>(base + dos->e_lfanew);
	if (nt->Signature != IMAGE_NT_SIGNATURE)
	{
		std::snprintf(g_last_error, sizeof(g_last_error), "snippet has no PE header");
		return false;
	}

	const IMAGE_DATA_DIRECTORY &dir =
		nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
	if (dir.VirtualAddress == 0 || dir.Size == 0)
	{
		std::snprintf(g_last_error, sizeof(g_last_error), "snippet has no import directory");
		return false;
	}

	auto *desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR *>(base + dir.VirtualAddress);
	for (; desc->Name != 0; ++desc)
	{
		// Bound imports leave OriginalFirstThunk zero; the names then live in FirstThunk.
		const DWORD names_rva =
			desc->OriginalFirstThunk != 0 ? desc->OriginalFirstThunk : desc->FirstThunk;
		if (names_rva == 0 || desc->FirstThunk == 0)
			continue;

		auto *name_thunk = reinterpret_cast<IMAGE_THUNK_DATA64 *>(base + names_rva);
		auto *addr_thunk = reinterpret_cast<IMAGE_THUNK_DATA64 *>(base + desc->FirstThunk);

		for (; name_thunk->u1.AddressOfData != 0; ++name_thunk, ++addr_thunk)
		{
			if ((name_thunk->u1.Ordinal & IMAGE_ORDINAL_FLAG64) != 0)
				continue; // imported by ordinal: no name to match
			auto *by_name = reinterpret_cast<IMAGE_IMPORT_BY_NAME *>(
				base + static_cast<std::size_t>(name_thunk->u1.AddressOfData));
			if (std::strcmp(by_name->Name, "GetModuleFileNameW") != 0)
				continue;

			void *slot = &addr_thunk->u1.Function;
			DWORD old_protect = 0;
			if (!::VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &old_protect))
			{
				const unsigned long err = static_cast<unsigned long>(::GetLastError());
				std::snprintf(g_last_error, sizeof(g_last_error),
					"VirtualProtect on the IAT slot %p failed, GetLastError=%lu", slot, err);
				STRAY_LOG_ERROR("NR: failed to make the snippet's IAT writable — %s. "
					"Security software may be protecting nvngx_dlssnr.dll; add an exclusion "
					"for the game folder. NR falls back to the NGX core path.", g_last_error);
				return false;
			}

			g_iat_slot = reinterpret_cast<PFN_GetModuleFileNameW *>(slot);
			*g_iat_slot = &hooked_GetModuleFileNameW;
			DWORD ignored = 0;
			::VirtualProtect(slot, sizeof(void *), old_protect, &ignored);
			::FlushInstructionCache(::GetCurrentProcess(), nullptr, 0);
			STRAY_LOG_WARN("NR: patched the snippet's GetModuleFileNameW import at %p "
				"(identity mode=%s). Every query it makes will be logged.", slot,
				identity_name());
			return true;
		}
	}

	std::snprintf(g_last_error, sizeof(g_last_error),
		"snippet has no GetModuleFileNameW import");
	STRAY_LOG_ERROR("NR: %s — this is not a valid signed NGX runtime (RenoDX reports the same "
		"condition for a wrong file). Continuing without the identity hook.", g_last_error);
	return false;
}

// Optional export: absence is normal and must never abort the direct path. An
// NVSDK_NGX_Parameter is just an interface object — whoever allocates it, the snippet can
// consume it — so a snippet without the parameter helpers is still fully drivable.
template <typename T>
bool resolve_optional(T &fn, const char *name)
{
	fn = reinterpret_cast<T>(reinterpret_cast<void *>(::GetProcAddress(g_module, name)));
	STRAY_LOG_INFO("NR: snippet export %s: %s (optional)",
		fn != nullptr ? "ok" : "absent", name);
	return fn != nullptr;
}

template <typename T>
bool resolve(T &fn, const char *name, int &missing)
{
	fn = reinterpret_cast<T>(
		reinterpret_cast<void *>(::GetProcAddress(g_module, name)));
	if (fn == nullptr)
	{
		++missing;
		STRAY_LOG_ERROR("NR: snippet export MISSING: %s", name);
		return false;
	}
	STRAY_LOG_INFO("NR: snippet export ok: %s", name);
	return true;
}

} // namespace

const char *identity_name()
{
	switch (g_identity)
	{
	case Identity::passthrough:  return "passthrough";
	case Identity::snippet_path: return "snippet";
	case Identity::nvngx_like:   return "nvngx";
	case Identity::exe_path:     return "exe";
	}
	return "?";
}

void set_identity(Identity identity) { g_identity = identity; }

void set_identity_from_string(const char *name)
{
	if (name == nullptr)
		return;
	if (std::strcmp(name, "passthrough") == 0)
		g_identity = Identity::passthrough;
	else if (std::strcmp(name, "snippet") == 0)
		g_identity = Identity::snippet_path;
	else if (std::strcmp(name, "nvngx") == 0)
		g_identity = Identity::nvngx_like;
	else if (std::strcmp(name, "exe") == 0)
		g_identity = Identity::exe_path;
	else if (name[0] != 0)
		STRAY_LOG_WARN("NgxNRIdentity '%s' unknown (passthrough|snippet|nvngx|exe); keeping "
			"%s.", name, identity_name());
}

const char *last_error() { return g_last_error; }
bool available() { return g_available; }
bool has_allocate_parameters() { return g_alloc_params != nullptr; }
bool has_destroy_parameters() { return g_destroy_params != nullptr; }
bool has_shutdown1() { return g_shutdown1 != nullptr; }

void log_identity_calls()
{
	STRAY_LOG_INFO("NR identity queries: total=%llu (self=%llu, null/exe=%llu, other=%llu), "
		"mode=%s",
		static_cast<unsigned long long>(g_identity_calls.load(std::memory_order_relaxed)),
		static_cast<unsigned long long>(g_identity_calls_self.load(std::memory_order_relaxed)),
		static_cast<unsigned long long>(g_identity_calls_null.load(std::memory_order_relaxed)),
		static_cast<unsigned long long>(g_identity_calls_other.load(std::memory_order_relaxed)),
		identity_name());
}

bool load(const char *utf8_path)
{
	if (g_module != nullptr)
		return g_available;

	const char *path = (utf8_path != nullptr && utf8_path[0] != 0) ? utf8_path
		: "nvngx_dlssnr.dll";
	wchar_t wide[kPathChars] = {};
	if (::MultiByteToWideChar(CP_UTF8, 0, path, -1, wide,
			static_cast<int>(kPathChars) - 1) <= 0)
	{
		std::snprintf(g_last_error, sizeof(g_last_error), "bad snippet path '%s'", path);
		STRAY_LOG_ERROR("NR: %s", g_last_error);
		return false;
	}

	g_module = ::LoadLibraryExW(wide, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
	if (g_module == nullptr)
		g_module = ::LoadLibraryW(wide);
	if (g_module == nullptr)
	{
		const unsigned long err = static_cast<unsigned long>(::GetLastError());
		set_error("LoadLibrary failed, GetLastError=%lu", err);
		STRAY_LOG_ERROR("NR: nvngx_dlssnr.dll NOT LOADED from '%s' — %s. Stage NVIDIA's "
			"nvngx_dlssnr.dll beside the game executable or set [STRAYDLSS] NgxNRDll. NR "
			"stays OFF; SR/RR are unaffected.", path, g_last_error);
		return false;
	}

	::GetModuleFileNameW(g_module, g_module_path, static_cast<DWORD>(kPathChars));
	wide_copy(g_module_dir, kPathChars, g_module_path);
	strip_to_directory(g_module_dir);
	std::snprintf(g_last_error, sizeof(g_last_error), "%s", "");

	// The "nvngx.dll-like" identity: the snippet's own directory, renamed.
	{
		const std::size_t n = wide_copy(g_nvngx_like, kPathChars, g_module_dir);
		const wchar_t *tail = L"\\nvngx.dll";
		std::size_t i = n;
		for (std::size_t k = 0; tail[k] != 0 && i + 1 < kPathChars; ++k, ++i)
			g_nvngx_like[i] = tail[k];
		g_nvngx_like[i] = 0;
	}
	::GetModuleFileNameW(nullptr, g_exe_path, static_cast<DWORD>(kPathChars));

	STRAY_LOG_WARN("NR: snippet LOADED from '%ls' (requested '%s'). Driving it DIRECTLY "
		"through its own NGX exports — the NGX core does not know this pre-release snippet, which is "
		"why asking the core for feature 18 returned FAIL_OutOfDate.", g_module_path, path);

	// REQUIRED: exactly the four calls needed to DRIVE the feature. Measured on the staged
	// build, the snippet exports all four but none of the parameter helpers — and it does not
	// need to: the parameter block can come from the NGX core (which we already allocate from
	// for SR/RR) and be handed to the snippet's CreateFeature/EvaluateFeature.
	int missing = 0;
	resolve(g_init_ext, "NVSDK_NGX_D3D12_Init_Ext", missing);
	resolve(g_create_feature, "NVSDK_NGX_D3D12_CreateFeature", missing);
	resolve(g_evaluate_feature, "NVSDK_NGX_D3D12_EvaluateFeature", missing);
	resolve(g_release_feature, "NVSDK_NGX_D3D12_ReleaseFeature", missing);

	// OPTIONAL: preferred when present, but their absence must not abort anything.
	// Shutdown1 is optional-but-preferred; without it we simply skip the snippet's own
	// shutdown (the device is torn down immediately afterwards anyway).
	resolve_optional(g_shutdown1, "NVSDK_NGX_D3D12_Shutdown1");
	resolve_optional(g_alloc_params, "NVSDK_NGX_D3D12_AllocateParameters");
	resolve_optional(g_destroy_params, "NVSDK_NGX_D3D12_DestroyParameters");
	resolve_optional(g_get_caps, "NVSDK_NGX_D3D12_GetCapabilityParameters");

	// The identity hook is best-effort: without it the runtime may refuse, but the direct
	// export path is still worth attempting and the failure is named.
	patch_identity_import();

	g_available = missing == 0;
	if (!g_available)
		STRAY_LOG_ERROR("NR: %d REQUIRED snippet export(s) missing (of Init_Ext, "
			"CreateFeature, EvaluateFeature, ReleaseFeature) — the direct path is unusable; "
			"NR falls back to the NGX core path.", missing);
	else
		STRAY_LOG_WARN("NR: all required snippet exports resolved; the DIRECT snippet path is "
			"available.");
	return g_available;
}

std::uint32_t init_ext(unsigned long long app_id, const wchar_t *app_data_path,
                       ID3D12Device *device, unsigned int sdk_version, const void *params)
{
	if (g_init_ext == nullptr)
		return kFailNotInitialized;
	return g_init_ext(app_id, app_data_path, device, sdk_version, params);
}

std::uint32_t allocate_parameters(void **out_params)
{
	if (g_alloc_params == nullptr)
		return kFailNotInitialized;
	return g_alloc_params(out_params);
}

std::uint32_t get_capability_parameters(void **out_params)
{
	if (g_get_caps == nullptr)
		return kFailNotInitialized;
	return g_get_caps(out_params);
}

std::uint32_t destroy_parameters(void *params)
{
	if (g_destroy_params == nullptr)
		return kFailNotInitialized;
	return g_destroy_params(params);
}

std::uint32_t create_feature(ID3D12GraphicsCommandList *cmd, unsigned int feature_id,
                             void *params, void **out_handle)
{
	if (g_create_feature == nullptr)
		return kFailNotInitialized;
	return g_create_feature(cmd, feature_id, params, out_handle);
}

std::uint32_t evaluate_feature(ID3D12GraphicsCommandList *cmd, const void *handle,
                               const void *params)
{
	if (g_evaluate_feature == nullptr)
		return kFailNotInitialized;
	return g_evaluate_feature(cmd, handle, params, nullptr);
}

std::uint32_t release_feature(void *handle)
{
	if (g_release_feature == nullptr)
		return kFailNotInitialized;
	return g_release_feature(handle);
}

std::uint32_t shutdown1(ID3D12Device *device)
{
	if (g_shutdown1 == nullptr)
		return kFailNotInitialized;
	return g_shutdown1(device);
}

} // namespace stray_dlss::snippet
