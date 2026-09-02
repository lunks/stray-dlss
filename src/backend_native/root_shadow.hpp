// Per-command-list compute root state, as the game set it — the argument list
// SetComputeRoot* actually received, in order, under the root signature it was set for
// (assessment §3.3). Replaces ReShade's state_tracking for the compute path, and unlike it
// carries root CBVs and 32-bit constants (frame_state.hpp's whole reason for existing).
#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

struct ID3D12GraphicsCommandList;
struct ID3D12RootSignature;
struct ID3D12PipelineState;
struct ID3D12DescriptorHeap;

namespace stray_dlss::native::root {

struct ListState
{
	::ID3D12RootSignature *compute_root_signature = nullptr;
	// Indexed by root parameter; 0 = not a table / never set under this signature.
	std::vector<std::uint64_t> compute_tables;
	std::unordered_map<std::uint32_t, std::uint64_t> compute_root_cbv; // param -> GPU VA
	std::unordered_map<std::uint32_t, std::uint64_t> compute_root_srv;
	std::unordered_map<std::uint32_t, std::uint64_t> compute_root_uav;
	std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> compute_constants;
	::ID3D12PipelineState *pso = nullptr;
	::ID3D12DescriptorHeap *heaps[2] = {};
	unsigned heap_count = 0;

	// D3D12: changing the root signature invalidates every root argument set under the old one.
	void set_compute_root_signature(::ID3D12RootSignature *rs);
};

// A COPY of the list's state, or false if the list was never seen.
bool snapshot(::ID3D12GraphicsCommandList *list, ListState &out);
// Mutators, one per hook. Each takes the lock, so a hook is one call.
void on_reset(::ID3D12GraphicsCommandList *list, ::ID3D12PipelineState *initial_pso);
void on_set_heaps(::ID3D12GraphicsCommandList *list, unsigned count, ::ID3D12DescriptorHeap *const *heaps);
void on_set_pso(::ID3D12GraphicsCommandList *list, ::ID3D12PipelineState *pso);
void on_set_compute_root_signature(::ID3D12GraphicsCommandList *list, ::ID3D12RootSignature *rs);
void on_set_compute_table(::ID3D12GraphicsCommandList *list, std::uint32_t param, std::uint64_t gpu_handle);
void on_set_compute_root_cbv(::ID3D12GraphicsCommandList *list, std::uint32_t param, std::uint64_t va);
void on_set_compute_root_srv(::ID3D12GraphicsCommandList *list, std::uint32_t param, std::uint64_t va);
void on_set_compute_root_uav(::ID3D12GraphicsCommandList *list, std::uint32_t param, std::uint64_t va);
void on_set_compute_constants(::ID3D12GraphicsCommandList *list, std::uint32_t param, std::uint32_t first,
                              std::uint32_t count, const std::uint32_t *values);
void forget(::ID3D12GraphicsCommandList *list);
std::size_t list_count();
void clear_for_test();

} // namespace stray_dlss::native::root
