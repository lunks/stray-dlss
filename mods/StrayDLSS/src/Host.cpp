#include "Host.hpp"

#include "Platform.hpp"
#include "Version.hpp"

#include "app/dlss_app.hpp"
#include "backend_native/native_backend.hpp"
#include "backend_native/present_owner.hpp"
#include "host/config.hpp"
#include "host/ini.hpp"
#include "intercept/backend.hpp"
#include "log.hpp"
#include "ngx_nr.hpp"
#include "pool_name_hook.hpp"

#include "TweakState.hpp"

#include <MinHook.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>

namespace stray_dlss::plugin {
namespace {

using PFN_D3D12CreateDevice = HRESULT(WINAPI *)(IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **);

PFN_D3D12CreateDevice g_orig_create_device = nullptr;
std::atomic<bool> g_minhook_up{ false };
std::atomic<bool> g_hook_installed{ false };
std::atomic<int> g_devices_seen{ 0 };
// Our own D3D12CreateDevice calls (none today; the unwrap creates a resource, not a device)
// must not count as the game's and must not recurse.
thread_local int t_in_host = 0;

// The device the application is currently attached to (the REAL one), and whether its
// destruction has been seen since.
std::mutex g_device_mutex;
ID3D12Device *g_device = nullptr;
std::atomic<bool> g_device_dead{ false };

// ---- config: <mod>/StrayDLSS.ini, [STRAYDLSS], hot-reloaded ----

host::IniFile g_ini;
std::string g_ini_path;
std::atomic<bool> g_native_mode_overridden{ false };
// [STRAYDLSS] TweakUi. Read once in Start(), because the tab is registered once and a key that
// could turn the whole path off mid-session would be a second thing to reason about, not a
// smaller one.
std::atomic<bool> g_tweak_ui{ true };

struct IniSource final : host::cfg::Source
{
	bool get(const char *key, char *buf, std::size_t *size) override
	{
		if (key == nullptr || buf == nullptr || size == nullptr)
			return false;
		std::string v;
		// This host HAS no other backend: the native one drives or nothing does. A file that
		// says otherwise is honoured nowhere and said so once.
		if (std::strcmp(key, "NativeMode") == 0)
		{
			std::string file_value;
			if (g_ini.get("STRAYDLSS", key, file_value) && file_value != "drive" && !g_native_mode_overridden.exchange(true))
				STRAY_LOG_WARN("StrayDLSS.ini says NativeMode=%s; the UE4SS host has no ReShade backend to fall back to, so it is drive.",
					file_value.c_str());
			v = "drive";
		}
		else if (!g_ini.get("STRAYDLSS", key, v))
		{
			return false;
		}
		if (v.size() + 1 > *size)
		{
			*size = v.size() + 1;
			return false;
		}
		std::memcpy(buf, v.c_str(), v.size() + 1);
		return true;
	}
};
IniSource g_source;

// ---- the device ----

// The module that owns a code address, for the log: d3d12core.dll (vkd3d's real device) or a
// proxy's DLL (ReShade's dxgi.dll).
void owner_of(const void *addr, char *out, std::size_t n)
{
	HMODULE owner = nullptr;
	::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		static_cast<LPCWSTR>(addr), &owner);
	if (owner == nullptr)
	{
		std::snprintf(out, n, "<unknown>");
		return;
	}
	char path[MAX_PATH] = {};
	::GetModuleFileNameA(owner, path, MAX_PATH);
	const char *base = path;
	for (const char *p = path; *p; ++p)
		if (*p == '\\' || *p == '/')
			base = p + 1;
	std::snprintf(out, n, "%s", base[0] ? base : "<unnamed>");
}

// The REAL device behind whatever D3D12CreateDevice returned to us. With ReShade in the process
// and its export hook BELOW ours, `dev` is ReShade's proxy; resources are never proxied on
// D3D12, so a throwaway buffer's GetDevice answers with the original vkd3d device (the probe
// build measured exactly this path). Without ReShade the two are the same object.
ID3D12Device *unwrap(ID3D12Device *dev)
{
	++t_in_host;
	native::OwnCodeScope own;
	ID3D12Device *real = nullptr;
	D3D12_HEAP_PROPERTIES hp = {};
	hp.Type = D3D12_HEAP_TYPE_DEFAULT;
	D3D12_RESOURCE_DESC bd = {};
	bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	bd.Width = 256; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
	bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	ID3D12Resource *buf = nullptr;
	if (SUCCEEDED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&buf))) && buf != nullptr)
	{
		if (FAILED(buf->GetDevice(IID_PPV_ARGS(&real))))
			real = nullptr;
		buf->Release();
	}
	--t_in_host;
	if (real != nullptr)
		real->Release(); // the device outlives us; a borrowed pointer is what every hook holds
	return real != nullptr ? real : dev;
}

