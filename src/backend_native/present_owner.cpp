#include "backend_native/present_owner.hpp"

#include "backend_native/fg_present.hpp"
#include "backend_native/native_backend.hpp"
#include "host/config.hpp"
#include "backend_native/vtable_patch.hpp"
#include "backend_native/vtable_slots.hpp"
#include "core/present_plan.hpp"
#include "perf.hpp"
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

// No single g_orig_* globals. Two DXGI implementations coexist in one Proton process (DXVK's,
// which the game uses, and wine's builtin, which our throwaway factory reached), each with its
// OWN factory/swapchain vtable and its OWN original functions. A global shadow collapses them
// and forwards one class's re-entrant CreateSwapChain->CreateSwapChainForHwnd into the OTHER
// class's implementation: wine's d3d12_swapchain_create then calls vkd3d_instance_from_device
// on a vkd3d-proton device, gets NULL, and vkd3d_instance_get_vk_instance(NULL) faults reading
// 0x0 (facts §20, the symbolised crash). So every hook resolves its original PER-VTABLE from
// the `self` it was handed, via vtable_patch::original_for.

// Re-entry depth for the factory creation hooks. DXVK implements the legacy CreateSwapChain by
// calling CreateSwapChainForHwnd on the SAME factory, which re-enters our patched slot; note
// the returned swapchain only at the OUTERMOST creation call, so a half-wired inner return is
// never recorded (belt-and-braces: lazy noting touches only the vtable pointer, set at
// construction, so even an inner note would be safe).
thread_local int t_create_depth = 0;

struct RingSlot
{
	ComPtr<ID3D12CommandAllocator> allocator;
	ComPtr<ID3D12GraphicsCommandList> list;
	std::uint64_t pending = 0; // fence value that retires this slot
};

