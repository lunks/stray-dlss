// GPU virtual address -> buffer, for turning a root CBV's D3D12_GPU_VIRTUAL_ADDRESS back into
// (resource, offset). The same shape as ReShade's `_buffer_gpu_addresses` registry
// (d3d12_impl_device.cpp:2107-2124, :2211 — an ordered map with upper_bound lookup). Pure.
#pragma once

#include <cstdint>
#include <map>

namespace stray_dlss::core {

class VaMap
{
public:
	// [start, start+size). A second insert at the same start REPLACES the first
	// (insert_or_assign): a placed resource re-created over the same heap range is the
	// newer one, as ReShade treats it.
	void insert(std::uint64_t start, std::uint64_t size, std::uint64_t id);
	// Removes every range carrying `id`.
	void erase(std::uint64_t id);
	// The range containing `va`; end is exclusive. `offset` is va - start. Ranges may NEST
	// (a placed buffer inside another buffer's span on one heap): the INNERMOST — the one with
	// the greatest start — wins, and an outer range is still found past the inner one's end.
	// ReShade's registry stops at the first range starting at or before `va`, which misses the
	// outer buffer whenever an inner one starts between them (measured: UE4's upload ring is
	// addressed at interior offsets in the MB range by root CBVs, run F).
	bool find(std::uint64_t va, std::uint64_t &id, std::uint64_t &offset) const;
	std::size_t size() const { return m_ranges.size(); }

private:
	struct Range { std::uint64_t size; std::uint64_t id; };
	std::map<std::uint64_t, Range> m_ranges; // keyed by start
	std::uint64_t m_max_size = 0;            // bounds the backward walk in find()
};

} // namespace stray_dlss::core
