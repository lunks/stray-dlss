#include "backend_native/present_owner.hpp"

#include "backend_native/native_backend.hpp"
#include "backend_native/vtable_patch.hpp"
#include "backend_native/vtable_slots.hpp"
#include "core/present_plan.hpp"
#include "log.hpp"

#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdio>
#include <mutex>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace stray_dlss::native::present {
namespace {

constexpr unsigned kRing = 3; // the swapchain has 3 buffers (CLAUDE.md §2.1); one list per frame in flight

using PFN_CreateSwapChain = HRESULT(STDMETHODCALLTYPE *)(IDXGIFactory *, IUnknown *, DXGI_SWAP_CHAIN_DESC *, IDXGISwapChain **);
using PFN_CreateSwapChainForHwnd = HRESULT(STDMETHODCALLTYPE *)(IDXGIFactory2 *, IUnknown *, HWND, const DXGI_SWAP_CHAIN_DESC1 *, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *, IDXGIOutput *, IDXGISwapChain1 **);
using PFN_CreateSwapChainForCoreWindow = HRESULT(STDMETHODCALLTYPE *)(IDXGIFactory2 *, IUnknown *, IUnknown *, const DXGI_SWAP_CHAIN_DESC1 *, IDXGIOutput *, IDXGISwapChain1 **);
using PFN_CreateSwapChainForComposition = HRESULT(STDMETHODCALLTYPE *)(IDXGIFactory2 *, IUnknown *, const DXGI_SWAP_CHAIN_DESC1 *, IDXGIOutput *, IDXGISwapChain1 **);
using PFN_Present = HRESULT(STDMETHODCALLTYPE *)(IDXGISwapChain *, UINT, UINT);
using PFN_Present1 = HRESULT(STDMETHODCALLTYPE *)(IDXGISwapChain1 *, UINT, UINT, const DXGI_PRESENT_PARAMETERS *);
using PFN_ResizeBuffers = HRESULT(STDMETHODCALLTYPE *)(IDXGISwapChain *, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using PFN_ResizeBuffers1 = HRESULT(STDMETHODCALLTYPE *)(IDXGISwapChain3 *, UINT, UINT, UINT, DXGI_FORMAT, UINT, const UINT *, IUnknown *const *);

PFN_CreateSwapChain g_orig_CreateSwapChain = nullptr;
PFN_CreateSwapChainForHwnd g_orig_CreateSwapChainForHwnd = nullptr;
PFN_CreateSwapChainForCoreWindow g_orig_CreateSwapChainForCoreWindow = nullptr;
PFN_CreateSwapChainForComposition g_orig_CreateSwapChainForComposition = nullptr;
PFN_Present g_orig_Present = nullptr;
PFN_Present1 g_orig_Present1 = nullptr;
PFN_ResizeBuffers g_orig_ResizeBuffers = nullptr;
PFN_ResizeBuffers1 g_orig_ResizeBuffers1 = nullptr;

struct RingSlot
{
	ComPtr<ID3D12CommandAllocator> allocator;
	ComPtr<ID3D12GraphicsCommandList> list;
	std::uint64_t pending = 0; // fence value that retires this slot
};

struct Entry
{
	IDXGISwapChain *swapchain = nullptr;  // non-owning: the game's object, whichever class
	IDXGISwapChain3 *swapchain3 = nullptr; // non-owning: the same object when it is a SwapChain3
	ID3D12CommandQueue *queue = nullptr;   // the REAL queue we execute on (non-owning)
	std::uint64_t device_arg = 0;          // what the game passed; may be a proxy
};

std::mutex g_mutex;
::ID3D12Device *g_device = nullptr;
std::vector<core::QueueRecord> g_queues;
std::vector<Entry> g_entries;
RingSlot g_ring[kRing];
ComPtr<ID3D12Fence> g_fence;
std::uint64_t g_fence_value = 0;
HANDLE g_fence_event = nullptr;
std::atomic<bool> g_list_used{ false };
std::atomic<bool> g_installed{ false };
Stats g_stats;
char g_report[512] = "present owner: not installed";

HRESULT STDMETHODCALLTYPE hk_Present(IDXGISwapChain *self, UINT sync, UINT flags);
HRESULT STDMETHODCALLTYPE hk_Present1(IDXGISwapChain1 *self, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS *params);
HRESULT STDMETHODCALLTYPE hk_ResizeBuffers(IDXGISwapChain *self, UINT count, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags);
HRESULT STDMETHODCALLTYPE hk_ResizeBuffers1(IDXGISwapChain3 *self, UINT count, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags, const UINT *node_masks, IUnknown *const *queues);

Entry *find_entry_locked(IDXGISwapChain *sc)
{
	for (Entry &e : g_entries)
		if (e.swapchain == sc)
			return &e;
	return nullptr;
}

bool ensure_ring()
{
	if (g_fence)
		return true;
	if (g_device == nullptr)
		return false;
	OwnCodeScope own;
	for (unsigned i = 0; i < kRing; ++i)
	{
		if (FAILED(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_ring[i].allocator))) ||
			FAILED(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_ring[i].allocator.Get(), nullptr, IID_PPV_ARGS(&g_ring[i].list))))
		{
			STRAY_LOG_ERROR("present owner: could not create the present list ring (slot %u); present-time work is unavailable", i);
			return false;
		}
		g_ring[i].list->Close();
	}
	if (FAILED(g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence))))
	{
		STRAY_LOG_ERROR("present owner: CreateFence failed; present-time work is unavailable");
		return false;
	}
	g_fence_event = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
	return true;
}

