#include "backend_native/descriptor_shadow.hpp"

#include "backend_native/resource_registry.hpp"
#include "core/dxgi_format.hpp"
#include "log.hpp"

#include <d3d12.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

namespace stray_dlss::native::shadow {
namespace {

// The selected implementation, set once at init (default fast). Read relaxed on the hot path.
std::atomic<std::uint8_t> g_mode{ static_cast<std::uint8_t>(Mode::fast) };
inline Mode current_mode() { return static_cast<Mode>(g_mode.load(std::memory_order_relaxed)); }

// Counters shared by both implementations (the resolver drives them; the status file reads them).
std::atomic<std::uint64_t> g_unknown_lookups{ 0 };
std::atomic<std::uint64_t> g_null_lookups{ 0 };
std::atomic<std::uint64_t> g_dead_lookups{ 0 };
std::atomic<std::uint64_t> g_unknown_copies{ 0 };
std::atomic<std::uint64_t> g_seq{ 0 };   // debug-only write sequence (provenance)
std::atomic<std::uint64_t> g_views{ 0 };
std::atomic<std::uint64_t> g_copies{ 0 };


// ======================= FAST PATH: per-heap flat lock-free arrays =======================
// A descriptor heap is a contiguous array; a CPU handle is cpu_base + index*increment. So the
// shadow is one std::atomic word triple per slot, allocated once at CreateDescriptorHeap, and a
// write/copy is a handful of relaxed atomic stores with NO hash, NO lock, NO allocation - the
// design ReShade uses and the fix for the copy cost that sharding only reduced (facts §28).
namespace fast {

// Per slot, three atomic words. Publication order: w0 (resource) and w2 (cbv) are written first
// (relaxed), then w1 last with release, and a reader loads w1 first with acquire - so a slot seen
// "written" has its w0/w2 already visible. A slot in flight (being rewritten by its owner while
// read) is undefined to use per D3D12 anyway.
struct Slot
{
	std::atomic<std::uint64_t> w0{ 0 }; // resource id (0 = none/null)
	std::atomic<std::uint64_t> w1{ 0 }; // bit0 written, bit1 is_null, bits8-11 kind, bits16-31 dxgi_format, bits32-63 generation
	std::atomic<std::uint64_t> w2{ 0 }; // cbv: offset (low 32) | size (high 32)
};
constexpr std::uint64_t kWritten = 1ull;
constexpr std::uint64_t kNull = 2ull;
inline std::uint64_t pack_w1(ViewKind kind, bool is_null, std::uint32_t fmt, std::uint64_t gen)
{
	return kWritten | (is_null ? kNull : 0) | (static_cast<std::uint64_t>(kind) << 8) |
	       (static_cast<std::uint64_t>(fmt & 0xFFFF) << 16) | ((gen & 0xFFFFFFFFull) << 32);
}

struct Heap
{
	std::uint64_t cpu_base = 0;
	std::uint64_t cpu_end = 0; // cpu_base + count*inc
	std::uint64_t gpu_base = 0; // 0 if not shader-visible
	std::uint32_t count = 0;
	std::uint32_t inc = 0;
	std::unique_ptr<Slot[]> slots;
};

// Ownership: heaps live forever here (UE4 makes a handful). The published table is an immutable
// sorted-by-cpu_base snapshot, swapped under g_reg and read lock-free via an atomic pointer; the
// old snapshot is leaked (a few pointers).
std::mutex g_reg;
std::vector<std::unique_ptr<Heap>> g_owned;              // guarded by g_reg
std::vector<Heap *> *g_owned_leaked_snapshots_unused = nullptr; // (documentation)
std::atomic<const std::vector<Heap *> *> g_table{ nullptr };
std::atomic<std::uint64_t> g_slot_writes{ 0 };
// Orphan handles: a write/copy whose CPU handle falls in NO registered heap - a heap created
// before our device vtable patches went in, or (under ReShade) through the proxy device, or an
// offline staging heap never bound. Counted by call site so the gap is named (facts §29).
std::atomic<std::uint64_t> g_orphan_view{ 0 };
std::atomic<std::uint64_t> g_orphan_copy_src{ 0 };
std::atomic<std::uint64_t> g_orphan_copy_dst{ 0 };
std::atomic<std::uint64_t> g_heaps_via_create{ 0 };
std::atomic<std::uint64_t> g_heaps_via_bind{ 0 };

// The OVERFLOW map: handles whose heap the flat table never registered (created before our
// device patches, through ReShade's proxy device, or an offline staging heap never bound).
// Sharded like the old shadow but ONLY EVER TOUCHED BY ORPHANS - Config A never reaches it, so
// its lock never contends there; under Config B it keeps the shadow CORRECT until the proxy
// path registers the real heaps (facts §29). Its running size and the orphan hit counters are
// in the status line; both must fall to 0 once every heap is registered.
struct Ov { std::uint64_t w0 = 0, w1 = 0, w2 = 0; };
constexpr unsigned kOvShards = 64;
struct OvShard { std::shared_mutex m; std::unordered_map<icept::DescriptorId, Ov> map; };
OvShard g_ov[kOvShards];
inline OvShard &ov_of(icept::DescriptorId c) { return g_ov[(c >> 6) & (kOvShards - 1)]; }

Slot *slot_for(std::uint64_t cpu)
{
	const std::vector<Heap *> *t = g_table.load(std::memory_order_acquire);
	if (t == nullptr || cpu == 0)
		return nullptr;
	// Binary search: last heap whose cpu_base <= cpu.
	std::size_t lo = 0, hi = t->size();
	while (lo < hi)
	{
		const std::size_t mid = (lo + hi) / 2;
		if ((*t)[mid]->cpu_base <= cpu)
			lo = mid + 1;
		else
			hi = mid;
	}
	if (lo == 0)
		return nullptr;
	Heap *h = (*t)[lo - 1];
	if (cpu < h->cpu_base || cpu >= h->cpu_end || h->inc == 0)
		return nullptr;
	const std::uint64_t idx = (cpu - h->cpu_base) / h->inc;
	if (idx >= h->count)
		return nullptr;
	return &h->slots[static_cast<std::size_t>(idx)];
}

bool gpu_to_cpu(std::uint64_t gpu, icept::DescriptorId &cpu)
{
	const std::vector<Heap *> *t = g_table.load(std::memory_order_acquire);
	if (t == nullptr)
		return false;
	for (Heap *h : *t)
	{
		if (h->gpu_base == 0)
			continue;
		const std::uint64_t gpu_end = h->gpu_base + static_cast<std::uint64_t>(h->count) * h->inc;
		if (gpu >= h->gpu_base && gpu < gpu_end)
		{
			cpu = h->cpu_base + (gpu - h->gpu_base); // same index*inc offset on both sides
			return true;
		}
	}
	return false;
}

void note_heap_created(::ID3D12DescriptorHeap *heap, bool via_bind)
{
	if (heap == nullptr)
		return;
	const D3D12_DESCRIPTOR_HEAP_DESC desc = heap->GetDesc();
	if (desc.Type != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV &&
	    desc.Type != D3D12_DESCRIPTOR_HEAP_TYPE_RTV && desc.Type != D3D12_DESCRIPTOR_HEAP_TYPE_DSV)
		return; // samplers are never resolved
	const std::uint64_t cpu_base = heap->GetCPUDescriptorHandleForHeapStart().ptr;
	std::uint64_t gpu_base = 0;
	if ((desc.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) != 0)
		gpu_base = heap->GetGPUDescriptorHandleForHeapStart().ptr;
	std::uint32_t inc = 0;
	{
		ID3D12Device *device = nullptr;
		if (SUCCEEDED(heap->GetDevice(IID_PPV_ARGS(&device))) && device != nullptr)
		{
			inc = device->GetDescriptorHandleIncrementSize(desc.Type);
			device->Release();
		}
	}
	if (inc == 0 || desc.NumDescriptors == 0)
		return;

	std::lock_guard<std::mutex> lock(g_reg);
	for (const auto &h : g_owned)
		if (h->cpu_base == cpu_base)
			return; // already known (idempotent)
	auto h = std::make_unique<Heap>();
	h->cpu_base = cpu_base;
	h->cpu_end = cpu_base + static_cast<std::uint64_t>(desc.NumDescriptors) * inc;
	h->gpu_base = gpu_base;
	h->count = desc.NumDescriptors;
	h->inc = inc;
	h->slots = std::make_unique<Slot[]>(desc.NumDescriptors);
	g_owned.push_back(std::move(h));
	auto *snapshot = new std::vector<Heap *>();
	for (const auto &e : g_owned)
		snapshot->push_back(e.get());
	std::sort(snapshot->begin(), snapshot->end(), [](Heap *a, Heap *b) { return a->cpu_base < b->cpu_base; });
	g_table.store(snapshot, std::memory_order_release); // old snapshot leaked on purpose
	if (via_bind) g_heaps_via_bind.fetch_add(1, std::memory_order_relaxed);
	else g_heaps_via_create.fetch_add(1, std::memory_order_relaxed);
	const char *tn = desc.Type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV ? "CBV_SRV_UAV" : desc.Type == D3D12_DESCRIPTOR_HEAP_TYPE_RTV ? "RTV" : "DSV";
	STRAY_LOG_INFO("descriptor_shadow(fast): heap %p registered via %s: %s %u descriptors, inc %u, cpu %llx gpu %llx (%s), %zu heaps",
		static_cast<void *>(heap), via_bind ? "BIND" : "CREATE", tn, desc.NumDescriptors, inc, static_cast<unsigned long long>(cpu_base),
		static_cast<unsigned long long>(gpu_base), gpu_base != 0 ? "shader-visible/ONLINE" : "cpu-only/OFFLINE", g_owned.size());
}

// Write a packed slot: flat and lock-free when the heap is registered, else into the overflow
// map under a shard lock. site 0 = a view creation, 1 = a copy destination (for the counters).
void write_slot(icept::DescriptorId cpu, std::uint64_t w0, std::uint64_t w1, std::uint64_t w2, int site)
{
	if (Slot *s = slot_for(cpu))
	{
		s->w0.store(w0, std::memory_order_relaxed);
		s->w2.store(w2, std::memory_order_relaxed);
		s->w1.store(w1, std::memory_order_release);
	}
	else
	{
		OvShard &o = ov_of(cpu);
		std::unique_lock<std::shared_mutex> lk(o.m);
		o.map[cpu] = Ov{ w0, w1, w2 };
		if (site == 0) g_orphan_view.fetch_add(1, std::memory_order_relaxed);
		else g_orphan_copy_dst.fetch_add(1, std::memory_order_relaxed);
	}
}
// A copy from an unknown source makes the destination unknown again.
void write_unwritten(icept::DescriptorId cpu)
{
	if (Slot *s = slot_for(cpu))
		s->w1.store(0, std::memory_order_release);
	else
	{
		OvShard &o = ov_of(cpu);
		std::unique_lock<std::shared_mutex> lk(o.m);
		o.map.erase(cpu);
	}
}
// Read a packed slot; false if the slot is unknown. *orphan (optional) says it was in overflow.
bool read_slot(icept::DescriptorId cpu, std::uint64_t &w0, std::uint64_t &w1, std::uint64_t &w2, bool *orphan = nullptr)
{
	Slot *s = slot_for(cpu);
	if (orphan) *orphan = (s == nullptr);
	if (s != nullptr)
	{
		w1 = s->w1.load(std::memory_order_acquire);
		if ((w1 & kWritten) == 0) return false;
		w0 = s->w0.load(std::memory_order_relaxed);
		w2 = s->w2.load(std::memory_order_relaxed);
		return true;
	}
	OvShard &o = ov_of(cpu);
	std::shared_lock<std::shared_mutex> lk(o.m);
	const auto it = o.map.find(cpu);
	if (it == o.map.end()) return false;
	w0 = it->second.w0; w1 = it->second.w1; w2 = it->second.w2;
	return (w1 & kWritten) != 0;
}

void note_view(icept::DescriptorId cpu, const ViewEntry &entry)
{
	std::uint64_t gen = entry.resource_gen;
	if (gen == 0 && entry.resource != 0)
		gen = registry::generation_of(entry.resource);
	const std::uint64_t w2 = (entry.buffer_offset & 0xFFFFFFFFull) | ((entry.buffer_size & 0xFFFFFFFFull) << 32);
	write_slot(cpu, entry.resource, pack_w1(entry.kind, false, entry.dxgi_format, gen), w2, 0);
	g_slot_writes.fetch_add(1, std::memory_order_relaxed);
}

void note_null_view(icept::DescriptorId cpu)
{
	write_slot(cpu, 0, kWritten | kNull, 0, 0);
	g_slot_writes.fetch_add(1, std::memory_order_relaxed);
}

void note_copy_range(icept::DescriptorId dst, icept::DescriptorId src, std::uint32_t n, std::uint32_t inc,
                     std::atomic<std::uint64_t> &unknown_copies)
{
	for (std::uint32_t i = 0; i < n; ++i)
	{
		const icept::DescriptorId s = src + static_cast<std::uint64_t>(i) * inc;
		const icept::DescriptorId d = dst + static_cast<std::uint64_t>(i) * inc;
		Slot *ds = slot_for(d);
		Slot *ss = slot_for(s);
		if (ds != nullptr && ss != nullptr)
		{
			// The common case, Config A and the online heap once it is registered: both slots are
			// flat. Pure lock-free atomics, no counter contention (g_slot_writes is bumped once
			// per range below, not per descriptor - that per-descriptor atomic across every RHI
			// thread cost Config A ~15 fps, facts §30).
			const std::uint64_t sw1 = ss->w1.load(std::memory_order_acquire);
			if ((sw1 & kWritten) == 0)
			{
				ds->w1.store(0, std::memory_order_release);
				unknown_copies.fetch_add(1, std::memory_order_relaxed);
				continue;
			}
			ds->w0.store(ss->w0.load(std::memory_order_relaxed), std::memory_order_relaxed);
			ds->w2.store(ss->w2.load(std::memory_order_relaxed), std::memory_order_relaxed);
			ds->w1.store(sw1, std::memory_order_release);
			continue;
		}
		// Orphan path (Config B: the online heap in overflow until the proxy patch). Rare, so a
		// little redundancy here is fine; Config A never reaches it (orphans=0).
		if (ss == nullptr)
			g_orphan_copy_src.fetch_add(1, std::memory_order_relaxed);
		std::uint64_t w0 = 0, w1 = 0, w2 = 0;
		if (!read_slot(s, w0, w1, w2))
		{
			write_unwritten(d);
			unknown_copies.fetch_add(1, std::memory_order_relaxed);
			continue;
		}
		write_slot(d, w0, w1, w2, 1);
	}
	g_slot_writes.fetch_add(n, std::memory_order_relaxed);
}

bool lookup(icept::DescriptorId cpu, ViewEntry &out)
{
	std::uint64_t w0 = 0, w1 = 0, w2 = 0;
	if (!read_slot(cpu, w0, w1, w2))
		return false;
	out = ViewEntry{};
	out.kind = static_cast<ViewKind>((w1 >> 8) & 0xF);
	out.is_null = (w1 & kNull) != 0;
	out.dxgi_format = static_cast<std::uint32_t>((w1 >> 16) & 0xFFFF);
	out.resource_gen = (w1 >> 32) & 0xFFFFFFFFull;
	out.resource = static_cast<icept::ResourceId>(w0);
	if (!out.is_null && out.resource != 0)
	{
		icept::ResourceInfo info;
		if (registry::describe(out.resource, info))
		{
			out.is_buffer = info.is_buffer;
			out.width = info.width;
			out.height = info.height;
			out.is_3d = info.is_3d;
			const std::uint32_t f = out.dxgi_format != 0 ? out.dxgi_format : info.dxgi_format;
			out.dxgi_format = f;
			out.format = info.is_buffer ? TexFormat::unknown : tex_format_from_dxgi(f);
		}
		out.buffer_offset = w2 & 0xFFFFFFFFull;
		out.buffer_size = (w2 >> 32) & 0xFFFFFFFFull;
		out.dead = out.resource_gen != 0 && registry::generation_of(out.resource) != out.resource_gen;
	}
	return true;
}

std::uint64_t slots_written() { return g_slot_writes.load(std::memory_order_relaxed); }
std::size_t heap_count() { const auto *t = g_table.load(std::memory_order_acquire); return t ? t->size() : 0; }
std::uint64_t overflow_size()
{
	std::uint64_t n = 0;
	for (OvShard &o : g_ov) { std::shared_lock<std::shared_mutex> lk(o.m); n += o.map.size(); }
	return n;
}
void orphan_counts(std::uint64_t &view, std::uint64_t &csrc, std::uint64_t &cdst, std::uint64_t &via_c, std::uint64_t &via_b)
{ view=g_orphan_view.load(); csrc=g_orphan_copy_src.load(); cdst=g_orphan_copy_dst.load(); via_c=g_heaps_via_create.load(); via_b=g_heaps_via_bind.load(); }

void clear_for_test()
{
	std::lock_guard<std::mutex> lock(g_reg);
	g_owned.clear();
	g_table.store(nullptr, std::memory_order_release); // old snapshots leaked; test-only
	g_slot_writes.store(0);
	g_orphan_view.store(0); g_orphan_copy_src.store(0); g_orphan_copy_dst.store(0);
	g_heaps_via_create.store(0); g_heaps_via_bind.store(0);
	for (OvShard &o : g_ov) { std::unique_lock<std::shared_mutex> lk(o.m); o.map.clear(); }
}

} // namespace fast

// ======================= DEBUG PATH: sharded hash map with provenance =======================
// Carries seq / via_copy / src_slot for the diff observer's adjudication (facts §16); the WARP
// provenance assertions run against this path. Sharded so it is not itself a convoy in the rare
// debug session; the fast path above is what ships.
namespace debug {

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

void note_view(icept::DescriptorId cpu, const ViewEntry &entry)
{
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
	constexpr std::uint32_t kChunk = 32;
	ViewEntry scratch[kChunk];
	bool known[kChunk];
	std::uint32_t base = 0;
	while (base < n)
	{
		const std::uint32_t m = (n - base) < kChunk ? (n - base) : kChunk;
		std::uint32_t i = 0;
		while (i < m)
		{
			const icept::DescriptorId s0 = src + static_cast<std::uint64_t>(base + i) * inc;
			const unsigned si = shard_index(s0);
			Shard &sh = g_shards[si];
			std::shared_lock<std::shared_mutex> lock(sh.mutex);
			for (; i < m; ++i)
			{
				const icept::DescriptorId s = src + static_cast<std::uint64_t>(base + i) * inc;
				if (shard_index(s) != si)
					break;
				const auto it = sh.slots.find(s);
				known[i] = it != sh.slots.end();
				if (known[i])
					scratch[i] = it->second;
			}
		}
		i = 0;
		while (i < m)
		{
			const icept::DescriptorId d0 = dst + static_cast<std::uint64_t>(base + i) * inc;
			const unsigned di = shard_index(d0);
			Shard &sh = g_shards[di];
			std::unique_lock<std::shared_mutex> lock(sh.mutex);
			for (; i < m; ++i)
			{
				const icept::DescriptorId d = dst + static_cast<std::uint64_t>(base + i) * inc;
				if (shard_index(d) != di)
					break;
				if (!known[i])
				{
					sh.slots.erase(d);
					g_unknown_copies.fetch_add(1, std::memory_order_relaxed);
					continue;
				}
				ViewEntry copy = scratch[i];
				copy.seq = g_seq.fetch_add(1, std::memory_order_relaxed) + 1;
				copy.via_copy = true;
				copy.src_slot = src + static_cast<std::uint64_t>(base + i) * inc;
				sh.slots[d] = copy;
			}
		}
		base += m;
	}
}

void note_heap_bound(::ID3D12DescriptorHeap *heap)
{
	const D3D12_DESCRIPTOR_HEAP_DESC desc = heap->GetDesc();
	if (desc.Type != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)
		return;
	core::HeapSpan span;
	span.cpu_base = heap->GetCPUDescriptorHandleForHeapStart().ptr;
	span.gpu_base = (desc.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) != 0
		? heap->GetGPUDescriptorHandleForHeapStart().ptr : 0;
	span.count = desc.NumDescriptors;
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
		if (r.heap == heap) { r.span = span; return; }
	g_heaps.push_back(HeapRecord{ heap, span });
	STRAY_LOG_INFO("descriptor_shadow(debug): heap %p bound: %u descriptors, inc %u, cpu %llx gpu %llx (%s)",
		static_cast<void *>(heap), span.count, span.increment,
		static_cast<unsigned long long>(span.cpu_base), static_cast<unsigned long long>(span.gpu_base),
		span.gpu_base != 0 ? "shader-visible" : "CPU-only");
}

bool lookup(icept::DescriptorId cpu, ViewEntry &out)
{
	{
		Shard &sh = shard_of(cpu);
		std::shared_lock<std::shared_mutex> lock(sh.mutex);
		const auto it = sh.slots.find(cpu);
		if (it == sh.slots.end())
			return false;
		out = it->second;
	}
	out.dead = out.resource != 0 && out.resource_gen != 0 && registry::generation_of(out.resource) != out.resource_gen;
	return true;
}

bool gpu_to_cpu(std::uint64_t gpu, icept::DescriptorId &cpu)
{
	std::lock_guard<std::mutex> lock(g_heaps_mutex);
	for (const HeapRecord &r : g_heaps)
		if (core::gpu_to_cpu(gpu, r.span, cpu))
			return true;
	return false;
}

std::uint64_t slots()
{
	std::uint64_t n = 0;
	for (Shard &sh : g_shards)
	{
		std::shared_lock<std::shared_mutex> lock(sh.mutex);
		n += sh.slots.size();
	}
	return n;
}
std::size_t heap_count() { std::lock_guard<std::mutex> lock(g_heaps_mutex); return g_heaps.size(); }

void clear_for_test()
{
	for (Shard &sh : g_shards)
	{
		std::unique_lock<std::shared_mutex> lock(sh.mutex);
		sh.slots.clear();
	}
	std::lock_guard<std::mutex> lock(g_heaps_mutex);
	g_heaps.clear();
}

} // namespace debug

} // namespace

// -------- mode selection --------
void set_mode(Mode m)
{
	g_mode.store(static_cast<std::uint8_t>(m), std::memory_order_relaxed);
	STRAY_LOG_INFO("descriptor_shadow: mode = %s", mode_name());
}
Mode mode() { return current_mode(); }
const char *mode_name() { return current_mode() == Mode::fast ? "fast (flat lock-free arrays)" : "debug (sharded map + provenance)"; }

// -------- the public interface: dispatch on the mode --------
void note_view(icept::DescriptorId cpu, const ViewEntry &entry)
{
	if (cpu == 0)
		return;
	if (current_mode() == Mode::fast)
		fast::note_view(cpu, entry);
	else
		debug::note_view(cpu, entry);
}

void note_null_view(icept::DescriptorId cpu)
{
	if (cpu == 0)
		return;
	if (current_mode() == Mode::fast)
		fast::note_null_view(cpu);
	else
		debug::note_null_view(cpu);
}

void note_copy_range(icept::DescriptorId dst, icept::DescriptorId src, std::uint32_t n, std::uint32_t inc)
{
	if (dst == 0 || n == 0)
		return;
	g_copies.fetch_add(n, std::memory_order_relaxed);
	if (current_mode() == Mode::fast)
		fast::note_copy_range(dst, src, n, inc, g_unknown_copies);
	else
		debug::note_copy_range(dst, src, n, inc);
}

void note_copy(icept::DescriptorId dst, icept::DescriptorId src) { note_copy_range(dst, src, 1, 0); }

void note_heap_bound(::ID3D12DescriptorHeap *heap)
{
	if (heap == nullptr)
		return;
	if (current_mode() == Mode::debug)
		debug::note_heap_bound(heap);
	else
		fast::note_heap_created(heap, /*via_bind=*/true);
}

void note_heap_created(::ID3D12DescriptorHeap *heap)
{
	if (heap != nullptr && current_mode() == Mode::fast)
		fast::note_heap_created(heap, /*via_bind=*/false);
}

bool lookup(icept::DescriptorId cpu, ViewEntry &out)
{
	if (cpu == 0)
		return false;
	return current_mode() == Mode::fast ? fast::lookup(cpu, out) : debug::lookup(cpu, out);
}

bool gpu_to_cpu(std::uint64_t gpu, icept::DescriptorId &cpu)
{
	if (gpu == 0)
		return false;
	return current_mode() == Mode::fast ? fast::gpu_to_cpu(gpu, cpu) : debug::gpu_to_cpu(gpu, cpu);
}

void forget_resource(icept::ResourceId)
{
	// Nothing to do in either mode: lookup() derives the tombstone from the registry generation.
}

std::uint64_t unknown_lookups() { return g_unknown_lookups.load(std::memory_order_relaxed); }
void count_unknown_lookup() { g_unknown_lookups.fetch_add(1, std::memory_order_relaxed); }
std::uint64_t unknown_copies() { return g_unknown_copies.load(std::memory_order_relaxed); }
std::uint64_t null_lookups() { return g_null_lookups.load(std::memory_order_relaxed); }
void count_null_lookup() { g_null_lookups.fetch_add(1, std::memory_order_relaxed); }
std::uint64_t dead_lookups() { return g_dead_lookups.load(std::memory_order_relaxed); }
void count_dead_lookup() { g_dead_lookups.fetch_add(1, std::memory_order_relaxed); }
std::uint64_t write_sequence() { return g_seq.load(std::memory_order_relaxed); }
void fast_orphan_counts(std::uint64_t &view, std::uint64_t &copy_src, std::uint64_t &copy_dst,
                        std::uint64_t &heaps_created, std::uint64_t &heaps_bound)
{ fast::orphan_counts(view, copy_src, copy_dst, heaps_created, heaps_bound); }
std::uint64_t fast_overflow_size() { return fast::overflow_size(); }

Stats stats()
{
	Stats s;
	s.views = g_views.load(std::memory_order_relaxed);
	s.copies = g_copies.load(std::memory_order_relaxed);
	if (current_mode() == Mode::fast)
	{
		s.slots = fast::slots_written();
		s.heaps = fast::heap_count();
	}
	else
	{
		s.slots = debug::slots();
		s.heaps = debug::heap_count();
	}
	return s;
}

void clear_for_test()
{
	debug::clear_for_test();
	fast::clear_for_test();
	g_unknown_lookups.store(0);
	g_null_lookups.store(0);
	g_dead_lookups.store(0);
	g_unknown_copies.store(0);
	g_seq.store(0);
	g_views.store(0);
	g_copies.store(0);
}

} // namespace stray_dlss::native::shadow
