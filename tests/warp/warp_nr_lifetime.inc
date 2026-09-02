// ---- the DLSS-NR codec pass's deferred destruction, on WARP ----
//
// Included by warp_mv_resolve.cpp after its helpers (Gpu, check, HR, drain_validation, flush).
//
// WHAT THIS PROVES. The codec pass records dispatches that read and write the engine's colour
// image through OUR descriptor heap, OUR PSOs and OUR proxy texture, and NGX then reads that same
// proxy as DLSSNR.Color. All of it is referenced by a command list the GPU executes later, so
// destroying any of it when the CPU decides to stop — the NgxNR 1->0 toggle, a resolution change,
// NR validation refusing — is a use-after-free. A sibling port of this integration shipped exactly
// that (RemixProjGroup/dxvk-remix @ a69254ab, "fix the crash on disabling the pass").
//
// The assertion is deterministic and does not depend on how fast WARP happens to be: the pass
// decides from the timeline WE hand it, so the test can hold the timeline behind the real fence
// and require that nothing has been freed, then advance it and require that everything has. The
// refcount probe is the direct form of the same claim — while the timeline is behind, the object
// is still alive and someone other than this test is holding it.
//
// NO NEGATIVE CONTROL HERE, deliberately. The way to make the debug layer prove it can catch this
// is to actually release a resource an executing list references — and that removes WARP's device
// (measured in this harness on 2026-09-02, see `discard`), taking every later test with it. The
// detector that runs instead is drain_validation over the whole sequence.

#include "nr_codec_pass.hpp"

#include "core/nr_lifetime.hpp"

namespace {

// The engine's colour target shape at our hook point: FP16 RGBA with ALLOW_UNORDERED_ACCESS
// (CLAUDE.md §2.3, the TAA pass's `u0`).
bool create_nr_image(Gpu &gpu, UINT w, UINT h, ComPtr<ID3D12Resource> &out)
{
	D3D12_HEAP_PROPERTIES def = {};
	def.Type = D3D12_HEAP_TYPE_DEFAULT;

	D3D12_RESOURCE_DESC d = {};
	d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	d.Width = w;
	d.Height = h;
	d.DepthOrArraySize = 1;
	d.MipLevels = 1;
	d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
	d.SampleDesc.Count = 1;
	d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

	HR(gpu.device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &d,
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&out)));
	return true;
}

// How many references OTHER than the one this test holds. The test keeps its own AddRef on the
// object for the whole sequence, so 0 here means "the pass has let go" and >0 means "the pass is
// still holding it" — which is exactly the question a deferred free has to answer.
ULONG foreign_refs(IUnknown *obj)
{
	obj->AddRef();
	const ULONG total = obj->Release();
	// total counts this test's own keep-alive reference too.
	return total > 1 ? total - 1 : 0;
}

// Records one encode/evaluate-shaped/decode cycle and submits it WITHOUT waiting.
bool record_and_submit(Gpu &gpu, ID3D12Resource *image, ID3D12Resource *neural, UINT w, UINT h,
                       UINT64 &submitted_value)
{
	if (!stray_dlss::nrp::record_encode(gpu.list.Get(), image, w, h, /*scale=*/8.0f,
			/*color_strength=*/1.0f, /*transfer_strength=*/1.0f))
	{
		fail("record_encode");
		return false;
	}
	// The evaluate's own barrier pair, which ngx_nr records around EvaluateFeature. Balanced, as
	// nr_codec_pass.hpp requires.
	stray_dlss::nrp::transition_proxy(gpu.list.Get(), /*to_shader_resource=*/true);
	stray_dlss::nrp::transition_proxy(gpu.list.Get(), /*to_shader_resource=*/false);
	if (!stray_dlss::nrp::record_decode(gpu.list.Get(), image, neural, w, h))
	{
		fail("record_decode");
		return false;
	}

	HR(gpu.list->Close());
	ID3D12CommandList *lists[] = { gpu.list.Get() };
	gpu.queue->ExecuteCommandLists(1, lists);
	++gpu.fence_value;
	HR(gpu.queue->Signal(gpu.fence.Get(), gpu.fence_value));
	submitted_value = gpu.fence_value;
	return true;
}

bool wait_and_reset(Gpu &gpu)
{
	while (gpu.fence->GetCompletedValue() < gpu.fence_value)
		Sleep(1);
	HR(gpu.allocator->Reset());
	HR(gpu.list->Reset(gpu.allocator.Get(), nullptr));
	return true;
}

// ---- test: nothing the codec pass owns is destroyed before its fence -----------------------

