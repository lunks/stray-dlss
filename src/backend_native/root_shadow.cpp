#include "backend_native/root_shadow.hpp"

#include <cstring>
#include <mutex>
#include <shared_mutex>

namespace stray_dlss::native::root {
namespace {

// A command list is single-threaded by the D3D12 contract, so the ONLY thing that needs
// synchronising is the list -> state map itself: two RHI threads recording two DIFFERENT lists
// must not race the map's structure. The per-call GLOBAL std::mutex the first design took on
// every SetComputeRoot* / SetDescriptorHeaps / SetPipelineState was a convoy (facts §28); every
// RHI thread serialised on it hundreds of times a frame.
//
// Now a shared_mutex guards the map. A mutator takes it SHARED to find its list's state and then
// mutates that ListState in place with no further lock — safe because only that list's own thread
// touches that element, and std::unordered_map keeps references/pointers to existing elements
// valid across another thread's insert (only erase invalidates, C++ [unord.req]). A first-sight
// list (or a Reset that must create state) takes the lock EXCLUSIVE to insert; that is rare (UE4
// pools its lists), and being exclusive it cannot race a concurrent rehash against the shared
// readers. So the hot path is lock-free of contention: different lists take the shared lock
// concurrently.
std::shared_mutex g_mutex;
std::unordered_map<::ID3D12GraphicsCommandList *, ListState> g_lists;

// The state for `list`, creating it under an exclusive lock if absent. Returned by pointer: the
// element outlives the lock (stable across later inserts), and the caller is the list's only
// mutating thread.
ListState *state_ptr(::ID3D12GraphicsCommandList *list)
{
	{
		std::shared_lock<std::shared_mutex> lock(g_mutex);
		const auto it = g_lists.find(list);
		if (it != g_lists.end())
			return &it->second;
	}
	std::unique_lock<std::shared_mutex> lock(g_mutex);
	return &g_lists[list]; // find-or-insert; a racing creator resolves to the same element
}

} // namespace

void ListState::set_compute_root_signature(::ID3D12RootSignature *rs)
{
	// Setting the root signature that is ALREADY set changes nothing: vkd3d-proton's
	// d3d12_command_list_set_root_signature early-outs on the same object, and ReShade's tracker
	// keeps its root arguments unless the layout differs. Only a CHANGE invalidates them.
	if (rs == compute_root_signature)
		return;
	compute_root_signature = rs;
	compute_tables.clear();
	compute_root_cbv.clear();
	compute_root_srv.clear();
	compute_root_uav.clear();
	compute_constants.clear();
}

bool snapshot(::ID3D12GraphicsCommandList *list, ListState &out)
{
	std::shared_lock<std::shared_mutex> lock(g_mutex);
	const auto it = g_lists.find(list);
	if (it == g_lists.end())
		return false;
	out = it->second;
	return true;
}

void on_reset(::ID3D12GraphicsCommandList *list, ::ID3D12PipelineState *initial_pso)
{
	ListState *s = state_ptr(list);
	*s = ListState{};
	s->pso = initial_pso;
}

void on_set_heaps(::ID3D12GraphicsCommandList *list, unsigned count, ::ID3D12DescriptorHeap *const *heaps)
{
	ListState *s = state_ptr(list);
	s->heap_count = 0;
	for (unsigned i = 0; i < count && i < 2; ++i)
		if (heaps != nullptr && heaps[i] != nullptr)
			s->heaps[s->heap_count++] = heaps[i];
}

void on_set_pso(::ID3D12GraphicsCommandList *list, ::ID3D12PipelineState *pso)
{
	state_ptr(list)->pso = pso;
}

void on_set_compute_root_signature(::ID3D12GraphicsCommandList *list, ::ID3D12RootSignature *rs)
{
	state_ptr(list)->set_compute_root_signature(rs);
}

void on_set_compute_table(::ID3D12GraphicsCommandList *list, std::uint32_t param, std::uint64_t gpu_handle)
{
	ListState *s = state_ptr(list);
	if (s->compute_tables.size() <= param)
		s->compute_tables.resize(param + 1, 0);
	s->compute_tables[param] = gpu_handle;
}

void on_set_compute_root_cbv(::ID3D12GraphicsCommandList *list, std::uint32_t param, std::uint64_t va)
{
	state_ptr(list)->compute_root_cbv[param] = va;
}

void on_set_compute_root_srv(::ID3D12GraphicsCommandList *list, std::uint32_t param, std::uint64_t va)
{
	state_ptr(list)->compute_root_srv[param] = va;
}

void on_set_compute_root_uav(::ID3D12GraphicsCommandList *list, std::uint32_t param, std::uint64_t va)
{
	state_ptr(list)->compute_root_uav[param] = va;
}

void on_set_compute_constants(::ID3D12GraphicsCommandList *list, std::uint32_t param, std::uint32_t first,
                              std::uint32_t count, const std::uint32_t *values)
{
	if (values == nullptr || count == 0)
		return;
	std::vector<std::uint32_t> &dst = state_ptr(list)->compute_constants[param];
	if (dst.size() < first + count)
		dst.resize(first + count, 0);
	std::memcpy(dst.data() + first, values, static_cast<size_t>(count) * 4);
}

void forget(::ID3D12GraphicsCommandList *list)
{
	std::unique_lock<std::shared_mutex> lock(g_mutex);
	g_lists.erase(list);
}

std::size_t list_count()
{
	std::shared_lock<std::shared_mutex> lock(g_mutex);
	return g_lists.size();
}

void clear_for_test()
{
	std::unique_lock<std::shared_mutex> lock(g_mutex);
	g_lists.clear();
}

} // namespace stray_dlss::native::root
