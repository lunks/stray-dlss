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

#include "core/fnv1a.hpp"
#include "core/ring.hpp"
#include "core/view_params.hpp"
#include "d3d12_restore.hpp"
#include "reshade_bindings.hpp"

#include "fake_reshade_command_list.hpp"

#include <state_tracking.hpp>
#include "mv_resolve.hpp"

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
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

// Set by --expect-reshade. When ReShade's DLL is dropped in beside this exe as d3d12.dll, our
// D3D12CreateDevice goes through its proxy, so EVERY test in this file then runs against
// ReShade's real device and command-list proxies — its descriptor-heap wrappers and its
// handle conversion — rather than straight at the runtime. That is the one part of the add-on's
// environment we otherwise cannot reach offline, so CI asserts it really happened rather than
// silently testing the plain runtime.
bool g_expect_reshade = false;

// True when the d3d12.dll we bound to is ReShade's proxy. It exports the add-on entry points;
// the real runtime does not.
bool running_under_reshade()
{
	const HMODULE d3d12 = ::GetModuleHandleW(L"d3d12.dll");
	return d3d12 != nullptr && ::GetProcAddress(d3d12, "ReShadeRegisterAddon") != nullptr;
}

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
// `expected` flips the reporting for the negative-control test, where an error is the
// desired outcome and its ID is the thing worth recording.
int drain_validation(Gpu &gpu, const char *phase, bool expected = false)
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
			std::printf("  [%s] %sD3D12 %s #%d: %.*s\n", phase, expected ? "EXPECTED " : "",
				msg->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION ? "CORRUPTION" : "ERROR",
				static_cast<int>(msg->ID),
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

// Throws the list's recorded commands away WITHOUT executing them, then reopens it. For a
// list that binds descriptors of a resource the test has already destroyed: executing that
// (even with no dispatch) removed WARP's device — DXGI_ERROR_DEVICE_REMOVED 0x887a0005 on the
// next creation call, measured in CI 2026-09-02 — and the removal only surfaced in a LATER
// test, because nothing else touched the device before the harness exited.
bool discard(Gpu &gpu)
{
	HR(gpu.list->Close());
	HR(gpu.allocator->Reset());
	HR(gpu.list->Reset(gpu.allocator.Get(), nullptr));
	return true;
}

// A plain default-heap buffer, for the object-model probes below. Nothing is ever written to
// it; only its identity and its vtable matter.
ComPtr<ID3D12Resource> create_buffer(Gpu &gpu, UINT64 size,
	D3D12_HEAP_TYPE heap_type = D3D12_HEAP_TYPE_DEFAULT)
{
	D3D12_HEAP_PROPERTIES hp = {};
	hp.Type = heap_type;
	D3D12_RESOURCE_DESC bd = {};
	bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	bd.Width = size; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
	bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	const D3D12_RESOURCE_STATES state = heap_type == D3D12_HEAP_TYPE_UPLOAD
		? D3D12_RESOURCE_STATE_GENERIC_READ
		: heap_type == D3D12_HEAP_TYPE_READBACK ? D3D12_RESOURCE_STATE_COPY_DEST
		                                        : D3D12_RESOURCE_STATE_COMMON;
	ComPtr<ID3D12Resource> res;
	if (FAILED(gpu.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, state,
			nullptr, IID_PPV_ARGS(&res))))
		return nullptr;
	return res;
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

// A minimal View for tests that only care whether the pass is LEGAL.
//
// ClipToPrevClip is the IDENTITY here, which is deliberately not enough to judge the camera
// branch's arithmetic: the identity is the one matrix whose transpose is itself, so it cannot
// tell mul(v, M) from mul(M, v). That blind spot is exactly how a missing `row_major` qualifier
// shipped. Test 3 below overrides this member with a matrix that does distinguish them.
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
	in.depth_resource = reinterpret_cast<std::uint64_t>(game.depth.Get());
	in.velocity_resource = reinterpret_cast<std::uint64_t>(game.velocity.Get());
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
		in.depth_resource = reinterpret_cast<std::uint64_t>(g.depth.Get());
		in.velocity_resource = reinterpret_cast<std::uint64_t>(g.velocity.Get());
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


// ---- test 3: the camera branch applies ClipToPrevClip, not its transpose --------------------
//
// The bug this pins, and it shipped: shaders/mv_resolve.hlsl declared `float4x4 ClipToPrevClip`
// with no `row_major` qualifier while cmake/CompileShaders.cmake invokes fxc with neither /Zpr
// nor /Zpc. HLSL packs constant-buffer matrices COLUMN-major by default, so fxc read our
// row-major upload as columns and `mul(v, M)` computed `v * transpose(M)`. UE 4.27 compiles its
// own shaders with D3D10_SHADER_PACK_MATRIX_ROW_MAJOR (D3DShaderCompiler.cpp:947-949) and
// src/mv_resolve.cpp memcpys those 16 floats verbatim, so the transpose was ours alone.
//
// Why every existing test missed it, which is the gap this closes:
//   * make_view() sets ClipToPrevClip to the IDENTITY, and the identity is the one matrix
//     whose transpose is itself — the single blind spot a transpose test can have;
//   * nothing in this harness read the motion-vector texture back at all, so even a
//     non-identity matrix would have gone unjudged.
//
// So: a matrix carrying BOTH a camera yaw and a translation, every pixel forced down the
// camera-reconstruction branch, mv::output() read back, and each sample compared against a
// scalar transcription of the shader. On a mismatch the transposed reference is evaluated too
// and reported, so a future regression reads as "the matrix is transposed again" rather than
// "the numbers are wrong".

using stray_dlss::ue4::Matrix4;

// The test's own render rect. 200 texels of R16G16_FLOAT is 800 bytes, which is NOT a multiple
// of D3D12_TEXTURE_DATA_PITCH_ALIGNMENT — deliberately, so the readback below really exercises
// the row padding rather than getting away with a tightly packed copy.
constexpr UINT kMvW = 200;
constexpr UINT kMvH = 120;

// UE's default near plane. §2.4 says to read View.NearPlane rather than assume 10 uu; this is
// the test's own synthetic camera, so it picks one and the reference uses the same number.
constexpr float kMvNearPlane = 10.0f;

// The constant device-Z the whole depth target is cleared to. UE 4.27 is reversed-Z with an
// infinite far plane, so DeviceZ = Near / ViewZ: 1.0 at the near plane, 0.0 at infinity
// (CLAUDE.md §2.4). 0.02 against a 10-unit near plane puts the plane 500 unreal units out —
// clear of both ends, so neither a degenerate w nor a divide by zero can flatter the result.
constexpr float kMvDeviceZ = 0.02f;

// Row-vector convention throughout: v' = v * M, storage row-major with m[r*4+c] == M[r][c].
// That is FMatrix's own layout and what the shader sees once `row_major` is present.
// (core/view_params.hpp Matrix4, docs/RESEARCH.md §4.7)
Matrix4 mat_mul(const Matrix4 &a, const Matrix4 &b)
{
	Matrix4 out;
	for (int r = 0; r < 4; ++r)
		for (int c = 0; c < 4; ++c)
		{
			float acc = 0.0f;
			for (int k = 0; k < 4; ++k)
				acc += a.m[r * 4 + k] * b.m[k * 4 + c];
			out.m[r * 4 + c] = acc;
		}
	return out;
}

// A ClipToPrevClip built the way UE builds it: clip -> view -> previous view -> clip, composed
// under the row-vector convention. Every term is named so a future reader can check it rather
// than trust it.
Matrix4 make_clip_to_prev_clip()
{
	// Projection, row-vector: p_clip = p_view * P. Reversed-Z with an infinite far plane is
	//     x' = sx*x     y' = sy*y     z' = Near*w     w' = z
	// so DeviceZ = z'/w' = Near/z, which is exactly the relation §2.4 records.
	const float sx = 1.0f; // 45-degree horizontal half-FOV: 1/tan(45) == 1
	const float sy = static_cast<float>(kMvW) / static_cast<float>(kMvH) * sx;
	Matrix4 proj;
	proj.m[0 * 4 + 0] = sx;
	proj.m[1 * 4 + 1] = sy;
	proj.m[2 * 4 + 3] = 1.0f;          // w' = z
	proj.m[3 * 4 + 2] = kMvNearPlane;  // z' = Near * w

	// Its exact inverse: x = X/sx, y = Y/sy, z = W, w = Z/Near. Non-symmetric on its own, so a
	// transpose cannot hide anywhere in the chain.
	Matrix4 proj_inv;
	proj_inv.m[0 * 4 + 0] = 1.0f / sx;
	proj_inv.m[1 * 4 + 1] = 1.0f / sy;
	proj_inv.m[3 * 4 + 2] = 1.0f;                  // view z comes from the clip w
	proj_inv.m[2 * 4 + 3] = 1.0f / kMvNearPlane;   // view w comes from the clip z

	// The camera's own motion between the two frames, in view space: a small yaw about Y AND a
	// translation. Both are required and neither alone is sufficient.
	//   * a yaw-only matrix, transposed, inverts the sign of the rotation and roughly halves
	//     the magnitude — visible, but only in one term;
	//   * a translation-only matrix collapses to EXACTLY zero, because a row-vector
	//     translation lives in ROW 3 and the transpose moves it into column 3, where it
	//     perturbs w and nothing else.
	// Only a matrix carrying both proves the whole transform.
	const float yaw = 0.02f; // ~1.15 degrees of pan in a single frame
	const float cos_yaw = std::cos(yaw);
	const float sin_yaw = std::sin(yaw);
	Matrix4 view_to_prev;
	view_to_prev.m[0 * 4 + 0] = cos_yaw;
	view_to_prev.m[0 * 4 + 2] = -sin_yaw;
	view_to_prev.m[1 * 4 + 1] = 1.0f;
	view_to_prev.m[2 * 4 + 0] = sin_yaw;
	view_to_prev.m[2 * 4 + 2] = cos_yaw;
	// Translation in ROW 3 — the row-vector convention's tell, and the term the transpose
	// destroys. Unreal units of camera movement in one frame: right, down, forward.
	view_to_prev.m[3 * 4 + 0] = 1.5f;
	view_to_prev.m[3 * 4 + 1] = -4.0f;
	view_to_prev.m[3 * 4 + 2] = -3.0f;
	view_to_prev.m[3 * 4 + 3] = 1.0f;

	// p_prevclip = p_clip * ProjInv * ViewToPrevView * Proj.
	return mat_mul(mat_mul(proj_inv, view_to_prev), proj);
}

// IEEE binary16 -> binary32. Deliberately hand-written rather than pulled in from anywhere:
// mv::output() is R16G16_FLOAT and this is the only place in the project that has to decode
// one, so a dependency would cost more than the twelve lines.
float half_to_float(std::uint16_t h)
{
	const bool negative = (h & 0x8000u) != 0;
	const int exponent = static_cast<int>((h >> 10) & 0x1Fu);
	const int mantissa = static_cast<int>(h & 0x03FFu);

	float v;
	if (exponent == 0)
		v = static_cast<float>(mantissa) * 5.9604645e-8f; // subnormal: mantissa * 2^-24
	else if (exponent == 31)
		v = mantissa != 0 ? std::numeric_limits<float>::quiet_NaN()
		                  : std::numeric_limits<float>::infinity();
	else
		v = std::ldexp(static_cast<float>(mantissa + 1024), exponent - 25); // (1+m/1024)*2^(e-15)

	return negative ? -v : v;
}

// A scalar transcription of the camera branch of shaders/mv_resolve.hlsl, written against the
// shader statement by statement. If the two ever disagree the SHADER is the specification and
// this is what has to change — the point of writing it out is that a silent divergence becomes
// a failing test rather than a wrong image.
struct Mv
{
	float x = 0.0f;
	float y = 0.0f;
};

Mv reference_camera_mv(UINT ix, UINT iy, const Matrix4 &clip_to_prev, bool transposed)
{
	const float w = static_cast<float>(kMvW);
	const float h = static_cast<float>(kMvH);

	// uv = (float2(tid.xy) + 0.5f) / RenderSize
	const float u = (static_cast<float>(ix) + 0.5f) / w;
	const float v = (static_cast<float>(iy) + 0.5f) / h;
	// ViewportUVToScreenPos: (2u - 1, 1 - 2v). NDC, Y-up.
	const float screen_x = 2.0f * u - 1.0f;
	const float screen_y = 1.0f - 2.0f * v;

	// this_clip = float4(screen_pos, device_z, 1.0f)
	const float in4[4] = { screen_x, screen_y, kMvDeviceZ, 1.0f };

	float prev[4] = {};
	for (int i = 0; i < 4; ++i)
	{
		float acc = 0.0f;
		for (int k = 0; k < 4; ++k)
		{
			// mul(v, M) — row-vector, the correct form: prev[c] = sum_r v[r] * M[r][c].
			// mul(M, v) — what the missing row_major qualifier produced, and what
			// LegacyTransposedClip reproduces: prev[r] = sum_c M[r][c] * v[c].
			acc += transposed ? clip_to_prev.m[i * 4 + k] * in4[k]
			                  : in4[k] * clip_to_prev.m[k * 4 + i];
		}
		prev[i] = acc;
	}

	float ndc_x = 0.0f, ndc_y = 0.0f;
	if (prev[3] > 0.0f)
	{
		// prev_screen = prev_clip.xy / prev_clip.w; velocity_ndc = screen_pos - prev_screen.
		// CameraSign is (1,1) here — see set_signs() in the test body.
		ndc_x = screen_x - prev[0] / prev[3];
		ndc_y = screen_y - prev[1] / prev[3];
	}
	// mv_pixels = velocity_ndc * float2(0.5*W, -0.5*H); OutMV = -mv_pixels.
	Mv out;
	out.x = -(ndc_x * 0.5f * w);
	out.y = -(ndc_y * -0.5f * h);
	return out;
}

// R16G16_FLOAT keeps 11 bits of mantissa, so the readback's own error is RELATIVE (~5e-4) and
// the absolute floor only matters near zero. Both terms, so a 5-pixel vector is not judged by a
// tolerance sized for a 0.05-pixel one, and neither is the reverse.
float mv_tolerance(float expected)
{
	return 0.01f + std::fabs(expected) * 1e-3f;
}

// Depth and velocity for the camera-branch test, both filled with KNOWN contents. The harness's
// create_game_resources() leaves them undefined, which is fine when only legality is being
// judged and useless when the pixels are.
//
// Velocity is cleared to zero so `encoded.x > 0.0f` is false everywhere and every pixel takes
// the camera-reconstruction branch — which is also the real game's situation for all static
// geometry, since UE 4.27 writes velocity only for movable primitives (§2.5). It carries
// ALLOW_RENDER_TARGET, unlike create_game_resources()'s copy, purely so it can be cleared; the
// game's own velocity target is a render target anyway.
struct CameraScene
{
	ComPtr<ID3D12Resource> depth;
	ComPtr<ID3D12Resource> velocity;
	ComPtr<ID3D12DescriptorHeap> dsv;
	ComPtr<ID3D12DescriptorHeap> rtv;
};

bool build_camera_scene(Gpu &gpu, CameraScene &s, UINT w, UINT h)
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

	// The game's depth target: R32G8X24_TYPELESS, read through an R32_FLOAT_X8X24 SRV (§2.4).
	// An optimized clear value is supplied so the clear below does not trip the debug layer's
	// mismatched-clear-value warning.
	d.Format = DXGI_FORMAT_R32G8X24_TYPELESS;
	d.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
	D3D12_CLEAR_VALUE depth_clear = {};
	depth_clear.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
	depth_clear.DepthStencil.Depth = kMvDeviceZ;
	HR(gpu.device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &d,
		D3D12_RESOURCE_STATE_DEPTH_WRITE, &depth_clear, IID_PPV_ARGS(&s.depth)));

	d.Format = DXGI_FORMAT_R16G16B16A16_UNORM;
	d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
	D3D12_CLEAR_VALUE velocity_clear = {};
	velocity_clear.Format = DXGI_FORMAT_R16G16B16A16_UNORM;
	HR(gpu.device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &d,
		D3D12_RESOURCE_STATE_RENDER_TARGET, &velocity_clear, IID_PPV_ARGS(&s.velocity)));

	D3D12_DESCRIPTOR_HEAP_DESC hd = {};
	hd.NumDescriptors = 1;
	hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
	HR(gpu.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&s.dsv)));
	hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
	HR(gpu.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&s.rtv)));

	D3D12_DEPTH_STENCIL_VIEW_DESC dsvd = {};
	dsvd.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
	dsvd.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
	const D3D12_CPU_DESCRIPTOR_HANDLE dsv = s.dsv->GetCPUDescriptorHandleForHeapStart();
	gpu.device->CreateDepthStencilView(s.depth.Get(), &dsvd, dsv);

	D3D12_RENDER_TARGET_VIEW_DESC rtvd = {};
	rtvd.Format = DXGI_FORMAT_R16G16B16A16_UNORM;
	rtvd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
	const D3D12_CPU_DESCRIPTOR_HANDLE rtv = s.rtv->GetCPUDescriptorHandleForHeapStart();
	gpu.device->CreateRenderTargetView(s.velocity.Get(), &rtvd, rtv);

	gpu.list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, kMvDeviceZ, 0, 0, nullptr);
	const float zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	gpu.list->ClearRenderTargetView(rtv, zero, 0, nullptr);

	D3D12_RESOURCE_BARRIER to_srv[2] = {};
	for (auto &b : to_srv)
	{
		b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		b.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
	}
	to_srv[0].Transition.pResource = s.depth.Get();
	to_srv[0].Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
	to_srv[1].Transition.pResource = s.velocity.Get();
	to_srv[1].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
	gpu.list->ResourceBarrier(2, to_srv);

	return flush(gpu);
}

