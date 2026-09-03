// ---- proof the slot-47 create hook forwards the CachedPSO blob untouched ----
//
// UE 4.27 routes every graphics and compute PSO through ID3D12Device2::CreatePipelineState
// (the stream API), so if our hook copied or rebuilt the desc, or dropped the CACHED_PSO
// subobject, vkd3d-proton's pipeline cache would be defeated and every material would compile
// on first sight, every session (the box symptom, facts §32.12). This creates a compute PSO
// through the patched slot - once plain, once with the PSO's own CachedPSO blob - and asserts,
// via native::hooks::last_create_forward(), that the exact stream pointer and the exact
// {blob, size} we passed reached the runtime.
//
// Runs after test_native_hooks_ue4_shaped_frame has installed the hooks. Uses build_probe's
// root signature so the stream create matches the probe shader (a mismatched root signature
// makes CreatePipelineState fail before the hook can even forward).

#include "backend_native/d3d12_hooks.hpp"

namespace {

// A stream subobject with the runtime's ABI layout (type, then payload at pointer alignment),
// matching CD3DX12_PIPELINE_STATE_STREAM_SUBOBJECT and the walker's own Subobject<T>.
template <typename T>
struct alignas(void *) StreamSub
{
	D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type;
	T value;
};

} // namespace

bool test_pso_cached_blob_passthrough(Gpu &gpu)
{
	std::printf("\n[test] slot-47 CreatePipelineState forwards the CachedPSO blob untouched (vkd3d cache is not defeated by us)\n");
	namespace native = stray_dlss::native;
	ID3D12Device *dev = real_device(gpu);
	ComPtr<ID3D12Device2> dev2;
	if (FAILED(dev->QueryInterface(IID_PPV_ARGS(&dev2))) || !dev2)
	{
		std::printf("  NOTE: no ID3D12Device2 on this device; the stream create path cannot be exercised here\n");
		return true;
	}

	// The probe's own root signature + PSO (CreateComputePipelineState, slot 11), so the stream
	// create below is shader-compatible and actually reaches our hook.
	Probe p;
	if (!build_probe(gpu, p))
	{
		fail("build_probe failed");
		return false;
	}

	// (1) A plain compute stream (root + CS), no cached blob: proves the stream pointer and the
	//     compute-shader observation survive our forward on the create path UE4 actually uses.
	struct PlainStream
	{
		StreamSub<ID3D12RootSignature *> root{ D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, {} };
		StreamSub<D3D12_SHADER_BYTECODE> cs{ D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS, {} };
	} plain{};
	plain.root.value = p.rs.Get();
	plain.cs.value.pShaderBytecode = g_warp_probe_dxbc;
	plain.cs.value.BytecodeLength = sizeof(g_warp_probe_dxbc);
	D3D12_PIPELINE_STATE_STREAM_DESC sd0 = { sizeof(plain), &plain };

	ComPtr<ID3D12PipelineState> pso0;
	const HRESULT hr0 = dev2->CreatePipelineState(&sd0, IID_PPV_ARGS(&pso0));
	check(SUCCEEDED(hr0) && pso0, "a compute PSO was created through the stream slot");
	if (!pso0)
	{
		std::printf("  CreatePipelineState hr=0x%08lx\n", static_cast<unsigned long>(hr0));
		return false;
	}
	{
		const native::hooks::ForwardProbe f = native::hooks::last_create_forward();
		std::printf("  plain create: stream=%p (want %p) cs_found=%d cached=%p/%zu stream_creates=%llu\n",
			f.stream, static_cast<const void *>(&plain), f.cs_found ? 1 : 0, f.cached_blob, f.cached_size,
			static_cast<unsigned long long>(f.stream_creates));
		check(f.stream == static_cast<const void *>(&plain), "the hook forwarded the caller's exact stream pointer");
		check(f.cs_found, "the hook found the compute shader in the stream (census works)");
		check(f.cached_blob == nullptr && f.cached_size == 0, "the plain create carried no cached blob");
	}

	// (2) The real cached blob for that PSO, re-created through the stream with a CACHED_PSO
	//     subobject: proves the blob pointer and size reach the runtime byte-for-byte.
	ComPtr<ID3DBlob> cached;
	const HRESULT hrb = pso0->GetCachedBlob(&cached);
	if (FAILED(hrb) || !cached || cached->GetBufferSize() == 0)
	{
		std::printf("  NOTE: GetCachedBlob unavailable here (hr=0x%08lx); the blob-passthrough half is skipped, the pointer half above stands\n", static_cast<unsigned long>(hrb));
		check(drain_validation(gpu, "pso-cache-plain") == 0, "no validation errors from the plain stream create");
		return true;
	}
	const void *const blob_ptr = cached->GetBufferPointer();
	const std::size_t blob_len = cached->GetBufferSize();
	std::printf("  cached blob: %p / %zu bytes\n", blob_ptr, blob_len);

	struct CachedStream
	{
		StreamSub<ID3D12RootSignature *> root{ D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, {} };
		StreamSub<D3D12_SHADER_BYTECODE> cs{ D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS, {} };
		StreamSub<D3D12_CACHED_PIPELINE_STATE> cached{ D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO, {} };
	} cs2{};
	cs2.root.value = p.rs.Get();
	cs2.cs.value.pShaderBytecode = g_warp_probe_dxbc;
	cs2.cs.value.BytecodeLength = sizeof(g_warp_probe_dxbc);
	cs2.cached.value.pCachedBlob = blob_ptr;
	cs2.cached.value.CachedBlobSizeInBytes = blob_len;
	D3D12_PIPELINE_STATE_STREAM_DESC sd1 = { sizeof(cs2), &cs2 };

	ComPtr<ID3D12PipelineState> pso1;
	const HRESULT hr1 = dev2->CreatePipelineState(&sd1, IID_PPV_ARGS(&pso1));
	check(SUCCEEDED(hr1) && pso1, "the PSO re-created from its own cached blob succeeded (blob accepted by the runtime)");
	const native::hooks::ForwardProbe f = native::hooks::last_create_forward();
	std::printf("  cached create: stream=%p (want %p) cached=%p/%zu (want %p/%zu) cs_found=%d\n",
		f.stream, static_cast<const void *>(&cs2), f.cached_blob, f.cached_size, blob_ptr, blob_len, f.cs_found ? 1 : 0);
	check(f.stream == static_cast<const void *>(&cs2), "the hook forwarded the caller's exact stream pointer");
	check(f.cached_blob == blob_ptr, "the CachedPSO blob POINTER in the forwarded stream is the caller's (not copied, not dropped)");
	check(f.cached_size == blob_len, "the CachedPSO blob SIZE in the forwarded stream is the caller's");
	check(f.cs_found, "the compute shader is still found (census intact) alongside the cached blob");
	check(drain_validation(gpu, "pso-cache-passthrough") == 0, "no validation errors from the stream creates");
	return true;
}
