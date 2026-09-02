/* Compile-time proof that vtable_slots.hpp's numbers are d3d12.h's. C, because the Vtbl
 * structs only exist in the C interface; mingw's older d3d12.h lacks the ID3D12Device2 one,
 * so that single check is conditional. Nothing here is linked into anything. */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#define CINTERFACE
#define COBJMACROS
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <stddef.h>

/* A negative array size is a compile error in every C standard MSVC and mingw speak; that
 * is the whole assertion. The typedef's name carries the member so the error names it. */
#define SLOT_IS(vtbl, member, index) \
	typedef char slot_check_##vtbl##_##member[(offsetof(vtbl, member) == (index) * sizeof(void *)) ? 1 : -1]

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
#ifdef __ID3D12Device4_INTERFACE_DEFINED__
SLOT_IS(ID3D12Device4Vtbl, CreateCommittedResource1, 53);
SLOT_IS(ID3D12Device4Vtbl, CreateReservedResource1, 55);
#endif
#ifdef __ID3D12Device8_INTERFACE_DEFINED__
SLOT_IS(ID3D12Device8Vtbl, CreateCommittedResource2, 69);
SLOT_IS(ID3D12Device8Vtbl, CreatePlacedResource1, 70);
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

SLOT_IS(IDXGIFactoryVtbl, CreateSwapChain, 10);
SLOT_IS(IDXGIFactory2Vtbl, CreateSwapChainForHwnd, 15);
SLOT_IS(IDXGIFactory2Vtbl, CreateSwapChainForCoreWindow, 16);
SLOT_IS(IDXGIFactory2Vtbl, CreateSwapChainForComposition, 24);
SLOT_IS(IDXGISwapChainVtbl, Present, 8);
SLOT_IS(IDXGISwapChainVtbl, GetBuffer, 9);
SLOT_IS(IDXGISwapChainVtbl, ResizeBuffers, 13);
SLOT_IS(IDXGISwapChain1Vtbl, Present1, 22);
SLOT_IS(IDXGISwapChain3Vtbl, GetCurrentBackBufferIndex, 36);
SLOT_IS(IDXGISwapChain3Vtbl, ResizeBuffers1, 39);

/* A translation unit needs at least one declaration under strict C. */
int stray_dlss_vtable_slots_checked = 1;