// Reads mv::output() back as interleaved (x, y) floats, w*h pairs.
//
// Unlike read_probe() above, which copies a BUFFER and so has no layout to get wrong, a texture
// copied into a buffer must have 256-byte-aligned rows (D3D12_TEXTURE_DATA_PITCH_ALIGNMENT):
// 200 texels of R16G16_FLOAT is 800 bytes of data in a 1024-byte row. Reading it as though it
// were tightly packed shears the image and would make every sample below wrong in a way that
// looks like a shader bug.
//
// The source may be LARGER than w*h — mv_resolve allocates grow-only and an earlier test in
// this file will already have taken it to 1920x1080 — so the copy is bounded by an explicit box.
bool read_mv_texture(Gpu &gpu, UINT w, UINT h, std::vector<float> &out)
{
	ID3D12Resource *const mv = stray_dlss::mv::output();
	if (mv == nullptr)
	{
		fail("mv::output() is null, so there is nothing to read back");
		return false;
	}

	constexpr UINT kPitchAlign = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
	const UINT row_bytes = w * 4; // R16G16_FLOAT
	const UINT row_pitch = ((row_bytes + kPitchAlign - 1) / kPitchAlign) * kPitchAlign;
	const UINT64 total = static_cast<UINT64>(row_pitch) * h;

	D3D12_HEAP_PROPERTIES rb = {};
	rb.Type = D3D12_HEAP_TYPE_READBACK;
	D3D12_RESOURCE_DESC bd = {};
	bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	bd.Width = total;
	bd.Height = 1;
	bd.DepthOrArraySize = 1;
	bd.MipLevels = 1;
	bd.SampleDesc.Count = 1;
	bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	ComPtr<ID3D12Resource> readback;
	HR(gpu.device->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd,
		D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)));

	D3D12_RESOURCE_BARRIER barrier = {};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Transition.pResource = mv;
	barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
	gpu.list->ResourceBarrier(1, &barrier);

	D3D12_TEXTURE_COPY_LOCATION dst = {};
	dst.pResource = readback.Get();
	dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	dst.PlacedFootprint.Offset = 0;
	dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R16G16_FLOAT;
	dst.PlacedFootprint.Footprint.Width = w;
	dst.PlacedFootprint.Footprint.Height = h;
	dst.PlacedFootprint.Footprint.Depth = 1;
	dst.PlacedFootprint.Footprint.RowPitch = row_pitch;

	D3D12_TEXTURE_COPY_LOCATION src = {};
	src.pResource = mv;
	src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	src.SubresourceIndex = 0;

	const D3D12_BOX box = { 0, 0, 0, w, h, 1 };
	gpu.list->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);

	// Back to UNORDERED_ACCESS: every other test in this file assumes that is where the
	// resolve's output lives.
	std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
	gpu.list->ResourceBarrier(1, &barrier);

	if (!flush(gpu))
		return false;

	void *mapped = nullptr;
	const D3D12_RANGE range = { 0, static_cast<SIZE_T>(total) };
	HR(readback->Map(0, &range, &mapped));

	const auto *const bytes = static_cast<const std::uint8_t *>(mapped);
	out.assign(static_cast<std::size_t>(w) * h * 2, 0.0f);
	for (UINT y = 0; y < h; ++y)
	{
		const auto *const row = reinterpret_cast<const std::uint16_t *>(
			bytes + static_cast<std::size_t>(y) * row_pitch);
		for (UINT x = 0; x < w; ++x)
		{
			const std::size_t i = (static_cast<std::size_t>(y) * w + x) * 2;
			out[i + 0] = half_to_float(row[x * 2 + 0]);
			out[i + 1] = half_to_float(row[x * 2 + 1]);
		}
	}
	readback->Unmap(0, nullptr);
	return true;
}