// A device-destruction sentinel, the resource registry's trick on the device itself: released
// by the runtime when the device dies, so a device recreate at the SAME address (the game
// destroys and recreates its first device at startup, facts §14) is not mistaken for the old one.
// {7E2C1A04-3B5D-4F6A-9C81-55D0E1F2A3B4}
constexpr GUID kDeviceSentinelGuid = { 0x7e2c1a04, 0x3b5d, 0x4f6a, { 0x9c, 0x81, 0x55, 0xd0, 0xe1, 0xf2, 0xa3, 0xb4 } };
struct DeviceSentinel final : IUnknown
{
	std::atomic<ULONG> refs{ 1 };
	ID3D12Device *device = nullptr;
	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override
	{
		if (out == nullptr)
			return E_POINTER;
		if (riid == __uuidof(IUnknown)) { *out = this; ++refs; return S_OK; }
		*out = nullptr;
		return E_NOINTERFACE;
	}
	ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
	ULONG STDMETHODCALLTYPE Release() override
	{
		const ULONG r = --refs;
		if (r == 0)
		{
			STRAY_LOG_WARN("host: the D3D12 device %p is being DESTROYED (its private-data sentinel was released). "
				"The next D3D12CreateDevice is a new device.", static_cast<void *>(device));
			g_device_dead.store(true);
			delete this;
		}
		return r;
	}
};

// The DXGI factory the game uses to create its swapchain. present::install patches a THROWAWAY
// factory, which only reaches the game's swapchain if DXVK shares one vtable per factory class
// (measured FALSE on the first box run: our throwaway patch never fired for the game's
// swapchain). So hook the CreateDXGIFactory* EXPORTS and patch the REAL factory the game
// receives - the same export-detour trick used for D3D12CreateDevice.
using PFN_CreateDXGIFactory = HRESULT(WINAPI *)(REFIID, void **);
using PFN_CreateDXGIFactory2 = HRESULT(WINAPI *)(UINT, REFIID, void **);
PFN_CreateDXGIFactory g_orig_CreateDXGIFactory = nullptr;
PFN_CreateDXGIFactory g_orig_CreateDXGIFactory1 = nullptr;
PFN_CreateDXGIFactory2 g_orig_CreateDXGIFactory2 = nullptr;
std::atomic<bool> g_dxgi_hooks_installed{ false };

void note_factory_out(void **out)
{
	if (out != nullptr && *out != nullptr && !native::in_own_code())
		native::present::note_factory(reinterpret_cast<IUnknown *>(*out));
}
HRESULT WINAPI hk_CreateDXGIFactory(REFIID riid, void **out)
{
	const HRESULT hr = g_orig_CreateDXGIFactory(riid, out);
	if (SUCCEEDED(hr)) note_factory_out(out);
	return hr;
}
HRESULT WINAPI hk_CreateDXGIFactory1(REFIID riid, void **out)
{
	const HRESULT hr = g_orig_CreateDXGIFactory1(riid, out);
	if (SUCCEEDED(hr)) note_factory_out(out);
	return hr;
}
HRESULT WINAPI hk_CreateDXGIFactory2(UINT flags, REFIID riid, void **out)
{
	const HRESULT hr = g_orig_CreateDXGIFactory2(flags, riid, out);
	if (SUCCEEDED(hr)) note_factory_out(out);
	return hr;
}

