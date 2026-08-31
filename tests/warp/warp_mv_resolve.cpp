// Runs the real motion-vector resolve pass against WARP — Microsoft's software D3D12 device —
// with the debug layer and GPU-based validation enabled, and fails on any validation message.
//
// This exists because the failures on the target machine have been silent. vkd3d-proton in a
// release Proton build validates root parameter indices and types by `assert` only, so passing
// a root argument that does not match the bound root signature does not produce an error — it
// produces a wrong image, or a GPU that stops responding. The D3D12 debug layer says so
// immediately, and it runs on a CI machine with no GPU at all.
//
// What this can prove: that our own D3D12 usage is legal, and that resource allocation stays
// bounded. What it cannot prove: anything specific to vkd3d-proton, or the game's actual state.

#include "core/ring.hpp"
#include "core/view_params.hpp"
#include "d3d12_restore.hpp"
#include "mv_resolve.hpp"

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

int g_failures = 0;

void fail(const char *what)
{
	std::printf("  FAIL: %s\n", what);
	++g_failures;
}

void check(bool cond, const char *what)
{
	if (cond)
		std::printf("  ok: %s\n", what);
	else
		fail(what);
}

#define HR(expr)                                                                    \
	do {                                                                            \
		const HRESULT _hr = (expr);                                                 \
		if (FAILED(_hr)) {                                                          \
			std::printf("  FAIL: %s -> 0x%08x\n", #expr, static_cast<unsigned>(_hr)); \
			++g_failures;                                                           \
			return false;                                                           \
		}                                                                           \
	} while (0)

struct Gpu
{
	ComPtr<ID3D12Device> device;
	ComPtr<ID3D12CommandQueue> queue;
	ComPtr<ID3D12CommandAllocator> allocator;
	ComPtr<ID3D12GraphicsCommandList> list;
	ComPtr<ID3D12Fence> fence;
	ComPtr<ID3D12InfoQueue> info;
	UINT64 fence_value = 0;
};

// Which adapter to run against. WARP is the CI default: deterministic, GPU-less, and it comes
// with the debug layer. Passing --hardware selects the real adapter instead, which is how this
// same binary is run under Proton on the target box to exercise vkd3d-proton and the actual
// NVIDIA driver — the layer WARP cannot model.
bool g_use_hardware = false;

bool create_gpu(Gpu &gpu)
{
	// The debug layer is the entire point of this harness; without it WARP would merely run
	// our commands, not judge them.
	ComPtr<ID3D12Debug> debug;
	if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
	{
		debug->EnableDebugLayer();
		ComPtr<ID3D12Debug1> debug1;
		if (SUCCEEDED(debug.As(&debug1)))
		{
			// Catches descriptor and root-argument misuse that the basic layer misses.
			debug1->SetEnableGPUBasedValidation(TRUE);
		}
	}
	else
	{
		std::printf("  NOTE: D3D12 debug layer unavailable; validation coverage reduced\n");
	}

	ComPtr<IDXGIFactory4> factory;
	HR(CreateDXGIFactory2(DXGI_CREATE_FACTORY_DEBUG, IID_PPV_ARGS(&factory)));

	ComPtr<IDXGIAdapter> adapter;
	if (g_use_hardware)
	{
		// First adapter that yields a D3D12 device. Under vkd3d-proton this is the real GPU.
		for (UINT i = 0; factory->EnumAdapters(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i)
		{
			if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0,
					IID_PPV_ARGS(&gpu.device))))
				break;
			adapter.Reset();
		}
		if (!gpu.device)
		{
			std::printf("  no hardware D3D12 adapter available\n");
			return false;
		}

		DXGI_ADAPTER_DESC desc = {};
		if (adapter && SUCCEEDED(adapter->GetDesc(&desc)))
			std::printf("  adapter: %ls (vendor 0x%04x)\n", desc.Description, desc.VendorId);
	}
	else
	{
		HR(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)));
		HR(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&gpu.device)));
	}

	if (SUCCEEDED(gpu.device.As(&gpu.info)))
	{
		gpu.info->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, FALSE);
		gpu.info->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, FALSE);
	}

	D3D12_COMMAND_QUEUE_DESC qd = {};
	qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	HR(gpu.device->CreateCommandQueue(&qd, IID_PPV_ARGS(&gpu.queue)));
	HR(gpu.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&gpu.allocator)));
	HR(gpu.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, gpu.allocator.Get(),
		nullptr, IID_PPV_ARGS(&gpu.list)));
	HR(gpu.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gpu.fence)));
	return true;
}