// Spread across the image on purpose. The transpose error grows with distance from the centre
// — screen_pos is (0,0) there, so most of the matrix multiplies zero — and a centre-only sample
// is the second-weakest test possible after an identity matrix.
struct MvSample
{
	UINT x;
	UINT y;
	const char *where;
};

constexpr MvSample kMvSamples[] = {
	{ kMvW / 2, kMvH / 2, "centre" },
	{ 0, 0, "top-left" },
	{ kMvW - 1, 0, "top-right" },
	{ 0, kMvH - 1, "bottom-left" },
	{ kMvW - 1, kMvH - 1, "bottom-right" },
	{ 37, 101, "lower-left quadrant" },
	{ 163, 18, "upper-right quadrant" },
};

// Compares one readback against the reference and, on a mismatch, says whether the TRANSPOSED
// reference explains it. That is the whole diagnostic value of this test: it turns "the numbers
// are wrong" into "the matrix is transposed again".
bool compare_against_reference(const std::vector<float> &got, const Matrix4 &clip_to_prev,
	bool expect_transposed, const char *phase)
{
	bool all_ok = true;
	for (const MvSample &sp : kMvSamples)
	{
		const std::size_t i = (static_cast<std::size_t>(sp.y) * kMvW + sp.x) * 2;
		const float gx = got[i + 0], gy = got[i + 1];

		const Mv want = reference_camera_mv(sp.x, sp.y, clip_to_prev, expect_transposed);
		const Mv other = reference_camera_mv(sp.x, sp.y, clip_to_prev, !expect_transposed);

		const bool ok = std::fabs(gx - want.x) <= mv_tolerance(want.x) &&
		                std::fabs(gy - want.y) <= mv_tolerance(want.y);
		if (ok)
			continue;

		all_ok = false;
		std::printf("  MISMATCH [%s] %s (%u,%u): got (% .5f, % .5f), expected (% .5f, % .5f)\n",
			phase, sp.where, sp.x, sp.y, static_cast<double>(gx), static_cast<double>(gy),
			static_cast<double>(want.x), static_cast<double>(want.y));

		const bool matches_other = std::fabs(gx - other.x) <= mv_tolerance(other.x) &&
		                           std::fabs(gy - other.y) <= mv_tolerance(other.y);
		if (matches_other && !expect_transposed)
		{
			std::printf("    ...and it MATCHES the TRANSPOSED reference (% .5f, % .5f).\n"
				"    ClipToPrevClip is reaching the shader transposed again. Look first at the\n"
				"    `row_major` qualifier on the ClipToPrevClip declaration in\n"
				"    shaders/mv_resolve.hlsl, then at cmake/CompileShaders.cmake for a /Zpc, then\n"
				"    at the memcpy in mv_resolve.cpp's record().\n",
				static_cast<double>(other.x), static_cast<double>(other.y));
		}
		else if (matches_other)
		{
			std::printf("    ...and it MATCHES the NON-transposed reference (% .5f, % .5f), so\n"
				"    LegacyTransposedClip is not reaching the shader.\n",
				static_cast<double>(other.x), static_cast<double>(other.y));
		}
		else
		{
			std::printf("    ...and it matches NEITHER orientation (the other is % .5f, % .5f),\n"
				"    so this is some other change to the camera branch, not a transpose.\n",
				static_cast<double>(other.x), static_cast<double>(other.y));
		}
	}
	return all_ok;
}