void install_dxgi_factory_hooks()
{
	if (g_dxgi_hooks_installed.exchange(true))
		return;
	HMODULE dxgi = ::GetModuleHandleW(L"dxgi.dll");
	if (dxgi == nullptr)
	{
		STRAY_LOG_WARN("host: dxgi.dll not loaded at device attach; the game's factory cannot be export-hooked (present owner relies on the throwaway-factory patch only)");
		return;
	}
	const auto hook = [&](const char *name, void *replacement, void **orig) {
		void *target = reinterpret_cast<void *>(::GetProcAddress(dxgi, name));
		if (target == nullptr)
			return;
		if (MH_CreateHook(target, replacement, orig) == MH_OK && MH_EnableHook(target) == MH_OK)
			STRAY_LOG_INFO("host: hooked %s for the present owner", name);
		else
			STRAY_LOG_WARN("host: could not hook %s", name);
	};
	hook("CreateDXGIFactory", reinterpret_cast<void *>(&hk_CreateDXGIFactory), reinterpret_cast<void **>(&g_orig_CreateDXGIFactory));
	hook("CreateDXGIFactory1", reinterpret_cast<void *>(&hk_CreateDXGIFactory1), reinterpret_cast<void **>(&g_orig_CreateDXGIFactory1));
	hook("CreateDXGIFactory2", reinterpret_cast<void *>(&hk_CreateDXGIFactory2), reinterpret_cast<void **>(&g_orig_CreateDXGIFactory2));
	// The game very likely already created its factory before our device hook fired (it makes
	// the swapchain right after the device). Patch every EXISTING factory we can reach: the one
	// behind the swapchain does not exist yet, but a factory the game keeps will be reused.
	STRAY_LOG_INFO("host: DXGI factory export hooks installed; new factories will be patched as the game creates them");
}

void attach_to(ID3D12Device *real, ID3D12Device *as_returned)
{
	app::DlssApp &a = app::instance();
	{
		std::lock_guard<std::mutex> lock(g_device_mutex);
		if (g_device != nullptr)
		{
			// A previous device. If its death was seen, only the hooks and the present owner
			// need undoing (they hold its pointer); the application's own device teardown
			// (NGX shutdown) is skipped, because the object is gone.
			if (g_device_dead.load())
			{
				STRAY_LOG_WARN("host: previous device %p died; re-attaching to %p", static_cast<void *>(g_device), static_cast<void *>(real));
				if (a.status().ngx_attempted)
					STRAY_LOG_ERROR("host: NGX had been initialised on the dead device; its state is now stale. This is not the "
						"startup recreate (which precedes NGX) and is not handled.");
				native::present::uninstall();
				native::uninstall();
			}
			else
			{
				STRAY_LOG_WARN("host: a second live device %p while %p is attached; detaching from the first",
					static_cast<void *>(real), static_cast<void *>(g_device));
				native::present::uninstall();
				a.on_device(g_device, false);
			}
		}
		g_device = real;
		g_device_dead.store(false);
	}
	// The sentinel rides on the REAL device.
	{
		auto *s = new DeviceSentinel();
		s->device = real;
		if (FAILED(real->SetPrivateDataInterface(kDeviceSentinelGuid, s)))
			STRAY_LOG_WARN("host: SetPrivateDataInterface on the device refused; a recreate at the same address would go unnoticed");
		s->Release();
	}
	// The seam: the native backend alone answers every question; the hooks deliver to the app.
	native::set_sink(&a);
	icept::set_backend(&native::backend());
	a.on_device(real, true); // reads NativeMode (forced to drive here) and installs the hooks
	if (native::mode() != native::Mode::drive)
	{
		STRAY_LOG_ERROR("host: the native backend is %s, not drive - nothing will drive DLSS this session (see the install report above)",
			native::mode_name(native::mode()));
		return;
	}
	install_dxgi_factory_hooks();
	if (!native::present::install(real))
		STRAY_LOG_ERROR("host: the present owner did not install - no frame boundary, so NGX will never initialise and nothing at present time runs");
	STRAY_LOG_WARN("host: attached. real device %p%s; seam backend=%s; %s", static_cast<void *>(real),
		as_returned != real ? " (unwrapped from a proxy)" : "", icept::backend()->name(), native::present::report());
}

