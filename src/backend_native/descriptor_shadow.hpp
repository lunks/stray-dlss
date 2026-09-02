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
	// A TOMBSTONE: the resource this slot referenced has died since the slot was written
	// (registry sentinel). Kept rather than erased, so a later claim that this slot holds
	// that resource is provably a stale map — the address may since have been reused.
	bool dead = false;
};

void note_view(icept::DescriptorId cpu, const ViewEntry &entry);
// A null view: the slot now holds a known-null entry (see ViewEntry::is_null).
void note_null_view(icept::DescriptorId cpu);
// One descriptor of a CopyDescriptors(Simple). An unknown source clears the destination
// and is counted (see unknown_copies).
void note_copy(icept::DescriptorId dst, icept::DescriptorId src);
// SetDescriptorHeaps: records the heap's CPU/GPU spans. Cheap; re-read on every bind.
void note_heap_bound(::ID3D12DescriptorHeap *heap);

// True for any recorded slot, tombstones included (check ViewEntry::dead).
bool lookup(icept::DescriptorId cpu, ViewEntry &out);
// Via the bound heaps' spans. False when no bound heap contains the GPU handle.
bool gpu_to_cpu(std::uint64_t gpu, icept::DescriptorId &cpu);

// From registry::on_destroyed: every slot referencing the resource becomes a tombstone.
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

struct Stats
{
	std::uint64_t views = 0;    // note_view calls
	std::uint64_t copies = 0;   // descriptors copied
	std::uint64_t slots = 0;    // live entries
	std::uint64_t heaps = 0;    // known heap spans
};
Stats stats();
void clear_for_test();

} // namespace stray_dlss::native::shadow