// Hooks the swapchain's own vtable slots (once per class; patch_slot is idempotent) and
// records which real queue it presents on.
void note_swapchain(IDXGISwapChain *sc, IUnknown *device_arg, const char *how)
{
	if (sc == nullptr)
		return;
	IDXGISwapChain3 *sc3 = nullptr;
	if (SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&sc3))) && sc3 != nullptr)
		sc3->Release(); // non-owning; the object outlives every use we make of it
	IDXGISwapChain1 *sc1 = nullptr;
	if (SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&sc1))) && sc1 != nullptr)
		sc1->Release();
	const bool same_object3 = sc3 != nullptr && static_cast<void *>(sc3) == static_cast<void *>(sc);
	const bool same_object1 = sc1 != nullptr && static_cast<void *>(sc1) == static_cast<void *>(sc);

	ID3D12CommandQueue *queue_arg = nullptr;
	if (device_arg != nullptr && SUCCEEDED(device_arg->QueryInterface(IID_PPV_ARGS(&queue_arg))) && queue_arg != nullptr)
		queue_arg->Release();

	std::lock_guard<std::mutex> lock(g_mutex);
	const int pick = core::pick_present_queue(g_queues, reinterpret_cast<std::uint64_t>(queue_arg));
	Entry *e = find_entry_locked(sc);
	if (e == nullptr)
	{
		g_entries.push_back(Entry{});
		e = &g_entries.back();
		++g_stats.swapchains;
	}
	e->swapchain = sc;
	e->swapchain3 = same_object3 ? sc3 : nullptr;
	e->device_arg = reinterpret_cast<std::uint64_t>(queue_arg);
	e->queue = pick >= 0 ? reinterpret_cast<ID3D12CommandQueue *>(g_queues[static_cast<std::size_t>(pick)].id) : nullptr;

	unsigned patched = 0;
	void *o = patch_slot(sc, slot::kSwapChain_Present, reinterpret_cast<void *>(&hk_Present), "IDXGISwapChain::Present");
	if (o != nullptr) { g_orig_Present = reinterpret_cast<PFN_Present>(o); ++patched; }
	o = patch_slot(sc, slot::kSwapChain_ResizeBuffers, reinterpret_cast<void *>(&hk_ResizeBuffers), "IDXGISwapChain::ResizeBuffers");
	if (o != nullptr) { g_orig_ResizeBuffers = reinterpret_cast<PFN_ResizeBuffers>(o); ++patched; }
	if (same_object1)
	{
		o = patch_slot(sc, slot::kSwapChain1_Present1, reinterpret_cast<void *>(&hk_Present1), "IDXGISwapChain1::Present1");
		if (o != nullptr) { g_orig_Present1 = reinterpret_cast<PFN_Present1>(o); ++patched; }
	}
	if (same_object3)
	{
		o = patch_slot(sc, slot::kSwapChain3_ResizeBuffers1, reinterpret_cast<void *>(&hk_ResizeBuffers1), "IDXGISwapChain3::ResizeBuffers1");
		if (o != nullptr) { g_orig_ResizeBuffers1 = reinterpret_cast<PFN_ResizeBuffers1>(o); ++patched; }
	}
	STRAY_LOG_WARN("present owner: swapchain %p via %s (SwapChain1=%d SwapChain3=%d) device-arg=%p -> %s queue %p (%s); %u slot(s) newly patched",
		static_cast<void *>(sc), how, same_object1 ? 1 : 0, same_object3 ? 1 : 0, static_cast<void *>(queue_arg),
		pick >= 0 && g_queues[static_cast<std::size_t>(pick)].id == reinterpret_cast<std::uint64_t>(queue_arg) ? "the SAME" : "the first DIRECT",
		static_cast<void *>(e->queue), e->queue == nullptr ? "NONE: no direct queue recorded, present-time work is unavailable" : "ok", patched);
}

