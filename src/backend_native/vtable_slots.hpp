// The COM vtable slots the native backend patches, in d3d12.h's declaration order. Every
// number here is checked at compile time against the C-interface Vtbl structs the Windows
// SDK ships (vtable_slots_check.c) — a wrong index would otherwise be a crash in the game
// rather than an error anywhere useful. Interface chains are single-inheritance, and vkd3d
// (measured) and WARP (measured) implement each object with ONE vtable covering the newest
// version of its interface, so an ID3D12Device2 slot is simply a higher index in the same
// table as ID3D12Device's.
#pragma once

namespace stray_dlss::native::slot {

// IUnknown 0-2, ID3D12Object 3-6, ID3D12Device 7-43, ID3D12Device1 44-46, ID3D12Device2 47.
constexpr unsigned kDevice_CreateCommandQueue = 8;
constexpr unsigned kDevice_CreateCommandAllocator = 9;
constexpr unsigned kDevice_CreateGraphicsPipelineState = 10;
constexpr unsigned kDevice_CreateComputePipelineState = 11;
constexpr unsigned kDevice_CreateCommandList = 12;
constexpr unsigned kDevice_CreateDescriptorHeap = 14;
constexpr unsigned kDevice_GetDescriptorHandleIncrementSize = 15;
constexpr unsigned kDevice_CreateRootSignature = 16;
constexpr unsigned kDevice_CreateConstantBufferView = 17;
constexpr unsigned kDevice_CreateShaderResourceView = 18;
constexpr unsigned kDevice_CreateUnorderedAccessView = 19;
constexpr unsigned kDevice_CreateRenderTargetView = 20;
constexpr unsigned kDevice_CreateDepthStencilView = 21;
constexpr unsigned kDevice_CopyDescriptors = 23;
constexpr unsigned kDevice_CopyDescriptorsSimple = 24;
constexpr unsigned kDevice_CreateCommittedResource = 27;
constexpr unsigned kDevice_CreatePlacedResource = 29;
constexpr unsigned kDevice_CreateReservedResource = 30;
constexpr unsigned kDevice2_CreatePipelineState = 47;
// ID3D12Device3 48-50, ID3D12Device4 51-56, ID3D12Device5 57-64, ID3D12Device6 65,
// ID3D12Device7 66-67, ID3D12Device8 68-72.
constexpr unsigned kDevice4_CreateCommittedResource1 = 53;
constexpr unsigned kDevice4_CreateReservedResource1 = 55;
constexpr unsigned kDevice8_CreateCommittedResource2 = 69;
constexpr unsigned kDevice8_CreatePlacedResource1 = 70;

// IUnknown 0-2, ID3D12Object 3-6, ID3D12DeviceChild::GetDevice 7, ID3D12CommandList::GetType 8,
// ID3D12GraphicsCommandList 9-59.
constexpr unsigned kList_Close = 9;
constexpr unsigned kList_Reset = 10;
constexpr unsigned kList_Dispatch = 14;
constexpr unsigned kList_SetPipelineState = 25;
constexpr unsigned kList_SetDescriptorHeaps = 28;
constexpr unsigned kList_SetComputeRootSignature = 29;
constexpr unsigned kList_SetComputeRootDescriptorTable = 31;
constexpr unsigned kList_SetComputeRoot32BitConstant = 33;
constexpr unsigned kList_SetComputeRoot32BitConstants = 35;
constexpr unsigned kList_SetComputeRootConstantBufferView = 37;
constexpr unsigned kList_SetComputeRootShaderResourceView = 39;
constexpr unsigned kList_SetComputeRootUnorderedAccessView = 41;

// IUnknown 0-2, ID3D12Object 3-6, GetDevice 7, ID3D12CommandQueue 8-18.
constexpr unsigned kQueue_ExecuteCommandLists = 10;

// ID3D12Resource: GetDevice 7, Map 8, Unmap 9, GetDesc 10, GetGPUVirtualAddress 11, ...
constexpr unsigned kResource_GetDesc = 10;

// DXGI, for the native present owner. IUnknown 0-2, IDXGIObject 3-6, IDXGIFactory 7-11,
// IDXGIFactory1 12-13, IDXGIFactory2 14-24. Checked against dxgi1_4.h's C Vtbl structs.
constexpr unsigned kFactory_CreateSwapChain = 10;
constexpr unsigned kFactory2_CreateSwapChainForHwnd = 15;
constexpr unsigned kFactory2_CreateSwapChainForCoreWindow = 16;
constexpr unsigned kFactory2_CreateSwapChainForComposition = 24;
// IUnknown 0-2, IDXGIObject 3-6, IDXGIDeviceSubObject::GetDevice 7, IDXGISwapChain 8-17,
// IDXGISwapChain1 18-28, IDXGISwapChain2 29-35, IDXGISwapChain3 36-39.
constexpr unsigned kSwapChain_Present = 8;
constexpr unsigned kSwapChain_GetBuffer = 9;
constexpr unsigned kSwapChain_SetFullscreenState = 10;
constexpr unsigned kSwapChain_ResizeBuffers = 13;
constexpr unsigned kSwapChain_ResizeTarget = 14;
constexpr unsigned kSwapChain1_Present1 = 22;
constexpr unsigned kSwapChain3_GetCurrentBackBufferIndex = 36;
constexpr unsigned kSwapChain3_SetColorSpace1 = 38;
constexpr unsigned kSwapChain3_ResizeBuffers1 = 39;

} // namespace stray_dlss::native::slot