bool test_nr_codec_deferred_destruction(Gpu &gpu)
{
	std::printf("\n[test] NR codec pass: teardown and recreate are fence-deferred\n");

	using namespace stray_dlss;

	const nrp::TypedUavSupport support =
		nrp::probe_typed_uav(gpu.device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT);
	std::printf("  R16G16B16A16_FLOAT typed UAV: queried=%d view=%d load=%d store=%d\n",
		support.queried ? 1 : 0, support.view ? 1 : 0, support.load ? 1 : 0,
		support.store ? 1 : 0);
	if (!(support.queried && support.view && support.load && support.store))
	{
		// Not a pass. The pass under test refuses this format by design, so there is nothing
		// left to judge, and saying "ok" here would be the vacuous assertion CLAUDE.md §6 warns
		// about.
		fail("this device cannot do typed UAV load+store on FP16 RGBA, so the codec pass "
		     "cannot be exercised at all");
		return false;
	}

	constexpr UINT kW = 320, kH = 180;
	constexpr UINT kBigW = 640, kBigH = 360;

	ComPtr<ID3D12Resource> image, neural, big_image, big_neural;
	if (!create_nr_image(gpu, kBigW, kBigH, image) ||
		!create_nr_image(gpu, kBigW, kBigH, neural) ||
		!create_nr_image(gpu, kBigW, kBigH, big_image) ||
		!create_nr_image(gpu, kBigW, kBigH, big_neural))
		return false;

	// The timeline this test owns, exactly as ngx_nr owns the real one. One present has already
	// happened, so the fence exists and work recorded from here is covered by a real signal value
	// rather than by the no-fence present ring.
	nrlife::Timeline timeline;
	nrlife::on_signalled(timeline, /*completed=*/0);
	nrp::set_timeline(timeline);

	if (!nrp::initialise(gpu.device.Get(), image.Get(), kW, kH))
	{
		fail("nrp::initialise");
		std::printf("  (%s)\n", nrp::last_error());
		return false;
	}
	check(nrp::is_ready(), "the codec pass came up");
	// Again after the first initialise, which takes the device-change path. The pass must not
	// lose the timeline there — it did once, and every tag silently fell back to the no-fence
	// present ring, which passes a "nothing is freed early" test while proving nothing about the
	// fence. Push it again and assert the pass is really on the fence.
	nrp::set_timeline(timeline);
	check(nrlife::tag_now(timeline).fence != 0,
		"tags taken now name a real fence value, not the no-fence fallback");

	// Hold our own reference on the proxy for the whole sequence, so the object survives long
	// enough for us to ask whether the PASS has released it.
	ComPtr<ID3D12Resource> proxy_keep = nrp::proxy();
	check(proxy_keep.Get() != nullptr, "the proxy exists");

	// The value the pass will tag this frame's work with. The timeline is a MODEL of the queue:
	// the test advances it only as far as the real fence has actually gone, which is what makes
	// every assertion below independent of how fast WARP happens to be.
	const std::uint64_t work_tag = nrlife::pending_signal(timeline);

	UINT64 submitted = 0;
	if (!record_and_submit(gpu, image.Get(), neural.Get(), kW, kH, submitted))
		return false;

	const std::uint64_t released_before = nrp::stats().resource_sets_released;

	// --- the transition the sibling port crashed on: disable while a frame is in flight ---
	// This is what the NgxNR 1->0 toggle and the ini reloader call, and it happens on their
	// thread, mid-frame, with the evaluate still executing.
	nrp::request_shutdown();
	check(nrp::proxy() == nullptr, "the pass has let go of the proxy pointer on teardown request");
	check(nrp::stats().resource_sets_released == released_before,
		"request_shutdown destroyed NOTHING (the GPU has not passed the frame yet)");
	check(foreign_refs(proxy_keep.Get()) >= 1,
		"the proxy is still REFERENCE-HELD by the pass while the frame is in flight");

	// A present whose fence has not reached the submitted work must still free nothing. This is
	// the assertion that makes the deferral real rather than merely late.
	nrlife::on_signalled(timeline, /*completed=*/0);
	nrp::set_timeline(timeline);
	nrp::collect();
	check(nrp::stats().resource_sets_released == released_before,
		"a present BEHIND the fence frees nothing");
	check(foreign_refs(proxy_keep.Get()) >= 1, "and the proxy is still held");

	// Now let the GPU finish, and only then tell the timeline that the work is behind us.
	if (!wait_and_reset(gpu))
		return false;
	nrlife::on_signalled(timeline, work_tag);
	nrp::set_timeline(timeline);
	nrp::collect();
	check(nrp::stats().resource_sets_released > released_before,
		"once the queue has passed the work, the retired set IS freed");
	check(foreign_refs(proxy_keep.Get()) == 0,
		"and the pass no longer holds the proxy — deferred is not leaked");
	proxy_keep.Reset();

	if (drain_validation(gpu, "nr-codec teardown") != 0)
		fail("validation errors during the teardown cycle");

	// --- toggle back on, at a NEW extent, repeatedly ---
	// Re-enable rebuilds from nothing; a resolution change retires the live set mid-flight. Both
	// are run several times because the defect class only shows up when a new set is built while
	// an old one is still in flight.
	for (int i = 0; i < 3; ++i)
	{
		const UINT w = (i % 2) == 0 ? kW : kBigW;
		const UINT h = (i % 2) == 0 ? kH : kBigH;
		ID3D12Resource *img = (i % 2) == 0 ? image.Get() : big_image.Get();
		ID3D12Resource *nrl = (i % 2) == 0 ? neural.Get() : big_neural.Get();

		if (!nrp::initialise(gpu.device.Get(), img, w, h))
		{
			fail("nrp::initialise on the re-enable");
			std::printf("  (%s)\n", nrp::last_error());
			return false;
		}
		const std::uint64_t tag = nrlife::pending_signal(timeline);
		if (!record_and_submit(gpu, img, nrl, w, h, submitted))
			return false;

		// Disable again with the work still in flight. One present passes with the fence still
		// behind, so nothing may go.
		const std::uint64_t freed_before = nrp::stats().resource_sets_released;
		nrp::request_shutdown();
		nrlife::on_signalled(timeline, tag - 1);
		nrp::set_timeline(timeline);
		nrp::collect();
		check(nrp::stats().resource_sets_released == freed_before,
			"toggle loop: nothing freed while the fence is behind");

		if (!wait_and_reset(gpu))
			return false;
		nrlife::on_signalled(timeline, tag);
		nrp::set_timeline(timeline);
		nrp::collect();
	}

	check(nrp::stats().live_retired == 0,
		"every deferred set was eventually freed once its fence passed");
	if (drain_validation(gpu, "nr-codec toggle loop") != 0)
		fail("validation errors during the toggle loop");

	nrp::shutdown();
	if (drain_validation(gpu, "nr-codec shutdown") != 0)
		fail("validation errors at shutdown");
	return true;
}

} // namespace