bool test_camera_branch_matrix_orientation(Gpu &gpu)
{
	std::printf("\n[test] the camera branch applies ClipToPrevClip, not its transpose\n");

	CameraScene scene;
	if (!build_camera_scene(gpu, scene, kMvW, kMvH))
		return false;
	drain_validation(gpu, "camera-branch-setup");

	if (!stray_dlss::mv::initialise(gpu.device.Get(), kMvW, kMvH))
	{
		fail(stray_dlss::mv::last_error());
		return false;
	}

	// Both per-branch signs at their shipped defaults, so this measures the matrix and nothing
	// else; set_signs() is otherwise a live tunable and a previous test could have moved it.
	stray_dlss::mv::set_signs(1.0f, 1.0f, 1.0f, 1.0f);
	stray_dlss::mv::set_legacy_transposed_clip(false);

	const Matrix4 clip_to_prev = make_clip_to_prev_clip();
	std::printf("  ClipToPrevClip (row-major, m[r*4+c] == M[r][c]):\n");
	for (int r = 0; r < 4; ++r)
		std::printf("    % .6f % .6f % .6f % .6f\n",
			static_cast<double>(clip_to_prev.m[r * 4 + 0]),
			static_cast<double>(clip_to_prev.m[r * 4 + 1]),
			static_cast<double>(clip_to_prev.m[r * 4 + 2]),
			static_cast<double>(clip_to_prev.m[r * 4 + 3]));

	stray_dlss::ue4::ViewParams view = make_view(kMvW, kMvH);
	view.clip_to_prev_clip = clip_to_prev;

	stray_dlss::mv::ResolveInputs in;
	in.depth_resource = reinterpret_cast<std::uint64_t>(scene.depth.Get());
	in.velocity_resource = reinterpret_cast<std::uint64_t>(scene.velocity.Get());
	in.render_width = kMvW;
	in.render_height = kMvH;
	in.view = &view;

	// --- the shipped orientation ---
	if (!stray_dlss::mv::record(gpu.list.Get(), in, 2))
	{
		fail(stray_dlss::mv::last_error());
		return false;
	}
	std::vector<float> got;
	if (!read_mv_texture(gpu, kMvW, kMvH, got))
		return false;
	check(drain_validation(gpu, "camera-branch") == 0,
		"no D3D12 validation errors from the camera-branch dispatch and readback");

	for (const MvSample &sp : kMvSamples)
	{
		const std::size_t i = (static_cast<std::size_t>(sp.y) * kMvW + sp.x) * 2;
		const Mv want = reference_camera_mv(sp.x, sp.y, clip_to_prev, false);
		const Mv bad = reference_camera_mv(sp.x, sp.y, clip_to_prev, true);
		std::printf("  %-20s (%3u,%3u) got (% .5f,% .5f)  row-vector (% .5f,% .5f)"
			"  transposed (% .5f,% .5f)\n",
			sp.where, sp.x, sp.y,
			static_cast<double>(got[i + 0]), static_cast<double>(got[i + 1]),
			static_cast<double>(want.x), static_cast<double>(want.y),
			static_cast<double>(bad.x), static_cast<double>(bad.y));
	}

	check(compare_against_reference(got, clip_to_prev, /*expect_transposed=*/false, "shipped"),
		"every sampled pixel matches mul(v, ClipToPrevClip), the row-vector orientation");

	// --- the A/B toggle, which is also this test's own negative control ---
	//
	// A regression test that has never been seen to fail is not yet a regression test.
	// LegacyTransposedClip reproduces the shipped bug exactly — HLSL's default column-major
	// packing of a row-major upload is `mul(M, v)`, which is what the legacy path issues — so
	// running it here proves two things at once: that the A/B switch does what
	// mv_resolve.hpp claims, and that the assertion above genuinely rejects the old behaviour
	// rather than passing on anything the GPU happens to write.
	stray_dlss::mv::set_legacy_transposed_clip(true);
	if (!stray_dlss::mv::record(gpu.list.Get(), in, 2))
	{
		fail(stray_dlss::mv::last_error());
		return false;
	}
	std::vector<float> legacy;
	if (!read_mv_texture(gpu, kMvW, kMvH, legacy))
		return false;
	stray_dlss::mv::set_legacy_transposed_clip(false);

	check(compare_against_reference(legacy, clip_to_prev, /*expect_transposed=*/true, "legacy"),
		"LegacyTransposedClip=1 reproduces mul(ClipToPrevClip, v) exactly");

	// And the half that makes it a negative control: the shipped assertion must REJECT it.
	bool legacy_would_fail = false;
	for (const MvSample &sp : kMvSamples)
	{
		const std::size_t i = (static_cast<std::size_t>(sp.y) * kMvW + sp.x) * 2;
		const Mv want = reference_camera_mv(sp.x, sp.y, clip_to_prev, false);
		if (std::fabs(legacy[i + 0] - want.x) > mv_tolerance(want.x) ||
			std::fabs(legacy[i + 1] - want.y) > mv_tolerance(want.y))
		{
			legacy_would_fail = true;
			break;
		}
	}
	check(legacy_would_fail,
		"the old transposed behaviour FAILS the check above - this test can actually fire");

	check(drain_validation(gpu, "camera-branch-legacy") == 0,
		"no D3D12 validation errors from the legacy A/B pass");
	return true;
}

