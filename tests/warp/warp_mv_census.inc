// ---- The motion-vector census, on WARP ----
//
// Included by warp_mv_resolve.cpp after its helpers (Gpu, check, HR, drain_validation, flush,
// CameraScene / build_camera_scene / make_view / kMvW / kMvH).
//
// WHAT THIS PROVES, and the two claims are separate:
//
// 1. THE INSTRUMENT IS LEGAL. The root UAV, the LDS reduction with its two group-wide barriers,
//    the counter snapshot and the GPU-side zeroing are all judged by the debug layer and by
//    GPU-based validation. A group barrier that some lanes do not reach is undefined behaviour,
//    and the bounds test in this shader used to be an early `return` — so this is exactly the
//    kind of change that can look correct and be wrong.
//
// 2. THE INSTRUMENT COUNTS THE RIGHT THING. `build_camera_scene` clears the velocity target to
//    ZERO, and UE4's own validity test is the strict `EncodedVelocity.x > 0` (CLAUDE.md §2.5),
//    so EVERY pixel must take the camera-reconstruction branch. `sparse == 0` and
//    `camera == total` is therefore an exact prediction, not a tolerance — and it is the one
//    assertion that would catch the counter ordinals in shaders/mv_resolve.hlsl drifting away
//    from `enum Counter` in src/core/mv_census.hpp, which would silently relabel every number
//    in the report.
//
// WHAT IT CANNOT PROVE: the actual density in Stray. That needs the game, and it is the whole
// point of the feature — see MvStats in mods/StrayDLSS/StrayDLSS.ini.

#include "core/mv_census.hpp"

bool test_mv_census_counts_the_branches(Gpu &gpu)
{
	std::printf("\n[test] the MV census counts branches, and its invariants hold\n");

	CameraScene scene;
	if (!build_camera_scene(gpu, scene, kMvW, kMvH))
		return false;
	drain_validation(gpu, "census-setup");

	if (!stray_dlss::mv::initialise(gpu.device.Get(), kMvW, kMvH))
	{
		fail(stray_dlss::mv::last_error());
		return false;
	}
	stray_dlss::mv::set_signs(1.0f, 1.0f, 1.0f, 1.0f);
	stray_dlss::mv::set_legacy_transposed_clip(false);

	// A one-frame window, so the snapshot is recorded by the very first dispatch and there is
	// nothing to wait for.
	constexpr std::uint32_t kWindow = 1;
	stray_dlss::mv::set_stats(true, kWindow);

	stray_dlss::ue4::ViewParams view = make_view(kMvW, kMvH);
	view.clip_to_prev_clip = make_clip_to_prev_clip();

	stray_dlss::mv::ResolveInputs in;
	in.depth_resource = reinterpret_cast<std::uint64_t>(scene.depth.Get());
	in.velocity_resource = reinterpret_cast<std::uint64_t>(scene.velocity.Get());
	in.render_width = kMvW;
	in.render_height = kMvH;
	in.view = &view;

	const std::uint64_t before = stray_dlss::mv::census_total().windows;

	if (!stray_dlss::mv::record(gpu.list.Get(), in, 2))
	{
		fail(stray_dlss::mv::last_error());
		return false;
	}
	if (!flush(gpu))
		return false;
	check(drain_validation(gpu, "census-dispatch") == 0,
		"no D3D12 validation errors from the census dispatch, its LDS barriers, the counter "
		"snapshot or the GPU-side reset");

	// The readback is paced in presents, deliberately — there is no fence of our own on the
	// game's queue. Drive it past the latency by hand.
	for (int i = 0; i < 8; ++i)
		stray_dlss::mv::on_present();

	const stray_dlss::mvcensus::Census &c = stray_dlss::mv::census_last();
	check(stray_dlss::mv::census_total().windows == before + 1,
		"exactly one census window landed");

	const stray_dlss::mvcensus::Invariant inv = stray_dlss::mvcensus::check(c);
	std::printf("  invariant=%s total=%llu sparse=%llu camera=%llu far=%llu reject=%llu\n",
		stray_dlss::mvcensus::invariant_name(inv),
		static_cast<unsigned long long>(c.c[stray_dlss::mvcensus::kTotal]),
		static_cast<unsigned long long>(c.c[stray_dlss::mvcensus::kSparse]),
		static_cast<unsigned long long>(c.c[stray_dlss::mvcensus::kCamera]),
		static_cast<unsigned long long>(c.c[stray_dlss::mvcensus::kFar]),
		static_cast<unsigned long long>(c.c[stray_dlss::mvcensus::kCameraReject]));
	check(inv == stray_dlss::mvcensus::Invariant::ok,
		"every invariant the report refuses to print without holds on real GPU counters");

	check(c.c[stray_dlss::mvcensus::kTotal] ==
			static_cast<std::uint64_t>(kMvW) * kMvH,
		"the census counted exactly the in-bounds pixels the dispatch covered - so the LDS "
		"reduction is neither dropping groups nor double-counting the padding lanes");

	// The exact prediction. Velocity is cleared to zero, and UE4's validity test is strict.
	check(c.c[stray_dlss::mvcensus::kSparse] == 0,
		"with an all-zero velocity target NOT ONE pixel is called engine-written - "
		"EncodedVelocity.x > 0 is reproduced exactly");
	check(c.c[stray_dlss::mvcensus::kCamera] == c.c[stray_dlss::mvcensus::kTotal],
		"so every pixel took the camera-reconstruction branch");

	char report[768];
	stray_dlss::mvcensus::format_report(report, sizeof(report), c);
	std::printf("  %s\n", report);

	// And back to the shipped default, so no later test measures a shader that is still
	// counting.
	stray_dlss::mv::set_stats(false, 300);
	return true;
}

