#include "backend_native/vram_query_watch.hpp"

#include "backend_native/vtable_patch.hpp"
#include "backend_native/vtable_slots.hpp"
#include "host/config.hpp"
#include "log.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <intrin.h>

namespace stray_dlss::native::vramwatch {
namespace {

std::atomic<bool> g_installed{ false };
std::atomic<unsigned long long> g_calls{ 0 };
char g_report[512] = "vram query watch: not installed";

// The distinct caller modules seen, with a count each. A handful is all this can ever be, so a
// fixed table beats any allocation on a path the game calls.
constexpr int kMaxCallers = 8;
struct Caller
{
	void *ret = nullptr;          // a representative return address, for the offset
	char module[64] = {};
	unsigned long long count = 0;
};
Caller g_callers[kMaxCallers];
int g_caller_count = 0;
CRITICAL_SECTION g_lock;
bool g_lock_ready = false;

double wall_s()
{
	return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
}

void note_caller(void *ret)
{
	char path[MAX_PATH] = "<unknown>";
	::HMODULE m = nullptr;
	if (::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			static_cast<LPCWSTR>(ret), &m) && m != nullptr)
		::GetModuleFileNameA(m, path, MAX_PATH);
	const char *base = std::strrchr(path, '\\');
	base = base != nullptr ? base + 1 : path;

	::EnterCriticalSection(&g_lock);
	int idx = -1;
	for (int i = 0; i < g_caller_count; ++i)
		if (std::strncmp(g_callers[i].module, base, sizeof(g_callers[i].module) - 1) == 0) { idx = i; break; }
	const bool is_new = (idx < 0);
	if (is_new && g_caller_count < kMaxCallers)
	{
		idx = g_caller_count++;
		g_callers[idx].ret = ret;
		std::snprintf(g_callers[idx].module, sizeof(g_callers[idx].module), "%s", base);
	}
	if (idx >= 0) ++g_callers[idx].count;
	::LeaveCriticalSection(&g_lock);

	if (is_new && idx >= 0)
	{
		// The OFFSET within the module is what a map file or a disassembler needs to name the
		// call site; the absolute address means nothing across runs under ASLR. Logged once per
		// module, the first time it appears.
		STRAY_LOG_WARN("[vram] QueryVideoMemoryInfo CALLER: module=%s ret=%p offset=+0x%llx tid=%lu t=%.6f",
			g_callers[idx].module, ret,
			m != nullptr ? static_cast<unsigned long long>(reinterpret_cast<char *>(ret) - reinterpret_cast<char *>(m)) : 0ull,
			static_cast<unsigned long>(::GetCurrentThreadId()), wall_s());
	}
}

using PFN_QVMI = HRESULT(STDMETHODCALLTYPE *)(::IDXGIAdapter3 *, UINT, ::DXGI_MEMORY_SEGMENT_GROUP, ::DXGI_QUERY_VIDEO_MEMORY_INFO *);

HRESULT STDMETHODCALLTYPE hk_QueryVideoMemoryInfo(::IDXGIAdapter3 *self, UINT node,
	::DXGI_MEMORY_SEGMENT_GROUP group, ::DXGI_QUERY_VIDEO_MEMORY_INFO *info)
{
	void *ret = _ReturnAddress();
	const unsigned long long n = g_calls.fetch_add(1, std::memory_order_relaxed) + 1;
	note_caller(ret);

	// Time the forwarded call. The whole point is that this one is expensive (49-83 ms frames,
	// facts §32.16), so its cost belongs in the log beside the caller that asked for it.
	const double t0 = wall_s();
	auto orig = reinterpret_cast<PFN_QVMI>(original_for(self, slot::kAdapter3_QueryVideoMemoryInfo));
	const HRESULT hr = orig != nullptr ? orig(self, node, group, info) : E_FAIL;
	const double ms = (wall_s() - t0) * 1000.0;

	// Every call for the first few, then only the slow ones. The cadence IS the measurement and
	// a line at 1 Hz is affordable, but this must never become the stall it is chasing.
	if (n <= 20 || ms > 5.0)
		STRAY_LOG_WARN("[vram] QueryVideoMemoryInfo #%llu node=%u group=%d took %.2f ms tid=%lu t=%.6f",
			n, node, static_cast<int>(group), ms, static_cast<unsigned long>(::GetCurrentThreadId()), t0);
	return hr;
}

} // namespace