// The back buffers, for the sink's swapchain event and the log.
void report_back_buffers(IDXGISwapChain *sc, bool created)
{
	icept::Sink *sk = sink();
	if (sk == nullptr)
		return;
	if (!created)
	{
		sk->on_swapchain(nullptr, 0, false);
		return;
	}
	DXGI_SWAP_CHAIN_DESC d = {};
	if (FAILED(sc->GetDesc(&d)))
		return;
	icept::ResourceId ids[16] = {};
	std::uint32_t n = 0;
	for (UINT i = 0; i < d.BufferCount && n < 16; ++i)
	{
		ID3D12Resource *buf = nullptr;
		if (SUCCEEDED(sc->GetBuffer(i, IID_PPV_ARGS(&buf))) && buf != nullptr)
		{
			ids[n++] = reinterpret_cast<icept::ResourceId>(buf);
			buf->Release();
		}
	}
	sk->on_swapchain(ids, n, true);
}

// ---- factory hooks ----

HRESULT STDMETHODCALLTYPE hk_CreateSwapChain(IDXGIFactory *self, IUnknown *device, DXGI_SWAP_CHAIN_DESC *desc, IDXGISwapChain **out)
{
	const HRESULT hr = g_orig_CreateSwapChain(self, device, desc, out);
	if (SUCCEEDED(hr) && out != nullptr && *out != nullptr && !in_own_code())
	{
		note_swapchain(*out, device, "CreateSwapChain");
		report_back_buffers(*out, true);
	}
	return hr;
}

HRESULT STDMETHODCALLTYPE hk_CreateSwapChainForHwnd(IDXGIFactory2 *self, IUnknown *device, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1 *desc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *fs, IDXGIOutput *restrict_to, IDXGISwapChain1 **out)
{
	const HRESULT hr = g_orig_CreateSwapChainForHwnd(self, device, hwnd, desc, fs, restrict_to, out);
	if (SUCCEEDED(hr) && out != nullptr && *out != nullptr && !in_own_code())
	{
		note_swapchain(*out, device, "CreateSwapChainForHwnd");
		report_back_buffers(*out, true);
	}
	return hr;
}

HRESULT STDMETHODCALLTYPE hk_CreateSwapChainForCoreWindow(IDXGIFactory2 *self, IUnknown *device, IUnknown *window, const DXGI_SWAP_CHAIN_DESC1 *desc, IDXGIOutput *restrict_to, IDXGISwapChain1 **out)
{
	const HRESULT hr = g_orig_CreateSwapChainForCoreWindow(self, device, window, desc, restrict_to, out);
	if (SUCCEEDED(hr) && out != nullptr && *out != nullptr && !in_own_code())
	{
		note_swapchain(*out, device, "CreateSwapChainForCoreWindow");
		report_back_buffers(*out, true);
	}
	return hr;
}

