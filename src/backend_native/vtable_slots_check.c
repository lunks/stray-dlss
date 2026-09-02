/* Compile-time proof that vtable_slots.hpp's numbers are d3d12.h's. C, because the Vtbl
 * structs only exist in the C interface; mingw's older d3d12.h lacks the ID3D12Device2 one,
 * so that single check is conditional. Nothing here is linked into anything. */
#define WIN32_LEAN_AND_MEAN
#define CINTERFACE
#define COBJMACROS
#include <windows.h>
#include <d3d12.h>
#include <stddef.h>

#define SLOT_IS(vtbl, member, index) \
	_Static_assert(offsetof(vtbl, member) == (index) * sizeof(void *), #vtbl "::" #member " is not slot " #index)

SLOT_IS(ID3D12DeviceVtbl, CreateCommandQueue, 8);
SLOT_IS(ID3D12DeviceVtbl, CreateCommandAllocator, 9);
SLOT_IS(ID3D12DeviceVtbl, CreateGraphicsPipelineState, 10);
SLOT_IS(ID3D12DeviceVtbl, CreateComputePipelineState, 11);
SLOT_IS(ID3D12DeviceVtbl, CreateCommandList, 12);
SLOT_IS(ID3D12DeviceVtbl, CreateDescriptorHeap, 14);
SLOT_IS(ID3D12DeviceVtbl, GetDescriptorHandleIncrementSize, 15);
SLOT_IS(ID3D12DeviceVtbl, CreateRootSignature, 16);
SLOT_IS(ID3D12DeviceVtbl, CreateConstantBufferView, 17);
SLOT_IS(ID3D12DeviceVtbl, CreateShaderResourceView, 18);
SLOT_IS(ID3D12DeviceVtbl, CreateUnorderedAccessView, 19);
SLOT_IS(ID3D12DeviceVtbl, CreateRenderTargetView, 20);
SLOT_IS(ID3D12DeviceVtbl, CreateDepthStencilView, 21);
SLOT_IS(ID3D12DeviceVtbl, CopyDescriptors, 23);
SLOT_IS(ID3D12DeviceVtbl, CopyDescriptorsSimple, 24);
SLOT_IS(ID3D12DeviceVtbl, CreateCommittedResource, 27);
SLOT_IS(ID3D12DeviceVtbl, CreatePlacedResource, 29);
SLOT_IS(ID3D12DeviceVtbl, CreateReservedResource, 30);
#ifdef __ID3D12Device2_INTERFACE_DEFINED__
SLOT_IS(ID3D12Device2Vtbl, CreatePipelineState, 47);
#endif

SLOT_IS(ID3D12GraphicsCommandListVtbl, Close, 9);
SLOT_IS(ID3D12GraphicsCommandListVtbl, Reset, 10);
SLOT_IS(ID3D12GraphicsCommandListVtbl, Dispatch, 14);
SLOT_IS(ID3D12GraphicsCommandListVtbl, SetPipelineState, 25);
SLOT_IS(ID3D12GraphicsCommandListVtbl, SetDescriptorHeaps, 28);
SLOT_IS(ID3D12GraphicsCommandListVtbl, SetComputeRootSignature, 29);
SLOT_IS(ID3D12GraphicsCommandListVtbl, SetComputeRootDescriptorTable, 31);
SLOT_IS(ID3D12GraphicsCommandListVtbl, SetComputeRoot32BitConstant, 33);
SLOT_IS(ID3D12GraphicsCommandListVtbl, SetComputeRoot32BitConstants, 35);
SLOT_IS(ID3D12GraphicsCommandListVtbl, SetComputeRootConstantBufferView, 37);
SLOT_IS(ID3D12GraphicsCommandListVtbl, SetComputeRootShaderResourceView, 39);
SLOT_IS(ID3D12GraphicsCommandListVtbl, SetComputeRootUnorderedAccessView, 41);

SLOT_IS(ID3D12CommandQueueVtbl, ExecuteCommandLists, 10);
SLOT_IS(ID3D12ResourceVtbl, GetDesc, 10);

/* A translation unit needs at least one declaration under strict C. */
int stray_dlss_vtable_slots_checked = 1;
