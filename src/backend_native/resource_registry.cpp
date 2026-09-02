#include "backend_native/resource_registry.hpp"

#include "backend_native/native_backend.hpp"

#include "core/dxgi_format.hpp"
#include "core/va_map.hpp"
#include "log.hpp"

#include <d3d12.h>

#include <atomic>
#include <cstring>
#include <new>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace stray_dlss::native::registry {
namespace {

// {5D1E9C30-7A4B-4E02-9B1F-223344556677}, the same GUID the harness probe used. The same
// GUID in every life ON PURPOSE: re-registering a resource in life 2 replaces life 1's
// sentinel through SetPrivateDataInterface, and the runtime's Release of the old one lands
// in life 1's inert stubs.
constexpr GUID kSentinelGuid = { 0x5d1e9c30, 0x7a4b, 0x4e02, { 0x9b, 0x1f, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77 } };

// IUnknown, laid out by hand (the COM ABI: a pointer to a table of three functions at
// offset 0, `this` in rcx). No C++ vtable: the table has to be somewhere we can rewrite at
// detach and that outlives the image.
struct SentinelVtbl
{
	void *query_interface;
	void *add_ref;
	void *release;
};
struct Sentinel
{
	const SentinelVtbl *vtbl;
	std::atomic<ULONG> refs;
	icept::ResourceId id;
};

// One page per LIFE, VirtualAlloc'ed (never inside the image, never freed): the live vtable
// at the start, the three inert stubs further in. RWX for its whole life: detach() has to
// write the table and the runtime has to execute the stubs, and a Wine/Proton process has
// no policy against it.
struct SentinelPage
{
	SentinelVtbl vtbl;
	unsigned char stubs[64];
};
SentinelPage *g_page = nullptr;
std::atomic<bool> g_armed{ false };

// x64 stubs, Microsoft calling convention (rcx=this, rdx, r8; result in eax):
//   QueryInterface: if (r8) *r8 = nullptr; return E_NOINTERFACE (0x80004002)
//   AddRef:         return 1
//   Release:        return 0   (the block is leaked on purpose; the runtime never dereferences
//                              a released sentinel again)
constexpr unsigned char kStubQueryInterface[] = {
	0x4D, 0x85, 0xC0,                               // test r8, r8
	0x74, 0x07,                                     // jz +7
	0x49, 0xC7, 0x00, 0x00, 0x00, 0x00, 0x00,       // mov qword ptr [r8], 0
	0xB8, 0x02, 0x40, 0x00, 0x80,                   // mov eax, 0x80004002
	0xC3,                                           // ret
};
constexpr unsigned char kStubAddRef[] = { 0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3 }; // mov eax, 1; ret
constexpr unsigned char kStubRelease[] = { 0x31, 0xC0, 0xC3 };                   // xor eax, eax; ret
constexpr unsigned kStubQiOffset = 0, kStubAddRefOffset = 24, kStubReleaseOffset = 40;

HRESULT STDMETHODCALLTYPE sentinel_query_interface(Sentinel *self, REFIID riid, void **out)
{
	if (out == nullptr)
		return E_POINTER;
	if (riid == __uuidof(IUnknown))
	{
		*out = self;
		++self->refs;
		return S_OK;
	}
	*out = nullptr;
	return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE sentinel_add_ref(Sentinel *self)
{
	return ++self->refs;
}

ULONG STDMETHODCALLTYPE sentinel_release(Sentinel *self)
{
	const ULONG r = --self->refs;
	if (r == 0)
	{
		const icept::ResourceId dead = self->id;
		::HeapFree(::GetProcessHeap(), 0, self);
		on_destroyed(dead);
	}
	return r;
}

SentinelPage *allocate_page()
{
	void *mem = ::VirtualAlloc(nullptr, sizeof(SentinelPage), MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
	if (mem == nullptr)
		return nullptr;
	auto *page = static_cast<SentinelPage *>(mem);
	std::memcpy(page->stubs + kStubQiOffset, kStubQueryInterface, sizeof(kStubQueryInterface));
	std::memcpy(page->stubs + kStubAddRefOffset, kStubAddRef, sizeof(kStubAddRef));
	std::memcpy(page->stubs + kStubReleaseOffset, kStubRelease, sizeof(kStubRelease));
	page->vtbl.query_interface = reinterpret_cast<void *>(&sentinel_query_interface);
	page->vtbl.add_ref = reinterpret_cast<void *>(&sentinel_add_ref);
	page->vtbl.release = reinterpret_cast<void *>(&sentinel_release);
	::FlushInstructionCache(::GetCurrentProcess(), page, sizeof(SentinelPage));
	return page;
}

std::mutex g_mutex;
std::unordered_map<icept::ResourceId, icept::ResourceInfo> g_resources;
std::unordered_set<icept::ResourceId> g_ever_seen;
core::VaMap g_vas;
void (*g_listener)(icept::ResourceId) = nullptr;
Stats g_stats;

} // namespace

void note_created(::ID3D12Resource *res)
{
	if (res == nullptr)
		return;
	const auto id = reinterpret_cast<icept::ResourceId>(res);
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		if (g_resources.count(id) != 0)
			return;
	}

	// The ONLY place a resource is interrogated. Everything later reads the snapshot.
	const D3D12_RESOURCE_DESC d = res->GetDesc();
	icept::ResourceInfo info;
	info.is_buffer = d.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER;
	if (info.is_buffer)
	{
		info.buffer_size = d.Width;
	}
	else
	{
		info.width = static_cast<std::uint32_t>(d.Width);
		info.height = d.Height;
		info.mip_levels = d.MipLevels;
		info.array_size = d.DepthOrArraySize;
		info.sample_count = d.SampleDesc.Count;
		info.dxgi_format = static_cast<std::uint32_t>(d.Format);
		info.format = tex_format_from_dxgi(info.dxgi_format);
		info.is_3d = d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D;
	}
	info.allow_uav = (d.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0;
	// Committed and placed resources answer this; reserved ones fail and stay "not upload".
	D3D12_HEAP_PROPERTIES hp = {};
	D3D12_HEAP_FLAGS hf = D3D12_HEAP_FLAG_NONE;
	if (SUCCEEDED(res->GetHeapProperties(&hp, &hf)))
		info.upload_heap = hp.Type == D3D12_HEAP_TYPE_UPLOAD ||
			(hp.Type == D3D12_HEAP_TYPE_CUSTOM && hp.CPUPageProperty == D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE);
	const std::uint64_t va = info.is_buffer ? res->GetGPUVirtualAddress() : 0;

	{
		std::lock_guard<std::mutex> lock(g_mutex);
		if (g_resources.count(id) != 0)
			return;
		g_resources[id] = info;
		g_ever_seen.insert(id);
		if (va != 0 && info.buffer_size != 0)
			g_vas.insert(va, info.buffer_size, id);
		++g_stats.registered;
		g_stats.live = g_resources.size();
	}

	// The destruction callback. The resource holds the only reference after our Release.
	// [STRAYDLSS] NativeSentinel=0 skips this, to isolate SetPrivateDataInterface as a crash
	// suspect on the box; then a resource stays live until forget_resource is never called,
	// which over-reports liveness but cannot fault.
	if (!use_sentinel())
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		++g_stats.sentinel_failures;
		return;
	}
	if (!g_armed.load(std::memory_order_acquire) || g_page == nullptr)
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		++g_stats.unarmed;
		return;
	}
	auto *s = static_cast<Sentinel *>(::HeapAlloc(::GetProcessHeap(), 0, sizeof(Sentinel)));
	if (s == nullptr)
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		++g_stats.sentinel_failures;
		return;
	}
	s->vtbl = &g_page->vtbl;
	new (&s->refs) std::atomic<ULONG>(1);
	s->id = id;
	if (FAILED(res->SetPrivateDataInterface(kSentinelGuid, reinterpret_cast<IUnknown *>(s))))
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		++g_stats.sentinel_failures;
		if (g_stats.sentinel_failures == 1)
			STRAY_LOG_ERROR("resource_registry: SetPrivateDataInterface refused on %p - liveness for such "
				"resources cannot be tracked and their slots will go stale. First occurrence only.",
				static_cast<void *>(res));
	}
	reinterpret_cast<IUnknown *>(s)->Release();
}

