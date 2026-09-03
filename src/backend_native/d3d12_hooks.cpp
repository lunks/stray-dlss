#include "backend_native/d3d12_hooks.hpp"

#include "app/diff_observer.hpp"
#include "backend_native/descriptor_shadow.hpp"
#include "backend_native/native_backend.hpp"
#include "backend_native/present_owner.hpp"
#include "backend_native/resource_registry.hpp"
#include "backend_native/root_shadow.hpp"
#include "backend_native/vtable_patch.hpp"
#include "backend_native/vtable_slots.hpp"
#include "core/dxgi_format.hpp"
#include "core/fnv1a.hpp"
#include "log.hpp"
#include "perf.hpp"

#include <d3d12.h>
#include <wrl/client.h>

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <mutex>
#include <unordered_map>

using Microsoft::WRL::ComPtr;

namespace stray_dlss::native::hooks {

// Stream-walk result types and the forward-record live at namespace scope (not the anonymous
// namespace) so the create hooks (anon) and walk_stream's later definition share one
// declaration. walk_stream's BODY needs ID3D12Device2 stream types; its signature does not, so
// the forward declaration is unguarded (unused and harmless where Device2 is absent).
struct StreamContents
{
	const void *cs = nullptr;
	std::size_t cs_len = 0;
	const void *cached_blob = nullptr;
	std::size_t cached_size = 0;
	const char *why = nullptr;
};

// The last stream-create forward: the desc/stream pointer we handed the original and the
// CachedPSO blob we OBSERVED there. The WARP test asserts these equal what the caller passed -
// proof the hook neither copies the desc nor drops the blob, so it is NOT what defeats vkd3d's
// pipeline cache (facts §32.12).
struct CreateForwardRecord
{
	std::atomic<const void *> desc{ nullptr };
	std::atomic<const void *> stream{ nullptr };
	std::atomic<std::size_t> stream_size{ 0 };
	std::atomic<const void *> cached_blob{ nullptr };
	std::atomic<std::size_t> cached_size{ 0 };
	std::atomic<bool> cs_found{ false };
	std::atomic<std::uint64_t> stream_creates{ 0 };
	std::atomic<std::uint64_t> graphics_creates{ 0 };
};
CreateForwardRecord g_forward;
StreamContents walk_stream(const void *stream, std::size_t size);

namespace {

// ---- what the hooks collect ----

std::mutex g_mutex;
std::unordered_map<::ID3D12RootSignature *, core::RootLayout> g_layouts;
std::unordered_map<void *, std::uint64_t> g_pipeline_hashes;
std::atomic<std::uint32_t> g_increment{ 0 };

// ---- the originals, one per patched slot ----

using PFN_CreateCommandQueue = HRESULT(STDMETHODCALLTYPE *)(ID3D12Device *, const D3D12_COMMAND_QUEUE_DESC *, REFIID, void **);
using PFN_CreateCommittedResource = HRESULT(STDMETHODCALLTYPE *)(ID3D12Device *, const D3D12_HEAP_PROPERTIES *, D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC *, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE *, REFIID, void **);
using PFN_CreateDescriptorHeap = HRESULT(STDMETHODCALLTYPE *)(ID3D12Device *, const D3D12_DESCRIPTOR_HEAP_DESC *, REFIID, void **);
using PFN_CreatePlacedResource = HRESULT(STDMETHODCALLTYPE *)(ID3D12Device *, ID3D12Heap *, UINT64, const D3D12_RESOURCE_DESC *, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE *, REFIID, void **);
using PFN_CreateReservedResource = HRESULT(STDMETHODCALLTYPE *)(ID3D12Device *, const D3D12_RESOURCE_DESC *, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE *, REFIID, void **);
using PFN_CreateConstantBufferView = void(STDMETHODCALLTYPE *)(ID3D12Device *, const D3D12_CONSTANT_BUFFER_VIEW_DESC *, D3D12_CPU_DESCRIPTOR_HANDLE);
using PFN_CreateShaderResourceView = void(STDMETHODCALLTYPE *)(ID3D12Device *, ID3D12Resource *, const D3D12_SHADER_RESOURCE_VIEW_DESC *, D3D12_CPU_DESCRIPTOR_HANDLE);
using PFN_CreateUnorderedAccessView = void(STDMETHODCALLTYPE *)(ID3D12Device *, ID3D12Resource *, ID3D12Resource *, const D3D12_UNORDERED_ACCESS_VIEW_DESC *, D3D12_CPU_DESCRIPTOR_HANDLE);
using PFN_CreateRenderTargetView = void(STDMETHODCALLTYPE *)(ID3D12Device *, ID3D12Resource *, const D3D12_RENDER_TARGET_VIEW_DESC *, D3D12_CPU_DESCRIPTOR_HANDLE);
using PFN_CreateDepthStencilView = void(STDMETHODCALLTYPE *)(ID3D12Device *, ID3D12Resource *, const D3D12_DEPTH_STENCIL_VIEW_DESC *, D3D12_CPU_DESCRIPTOR_HANDLE);
using PFN_CopyDescriptors = void(STDMETHODCALLTYPE *)(ID3D12Device *, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE *, const UINT *, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE *, const UINT *, D3D12_DESCRIPTOR_HEAP_TYPE);
using PFN_CopyDescriptorsSimple = void(STDMETHODCALLTYPE *)(ID3D12Device *, UINT, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_DESCRIPTOR_HEAP_TYPE);
using PFN_CreateRootSignature = HRESULT(STDMETHODCALLTYPE *)(ID3D12Device *, UINT, const void *, SIZE_T, REFIID, void **);
using PFN_CreateComputePipelineState = HRESULT(STDMETHODCALLTYPE *)(ID3D12Device *, const D3D12_COMPUTE_PIPELINE_STATE_DESC *, REFIID, void **);
#ifdef __ID3D12Device2_INTERFACE_DEFINED__
using PFN_CreatePipelineState = HRESULT(STDMETHODCALLTYPE *)(ID3D12Device2 *, const D3D12_PIPELINE_STATE_STREAM_DESC *, REFIID, void **);
#endif

using PFN_List_Reset = HRESULT(STDMETHODCALLTYPE *)(ID3D12GraphicsCommandList *, ID3D12CommandAllocator *, ID3D12PipelineState *);
using PFN_List_SetDescriptorHeaps = void(STDMETHODCALLTYPE *)(ID3D12GraphicsCommandList *, UINT, ID3D12DescriptorHeap *const *);
using PFN_List_SetPipelineState = void(STDMETHODCALLTYPE *)(ID3D12GraphicsCommandList *, ID3D12PipelineState *);
using PFN_List_SetComputeRootSignature = void(STDMETHODCALLTYPE *)(ID3D12GraphicsCommandList *, ID3D12RootSignature *);
using PFN_List_SetComputeRootDescriptorTable = void(STDMETHODCALLTYPE *)(ID3D12GraphicsCommandList *, UINT, D3D12_GPU_DESCRIPTOR_HANDLE);
using PFN_List_SetComputeRoot32BitConstant = void(STDMETHODCALLTYPE *)(ID3D12GraphicsCommandList *, UINT, UINT, UINT);
using PFN_List_SetComputeRoot32BitConstants = void(STDMETHODCALLTYPE *)(ID3D12GraphicsCommandList *, UINT, UINT, const void *, UINT);
using PFN_List_SetComputeRootView = void(STDMETHODCALLTYPE *)(ID3D12GraphicsCommandList *, UINT, D3D12_GPU_VIRTUAL_ADDRESS);
using PFN_List_Dispatch = void(STDMETHODCALLTYPE *)(ID3D12GraphicsCommandList *, UINT, UINT, UINT);

PFN_CreateCommandQueue g_orig_CreateCommandQueue = nullptr;
PFN_CreateCommittedResource g_orig_CreateCommittedResource = nullptr;
PFN_CreateDescriptorHeap g_orig_CreateDescriptorHeap = nullptr;
PFN_CreatePlacedResource g_orig_CreatePlacedResource = nullptr;
PFN_CreateReservedResource g_orig_CreateReservedResource = nullptr;
PFN_CreateConstantBufferView g_orig_CreateConstantBufferView = nullptr;
PFN_CreateShaderResourceView g_orig_CreateShaderResourceView = nullptr;
PFN_CreateUnorderedAccessView g_orig_CreateUnorderedAccessView = nullptr;
PFN_CreateRenderTargetView g_orig_CreateRenderTargetView = nullptr;
PFN_CreateDepthStencilView g_orig_CreateDepthStencilView = nullptr;
PFN_CopyDescriptors g_orig_CopyDescriptors = nullptr;
PFN_CopyDescriptorsSimple g_orig_CopyDescriptorsSimple = nullptr;
PFN_CreateRootSignature g_orig_CreateRootSignature = nullptr;
PFN_CreateComputePipelineState g_orig_CreateComputePipelineState = nullptr;
#ifdef __ID3D12Device2_INTERFACE_DEFINED__
PFN_CreatePipelineState g_orig_CreatePipelineState = nullptr;
#endif
#ifdef __ID3D12Device8_INTERFACE_DEFINED__
using PFN_CreateCommittedResource1 = HRESULT(STDMETHODCALLTYPE *)(ID3D12Device4 *, const D3D12_HEAP_PROPERTIES *, D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC *, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE *, ID3D12ProtectedResourceSession *, REFIID, void **);
using PFN_CreateReservedResource1 = HRESULT(STDMETHODCALLTYPE *)(ID3D12Device4 *, const D3D12_RESOURCE_DESC *, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE *, ID3D12ProtectedResourceSession *, REFIID, void **);
using PFN_CreateCommittedResource2 = HRESULT(STDMETHODCALLTYPE *)(ID3D12Device8 *, const D3D12_HEAP_PROPERTIES *, D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC1 *, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE *, ID3D12ProtectedResourceSession *, REFIID, void **);
using PFN_CreatePlacedResource1 = HRESULT(STDMETHODCALLTYPE *)(ID3D12Device8 *, ID3D12Heap *, UINT64, const D3D12_RESOURCE_DESC1 *, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE *, REFIID, void **);
PFN_CreateCommittedResource1 g_orig_CreateCommittedResource1 = nullptr;
PFN_CreateReservedResource1 g_orig_CreateReservedResource1 = nullptr;
PFN_CreateCommittedResource2 g_orig_CreateCommittedResource2 = nullptr;
PFN_CreatePlacedResource1 g_orig_CreatePlacedResource1 = nullptr;
#endif
PFN_List_Reset g_orig_List_Reset = nullptr;
PFN_List_SetDescriptorHeaps g_orig_List_SetDescriptorHeaps = nullptr;
PFN_List_SetPipelineState g_orig_List_SetPipelineState = nullptr;
PFN_List_SetComputeRootSignature g_orig_List_SetComputeRootSignature = nullptr;
PFN_List_SetComputeRootDescriptorTable g_orig_List_SetComputeRootDescriptorTable = nullptr;
PFN_List_SetComputeRoot32BitConstant g_orig_List_SetComputeRoot32BitConstant = nullptr;
PFN_List_SetComputeRoot32BitConstants g_orig_List_SetComputeRoot32BitConstants = nullptr;
PFN_List_SetComputeRootView g_orig_List_SetComputeRootConstantBufferView = nullptr;
PFN_List_SetComputeRootView g_orig_List_SetComputeRootShaderResourceView = nullptr;
PFN_List_SetComputeRootView g_orig_List_SetComputeRootUnorderedAccessView = nullptr;
PFN_List_Dispatch g_orig_List_Dispatch = nullptr;

// ---- helpers ----

shadow::ViewEntry entry_for(ID3D12Resource *res, shadow::ViewKind kind, DXGI_FORMAT view_format)
{
	shadow::ViewEntry e;
	e.kind = kind;
	if (res == nullptr)
		return e;
	registry::note_created(res); // swapchain buffers never pass through the creation hooks
	icept::ResourceInfo info;
	if (!registry::describe(reinterpret_cast<icept::ResourceId>(res), info))
		return e;
	e.resource = reinterpret_cast<icept::ResourceId>(res);
	e.resource_gen = registry::generation_of(e.resource);
	e.is_buffer = info.is_buffer;
	e.width = info.width;
	e.height = info.height;
	e.is_3d = info.is_3d;
	// The VIEW format is what matters, not the resource format: depth and stencil are two
	// views of one typeless resource and are only distinguishable this way (CLAUDE.md §2.3).
	// A default view (no desc, or DXGI_FORMAT_UNKNOWN) takes the resource's.
	const std::uint32_t f = view_format != DXGI_FORMAT_UNKNOWN ? static_cast<std::uint32_t>(view_format) : info.dxgi_format;
	e.dxgi_format = f;
	e.format = info.is_buffer ? TexFormat::unknown : tex_format_from_dxgi(f);
	return e;
}

void store_layout(ID3D12RootSignature *rs, const void *blob, SIZE_T len)
{
	ComPtr<ID3D12VersionedRootSignatureDeserializer> deser;
	if (FAILED(D3D12CreateVersionedRootSignatureDeserializer(blob, len, IID_PPV_ARGS(&deser))) || !deser)
	{
		STRAY_LOG_WARN("native hooks: could not deserialize a root signature (%p, %zu bytes); its tables "
			"cannot be walked", static_cast<void *>(rs), static_cast<size_t>(len));
		return;
	}
	const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *v = deser->GetUnconvertedRootSignatureDesc();
	if (v == nullptr)
		return;
	core::RootLayout layout;
	const auto convert_range_type = [](D3D12_DESCRIPTOR_RANGE_TYPE t) {
		switch (t)
		{
		case D3D12_DESCRIPTOR_RANGE_TYPE_SRV: return core::RangeKind::srv;
		case D3D12_DESCRIPTOR_RANGE_TYPE_UAV: return core::RangeKind::uav;
		case D3D12_DESCRIPTOR_RANGE_TYPE_CBV: return core::RangeKind::cbv;
		default: return core::RangeKind::sampler;
		}
	};
	if (v->Version == D3D_ROOT_SIGNATURE_VERSION_1_0)
	{
		const D3D12_ROOT_SIGNATURE_DESC &d = v->Desc_1_0;
		for (UINT i = 0; i < d.NumParameters; ++i)
		{
			const D3D12_ROOT_PARAMETER &p = d.pParameters[i];
			core::RootParam rp;
			switch (p.ParameterType)
			{
			case D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE:
				rp.kind = core::ParamKind::table;
				for (UINT r = 0; r < p.DescriptorTable.NumDescriptorRanges; ++r)
				{
					const D3D12_DESCRIPTOR_RANGE &dr = p.DescriptorTable.pDescriptorRanges[r];
					rp.ranges.push_back(core::RootRange{ convert_range_type(dr.RangeType), dr.BaseShaderRegister,
						dr.NumDescriptors, dr.RegisterSpace, dr.OffsetInDescriptorsFromTableStart });
				}
				break;
			case D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS:
				rp.kind = core::ParamKind::constants;
				rp.shader_register = p.Constants.ShaderRegister;
				rp.space = p.Constants.RegisterSpace;
				rp.num_32bit_values = p.Constants.Num32BitValues;
				break;
			case D3D12_ROOT_PARAMETER_TYPE_CBV: rp.kind = core::ParamKind::cbv; rp.shader_register = p.Descriptor.ShaderRegister; rp.space = p.Descriptor.RegisterSpace; break;
			case D3D12_ROOT_PARAMETER_TYPE_SRV: rp.kind = core::ParamKind::srv; rp.shader_register = p.Descriptor.ShaderRegister; rp.space = p.Descriptor.RegisterSpace; break;
			case D3D12_ROOT_PARAMETER_TYPE_UAV: rp.kind = core::ParamKind::uav; rp.shader_register = p.Descriptor.ShaderRegister; rp.space = p.Descriptor.RegisterSpace; break;
			}
			layout.params.push_back(std::move(rp));
		}
	}
	else if (v->Version == D3D_ROOT_SIGNATURE_VERSION_1_1)
	{
		const D3D12_ROOT_SIGNATURE_DESC1 &d = v->Desc_1_1;
		for (UINT i = 0; i < d.NumParameters; ++i)
		{
			const D3D12_ROOT_PARAMETER1 &p = d.pParameters[i];
			core::RootParam rp;
			switch (p.ParameterType)
			{
			case D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE:
				rp.kind = core::ParamKind::table;
				for (UINT r = 0; r < p.DescriptorTable.NumDescriptorRanges; ++r)
				{
					const D3D12_DESCRIPTOR_RANGE1 &dr = p.DescriptorTable.pDescriptorRanges[r];
					rp.ranges.push_back(core::RootRange{ convert_range_type(dr.RangeType), dr.BaseShaderRegister,
						dr.NumDescriptors, dr.RegisterSpace, dr.OffsetInDescriptorsFromTableStart });
				}
				break;
			case D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS:
				rp.kind = core::ParamKind::constants;
				rp.shader_register = p.Constants.ShaderRegister;
				rp.space = p.Constants.RegisterSpace;
				rp.num_32bit_values = p.Constants.Num32BitValues;
				break;
			case D3D12_ROOT_PARAMETER_TYPE_CBV: rp.kind = core::ParamKind::cbv; rp.shader_register = p.Descriptor.ShaderRegister; rp.space = p.Descriptor.RegisterSpace; break;
			case D3D12_ROOT_PARAMETER_TYPE_SRV: rp.kind = core::ParamKind::srv; rp.shader_register = p.Descriptor.ShaderRegister; rp.space = p.Descriptor.RegisterSpace; break;
			case D3D12_ROOT_PARAMETER_TYPE_UAV: rp.kind = core::ParamKind::uav; rp.shader_register = p.Descriptor.ShaderRegister; rp.space = p.Descriptor.RegisterSpace; break;
			}
			layout.params.push_back(std::move(rp));
		}
	}
	else
	{
		STRAY_LOG_WARN("native hooks: root signature %p has version %d, which this build does not walk",
			static_cast<void *>(rs), static_cast<int>(v->Version));
		return;
	}
	std::lock_guard<std::mutex> lock(g_mutex);
	g_layouts[rs] = std::move(layout);
}

// drive mode: the sink to deliver to, or null (observe, off, no sink, or our own code).
icept::Sink *drive_sink()
{
	if (mode() != Mode::drive || in_own_code())
		return nullptr;
	return sink();
}

void store_pipeline(void *pso, const void *code, std::size_t len)
{
	if (pso == nullptr)
		return;
	const std::uint64_t hash = (code != nullptr && len != 0) ? fnv1a64(code, len) : 0;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		g_pipeline_hashes[pso] = hash;
	}
	// drive: the application hashes and identifies the shader itself (on_pipeline), exactly
	// as the ReShade host's init_pipeline fed it. Compute only; the graphics PSOs the finders
	// want are still the ReShade host's while it is loaded.
	if (code != nullptr && len != 0)
	{
		if (icept::Sink *sk = drive_sink())
		{
			count_drive_pipeline();
			sk->on_pipeline(reinterpret_cast<std::uint64_t>(pso), code, len, icept::ShaderKind::compute, true);
		}
	}
}

// ---- device hooks ----

HRESULT STDMETHODCALLTYPE hk_CreateCommandQueue(ID3D12Device *self, const D3D12_COMMAND_QUEUE_DESC *desc, REFIID riid, void **out)
{
	const HRESULT hr = g_orig_CreateCommandQueue(self, desc, riid, out);
	// Every queue, ours included: the present owner needs the REAL queue objects (a proxy
	// device forwards here with the real ones), and it picks by identity or by type.
	if (SUCCEEDED(hr) && out != nullptr && *out != nullptr && desc != nullptr)
		present::note_queue(static_cast<ID3D12CommandQueue *>(*out), static_cast<int>(desc->Type));
	return hr;
}

HRESULT STDMETHODCALLTYPE hk_CreateDescriptorHeap(ID3D12Device *self, const D3D12_DESCRIPTOR_HEAP_DESC *desc, REFIID riid, void **out)
{
	if (!in_own_code())
		perf::stall_note_heap_created();
	const HRESULT hr = g_orig_CreateDescriptorHeap(self, desc, riid, out);
	// The fast shadow needs every CBV_SRV_UAV/RTV/DSV heap the moment it exists, to allocate its
	// flat slot array; note_heap_created is a no-op in debug mode. QueryInterface for the heap
	// (out is void**) so the arg type is right.
	if (SUCCEEDED(hr) && out != nullptr && *out != nullptr && !in_own_code())
	{
		ID3D12DescriptorHeap *heap = nullptr;
		if (SUCCEEDED(static_cast<IUnknown *>(*out)->QueryInterface(IID_PPV_ARGS(&heap))) && heap != nullptr)
		{
			shadow::note_heap_created(heap);
			heap->Release();
		}
	}
	return hr;
}

HRESULT STDMETHODCALLTYPE hk_CreateCommittedResource(ID3D12Device *self, const D3D12_HEAP_PROPERTIES *hp, D3D12_HEAP_FLAGS hf, const D3D12_RESOURCE_DESC *desc, D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE *clear, REFIID riid, void **out)
{
	const HRESULT hr = g_orig_CreateCommittedResource(self, hp, hf, desc, state, clear, riid, out);
	if (SUCCEEDED(hr) && out != nullptr && *out != nullptr && !in_own_code())
		registry::note_created(static_cast<ID3D12Resource *>(*out));
	return hr;
}

HRESULT STDMETHODCALLTYPE hk_CreatePlacedResource(ID3D12Device *self, ID3D12Heap *heap, UINT64 offset, const D3D12_RESOURCE_DESC *desc, D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE *clear, REFIID riid, void **out)
{
	const HRESULT hr = g_orig_CreatePlacedResource(self, heap, offset, desc, state, clear, riid, out);
	if (SUCCEEDED(hr) && out != nullptr && *out != nullptr && !in_own_code())
		registry::note_created(static_cast<ID3D12Resource *>(*out));
	return hr;
}

HRESULT STDMETHODCALLTYPE hk_CreateReservedResource(ID3D12Device *self, const D3D12_RESOURCE_DESC *desc, D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE *clear, REFIID riid, void **out)
{
	const HRESULT hr = g_orig_CreateReservedResource(self, desc, state, clear, riid, out);
	if (SUCCEEDED(hr) && out != nullptr && *out != nullptr && !in_own_code())
		registry::note_created(static_cast<ID3D12Resource *>(*out));
	return hr;
}

#ifdef __ID3D12Device8_INTERFACE_DEFINED__
HRESULT STDMETHODCALLTYPE hk_CreateCommittedResource1(ID3D12Device4 *self, const D3D12_HEAP_PROPERTIES *hp, D3D12_HEAP_FLAGS hf, const D3D12_RESOURCE_DESC *desc, D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE *clear, ID3D12ProtectedResourceSession *session, REFIID riid, void **out)
{
	const HRESULT hr = g_orig_CreateCommittedResource1(self, hp, hf, desc, state, clear, session, riid, out);
	if (SUCCEEDED(hr) && out != nullptr && *out != nullptr && !in_own_code())
		registry::note_created(static_cast<ID3D12Resource *>(*out));
	return hr;
}

HRESULT STDMETHODCALLTYPE hk_CreateReservedResource1(ID3D12Device4 *self, const D3D12_RESOURCE_DESC *desc, D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE *clear, ID3D12ProtectedResourceSession *session, REFIID riid, void **out)
{
	const HRESULT hr = g_orig_CreateReservedResource1(self, desc, state, clear, session, riid, out);
	if (SUCCEEDED(hr) && out != nullptr && *out != nullptr && !in_own_code())
		registry::note_created(static_cast<ID3D12Resource *>(*out));
	return hr;
}

HRESULT STDMETHODCALLTYPE hk_CreateCommittedResource2(ID3D12Device8 *self, const D3D12_HEAP_PROPERTIES *hp, D3D12_HEAP_FLAGS hf, const D3D12_RESOURCE_DESC1 *desc, D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE *clear, ID3D12ProtectedResourceSession *session, REFIID riid, void **out)
{
	const HRESULT hr = g_orig_CreateCommittedResource2(self, hp, hf, desc, state, clear, session, riid, out);
	if (SUCCEEDED(hr) && out != nullptr && *out != nullptr && !in_own_code())
		registry::note_created(static_cast<ID3D12Resource *>(*out));
	return hr;
}

HRESULT STDMETHODCALLTYPE hk_CreatePlacedResource1(ID3D12Device8 *self, ID3D12Heap *heap, UINT64 offset, const D3D12_RESOURCE_DESC1 *desc, D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE *clear, REFIID riid, void **out)
{
	const HRESULT hr = g_orig_CreatePlacedResource1(self, heap, offset, desc, state, clear, riid, out);
	if (SUCCEEDED(hr) && out != nullptr && *out != nullptr && !in_own_code())
		registry::note_created(static_cast<ID3D12Resource *>(*out));
	return hr;
}
#endif

void STDMETHODCALLTYPE hk_CreateConstantBufferView(ID3D12Device *self, const D3D12_CONSTANT_BUFFER_VIEW_DESC *desc, D3D12_CPU_DESCRIPTOR_HANDLE handle)
{
	g_orig_CreateConstantBufferView(self, desc, handle);
	perf::Scope _ps(perf::kShadowWrite);
	perf::count(perf::kCntViews);
	if (in_own_code())
		return;
	if (desc == nullptr || desc->BufferLocation == 0)
	{
		shadow::note_null_view(handle.ptr);
		return;
	}
	shadow::ViewEntry e;
	e.kind = shadow::ViewKind::cbv;
	e.is_buffer = true;
	icept::BufferRange br;
	if (registry::buffer_for_va(desc->BufferLocation, br))
	{
		e.resource = br.buffer;
		e.resource_gen = registry::generation_of(br.buffer);
		e.buffer_offset = br.offset;
	}
	else
	{
		shadow::count_unknown_lookup();
	}
	e.buffer_size = desc->SizeInBytes;
	shadow::note_view(handle.ptr, e);
}

void STDMETHODCALLTYPE hk_CreateShaderResourceView(ID3D12Device *self, ID3D12Resource *res, const D3D12_SHADER_RESOURCE_VIEW_DESC *desc, D3D12_CPU_DESCRIPTOR_HANDLE handle)
{
	g_orig_CreateShaderResourceView(self, res, desc, handle);
	perf::Scope _ps(perf::kShadowWrite);
	perf::count(perf::kCntViews);
	if (in_own_code())
		return;
	if (res == nullptr)
	{
		shadow::note_null_view(handle.ptr);
		return;
	}
	shadow::note_view(handle.ptr, entry_for(res, shadow::ViewKind::srv, desc != nullptr ? desc->Format : DXGI_FORMAT_UNKNOWN));
}

void STDMETHODCALLTYPE hk_CreateUnorderedAccessView(ID3D12Device *self, ID3D12Resource *res, ID3D12Resource *counter, const D3D12_UNORDERED_ACCESS_VIEW_DESC *desc, D3D12_CPU_DESCRIPTOR_HANDLE handle)
{
	g_orig_CreateUnorderedAccessView(self, res, counter, desc, handle);
	perf::Scope _ps(perf::kShadowWrite);
	perf::count(perf::kCntViews);
	if (in_own_code())
		return;
	if (res == nullptr)
	{
		shadow::note_null_view(handle.ptr);
		return;
	}
	shadow::note_view(handle.ptr, entry_for(res, shadow::ViewKind::uav, desc != nullptr ? desc->Format : DXGI_FORMAT_UNKNOWN));
}

void STDMETHODCALLTYPE hk_CreateRenderTargetView(ID3D12Device *self, ID3D12Resource *res, const D3D12_RENDER_TARGET_VIEW_DESC *desc, D3D12_CPU_DESCRIPTOR_HANDLE handle)
{
	g_orig_CreateRenderTargetView(self, res, desc, handle);
	perf::Scope _ps(perf::kShadowWrite);
	perf::count(perf::kCntViews);
	if (in_own_code())
		return;
	if (res == nullptr)
	{
		shadow::note_null_view(handle.ptr);
		return;
	}
	shadow::note_view(handle.ptr, entry_for(res, shadow::ViewKind::rtv, desc != nullptr ? desc->Format : DXGI_FORMAT_UNKNOWN));
}

void STDMETHODCALLTYPE hk_CreateDepthStencilView(ID3D12Device *self, ID3D12Resource *res, const D3D12_DEPTH_STENCIL_VIEW_DESC *desc, D3D12_CPU_DESCRIPTOR_HANDLE handle)
{
	g_orig_CreateDepthStencilView(self, res, desc, handle);
	perf::Scope _ps(perf::kShadowWrite);
	perf::count(perf::kCntViews);
	if (in_own_code())
		return;
	if (res == nullptr)
	{
		shadow::note_null_view(handle.ptr);
		return;
	}
	shadow::note_view(handle.ptr, entry_for(res, shadow::ViewKind::dsv, desc != nullptr ? desc->Format : DXGI_FORMAT_UNKNOWN));
}

bool shadowed_heap_type(D3D12_DESCRIPTOR_HEAP_TYPE t)
{
	return t == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV || t == D3D12_DESCRIPTOR_HEAP_TYPE_RTV || t == D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
}

std::uint32_t increment_for(ID3D12Device *device, D3D12_DESCRIPTOR_HEAP_TYPE t)
{
	// Per device constant; cached per type.
	static std::atomic<std::uint32_t> s_inc[4] = {};
	const unsigned i = static_cast<unsigned>(t) < 4 ? static_cast<unsigned>(t) : 0;
	std::uint32_t v = s_inc[i].load(std::memory_order_relaxed);
	if (v == 0)
	{
		v = device->GetDescriptorHandleIncrementSize(t);
		s_inc[i].store(v, std::memory_order_relaxed);
	}
	return v;
}

void STDMETHODCALLTYPE hk_CopyDescriptors(ID3D12Device *self, UINT num_dst, const D3D12_CPU_DESCRIPTOR_HANDLE *dst_starts, const UINT *dst_sizes, UINT num_src, const D3D12_CPU_DESCRIPTOR_HANDLE *src_starts, const UINT *src_sizes, D3D12_DESCRIPTOR_HEAP_TYPE type)
{
	g_orig_CopyDescriptors(self, num_dst, dst_starts, dst_sizes, num_src, src_starts, src_sizes, type);
	perf::Scope _ps(perf::kShadowCopy);
	perf::count(perf::kCntCopyCalls);
	if (in_own_code() || !shadowed_heap_type(type) || dst_starts == nullptr || src_starts == nullptr)
		return;
	// D3D12's rule: destination ranges are filled in order from the source ranges in order;
	// a null size array means every range is one descriptor.
	const std::uint32_t inc = increment_for(self, type);
	// Walk destination ranges against source ranges and hand the shadow the longest runs that
	// are consecutive on BOTH sides (the common case is one 1:1 range).
	UINT si = 0, sj = 0; // source range, index within it
	for (UINT di = 0; di < num_dst; ++di)
	{
		const UINT dn = dst_sizes != nullptr ? dst_sizes[di] : 1;
		UINT dj = 0;
		while (dj < dn)
		{
			while (si < num_src && sj >= (src_sizes != nullptr ? src_sizes[si] : 1))
			{
				++si;
				sj = 0;
			}
			if (si >= num_src)
				return;
			const UINT sn = src_sizes != nullptr ? src_sizes[si] : 1;
			const UINT run = (dn - dj) < (sn - sj) ? (dn - dj) : (sn - sj);
			shadow::note_copy_range(dst_starts[di].ptr + static_cast<std::uint64_t>(dj) * inc,
				src_starts[si].ptr + static_cast<std::uint64_t>(sj) * inc, run, inc);
			perf::count(perf::kCntCopyDescs, run);
			dj += run;
			sj += run;
		}
	}
}

void STDMETHODCALLTYPE hk_CopyDescriptorsSimple(ID3D12Device *self, UINT n, D3D12_CPU_DESCRIPTOR_HANDLE dst, D3D12_CPU_DESCRIPTOR_HANDLE src, D3D12_DESCRIPTOR_HEAP_TYPE type)
{
	g_orig_CopyDescriptorsSimple(self, n, dst, src, type);
	perf::Scope _ps(perf::kShadowCopy);
	perf::count(perf::kCntCopyCalls);
	perf::count(perf::kCntCopyDescs, n);
	if (in_own_code() || !shadowed_heap_type(type))
		return;
	const std::uint32_t inc = increment_for(self, type);
	shadow::note_copy_range(dst.ptr, src.ptr, n, inc);
}

HRESULT STDMETHODCALLTYPE hk_CreateRootSignature(ID3D12Device *self, UINT node_mask, const void *blob, SIZE_T len, REFIID riid, void **out)
{
	const HRESULT hr = g_orig_CreateRootSignature(self, node_mask, blob, len, riid, out);
	if (SUCCEEDED(hr) && out != nullptr && *out != nullptr && !in_own_code())
		store_layout(static_cast<ID3D12RootSignature *>(*out), blob, len);
	return hr;
}

HRESULT STDMETHODCALLTYPE hk_CreateComputePipelineState(ID3D12Device *self, const D3D12_COMPUTE_PIPELINE_STATE_DESC *desc, REFIID riid, void **out)
{
	// The caller's desc, UNMODIFIED, to the original: no CachedPSO stripping, no side effect.
	// The forwarded call is timed for stall attribution (a first-sight compile lands here).
	const std::uint64_t t0 = perf::stall_clock_ns();
	const HRESULT hr = g_orig_CreateComputePipelineState(self, desc, riid, out);
	if (SUCCEEDED(hr) && out != nullptr && *out != nullptr && desc != nullptr && !in_own_code())
	{
		perf::stall_note_pso_create(perf::stall_clock_ns() - t0, /*is_compute=*/true);
		store_pipeline(*out, desc->CS.pShaderBytecode, desc->CS.BytecodeLength);
	}
	return hr;
}

#ifdef __ID3D12Device2_INTERFACE_DEFINED__
HRESULT STDMETHODCALLTYPE hk_CreatePipelineState(ID3D12Device2 *self, const D3D12_PIPELINE_STATE_STREAM_DESC *desc, REFIID riid, void **out)
{
	// The caller's desc pointer straight to the original: UE 4.27 routes EVERY graphics and
	// compute PSO through this slot (WinPSO.cpp:819 CreatePipelineStateWrapper with bUseStream),
	// so the desc - CachedPSO subobject and all - MUST reach vkd3d untouched or the app pipeline
	// library and the internal disk cache are defeated (facts §32.12). We only OBSERVE, after
	// the create returns, to hash a compute shader for the census; graphics PSOs are not recorded.
	const std::uint64_t t0 = perf::stall_clock_ns();
	const HRESULT hr = g_orig_CreatePipelineState(self, desc, riid, out);
	if (SUCCEEDED(hr) && out != nullptr && *out != nullptr && desc != nullptr && !in_own_code())
	{
		const StreamContents sc = walk_stream(desc->pPipelineStateSubobjectStream, desc->SizeInBytes);
		perf::stall_note_pso_create(perf::stall_clock_ns() - t0, /*is_compute=*/sc.cs != nullptr);
		g_forward.stream_creates.fetch_add(1, std::memory_order_relaxed);
		g_forward.desc.store(desc, std::memory_order_relaxed);
		g_forward.stream.store(desc->pPipelineStateSubobjectStream, std::memory_order_relaxed);
		g_forward.stream_size.store(desc->SizeInBytes, std::memory_order_relaxed);
		g_forward.cs_found.store(sc.cs != nullptr, std::memory_order_relaxed);
		g_forward.cached_blob.store(sc.cached_blob, std::memory_order_relaxed);
		g_forward.cached_size.store(sc.cached_size, std::memory_order_relaxed);
		if (sc.cs != nullptr)
			store_pipeline(*out, sc.cs, sc.cs_len); // compute: census + TAA hash
		else
			g_forward.graphics_creates.fetch_add(1, std::memory_order_relaxed); // graphics: observed, NOT recorded
		if (sc.cs == nullptr && sc.why != nullptr)
		{
			static std::atomic<int> s_said{ 0 };
			if (s_said.fetch_add(1) < 3)
				STRAY_LOG_WARN("native hooks: pipeline-state stream not fully walked (%s); a compute PSO "
					"in it stays unhashed (graphics PSOs are unaffected - the desc reached the runtime untouched). Logged 3x.", sc.why);
		}
	}
	return hr;
}
#endif

// ---- command-list hooks ----
//
// Every forward to the original runs under OwnCodeScope: whatever the runtime does INSIDE
// our call is not the game's (the debug layer re-enters the public vtable from Dispatch and
// SetPipelineState with GPU-based validation on — measured in CI; vkd3d-proton never does).
// The shadow is written and the sink delivered AFTER the forward, outside that scope.

icept::CommandContext context_for(ID3D12GraphicsCommandList *list)
{
	icept::CommandContext ctx;
	ctx.native = list;
	ctx.device = game_device();
	ctx.backend_cookie = 0; // the native backend needs none
	return ctx;
}

HRESULT STDMETHODCALLTYPE hk_List_Reset(ID3D12GraphicsCommandList *self, ID3D12CommandAllocator *alloc, ID3D12PipelineState *pso)
{
	HRESULT hr;
	{
		OwnCodeScope forward;
		hr = g_orig_List_Reset(self, alloc, pso);
	}
	if (!in_own_code())
	{
		perf::Scope _ps(perf::kRootBind);
		perf::count(perf::kCntRootBinds);
		root::on_reset(self, pso);
		if (icept::Sink *sk = drive_sink())
			sk->on_command_list_reset(context_for(self));
	}
	return hr;
}

void STDMETHODCALLTYPE hk_List_SetDescriptorHeaps(ID3D12GraphicsCommandList *self, UINT n, ID3D12DescriptorHeap *const *heaps)
{
	{
		OwnCodeScope forward;
		g_orig_List_SetDescriptorHeaps(self, n, heaps);
	}
	if (in_own_code())
		return;
	perf::Scope _ps(perf::kHeapBind);
	perf::count(perf::kCntHeapBinds);
	root::on_set_heaps(self, n, heaps);
	for (UINT i = 0; i < n && heaps != nullptr; ++i)
		shadow::note_heap_bound(heaps[i]);
}

void STDMETHODCALLTYPE hk_List_SetPipelineState(ID3D12GraphicsCommandList *self, ID3D12PipelineState *pso)
{
	{
		OwnCodeScope forward;
		g_orig_List_SetPipelineState(self, pso);
	}
	if (!in_own_code())
	{
		perf::Scope _ps(perf::kRootBind);
		perf::count(perf::kCntRootBinds);
		root::on_set_pso(self, pso);
		// The pipeline handle is the ID3D12PipelineState* under both hosts (ReShade's
		// pipeline.handle is the same pointer on D3D12), so the application's per-list
		// "which shader is bound" map keys agree whichever side fed it.
		if (icept::Sink *sk = drive_sink())
			sk->on_bind_pipeline(context_for(self), reinterpret_cast<std::uint64_t>(pso));
	}
}

void STDMETHODCALLTYPE hk_List_SetComputeRootSignature(ID3D12GraphicsCommandList *self, ID3D12RootSignature *rs)
{
	{
		OwnCodeScope forward;
		g_orig_List_SetComputeRootSignature(self, rs);
	}
	if (!in_own_code())
	{
		perf::Scope _ps(perf::kRootBind);
		perf::count(perf::kCntRootBinds);
		root::on_set_compute_root_signature(self, rs);
	}
}

void STDMETHODCALLTYPE hk_List_SetComputeRootDescriptorTable(ID3D12GraphicsCommandList *self, UINT param, D3D12_GPU_DESCRIPTOR_HANDLE handle)
{
	{
		OwnCodeScope forward;
		g_orig_List_SetComputeRootDescriptorTable(self, param, handle);
	}
	if (!in_own_code())
	{
		perf::Scope _ps(perf::kRootBind);
		perf::count(perf::kCntRootBinds);
		root::on_set_compute_table(self, param, handle.ptr);
	}
}

void STDMETHODCALLTYPE hk_List_SetComputeRoot32BitConstant(ID3D12GraphicsCommandList *self, UINT param, UINT value, UINT offset)
{
	{
		OwnCodeScope forward;
		g_orig_List_SetComputeRoot32BitConstant(self, param, value, offset);
	}
	if (!in_own_code())
	{
		perf::Scope _ps(perf::kRootBind);
		perf::count(perf::kCntRootBinds);
		root::on_set_compute_constants(self, param, offset, 1, &value);
	}
}

void STDMETHODCALLTYPE hk_List_SetComputeRoot32BitConstants(ID3D12GraphicsCommandList *self, UINT param, UINT n, const void *data, UINT offset)
{
	{
		OwnCodeScope forward;
		g_orig_List_SetComputeRoot32BitConstants(self, param, n, data, offset);
	}
	if (!in_own_code())
	{
		perf::Scope _ps(perf::kRootBind);
		perf::count(perf::kCntRootBinds);
		root::on_set_compute_constants(self, param, offset, n, static_cast<const std::uint32_t *>(data));
	}
}

void STDMETHODCALLTYPE hk_List_SetComputeRootConstantBufferView(ID3D12GraphicsCommandList *self, UINT param, D3D12_GPU_VIRTUAL_ADDRESS va)
{
	{
		OwnCodeScope forward;
		g_orig_List_SetComputeRootConstantBufferView(self, param, va);
	}
	if (!in_own_code())
	{
		perf::Scope _ps(perf::kRootBind);
		perf::count(perf::kCntRootBinds);
		root::on_set_compute_root_cbv(self, param, va);
	}
}

void STDMETHODCALLTYPE hk_List_SetComputeRootShaderResourceView(ID3D12GraphicsCommandList *self, UINT param, D3D12_GPU_VIRTUAL_ADDRESS va)
{
	{
		OwnCodeScope forward;
		g_orig_List_SetComputeRootShaderResourceView(self, param, va);
	}
	if (!in_own_code())
	{
		perf::Scope _ps(perf::kRootBind);
		perf::count(perf::kCntRootBinds);
		root::on_set_compute_root_srv(self, param, va);
	}
}

void STDMETHODCALLTYPE hk_List_SetComputeRootUnorderedAccessView(ID3D12GraphicsCommandList *self, UINT param, D3D12_GPU_VIRTUAL_ADDRESS va)
{
	{
		OwnCodeScope forward;
		g_orig_List_SetComputeRootUnorderedAccessView(self, param, va);
	}
	if (!in_own_code())
	{
		perf::Scope _ps(perf::kRootBind);
		perf::count(perf::kCntRootBinds);
		root::on_set_compute_root_uav(self, param, va);
	}
}

void STDMETHODCALLTYPE hk_List_Dispatch(ID3D12GraphicsCommandList *self, UINT x, UINT y, UINT z)
{
	// DRIVE: the sink decides. `true` means it produced this pass's output itself (DLSS wrote
	// u0 and the game's compute state was put back), so the game's dispatch must NOT run —
	// the one skip-capable event on our path, now ours to skip. Everything the sink records
	// onto this list in between arrives here under OwnCodeScope and passes straight through.
	//
	// THE FORWARD RUNS UNDER OwnCodeScope, in both branches. MEASURED in CI (2026-09-02): with
	// GPU-based validation on, the debug layer's Dispatch RE-ENTERS the public vtable — it
	// binds its own validation PSO and dispatches it inside the game's call — so every game
	// dispatch reached this hook again from inside its own forward, was delivered to the
	// sink a second time, and left the debug layer's PSO in the root shadow as "the game's"
	// (the restore then replayed it: D3D12 #953, root signature does not match the PSO).
	// The game and vkd3d-proton never re-enter, so treating anything nested inside a forward
	// as not-the-game's costs nothing there and is exactly right here.
	if (icept::Sink *sk = drive_sink())
	{
		const bool suppress = sk->on_dispatch(context_for(self), x, y, z);
		count_drive_dispatch(suppress);
		if (suppress)
			return;
		OwnCodeScope forward;
		g_orig_List_Dispatch(self, x, y, z);
		return;
	}
	// OBSERVE: the driver has already decided; this fires inside its forward of the call.
	// The expectation it parked for this list on this thread is consumed here.
	if (!in_own_code() && diff::enabled() && diff::has_expected(self))
	{
		icept::CommandContext ctx;
		ctx.native = self;
		ctx.device = game_device();
		icept::DispatchBindings actual;
		backend().resolve_compute_bindings(ctx, actual);
		// The adjudicator: the driver's liveness (ReShade's, through the seam) against the
		// registry's sentinel. What the root shadow raw held goes on the line, so "the native
		// side had NO root CBVs" is not confused with "it had different ones".
		diff::Adjudicator adj;
		adj.oracle_live = [](icept::ResourceId r) { return icept::backend()->is_resource_live(r); };
		adj.native_live = [](icept::ResourceId r) { return registry::is_live(r); };
		adj.native_seen = [](icept::ResourceId r) { return registry::ever_seen(r); };
		adj.oracle_view_resource = [](icept::DescriptorId v) {
			icept::ResourceId r = 0;
			return icept::backend()->resource_from_view(v, r) ? r : 0;
		};
		adj.native_slot = [](icept::DescriptorId slot, icept::ResourceId &res, std::uint64_t &seq, bool &via_copy,
		                     icept::DescriptorId &src, bool &dead) {
			shadow::ViewEntry e;
			if (!shadow::lookup(slot, e))
				return false;
			res = e.resource;
			seq = e.seq;
			via_copy = e.via_copy;
			src = e.src_slot;
			dead = e.dead;
			return true;
		};
		char note[160];
		{
			root::ListState st;
			if (root::snapshot(self, st))
				std::snprintf(note, sizeof(note), "shadow rs=%p tables=%zu root-cbv=%zu root-srv=%zu root-uav=%zu consts=%zu",
					static_cast<void *>(st.compute_root_signature), st.compute_tables.size(), st.compute_root_cbv.size(),
					st.compute_root_srv.size(), st.compute_root_uav.size(), st.compute_constants.size());
			else
				std::snprintf(note, sizeof(note), "shadow: list never seen");
		}
		diff::consume_and_compare(self, actual, shadow::unknown_lookups(), &adj, note);
	}
	OwnCodeScope forward;
	g_orig_List_Dispatch(self, x, y, z);
}

} // namespace

