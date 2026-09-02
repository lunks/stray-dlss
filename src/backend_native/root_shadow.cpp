#include "backend_native/root_shadow.hpp"

#include <cstring>
#include <mutex>

namespace stray_dlss::native::root {
namespace {

std::mutex g_mutex;
std::unordered_map<::ID3D12GraphicsCommandList *, ListState> g_lists;

ListState &state_locked(::ID3D12GraphicsCommandList *list)
{
	return g_lists[list];
}

} // namespace

void ListState::set_compute_root_signature(::ID3D12RootSignature *rs)
{
	compute_root_signature = rs;
	compute_tables.clear();
	compute_root_cbv.clear();
	compute_root_srv.clear();
	compute_root_uav.clear();
	compute_constants.clear();
}

bool snapshot(::ID3D12GraphicsCommandList *list, ListState &out)
{
	std::lock_guard<std::mutex> lock(g_mutex);
	const auto it = g_lists.find(list);
	if (it == g_lists.end())
		return false;
	out = it->second;
	return true;
}

void on_reset(::ID3D12GraphicsCommandList *list, ::ID3D12PipelineState *initial_pso)
{
	std::lock_guard<std::mutex> lock(g_mutex);
	ListState &s = state_locked(list);
	s = ListState{};
	s.pso = initial_pso;
}

void on_set_heaps(::ID3D12GraphicsCommandList *list, unsigned count, ::ID3D12DescriptorHeap *const *heaps)
{
	std::lock_guard<std::mutex> lock(g_mutex);
	ListState &s = state_locked(list);
	s.heap_count = 0;
	for (unsigned i = 0; i < count && i < 2; ++i)
		if (heaps != nullptr && heaps[i] != nullptr)
			s.heaps[s.heap_count++] = heaps[i];
}

void on_set_pso(::ID3D12GraphicsCommandList *list, ::ID3D12PipelineState *pso)
{
	std::lock_guard<std::mutex> lock(g_mutex);
	state_locked(list).pso = pso;
}

void on_set_compute_root_signature(::ID3D12GraphicsCommandList *list, ::ID3D12RootSignature *rs)
{
	std::lock_guard<std::mutex> lock(g_mutex);
	state_locked(list).set_compute_root_signature(rs);
}

void on_set_compute_table(::ID3D12GraphicsCommandList *list, std::uint32_t param, std::uint64_t gpu_handle)
{
	std::lock_guard<std::mutex> lock(g_mutex);
	ListState &s = state_locked(list);
	if (s.compute_tables.size() <= param)
		s.compute_tables.resize(param + 1, 0);
	s.compute_tables[param] = gpu_handle;
}

void on_set_compute_root_cbv(::ID3D12GraphicsCommandList *list, std::uint32_t param, std::uint64_t va)
{
	std::lock_guard<std::mutex> lock(g_mutex);
	state_locked(list).compute_root_cbv[param] = va;
}

void on_set_compute_root_srv(::ID3D12GraphicsCommandList *list, std::uint32_t param, std::uint64_t va)
{
	std::lock_guard<std::mutex> lock(g_mutex);
	state_locked(list).compute_root_srv[param] = va;
}

void on_set_compute_root_uav(::ID3D12GraphicsCommandList *list, std::uint32_t param, std::uint64_t va)
{
	std::lock_guard<std::mutex> lock(g_mutex);
	state_locked(list).compute_root_uav[param] = va;
}

void on_set_compute_constants(::ID3D12GraphicsCommandList *list, std::uint32_t param, std::uint32_t first,
                              std::uint32_t count, const std::uint32_t *values)
{
	if (values == nullptr || count == 0)
		return;
	std::lock_guard<std::mutex> lock(g_mutex);
	std::vector<std::uint32_t> &dst = state_locked(list).compute_constants[param];
	if (dst.size() < first + count)
		dst.resize(first + count, 0);
	std::memcpy(dst.data() + first, values, static_cast<size_t>(count) * 4);
}

void forget(::ID3D12GraphicsCommandList *list)
{
	std::lock_guard<std::mutex> lock(g_mutex);
	g_lists.erase(list);
}

std::size_t list_count()
{
	std::lock_guard<std::mutex> lock(g_mutex);
	return g_lists.size();
}

void clear_for_test()
{
	std::lock_guard<std::mutex> lock(g_mutex);
	g_lists.clear();
}

} // namespace stray_dlss::native::root