// Drains the info queue, printing and counting anything at error severity or worse.
int drain_validation(Gpu &gpu, const char *phase)
{
	if (!gpu.info)
		return 0;

	int errors = 0;
	const UINT64 n = gpu.info->GetNumStoredMessages();
	for (UINT64 i = 0; i < n; ++i)
	{
		SIZE_T len = 0;
		gpu.info->GetMessage(i, nullptr, &len);
		std::vector<char> buf(len);
		auto *msg = reinterpret_cast<D3D12_MESSAGE *>(buf.data());
		if (FAILED(gpu.info->GetMessage(i, msg, &len)))
			continue;

		if (msg->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION ||
			msg->Severity == D3D12_MESSAGE_SEVERITY_ERROR)
		{
			std::printf("  [%s] D3D12 %s: %.*s\n", phase,
				msg->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION ? "CORRUPTION" : "ERROR",
				static_cast<int>(msg->DescriptionByteLength), msg->pDescription);
			++errors;
		}
	}
	gpu.info->ClearStoredMessages();
	return errors;
}

bool flush(Gpu &gpu)
{
	HR(gpu.list->Close());
	ID3D12CommandList *lists[] = { gpu.list.Get() };
	gpu.queue->ExecuteCommandLists(1, lists);

	++gpu.fence_value;
	HR(gpu.queue->Signal(gpu.fence.Get(), gpu.fence_value));
	while (gpu.fence->GetCompletedValue() < gpu.fence_value)
		Sleep(1);

	HR(gpu.allocator->Reset());
	HR(gpu.list->Reset(gpu.allocator.Get(), nullptr));
	return true;
}

// Stand-ins for the game's depth and velocity targets, with SRVs in a NON-shader-visible heap
// — which is where the real game's descriptors live, and what CopyDescriptorsSimple requires
// as its source.
struct GameResources
{
	ComPtr<ID3D12Resource> depth;
	ComPtr<ID3D12Resource> velocity;
	ComPtr<ID3D12DescriptorHeap> staging;
	D3D12_CPU_DESCRIPTOR_HANDLE depth_srv = {};
	D3D12_CPU_DESCRIPTOR_HANDLE velocity_srv = {};
};

bool create_game_resources(Gpu &gpu, GameResources &out, UINT w, UINT h)
{
	D3D12_HEAP_PROPERTIES def = {};
	def.Type = D3D12_HEAP_TYPE_DEFAULT;

	D3D12_RESOURCE_DESC d = {};
	d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	d.Width = w;
	d.Height = h;
	d.DepthOrArraySize = 1;
	d.MipLevels = 1;
	d.SampleDesc.Count = 1;

	// The real depth target is R32G8X24_TYPELESS, read through an R32_FLOAT_X8X24 SRV.
	d.Format = DXGI_FORMAT_R32G8X24_TYPELESS;
	d.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
	HR(gpu.device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &d,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&out.depth)));

	d.Format = DXGI_FORMAT_R16G16B16A16_UNORM;
	d.Flags = D3D12_RESOURCE_FLAG_NONE;
	HR(gpu.device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &d,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&out.velocity)));

	D3D12_DESCRIPTOR_HEAP_DESC hd = {};
	hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	hd.NumDescriptors = 2;
	hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE; // staging, exactly like UE4's offline heaps
	HR(gpu.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&out.staging)));

	const UINT inc = gpu.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	out.depth_srv = out.staging->GetCPUDescriptorHandleForHeapStart();
	out.velocity_srv = out.depth_srv;
	out.velocity_srv.ptr += inc;

	D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
	sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	sd.Texture2D.MipLevels = 1;

	sd.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
	gpu.device->CreateShaderResourceView(out.depth.Get(), &sd, out.depth_srv);

	sd.Format = DXGI_FORMAT_R16G16B16A16_UNORM;
	gpu.device->CreateShaderResourceView(out.velocity.Get(), &sd, out.velocity_srv);
	return true;
}

