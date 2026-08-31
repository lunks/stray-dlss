#include "d3d12_restore.hpp"

#include <d3d12.h>

namespace stray_dlss {

void restore_native_compute_state(ID3D12GraphicsCommandList *cmd, const NativeComputeState &state)
{
	if (cmd == nullptr)
		return;

	// The root signature first. Changing it invalidates every compute root argument, so
	// anything set before this point would be discarded.
	if (state.root_signature != nullptr)
		cmd->SetComputeRootSignature(state.root_signature);

	// Tables, skipping zero handles. A zero handle means the parameter is not a table at all,
	// and binding one there is undefined behaviour that vkd3d-proton will not report in a
	// release build.
	for (std::uint32_t param = 0; param < state.tables.size(); ++param)
	{
		if (state.tables[param] == 0)
			continue;
		D3D12_GPU_DESCRIPTOR_HANDLE h = {};
		h.ptr = state.tables[param];
		cmd->SetComputeRootDescriptorTable(param, h);
	}

	for (const auto &cbv : state.root_cbv)
		cmd->SetComputeRootConstantBufferView(cbv.first, cbv.second);
	for (const auto &srv : state.root_srv)
		cmd->SetComputeRootShaderResourceView(srv.first, srv.second);
	for (const auto &uav : state.root_uav)
		cmd->SetComputeRootUnorderedAccessView(uav.first, uav.second);

	for (const auto &rc : state.root_constants)
	{
		if (rc.second.empty())
			continue;
		cmd->SetComputeRoot32BitConstants(rc.first,
			static_cast<UINT>(rc.second.size()), rc.second.data(), 0);
	}

	if (state.pso != nullptr)
		cmd->SetPipelineState(state.pso);
}

} // namespace stray_dlss