bool install(::ID3D12Device *device)
{
	if (device == nullptr) return false;
	if (!host::cfg::get_bool("VramQueryWatch", false))
	{
		std::snprintf(g_report, sizeof(g_report), "vram query watch: off (VramQueryWatch=0)");
		return false;
	}
	if (g_installed.load(std::memory_order_acquire)) return true;

	if (!g_lock_ready) { ::InitializeCriticalSection(&g_lock); g_lock_ready = true; }

	// Reach the adapter the DEVICE was created from, by LUID, so this is the adapter the game
	// actually renders on rather than whatever EnumAdapters(0) happens to be.
	::HMODULE dxgi = ::GetModuleHandleW(L"dxgi.dll");
	using PFN_CreateDXGIFactory1 = HRESULT(WINAPI *)(REFIID, void **);
	auto create = dxgi != nullptr
		? reinterpret_cast<PFN_CreateDXGIFactory1>(reinterpret_cast<void *>(::GetProcAddress(dxgi, "CreateDXGIFactory1")))
		: nullptr;
	if (create == nullptr)
	{
		std::snprintf(g_report, sizeof(g_report), "vram query watch: dxgi.dll/CreateDXGIFactory1 unavailable");
		STRAY_LOG_ERROR("%s", g_report);
		return false;
	}
	::IDXGIFactory4 *factory = nullptr;
	if (FAILED(create(IID_PPV_ARGS(&factory))) || factory == nullptr)
	{
		std::snprintf(g_report, sizeof(g_report), "vram query watch: CreateDXGIFactory1 failed");
		STRAY_LOG_ERROR("%s", g_report);
		return false;
	}
	const LUID luid = device->GetAdapterLuid();
	::IDXGIAdapter3 *adapter = nullptr;
	const HRESULT hr = factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter));
	if (FAILED(hr) || adapter == nullptr)
	{
		std::snprintf(g_report, sizeof(g_report), "vram query watch: EnumAdapterByLuid failed hr=0x%08lx",
			static_cast<unsigned long>(hr));
		STRAY_LOG_ERROR("%s", g_report);
		factory->Release();
		return false;
	}

	const bool ok = patch_slot(adapter, slot::kAdapter3_QueryVideoMemoryInfo,
		reinterpret_cast<void *>(&hk_QueryVideoMemoryInfo), "IDXGIAdapter3::QueryVideoMemoryInfo") != nullptr;
	// Released deliberately: the PATCH lives in the class's vtable, not in this object, which is
	// the same static-vtable property the present owner relies on. Should DXGI ever hand out a
	// per-instance vtable here, the hook simply never fires and the report says calls=0 — a
	// visible null result rather than a silent one.
	adapter->Release();
	factory->Release();

	g_installed.store(ok, std::memory_order_release);
	std::snprintf(g_report, sizeof(g_report), "vram query watch: %s (adapter LUID %08lx:%08lx)",
		ok ? "INSTALLED on IDXGIAdapter3::QueryVideoMemoryInfo" : "patch FAILED",
		static_cast<unsigned long>(luid.HighPart), static_cast<unsigned long>(luid.LowPart));
	STRAY_LOG_WARN("%s", g_report);
	return ok;
}

const char *report()
{
	if (!g_installed.load(std::memory_order_acquire))
		return g_report;

	char tail[320] = {};
	int off = 0;
	if (g_lock_ready)
	{
		::EnterCriticalSection(&g_lock);
		for (int i = 0; i < g_caller_count && off < static_cast<int>(sizeof(tail)) - 1; ++i)
			off += std::snprintf(tail + off, sizeof(tail) - static_cast<size_t>(off), " %s=%llu",
				g_callers[i].module, g_callers[i].count);
		::LeaveCriticalSection(&g_lock);
	}
	static char line[512];
	std::snprintf(line, sizeof(line), "vram query watch: calls=%llu callers:%s",
		g_calls.load(std::memory_order_relaxed), off > 0 ? tail : " (none yet)");
	return line;
}

} // namespace stray_dlss::native::vramwatch