bool arm()
{
	if (g_page == nullptr)
	{
		g_page = allocate_page();
		if (g_page == nullptr)
		{
			STRAY_LOG_ERROR("resource_registry: VirtualAlloc for the sentinel page failed (error %lu); "
				"no sentinel will be attached this life and liveness is UNKNOWN", ::GetLastError());
			return false;
		}
		STRAY_LOG_INFO("resource_registry: sentinel page %p (vtable outside the image; stubs at +%u/+%u/+%u)",
			static_cast<void *>(g_page), kStubQiOffset, kStubAddRefOffset, kStubReleaseOffset);
	}
	g_armed.store(true, std::memory_order_release);
	return true;
}

bool armed() { return g_armed.load(std::memory_order_acquire); }

const void *sentinel_vtable_for_test() { return g_page != nullptr ? &g_page->vtbl : nullptr; }

void detach()
{
	g_armed.store(false, std::memory_order_release);
	SentinelPage *page = g_page;
	g_page = nullptr;
	std::uint64_t orphaned = 0;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		orphaned = g_resources.size();
		g_resources.clear();
		g_ever_seen.clear();
		g_vas = core::VaMap{};
		g_stats.live = 0;
		g_stats.orphaned += orphaned;
		g_listener = nullptr;
	}
	if (page == nullptr)
		return;
	// The order matters: Release first, so a sentinel dying between these three writes
	// cannot reach this image's code any more, then the two that are never called by the
	// runtime's private-data release path.
	::InterlockedExchangePointer(&page->vtbl.release, page->stubs + kStubReleaseOffset);
	::InterlockedExchangePointer(&page->vtbl.add_ref, page->stubs + kStubAddRefOffset);
	::InterlockedExchangePointer(&page->vtbl.query_interface, page->stubs + kStubQiOffset);
	STRAY_LOG_INFO("resource_registry: detached - %llu live sentinel(s) made inert on page %p; the page is "
		"leaked on purpose so a later Release cannot reach an unmapped image",
		static_cast<unsigned long long>(orphaned), static_cast<void *>(page));
}

