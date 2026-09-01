#include "Probe.hpp"

#include "Log.hpp"

#include <MinHook.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdio>

using Microsoft::WRL::ComPtr;

namespace stray_dlss::probe {
namespace {

using PFN_D3D12CreateDevice = HRESULT(WINAPI *)(IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **);

PFN_D3D12CreateDevice g_orig = nullptr;
ULONGLONG g_t0 = 0;
std::atomic<bool> g_hook_installed{ false };
std::atomic<bool> g_minhook_up{ false };
std::atomic<int> g_hook_fired{ 0 };
std::atomic<bool> g_dummy_done{ false };
// The probe's own D3D12CreateDevice must not count as the game's, and must not recurse.
thread_local int t_in_probe = 0;

ULONGLONG since_start() { return ::GetTickCount64() - g_t0; }

// "HH:MM:SS.mmm" LOCAL time and the same instant in UTC, so the line can be placed against
// ReShade.log (local, e.g. 18:10:04:414) and UE4SS.log (UTC) without guessing the offset.
void log_clock(const char *what)
{
	SYSTEMTIME lt{}, ut{};
	::GetLocalTime(&lt);
	::GetSystemTime(&ut);
	SDS_LOG_INFO("probe: %s at local %02u:%02u:%02u.%03u = utc %02u:%02u:%02u.%03u (t=%llu ms)",
		what, lt.wHour, lt.wMinute, lt.wSecond, lt.wMilliseconds,
		ut.wHour, ut.wMinute, ut.wSecond, ut.wMilliseconds,
		static_cast<unsigned long long>(since_start()));
}

// The module that owns a code address — for a vtable slot, that names who implements the
// object: d3d12core.dll / libvkd3d-proton (the real device) or ReShade's dxgi.dll (a proxy).
void owner_of(const void *addr, char *out, size_t n)
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
	const bool reshade = ::GetProcAddress(owner, "ReShadeRegisterAddon") != nullptr;
	std::snprintf(out, n, "%s%s", base[0] ? base : "<unnamed>", reshade ? " (RESHADE PROXY)" : "");
}

void describe_device(const char *what, IUnknown *dev)
{
	if (dev == nullptr)
	{
		SDS_LOG_INFO("probe: %s = null", what);
		return;
	}
	void *vtable = *reinterpret_cast<void **>(dev);
	void *slot0 = (*reinterpret_cast<void ***>(dev))[0];
	char owner[MAX_PATH + 32];
	owner_of(slot0, owner, sizeof(owner));
	SDS_LOG_INFO("probe: %s = %p vtable=%p implemented by %s", what, static_cast<void *>(dev), vtable, owner);
}

HRESULT WINAPI hook_create_device(IUnknown *adapter, D3D_FEATURE_LEVEL fl, REFIID riid, void **out)
{
	const HRESULT hr = g_orig(adapter, fl, riid, out);
	if (t_in_probe > 0)
		return hr; // our own throwaway device; reported by the caller
	const int n = ++g_hook_fired;
	SDS_LOG_WARN("probe: D3D12CreateDevice HOOK FIRED #%d hr=0x%08lx fl=0x%x device=%p",
		n, static_cast<unsigned long>(hr), static_cast<unsigned>(fl), out ? *out : nullptr);
	log_clock("D3D12CreateDevice (the game's)");
	if (SUCCEEDED(hr) && out != nullptr && *out != nullptr)
	{
		// Whether an export hook installed THIS EARLY sits above or below ReShade's own hook on
		// the same export decides what a mod-loaded backend can see: the real vkd3d device, or
		// ReShade's proxy. Above = we call ReShade's detour as "the original" and get the proxy.
		describe_device("device returned through our hook", static_cast<IUnknown *>(*out));
	}
	return hr;
}

bool try_install_hook(const char *when)
{
	if (g_hook_installed.load())
		return true;
	const HMODULE d3d12 = ::GetModuleHandleW(L"d3d12.dll");
	if (d3d12 == nullptr)
		return false;
	log_clock(when);
	if (!g_minhook_up.load())
	{
		const MH_STATUS st = MH_Initialize();
		if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED)
		{
			SDS_LOG_ERROR("probe: MH_Initialize failed (%d)", static_cast<int>(st));
			return true; // stop trying
		}
		g_minhook_up.store(true);
	}
	void *target = reinterpret_cast<void *>(::GetProcAddress(d3d12, "D3D12CreateDevice"));
	char owner[MAX_PATH + 32];
	owner_of(target, owner, sizeof(owner));
	SDS_LOG_INFO("probe: d3d12.dll=%p D3D12CreateDevice=%p (in %s)", static_cast<void *>(d3d12), target, owner);
	if (target == nullptr)
	{
		SDS_LOG_ERROR("probe: D3D12CreateDevice not exported?!");
		g_hook_installed.store(true);
		return true;
	}
	// Is the export ALREADY detoured (ReShade installs its d3d12 hooks the moment the DLL is
	// loaded via LoadLibrary)? A first byte of 0xE9 (jmp rel32) or 0xFF 0x25 (jmp [rip+x]) says so.
	const unsigned char *b = static_cast<const unsigned char *>(target);
	SDS_LOG_INFO("probe: D3D12CreateDevice first bytes %02X %02X %02X %02X %02X -> %s",
		b[0], b[1], b[2], b[3], b[4],
		(b[0] == 0xE9 || (b[0] == 0xFF && b[1] == 0x25)) ? "ALREADY DETOURED (ReShade is below us)" : "pristine (we are the first hook)");
	const MH_STATUS c = MH_CreateHook(target, reinterpret_cast<void *>(&hook_create_device), reinterpret_cast<void **>(&g_orig));
	const MH_STATUS e = c == MH_OK ? MH_EnableHook(target) : c;
	if (c != MH_OK || e != MH_OK)
	{
		SDS_LOG_ERROR("probe: MH_CreateHook=%d MH_EnableHook=%d", static_cast<int>(c), static_cast<int>(e));
		g_hook_installed.store(true);
		return true;
	}
	g_hook_installed.store(true);
	SDS_LOG_INFO("probe: D3D12CreateDevice hooked; if the game already has a device this never fires");
	return true;
}