struct Entry
{
	IDXGISwapChain *swapchain = nullptr;  // non-owning: the game's object, whichever class
	IDXGISwapChain3 *swapchain3 = nullptr; // non-owning: the same object when it is a SwapChain3, resolved at first Present
	ID3D12CommandQueue *queue = nullptr;   // the REAL queue we execute on (non-owning), resolved at first Present
	IUnknown *device_arg_raw = nullptr;    // the swapchain's "device" arg, captured at creation as a bare pointer; QI'd at first Present
	bool finalized = false;                // the one-time first-Present work (QI, queue pick, back buffers) is done
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
std::atomic<std::uint64_t> g_stats_last_frame{ 0 }; // pc.frame of the last delivered on_present
std::atomic<bool> g_hook_trace{ false };
#define PRESENT_HOOK_TRACE(name, self) \
	do { if (g_hook_trace.load(std::memory_order_relaxed)) \
		STRAY_LOG_INFO("[hook] %s self=%p thread=%lu own=%d", name, static_cast<void *>(self), static_cast<unsigned long>(::GetCurrentThreadId()), in_own_code() ? 1 : 0); } while (0)
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

// LAZY. Records the returned swapchain pointer and its "device" arg, and patches ITS OWN
// Present/ResizeBuffers vtable slots — nothing else. It does NOT touch the object (no
// QueryInterface, no GetDesc, no GetBuffer): the swapchain is still inside its own creation
// call, DXVK/vkd3d have not finished wiring its back-buffer images, and every earlier crash
// on this path was work done against a half-built swapchain or a mis-resolved original. All of
// that moves to the first Present, where the object has proven itself. Patching a vtable slot
// reads only the object's vtable pointer (set at construction) and writes into the shared
// static vtable, so it is safe even mid-creation. Present1/ResizeBuffers1 are patched at first
// Present, after a QueryInterface confirms the object really is a SwapChain1/3. (facts §20)
void note_swapchain_lazy(IDXGISwapChain *sc, IUnknown *device_arg, const char *how)
{
	if (sc == nullptr)
		return;
	std::lock_guard<std::mutex> lock(g_mutex);
	if (find_entry_locked(sc) != nullptr)
		return; // already known (creation re-entry, or a re-hook); note once
	g_entries.push_back(Entry{});
	Entry *e = &g_entries.back();
	e->swapchain = sc;
	e->device_arg_raw = device_arg;
	++g_stats.swapchains;

	unsigned patched = 0;
	if (patch_slot(sc, slot::kSwapChain_Present, reinterpret_cast<void *>(&hk_Present), "IDXGISwapChain::Present") != nullptr)
		++patched;
	if (patch_slot(sc, slot::kSwapChain_ResizeBuffers, reinterpret_cast<void *>(&hk_ResizeBuffers), "IDXGISwapChain::ResizeBuffers") != nullptr)
		++patched;
	STRAY_LOG_WARN("present owner: swapchain %p via %s device-arg=%p recorded; %u Present/Resize slot(s) patched. "
		"QI/queue/back-buffers deferred to first Present.",
		static_cast<void *>(sc), how, static_cast<void *>(device_arg), patched);
	// Frame generation hooks GetBuffer on the same vtable, and must do so NOW: the game asks for
	// its back buffers right after creation, before the first Present (facts §32.4).
	fg::on_swapchain_recorded(sc, g_device);
}

// The one-time work that used to sit in the creation hook, now run at the FIRST Present of a
// swapchain, when it is fully built: resolve SwapChain3/1, pick the presenting queue from the
// captured device arg, patch Present1/ResizeBuffers1, and report the back buffers. Caller holds
// g_mutex. Returns with e->finalized set.
void finalize_swapchain_locked(Entry *e)
{
	IDXGISwapChain *sc = e->swapchain;
	IDXGISwapChain3 *sc3 = nullptr;
	if (SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&sc3))) && sc3 != nullptr)
		sc3->Release(); // non-owning; the object outlives every use we make of it
	IDXGISwapChain1 *sc1 = nullptr;
	if (SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&sc1))) && sc1 != nullptr)
		sc1->Release();
	const bool same_object3 = sc3 != nullptr && static_cast<void *>(sc3) == static_cast<void *>(sc);
	const bool same_object1 = sc1 != nullptr && static_cast<void *>(sc1) == static_cast<void *>(sc);
	e->swapchain3 = same_object3 ? sc3 : nullptr;

	ID3D12CommandQueue *queue_arg = nullptr;
	if (e->device_arg_raw != nullptr && SUCCEEDED(e->device_arg_raw->QueryInterface(IID_PPV_ARGS(&queue_arg))) && queue_arg != nullptr)
		queue_arg->Release();
	const int pick = core::pick_present_queue(g_queues, reinterpret_cast<std::uint64_t>(queue_arg));
	e->queue = pick >= 0 ? reinterpret_cast<ID3D12CommandQueue *>(g_queues[static_cast<std::size_t>(pick)].id) : nullptr;

	unsigned patched = 0;
	if (same_object1 && patch_slot(sc, slot::kSwapChain1_Present1, reinterpret_cast<void *>(&hk_Present1), "IDXGISwapChain1::Present1") != nullptr)
		++patched;
	if (same_object3 && patch_slot(sc, slot::kSwapChain3_ResizeBuffers1, reinterpret_cast<void *>(&hk_ResizeBuffers1), "IDXGISwapChain3::ResizeBuffers1") != nullptr)
		++patched;
	e->finalized = true;
	fg::on_swapchain_finalised(sc, e->swapchain3, g_device, e->queue);
	STRAY_LOG_WARN("present owner: swapchain %p FINALISED at first Present (SwapChain1=%d SwapChain3=%d) device-arg=%p -> %s queue %p (%s); %u extra slot(s) patched",
		static_cast<void *>(sc), same_object1 ? 1 : 0, same_object3 ? 1 : 0, static_cast<void *>(queue_arg),
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

// A creation hook's shared tail: forward to the PER-VTABLE original for `self` (never a global),
// then note the returned swapchain lazily, but only at the outermost creation call (DXVK's
// legacy CreateSwapChain re-enters CreateSwapChainForHwnd on the same factory).
struct CreateGuard
{
	CreateGuard() { ++t_create_depth; }
	~CreateGuard() { --t_create_depth; }
	bool outermost() const { return t_create_depth == 1; }
};

HRESULT STDMETHODCALLTYPE hk_CreateSwapChain(IDXGIFactory *self, IUnknown *device, DXGI_SWAP_CHAIN_DESC *desc, IDXGISwapChain **out)
{
	auto orig = reinterpret_cast<PFN_CreateSwapChain>(original_for(self, slot::kFactory_CreateSwapChain));
	if (orig == nullptr)
	{
		STRAY_LOG_ERROR("present owner: CreateSwapChain has no recorded original for factory %p; cannot forward", static_cast<void *>(self));
		return E_FAIL;
	}
	CreateGuard guard;
	const HRESULT hr = orig(self, device, desc, out);
	if (SUCCEEDED(hr) && out != nullptr && *out != nullptr && guard.outermost() && !in_own_code())
		note_swapchain_lazy(*out, device, "CreateSwapChain");
	return hr;
}

HRESULT STDMETHODCALLTYPE hk_CreateSwapChainForHwnd(IDXGIFactory2 *self, IUnknown *device, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1 *desc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *fs, IDXGIOutput *restrict_to, IDXGISwapChain1 **out)
{
	auto orig = reinterpret_cast<PFN_CreateSwapChainForHwnd>(original_for(self, slot::kFactory2_CreateSwapChainForHwnd));
	if (orig == nullptr)
	{
		STRAY_LOG_ERROR("present owner: CreateSwapChainForHwnd has no recorded original for factory %p; cannot forward", static_cast<void *>(self));
		return E_FAIL;
	}
	CreateGuard guard;
	const HRESULT hr = orig(self, device, hwnd, desc, fs, restrict_to, out);
	if (SUCCEEDED(hr) && out != nullptr && *out != nullptr && guard.outermost() && !in_own_code())
		note_swapchain_lazy(*out, device, "CreateSwapChainForHwnd");
	return hr;
}

HRESULT STDMETHODCALLTYPE hk_CreateSwapChainForCoreWindow(IDXGIFactory2 *self, IUnknown *device, IUnknown *window, const DXGI_SWAP_CHAIN_DESC1 *desc, IDXGIOutput *restrict_to, IDXGISwapChain1 **out)
{
	auto orig = reinterpret_cast<PFN_CreateSwapChainForCoreWindow>(original_for(self, slot::kFactory2_CreateSwapChainForCoreWindow));
	if (orig == nullptr)
	{
		STRAY_LOG_ERROR("present owner: CreateSwapChainForCoreWindow has no recorded original for factory %p; cannot forward", static_cast<void *>(self));
		return E_FAIL;
	}
	CreateGuard guard;
	const HRESULT hr = orig(self, device, window, desc, restrict_to, out);
	if (SUCCEEDED(hr) && out != nullptr && *out != nullptr && guard.outermost() && !in_own_code())
		note_swapchain_lazy(*out, device, "CreateSwapChainForCoreWindow");
	return hr;
}

HRESULT STDMETHODCALLTYPE hk_CreateSwapChainForComposition(IDXGIFactory2 *self, IUnknown *device, const DXGI_SWAP_CHAIN_DESC1 *desc, IDXGIOutput *restrict_to, IDXGISwapChain1 **out)
{
	auto orig = reinterpret_cast<PFN_CreateSwapChainForComposition>(original_for(self, slot::kFactory2_CreateSwapChainForComposition));
	if (orig == nullptr)
	{
		STRAY_LOG_ERROR("present owner: CreateSwapChainForComposition has no recorded original for factory %p; cannot forward", static_cast<void *>(self));
		return E_FAIL;
	}
	CreateGuard guard;
	const HRESULT hr = orig(self, device, desc, restrict_to, out);
	if (SUCCEEDED(hr) && out != nullptr && *out != nullptr && guard.outermost() && !in_own_code())
		note_swapchain_lazy(*out, device, "CreateSwapChainForComposition");
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
	bool just_finalized = false;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		Entry *e = find_entry_locked(sc);
		if (e == nullptr)
		{
			++g_stats.skipped;
			return;
		}
		// First Present of this swapchain: it is fully built now, so do the deferred work the
		// creation hook must not (QI, queue pick, Present1/Resize1 patch). (facts §20)
		if (!e->finalized)
		{
			OwnCodeScope own_finalize; // the QIs below must not be treated as the game's calls
			finalize_swapchain_locked(e);
			just_finalized = true;
		}
		entry = *e;
	}
	if (just_finalized)
		report_back_buffers(sc, true); // outside the lock: it does GetDesc/GetBuffer + sink()
	OwnCodeScope own; // everything below is ours: the ring's Reset, the sink's recording, our execute

	icept::PresentContext pc;
	pc.queue = entry.queue;
	pc.swapchain = entry.swapchain3;
	pc.frame = sk->next_frame();
	pc.backend_cookie = 0;
	g_stats_last_frame.store(pc.frame, std::memory_order_relaxed);
	// The current back buffer, non-owning, exactly as the ReShade host reported it — or, with
	// frame generation armed, the REPLACEMENT the game rendered this frame into (the real ring
	// only ever receives our copies, fg_present.hpp).
	if (ID3D12Resource *replacement = fg::game_frame(sc))
	{
		pc.back_buffer = replacement;
	}
	else
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
			perf::Scope _wait(perf::kPresentWait);
			g_fence->SetEventOnCompletion(slot->pending, g_fence_event);
			::WaitForSingleObject(g_fence_event, 2000);
		}
		{
			perf::Scope _reset(perf::kPresentOwner);
			if (SUCCEEDED(slot->allocator->Reset()) && SUCCEEDED(slot->list->Reset(slot->allocator.Get(), nullptr)))
				pc.present_list = slot->list.Get();
			else
				slot = nullptr;
		}
	}
	g_list_used.store(false, std::memory_order_relaxed);

	{
		static std::atomic<bool> s_said{ false };
		if (!s_said.exchange(true))
			STRAY_LOG_WARN("present owner: FIRST Present delivered on_present (queue=%p present_list=%p back_buffer=%p) - the frame boundary is live",
				static_cast<void *>(pc.queue), static_cast<void *>(pc.present_list), reinterpret_cast<void *>(pc.back_buffer));
	}
	sk->on_present(pc);
	// Frame generation's game-thread half: the generated frame's production (and, in ngx mode,
	// the validation crop) recorded onto this same list, after everything the sink recorded.
	// Executed whenever FG could have recorded: a generator that records feature-creation work
	// and then declines (a failed evaluate) must still have that work run, and an empty closed
	// list costs nothing.
	if (pc.present_list != nullptr && fg::enabled() && fg::game_frame(sc) != nullptr)
	{
		fg::record(sc, pc.present_list, pc.frame);
		g_list_used.store(true, std::memory_order_relaxed);
	}

	{
		std::lock_guard<std::mutex> lock(g_mutex);
		++g_stats.presents;
	}
	if (slot != nullptr)
	{
		perf::Scope _submit(perf::kPresentOwner);
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

// Frame generation owns the present once it is armed: it copies the game's frame (and the
// generated one) into the real ring and calls the ORIGINAL Present itself, once or twice. The
// frame index it is handed is the one before_present just consumed.
bool fg_present(IDXGISwapChain *self, UINT sync, UINT flags, HRESULT *hr)
{
	if (!fg::enabled() || (flags & DXGI_PRESENT_TEST) != 0 || in_own_code())
		return false;
	fg::PresentArgs args;
	args.sc = self;
	args.sync = sync;
	args.flags = flags;
	{
		icept::Sink *sk = sink();
		args.frame = sk != nullptr ? g_stats_last_frame.load(std::memory_order_relaxed) : 0;
	}
	args.orig_present = original_for(self, slot::kSwapChain_Present);
	long out = S_OK;
	OwnCodeScope own; // the presents and copies below are ours
	if (!fg::present(args, &out))
		return false;
	*hr = static_cast<HRESULT>(out);
	return true;
}

HRESULT STDMETHODCALLTYPE hk_Present(IDXGISwapChain *self, UINT sync, UINT flags)
{
	PRESENT_HOOK_TRACE("Present", self);
	auto orig = reinterpret_cast<PFN_Present>(original_for(self, slot::kSwapChain_Present));
	before_present(self, flags);
	HRESULT hr = S_OK;
	if (fg_present(self, sync, flags, &hr))
		return hr;
	if (orig == nullptr)
		return S_OK;
	const std::uint64_t t0 = perf::stall_clock_ns();
	hr = orig(self, sync, flags);
	perf::stall_note_orig(perf::kOrigPresent, perf::stall_clock_ns() - t0);
	return hr;
}

HRESULT STDMETHODCALLTYPE hk_Present1(IDXGISwapChain1 *self, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS *params)
{
	PRESENT_HOOK_TRACE("Present1", self);
	auto orig = reinterpret_cast<PFN_Present1>(original_for(self, slot::kSwapChain1_Present1));
	before_present(self, flags);
	HRESULT hr = S_OK;
	if (fg_present(self, sync, flags, &hr))
		return hr; // UE 4.27 calls Present, not Present1 (facts §32.4); FG presents through Present either way
	if (orig == nullptr)
		return S_OK;
	const std::uint64_t t0 = perf::stall_clock_ns();
	hr = orig(self, sync, flags, params);
	perf::stall_note_orig(perf::kOrigPresent, perf::stall_clock_ns() - t0);
	return hr;
}

HRESULT STDMETHODCALLTYPE hk_ResizeBuffers(IDXGISwapChain *self, UINT count, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags)
{
	PRESENT_HOOK_TRACE("ResizeBuffers", self);
	auto orig = reinterpret_cast<PFN_ResizeBuffers>(original_for(self, slot::kSwapChain_ResizeBuffers));
	const bool game_call = !in_own_code();
	if (game_call)
	{
		report_back_buffers(self, false);
		fg::before_reconfigure(self, "ResizeBuffers");
	}
	const HRESULT hr = orig != nullptr ? orig(self, count, w, h, fmt, flags) : E_FAIL;
	if (game_call)
		fg::after_reconfigure(self, "ResizeBuffers", /*drop_replacements=*/true, count, hr);
	if (game_call)
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
	PRESENT_HOOK_TRACE("ResizeBuffers1", self);
	auto orig = reinterpret_cast<PFN_ResizeBuffers1>(original_for(self, slot::kSwapChain3_ResizeBuffers1));
	const bool game_call = !in_own_code();
	if (game_call)
	{
		report_back_buffers(self, false);
		fg::before_reconfigure(self, "ResizeBuffers1");
	}
	const HRESULT hr = orig != nullptr ? orig(self, count, w, h, fmt, flags, node_masks, queues) : E_FAIL;
	if (game_call)
		fg::after_reconfigure(self, "ResizeBuffers1", /*drop_replacements=*/true, count, hr);
	if (game_call)
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
	// patch_slot records the original per (vtable slot); the hooks resolve it with original_for,
	// so no g_orig_* shadow is kept — that shadow is what collapsed two DXGI vtables into one and
	// crashed (facts §20). A non-null return counts as "newly patched" for the log only.
	unsigned n = 0;
	if (patch_slot(factory, slot::kFactory_CreateSwapChain, reinterpret_cast<void *>(&hk_CreateSwapChain), "IDXGIFactory::CreateSwapChain") != nullptr)
		++n;
	IDXGIFactory2 *f2 = nullptr;
	if (SUCCEEDED(factory->QueryInterface(IID_PPV_ARGS(&f2))) && f2 != nullptr)
	{
		const bool same = static_cast<void *>(f2) == static_cast<void *>(factory);
		f2->Release();
		if (same)
		{
			if (patch_slot(factory, slot::kFactory2_CreateSwapChainForHwnd, reinterpret_cast<void *>(&hk_CreateSwapChainForHwnd), "IDXGIFactory2::CreateSwapChainForHwnd") != nullptr)
				++n;
			if (patch_slot(factory, slot::kFactory2_CreateSwapChainForCoreWindow, reinterpret_cast<void *>(&hk_CreateSwapChainForCoreWindow), "IDXGIFactory2::CreateSwapChainForCoreWindow") != nullptr)
				++n;
			if (patch_slot(factory, slot::kFactory2_CreateSwapChainForComposition, reinterpret_cast<void *>(&hk_CreateSwapChainForComposition), "IDXGIFactory2::CreateSwapChainForComposition") != nullptr)
				++n;
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

	// [STRAYDLSS] NgxFG and its knobs (fg_present.hpp). Read here, once, before the game can
	// create a swapchain: the GetBuffer hook has to be in place before the first GetBuffer.
	{
		fg::Config fc;
		fc.enabled = host::cfg::get_bool("NgxFG", false);
		const int mode = host::cfg::get_int("NgxFGMode", 2);
		fc.mode = mode == 1 ? fg::Mode::experiment : fg::Mode::ngx;
		const int pacing = host::cfg::get_int("NgxFGPacing", 1);
		fc.pacing = pacing == 0 ? fg::Pacing::none : pacing == 2 ? fg::Pacing::sync : fg::Pacing::thread;
		fc.wait_ms = host::cfg::get_int("NgxFGWaitMs", -1);
		fc.band = host::cfg::get_bool("NgxFGBand", true);
		fc.validate = host::cfg::get_int("NgxFGValidate", 1);
		fc.reflex = host::cfg::get_int("NgxFGReflex", 1);
		fc.trace = host::cfg::get_int("NgxFGTrace", 0);
		fg::configure(fc);
	}

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
	fg::uninstall();
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
void set_hook_trace(bool on) { g_hook_trace.store(on, std::memory_order_relaxed); }
bool hook_trace() { return g_hook_trace.load(std::memory_order_relaxed); }

Stats stats()
{
	std::lock_guard<std::mutex> lock(g_mutex);
	return g_stats;
}

const char *report() { return g_report; }

} // namespace stray_dlss::native::present
