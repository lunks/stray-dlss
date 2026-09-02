#include "backend_native/descriptor_shadow.hpp"

#include "backend_native/resource_registry.hpp"
#include "log.hpp"

#include <d3d12.h>

#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

namespace stray_dlss::native::shadow {
namespace {

// SHARDED by descriptor address. MEASURED (facts §27, arm C on the user's traverse): with one
// exclusive shared_mutex for the whole map, note_copy cost 6 -> 26 -> 53 ms per frame as UE4's
// RHI threads pushed 2.7k -> 6.7k descriptor copies a frame through it - ~8 us per copy, the
// signature of lock convoy (an SRW lock under Wine), not of the hash map (which never rehashed).
// 64 shards keyed on the handle's 4 KB page put threads writing different regions of the
// 500k-descriptor online heap on different locks, and the range API below takes a shard lock
// once per RUN of consecutive descriptors rather than once per descriptor. No thread ever holds
// two shard locks: a copy reads its sources under a shared lock, releases, then writes.
constexpr unsigned kShardBits = 6;
constexpr unsigned kShards = 1u << kShardBits;
struct Shard
{
	std::shared_mutex mutex;
	std::unordered_map<icept::DescriptorId, ViewEntry> slots;
};
Shard g_shards[kShards];
inline unsigned shard_index(icept::DescriptorId cpu) { return static_cast<unsigned>((cpu >> 12) & (kShards - 1)); }
inline Shard &shard_of(icept::DescriptorId cpu) { return g_shards[shard_index(cpu)]; }

std::mutex g_heaps_mutex;
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
std::atomic<std::uint64_t> g_views{ 0 };
std::atomic<std::uint64_t> g_copies{ 0 };

} // namespace

void note_view(icept::DescriptorId cpu, const ViewEntry &entry)
{
	if (cpu == 0)
		return;
	// The generation stamp, taken here when the caller did not (a hand-built entry, as the
	// harness writes them), outside the shadow's lock: the registry has its own.
	std::uint64_t gen = entry.resource_gen;
	if (gen == 0 && entry.resource != 0)
		gen = registry::generation_of(entry.resource);
	Shard &sh = shard_of(cpu);
	std::unique_lock<std::shared_mutex> lock(sh.mutex);
	ViewEntry &slot = sh.slots[cpu];
	slot = entry;
	slot.resource_gen = gen;
	slot.seq = g_seq.fetch_add(1, std::memory_order_relaxed) + 1;
	slot.via_copy = false;
	g_views.fetch_add(1, std::memory_order_relaxed);
}

void note_null_view(icept::DescriptorId cpu)
{
	if (cpu == 0)
		return;
	Shard &sh = shard_of(cpu);
	std::unique_lock<std::shared_mutex> lock(sh.mutex);
	ViewEntry &slot = sh.slots[cpu];
	slot = ViewEntry{};
	slot.is_null = true;
	slot.seq = g_seq.fetch_add(1, std::memory_order_relaxed) + 1;
	g_views.fetch_add(1, std::memory_order_relaxed);
}

void note_copy_range(icept::DescriptorId dst, icept::DescriptorId src, std::uint32_t n, std::uint32_t inc)
{
	if (dst == 0 || n == 0)
		return;
	g_copies.fetch_add(n, std::memory_order_relaxed);
	// Pass 1: read every source entry, one shared lock per run of sources in one shard.
	// (A source that is itself in this copy's destination range is read as it was BEFORE the
	// copy, which is D3D12's semantics for a CopyDescriptors: it copies by value.)
	std::vector<ViewEntry> src_entries(n);
	std::vector<unsigned char> known(n, 0);
	{
		std::uint32_t i = 0;
		while (i < n)
		{
			const unsigned si = shard_index(src + static_cast<std::uint64_t>(i) * inc);
			Shard &sh = g_shards[si];
			std::shared_lock<std::shared_mutex> lock(sh.mutex);
			for (; i < n && shard_index(src + static_cast<std::uint64_t>(i) * inc) == si; ++i)
			{
				const auto it = sh.slots.find(src + static_cast<std::uint64_t>(i) * inc);
				if (it != sh.slots.end())
				{
					src_entries[i] = it->second;
					known[i] = 1;
				}
			}
		}
	}
	// Pass 2: write every destination, one exclusive lock per run of destinations in one shard.
	std::uint32_t i = 0;
	while (i < n)
	{
		const unsigned di = shard_index(dst + static_cast<std::uint64_t>(i) * inc);
		Shard &sh = g_shards[di];
		std::unique_lock<std::shared_mutex> lock(sh.mutex);
		for (; i < n && shard_index(dst + static_cast<std::uint64_t>(i) * inc) == di; ++i)
		{
			const icept::DescriptorId d = dst + static_cast<std::uint64_t>(i) * inc;
			if (!known[i])
			{
				// Not necessarily an error: a slot copied before we attached, a sampler, or a
				// null descriptor. Counted so a persistent stream of them is visible.
				sh.slots.erase(d);
				g_unknown_copies.fetch_add(1, std::memory_order_relaxed);
				continue;
			}
			ViewEntry copy = src_entries[i];
			copy.seq = g_seq.fetch_add(1, std::memory_order_relaxed) + 1;
			copy.via_copy = true;
			copy.src_slot = src + static_cast<std::uint64_t>(i) * inc;
			sh.slots[d] = copy;
		}
	}
}

void note_copy(icept::DescriptorId dst, icept::DescriptorId src)
{
	note_copy_range(dst, src, 1, 0);
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

	std::lock_guard<std::mutex> lock(g_heaps_mutex);
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
	{
		Shard &sh = shard_of(cpu);
		std::shared_lock<std::shared_mutex> lock(sh.mutex);
		const auto it = sh.slots.find(cpu);
		if (it == sh.slots.end())
			return false;
		out = it->second;
	}
	// Derived tombstone: the resource this slot named is gone, or its address now belongs to a
	// newer registration. Outside the shadow's lock (the registry has its own).
	out.dead = out.resource != 0 && out.resource_gen != 0 && registry::generation_of(out.resource) != out.resource_gen;
	return true;
}

bool gpu_to_cpu(std::uint64_t gpu, icept::DescriptorId &cpu)
{
	if (gpu == 0)
		return false;
	std::lock_guard<std::mutex> lock(g_heaps_mutex);
	for (const HeapRecord &r : g_heaps)
		if (core::gpu_to_cpu(gpu, r.span, cpu))
			return true;
	return false;
}

void forget_resource(icept::ResourceId)
{
	// Nothing to do: lookup() derives the tombstone from the registry generation.
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
	Stats s;
	s.views = g_views.load(std::memory_order_relaxed);
	s.copies = g_copies.load(std::memory_order_relaxed);
	for (Shard &sh : g_shards)
	{
		std::shared_lock<std::shared_mutex> lock(sh.mutex);
		s.slots += sh.slots.size();
		s.slots_buckets += sh.slots.bucket_count();
	}
	{
		std::lock_guard<std::mutex> lock(g_heaps_mutex);
		s.heaps = g_heaps.size();
	}
	return s;
}

void clear_for_test()
{
	for (Shard &sh : g_shards)
	{
		std::unique_lock<std::shared_mutex> lock(sh.mutex);
		sh.slots.clear();
	}
	{
		std::lock_guard<std::mutex> lock(g_heaps_mutex);
		g_heaps.clear();
	}
	g_unknown_lookups.store(0);
	g_null_lookups.store(0);
	g_dead_lookups.store(0);
	g_unknown_copies.store(0);
	g_views.store(0);
	g_copies.store(0);
}

} // namespace stray_dlss::native::shadow