// ---- test 4: the restore puts the game's state back exactly ------------------------------
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
	in.depth_resource = reinterpret_cast<std::uint64_t>(game.depth.Get());
	in.velocity_resource = reinterpret_cast<std::uint64_t>(game.velocity.Get());
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

// A negative control for every other test in this file.
//
// The others assert "no D3D12 validation errors", which is only evidence if the detector
// can actually fire. So commit the precise bug we are guarding against — binding a
// descriptor table to a root parameter that is really a root CBV — and require the debug
// layer to catch it. ReShade's own state_block makes exactly this mistake, which is why it
// is the bug we most need a tripwire for. If this test ever stops reporting an error, the
// clean bill of health the other tests give is worthless and they must not be trusted.
bool test_validation_catches_wrong_root_parameter_type(Gpu &gpu)
{
	std::printf("\n[test] the debug layer catches a table bound to a root-CBV parameter\n");

	if (!gpu.info)
	{
		// vkd3d-proton does not implement ID3D12InfoQueue, so there is nothing here to detect
		// with. Say so rather than passing quietly: on hardware this proves nothing, and the
		// "no validation errors" claims in the other tests are equally hollow there.
		std::printf("  SKIP: no info queue on this device (expected under vkd3d-proton)\n");
		std::printf("  NOTE: validation coverage is a CI/WARP property, not a hardware one\n");
		return true;
	}

	Probe p;
	if (!build_probe(gpu, p))
		return false;
	drain_validation(gpu, "negative-control-setup");

	// Root parameter 1 is D3D12_ROOT_PARAMETER_TYPE_CBV (see build_probe). Binding a
	// descriptor table to it is the illegal call we must never emit.
	ID3D12DescriptorHeap *heaps[] = { p.gpu_heap.Get() };
	gpu.list->SetDescriptorHeaps(1, heaps);
	gpu.list->SetComputeRootSignature(p.rs.Get());
	gpu.list->SetComputeRootDescriptorTable(1, p.table);

	const int errors = drain_validation(gpu, "deliberate-misuse", /*expected=*/true);
	check(errors > 0, "the debug layer reported the illegal root-parameter binding");

	// The recorded list is garbage now; reset it so later tests start clean.
	gpu.list->Close();
	gpu.allocator->Reset();
	gpu.list->Reset(gpu.allocator.Get(), nullptr);
	drain_validation(gpu, "negative-control-teardown");
	return true;
}

// Exercises the ReShade-facing half of restore_game_compute_state.
//
// The native half has a golden-output test; this half only had comments. It is also where the
// suspected on-screen corruption lives, so the rules it claims to follow are asserted here:
//
//   1. the FIRST call must be count==0, which is what forces ReShade to re-issue the game's
//      descriptor heaps and compute root signature and to re-sync its own caches;
//   2. every later call binds exactly ONE table, never a whole vector — binding the vector is
//      what ReShade's own state_block does, and it passes zero handles through for parameters
//      that are not descriptor tables at all;
//   3. a parameter whose tracked handle is zero is never bound;
//   4. graphics tables are restored too, after the compute ones.
//
// get_native() hands the restore the harness's real WARP command list, so the native calls it
// emits genuinely execute and the debug layer judges them.
bool test_reshade_restore_call_pattern(Gpu &gpu)
{
	std::printf("\n[test] the ReShade half of restore issues the right calls, in order\n");

	Probe p;
	if (!build_probe(gpu, p))
		return false;
	drain_validation(gpu, "reshade-restore-setup");

	stray_dlss::test::FakeCommandList fake;
	fake.native = reinterpret_cast<std::uint64_t>(gpu.list.Get());

	// A layout whose parameter 1 is NOT a descriptor table. state_tracking represents that as
	// a zero handle, and the restore must skip it rather than bind a table there.
	state_tracking st;
	const reshade::api::pipeline_layout layout = {
		reinterpret_cast<std::uint64_t>(p.rs.Get()) };
	st.descriptor_tables[reshade::api::shader_stage::all_compute] = { layout,
		{ reshade::api::descriptor_table{ p.table.ptr },
		  reshade::api::descriptor_table{ 0 },
		  reshade::api::descriptor_table{ p.table.ptr } } };
	st.descriptor_tables[reshade::api::shader_stage::all_graphics] = { layout,
		{ reshade::api::descriptor_table{ p.table.ptr } } };
	st.pipelines[reshade::api::pipeline_stage::all] = reshade::api::pipeline{
		reinterpret_cast<std::uint64_t>(p.pso.Get()) };
	fake.put_private_data(&st);

	stray_dlss::rsb::restore_game_compute_state(&fake);

	const auto &b = fake.binds;
	check(!b.empty(), "the restore called into ReShade at all");
	if (b.empty())
		return false;

	check(b[0].count == 0 && b[0].first == 0,
		"the first call is the count==0 resync that re-issues heaps and root signature");
	check(b[0].layout.handle == layout.handle, "the resync names the game's own layout");

	bool all_single = true, bound_zero_handle = false;
	for (std::size_t i = 1; i < b.size(); ++i)
	{
		if (b[i].count != 1)
			all_single = false;
		for (const auto t : b[i].tables)
			if (t.handle == 0)
				bound_zero_handle = true;
	}
	check(all_single, "every later call binds exactly one table, never a whole vector");
	check(!bound_zero_handle, "no zero handle is ever bound (that would hit a root CBV)");

	// Parameter 1 is not a table, so only 0 and 2 may be restored for compute.
	int compute_binds = 0, graphics_binds = 0;
	bool touched_param_1 = false;
	for (std::size_t i = 1; i < b.size(); ++i)
	{
		const bool is_compute = (static_cast<std::uint32_t>(b[i].stages) &
			static_cast<std::uint32_t>(reshade::api::shader_stage::all_compute)) != 0;
		if (is_compute)
		{
			++compute_binds;
			if (b[i].first == 1)
				touched_param_1 = true;
		}
		else
		{
			++graphics_binds;
		}
	}
	check(compute_binds == 2, "exactly the two compute parameters that are tables were bound");
	check(!touched_param_1, "the non-table parameter was skipped");
	check(graphics_binds == 1, "the graphics table was restored as well");

	// The native calls really ran on the WARP list; the debug layer gets the final word.
	const int errors = drain_validation(gpu, "reshade-restore");
	check(errors == 0, "no D3D12 validation errors from the native half of the restore");

	gpu.list->Close();
	gpu.allocator->Reset();
	gpu.list->Reset(gpu.allocator.Get(), nullptr);
	stray_dlss::rsb::forget_all_command_lists();
	drain_validation(gpu, "reshade-restore-teardown");
	return true;
}

