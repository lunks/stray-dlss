#include "backend_native/resource_registry.hpp"

#include "core/dxgi_format.hpp"
#include "core/va_map.hpp"
#include "log.hpp"

#include <d3d12.h>

#include <atomic>
#include <mutex>
#include <unordered_map>

namespace stray_dlss::native::registry {
namespace {

// {5D1E9C30-7A4B-4E02-9B1F-223344556677}, the same GUID the harness probe used.
constexpr GUID kSentinelGuid = { 0x5d1e9c30, 0x7a4b, 0x4e02, { 0x9b, 0x1f, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77 } };

struct Sentinel : IUnknown
{
	std::atomic<ULONG> refs{ 1 };
	icept::ResourceId id;
	explicit Sentinel(icept::ResourceId i) : id(i) {}
	virtual ~Sentinel() = default;
	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override
	{
		if (out == nullptr)
			return E_POINTER;
		if (riid == __uuidof(IUnknown))
		{
			*out = this;
			AddRef();
			return S_OK;
		}
		*out = nullptr;
		return E_NOINTERFACE;
	}
	ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
	ULONG STDMETHODCALLTYPE Release() override
	{
		const ULONG r = --refs;
		if (r == 0)
		{
			const icept::ResourceId dead = id;
			delete this;
			on_destroyed(dead);
		}
		return r;
	}
};

std::mutex g_mutex;
std::unordered_map<icept::ResourceId, icept::ResourceInfo> g_resources;
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
		if (va != 0 && info.buffer_size != 0)
			g_vas.insert(va, info.buffer_size, id);
		++g_stats.registered;
		g_stats.live = g_resources.size();
	}

	// The destruction callback. The resource holds the only reference after our Release.
	Sentinel *s = new Sentinel(id);
	if (FAILED(res->SetPrivateDataInterface(kSentinelGuid, s)))
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		++g_stats.sentinel_failures;
		if (g_stats.sentinel_failures == 1)
			STRAY_LOG_ERROR("resource_registry: SetPrivateDataInterface refused on %p - liveness for such "
				"resources cannot be tracked and their slots will go stale. First occurrence only.",
				static_cast<void *>(res));
	}
	s->Release();
}

bool is_live(icept::ResourceId res)
{
	if (res == 0)
		return false;
	std::lock_guard<std::mutex> lock(g_mutex);
	return g_resources.count(res) != 0;
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
	g_vas = core::VaMap{};
	g_stats = Stats{};
}

} // namespace stray_dlss::native::registry