HRESULT WINAPI hook_create_device(IUnknown *adapter, D3D_FEATURE_LEVEL fl, REFIID riid, void **out)
{
	const HRESULT hr = g_orig_create_device(adapter, fl, riid, out);
	if (t_in_host > 0 || FAILED(hr) || out == nullptr || *out == nullptr)
		return hr;
	const int n = ++g_devices_seen;
	ID3D12Device *dev = nullptr;
	if (FAILED(static_cast<IUnknown *>(*out)->QueryInterface(IID_PPV_ARGS(&dev))) || dev == nullptr)
	{
		STRAY_LOG_WARN("host: D3D12CreateDevice #%d returned something that is not an ID3D12Device; ignored", n);
		return hr;
	}
	char owner_dev[MAX_PATH], owner_real[MAX_PATH];
	owner_of((*reinterpret_cast<void ***>(dev))[0], owner_dev, sizeof(owner_dev));
	ID3D12Device *real = unwrap(dev);
	owner_of((*reinterpret_cast<void ***>(real))[0], owner_real, sizeof(owner_real));
	STRAY_LOG_WARN("host: D3D12CreateDevice #%d hr=0x%08lx fl=0x%x -> %p (vtable in %s); real device %p (vtable in %s)%s",
		n, static_cast<unsigned long>(hr), static_cast<unsigned>(fl), static_cast<void *>(dev), owner_dev,
		static_cast<void *>(real), owner_real, real != dev ? " - A PROXY SITS ABOVE US (ReShade)" : "");
	dev->Release();
	bool attach = true;
	{
		std::lock_guard<std::mutex> lock(g_device_mutex);
		if (g_device == real && !g_device_dead.load())
		{
			// vkd3d hands the same object back for a second D3D12CreateDevice while the first is
			// alive (facts §11): the same device, already attached.
			attach = false;
		}
	}
	if (attach)
		attach_to(real, dev);
	else
		STRAY_LOG_INFO("host: the same live device again; nothing to do");
	return hr;
}

// The trampoline installer handed to src/pool_name_hook.cpp. MinHook validates the target's
// prologue itself (hde64) and returns MH_ERROR_UNSUPPORTED_FUNCTION rather than corrupting a
// function it cannot relocate, which is one more reason a wrong target refuses instead of
// crashing.
bool pool_install_hook(void *target, void *replacement, void **original)
{
	if (target == nullptr || replacement == nullptr || original == nullptr)
		return false;
	if (MH_CreateHook(target, replacement, original) != MH_OK)
		return false;
	if (MH_EnableHook(target) != MH_OK)
	{
		MH_RemoveHook(target);
		return false;
	}
	return true;
}

bool pool_remove_hook(void *target)
{
	if (target == nullptr)
		return false;
	MH_DisableHook(target);
	return MH_RemoveHook(target) == MH_OK;
}

