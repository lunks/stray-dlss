// ---- frame generation's present path on WARP (present-twice, experiment mode, no NGX) ----
//
// Included by warp_mv_resolve.cpp after warp_native_backend.inc (it borrows DriveTestSink and
// real_device). Runs LAST, in drive mode: it installs the present owner on a real DXGI
// swapchain over a hidden window, plays the game's part exactly as UE 4.27 does (GetBuffer per
// index after creation and after every ResizeBuffers, one Present per frame, its own index
// counter), and checks through the REAL ring — read back via the ORIGINAL GetBuffer — that
// each game present produced two presents in the right order with the right pixels:
// the previous frame under a magenta band, then the frame the game just rendered. Every
// D3D12 call the FG path makes runs under the debug layer here.

#include "backend_native/fg_present.hpp"
#include "backend_native/present_owner.hpp"

#include <dxgi1_4.h>

#include <cstring>

namespace {

using PFN_FgGetBuffer = HRESULT(STDMETHODCALLTYPE *)(IDXGISwapChain *, UINT, REFIID, void **);

struct FgPixel
{
	unsigned r = 0, g = 0, b = 0, a = 0; // 10/10/10/2 as stored
	bool ok = false;
};

// Reads one texel of an R10G10B10A2 texture in COMMON/PRESENT state through a readback buffer.
FgPixel fg_read_pixel(Gpu &gpu, ID3D12Resource *tex, UINT x, UINT y)
{
	FgPixel px;
	D3D12_RESOURCE_DESC td = tex->GetDesc();
	D3D12_RESOURCE_DESC one = td;
	one.Width = 1;
	one.Height = 1;
	one.Flags = D3D12_RESOURCE_FLAG_NONE;
	D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
	UINT64 bytes = 0;
	gpu.device->GetCopyableFootprints(&one, 0, 1, 0, &fp, nullptr, nullptr, &bytes);
	ComPtr<ID3D12Resource> rb = create_buffer(gpu, bytes, D3D12_HEAP_TYPE_READBACK);
	if (!rb)
		return px;
	if (FAILED(gpu.allocator->Reset()) || FAILED(gpu.list->Reset(gpu.allocator.Get(), nullptr)))
		return px;
	D3D12_RESOURCE_BARRIER b = {};
	b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	b.Transition.pResource = tex;
	b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
	b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
	b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	gpu.list->ResourceBarrier(1, &b);
	D3D12_TEXTURE_COPY_LOCATION dst = {};
	dst.pResource = rb.Get();
	dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	dst.PlacedFootprint = fp;
	D3D12_TEXTURE_COPY_LOCATION src = {};
	src.pResource = tex;
	src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	D3D12_BOX box = { x, y, 0, x + 1, y + 1, 1 };
	gpu.list->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
	b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
	b.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
	gpu.list->ResourceBarrier(1, &b);
	gpu.list->Close();
	ID3D12CommandList *lists[] = { gpu.list.Get() };
	gpu.queue->ExecuteCommandLists(1, lists);
	gpu.queue->Signal(gpu.fence.Get(), ++gpu.fence_value);
	if (gpu.fence->GetCompletedValue() < gpu.fence_value)
	{
		HANDLE ev = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
		gpu.fence->SetEventOnCompletion(gpu.fence_value, ev);
		::WaitForSingleObject(ev, 5000);
		::CloseHandle(ev);
	}
	void *p = nullptr;
	D3D12_RANGE range = { 0, static_cast<SIZE_T>(bytes) };
	if (FAILED(rb->Map(0, &range, &p)) || p == nullptr)
		return px;
	std::uint32_t v = 0;
	std::memcpy(&v, p, 4);
	D3D12_RANGE none = { 0, 0 };
	rb->Unmap(0, &none);
	px.r = v & 0x3FF;
	px.g = (v >> 10) & 0x3FF;
	px.b = (v >> 20) & 0x3FF;
	px.a = (v >> 30) & 0x3;
	px.ok = true;
	return px;
}

bool fg_near(unsigned v, float f) { const int want = static_cast<int>(f * 1023.0f + 0.5f); return static_cast<int>(v) >= want - 2 && static_cast<int>(v) <= want + 2; }

// The game's frame: clear the buffer it believes is current to a colour, execute, wait.
bool fg_render_frame(Gpu &gpu, ID3D12Resource *buf, ID3D12DescriptorHeap *rtv_heap, const float rgba[4])
{
	if (FAILED(gpu.allocator->Reset()) || FAILED(gpu.list->Reset(gpu.allocator.Get(), nullptr)))
		return false;
	const D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
	gpu.device->CreateRenderTargetView(buf, nullptr, rtv);
	D3D12_RESOURCE_BARRIER b = {};
	b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	b.Transition.pResource = buf;
	b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
	b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
	b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	gpu.list->ResourceBarrier(1, &b);
	gpu.list->ClearRenderTargetView(rtv, rgba, 0, nullptr);
	b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
	b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
	gpu.list->ResourceBarrier(1, &b);
	gpu.list->Close();
	ID3D12CommandList *lists[] = { gpu.list.Get() };
	gpu.queue->ExecuteCommandLists(1, lists);
	gpu.queue->Signal(gpu.fence.Get(), ++gpu.fence_value);
	return true;
}

void fg_wait_idle(Gpu &gpu)
{
	gpu.queue->Signal(gpu.fence.Get(), ++gpu.fence_value);
	if (gpu.fence->GetCompletedValue() < gpu.fence_value)
	{
		HANDLE ev = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
		gpu.fence->SetEventOnCompletion(gpu.fence_value, ev);
		::WaitForSingleObject(ev, 5000);
		::CloseHandle(ev);
	}
}

} // namespace