// ---- the pipeline-state stream walk ----
//
// Needs ID3D12Device2's stream types; mingw's older d3d12.h (used for the local syntax
// check only) has none of them, so that build gets a stub. CI's SDK has them all.
#ifdef __ID3D12Device2_INTERFACE_DEFINED__

namespace {

// The padding is the point (it is the stream's layout), so C4324 — "padded due to
// alignment specifier" — is silenced exactly as d3dx12.h silences it for its own version.
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4324)
#endif
template <typename T>
struct alignas(void *) Subobject
{
	D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type;
	T value;
};
#ifdef _MSC_VER
#pragma warning(pop)
#endif

// The size of one subobject of payload type T in the stream, and where its payload sits —
// what CD3DX12_PIPELINE_STATE_STREAM_SUBOBJECT lays out (the type, the payload at its own
// alignment, the whole padded to pointer alignment).
template <typename T>
bool step(const unsigned char *&p, const unsigned char *end, std::size_t &payload_offset)
{
	if (static_cast<std::size_t>(end - p) < sizeof(Subobject<T>))
		return false;
	payload_offset = offsetof(Subobject<T>, value);
	p += sizeof(Subobject<T>);
	return true;
}

} // namespace

// One read-only pass over a pipeline-state stream: reports the compute-shader bytecode AND the
// CachedPSO blob it contains. The hook uses the CS for the census and the CachedPSO ONLY to
// prove (in the WARP test) that our forward left the blob intact - we never touch the stream.
StreamContents walk_stream(const void *stream, std::size_t size)
{
	StreamContents out;
	const auto *p = static_cast<const unsigned char *>(stream);
	const unsigned char *const end = p + size;
	while (p != nullptr && static_cast<std::size_t>(end - p) >= sizeof(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE))
	{
		const auto type = *reinterpret_cast<const D3D12_PIPELINE_STATE_SUBOBJECT_TYPE *>(p);
		const unsigned char *const here = p;
		std::size_t off = 0;
		bool ok = true;
		switch (type)
		{
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE: ok = step<ID3D12RootSignature *>(p, end, off); break;
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS:
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS:
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS:
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS:
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS:
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS:
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS:
			ok = step<D3D12_SHADER_BYTECODE>(p, end, off); break;
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS:
			ok = step<D3D12_SHADER_BYTECODE>(p, end, off);
			if (ok)
			{
				const auto *cs = reinterpret_cast<const D3D12_SHADER_BYTECODE *>(here + off);
				out.cs = cs->pShaderBytecode;
				out.cs_len = cs->BytecodeLength;
			}
			break;
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT: ok = step<D3D12_STREAM_OUTPUT_DESC>(p, end, off); break;
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND: ok = step<D3D12_BLEND_DESC>(p, end, off); break;
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK: ok = step<UINT>(p, end, off); break;
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER: ok = step<D3D12_RASTERIZER_DESC>(p, end, off); break;
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL: ok = step<D3D12_DEPTH_STENCIL_DESC>(p, end, off); break;
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT: ok = step<D3D12_INPUT_LAYOUT_DESC>(p, end, off); break;
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE: ok = step<D3D12_INDEX_BUFFER_STRIP_CUT_VALUE>(p, end, off); break;
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY: ok = step<D3D12_PRIMITIVE_TOPOLOGY_TYPE>(p, end, off); break;
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS: ok = step<D3D12_RT_FORMAT_ARRAY>(p, end, off); break;
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT: ok = step<DXGI_FORMAT>(p, end, off); break;
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC: ok = step<DXGI_SAMPLE_DESC>(p, end, off); break;
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK: ok = step<UINT>(p, end, off); break;
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO:
			ok = step<D3D12_CACHED_PIPELINE_STATE>(p, end, off);
			if (ok)
			{
				const auto *cp = reinterpret_cast<const D3D12_CACHED_PIPELINE_STATE *>(here + off);
				out.cached_blob = cp->pCachedBlob;
				out.cached_size = cp->CachedBlobSizeInBytes;
			}
			break;
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS: ok = step<D3D12_PIPELINE_STATE_FLAGS>(p, end, off); break;
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1: ok = step<D3D12_DEPTH_STENCIL_DESC1>(p, end, off); break;
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING: ok = step<D3D12_VIEW_INSTANCING_DESC>(p, end, off); break;
		default:
			// A subobject type this build does not know the size of: stop rather than guess.
			out.why = "unknown subobject type";
			return out;
		}
		if (!ok)
		{
			out.why = "truncated stream";
			return out;
		}
	}
	return out; // walked to the end
}