// ---- The DLSS bias-current-colour mask ----
//
// WHAT THIS PROVES: that the fill and its barriers are LEGAL. The mask is filled with
// `ClearUnorderedAccessViewFloat` rather than a compute dispatch, precisely so there is no
// shader that could record cleanly and store nothing — so the open question here is the
// STATE MACHINE, which is what the debug layer judges: the texture must be in
// UNORDERED_ACCESS for the clear and in NON_PIXEL_SHADER_RESOURCE when NGX reads it, and the
// module tracks that itself because D3D12 cannot be asked.
//
// It also pins the no-op. The mask is re-filled only when the VALUE changes, so a session
// holding one value must record exactly one clear no matter how many frames run. A mask
// re-cleared every frame would put a UAV write and two transitions on the game's list forever.
//
// WHAT IT CANNOT PROVE: whether the DLSS runtime does anything with it. That is the whole
// question, it needs the game, and the alternation exists to answer it in one session.

#include "mv_mask.hpp"

bool test_mv_bias_mask_is_legal(Gpu &gpu)
{
	std::printf("\n[test] the DLSS bias-current-colour mask: fill, states, and the no-op\n");

	drain_validation(gpu, "bias-mask-setup");

	// OFF is the shipped default and must produce nothing at all.
	stray_dlss::mvmask::configure(false, 1.0f, 0, stray_dlss::mvmask::kFormatR8Unorm);
	check(!stray_dlss::mvmask::enabled(), "MvMask defaults OFF");
	check(stray_dlss::mvmask::record(gpu.list.Get(), kMvW, kMvH, 0) == nullptr,
		"and a disabled mask binds NOTHING - byte-identical to before this existed");

	stray_dlss::mvmask::configure(true, 0.5f, /*alternate_period=*/0,
		stray_dlss::mvmask::kFormatR8Unorm);
	check(stray_dlss::mvmask::initialise(gpu.device.Get(), kMvW, kMvH),
		"the mask allocates at the render rect");

	ID3D12Resource *bound = stray_dlss::mvmask::record(gpu.list.Get(), kMvW, kMvH, 1);
	check(bound != nullptr, "and the first frame fills it and binds it");
	check(stray_dlss::mvmask::stats().fills == 1, "exactly one clear so far");
	check(stray_dlss::mvmask::stats().bound == 1, "bound once");

	// The no-op: same value, nothing new to write.
	for (std::uint64_t f = 2; f < 8; ++f)
		check(stray_dlss::mvmask::record(gpu.list.Get(), kMvW, kMvH, f) == bound,
			"the same texture comes back every frame");
	check(stray_dlss::mvmask::stats().fills == 1,
		"and STILL exactly one clear - the mask is re-filled only when the value moves");

	// A value change must re-fill exactly once.
	stray_dlss::mvmask::configure(true, 0.25f, 0, stray_dlss::mvmask::kFormatR8Unorm);
	check(stray_dlss::mvmask::record(gpu.list.Get(), kMvW, kMvH, 8) != nullptr, "still binds");
	check(stray_dlss::mvmask::stats().fills == 2, "and re-filled once for the new value");

	// A render rect the mask cannot cover is REFUSED, not silently sampled out of bounds.
	check(stray_dlss::mvmask::record(gpu.list.Get(), kMvW * 4, kMvH * 4, 9) == nullptr,
		"a render rect larger than the mask is refused");

	if (!flush(gpu))
		return false;
	check(drain_validation(gpu, "bias-mask") == 0,
		"no D3D12 validation errors across the clear, both transitions and the rebind - the "
		"state machine mv_mask.cpp tracks by hand is the thing D3D12 cannot be asked about");

	stray_dlss::mvmask::configure(false, 0.0f, 0, stray_dlss::mvmask::kDefaultFormat);
	stray_dlss::mvmask::shutdown();
	return true;
}