// The late-attach question (plan Task 13): can a mod that missed D3D12CreateDevice still reach
// the REAL device? A throwaway device from D3D12CreateDevice tells us what the export returns
// to us (a ReShade proxy while ReShade is loaded), and ID3D12Resource::GetDevice — resources
// are never proxied by ReShade on D3D12 — tells us whether the real device is reachable from
// there without touching any vkd3d interop interface.
void dummy_device_probe()
{
	++t_in_probe;
	log_clock("throwaway device probe");
	ComPtr<ID3D12Device> dev;
	const HRESULT hr = ::D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev));
	SDS_LOG_WARN("probe: throwaway D3D12CreateDevice hr=0x%08lx", static_cast<unsigned long>(hr));
	if (SUCCEEDED(hr) && dev)
	{
		describe_device("throwaway device (as returned to us)", dev.Get());

		D3D12_HEAP_PROPERTIES hp = {};
		hp.Type = D3D12_HEAP_TYPE_DEFAULT;
		D3D12_RESOURCE_DESC bd = {};
		bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		bd.Width = 256; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
		bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		ComPtr<ID3D12Resource> buf;
		const HRESULT rh = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
			D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&buf));
		SDS_LOG_INFO("probe: throwaway buffer hr=0x%08lx", static_cast<unsigned long>(rh));
		if (SUCCEEDED(rh) && buf)
		{
			describe_device("throwaway buffer (ID3D12Resource)", buf.Get());
			ComPtr<ID3D12Device> real;
			const HRESULT gh = buf->GetDevice(IID_PPV_ARGS(&real));
			SDS_LOG_INFO("probe: ID3D12Resource::GetDevice hr=0x%08lx", static_cast<unsigned long>(gh));
			if (SUCCEEDED(gh) && real)
			{
				describe_device("device via ID3D12Resource::GetDevice", real.Get());
				SDS_LOG_WARN("probe: GetDevice %s the object D3D12CreateDevice returned to us",
					real.Get() == dev.Get() ? "returned THE SAME as" : "returned a DIFFERENT object from");
				// And the real device's own command list / queue vtables, for the slot-patch design.
				ComPtr<ID3D12CommandAllocator> alloc;
				ComPtr<ID3D12GraphicsCommandList> list;
				if (SUCCEEDED(real->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) &&
					SUCCEEDED(real->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&list))))
				{
					list->Close();
					describe_device("command list from the GetDevice device", list.Get());
				}
			}
		}
	}
	// No hidden window and no swapchain: that half of the plan's probe is deferred (the observer
	// build needs no Present hook while ReShade owns the frame), and a Wine toplevel under
	// gamescope has a documented focus-steal freeze (CLAUDE.md §6).
	SDS_LOG_WARN("probe: throwaway objects released; hidden-window/swapchain probe deliberately NOT run");
	--t_in_probe;
}

} // namespace

void Start()
{
	g_t0 = ::GetTickCount64();
	log_clock("start_mod");
	const HMODULE d3d12 = ::GetModuleHandleW(L"d3d12.dll");
	const HMODULE d3d12core = ::GetModuleHandleW(L"d3d12core.dll");
	const HMODULE dxgi = ::GetModuleHandleW(L"dxgi.dll");
	char dxgi_owner[MAX_PATH + 32] = "<absent>";
	if (dxgi != nullptr)
		owner_of(reinterpret_cast<const void *>(dxgi), dxgi_owner, sizeof(dxgi_owner));
	SDS_LOG_WARN("probe: start_mod at t=0 ms; d3d12.dll loaded=%d d3d12core.dll loaded=%d dxgi.dll loaded=%d (%s)",
		d3d12 != nullptr, d3d12core != nullptr, dxgi != nullptr, dxgi_owner);
	if (!try_install_hook("D3D12CreateDevice hook installed from start_mod"))
		SDS_LOG_WARN("probe: d3d12.dll is NOT loaded yet at start_mod; polling for it from on_update");
}

void Tick()
{
	if (!g_hook_installed.load())
	{
		if (try_install_hook("d3d12.dll first seen from on_update; hook installed"))
			return;
	}
	if (!g_dummy_done.load() && since_start() >= 30000)
	{
		g_dummy_done.store(true);
		SDS_LOG_INFO("probe: hook fired %d time(s) in the first 30 s", g_hook_fired.load());
		dummy_device_probe();
	}
}

void Stop()
{
	if (g_minhook_up.load())
	{
		MH_DisableHook(MH_ALL_HOOKS);
		MH_Uninitialize();
		g_minhook_up.store(false);
	}
}

} // namespace stray_dlss::probe