bool is_live(icept::ResourceId res)
{
	if (res == 0)
		return false;
	std::lock_guard<std::mutex> lock(g_mutex);
	return g_resources.count(res) != 0;
}

bool ever_seen(icept::ResourceId res)
{
	if (res == 0)
		return false;
	std::lock_guard<std::mutex> lock(g_mutex);
	return g_ever_seen.count(res) != 0;
}

bool describe(icept::ResourceId res, icept::ResourceInfo &out)
{
	if (res == 0)
		return false;
	std::lock_guard<std::mutex> lock(g_mutex);
	const auto it = g_resources.find(res);
	if (it == g_resources.end())
		return false;
	out = it->second;
	return true;
}

bool buffer_for_va(std::uint64_t gpu_va, icept::BufferRange &out)
{
	if (gpu_va == 0)
		return false;
	std::lock_guard<std::mutex> lock(g_mutex);
	std::uint64_t id = 0, offset = 0;
	if (!g_vas.find(gpu_va, id, offset))
		return false;
	const auto it = g_resources.find(id);
	if (it == g_resources.end())
		return false;
	out.buffer = id;
	out.offset = offset;
	out.size = it->second.buffer_size > offset ? it->second.buffer_size - offset : 0;
	return true;
}

void on_destroyed(icept::ResourceId res)
{
	void (*listener)(icept::ResourceId) = nullptr;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		if (g_resources.erase(res) == 0)
			return;
		g_vas.erase(res);
		++g_stats.destroyed;
		g_stats.live = g_resources.size();
		listener = g_listener;
	}
	if (listener != nullptr)
		listener(res);
}

void set_destroy_listener(void (*listener)(icept::ResourceId))
{
	std::lock_guard<std::mutex> lock(g_mutex);
	g_listener = listener;
}

Stats stats()
{
	std::lock_guard<std::mutex> lock(g_mutex);
	return g_stats;
}

void clear_for_test()
{
	std::lock_guard<std::mutex> lock(g_mutex);
	g_resources.clear();
	g_ever_seen.clear();
	g_vas = core::VaMap{};
	g_stats = Stats{};
}

} // namespace stray_dlss::native::registry