// Does ReShade's vkd3d extension hook reach descriptors we mint on the ORIGINAL device?
//
// CLAUDE.md §1 mandates initialising NGX with device::get_native() -- the original vkd3d
// device -- precisely to bypass ReShade's descriptor remapping. ReShade 6.8.0 added a hook that
// assumes the opposite: inside its proxy's QueryInterface for IID_ID3D12DeviceExt* it patches
// vtable slots 7/8 (and 14/15 for Ext2) of vkd3d's ID3D12DeviceExt, and those hooks run every
// handle through convert_to_original_cpu_descriptor_handle
// (d3d12_device.cpp:123-141, d3d12_extensions.cpp).
//
// That conversion is:
//     heap_index = (handle.ptr >> heap_index_start) & 0xFFFFFFF;
//     assert(heap_index < _descriptor_heaps.size() && ...);      // compiled out in Release
//     return _descriptor_heaps[heap_index]->_orig_base_cpu_handle.ptr + ...;
// A real vkd3d handle yields a garbage index, an out-of-bounds read, and a handle pointing
// anywhere. Those entry points are the CUDA texture/surface objects, which is the path
// nvngx_dlss.dll uses under vkd3d -- so DLSS would sample the wrong texture, silently. A colour
// cast is exactly what that looks like.
//
// The whole question is whether the patch is REACHABLE in our configuration. vkd3d-proton uses a
// single static vtable for the extension interface, so if anything in the process queries
// ReShade's proxy for this interface even once, the patch becomes process-global and applies to
// interfaces obtained straight from the original device -- ours included.
//
// This probe answers only that, and deliberately never calls the CUDA functions: it reads the
// vtable slot from the original device's interface, forces the patch by querying the proxy, and
// reads the slot again.
bool test_vkd3d_ext_hook_reachability(Gpu &gpu)
{
	std::printf("\n[test] whether ReShade's vkd3d ext hook reaches the original device\n");

	if (!running_under_reshade())
	{
		std::printf("  SKIP: not running under ReShade, so there is no hook to observe\n");
		return true;
	}

	// {7F2C9A11-3B4E-4D6A-812F-5E9CD37A1B42} - ReShade's "give me the original object".
	constexpr GUID kUnwrapped = { 0x7f2c9a11, 0x3b4e, 0x4d6a,
		{ 0x81, 0x2f, 0x5e, 0x9c, 0xd3, 0x7a, 0x1b, 0x42 } };
	// {11EA7A1A-0F6A-49BF-B612-3E30F8E201DD} / {E859C4AC-BA8F-41C4-8EAC-1137FDE6158D}
	constexpr GUID kDeviceExt = { 0x11ea7a1a, 0x0f6a, 0x49bf,
		{ 0xb6, 0x12, 0x3e, 0x30, 0xf8, 0xe2, 0x01, 0xdd } };
	constexpr GUID kDeviceExt2 = { 0xe859c4ac, 0xba8f, 0x41c4,
		{ 0x8e, 0xac, 0x11, 0x37, 0xfd, 0xe6, 0x15, 0x8d } };

	ComPtr<IUnknown> original;
	if (FAILED(gpu.device->QueryInterface(kUnwrapped, reinterpret_cast<void **>(original.GetAddressOf()))))
	{
		std::printf("  SKIP: could not unwrap ReShade's proxy device\n");
		return true;
	}

	// Take the extension interface from the ORIGINAL device. This goes through vkd3d's own
	// QueryInterface, so it installs nothing.
	ComPtr<IUnknown> ext_via_original;
	if (FAILED(original->QueryInterface(kDeviceExt, reinterpret_cast<void **>(ext_via_original.GetAddressOf()))))
	{
		std::printf("  SKIP: no ID3D12DeviceExt on this device (expected off vkd3d-proton)\n");
		return true;
	}

	const auto vtable_slot = [](IUnknown *obj, int slot) -> void * {
		return (*reinterpret_cast<void ***>(obj))[slot];
	};
	void *const before_get_cuda_surface = vtable_slot(ext_via_original.Get(), 8);

	// Now force the patch, the way any component that queries the PROXY would.
	ComPtr<IUnknown> ext_via_proxy;
	const HRESULT hr = gpu.device->QueryInterface(kDeviceExt2,
		reinterpret_cast<void **>(ext_via_proxy.GetAddressOf()));
	if (FAILED(hr))
	{
		// Fall back to the base interface; only slots 7/8 get patched then.
		gpu.device->QueryInterface(kDeviceExt, reinterpret_cast<void **>(ext_via_proxy.GetAddressOf()));
	}

	void *const after_get_cuda_surface = vtable_slot(ext_via_original.Get(), 8);
	const bool patched = before_get_cuda_surface != after_get_cuda_surface;

	std::printf("  GetCudaSurfaceObject slot: before=%p after=%p\n",
		before_get_cuda_surface, after_get_cuda_surface);

	if (patched)
	{
		// Confirm the new pointer really lives inside ReShade's module rather than being
		// coincidental churn.
		HMODULE owner = nullptr;
		::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCWSTR>(after_get_cuda_surface), &owner);
		wchar_t path[MAX_PATH] = {};
		if (owner != nullptr)
			::GetModuleFileNameW(owner, path, MAX_PATH);
		std::printf("  patched entry belongs to: %ls\n", path[0] ? path : L"<unknown>");

		std::printf("  REACHABLE: querying ReShade's proxy patched the vtable that the ORIGINAL\n");
		std::printf("             device's interface uses. NGX descriptors minted per CLAUDE.md\n");
		std::printf("             would be run through convert_to_original_cpu_descriptor_handle.\n");
	}
	else
	{
		std::printf("  NOT REACHABLE: the original device's interface is unaffected.\n");
	}

	// Deliberately not a pass/fail assertion. Both answers are legitimate findings, and which
	// one holds is exactly what we did not know; failing CI on it would be asserting the
	// conclusion rather than measuring it.
	std::printf("  (recorded, not asserted - this probe measures rather than judges)\n");
	return true;
}

