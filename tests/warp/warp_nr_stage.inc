// ---- the DLSS-NR PRESENT STAGE's D3D12 half, on WARP ----
//
// Included by warp_mv_resolve.cpp after its helpers (Gpu, check, HR, drain_validation, flush).
//
// WHAT THIS PROVES, and why it is worth a harness test at all. The stage's whole D3D12 surface is
// one committed texture and two `CopyResource` calls wrapped in four barriers — which sounds too
// small to get wrong, and is exactly the shape of thing this project has got wrong twice. A
// barrier whose `StateBefore` does not match reality is undefined behaviour under vkd3d-proton
// with no debug layer to object (CLAUDE.md §5), and a copy between two textures whose descs
// disagree is a validation error nobody would see until the box. Here the debug layer and
// GPU-based validation judge both, and the pixels are read back so "it recorded" and "it copied
// the right bytes" are separate claims.
//
// WHAT IT CANNOT PROVE: anything about NGX, about the guides, or about the back buffer really
// being in PRESENT at the moment the host calls us. Those are the phase-2 questions.
//
// NOTE ON THE FORMAT. WARP is asked about `R10G10B10A2_UNORM` — Stray's own back-buffer format
// (CLAUDE.md §2.1, docs/STRAY-RENDERING-FACTS.md §32) — but the round trip is run on
// `R8G8B8A8_UNORM`, because the staging texture is created with ALLOW_UNORDERED_ACCESS and
// whether a given device permits that on a given format is precisely what the probe is for. The
// copies are format-independent; the probe's verdict is reported and, on WARP, not asserted.

#include "nr_stage.hpp"

#include <d3d12.h>

namespace {

// The barrier callback the stage takes instead of issuing its own. In the game this is
// icept::Backend::present_barrier (which also marks the present list as used); here it is a plain
// ResourceBarrier on the harness's list, so what is under test is the SEQUENCE of transitions the
// stage asks for rather than a stand-in for it.
struct WarpBarrierCtx
{
	ID3D12GraphicsCommandList *list = nullptr;
	int calls = 0;
};

void warp_stage_barrier(void *ctx, ID3D12Resource *res, std::uint32_t before, std::uint32_t after)
{
	auto *bc = static_cast<WarpBarrierCtx *>(ctx);
	if (bc == nullptr || bc->list == nullptr || res == nullptr || before == after)
		return;
	++bc->calls;
	D3D12_RESOURCE_BARRIER b = {};
	b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	b.Transition.pResource = res;
	b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	b.Transition.StateBefore = static_cast<D3D12_RESOURCE_STATES>(before);
	b.Transition.StateAfter = static_cast<D3D12_RESOURCE_STATES>(after);
	bc->list->ResourceBarrier(1, &b);
}

// A stand-in for a swapchain back buffer: plain 2D, single subresource, NO ALLOW_UNORDERED_ACCESS
// (a real swapchain buffer has none either — which is the first reason the stage stages at all),
// resting in COMMON, which IS D3D12_RESOURCE_STATE_PRESENT.
bool create_fake_back_buffer(Gpu &gpu, UINT w, UINT h, DXGI_FORMAT fmt,
                             ComPtr<ID3D12Resource> &out)
{
	D3D12_HEAP_PROPERTIES def = {};
	def.Type = D3D12_HEAP_TYPE_DEFAULT;

	D3D12_RESOURCE_DESC d = {};
	d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	d.Width = w;
	d.Height = h;
	d.DepthOrArraySize = 1;
	d.MipLevels = 1;
	d.Format = fmt;
	d.SampleDesc.Count = 1;
	d.Flags = D3D12_RESOURCE_FLAG_NONE;

	HR(gpu.device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &d,
		D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&out)));
	return true;
}