stray_dlss::ue4::ViewParams make_view(UINT w, UINT h)
{
	stray_dlss::ue4::ViewParams v;
	for (int i = 0; i < 4; ++i)
		v.clip_to_prev_clip.m[i * 4 + i] = 1.0f; // identity
	v.view_size_and_inv_size = { static_cast<float>(w), static_cast<float>(h),
	                             1.0f / w, 1.0f / h };
	v.buffer_size_and_inv_size = v.view_size_and_inv_size;
	v.view_rect_min = { 0, 0, 0, 0 };
	v.temporal_aa_params = { 3.0f, 32.0f, -0.25f, 0.125f };
	return v;
}

// ---- test 1: the pass is legal D3D12 -------------------------------------------------------

bool test_dispatch_is_valid(Gpu &gpu)
{
	std::printf("\n[test] resolve pass validates clean under the debug layer\n");

	constexpr UINT kW = 1920, kH = 1080;
	GameResources game;
	if (!create_game_resources(gpu, game, kW, kH))
		return false;

	drain_validation(gpu, "setup");

	if (!stray_dlss::mv::initialise(gpu.device.Get(), kW, kH))
	{
		fail(stray_dlss::mv::last_error());
		return false;
	}

	const auto view = make_view(kW, kH);
	stray_dlss::mv::ResolveInputs in;
	in.depth_descriptor = game.depth_srv.ptr;
	in.velocity_descriptor = game.velocity_srv.ptr;
	in.render_width = kW;
	in.render_height = kH;
	in.view = &view;

	check(stray_dlss::mv::record(gpu.list.Get(), in, 2), "record() issued the dispatch");
	if (!flush(gpu))
		return false;

	const int errors = drain_validation(gpu, "dispatch");
	check(errors == 0, "no D3D12 validation errors from the resolve pass");
	return true;
}

// ---- test 2: allocation stays bounded when the resolution flaps ----------------------------