// Is copying FROM a shader-visible heap what kills us?
//
// mv_resolve copies the game's SRV descriptors into its own heap. The harness has always
// created those source SRVs in a NON-shader-visible heap, on the assumption that the handles we
// capture come from UE4's offline heaps. That assumption is load-bearing and untested: the
// descriptors ReShade reports at dispatch time are the ones in the heap the game has BOUND,
// and a bound heap is necessarily shader-visible.
//
// D3D12 forbids a shader-visible source for CopyDescriptorsSimple. If that is what we do in
// the game, the copy produces a descriptor the GPU rejects — which matches the measured
// behaviour exactly: MvDispatch=0 (copy, never dispatch) survives, MvDispatch=1 (one 1x1
// group) hangs with Xid 109, because a bad descriptor only bites when it is read.
//
// This test does not assert a verdict; it establishes which way D3D12 rules, so the fix is
// chosen on evidence rather than on my reading of the spec.
bool test_copy_from_shader_visible_source(Gpu &gpu)
{
	std::printf("\n[test] is CopyDescriptorsSimple from a SHADER-VISIBLE source rejected?\n");

	if (!gpu.info)
	{
		std::printf("  SKIP: no info queue, so nothing can judge this\n");
		return true;
	}

	GameResources game;
	if (!create_game_resources(gpu, game, 1920, 1080))
		return false;

	// A shader-visible heap holding one SRV — the shape a game actually has bound.
	D3D12_DESCRIPTOR_HEAP_DESC hd = {};
	hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	hd.NumDescriptors = 4;
	hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	ComPtr<ID3D12DescriptorHeap> visible;
	HR(gpu.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&visible)));

	D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
	// The depth resource is R32G8X24_TYPELESS, so the only legal depth-plane view format is
	// R32_FLOAT_X8X24_TYPELESS. R32_FLOAT is rejected and removes the device.
	srv.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
	srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srv.Texture2D.MipLevels = 1;
	const D3D12_CPU_DESCRIPTOR_HANDLE visible_cpu = visible->GetCPUDescriptorHandleForHeapStart();
	gpu.device->CreateShaderResourceView(game.depth.Get(), &srv, visible_cpu);
	drain_validation(gpu, "shader-visible-setup");

	// A non-shader-visible destination, as our resolve pass would use.
	hd.NumDescriptors = 4;
	hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	ComPtr<ID3D12DescriptorHeap> dest_heap;
	HR(gpu.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&dest_heap)));

	gpu.device->CopyDescriptorsSimple(1, dest_heap->GetCPUDescriptorHandleForHeapStart(),
		visible_cpu, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

	const int errors = drain_validation(gpu, "copy-from-shader-visible", /*expected=*/true);
	if (errors > 0)
	{
		std::printf("  FINDING: D3D12 REJECTS a shader-visible source. If the handles we capture\n");
		std::printf("           in game live in the game's BOUND heap, our copy is illegal and\n");
		std::printf("           that is the hang.\n");
	}
	else
	{
		std::printf("  FINDING: D3D12 accepts a shader-visible source here, so this is NOT the\n");
		std::printf("           cause and the hang lies elsewhere.\n");
	}
	std::printf("  (recorded, not asserted)\n");
	return true;
}

// ---- object-model probes for the native backend (docs/superpowers/plans/2026-09-01, Task 1) ----
//
// The native backend patches vtable SLOTS. That only reaches every object of a class if the
// runtime shares ONE vtable across them, which vkd3d-proton (C, CONST_VTBL structs) almost
// certainly does and Microsoft's runtime may not. Measured on both, never assumed: the answer
// decides whether slot patching is the installation strategy at all (assessment §1.2, §8.3).
//
// Neither probe asserts. The value IS the measurement, and it is recorded in
// docs/STRAY-RENDERING-FACTS.md from the box run.
namespace {

void *vtable_of(IUnknown *o)
{
	return o != nullptr ? *reinterpret_cast<void **>(o) : nullptr;
}

// ReShade's proxies answer this IID with the object they wrap (the reachability probe above
// relies on the same GUID). Off ReShade it simply fails and the object is its own original.
IUnknown *unwrap(IUnknown *o)
{
	constexpr GUID kUnwrapped = { 0x7f2c9a11, 0x3b4e, 0x4d6a,
		{ 0x81, 0x2f, 0x5e, 0x9c, 0xd3, 0x7a, 0x1b, 0x42 } };
	IUnknown *orig = nullptr;
	if (o != nullptr && SUCCEEDED(o->QueryInterface(kUnwrapped, reinterpret_cast<void **>(&orig))))
		return orig;
	return nullptr;
}

} // namespace

