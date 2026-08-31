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

	ComPtr<IDXGIAdapter> warp;
	HR(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)));
	HR(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&gpu.device)));

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

	// 200 alternating frames must not leave 200 sets of resources alive. A handful in flight is
	// expected and correct; unbounded growth is the bug.
	check(after.live_retired <= 2 * stray_dlss::ring::kFrameCount,
		"retired resources are released rather than accumulating");
	check(after.bytes_live < 200ull * 1024 * 1024,
		"live GPU memory stays well under 200 MB");

	const auto created = after.resource_sets_created - before.resource_sets_created;
	std::printf("  NOTE: %llu resource sets created across 200 alternating frames\n",
		static_cast<unsigned long long>(created));

	const int errors = drain_validation(gpu, "churn");
	check(errors == 0, "no D3D12 validation errors during churn");
	return true;
}

} // namespace

int main()
{
	std::printf("WARP harness for the motion-vector resolve pass\n");

	Gpu gpu;
	if (!create_gpu(gpu))
	{
		std::printf("could not create a WARP device\n");
		return 1;
	}
	std::printf("WARP device up, debug layer %s\n", gpu.info ? "active" : "UNAVAILABLE");

	test_dispatch_is_valid(gpu);
	test_no_allocation_churn(gpu);

	stray_dlss::mv::shutdown();
	drain_validation(gpu, "shutdown");

	std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "PASS" : "FAIL",
		g_failures, g_failures == 1 ? "" : "s");
	return g_failures == 0 ? 0 : 1;
}