bool install_device_hook()
{
	if (g_hook_installed.load())
		return true;
	// DO NOT force-load d3d12.dll. The probe measured (facts §12) that at start_mod the game has
	// NOT loaded it yet, and it poll-installed the hook from on_update once the game did — which
	// ran without incident. Force-loading it ourselves from start_mod is what the FIRST host
	// build did, and it crashed the process ~11 s in with c0000005 in a wine builtin, ReShade's
	// log ending exactly at "d3d12.dll ... Just loaded via LoadLibrary('...main.dll')": loading
	// it ~970 ms early, and triggering ReShade's delayed d3d12-hook install at that point, is not
	// survivable here. So wait for the game to load it, exactly as the probe did.
	HMODULE d3d12 = ::GetModuleHandleW(L"d3d12.dll");
	if (d3d12 == nullptr)
		return false; // not yet; Tick() polls
	const bool already_loaded = true;
	if (!g_minhook_up.load())
	{
		const MH_STATUS st = MH_Initialize();
		if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED)
		{
			STRAY_LOG_ERROR("host: MH_Initialize failed (%d)", static_cast<int>(st));
			return false;
		}
		g_minhook_up.store(true);
		// THE ONLY TRAMPOLINE INSTALLER IN THE PROCESS, and it is registered here rather than
		// linked into stray_dlss_native on purpose. That library is shared with the ReShade
		// add-on host, which does not own MinHook; a mechanism that needs to patch engine CODE
		// must therefore ask its host for a patcher and be told "no" cleanly. Today the only
		// caller is the render-target pool's name hook ([STRAYDLSS] PoolNames >= 2), which
		// installs nothing unless its own static scan cleared the agreement bar first.
		poolhook::set_installer(&pool_install_hook, &pool_remove_hook);
	}
	void *target = reinterpret_cast<void *>(::GetProcAddress(d3d12, "D3D12CreateDevice"));
	if (target == nullptr)
	{
		STRAY_LOG_ERROR("host: D3D12CreateDevice is not exported by d3d12.dll?!");
		return false;
	}
	char owner[MAX_PATH];
	owner_of(target, owner, sizeof(owner));
	const unsigned char *b = static_cast<const unsigned char *>(target);
	const bool detoured = b[0] == 0xE9 || (b[0] == 0xFF && b[1] == 0x25);
	STRAY_LOG_INFO("host: d3d12.dll %s (%p), D3D12CreateDevice=%p in %s, first bytes %02X %02X %02X -> %s",
		already_loaded ? "was already loaded" : "loaded by us", static_cast<void *>(d3d12), target, owner, b[0], b[1], b[2],
		detoured ? "ALREADY DETOURED (ReShade's hook is below ours; we will see its proxy and unwrap it)" : "pristine (we are the first hook)");
	const MH_STATUS c = MH_CreateHook(target, reinterpret_cast<void *>(&hook_create_device), reinterpret_cast<void **>(&g_orig_create_device));
	const MH_STATUS e = c == MH_OK ? MH_EnableHook(target) : c;
	if (c != MH_OK || e != MH_OK)
	{
		STRAY_LOG_ERROR("host: MH_CreateHook=%d MH_EnableHook=%d", static_cast<int>(c), static_cast<int>(e));
		return false;
	}
	g_hook_installed.store(true);
	STRAY_LOG_INFO("host: D3D12CreateDevice detoured; waiting for the game's device");
	return true;
}

} // namespace