// Uploads `w*h` RGBA8 texels into `dst` (which must be in COMMON) and leaves it in COMMON.
bool upload_rgba8(Gpu &gpu, ID3D12Resource *dst, UINT w, UINT h,
                  const std::vector<std::uint32_t> &texels)
{
	constexpr UINT kPitchAlign = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
	const UINT row_pitch = ((w * 4 + kPitchAlign - 1) / kPitchAlign) * kPitchAlign;
	const UINT64 total = static_cast<UINT64>(row_pitch) * h;

	D3D12_HEAP_PROPERTIES up = {};
	up.Type = D3D12_HEAP_TYPE_UPLOAD;
	D3D12_RESOURCE_DESC bd = {};
	bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	bd.Width = total;
	bd.Height = 1;
	bd.DepthOrArraySize = 1;
	bd.MipLevels = 1;
	bd.SampleDesc.Count = 1;
	bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	ComPtr<ID3D12Resource> staging;
	HR(gpu.device->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd,
		D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&staging)));

	void *mapped = nullptr;
	const D3D12_RANGE none = { 0, 0 };
	HR(staging->Map(0, &none, &mapped));
	for (UINT y = 0; y < h; ++y)
	{
		auto *row = reinterpret_cast<std::uint32_t *>(
			static_cast<std::uint8_t *>(mapped) + static_cast<std::size_t>(y) * row_pitch);
		for (UINT x = 0; x < w; ++x)
			row[x] = texels[static_cast<std::size_t>(y) * w + x];
	}
	staging->Unmap(0, nullptr);

	D3D12_RESOURCE_BARRIER b = {};
	b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	b.Transition.pResource = dst;
	b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
	b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
	gpu.list->ResourceBarrier(1, &b);

	D3D12_TEXTURE_COPY_LOCATION cdst = {};
	cdst.pResource = dst;
	cdst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	cdst.SubresourceIndex = 0;
	D3D12_TEXTURE_COPY_LOCATION csrc = {};
	csrc.pResource = staging.Get();
	csrc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	csrc.PlacedFootprint.Offset = 0;
	csrc.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	csrc.PlacedFootprint.Footprint.Width = w;
	csrc.PlacedFootprint.Footprint.Height = h;
	csrc.PlacedFootprint.Footprint.Depth = 1;
	csrc.PlacedFootprint.Footprint.RowPitch = row_pitch;
	gpu.list->CopyTextureRegion(&cdst, 0, 0, 0, &csrc, nullptr);

	std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
	gpu.list->ResourceBarrier(1, &b);

	// The upload buffer must outlive execution, so flush here rather than leaving it to the
	// caller's next flush.
	return flush(gpu);
}

// Reads `src` (in COMMON) back into `out` and leaves it in COMMON.
bool readback_rgba8(Gpu &gpu, ID3D12Resource *src, UINT w, UINT h,
                    std::vector<std::uint32_t> &out)
{
	constexpr UINT kPitchAlign = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
	const UINT row_pitch = ((w * 4 + kPitchAlign - 1) / kPitchAlign) * kPitchAlign;
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

	D3D12_RESOURCE_BARRIER b = {};
	b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	b.Transition.pResource = src;
	b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
	b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
	gpu.list->ResourceBarrier(1, &b);

	D3D12_TEXTURE_COPY_LOCATION cdst = {};
	cdst.pResource = readback.Get();
	cdst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	cdst.PlacedFootprint.Offset = 0;
	cdst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	cdst.PlacedFootprint.Footprint.Width = w;
	cdst.PlacedFootprint.Footprint.Height = h;
	cdst.PlacedFootprint.Footprint.Depth = 1;
	cdst.PlacedFootprint.Footprint.RowPitch = row_pitch;
	D3D12_TEXTURE_COPY_LOCATION csrc = {};
	csrc.pResource = src;
	csrc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	csrc.SubresourceIndex = 0;
	gpu.list->CopyTextureRegion(&cdst, 0, 0, 0, &csrc, nullptr);

	std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
	gpu.list->ResourceBarrier(1, &b);

	if (!flush(gpu))
		return false;

	void *mapped = nullptr;
	const D3D12_RANGE range = { 0, static_cast<SIZE_T>(total) };
	HR(readback->Map(0, &range, &mapped));
	out.assign(static_cast<std::size_t>(w) * h, 0u);
	for (UINT y = 0; y < h; ++y)
	{
		const auto *row = reinterpret_cast<const std::uint32_t *>(
			static_cast<const std::uint8_t *>(mapped) + static_cast<std::size_t>(y) * row_pitch);
		for (UINT x = 0; x < w; ++x)
			out[static_cast<std::size_t>(y) * w + x] = row[x];
	}
	const D3D12_RANGE no_write = { 0, 0 };
	readback->Unmap(0, &no_write);
	return true;
}

// How many references OTHER than the one this test holds. Reference-count probe: AddRef then Release returns the prior count.
ULONG stage_foreign_refs(IUnknown *obj)
{
	obj->AddRef();
	const ULONG total = obj->Release();
	return total > 1 ? total - 1 : 0;
}

} // namespace