const void *find_cs_in_stream(const void *stream, std::size_t size, std::size_t &length, const char **stop_reason)
{
	const StreamContents sc = walk_stream(stream, size);
	length = sc.cs_len;
	if (stop_reason != nullptr)
		*stop_reason = sc.why;
	return sc.cs;
}

#else
const void *find_cs_in_stream(const void *, std::size_t, std::size_t &length, const char **stop_reason)
{
	length = 0;
	if (stop_reason != nullptr)
		*stop_reason = "no ID3D12Device2 in this SDK";
	return nullptr;
}
#endif

// ---- installation ----

unsigned install_device_hooks(::ID3D12Device *device, bool query_device2)
{
	if (device == nullptr)
		return 0;
	g_increment.store(device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV));
	unsigned n = 0;
	const auto hook = [&](unsigned index, void *replacement, void **orig, const char *name) {
		void *o = patch_slot(device, index, replacement, name);
		if (o != nullptr)
		{
			*orig = o;
			++n;
		}
	};
	hook(slot::kDevice_CreateCommandQueue, reinterpret_cast<void *>(&hk_CreateCommandQueue), reinterpret_cast<void **>(&g_orig_CreateCommandQueue), "ID3D12Device::CreateCommandQueue");
	hook(slot::kDevice_CreateDescriptorHeap, reinterpret_cast<void *>(&hk_CreateDescriptorHeap), reinterpret_cast<void **>(&g_orig_CreateDescriptorHeap), "ID3D12Device::CreateDescriptorHeap");
	hook(slot::kDevice_CreateCommittedResource, reinterpret_cast<void *>(&hk_CreateCommittedResource), reinterpret_cast<void **>(&g_orig_CreateCommittedResource), "ID3D12Device::CreateCommittedResource");
	hook(slot::kDevice_CreatePlacedResource, reinterpret_cast<void *>(&hk_CreatePlacedResource), reinterpret_cast<void **>(&g_orig_CreatePlacedResource), "ID3D12Device::CreatePlacedResource");
	hook(slot::kDevice_CreateReservedResource, reinterpret_cast<void *>(&hk_CreateReservedResource), reinterpret_cast<void **>(&g_orig_CreateReservedResource), "ID3D12Device::CreateReservedResource");
	hook(slot::kDevice_CreateConstantBufferView, reinterpret_cast<void *>(&hk_CreateConstantBufferView), reinterpret_cast<void **>(&g_orig_CreateConstantBufferView), "ID3D12Device::CreateConstantBufferView");
	hook(slot::kDevice_CreateShaderResourceView, reinterpret_cast<void *>(&hk_CreateShaderResourceView), reinterpret_cast<void **>(&g_orig_CreateShaderResourceView), "ID3D12Device::CreateShaderResourceView");
	hook(slot::kDevice_CreateUnorderedAccessView, reinterpret_cast<void *>(&hk_CreateUnorderedAccessView), reinterpret_cast<void **>(&g_orig_CreateUnorderedAccessView), "ID3D12Device::CreateUnorderedAccessView");
	hook(slot::kDevice_CreateRenderTargetView, reinterpret_cast<void *>(&hk_CreateRenderTargetView), reinterpret_cast<void **>(&g_orig_CreateRenderTargetView), "ID3D12Device::CreateRenderTargetView");
	hook(slot::kDevice_CreateDepthStencilView, reinterpret_cast<void *>(&hk_CreateDepthStencilView), reinterpret_cast<void **>(&g_orig_CreateDepthStencilView), "ID3D12Device::CreateDepthStencilView");
	hook(slot::kDevice_CopyDescriptors, reinterpret_cast<void *>(&hk_CopyDescriptors), reinterpret_cast<void **>(&g_orig_CopyDescriptors), "ID3D12Device::CopyDescriptors");
	hook(slot::kDevice_CopyDescriptorsSimple, reinterpret_cast<void *>(&hk_CopyDescriptorsSimple), reinterpret_cast<void **>(&g_orig_CopyDescriptorsSimple), "ID3D12Device::CopyDescriptorsSimple");
	hook(slot::kDevice_CreateRootSignature, reinterpret_cast<void *>(&hk_CreateRootSignature), reinterpret_cast<void **>(&g_orig_CreateRootSignature), "ID3D12Device::CreateRootSignature");
	hook(slot::kDevice_CreateComputePipelineState, reinterpret_cast<void *>(&hk_CreateComputePipelineState), reinterpret_cast<void **>(&g_orig_CreateComputePipelineState), "ID3D12Device::CreateComputePipelineState");
	// ID3D12Device2::CreatePipelineState is a higher slot in the SAME vtable (one vtable per
	// object covering its newest interface — measured); patch it only if the device is a
	// Device2 at all.
	(void)query_device2; // unused when the SDK has no ID3D12Device2 (mingw's local check)