// THE DLSS ON-SCREEN INDICATOR, AND WHY IT HAS TO BE SET FROM IN HERE.
//
// The indicator is gated on HKLM\SOFTWARE\NVIDIA Corporation\Global\NGXCore\ShowDlssIndicator
// = 0x400 (CLAUDE.md §5). Setting it in the Proton prefix with the game closed does not work, and
// MEASURED 2026-09-04 is why: PROTON_NVIDIA_LIBS=1 REINSTALLS the NVIDIA wine NGX libraries and
// REWRITES that whole registry block on every launch. Our value was verified written with the
// game down and the wineserver down, and came back dword:00000000 with a fresh block timestamp
// and freshly dated nvngx.dll / _nvngx.dll. So the race is not with wineserver, it is with
// Proton's own setup, and nothing outside the process can win it.
//
// Inside the process we can. This runs at start_mod, which UE4SS calls ~970 ms before the game's
// D3D12CreateDevice (facts §12) and long before NGX initialises, so it lands after Proton has
// finished writing and before anything reads.
//
// [STRAYDLSS] NgxIndicator / NgxIndicatorFG: -1 (default) leaves the key exactly as found, 0
// writes off, 1 writes on. Default is "do not touch" on purpose: this is the one thing in the
// plugin that writes OUTSIDE the game directory, into a registry hive shared with whatever else
// uses NGX in this prefix, so it happens only when asked for by name.
static void apply_ngx_indicator_keys()
{
	struct Knob { const char *key; const wchar_t *value; unsigned on; };
	static const Knob knobs[] = {
		{ "NgxIndicator",   L"ShowDlssIndicator",  0x400u }, // DLSS SR/RR overlay
		{ "NgxIndicatorFG", L"DLSSG_IndicatorText", 1u    }, // frame-generation text
	};

	for (const Knob &k : knobs)
	{
		const int want = host::cfg::get_int(k.key, -1);
		if (want < 0)
			continue; // left exactly as found, which is the default

		HKEY hkey = nullptr;
		const LSTATUS open = RegCreateKeyExW(HKEY_LOCAL_MACHINE,
			L"SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore", 0, nullptr,
			REG_OPTION_NON_VOLATILE, KEY_SET_VALUE | KEY_QUERY_VALUE, nullptr, &hkey, nullptr);
		if (open != ERROR_SUCCESS || hkey == nullptr)
		{
			STRAY_LOG_ERROR("host: [STRAYDLSS] %s=%d but the NGXCore registry key could not be "
				"opened (RegCreateKeyEx = %ld). The indicator will not appear.", k.key, want, (long)open);
			continue;
		}

		DWORD before = 0, cb = sizeof(before), type = 0;
		const bool had = RegQueryValueExW(hkey, k.value, nullptr, &type,
			reinterpret_cast<BYTE *>(&before), &cb) == ERROR_SUCCESS && type == REG_DWORD;

		DWORD value = want ? k.on : 0u;
		const LSTATUS set = RegSetValueExW(hkey, k.value, 0, REG_DWORD,
			reinterpret_cast<const BYTE *>(&value), sizeof(value));
		RegCloseKey(hkey);

		if (set == ERROR_SUCCESS)
			STRAY_LOG_WARN("host: NGX indicator: wrote %ls = 0x%08lX (was %s0x%08lX). Proton rewrites this "
				"block on every launch, which is why it is set from in here rather than in the prefix.",
				k.value, (unsigned long)value, had ? "" : "absent, ", (unsigned long)before);
		else
			STRAY_LOG_ERROR("host: NGX indicator: RegSetValueEx(%ls) failed (%ld); the indicator will "
				"not appear.", k.value, (long)set);
	}
}

