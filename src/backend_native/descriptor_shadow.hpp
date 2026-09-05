// What every descriptor slot holds, keyed by the REAL D3D12_CPU_DESCRIPTOR_HANDLE.ptr.
//
// Fed by the Create*View and CopyDescriptors(Simple) hooks. Keying by the raw handle rather
// than by (heap, index) means an OFFLINE heap never has to be identified at all: a copy from
// it is a lookup at src.ptr and a store at dst.ptr (assessment §1.1 decision 1). Only the
// ONLINE heaps need their spans, and those arrive at SetDescriptorHeaps — which is how the
// GPU handle in SetComputeRootDescriptorTable turns back into a CPU handle.
#pragma once

#include "core/heap_math.hpp"
#include "intercept/types.hpp"

#include <cstdint>

struct ID3D12DescriptorHeap;

namespace stray_dlss::native::shadow {

// TWO implementations behind this one interface (facts §28), chosen once at init:
//   fast  - per-heap flat arrays of lock-free atomics; the default, the perf path.
//   debug - the sharded hash map that also carries provenance (seq, via_copy, src_slot) for the
//           diff observer's adjudication; the WARP provenance assertions run against it.
// d3d12_hooks.cpp calls the same shadow:: functions regardless; the mode dispatches inside.
enum class Mode : std::uint8_t { fast, debug };
void set_mode(Mode m);   // idempotent; call once before any note_*; logs the choice
Mode mode();
const char *mode_name();

enum class ViewKind : std::uint8_t { srv, uav, cbv, rtv, dsv };

struct ViewEntry
{
	ViewKind kind = ViewKind::srv;
	icept::ResourceId resource = 0;
	TexFormat format = TexFormat::unknown; // the VIEW's format (or the resource's when the view is default)
	std::uint32_t dxgi_format = 0;
	std::uint32_t width = 0;
	std::uint32_t height = 0;
	bool is_3d = false;
	bool is_buffer = false;
	std::uint64_t buffer_offset = 0; // cbv: from the buffer's start
	std::uint64_t buffer_size = 0;   // cbv: SizeInBytes
	// A NULL view (Create*View with a null resource, or a CBV with no BufferLocation): the
	// slot is KNOWN to hold nothing. Distinct from an unknown slot — UE4 fills every unbound
	// register of a table from its null descriptors, so a lookup here is not a defect and
	// must not count as one (run F: 3.6M "unknown" copies were exactly this).
	bool is_null = false;
	// Provenance, for adjudicating a disagreement: the global write sequence at which this
	// slot was last written, and whether that write was a copy rather than a view creation.
	std::uint64_t seq = 0;
	bool via_copy = false;
	icept::DescriptorId src_slot = 0; // via_copy: the slot it was copied from
	// The registry generation the resource had when this slot was written (copies carry the
	// source's). `dead` is DERIVED at lookup: the registry's current generation for the address
	// differs (the resource died, or the address was reused by a newer resource). The slot is
	// kept, so a later claim that it holds that resource is provably a stale map. This
	// replaced a per-resource reverse index that grew without bound (facts §27).
	std::uint64_t resource_gen = 0;
	bool dead = false;
};

// [STRAYDLSS] ShadowGraphicsHeaps, default 0 = DO NOT record RTV and DSV descriptors.
//
// The shadow accepts three heap types, and only CBV_SRV_UAV has a reader. The RTV and DSV
// halves had two: the dataflow pass finder, which described a draw's bound render targets and
// was DELETED on 2026-09-03 (docs/RESEARCH-RESHADE-SHAPE-SWEEP.md §13.6.1), and the differential
// observer, which resolves COMPUTE tables only. So on every path, shipping or diagnostic, an
// RTV or DSV entry is written and never looked up.
//
// Recording them was therefore a per-view write, a slot array per RTV/DSV heap and a
// copy-tracking branch paid for nobody. The saving is SMALL and honestly [derived]: RTV/DSV view
// creation is a subset of the measured `shadow-write 0.050ms (44 views)` bucket, and their
// `CopyDescriptors` traffic is expected to be near zero because those heaps are never
// shader-visible, so UE4 does not stream them the way it streams the online heap. One launch
// reads it off the `[perf] native hooks/frame` line: the `views` count drops by the RTV/DSV
// share and nothing else moves.
//
// The knob stays because one FUTURE reader is on record: locating Stray's custom-depth target
// by its DSV (the DLSSNR ControlMask idea, CLAUDE.md §5 "the cat is unchanged under NR") would
// want DSVs shadowed again. Turning it ON restores the previous behaviour exactly.
void set_shadow_graphics_heaps(bool on);
bool shadow_graphics_heaps();

// THE COPY HALF'S RECORDING SWITCH ([STRAYDLSS] U0HookSkipWalk, src/core/u0_authority.hpp).
// Default ON. When OFF, CopyDescriptors(Simple) is forwarded and counted but note_copy_range is
// not called, so the ONLINE heap's slots stop being shadowed - which is the measured
// `shadow-copy 1.644ms` (CLAUDE.md §5). The WRITE half (note_view from Create*View) is
// untouched: it records the OFFLINE handle the RHI objects hold, which is what the bind-stream
// route cross-matches against, so it is level 3's dependency rather than its saving.
//
// ONE-WAY IN PRACTICE. The only caller turns it off and never back on within a session: once
// recording has stopped, a slot the game rewrote meanwhile holds its OLD entry, and a table
// bound after a resume would resolve to a live, wrong resource with no error (the §5 stale-map
// class). The setter is symmetric only so the WARP lane can exercise both states.
void set_copy_recording(bool on);
bool copy_recording();

void note_view(icept::DescriptorId cpu, const ViewEntry &entry);
// A null view: the slot now holds a known-null entry (see ViewEntry::is_null).
void note_null_view(icept::DescriptorId cpu);
// One descriptor of a CopyDescriptors(Simple). An unknown source clears the destination
// and is counted (see unknown_copies).
void note_copy(icept::DescriptorId dst, icept::DescriptorId src);
// A whole CopyDescriptors range: n consecutive descriptors `inc` bytes apart, sources read
// before any destination is written (by-value semantics). One shard lock per run, not one per
// descriptor - the fix for the lock convoy measured in facts §27.
void note_copy_range(icept::DescriptorId dst, icept::DescriptorId src, std::uint32_t n, std::uint32_t inc);
// SetDescriptorHeaps: records the heap's CPU/GPU spans (debug path; fast ignores it).
void note_heap_bound(::ID3D12DescriptorHeap *heap);
// CreateDescriptorHeap: the fast path allocates this heap's flat slot array here, so a CPU
// handle -> (heap, index) is arithmetic over an append-only heap list. Debug ignores it.
void note_heap_created(::ID3D12DescriptorHeap *heap);

// True for any recorded slot, tombstones included (check ViewEntry::dead).
bool lookup(icept::DescriptorId cpu, ViewEntry &out);
// Via the bound heaps' spans. False when no bound heap contains the GPU handle.
bool gpu_to_cpu(std::uint64_t gpu, icept::DescriptorId &cpu);

// From registry::on_destroyed. A no-op since the generation scheme: tombstones are derived at
// lookup from the registry's generation, so nothing needs walking when a resource dies.
void forget_resource(icept::ResourceId res);

// The §6.2 counter: lookups (by CPU or GPU handle) that found nothing. Incremented by the
// resolver on every unresolved slot; readable for the DIFF and the status file.
std::uint64_t unknown_lookups();
void count_unknown_lookup();
std::uint64_t unknown_copies();

// A lookup that found a known-null slot. Not a defect; counted so the ratio is visible.
std::uint64_t null_lookups();
void count_null_lookup();
// A lookup that found a tombstone (the slot's resource died after the write). Not a defect.
std::uint64_t dead_lookups();
void count_dead_lookup();
// The current write sequence (every note_view / note_null_view / note_copy advances it).
std::uint64_t write_sequence();
// FAST-path orphan diagnostics (facts §29): writes/copies whose handle fell in no registered
// heap, by call site, plus how many heaps were first seen at CREATE vs at BIND. Zero in Config A.
void fast_orphan_counts(std::uint64_t &view, std::uint64_t &copy_src, std::uint64_t &copy_dst,
                        std::uint64_t &heaps_created, std::uint64_t &heaps_bound);
// Live entries in the fast path's overflow map (orphan handles). Must reach 0 once every heap
// is registered through the proper (proxy-device) path.
std::uint64_t fast_overflow_size();

struct Stats
{
	std::uint64_t views = 0;    // note_view calls
	std::uint64_t copies = 0;   // descriptors copied
	std::uint64_t slots = 0;    // live entries
	std::uint64_t heaps = 0;    // known heap spans
	std::uint64_t slots_buckets = 0;       // g_slots.bucket_count(): changes only on rehash
};
Stats stats();
void clear_for_test();

} // namespace stray_dlss::native::shadow
