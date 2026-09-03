// ---- DLSSNR.ControlMask: the fill pass, on WARP ----
//
// Included by warp_mv_resolve.cpp after its helpers (Gpu, check, HR, drain_validation, flush) and
// after warp_nr_stage.inc, whose readback_rgba8 this reuses.
//
// WHAT THIS PROVES. That the compute fill is LEGAL — the debug layer and GPU-based validation
// judge the root signature, the descriptor table, the barriers and the dispatch — and that it
// actually WROTE the value, read back texel by texel. Those are separate claims and this project
// has been burnt by conflating them: a pass that records cleanly and stores nothing is exactly
// what "DLSS evaluate OK and a byte-identical all-zero output" looked like (CLAUDE.md §5).
//
// It also pins the no-op: calling record_fill twice with the same values must record NOTHING the
// second time. That is not a performance nicety — a mask re-filled every present would put a
// UAV write and two transitions on the present list forever, and the fill count is the number
// that says whether it is happening.
//
// WHAT IT CANNOT PROVE: that the runtime does anything at all with the values. That needs the
// game. What it does NOT need to prove any more is the format — the runtime creates the SRV from
// whatever DXGI_FORMAT it is handed and lets the texture unit resolve it, so UNORM and FLOAT are
// both correct and only the integer families are not (refused in nrmaskplan, tested in the doctest
// lane).

#include "core/nr_mask_plan.hpp"
#include "nr_mask.hpp"

namespace {

// UNORM8 round-trip of a float in [0,1], as the hardware does it.
std::uint32_t unorm8_rgba(float r, float g, float b, float a)
{
	auto q = [](float v) -> std::uint32_t {
		if (v < 0.0f)
			v = 0.0f;
		if (v > 1.0f)
			v = 1.0f;
		return static_cast<std::uint32_t>(v * 255.0f + 0.5f);
	};
	return q(r) | (q(g) << 8) | (q(b) << 16) | (q(a) << 24);
}

} // namespace

bool test_nr_control_mask_fill(Gpu &gpu)
{
	std::printf("\n-- DLSSNR.ControlMask: the fill pass --\n");

	constexpr UINT kW = 64;
	constexpr UINT kH = 32;

	// R8G8B8A8_UNORM, which is both what readback_rgba8 can read and the shipping default —
	// the kernel's weight is saturate(Intensity * mask.x), so [0,1] is the whole meaningful range.
	stray_dlss::nrmask::set_format(static_cast<int>(DXGI_FORMAT_R8G8B8A8_UNORM));

	check(stray_dlss::nrmask::ensure(gpu.device.Get(), kW, kH, /*frame=*/1),
		"the mask texture allocates at the colour rect");
	check(stray_dlss::nrmask::texture() == nullptr,
		"but texture() reports NOTHING until it has been filled — an unfilled control texture "
		"holds undefined bytes and the runtime validates nothing, so binding one would be the "
		"silently-wrong-image failure this gate exists to prevent");

	// Half strength in every channel: a value that is neither the identity nor zero, so a fill
	// that wrote the wrong constant is distinguishable from one that wrote nothing.
	stray_dlss::nrmaskplan::Config cfg;
	cfg.enabled = true;
	cfg.channel_r = cfg.channel_g = cfg.channel_b = true;
	cfg.value_r = cfg.value_g = cfg.value_b = 0.5f;

	stray_dlss::nrmaskplan::FormatSupport support;
	support.queried = support.view = support.store = true;
	const stray_dlss::nrmaskplan::Plan plan = stray_dlss::nrmaskplan::plan_mask(cfg, kW, kH,
		static_cast<int>(DXGI_FORMAT_R8G8B8A8_UNORM), support);
	check(plan.result == stray_dlss::nrmaskplan::MaskResult::ok, "the plan admits the fill");
	check(!plan.is_identity, "and it is not the identity mask");

	check(stray_dlss::nrmask::record_fill(gpu.list.Get(), plan), "the fill records");
	const std::uint64_t fills_after_first = stray_dlss::nrmask::stats().fills;
	check(fills_after_first == 1, "exactly one dispatch so far");

	// The no-op. Same plan, nothing new to write.
	check(stray_dlss::nrmask::record_fill(gpu.list.Get(), plan),
		"a second call with the same values still reports the mask ready");
	check(stray_dlss::nrmask::stats().fills == fills_after_first,
		"and records NO second dispatch — a mask re-filled every present would put a UAV write "
		"and two transitions on the present list forever");

	ID3D12Resource *mask = stray_dlss::nrmask::texture();
	check(mask != nullptr, "texture() reports the mask once it is filled");
	if (mask == nullptr)
		return false;

	if (!flush(gpu))
		return false;

	// readback_rgba8 wants its source in COMMON; the fill leaves it in
	// NON_PIXEL_SHADER_RESOURCE, which is the state NGX wants and therefore the one it rests in.
	{
		D3D12_RESOURCE_BARRIER b = {};
		b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		b.Transition.pResource = mask;
		b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		b.Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
		b.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
		gpu.list->ResourceBarrier(1, &b);
		if (!flush(gpu))
			return false;
	}

	std::vector<std::uint32_t> got;
	if (!readback_rgba8(gpu, mask, kW, kH, got))
		return false;

	const std::uint32_t want = unorm8_rgba(0.5f, 0.5f, 0.5f, 1.0f);
	bool all = got.size() == static_cast<std::size_t>(kW) * kH;
	std::size_t first_bad = 0;
	for (std::size_t i = 0; all && i < got.size(); ++i)
	{
		if (got[i] != want)
		{
			all = false;
			first_bad = i;
		}
	}
	if (!all && !got.empty())
		std::printf("  first mismatching texel [%zu] = 0x%08X, wanted 0x%08X\n", first_bad,
			got[first_bad], want);
	check(all, "EVERY texel carries the requested value — the dispatch stored, it did not merely "
		"record legally, and its bounds covered the whole rect rather than one tile");

	// Alpha is not a documented channel; it is written as 1.0 so nothing is uninitialised.
	check((got.empty() ? 0u : (got[0] >> 24)) == 255u, "alpha is the neutral 1.0");

	check(drain_validation(gpu, "nr control mask fill") == 0,
		"no D3D12 validation errors across the fill");

	stray_dlss::nrmask::shutdown();
	check(stray_dlss::nrmask::texture() == nullptr, "shutdown() frees it");
	stray_dlss::nrmask::set_format(stray_dlss::nrmask::kDefaultMaskFormat);
	return true;
}