#ifdef __ID3D12Device8_INTERFACE_DEFINED__
	// The newer creation entry points share the one vtable (ID3D12Device8 on vkd3d-proton and
	// WARP alike); patched without a query, like Device2's below on the proxy. A resource
	// created through one of these and never viewed would otherwise be invisible to the
	// registry, and a root CBV into it "unknown".
	{
		ID3D12Device8 *dev8 = nullptr;
		const bool have8 = !query_device2 ||
			(SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dev8))) && dev8 != nullptr && static_cast<void *>(dev8) == static_cast<void *>(device));
		if (dev8 != nullptr)
			dev8->Release();
		if (have8)
		{
			hook(slot::kDevice4_CreateCommittedResource1, reinterpret_cast<void *>(&hk_CreateCommittedResource1), reinterpret_cast<void **>(&g_orig_CreateCommittedResource1), "ID3D12Device4::CreateCommittedResource1");
			hook(slot::kDevice4_CreateReservedResource1, reinterpret_cast<void *>(&hk_CreateReservedResource1), reinterpret_cast<void **>(&g_orig_CreateReservedResource1), "ID3D12Device4::CreateReservedResource1");
			hook(slot::kDevice8_CreateCommittedResource2, reinterpret_cast<void *>(&hk_CreateCommittedResource2), reinterpret_cast<void **>(&g_orig_CreateCommittedResource2), "ID3D12Device8::CreateCommittedResource2");
			hook(slot::kDevice8_CreatePlacedResource1, reinterpret_cast<void *>(&hk_CreatePlacedResource1), reinterpret_cast<void **>(&g_orig_CreatePlacedResource1), "ID3D12Device8::CreatePlacedResource1");
		}
		else
			STRAY_LOG_WARN("native hooks: no ID3D12Device8 on this device; the *Resource1/2 creation entry points are not hooked");
	}