HRESULT STDMETHODCALLTYPE hk_CreateSwapChainForComposition(IDXGIFactory2 *self, IUnknown *device, const DXGI_SWAP_CHAIN_DESC1 *desc, IDXGIOutput *restrict_to, IDXGISwapChain1 **out)
{
	const HRESULT hr = g_orig_CreateSwapChainForComposition(self, device, desc, restrict_to, out);
	if (SUCCEEDED(hr) && out != nullptr && *out != nullptr && !in_own_code())
	{
		note_swapchain(*out, device, "CreateSwapChainForComposition");
		report_back_buffers(*out, true);
	}
	return hr;
}

// ---- the present ----

// Delivers on_present with our list on the presenting queue, then executes that list if the
// sink recorded anything on it. Returns false when the present must go through untouched.
void before_present(IDXGISwapChain *sc, UINT flags)
{
	icept::Sink *sk = sink();
	if (sk == nullptr || (flags & DXGI_PRESENT_TEST) != 0 || in_own_code() || mode() != Mode::drive)
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		++g_stats.skipped;
		return;
	}
	Entry entry;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		Entry *e = find_entry_locked(sc);
		if (e == nullptr)
		{
			++g_stats.skipped;
			return;
		}
		entry = *e;
	}
	OwnCodeScope own; // everything below is ours: the ring's Reset, the sink's recording, our execute

	icept::PresentContext pc;
	pc.queue = entry.queue;
	pc.swapchain = entry.swapchain3;
	pc.frame = sk->next_frame();
	pc.backend_cookie = 0;
	// The current back buffer, non-owning, exactly as the ReShade host reported it.
	{
		const UINT index = entry.swapchain3 != nullptr ? entry.swapchain3->GetCurrentBackBufferIndex() : 0;
		ID3D12Resource *buf = nullptr;
		if (SUCCEEDED(sc->GetBuffer(index, IID_PPV_ARGS(&buf))) && buf != nullptr)
		{
			pc.back_buffer = buf;
			buf->Release();
		}
	}

	RingSlot *slot = nullptr;
	if (entry.queue != nullptr && ensure_ring())
	{
		slot = &g_ring[pc.frame % kRing];
		if (slot->pending != 0 && g_fence->GetCompletedValue() < slot->pending)
		{
			g_fence->SetEventOnCompletion(slot->pending, g_fence_event);
			::WaitForSingleObject(g_fence_event, 2000);
		}
		if (SUCCEEDED(slot->allocator->Reset()) && SUCCEEDED(slot->list->Reset(slot->allocator.Get(), nullptr)))
			pc.present_list = slot->list.Get();
		else
			slot = nullptr;
	}
	g_list_used.store(false, std::memory_order_relaxed);

	{
		static std::atomic<bool> s_said{ false };
		if (!s_said.exchange(true))
			STRAY_LOG_WARN("present owner: FIRST Present delivered on_present (queue=%p present_list=%p back_buffer=%p) - the frame boundary is live",
				static_cast<void *>(pc.queue), static_cast<void *>(pc.present_list), reinterpret_cast<void *>(pc.back_buffer));
	}
	sk->on_present(pc);

	{
		std::lock_guard<std::mutex> lock(g_mutex);
		++g_stats.presents;
	}
	if (slot != nullptr)
	{
		slot->list->Close();
		if (g_list_used.load(std::memory_order_relaxed))
		{
			ID3D12CommandList *lists[] = { slot->list.Get() };
			entry.queue->ExecuteCommandLists(1, lists);
			std::lock_guard<std::mutex> lock(g_mutex);
			++g_stats.executed;
		}
		entry.queue->Signal(g_fence.Get(), ++g_fence_value);
		slot->pending = g_fence_value;
	}
}

HRESULT STDMETHODCALLTYPE hk_Present(IDXGISwapChain *self, UINT sync, UINT flags)
{
	before_present(self, flags);
	return g_orig_Present(self, sync, flags);
}

HRESULT STDMETHODCALLTYPE hk_Present1(IDXGISwapChain1 *self, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS *params)
{
	before_present(self, flags);
	return g_orig_Present1(self, sync, flags, params);
}