bool test_no_allocation_churn(Gpu &gpu)
{
	std::printf("\n[test] flapping resolution does not grow GPU allocations without bound\n");

	// This is the failure that actually took the machine down: the render resolution comes from
	// whichever dispatch matched, so it can alternate, and every change reallocated the heap,
	// constant buffer and output texture. On the real run the GPU reported NV_ERR_NO_MEMORY and
	// then Xid 109 CTX SWITCH TIMEOUT.
	constexpr UINT kA = 1920, kB = 1280;

	GameResources a, b;
	if (!create_game_resources(gpu, a, kA, 1080) || !create_game_resources(gpu, b, kB, 720))
		return false;
	drain_validation(gpu, "churn-setup");

	const auto before = stray_dlss::mv::stats();

	for (int i = 0; i < 200; ++i)
	{
		const bool even = (i % 2) == 0;
		const UINT w = even ? kA : kB;
		const UINT h = even ? 1080u : 720u;
		const GameResources &g = even ? a : b;

		if (!stray_dlss::mv::initialise(gpu.device.Get(), w, h))
		{
			fail(stray_dlss::mv::last_error());
			return false;
		}

		const auto view = make_view(w, h);
		stray_dlss::mv::ResolveInputs in;
		in.depth_descriptor = g.depth_srv.ptr;
		in.velocity_descriptor = g.velocity_srv.ptr;
		in.render_width = w;
		in.render_height = h;
		in.view = &view;

		stray_dlss::mv::record(gpu.list.Get(), in, 1);

		if ((i % 20) == 19 && !flush(gpu))
			return false;
	}
	if (!flush(gpu))
		return false;

	const auto after = stray_dlss::mv::stats();
	std::printf("  created=%llu retired=%llu released=%llu live_retired=%u bytes_live=%llu\n",
		static_cast<unsigned long long>(after.resource_sets_created),
		static_cast<unsigned long long>(after.resource_sets_retired),
		static_cast<unsigned long long>(after.resource_sets_released),
		after.live_retired,
		static_cast<unsigned long long>(after.bytes_live));

	// Under a grow-only policy an alternating resolution must allocate a bounded number of
	// times, not once per change. Anything that scales with frame count is the bug that took
	// the machine down.
	const auto created = after.resource_sets_created - before.resource_sets_created;
	std::printf("  created %llu resource sets across 200 alternating frames\n",
		static_cast<unsigned long long>(created));

	check(created <= 4, "allocation count is bounded, not per-frame");
	check(after.live_retired <= 4, "retired sets stay few");
	check(after.bytes_live < 200ull * 1024 * 1024,
		"live GPU memory stays well under 200 MB");

	const int errors = drain_validation(gpu, "churn");
	check(errors == 0, "no D3D12 validation errors during churn");
	return true;
}


// ---- test 3: the restore puts the game's state back exactly ------------------------------
//
// This is the corruption test. A mock "game" dispatch reads a root constant, a root CBV and a
// descriptor-table SRV, and writes them out. Run it once to get a golden result; then run it
// again with our clobber and restore in between. If the restore is wrong, the second result
// differs — which is what "the image went purple" looks like when reduced to numbers.

#include <warp_probe.h>

struct Probe
{
	ComPtr<ID3D12RootSignature> rs;
	ComPtr<ID3D12PipelineState> pso;
	ComPtr<ID3D12DescriptorHeap> gpu_heap;   // shader-visible: the "game's" heap
	ComPtr<ID3D12Resource> cbv;              // root CBV contents
	ComPtr<ID3D12Resource> source;           // texture read through the table
	ComPtr<ID3D12Resource> out;              // UAV the probe writes
	ComPtr<ID3D12Resource> readback;
	D3D12_GPU_DESCRIPTOR_HANDLE table = {};
};