#endif
#ifdef __ID3D12Device2_INTERFACE_DEFINED__
	if (!query_device2)
	{
		hook(slot::kDevice2_CreatePipelineState, reinterpret_cast<void *>(&hk_CreatePipelineState), reinterpret_cast<void **>(&g_orig_CreatePipelineState), "ID3D12Device2::CreatePipelineState (unqueried)");
		return n;
	}
	ID3D12Device2 *dev2 = nullptr;
	if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dev2))) && dev2 != nullptr)
	{
		if (static_cast<void *>(dev2) == static_cast<void *>(device))
			hook(slot::kDevice2_CreatePipelineState, reinterpret_cast<void *>(&hk_CreatePipelineState), reinterpret_cast<void **>(&g_orig_CreatePipelineState), "ID3D12Device2::CreatePipelineState");
		else
			STRAY_LOG_WARN("native hooks: ID3D12Device2 is a DIFFERENT object (%p vs %p); CreatePipelineState "
				"is not hooked and stream-created compute PSOs stay unhashed", static_cast<void *>(dev2), static_cast<void *>(device));
		dev2->Release();
	}
#endif
	return n;
}

unsigned install_list_hooks(::ID3D12Device *device)
{
	if (device == nullptr)
		return 0;
	// A throwaway list, created with the hooks passing through, purely to reach the
	// command-list vtable — shared by every list of the runtime (measured on vkd3d-proton
	// and WARP, docs/STRAY-RENDERING-FACTS.md §11).
	OwnCodeScope own;
	ComPtr<ID3D12CommandAllocator> alloc;
	ComPtr<ID3D12GraphicsCommandList> list;
	HRESULT hr_alloc = S_OK, hr_list = S_OK;
#ifdef __ID3D12Device4_INTERFACE_DEFINED__
	// Device4's CreateCommandList1 makes a CLOSED list with no allocator at all — nothing to
	// reset, nothing to close, nothing the debug layer can object to. The allocator path is
	// the fallback for a device without it.
	{
		ComPtr<ID3D12Device4> dev4;
		if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dev4))) && dev4)
			hr_list = dev4->CreateCommandList1(0, D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_LIST_FLAG_NONE, IID_PPV_ARGS(&list));
	}