bool test_nr_present_stage_round_trip(Gpu &gpu)
{
	std::printf("\n-- NR present stage: the back-buffer round trip --\n");

	// THE PROBE, on the format that actually matters. Reported rather than asserted: WARP's answer
	// for R10G10B10A2_UNORM is WARP's, and the target is an RTX 4090 through vkd3d-proton. What
	// this pins is that the probe RUNS and answers, which is what the gate consumes.
	const stray_dlss::nrstage::TypedUavSupport probe =
		stray_dlss::nrstage::probe(gpu.device.Get(), DXGI_FORMAT_R10G10B10A2_UNORM);
	check(probe.queried, "CheckFeatureSupport answered for R10G10B10A2_UNORM (Stray's back-buffer "
		"format) — view/store are what nrplan::plan_post_tonemap gates on");
	std::printf("  note: R10G10B10A2_UNORM typedUavView=%d typedLoad=%d typedStore=%d on this "
		"adapter\n", probe.view ? 1 : 0, probe.load ? 1 : 0, probe.store ? 1 : 0);

	constexpr UINT kW = 64;
	constexpr UINT kH = 32;

	ComPtr<ID3D12Resource> back;
	if (!create_fake_back_buffer(gpu, kW, kH, DXGI_FORMAT_R8G8B8A8_UNORM, back))
		return false;

	// Pattern A: what the game rendered.
	std::vector<std::uint32_t> pattern_a(static_cast<std::size_t>(kW) * kH);
	for (std::size_t i = 0; i < pattern_a.size(); ++i)
		pattern_a[i] = 0xFF000000u | (static_cast<std::uint32_t>(i * 2654435761u) >> 8);
	if (!upload_rgba8(gpu, back.Get(), kW, kH, pattern_a))
		return false;

	check(stray_dlss::nrstage::ensure(gpu.device.Get(), kW, kH,
			static_cast<int>(DXGI_FORMAT_R8G8B8A8_UNORM), /*frame=*/1),
		"the staging texture allocates at the back buffer's own extent and format");
	ID3D12Resource *staging = stray_dlss::nrstage::staging();
	check(staging != nullptr, "staging() reports it");
	if (staging == nullptr)
		return false;

	// --- capture: back buffer -> staging ---
	WarpBarrierCtx bctx;
	bctx.list = gpu.list.Get();
	stray_dlss::nrstage::record_capture(gpu.list.Get(), back.Get(),
		D3D12_RESOURCE_STATE_COMMON, warp_stage_barrier, &bctx);
	check(bctx.calls == 2, "the capture asks the HOST for exactly two back-buffer transitions "
		"(into COPY_SOURCE and straight back), so an early return later cannot leave the frame in "
		"a state Present does not expect");

	// Scribble pattern B over the back buffer, so a writeback that does nothing is visible.
	std::vector<std::uint32_t> pattern_b(pattern_a.size(), 0xFF00FF00u);
	if (!flush(gpu))
		return false;
	if (!upload_rgba8(gpu, back.Get(), kW, kH, pattern_b))
		return false;

	// --- writeback: staging -> back buffer ---
	bctx.calls = 0;
	stray_dlss::nrstage::record_writeback(gpu.list.Get(), back.Get(),
		D3D12_RESOURCE_STATE_COMMON, warp_stage_barrier, &bctx);
	check(bctx.calls == 2, "the write-back asks for exactly two back-buffer transitions");
	if (!flush(gpu))
		return false;

	std::vector<std::uint32_t> got;
	if (!readback_rgba8(gpu, back.Get(), kW, kH, got))
		return false;

	bool same = got.size() == pattern_a.size();
	for (std::size_t i = 0; same && i < got.size(); ++i)
		same = got[i] == pattern_a[i];
	check(same, "the frame captured into staging came back into the back buffer byte for byte — "
		"both CopyResource calls moved the right pixels, not merely legal ones");

	check(drain_validation(gpu, "nr present stage") == 0,
		"no D3D12 validation errors across the capture/write-back pair");
	return true;
}

bool test_nr_present_stage_deferred_free(Gpu &gpu)
{
	std::printf("\n-- NR present stage: a resize retires, it does not release --\n");

	// The staging texture is bound into command lists the GPU executes LATER, so a resize that
	// released it inline would be a use-after-free with no error return under vkd3d-proton. Same
	// hazard, same two-ring-cycle rule as every other grow-only allocation in this codebase.
	constexpr UINT kW = 32, kH = 16;
	check(stray_dlss::nrstage::ensure(gpu.device.Get(), kW, kH,
			static_cast<int>(DXGI_FORMAT_R8G8B8A8_UNORM), /*frame=*/100),
		"a first staging texture exists");
	ID3D12Resource *first = stray_dlss::nrstage::staging();
	if (first == nullptr)
		return false;
	first->AddRef(); // this test's own keep-alive, so foreign_refs means "someone else holds it"

	// The resize.
	check(stray_dlss::nrstage::ensure(gpu.device.Get(), kW * 2, kH * 2,
			static_cast<int>(DXGI_FORMAT_R8G8B8A8_UNORM), /*frame=*/100),
		"a resize allocates a new one");
	check(stray_dlss::nrstage::staging() != first, "and staging() now reports the new texture");
	check(stage_foreign_refs(first) > 0,
		"the superseded texture is still HELD — retired, not released, because a command list "
		"recorded frames ago may still be reading it");

	// One present later is not enough: the rule is two full ring cycles.
	stray_dlss::nrstage::collect(101);
	check(stage_foreign_refs(first) > 0, "one present later it is still held");

	stray_dlss::nrstage::collect(100 + stray_dlss::ring::kFrameCount * 2);
	check(stage_foreign_refs(first) == 0,
		"two ring cycles later collect() has released it — a retirement that never completes is a "
		"leak, and this is the assertion that would catch one");

	first->Release();

	stray_dlss::nrstage::shutdown();
	check(stray_dlss::nrstage::staging() == nullptr, "shutdown() frees the live one too");
	check(drain_validation(gpu, "nr present stage lifetime") == 0,
		"no D3D12 validation errors across the retire/collect sequence");
	return true;
}