bool build_probe(Gpu &gpu, Probe &p)
{
	// Root layout on purpose mirrors UE4's shape: constants, a root CBV, and a table.
	D3D12_DESCRIPTOR_RANGE ranges[2] = {};
	ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	ranges[0].NumDescriptors = 1;
	ranges[0].BaseShaderRegister = 0;
	ranges[0].OffsetInDescriptorsFromTableStart = 0;
	ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
	ranges[1].NumDescriptors = 1;
	ranges[1].BaseShaderRegister = 0;
	ranges[1].OffsetInDescriptorsFromTableStart = 1;

	D3D12_ROOT_PARAMETER params[3] = {};
	params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
	params[0].Constants.ShaderRegister = 0;
	params[0].Constants.Num32BitValues = 4;
	params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
	params[1].Descriptor.ShaderRegister = 1;
	params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	params[2].DescriptorTable.NumDescriptorRanges = 2;
	params[2].DescriptorTable.pDescriptorRanges = ranges;

	D3D12_ROOT_SIGNATURE_DESC rd = {};
	rd.NumParameters = 3;
	rd.pParameters = params;

	ComPtr<ID3DBlob> blob, err;
	HR(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err));
	HR(gpu.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
		IID_PPV_ARGS(&p.rs)));

	D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
	pd.pRootSignature = p.rs.Get();
	pd.CS.pShaderBytecode = g_warp_probe_dxbc;
	pd.CS.BytecodeLength = sizeof(g_warp_probe_dxbc);
	HR(gpu.device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&p.pso)));

	D3D12_HEAP_PROPERTIES def = {}; def.Type = D3D12_HEAP_TYPE_DEFAULT;
	D3D12_HEAP_PROPERTIES up = {};  up.Type = D3D12_HEAP_TYPE_UPLOAD;
	D3D12_HEAP_PROPERTIES rb = {};  rb.Type = D3D12_HEAP_TYPE_READBACK;

	D3D12_RESOURCE_DESC bd = {};
	bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	bd.Width = 256; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
	bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	HR(gpu.device->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd,
		D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&p.cbv)));

	// A known value in the root CBV, so a lost binding is visible in the output.
	{
		void *m = nullptr; D3D12_RANGE nr = {0,0};
		HR(p.cbv->Map(0, &nr, &m));
		const UINT v[4] = { 0xABCD1234u, 0, 0, 0 };
		memcpy(m, v, sizeof(v));
		p.cbv->Unmap(0, nullptr);
	}

	bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
	HR(gpu.device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &bd,
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&p.out)));

	bd.Flags = D3D12_RESOURCE_FLAG_NONE;
	HR(gpu.device->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd,
		D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&p.readback)));

	D3D12_RESOURCE_DESC td = {};
	td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	td.Width = 4; td.Height = 4; td.DepthOrArraySize = 1; td.MipLevels = 1;
	td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; td.SampleDesc.Count = 1;
	HR(gpu.device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&p.source)));

	D3D12_DESCRIPTOR_HEAP_DESC hd = {};
	hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	hd.NumDescriptors = 2;
	hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	HR(gpu.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&p.gpu_heap)));

	const UINT inc = gpu.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	D3D12_CPU_DESCRIPTOR_HANDLE c = p.gpu_heap->GetCPUDescriptorHandleForHeapStart();

	D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
	sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	sd.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
	sd.Texture2D.MipLevels = 1;
	gpu.device->CreateShaderResourceView(p.source.Get(), &sd, c);

	c.ptr += inc;
	D3D12_UNORDERED_ACCESS_VIEW_DESC ud = {};
	ud.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
	ud.Format = DXGI_FORMAT_UNKNOWN;
	ud.Buffer.NumElements = 4;
	ud.Buffer.StructureByteStride = 16;
	gpu.device->CreateUnorderedAccessView(p.out.Get(), nullptr, &ud, c);

	p.table = p.gpu_heap->GetGPUDescriptorHandleForHeapStart();
	return true;
}

// Binds the probe's state exactly as a game would, then dispatches.
void bind_probe_state(Gpu &gpu, Probe &p, UINT marker)
{
	ID3D12DescriptorHeap *heaps[] = { p.gpu_heap.Get() };
	gpu.list->SetDescriptorHeaps(1, heaps);
	gpu.list->SetComputeRootSignature(p.rs.Get());
	const UINT consts[4] = { marker, 0, 0, 0 };
	gpu.list->SetComputeRoot32BitConstants(0, 4, consts, 0);
	gpu.list->SetComputeRootConstantBufferView(1, p.cbv->GetGPUVirtualAddress());
	gpu.list->SetComputeRootDescriptorTable(2, p.table);
	gpu.list->SetPipelineState(p.pso.Get());
}

bool read_probe(Gpu &gpu, Probe &p, UINT out[4])
{
	D3D12_RESOURCE_BARRIER b = {};
	b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	b.Transition.pResource = p.out.Get();
	b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
	b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
	b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	gpu.list->ResourceBarrier(1, &b);
	gpu.list->CopyBufferRegion(p.readback.Get(), 0, p.out.Get(), 0, 16);
	std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
	gpu.list->ResourceBarrier(1, &b);

	if (!flush(gpu))
		return false;

	void *m = nullptr;
	D3D12_RANGE r = { 0, 16 };
	HR(p.readback->Map(0, &r, &m));
	memcpy(out, m, 16);
	p.readback->Unmap(0, nullptr);
	return true;
}