bool test_fg_present_twice(Gpu &gpu)
{
	std::printf("\n[test] frame generation: replacement back buffers, two presents per game frame, resize survival (experiment mode, no NGX)\n");
	namespace native = stray_dlss::native;
	namespace fg = native::fg;
	namespace present = native::present;
	ID3D12Device *dev = real_device(gpu);

	static DriveTestSink sink;
	sink = DriveTestSink{};
	sink.gpu = &gpu;
	native::set_sink(&sink);
	check(native::mode() == native::Mode::drive, "the native backend is in drive mode (the present owner requires it)");

	// The REAL queue, as the device hook would have recorded it for the game.
	{
		ID3D12CommandQueue *real_queue = gpu.queue.Get();
		if (running_under_reshade())
		{
			IUnknown *orig = unwrap(gpu.queue.Get());
			if (orig != nullptr)
			{
				ID3D12CommandQueue *q = nullptr;
				orig->QueryInterface(IID_PPV_ARGS(&q));
				orig->Release();
				if (q != nullptr)
				{
					q->Release();
					real_queue = q;
				}
			}
		}
		present::note_queue(real_queue, D3D12_COMMAND_LIST_TYPE_DIRECT);
	}

	// install() reads the FG knobs from the config source (none here -> NgxFG=0), so the test's
	// configuration goes in AFTER it; the GetBuffer patch only happens at swapchain creation.
	if (!present::install(dev))
	{
		fail("present::install failed");
		return false;
	}
	std::printf("  %s\n", present::report());
	fg::Config fc;
	fc.enabled = true;
	fc.mode = fg::Mode::experiment;
	fc.pacing = fg::Pacing::none; // everything on this thread: deterministic readbacks
	fc.wait_ms = 0;
	fc.band = true;
	fg::configure(fc);

	// A hidden window and a real flip-model swapchain in the game's format.
	HWND hwnd = ::CreateWindowExW(0, L"STATIC", L"stray-dlss fg harness", WS_OVERLAPPEDWINDOW, 0, 0, 320, 240, nullptr, nullptr, nullptr, nullptr);
	ComPtr<IDXGIFactory2> factory;
	if (hwnd == nullptr || FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))))
	{
		std::printf("  NOTE: no window or DXGI factory on this runner; the FG present path is NOT exercised here\n");
		return true;
	}
	present::note_factory(factory.Get());
	const UINT W = 640, H = 360;
	DXGI_SWAP_CHAIN_DESC1 sd = {};
	sd.Width = W;
	sd.Height = H;
	sd.Format = DXGI_FORMAT_R10G10B10A2_UNORM; // the game's (CLAUDE.md §2.1)
	sd.SampleDesc.Count = 1;
	sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	sd.BufferCount = 3;
	sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	sd.Scaling = DXGI_SCALING_STRETCH;
	sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
	ComPtr<IDXGISwapChain1> sc1;
	const HRESULT create_hr = factory->CreateSwapChainForHwnd(gpu.queue.Get(), hwnd, &sd, nullptr, nullptr, &sc1);
	if (FAILED(create_hr))
	{
		std::printf("  NOTE: CreateSwapChainForHwnd failed 0x%08lx on this runner; the FG present path is NOT exercised here\n", static_cast<unsigned long>(create_hr));
		::DestroyWindow(hwnd);
		return true;
	}
	IDXGISwapChain *sc = sc1.Get();
	ComPtr<IDXGISwapChain3> sc3;
	sc1.As(&sc3);
	check(present::stats().swapchains >= 1, "the present owner recorded the swapchain at creation");
	auto orig_get_buffer = reinterpret_cast<PFN_FgGetBuffer>(native::original_for(sc, native::slot::kSwapChain_GetBuffer));
	check(orig_get_buffer != nullptr, "GetBuffer is hooked on the swapchain's vtable (FG enabled at creation)");
	if (orig_get_buffer == nullptr)
	{
		::DestroyWindow(hwnd);
		return false; // nothing below can run without the hook; do not call a null original
	}

	ComPtr<ID3D12DescriptorHeap> rtv_heap;
	{
		D3D12_DESCRIPTOR_HEAP_DESC hd = {};
		hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
		hd.NumDescriptors = 1;
		HR(gpu.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtv_heap)));
	}

	// UE 4.27's viewport: GetBuffer(i) for every index right after creation / resize, then its
	// own counter from 0 (facts §30.4). The colours make every frame identifiable.
	const float colours[6][4] = {
		{ 1.0f, 0.0f, 0.0f, 1.0f }, { 0.0f, 1.0f, 0.0f, 1.0f }, { 0.0f, 0.0f, 1.0f, 1.0f },
		{ 1.0f, 1.0f, 0.0f, 1.0f }, { 0.0f, 1.0f, 1.0f, 1.0f }, { 0.5f, 0.5f, 0.5f, 1.0f },
	};
	auto play = [&](UINT buffer_count, int frames, const char *phase, bool verify) -> bool {
		std::vector<ComPtr<ID3D12Resource>> game_buffers(buffer_count);
		for (UINT i = 0; i < buffer_count; ++i)
		{
			if (FAILED(sc->GetBuffer(i, IID_PPV_ARGS(&game_buffers[i]))))
			{
				fail("GetBuffer through the hook failed");
				return false;
			}
		}
		{
			const fg::Stats st = fg::stats();
			std::printf("  [%s] after GetBuffer: replacements=%u %ux%u fmt %u\n", phase, st.replacement_count, st.width, st.height, st.format);
			check(st.replacement_count == buffer_count, "the game received one replacement per requested buffer");
			// A replacement is OUR texture: not the real back buffer.
			ComPtr<ID3D12Resource> real0;
			orig_get_buffer(sc, 0, IID_PPV_ARGS(&real0));
			check(real0 && real0.Get() != game_buffers[0].Get(), "the game's buffer 0 is a replacement, not the swapchain's real buffer 0");
			const D3D12_RESOURCE_DESC rd = game_buffers[0]->GetDesc();
			check((rd.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) != 0 && rd.Format == DXGI_FORMAT_R10G10B10A2_UNORM, "the replacement is render-target capable in the swapchain's format");
		}
		UINT game_index = 0;
		for (int k = 0; k < frames; ++k)
		{
			const fg::Stats before = fg::stats();
			if (!fg_render_frame(gpu, game_buffers[game_index].Get(), rtv_heap.Get(), colours[k % 6]))
			{
				fail("could not render the game's frame");
				return false;
			}
			const HRESULT hr = sc->Present(0, 0);
			game_index = (game_index + 1) % buffer_count;
			const fg::Stats after = fg::stats();
			const std::uint64_t issued = after.presents_issued - before.presents_issued;
			const bool generated = after.generated_presented > before.generated_presented;
			std::printf("  [%s] frame %d: Present hr=0x%08lx issued=%llu generated=%d\n", phase, k, static_cast<unsigned long>(hr),
				static_cast<unsigned long long>(issued), generated ? 1 : 0);
			if (k == 0)
				check(issued == 1 && !generated, "the first frame after (re)creation presents the real frame only (no previous frame)");
			else
				check(issued == 2 && generated, "every later game present issues TWO presents: generated, then real");
			if (verify && k >= 1 && sc3)
			{
				fg_wait_idle(gpu);
				const UINT n = buffer_count;
				const UINT cur = sc3->GetCurrentBackBufferIndex();
				ComPtr<ID3D12Resource> real_last, real_generated;
				orig_get_buffer(sc, (cur + n - 1) % n, IID_PPV_ARGS(&real_last));
				orig_get_buffer(sc, (cur + n - 2) % n, IID_PPV_ARGS(&real_generated));
				const FgPixel pr = fg_read_pixel(gpu, real_last.Get(), W / 2, H / 2);
				const FgPixel pg = fg_read_pixel(gpu, real_generated.Get(), W / 2, H / 2);
				const FgPixel pb = fg_read_pixel(gpu, real_generated.Get(), W / 2, 8);
				const float *c = colours[k % 6], *p = colours[(k - 1) % 6];
				std::printf("    real ring: last=(%u,%u,%u) want (%.0f,%.0f,%.0f) | generated centre=(%u,%u,%u) want previous (%.0f,%.0f,%.0f) | band=(%u,%u,%u) want magenta\n",
					pr.r, pr.g, pr.b, c[0] * 1023, c[1] * 1023, c[2] * 1023, pg.r, pg.g, pg.b, p[0] * 1023, p[1] * 1023, p[2] * 1023, pb.r, pb.g, pb.b);
				check(pr.ok && fg_near(pr.r, c[0]) && fg_near(pr.g, c[1]) && fg_near(pr.b, c[2]), "the last presented real buffer holds the frame the game just rendered (mirror + copy correct)");
				check(pg.ok && fg_near(pg.r, p[0]) && fg_near(pg.g, p[1]) && fg_near(pg.b, p[2]), "the generated frame before it holds the PREVIOUS game frame");
				check(pb.ok && fg_near(pb.r, 1.0f) && fg_near(pb.g, 0.0f) && fg_near(pb.b, 1.0f), "the generated frame carries the magenta band");
			}
		}
		// UE4 releases every back buffer before ResizeBuffers.
		game_buffers.clear();
		return true;
	};

	if (!play(3, 4, "creation", true))
		return false;
	{
		const fg::Stats st = fg::stats();
		check(st.game_presents == 4 && st.presents_issued == 7 && st.generated_presented == 3, "counters: 4 game presents -> 7 presents issued, 3 generated");
		check(st.refused[static_cast<int>(stray_dlss::core::fg::Refusal::no_previous_frame)] == 1, "exactly one refusal: the first frame had no previous frame");
	}
	check(drain_validation(gpu, "fg-creation") == 0, "no validation errors across the present-twice sequence");

	// ResizeBuffers, as UE4 does it (count 0 = keep, new size): the replacements are dropped,
	// the mirror resets, GetBuffer re-arms at the new size.
	const HRESULT rhr = sc->ResizeBuffers(0, 800, 450, DXGI_FORMAT_UNKNOWN, 0);
	std::printf("  ResizeBuffers(0, 800x450) hr=0x%08lx\n", static_cast<unsigned long>(rhr));
	check(SUCCEEDED(rhr), "ResizeBuffers succeeded with the game's buffers released (our references do not block it)");
	{
		const fg::Stats st = fg::stats();
		check(st.reconfigures == 1 && st.replacement_count == 0, "the resize dropped the replacements and bumped the epoch");
	}
	if (!play(3, 3, "after-resize", true))
		return false;
	{
		const fg::Stats st = fg::stats();
		check(st.width == 800 && st.height == 450, "the replacements were re-armed at the new size");
		check(st.refused[static_cast<int>(stray_dlss::core::fg::Refusal::no_previous_frame)] == 2, "one more no-previous-frame refusal after the resize, none other");
	}
	// SetFullscreenState(FALSE) on a windowed chain is a no-op for DXGI but exercises the drain +
	// epoch path that the fullscreen transition takes on the box.
	const HRESULT fhr = sc->SetFullscreenState(FALSE, nullptr);
	std::printf("  SetFullscreenState(FALSE) hr=0x%08lx reconfigures=%llu\n", static_cast<unsigned long>(fhr), static_cast<unsigned long long>(fg::stats().reconfigures));
	check(fg::stats().reconfigures == 2, "SetFullscreenState went through the reconfigure path");
	if (!play(3, 3, "after-fullscreen-state", false))
		return false;
	check(drain_validation(gpu, "fg-resize") == 0, "no validation errors across resize, fullscreen-state and the presents after them");

	// The worker-thread presenter: the same sequence off the game thread, with a 1 ms pacing
	// delay. Counters are read after the drain (present::uninstall drains and releases).
	fc.pacing = fg::Pacing::thread;
	fc.wait_ms = 1;
	fg::configure(fc);
	const fg::Stats before_thread = fg::stats();
	if (!play(3, 6, "worker", false))
		return false;
	present::uninstall(); // drains the worker, releases the replacements and the ring
	{
		const fg::Stats st = fg::stats();
		std::printf("  worker phase: game presents +%llu, issued +%llu, generated +%llu, worker waits %llu, issued-interval p50=%u p99=%u ms %s\n",
			static_cast<unsigned long long>(st.game_presents - before_thread.game_presents),
			static_cast<unsigned long long>(st.presents_issued - before_thread.presents_issued),
			static_cast<unsigned long long>(st.generated_presented - before_thread.generated_presented),
			static_cast<unsigned long long>(st.worker_waits), st.issued_p50_ms, st.issued_p99_ms,
			st.issued_second_peak_ms >= 0 ? "bimodal" : "unimodal");
		check(st.game_presents - before_thread.game_presents == 6, "the worker phase saw 6 game presents");
		check(st.presents_issued - before_thread.presents_issued == 11, "the worker issued 11 presents for them (2 per frame, 1 for the first)");
		check(st.generated_presented - before_thread.generated_presented == 5, "5 generated frames were presented by the worker");
	}
	fg::log_stats("harness");
	check(drain_validation(gpu, "fg-worker") == 0, "no validation errors from the worker thread's copies and presents");

	sc3.Reset();
	sc1.Reset();
	factory.Reset();
	::DestroyWindow(hwnd);
	native::set_sink(nullptr);
	return true;
}