bool test_static_vtables(Gpu &gpu)
{
	std::printf("\n[test] whether vtables are shared across objects of a class\n");

	// One allocator may back only ONE open list at a time (D3D12 ERROR #540 otherwise), so
	// each list is closed before the next is created on the same allocator.
	ComPtr<ID3D12CommandAllocator> alloc;
	ComPtr<ID3D12GraphicsCommandList> list_a, list_b;
	if (FAILED(gpu.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) ||
		FAILED(gpu.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&list_a))) ||
		FAILED(list_a->Close()) ||
		FAILED(gpu.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&list_b))) ||
		FAILED(list_b->Close()))
	{
		fail("could not create two command lists");
		return false;
	}

	D3D12_COMMAND_QUEUE_DESC qd = {};
	qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	ComPtr<ID3D12CommandQueue> queue_b;
	if (FAILED(gpu.device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue_b))))
	{
		fail("could not create a second command queue");
		return false;
	}

	ComPtr<ID3D12Resource> res_a = create_buffer(gpu, 256), res_b = create_buffer(gpu, 256);
	if (!res_a || !res_b)
	{
		fail("could not create two buffers");
		return false;
	}

	// A second device on the same adapter. Microsoft's runtime hands back the EXISTING device
	// for an adapter; vkd3d-proton creates another. Both are fine: distinct=0 means the
	// comparison is trivially true, and that is reported rather than hidden.
	ComPtr<IUnknown> adapter_unk;
	ComPtr<ID3D12Device> device_b;
	{
		LUID luid = gpu.device->GetAdapterLuid();
		ComPtr<IDXGIFactory4> factory;
		ComPtr<IDXGIAdapter> adapter;
		if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) &&
			SUCCEEDED(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter))))
			D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device_b));
	}

	const bool list_static   = vtable_of(list_a.Get()) == vtable_of(list_b.Get());
	const bool queue_static  = vtable_of(gpu.queue.Get()) == vtable_of(queue_b.Get());
	const bool res_static    = vtable_of(res_a.Get()) == vtable_of(res_b.Get());
	const bool device_static = device_b && vtable_of(gpu.device.Get()) == vtable_of(device_b.Get());
	const bool device_distinct = device_b && device_b.Get() != gpu.device.Get();

	std::printf("  vtable-static: list=%d queue=%d resource=%d device=%d (second device %s)\n",
		list_static, queue_static, res_static, device_static,
		!device_b ? "not created" : device_distinct ? "distinct object" : "SAME object");
	std::printf("  vtables: list=%p/%p queue=%p/%p resource=%p/%p device=%p/%p\n",
		vtable_of(list_a.Get()), vtable_of(list_b.Get()),
		vtable_of(gpu.queue.Get()), vtable_of(queue_b.Get()),
		vtable_of(res_a.Get()), vtable_of(res_b.Get()),
		vtable_of(gpu.device.Get()), vtable_of(device_b.Get()));

	// Under ReShade the objects above are its C++ proxies, whose vtables are trivially shared.
	// The question is about the RUNTIME underneath, so repeat it on the unwrapped originals.
	if (running_under_reshade())
	{
		ComPtr<IUnknown> la(unwrap(list_a.Get())), lb(unwrap(list_b.Get()));
		ComPtr<IUnknown> qa(unwrap(gpu.queue.Get())), qb(unwrap(queue_b.Get()));
		ComPtr<IUnknown> da(unwrap(gpu.device.Get())), db(unwrap(device_b.Get()));
		std::printf("  under ReShade, unwrapped originals: list=%s queue=%s device=%s\n",
			(la && lb) ? (vtable_of(la.Get()) == vtable_of(lb.Get()) ? "1" : "0") : "n/a",
			(qa && qb) ? (vtable_of(qa.Get()) == vtable_of(qb.Get()) ? "1" : "0") : "n/a",
			(da && db) ? (vtable_of(da.Get()) == vtable_of(db.Get()) ? "1" : "0") : "n/a");
	}
	std::printf("  (recorded, not asserted)\n");
	return true;
}

// Whether ID3D12Object::SetPrivateDataInterface releases the interface when the object dies.
// That final Release is the destruction callback the native backend keys resource liveness on
// (assessment §1.4). Documented D3D12 behaviour on Microsoft's runtime; vkd3d-proton is the
// question, and it is answered by the box run.
namespace {

struct Sentinel : IUnknown
{
	std::atomic<ULONG> refs{ 1 };
	bool *fired;
	explicit Sentinel(bool *f) : fired(f) {}
	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override
	{
		if (out == nullptr)
			return E_POINTER;
		if (riid == __uuidof(IUnknown))
		{
			*out = this;
			AddRef();
			return S_OK;
		}
		*out = nullptr;
		return E_NOINTERFACE;
	}
	ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
	ULONG STDMETHODCALLTYPE Release() override
	{
		const ULONG r = --refs;
		if (r == 0)
		{
			*fired = true;
			delete this;
		}
		return r;
	}
};

} // namespace

bool test_private_data_release_on_destroy(Gpu &gpu)
{
	std::printf("\n[test] whether private-data interfaces are released at object destruction\n");
	static const GUID kTag = { 0x5d1e9c30, 0x7a4b, 0x4e02,
		{ 0x9b, 0x1f, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77 } };

	bool fired = false;
	bool fired_early = false;
	{
		ComPtr<ID3D12Resource> res = create_buffer(gpu, 256);
		if (!res)
		{
			fail("could not create a buffer");
			return false;
		}
		Sentinel *s = new Sentinel(&fired);
		const HRESULT hr = res->SetPrivateDataInterface(kTag, s);
		s->Release(); // the object now holds the only reference
		std::printf("  SetPrivateDataInterface hr=0x%08x\n", static_cast<unsigned>(hr));
		fired_early = fired; // must still be false: the resource is alive
	} // res destroyed here
	std::printf("  private-data-release: fired=%d (fired_before_destroy=%d)\n",
		fired ? 1 : 0, fired_early ? 1 : 0);
	std::printf("  (recorded, not asserted)\n");
	return true;
}

#include "warp_nr_lifetime.inc"

#include "warp_native_backend.inc"

int main(int argc, char **argv)
{
	for (int i = 1; i < argc; ++i)
	{
		const std::string arg = argv[i];
		if (arg == "--hardware")
			g_use_hardware = true;
		else if (arg == "--expect-reshade")
			g_expect_reshade = true;
	}

	std::printf("D3D12 harness for the motion-vector resolve pass (%s)\n",
		g_use_hardware ? "HARDWARE adapter" : "WARP software adapter");

	const bool under_reshade = running_under_reshade();
	std::printf("d3d12.dll is %s\n",
		under_reshade ? "ReShade's proxy - every test below runs through it"
		              : "the plain runtime");
	if (g_expect_reshade)
		check(under_reshade, "we are running through ReShade's D3D12 proxy, as required");

	Gpu gpu;
	if (!create_gpu(gpu))
	{
		std::printf("could not create a WARP device\n");
		return 1;
	}
	std::printf("device up, info queue %s\n", gpu.info ? "active" : "UNAVAILABLE");

	test_validation_catches_wrong_root_parameter_type(gpu);
	test_dispatch_is_valid(gpu);
	test_no_allocation_churn(gpu);
	test_camera_branch_matrix_orientation(gpu);
	test_restore_preserves_game_state(gpu);
	test_reshade_restore_call_pattern(gpu);
	test_vkd3d_ext_hook_reachability(gpu);
	test_copy_from_shader_visible_source(gpu);
	test_static_vtables(gpu);
	test_private_data_release_on_destroy(gpu);
	test_nr_codec_deferred_destruction(gpu);
	// The native backend last: its hooks are installed on this device and never restored.
	test_vtable_patch_roundtrip(gpu);
	test_registry_liveness(gpu);
	test_registry_sentinel_survives_detach(gpu);
	test_registry_va_ranges_ring_and_placed(gpu);
	stray_dlss::native::shadow::set_mode(stray_dlss::native::shadow::Mode::debug); // the harness proves the debug shadow as today
	test_descriptor_shadow_copy_chain(gpu);
	test_descriptor_shadow_fast(gpu);
	test_native_stream_walk();
	test_native_hooks_ue4_shaped_frame(gpu);
	test_drive_mode_restore_is_complete(gpu);

	stray_dlss::mv::shutdown();
	drain_validation(gpu, "shutdown");

	std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "PASS" : "FAIL",
		g_failures, g_failures == 1 ? "" : "s");
	return g_failures == 0 ? 0 : 1;
}