bool test_restore_preserves_game_state(Gpu &gpu)
{
	std::printf("\n[test] our clobber + restore leaves the game's dispatch unchanged\n");

	Probe p;
	if (!build_probe(gpu, p))
		return false;

	GameResources game;
	if (!create_game_resources(gpu, game, 1920, 1080))
		return false;
	if (!stray_dlss::mv::initialise(gpu.device.Get(), 1920, 1080))
		return false;
	drain_validation(gpu, "probe-setup");

	// GOLDEN: the game's dispatch with nothing of ours in the way.
	UINT golden[4] = {};
	bind_probe_state(gpu, p, 0x1111u);
	gpu.list->Dispatch(1, 1, 1);
	if (!read_probe(gpu, p, golden))
		return false;
	std::printf("  golden  = %08X %08X %08X %08X\n", golden[0], golden[1], golden[2], golden[3]);

	// NOW: bind the game state, clobber it with our pass, restore, then dispatch.
	bind_probe_state(gpu, p, 0x1111u);

	const auto view = make_view(1920, 1080);
	stray_dlss::mv::ResolveInputs in;
	in.depth_descriptor = game.depth_srv.ptr;
	in.velocity_descriptor = game.velocity_srv.ptr;
	in.render_width = 1920;
	in.render_height = 1080;
	in.view = &view;
	stray_dlss::mv::record(gpu.list.Get(), in, 2);

	// The restore, exactly as the add-on performs it: heaps first (the ReShade half, done here
	// natively because the harness has no ReShade), then the native replay under test.
	ID3D12DescriptorHeap *heaps[] = { p.gpu_heap.Get() };
	gpu.list->SetDescriptorHeaps(1, heaps);

	stray_dlss::NativeComputeState snap;
	snap.root_signature = p.rs.Get();
	snap.tables = { 0, 0, p.table.ptr };            // params 0 and 1 are NOT tables
	snap.root_cbv = { { 1u, p.cbv->GetGPUVirtualAddress() } };
	snap.root_constants = { { 0u, { 0x1111u, 0, 0, 0 } } };
	snap.pso = p.pso.Get();
	stray_dlss::restore_native_compute_state(gpu.list.Get(), snap);

	UINT after[4] = {};
	gpu.list->Dispatch(1, 1, 1);
	if (!read_probe(gpu, p, after))
		return false;
	std::printf("  restored= %08X %08X %08X %08X\n", after[0], after[1], after[2], after[3]);

	check(memcmp(golden, after, sizeof(golden)) == 0,
		"the game's dispatch produces identical output after our clobber and restore");
	check(drain_validation(gpu, "probe") == 0, "no validation errors across clobber and restore");
	return true;
}

} // namespace

int main(int argc, char **argv)
{
	for (int i = 1; i < argc; ++i)
		if (std::string(argv[i]) == "--hardware")
			g_use_hardware = true;

	std::printf("D3D12 harness for the motion-vector resolve pass (%s)\n",
		g_use_hardware ? "HARDWARE adapter" : "WARP software adapter");

	Gpu gpu;
	if (!create_gpu(gpu))
	{
		std::printf("could not create a WARP device\n");
		return 1;
	}
	std::printf("device up, info queue %s\n", gpu.info ? "active" : "UNAVAILABLE");

	test_dispatch_is_valid(gpu);
	test_no_allocation_churn(gpu);
	test_restore_preserves_game_state(gpu);

	stray_dlss::mv::shutdown();
	drain_validation(gpu, "shutdown");

	std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "PASS" : "FAIL",
		g_failures, g_failures == 1 ? "" : "s");
	return g_failures == 0 ? 0 : 1;
}