void Start(const std::wstring &mod_dir, const std::wstring &game_dir)
{
	log::init_file_sink((game_dir + L"stray-dlss-plugin.log").c_str());
	STRAY_LOG_INFO("StrayDLSS %s attaching (UE4SS C++ mod, the HOST): mod dir %s, game dir %s",
		STRAY_DLSS_PLUGIN_VERSION_STRING, sds::Narrow(mod_dir).c_str(), sds::Narrow(game_dir).c_str());

	// WHAT A CRASH DUMP WILL CALL US. A UE4 dump line is `<module> 0x<base> + <offset>` and
	// nothing else, so the DLL's filename and its load address ARE the identity. Until
	// 2026-09-03 both C++ plugins shipped as dlls/main.dll and every dump said `main`; working
	// out which mod that was cost real time. We now ship dlls/StrayDLSS.dll (UE4SS falls back to
	// dlls/<ModName>.dll - CMakeLists.txt has the chain), so the name should read `StrayDLSS`.
	//
	// This line is also the detector for the one silent failure of that scheme: UE4SS prefers
	// dlls/main.dll whenever it exists, so a stale one left behind by an older install is loaded
	// INSTEAD of this build, and nothing else would say so.
	{
		const void *self_base = nullptr;
		std::wstring self_path;
		if (sds::ModuleIdentity(reinterpret_cast<const void *>(&Start), self_base, self_path))
		{
			const std::string path = sds::Narrow(self_path);
			const size_t slash = path.find_last_of("\\/");
			std::string file = slash == std::string::npos ? path : path.substr(slash + 1);
			const size_t dot = file.find_last_of('.');
			const std::string module_name = dot == std::string::npos ? file : file.substr(0, dot);
			STRAY_LOG_INFO("module identity: StrayDLSS is loaded at base 0x%016llx from %s -- a UE4 crash "
				"dump will name this module `%s`, so a `%s 0x<base> + <offset>` line with that base is "
				"OURS and the offset is the RVA; symbolize it against StrayDLSS.pdb.%s",
				reinterpret_cast<unsigned long long>(self_base), path.c_str(),
				module_name.c_str(), module_name.c_str(),
				module_name == "StrayDLSS"
					? ""
					: " WARNING: that is not `StrayDLSS`. A stale dlls/main.dll is being loaded in "
					  "preference to dlls/StrayDLSS.dll - delete it, or dumps stay ambiguous.");
		}
		else
			STRAY_LOG_WARN("module identity: GetModuleHandleEx could not name our own module; a crash "
				"dump's `<module> + <offset>` line cannot be attributed to StrayDLSS from this log");
	}

	// The DLL lives at <Mod>/dlls/StrayDLSS.dll but StrayDLSS.ini sits at the mod ROOT (<Mod>/), the
	// UE4SS convention. mod_dir is the dll's directory, so try the parent first, then the dll
	// dir as a fallback. (Measured 2026-09-02: the first host run looked only in dlls/, found
	// nothing, and every key silently took its default - which left EnableNGX and NgxEvaluate
	// OFF, so DLSS never ran.)
	std::wstring root = mod_dir; // ".../StrayDLSS/dlls/"
	if (root.size() >= 2)
	{
		root.pop_back(); // trailing separator
		const size_t slash = root.find_last_of(L"\\/");
		if (slash != std::wstring::npos)
			root = root.substr(0, slash + 1); // ".../StrayDLSS/"
	}
	const std::wstring candidates[] = { root + L"StrayDLSS.ini", mod_dir + L"StrayDLSS.ini" };
	bool loaded = false;
	for (const std::wstring &c : candidates)
	{
		g_ini_path = sds::Narrow(c);
		if (g_ini.load(g_ini_path))
		{
			STRAY_LOG_INFO("host: %s loaded (%zu values), hot-reloaded from on_update", g_ini_path.c_str(), g_ini.size());
			loaded = true;
			break;
		}
	}
	if (!loaded)
	{
		STRAY_LOG_ERROR("host: StrayDLSS.ini not found (tried %s and the dll dir); every [STRAYDLSS] key reads its "
			"default, which leaves EnableNGX/NgxEvaluate OFF and DLSS inert.", sds::Narrow(candidates[0]).c_str());
		// The loop above leaves the LAST candidate it tried in g_ini_path. An unfound file has no
		// path, and IniPath()'s one caller - the tuning tab's save - must refuse rather than
		// create a second ini beside a file the session never read.
		g_ini_path.clear();
	}
	host::cfg::set_source(&g_source);

	// Before anything can initialise NGX. See apply_ngx_indicator_keys.
	apply_ngx_indicator_keys();

	g_tweak_ui.store(host::cfg::get_bool("TweakUi", true));
	STRAY_LOG_INFO("host: [STRAYDLSS] TweakUi=%d - the in-game tuning tab %s. It lives in UE4SS's "
		"own debug GUI, which at UE4SS 68caddcf renders into a SEPARATE OS window (there is no "
		"in-game overlay path at that commit): open it with Ctrl+<[Debug] ToggleGUIKey, default "
		"O> and only when [Debug] GuiConsoleEnabled=1. Whether that window is usable under "
		"gamescope's DRM backend is UNCONFIRMED - if it is not, hand-editing this ini is the "
		"same control path: every key the tab exposes is now re-applied on reload.",
		g_tweak_ui.load() ? 1 : 0, g_tweak_ui.load() ? "is registered" : "is DISABLED");

	// The application's one-time configuration (which events a host must deliver; here the
	// native hooks deliver them all, so the answer is only logged).
	const app::EventNeeds needs = app::instance().configure_events();
	STRAY_LOG_INFO("host: event needs: pipeline=%d (the native hooks deliver pipelines, binds, resets and "
		"dispatches, which is now the whole seam)", needs.pipeline_events ? 1 : 0);

	if (install_device_hook())
		STRAY_LOG_INFO("host: d3d12.dll was already loaded at start_mod; hook installed now");
	else
		STRAY_LOG_WARN("host: d3d12.dll is not loaded yet at start_mod (expected, facts §12); polling for it from on_update. "
			"NOT force-loading it - doing so from start_mod crashed the process in the first host build.");
}