HRESULT STDMETHODCALLTYPE hk_ResizeBuffers(IDXGISwapChain *self, UINT count, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags)
{
	if (!in_own_code())
		report_back_buffers(self, false);
	const HRESULT hr = g_orig_ResizeBuffers(self, count, w, h, fmt, flags);
	if (!in_own_code())
	{
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			++g_stats.resizes;
		}
		STRAY_LOG_INFO("present owner: ResizeBuffers(%u, %ux%u, fmt %d) hr=0x%08lx", count, w, h, static_cast<int>(fmt), static_cast<unsigned long>(hr));
		if (SUCCEEDED(hr))
			report_back_buffers(self, true);
	}
	return hr;
}

HRESULT STDMETHODCALLTYPE hk_ResizeBuffers1(IDXGISwapChain3 *self, UINT count, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags, const UINT *node_masks, IUnknown *const *queues)
{
	if (!in_own_code())
		report_back_buffers(self, false);
	const HRESULT hr = g_orig_ResizeBuffers1(self, count, w, h, fmt, flags, node_masks, queues);
	if (!in_own_code())
	{
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			++g_stats.resizes;
		}
		STRAY_LOG_INFO("present owner: ResizeBuffers1(%u, %ux%u, fmt %d) hr=0x%08lx", count, w, h, static_cast<int>(fmt), static_cast<unsigned long>(hr));
		if (SUCCEEDED(hr))
			report_back_buffers(self, true);
	}
	return hr;
}

} // namespace

// Patches one factory's CreateSwapChain* slots (idempotent per slot). Returns how many were
// newly patched.
unsigned patch_factory(IDXGIFactory *factory)
{
	if (factory == nullptr)
		return 0;
	unsigned n = 0;
	void *o = patch_slot(factory, slot::kFactory_CreateSwapChain, reinterpret_cast<void *>(&hk_CreateSwapChain), "IDXGIFactory::CreateSwapChain");
	if (o != nullptr) { g_orig_CreateSwapChain = reinterpret_cast<PFN_CreateSwapChain>(o); ++n; }
	IDXGIFactory2 *f2 = nullptr;
	if (SUCCEEDED(factory->QueryInterface(IID_PPV_ARGS(&f2))) && f2 != nullptr)
	{
		const bool same = static_cast<void *>(f2) == static_cast<void *>(factory);
		f2->Release();
		if (same)
		{
			o = patch_slot(factory, slot::kFactory2_CreateSwapChainForHwnd, reinterpret_cast<void *>(&hk_CreateSwapChainForHwnd), "IDXGIFactory2::CreateSwapChainForHwnd");
			if (o != nullptr) { g_orig_CreateSwapChainForHwnd = reinterpret_cast<PFN_CreateSwapChainForHwnd>(o); ++n; }
			o = patch_slot(factory, slot::kFactory2_CreateSwapChainForCoreWindow, reinterpret_cast<void *>(&hk_CreateSwapChainForCoreWindow), "IDXGIFactory2::CreateSwapChainForCoreWindow");
			if (o != nullptr) { g_orig_CreateSwapChainForCoreWindow = reinterpret_cast<PFN_CreateSwapChainForCoreWindow>(o); ++n; }
			o = patch_slot(factory, slot::kFactory2_CreateSwapChainForComposition, reinterpret_cast<void *>(&hk_CreateSwapChainForComposition), "IDXGIFactory2::CreateSwapChainForComposition");
			if (o != nullptr) { g_orig_CreateSwapChainForComposition = reinterpret_cast<PFN_CreateSwapChainForComposition>(o); ++n; }
		}
	}
	return n;
}