#endif
	if (!list)
	{
		hr_alloc = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
		if (SUCCEEDED(hr_alloc))
			hr_list = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&list));
		if (list)
			list->Close();
	}
	if (!list)
	{
		STRAY_LOG_ERROR("native hooks: could not create the throwaway command list (allocator hr=0x%08lx, list hr=0x%08lx); "
			"list slots NOT hooked", static_cast<unsigned long>(hr_alloc), static_cast<unsigned long>(hr_list));
		return 0;
	}
	unsigned n = 0;
	const auto hook = [&](unsigned index, void *replacement, void **orig, const char *name) {
		void *o = patch_slot(list.Get(), index, replacement, name);
		if (o != nullptr)
		{
			*orig = o;
			++n;
		}
	};
	hook(slot::kList_Reset, reinterpret_cast<void *>(&hk_List_Reset), reinterpret_cast<void **>(&g_orig_List_Reset), "ID3D12GraphicsCommandList::Reset");
	hook(slot::kList_SetDescriptorHeaps, reinterpret_cast<void *>(&hk_List_SetDescriptorHeaps), reinterpret_cast<void **>(&g_orig_List_SetDescriptorHeaps), "ID3D12GraphicsCommandList::SetDescriptorHeaps");
	hook(slot::kList_SetPipelineState, reinterpret_cast<void *>(&hk_List_SetPipelineState), reinterpret_cast<void **>(&g_orig_List_SetPipelineState), "ID3D12GraphicsCommandList::SetPipelineState");
	hook(slot::kList_SetComputeRootSignature, reinterpret_cast<void *>(&hk_List_SetComputeRootSignature), reinterpret_cast<void **>(&g_orig_List_SetComputeRootSignature), "ID3D12GraphicsCommandList::SetComputeRootSignature");
	hook(slot::kList_SetComputeRootDescriptorTable, reinterpret_cast<void *>(&hk_List_SetComputeRootDescriptorTable), reinterpret_cast<void **>(&g_orig_List_SetComputeRootDescriptorTable), "ID3D12GraphicsCommandList::SetComputeRootDescriptorTable");
	hook(slot::kList_SetComputeRoot32BitConstant, reinterpret_cast<void *>(&hk_List_SetComputeRoot32BitConstant), reinterpret_cast<void **>(&g_orig_List_SetComputeRoot32BitConstant), "ID3D12GraphicsCommandList::SetComputeRoot32BitConstant");
	hook(slot::kList_SetComputeRoot32BitConstants, reinterpret_cast<void *>(&hk_List_SetComputeRoot32BitConstants), reinterpret_cast<void **>(&g_orig_List_SetComputeRoot32BitConstants), "ID3D12GraphicsCommandList::SetComputeRoot32BitConstants");
	hook(slot::kList_SetComputeRootConstantBufferView, reinterpret_cast<void *>(&hk_List_SetComputeRootConstantBufferView), reinterpret_cast<void **>(&g_orig_List_SetComputeRootConstantBufferView), "ID3D12GraphicsCommandList::SetComputeRootConstantBufferView");
	hook(slot::kList_SetComputeRootShaderResourceView, reinterpret_cast<void *>(&hk_List_SetComputeRootShaderResourceView), reinterpret_cast<void **>(&g_orig_List_SetComputeRootShaderResourceView), "ID3D12GraphicsCommandList::SetComputeRootShaderResourceView");
	hook(slot::kList_SetComputeRootUnorderedAccessView, reinterpret_cast<void *>(&hk_List_SetComputeRootUnorderedAccessView), reinterpret_cast<void **>(&g_orig_List_SetComputeRootUnorderedAccessView), "ID3D12GraphicsCommandList::SetComputeRootUnorderedAccessView");
	hook(slot::kList_Dispatch, reinterpret_cast<void *>(&hk_List_Dispatch), reinterpret_cast<void **>(&g_orig_List_Dispatch), "ID3D12GraphicsCommandList::Dispatch");
	return n;
}