bool test_nr_control_mask_resize_retires(Gpu &gpu)
{
	std::printf("\n-- DLSSNR.ControlMask: a resize retires, it does not release --\n");

	stray_dlss::nrmask::set_format(static_cast<int>(DXGI_FORMAT_R8G8B8A8_UNORM));
	check(stray_dlss::nrmask::ensure(gpu.device.Get(), 32, 16, /*frame=*/200),
		"a first mask exists");

	stray_dlss::nrmaskplan::Config cfg;
	cfg.enabled = true;
	cfg.channel_r = true;
	cfg.value_r = 0.25f;
	stray_dlss::nrmaskplan::FormatSupport support;
	support.queried = support.view = support.store = true;
	const stray_dlss::nrmaskplan::Plan p32 = stray_dlss::nrmaskplan::plan_mask(cfg, 32, 16,
		static_cast<int>(DXGI_FORMAT_R8G8B8A8_UNORM), support);
	check(stray_dlss::nrmask::record_fill(gpu.list.Get(), p32), "and it fills");
	if (!flush(gpu))
		return false;

	ID3D12Resource *first = stray_dlss::nrmask::texture();
	if (first == nullptr)
		return false;
	first->AddRef(); // this test's own keep-alive

	check(stray_dlss::nrmask::ensure(gpu.device.Get(), 64, 32, /*frame=*/200),
		"a resize allocates a new one");
	check(stray_dlss::nrmask::texture() == nullptr,
		"and the new one is UNFILLED, so it is not offered — a resize must never hand the "
		"runtime the previous rect's contents in a new allocation");
	check(stage_foreign_refs(first) > 0,
		"the superseded texture is still HELD: a fill recorded frames ago may still be reading "
		"it, and so may an evaluate");

	stray_dlss::nrmask::collect(201);
	check(stage_foreign_refs(first) > 0, "one present later it is still held");
	stray_dlss::nrmask::collect(200 + stray_dlss::ring::kFrameCount * 2);
	check(stage_foreign_refs(first) == 0, "two ring cycles later collect() has released it");

	first->Release();
	stray_dlss::nrmask::shutdown();
	check(drain_validation(gpu, "nr control mask lifetime") == 0,
		"no D3D12 validation errors across the retire/collect sequence");
	stray_dlss::nrmask::set_format(stray_dlss::nrmask::kDefaultMaskFormat);
	return true;
}