bool install(::ID3D12Device *device)
{
	if (device == nullptr)
		return false;
	if (g_installed.load())
		return true;
	g_device = device;

	// A throwaway factory reaches the factory class's vtable. dxgi.dll is whatever the process
	// has: DXVK's, or ReShade's proxy when it is loaded as dxgi.dll — both one class.
	HMODULE dxgi = ::GetModuleHandleW(L"dxgi.dll");
	if (dxgi == nullptr)
		dxgi = ::LoadLibraryW(L"dxgi.dll");
	using PFN_CreateDXGIFactory1 = HRESULT(WINAPI *)(REFIID, void **);
	auto create = dxgi != nullptr ? reinterpret_cast<PFN_CreateDXGIFactory1>(reinterpret_cast<void *>(::GetProcAddress(dxgi, "CreateDXGIFactory1"))) : nullptr;
	if (create == nullptr)
	{
		std::snprintf(g_report, sizeof(g_report), "present owner: dxgi.dll / CreateDXGIFactory1 unavailable; NOT installed");
		STRAY_LOG_ERROR("%s", g_report);
		return false;
	}
	ComPtr<IDXGIFactory1> factory;
	{
		OwnCodeScope own;
		if (FAILED(create(IID_PPV_ARGS(&factory))) || !factory)
		{
			std::snprintf(g_report, sizeof(g_report), "present owner: CreateDXGIFactory1 failed; NOT installed");
			STRAY_LOG_ERROR("%s", g_report);
			return false;
		}
	}
	const unsigned n = patch_factory(factory.Get());
	char owner[MAX_PATH] = "?";
	{
		HMODULE m = nullptr;
		void **vt = *reinterpret_cast<void ***>(factory.Get());
		if (::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCWSTR>(vt[0]), &m) && m != nullptr)
			::GetModuleFileNameA(m, owner, MAX_PATH);
	}
	g_installed.store(n != 0);
	std::snprintf(g_report, sizeof(g_report), "present owner: factory %p (class implemented by %s) %u CreateSwapChain* slot(s) patched; ring=%u",
		static_cast<void *>(factory.Get()), owner, n, kRing);
	if (n == 0)
		STRAY_LOG_ERROR("%s - NOT installed", g_report);
	else
		STRAY_LOG_INFO("%s", g_report);
	return n != 0;
}

void uninstall()
{
	if (!g_installed.load())
		return;
	if (g_fence && g_fence_value != 0 && g_fence->GetCompletedValue() < g_fence_value && g_fence_event != nullptr)
	{
		g_fence->SetEventOnCompletion(g_fence_value, g_fence_event);
		::WaitForSingleObject(g_fence_event, 2000);
	}
	for (RingSlot &s : g_ring)
	{
		s.list.Reset();
		s.allocator.Reset();
		s.pending = 0;
	}
	g_fence.Reset();
	if (g_fence_event != nullptr)
	{
		::CloseHandle(g_fence_event);
		g_fence_event = nullptr;
	}
	std::lock_guard<std::mutex> lock(g_mutex);
	g_entries.clear();
	g_queues.clear();
	g_device = nullptr;
	g_installed.store(false);
	std::snprintf(g_report, sizeof(g_report), "present owner: uninstalled");
	STRAY_LOG_INFO("%s", g_report);
}

void note_factory(::IUnknown *factory)
{
	if (factory == nullptr)
		return;
	IDXGIFactory *f = nullptr;
	if (FAILED(factory->QueryInterface(IID_PPV_ARGS(&f))) || f == nullptr)
		return;
	const unsigned n = patch_factory(f);
	f->Release();
	std::lock_guard<std::mutex> lock(g_mutex);
	if (n != 0)
		STRAY_LOG_INFO("present owner: patched the game's factory %p (%u CreateSwapChain* slot(s) newly hooked)", static_cast<void *>(factory), n);
}

void note_queue(::ID3D12CommandQueue *queue, int type)
{
	if (queue == nullptr)
		return;
	std::lock_guard<std::mutex> lock(g_mutex);
	for (const core::QueueRecord &r : g_queues)
		if (r.id == reinterpret_cast<std::uint64_t>(queue))
			return;
	g_queues.push_back(core::QueueRecord{ reinterpret_cast<std::uint64_t>(queue), type });
	++g_stats.queues;
	STRAY_LOG_INFO("present owner: queue #%zu %p type %d (%s)", g_queues.size(), static_cast<void *>(queue), type,
		type == 0 ? "DIRECT" : type == 2 ? "compute" : type == 3 ? "copy" : "other");
}

void note_present_list_used() { g_list_used.store(true, std::memory_order_relaxed); }

Stats stats()
{
	std::lock_guard<std::mutex> lock(g_mutex);
	return g_stats;
}

const char *report() { return g_report; }

} // namespace stray_dlss::native::present