bool layout_for(::ID3D12RootSignature *rs, core::RootLayout &out)
{
	std::lock_guard<std::mutex> lock(g_mutex);
	const auto it = g_layouts.find(rs);
	if (it == g_layouts.end())
		return false;
	out = it->second;
	return true;
}

std::uint64_t root_signature_count()
{
	std::lock_guard<std::mutex> lock(g_mutex);
	return g_layouts.size();
}

std::uint64_t pipeline_count()
{
	std::lock_guard<std::mutex> lock(g_mutex);
	return g_pipeline_hashes.size();
}

ForwardProbe last_create_forward()
{
	ForwardProbe f;
	f.desc = g_forward.desc.load(std::memory_order_relaxed);
	f.stream = g_forward.stream.load(std::memory_order_relaxed);
	f.stream_size = g_forward.stream_size.load(std::memory_order_relaxed);
	f.cached_blob = g_forward.cached_blob.load(std::memory_order_relaxed);
	f.cached_size = g_forward.cached_size.load(std::memory_order_relaxed);
	f.cs_found = g_forward.cs_found.load(std::memory_order_relaxed);
	f.stream_creates = g_forward.stream_creates.load(std::memory_order_relaxed);
	f.graphics_creates = g_forward.graphics_creates.load(std::memory_order_relaxed);
	return f;
}

std::uint64_t pipeline_hash(void *pso)
{
	std::lock_guard<std::mutex> lock(g_mutex);
	const auto it = g_pipeline_hashes.find(pso);
	return it == g_pipeline_hashes.end() ? 0 : it->second;
}

std::uint32_t descriptor_increment()
{
	return g_increment.load(std::memory_order_relaxed);
}

} // namespace stray_dlss::native::hooks
