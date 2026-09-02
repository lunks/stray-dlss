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

#include <MinHook.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>

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

bool install_device_hook()
{
	if (g_hook_installed.load())
		return true;
	HMODULE d3d12 = ::GetModuleHandleW(L"d3d12.dll");
	const bool already_loaded = d3d12 != nullptr;
	if (d3d12 == nullptr)
		d3d12 = ::LoadLibraryW(L"d3d12.dll");
	if (d3d12 == nullptr)
	{
		STRAY_LOG_ERROR("host: d3d12.dll could not be loaded (error %lu); nothing can be hooked", ::GetLastError());
		return false;
	}
	if (!g_minhook_up.load())
	{
		const MH_STATUS st = MH_Initialize();
		if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED)
		{
			STRAY_LOG_ERROR("host: MH_Initialize failed (%d)", static_cast<int>(st));
			return false;
		}
		g_minhook_up.store(true);
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

void Start(const std::wstring &mod_dir, const std::wstring &game_dir)
{
	log::init_file_sink((game_dir + L"stray-dlss-plugin.log").c_str());
	STRAY_LOG_INFO("StrayDLSS %s attaching (UE4SS C++ mod, the HOST): mod dir %s, game dir %s",
		STRAY_DLSS_PLUGIN_VERSION_STRING, sds::Narrow(mod_dir).c_str(), sds::Narrow(game_dir).c_str());

	g_ini_path = sds::Narrow(mod_dir + L"StrayDLSS.ini");
	if (!g_ini.load(g_ini_path))
		STRAY_LOG_ERROR("host: %s could not be opened; every [STRAYDLSS] key reads its default", g_ini_path.c_str());
	else
		STRAY_LOG_INFO("host: %s loaded (%zu values), hot-reloaded from on_update", g_ini_path.c_str(), g_ini.size());
	host::cfg::set_source(&g_source);

	// The application's one-time configuration (which events a host must deliver; here the
	// native hooks deliver them all, so the answer is only logged).
	const app::EventNeeds needs = app::instance().configure_events();
	STRAY_LOG_INFO("host: event needs: pipeline=%d finder-rt=%d pass-finder=%d (the native hooks deliver pipelines, binds, "
		"resets and dispatches; the finders' render-target/draw/copy taps are NOT delivered by this host)",
		needs.pipeline_events ? 1 : 0, needs.finder_rt_events ? 1 : 0, needs.pass_finder_events ? 1 : 0);

	install_device_hook();
}

void Tick()
{
	if (g_ini.reload_if_changed())
		STRAY_LOG_INFO("host: %s changed on disk and was re-read (%zu values); keys read at startup keep their old value",
			g_ini_path.c_str(), g_ini.size());
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