void Tick()
{
	if (!g_hook_installed.load())
		install_device_hook();
	if (g_ini.reload_if_changed())
	{
		STRAY_LOG_INFO("host: %s changed on disk and was re-read (%zu values); keys read at startup keep their old value",
			g_ini_path.c_str(), g_ini.size());

		// ...with ONE exception, and it is deliberate: NgxNR. It is the switch a live A/B needs,
		// this host has no overlay to flip it from, and turning DLSS Neural Rendering off is the
		// transition whose safety the deferred-teardown work exists to guarantee
		// (src/core/nr_lifetime.hpp). Re-applying it here is also the honest test of that
		// guarantee, because THIS IS NOT THE RENDER THREAD: UE4SS calls Tick from its own thread
		// while the game is recording and submitting frames. nr::set_enabled destroys nothing on
		// the caller's thread by design — it queues, and nr::on_present carries the teardown out
		// once the GPU fence has passed the last evaluate.
		//
		// ...and, since 2026-09-04, every knob the live-tuning tab exposes (TweakState.hpp) -
		// which is the SAME argument extended, not a new one: not the render thread, plain
		// scalar setters, values re-sent to the NGX parameter block on every evaluate, so the
		// change lands on the next frame with no feature recreation and no lock on the render
		// path. It is what keeps this file a WORKING control channel on a box where UE4SS's
		// debug GUI - a separate OS window, not an overlay - may not be openable at all.
		//
		// Every OTHER key stays startup-only. A knob that reconfigures the device or the
		// feature mid-frame is a much larger promise than a value that rides the next evaluate.
		if (g_tweak_ui.load())
		{
			const int moved = tweak::apply_from_config();
			if (moved > 0)
				STRAY_LOG_WARN("host: %d live tuning value(s) re-applied from %s.", moved, g_ini_path.c_str());
		}

		const bool want = host::cfg::get_bool("NgxNR", nr::enabled());
		if (want != nr::enabled())
		{
			STRAY_LOG_WARN("host: NgxNR changed to %d in %s. Applied from the UE4SS tick thread, "
				"which is NOT the render thread: the teardown is queued and performed at the "
				"present boundary once the fence has passed the last evaluate.",
				want ? 1 : 0, g_ini_path.c_str());
			nr::set_enabled(want);
		}
	}
}

const std::string &IniPath()
{
	return g_ini_path;
}

bool TweakUiEnabled()
{
	return g_tweak_ui.load();
}

void Stop()
{
	STRAY_LOG_INFO("StrayDLSS detaching");
	app::instance().log_final_census(true, true);
	native::present::uninstall();
	app::instance().shutdown();
	if (g_minhook_up.load())
	{
		MH_DisableHook(MH_ALL_HOOKS);
		MH_Uninitialize();
		g_minhook_up.store(false);
	}
	g_hook_installed.store(false);
	host::cfg::set_source(nullptr);
	log::shutdown_file_sink();
}

} // namespace stray_dlss::plugin
