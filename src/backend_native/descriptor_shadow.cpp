#include "backend_native/descriptor_shadow.hpp"

#include "log.hpp"

#include <d3d12.h>

#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

namespace stray_dlss::native::shadow {
namespace {

std::shared_mutex g_mutex;
std::unordered_map<icept::DescriptorId, ViewEntry> g_slots;
// Reverse index for forget_resource: resource -> the slots that reference it. Slots are
// removed from here lazily (a slot rewritten to another resource stays listed under the
// old one until that one dies, when the lookup simply finds a different resource and skips).
std::unordered_map<icept::ResourceId, std::vector<icept::DescriptorId>> g_by_resource;

struct HeapRecord
{
	::ID3D12DescriptorHeap *heap = nullptr;
	core::HeapSpan span;
};
std::vector<HeapRecord> g_heaps;

std::atomic<std::uint64_t> g_unknown_lookups{ 0 };
std::atomic<std::uint64_t> g_null_lookups{ 0 };
std::atomic<std::uint64_t> g_dead_lookups{ 0 };
std::atomic<std::uint64_t> g_unknown_copies{ 0 };
std::atomic<std::uint64_t> g_seq{ 0 };
std::uint64_t g_views = 0;
std::uint64_t g_copies = 0;

} // namespace

void note_view(icept::DescriptorId cpu, const ViewEntry &entry)
{
	if (cpu == 0)
		return;
	std::unique_lock<std::shared_mutex> lock(g_mutex);
	ViewEntry &slot = g_slots[cpu];
	slot = entry;
	slot.seq = g_seq.fetch_add(1, std::memory_order_relaxed) + 1;
	slot.via_copy = false;
	if (entry.resource != 0)
		g_by_resource[entry.resource].push_back(cpu);
	++g_views;
}

void note_null_view(icept::DescriptorId cpu)
{
	if (cpu == 0)
		return;
	std::unique_lock<std::shared_mutex> lock(g_mutex);
	ViewEntry &slot = g_slots[cpu];
	slot = ViewEntry{};
	slot.is_null = true;
	slot.seq = g_seq.fetch_add(1, std::memory_order_relaxed) + 1;
	++g_views;
}

void note_copy(icept::DescriptorId dst, icept::DescriptorId src)
{
	if (dst == 0)
		return;
	std::unique_lock<std::shared_mutex> lock(g_mutex);
	++g_copies;
	const auto it = g_slots.find(src);
	if (it == g_slots.end())
	{
		// Not necessarily an error: a slot copied before we attached, a sampler, or a null
		// descriptor. Counted so a persistent stream of them is visible.
		g_slots.erase(dst);
		g_unknown_copies.fetch_add(1, std::memory_order_relaxed);
		return;
	}
	ViewEntry copy = it->second;
	copy.seq = g_seq.fetch_add(1, std::memory_order_relaxed) + 1;
	copy.via_copy = true;
	copy.src_slot = src;
	g_slots[dst] = copy;
	if (copy.resource != 0)
		g_by_resource[copy.resource].push_back(dst);
}

void note_heap_bound(::ID3D12DescriptorHeap *heap)
{
	if (heap == nullptr)
		return;
	// Read every time: three trivial calls, and a heap pointer reused by the runtime for a
	// new heap would otherwise carry a stale span forever.
	const D3D12_DESCRIPTOR_HEAP_DESC desc = heap->GetDesc();
	if (desc.Type != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)
		return; // samplers are never resolved
	core::HeapSpan span;
	span.cpu_base = heap->GetCPUDescriptorHandleForHeapStart().ptr;
	span.gpu_base = (desc.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) != 0
		? heap->GetGPUDescriptorHandleForHeapStart().ptr : 0;
	span.count = desc.NumDescriptors;
	// The increment is a device constant; ask the heap's device once.
	static std::atomic<std::uint32_t> s_increment{ 0 };
	std::uint32_t inc = s_increment.load(std::memory_order_relaxed);
	if (inc == 0)
	{
		ID3D12Device *device = nullptr;
		if (SUCCEEDED(heap->GetDevice(IID_PPV_ARGS(&device))) && device != nullptr)
		{
			inc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
			device->Release();
			s_increment.store(inc, std::memory_order_relaxed);
		}
	}
	span.increment = inc;

	std::unique_lock<std::shared_mutex> lock(g_mutex);
	for (HeapRecord &r : g_heaps)
	{
		if (r.heap == heap)
		{
			r.span = span;
			return;
		}
	}
	g_heaps.push_back(HeapRecord{ heap, span });
	STRAY_LOG_INFO("descriptor_shadow: heap %p bound: %u descriptors, inc %u, cpu %llx gpu %llx (%s)",
		static_cast<void *>(heap), span.count, span.increment,
		static_cast<unsigned long long>(span.cpu_base), static_cast<unsigned long long>(span.gpu_base),
		span.gpu_base != 0 ? "shader-visible" : "CPU-only");
}

bool lookup(icept::DescriptorId cpu, ViewEntry &out)
{
	if (cpu == 0)
		return false;
	std::shared_lock<std::shared_mutex> lock(g_mutex);
	const auto it = g_slots.find(cpu);
	if (it == g_slots.end())
		return false;
	out = it->second;
	return true;
}

bool gpu_to_cpu(std::uint64_t gpu, icept::DescriptorId &cpu)
{
	if (gpu == 0)
		return false;
	std::shared_lock<std::shared_mutex> lock(g_mutex);
	for (const HeapRecord &r : g_heaps)
		if (core::gpu_to_cpu(gpu, r.span, cpu))
			return true;
	return false;
}

void forget_resource(icept::ResourceId res)
{
	if (res == 0)
		return;
	std::unique_lock<std::shared_mutex> lock(g_mutex);
	const auto it = g_by_resource.find(res);
	if (it == g_by_resource.end())
		return;
	for (const icept::DescriptorId slot : it->second)
	{
		const auto s = g_slots.find(slot);
		if (s != g_slots.end() && s->second.resource == res)
			s->second.dead = true; // a tombstone, overwritten by the next write to the slot
	}
	g_by_resource.erase(it);
}

std::uint64_t unknown_lookups() { return g_unknown_lookups.load(std::memory_order_relaxed); }
void count_unknown_lookup() { g_unknown_lookups.fetch_add(1, std::memory_order_relaxed); }
std::uint64_t unknown_copies() { return g_unknown_copies.load(std::memory_order_relaxed); }
std::uint64_t null_lookups() { return g_null_lookups.load(std::memory_order_relaxed); }
void count_null_lookup() { g_null_lookups.fetch_add(1, std::memory_order_relaxed); }
std::uint64_t dead_lookups() { return g_dead_lookups.load(std::memory_order_relaxed); }
void count_dead_lookup() { g_dead_lookups.fetch_add(1, std::memory_order_relaxed); }
std::uint64_t write_sequence() { return g_seq.load(std::memory_order_relaxed); }

Stats stats()
{
	std::shared_lock<std::shared_mutex> lock(g_mutex);
	Stats s;
	s.views = g_views;
	s.copies = g_copies;
	s.slots = g_slots.size();
	s.heaps = g_heaps.size();
	s.slots_buckets = g_slots.bucket_count();
	s.by_resource_keys = g_by_resource.size();
	s.by_resource_buckets = g_by_resource.bucket_count();
	std::uint64_t entries = 0;
	for (const auto &kv : g_by_resource)
		entries += kv.second.size();
	s.by_resource_entries = entries;
	return s;
}

void clear_for_test()
{
	std::unique_lock<std::shared_mutex> lock(g_mutex);
	g_slots.clear();
	g_by_resource.clear();
	g_heaps.clear();
	g_unknown_lookups.store(0);
	g_null_lookups.store(0);
	g_dead_lookups.store(0);
	g_unknown_copies.store(0);
	g_views = g_copies = 0;
}

} // namespace stray_dlss::native::shadow
