#include "nr_hook.hpp"

#include "frame_state.hpp"
#include "log.hpp"
#include "mv_resolve.hpp"
#include "ngx_nr.hpp"
#include "nr_codec_pass.hpp"
#include "perf.hpp"

#include <d3d12.h>

#include <state_tracking.hpp>

#include <atomic>
#include <cstdio>
#include <mutex>
#include <vector>

namespace stray_dlss::nrhook {
namespace {

// THE STATE THE COLOUR TARGET IS ASSUMED TO BE IN, at each trigger. Both are HARD, and both are
// asserted by a barrier whose StateBefore has to match reality — under vkd3d-proton there is no
// debug layer to object, so getting one wrong is silent undefined behaviour.
//
//  present — ReShade v6.8.0 `runtime::on_present` (source/runtime.cpp:745) barriers the back
//            buffer `present -> render_target` immediately before calling `render_effects`, which
//            fires our event at runtime.cpp:4020 having issued no further barriers. The
//            `_back_buffer_resolved` branch (MSAA / sRGB variant) puts ITS target into
//            render_target too, at :715/:721. Either way: RENDER_TARGET.
//            reshade_api.hpp:88 states the same contract in prose: "The resource the render
//            target views point to has to be in the resource_usage::render_target state."
//
//  preui   — we fire BEFORE the OMSetRenderTargets that would bind the back buffer again, so the
//            pass that just finished left it as a render target. RENDER_TARGET.
constexpr D3D12_RESOURCE_STATES kColourAssumedState = D3D12_RESOURCE_STATE_RENDER_TARGET;

// The state our staging copy lives in between frames, and the state NVIDIA's guide wants an NGX
// input in (docs/RESEARCH.md §3.5). Keeping the resting state equal to the NGX state means the
// steady-state frame records two transition pairs and no more.
constexpr D3D12_RESOURCE_STATES kStagingRestState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

// Presents a retired staging texture is held for before release, matching the discipline in
// mv_resolve / gbuffer_resolve / nr_codec_pass: the GPU may still be reading it.
constexpr std::uint64_t kRetireLatency = 8;

// How often the periodic diagnostic goes out, in presents.
constexpr std::uint64_t kReportInterval = 600;

// The first N presents in which every back-buffer render-target bind is logged individually. This
// is the census that settles NgxNRPreUiBind: it names, for one frame, exactly how many binds
// there are and what each one's target looks like. Two frames is enough and costs nothing after.
constexpr std::uint64_t kBindCensusPresents = 2;

std::atomic<int> g_mode{ static_cast<int>(nrplan::HookMode::taa) };
std::atomic<std::uint32_t> g_preui_ordinal{ 2 };
std::atomic<bool> g_restore_state{ true };

// --- the guides the TAA path publishes, and the lock that carries them across threads ---
//
// note_guides runs on whatever thread recorded the TAA dispatch; both triggers run on a different
// one (the present thread for `present`, a recording thread for `preui`). A torn read here would
// pair one frame's depth with another's motion vectors.
std::mutex g_guides_mutex;
struct Guides
{
	bool published = false;
	// A PUBLICATION COUNTER, not a present index — see nrplan::GuideState. Freshness is "has this
	// capture been consumed yet?", which is independent of the order in which ReShade fires
	// addon_event::present and addon_event::reshade_begin_effects.
	std::uint64_t sequence = 0;
	ID3D12Resource *depth = nullptr;
	ID3D12Resource *motion_vectors = nullptr;
	std::uint32_t render_width = 0;
	std::uint32_t render_height = 0;
	bool reset = false;
};
Guides g_guides;
std::uint64_t g_guide_sequence = 0;      // advanced by note_guides, under g_guides_mutex
std::atomic<std::uint64_t> g_consumed{ 0 }; // highest sequence a trigger has used

// --- swapchain back-buffer identities ---
//
// The `preui` boundary is a RENDER-TARGET IDENTITY test, so this is the whole lookup table. Read
// from a recording thread, written from ReShade's swapchain callbacks.
std::mutex g_swapchain_mutex;
std::vector<std::uint64_t> g_back_buffers;

// --- per-frame state ---
std::atomic<std::uint64_t> g_frame{ 0 };
std::atomic<std::uint32_t> g_bind_ordinal{ 0 }; // back-buffer RTV binds so far this frame
std::atomic<bool> g_ran_this_frame{ false };

// --- counters ---
std::atomic<std::uint64_t> g_triggered{ 0 };
std::atomic<std::uint64_t> g_applied{ 0 };
std::atomic<std::uint32_t> g_reasons[nrplan::kPlanResultCount] = {};
std::atomic<std::uint64_t> g_begin_effects{ 0 };
std::atomic<std::uint32_t> g_last_binds{ 0 };
std::atomic<std::uint32_t> g_max_binds{ 0 };
std::atomic<std::uint32_t> g_frames_without_boundary{ 0 };

// --- the staging copy of the colour target ---
//
// WHY A COPY AT ALL, rather than handing NGX the back buffer directly. Two reasons, and the first
// is sufficient: NGX needs to WRITE its answer through a UAV, and a swapchain buffer is not
// created with ALLOW_UNORDERED_ACCESS, so it cannot be the DLSSNR.Output. The second is that a
// swapchain allocation is a special one, and handing it to a CUDA-interop texture object is
// exactly the kind of thing that hangs rather than errors on this stack.
//
// WHY THE BACK BUFFER'S OWN FORMAT rather than FP16. A copy cannot convert formats, so an FP16
// staging pair would need a conversion compute pass in EACH direction — and the write-back one
// would still need typed UAV store on the back buffer's format, so it would not even avoid the
// probe. Staging in the native format makes both transfers plain same-format copies with no
// shader of ours anywhere in the path, which is both less code and less to be wrong about. The
// image is display-referred either way; there is no HDR range here to lose.
struct Staging
{
	ID3D12Resource *texture = nullptr;
	std::uint32_t width = 0;
	std::uint32_t height = 0;
	DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
	std::uint64_t bytes = 0;
};
Staging g_staging;

struct Retired
{
	ID3D12Resource *texture = nullptr;
	std::uint64_t frame = 0;
};
std::vector<Retired> g_retired;

// Formats whose typed-UAV verdict has already been logged, so the probe result is stated once
// rather than every frame.
DXGI_FORMAT g_probed_format = DXGI_FORMAT_UNKNOWN;
nrp::TypedUavSupport g_probe;

char g_last_error[192] = "";

void release(ID3D12Resource *&p)
{
	if (p != nullptr)
	{
		p->Release();
		p = nullptr;
	}
}

// One counted, once-per-reason-logged refusal, in the shape the TAA path's log_gate_refusal uses.
// A hook that never fires must never be indistinguishable from a hook that fired and did nothing.
bool refuse(nrplan::PlanResult reason, const char *detail)
{
	const auto i = static_cast<int>(reason);
	if (i >= 0 && i < nrplan::kPlanResultCount)
	{
		const std::uint32_t before = g_reasons[i].fetch_add(1, std::memory_order_relaxed);
		if (before == 0)
			STRAY_LOG_WARN("NR hook [%s]: refused (%s) — %s. First occurrence of this reason "
				"only; the running total is in the periodic NR HOOK line.",
				nrplan::hook_mode_name(static_cast<nrplan::HookMode>(
					g_mode.load(std::memory_order_relaxed))),
				nrplan::plan_result_name(reason), detail);
	}
	return false;
}

bool ensure_staging(ID3D12Device *device, const D3D12_RESOURCE_DESC &src)
{
	const auto w = static_cast<std::uint32_t>(src.Width);
	const auto h = src.Height;
	if (g_staging.texture != nullptr && g_staging.width == w && g_staging.height == h &&
		g_staging.format == src.Format)
		return true;

	// Retire rather than release: the GPU may still be reading the old one several frames back.
	if (g_staging.texture != nullptr)
	{
		Retired r;
		r.texture = g_staging.texture;
		r.frame = g_frame.load(std::memory_order_relaxed);
		g_retired.push_back(r);
		g_staging.texture = nullptr;
	}

	D3D12_HEAP_PROPERTIES heap = {};
	heap.Type = D3D12_HEAP_TYPE_DEFAULT;

	// PLAIN, TYPED, SINGLE-SUBRESOURCE, and never a mirror of what the swapchain allocated. A
	// mipped texture handed to feature 18 is a documented DXGI_ERROR_DEVICE_HUNG arriving seconds
	// later rather than an error return; the same lesson as ngx_nr::ensure_output_texture.
	D3D12_RESOURCE_DESC tex = {};
	tex.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	tex.Width = w;
	tex.Height = h;
	tex.DepthOrArraySize = 1;
	tex.MipLevels = 1;
	tex.Format = src.Format;
	tex.SampleDesc.Count = 1;
	tex.SampleDesc.Quality = 0;
	tex.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	// ALLOW_UNORDERED_ACCESS because ngx_nr clones this desc for DLSSNR.Output, which NGX writes
	// through a UAV — and a missing flag there is a black result with no error at all.
	tex.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

	const HRESULT hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &tex,
		kStagingRestState, nullptr, IID_PPV_ARGS(&g_staging.texture));
	if (FAILED(hr))
	{
		std::snprintf(g_last_error, sizeof(g_last_error),
			"CreateCommittedResource(nr staging %ux%u fmt=%d) hr=0x%08x", w, h,
			static_cast<int>(src.Format), static_cast<unsigned int>(hr));
		STRAY_LOG_ERROR("NR hook: %s. NR stays off this frame; the image is untouched.",
			g_last_error);
		g_staging.width = 0;
		g_staging.height = 0;
		return false;
	}

	g_staging.width = w;
	g_staging.height = h;
	g_staging.format = src.Format;
	// Two textures per pixel: this staging copy and ngx_nr's neural output, which is cloned from
	// it. Reported so the allocation is visible the way mv_resolve's and the codec's are.
	g_staging.bytes = static_cast<std::uint64_t>(w) * h * 8;
	STRAY_LOG_INFO("NR hook: staging colour texture %ux%u fmt=%d created "
		"(single-mip/single-slice/non-MSAA, ALLOW_UNORDERED_ACCESS). It is a plain same-format "
		"copy of the frame's colour target — no conversion, no shader of ours in the path.",
		w, h, static_cast<int>(src.Format));
	return true;
}

void transition(ID3D12GraphicsCommandList *cmd, ID3D12Resource *res, D3D12_RESOURCE_STATES from,
                D3D12_RESOURCE_STATES to)
{
	if (from == to)
		return;
	D3D12_RESOURCE_BARRIER b = {};
	b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	b.Transition.pResource = res;
	b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	b.Transition.StateBefore = from;
	b.Transition.StateAfter = to;
	cmd->ResourceBarrier(1, &b);
}

// Restores what our pass replaced on the GAME's command list. Only reached from `preui`; see
// run_on_colour for why `present` deliberately restores nothing.
void restore_after_preui(reshade::api::command_list *cmd_list)
{
	perf::Scope scope(perf::kRestore);

	// Descriptor heaps, root signatures, tables, root arguments and the PSO — the same helper the
	// TAA path uses, which is exercised against WARP in CI and is the only thing that correctly
	// replays UE4's ROOT descriptors (ReShade's own state_block cannot: it registers no
	// push_descriptors handler). See src/frame_state.hpp for the full argument.
	restore_game_compute_state(cmd_list);

	// Viewports and scissors on top. NGX clobbers them and `restore_game_compute_state`
	// deliberately does not touch graphics dynamic state. UE 4.27's RHI does set a viewport in
	// RHISetRenderTargets — which is the very command we are recording in front of — so this is
	// belt and braces rather than the load-bearing part, and it is six lines.
	//
	// Render targets are NOT restored here on purpose: the next command IS the
	// OMSetRenderTargets we intercepted, so re-binding them would be redundant work whose only
	// effect would be to make the game's own bind look redundant to ReShade's cache.
	if (const auto *state = cmd_list->get_private_data<state_tracking>())
	{
		if (!state->viewports.empty())
			cmd_list->bind_viewports(0, static_cast<uint32_t>(state->viewports.size()),
				state->viewports.data());
		if (!state->scissor_rects.empty())
			cmd_list->bind_scissor_rects(0, static_cast<uint32_t>(state->scissor_rects.size()),
				state->scissor_rects.data());
	}
}

// THE ONE ENTRY POINT BOTH TRIGGERS SHARE. Acquiring a colour resource and its rect is the
// trigger's job; everything from here down is identical whichever one supplied it, which is what
// keeps a third trigger from needing a third copy of this.
bool run_on_colour(reshade::api::command_list *cmd_list, ID3D12Resource *colour,
                   nrplan::HookMode mode)
{
	auto *device = reinterpret_cast<ID3D12Device *>(cmd_list->get_device()->get_native());
	auto *native = reinterpret_cast<ID3D12GraphicsCommandList *>(cmd_list->get_native());
	if (device == nullptr || native == nullptr)
		return refuse(nrplan::PlanResult::no_colour,
			"the command list gave us no native device or list");

	const D3D12_RESOURCE_DESC src = colour->GetDesc();

	// The typed-UAV probe, through nr_codec_pass's single implementation. The verdict is logged
	// the first time each format is seen, because "the back buffer is R10G10B10A2_UNORM and the
	// device cannot store to it through a typed UAV" is precisely the kind of fact that must not
	// have to be inferred from a black screen (CLAUDE.md §0.2).
	if (g_probed_format != src.Format)
	{
		g_probed_format = src.Format;
		g_probe = nrp::probe_typed_uav(device, static_cast<int>(src.Format));
		STRAY_LOG_INFO("NR hook: typed-UAV probe on the colour target's format %d — queried=%d "
			"typedUavView=%d typedLoad=%d typedStore=%d. NGX writes DLSSNR.Output through a typed "
			"UAV, so VIEW and STORE decide; a missing typed LOAD is harmless here because the "
			"runtime reads DLSSNR.Color through its own path.",
			static_cast<int>(src.Format), g_probe.queried ? 1 : 0, g_probe.view ? 1 : 0,
			g_probe.load ? 1 : 0, g_probe.store ? 1 : 0);
		if (g_probe.queried && g_probe.view && g_probe.store && !g_probe.load)
			STRAY_LOG_WARN("NR hook: the colour format supports typed UAV STORE but not typed "
				"LOAD. That is expected for some formats and is not a refusal — noted only so a "
				"later problem is not blamed on it.");
	}

	nrplan::ColourDesc cd;
	cd.width = static_cast<std::uint32_t>(src.Width);
	cd.height = src.Height;
	cd.mip_levels = src.MipLevels;
	cd.array_size = src.DepthOrArraySize;
	cd.sample_count = src.SampleDesc.Count;
	cd.typed_uav_view = g_probe.queried && g_probe.view;
	cd.typed_uav_load = g_probe.queried && g_probe.load;
	cd.typed_uav_store = g_probe.queried && g_probe.store;
	// Liveness is established by IDENTITY rather than by frame_state's live set: for `preui` the
	// resource is one the swapchain owns (we matched it against the swapchain's own back-buffer
	// list), and for `present` ReShade has just handed it to us and is about to render into it.
	// Neither can be a recycled descriptor pointing at a destroyed resource, which is the hazard
	// is_resource_live exists to catch (CLAUDE.md §5).
	cd.live = true;

	nrplan::GuideState gs;
	Guides guides;
	{
		std::lock_guard<std::mutex> lock(g_guides_mutex);
		guides = g_guides;
	}
	gs.published = guides.published;
	gs.have_depth = guides.depth != nullptr;
	gs.have_motion = guides.motion_vectors != nullptr;
	gs.sequence = guides.sequence;
	gs.render_width = guides.render_width;
	gs.render_height = guides.render_height;

	const std::uint64_t consumed = g_consumed.load(std::memory_order_relaxed);
	const nrplan::Plan plan = nrplan::plan_post_tonemap(cd, gs, consumed);
	if (plan.result != nrplan::PlanResult::ok)
	{
		char detail[224];
		std::snprintf(detail, sizeof(detail),
			"colour %ux%u fmt=%d mips=%u slices=%u samples=%u; guides published=%d seq=%llu "
			"(consumed %llu) %ux%u",
			cd.width, cd.height, static_cast<int>(src.Format), cd.mip_levels, cd.array_size,
			cd.sample_count, gs.published ? 1 : 0,
			static_cast<unsigned long long>(gs.sequence),
			static_cast<unsigned long long>(consumed),
			gs.render_width, gs.render_height);
		return refuse(plan.result, detail);
	}
	// Claim the capture. One attempt per TAA dispatch, whatever nr::apply then decides — a
	// warmup refusal has still spent the frame.
	g_consumed.store(gs.sequence, std::memory_order_relaxed);

	if (!ensure_staging(device, src))
		return refuse(nrplan::PlanResult::staging_failed, g_last_error);

	// --- read the frame out of the colour target ---
	transition(native, colour, kColourAssumedState, D3D12_RESOURCE_STATE_COPY_SOURCE);
	transition(native, g_staging.texture, kStagingRestState, D3D12_RESOURCE_STATE_COPY_DEST);
	native->CopyResource(g_staging.texture, colour);
	transition(native, g_staging.texture, D3D12_RESOURCE_STATE_COPY_DEST, kStagingRestState);
	// Put the colour target back immediately. Holding it in COPY_SOURCE across the evaluate would
	// work but would leave the frame in a state nothing else expects if any path below returns
	// early, and there are several.
	transition(native, colour, D3D12_RESOURCE_STATE_COPY_SOURCE, kColourAssumedState);

	nr::ApplyInputs ni;
	ni.site = nr::Site::post_tonemap;
	ni.image = g_staging.texture;
	ni.render_color = nullptr;
	ni.depth = guides.depth;
	ni.motion_vectors = guides.motion_vectors;
	ni.render_width = guides.render_width;
	ni.render_height = guides.render_height;
	// The COLOUR rect, which at a post-tonemap site is the back buffer's and NOT the TAA output
	// rect the guides were sized against — so the ratio is computed from these two and passed
	// through rather than re-derived inside apply().
	ni.output_width = plan.width;
	ni.output_height = plan.height;
	ni.mvec_scale_x = plan.mvec_scale_x;
	ni.mvec_scale_y = plan.mvec_scale_y;
	// The camera-cut OR from the TAA capture (CLAUDE.md §2.8). Feature 18 keeps its own temporal
	// accumulation, so dropping this on the way to a new hook site would reintroduce exactly the
	// bug the TAA path already fixed once.
	ni.reset = guides.reset;

	// Our dense RG16_FLOAT motion vectors rest in UNORDERED_ACCESS; NGX wants its inputs in
	// NON_PIXEL_SHADER_RESOURCE. Balanced on every path out, like the TAA hook's pair.
	//
	// The DEPTH is deliberately NOT barriered, and that is the same discipline the TAA path
	// already runs on: it is the game's own resource and we do not know its current state, so a
	// transition with a guessed StateBefore would be worse than none. UE4 leaves scene depth
	// shader-readable through the post chain.
	mv::transition_output(native, /*to_shader_resource=*/true);

	bool applied = false;
	{
		perf::Scope scope(perf::kNgxNr);
		// ext_unhook::repair() is called inside nr::apply and ensure_feature — the ReShade
		// ext-vtable patch can be reinstalled by any QueryInterface at any time, so the repair is
		// re-applied before every NGX call rather than once (CLAUDE.md §1).
		applied = nr::apply(device, native, ni);
	}

	mv::transition_output(native, /*to_shader_resource=*/false);

	if (applied)
	{
		// --- write the neural answer back into the frame ---
		transition(native, g_staging.texture, kStagingRestState,
			D3D12_RESOURCE_STATE_COPY_SOURCE);
		transition(native, colour, kColourAssumedState, D3D12_RESOURCE_STATE_COPY_DEST);
		native->CopyResource(colour, g_staging.texture);
		transition(native, colour, D3D12_RESOURCE_STATE_COPY_DEST, kColourAssumedState);
		transition(native, g_staging.texture, D3D12_RESOURCE_STATE_COPY_SOURCE,
			kStagingRestState);
		g_applied.fetch_add(1, std::memory_order_relaxed);
	}
	// else: nr::apply counted and named its own refusal (warmup, validating, degenerate, ...) and
	// left the staging copy untouched. The colour target was never written, so the frame is the
	// engine's own, byte for byte.

	// STATE RESTORE, and the two sites need genuinely different treatment.
	if (mode == nrplan::HookMode::preui)
	{
		// The GAME's command list. UE4 filters its own redundant binds, so a clobber that is
		// never undone survives into the next draw — which is why the TAA path restores, and why
		// this does too.
		if (g_restore_state.load(std::memory_order_relaxed))
			restore_after_preui(cmd_list);
	}
	else
	{
		// RESHADE'S OWN IMMEDIATE COMMAND LIST — a different ID3D12GraphicsCommandList from the
		// game's (v6.8.0 d3d12_impl_command_queue.cpp:22, d3d12_impl_command_list_immediate.cpp:34),
		// so the game's state is untouched by construction and there is nothing of the game's to
		// restore.
		//
		// AND RESHADE'S OWN CACHES DO NOT NEED HELP HERE, which is worth stating because the
		// obvious worry is that they do. `command_list_impl` skips a redundant SetDescriptorHeaps
		// / SetComputeRootSignature by comparing against `_current_descriptor_heaps` /
		// `_current_root_signature` (d3d12_impl_command_list.cpp:538/549), and a native clobber of
		// ours would desync exactly that — except those four members are nulled on every flush of
		// the immediate list (d3d12_impl_command_list_immediate.cpp:127-130), the flush happens
		// once per present (dxgi_swapchain.cpp:1009), and nothing binds through the proxy between
		// the flush and reshade_begin_effects. So the cache reads {nullptr} when we arrive, our
		// clobber cannot make it agree with reality by accident, and ReShade's first per-pass bind
		// (runtime.cpp:4160-4169) therefore issues the real calls.
		//
		// Calling restore_game_compute_state here would be actively WORSE: on this list the
		// state_tracking block holds ReShade's own previous bindings, and replaying those is not
		// a restore of anything.
	}

	return applied;
}

} // namespace

void set_hook_mode(nrplan::HookMode mode)
{
	g_mode.store(static_cast<int>(mode), std::memory_order_relaxed);
}

nrplan::HookMode hook_mode()
{
	return static_cast<nrplan::HookMode>(g_mode.load(std::memory_order_relaxed));
}

void set_preui_bind_ordinal(std::uint32_t ordinal)
{
	g_preui_ordinal.store(ordinal, std::memory_order_relaxed);
}

void set_restore_state(bool enabled)
{
	g_restore_state.store(enabled, std::memory_order_relaxed);
}

void note_guides(std::uint64_t frame, ID3D12Resource *depth, ID3D12Resource *motion_vectors,
                 std::uint32_t render_width, std::uint32_t render_height, bool reset)
{
	(void)frame; // kept in the signature so the caller's intent stays legible at the call site
	std::lock_guard<std::mutex> lock(g_guides_mutex);
	g_guides.published = true;
	g_guides.sequence = ++g_guide_sequence;
	g_guides.depth = depth;
	g_guides.motion_vectors = motion_vectors;
	g_guides.render_width = render_width;
	g_guides.render_height = render_height;
	g_guides.reset = reset;
}

void note_swapchain(reshade::api::swapchain *swapchain)
{
	if (swapchain == nullptr)
		return;
	std::vector<std::uint64_t> buffers;
	const uint32_t count = swapchain->get_back_buffer_count();
	buffers.reserve(count);
	for (uint32_t i = 0; i < count; ++i)
		buffers.push_back(swapchain->get_back_buffer(i).handle);

	{
		std::lock_guard<std::mutex> lock(g_swapchain_mutex);
		g_back_buffers.swap(buffers);
	}
	STRAY_LOG_INFO("NR hook: cached %u swapchain back-buffer identities. The `preui` boundary is "
		"a render-target IDENTITY test against this list, not a shader-bytecode match.", count);
}

void forget_swapchain(reshade::api::swapchain *swapchain)
{
	(void)swapchain;
	std::lock_guard<std::mutex> lock(g_swapchain_mutex);
	g_back_buffers.clear();
}

void on_present(std::uint64_t frame)
{
	const auto mode = hook_mode();
	const std::uint32_t binds = g_bind_ordinal.exchange(0, std::memory_order_relaxed);
	const bool ran = g_ran_this_frame.exchange(false, std::memory_order_relaxed);
	g_frame.store(frame, std::memory_order_relaxed);

	if (mode == nrplan::HookMode::preui)
	{
		g_last_binds.store(binds, std::memory_order_relaxed);
		std::uint32_t seen = g_max_binds.load(std::memory_order_relaxed);
		while (binds > seen &&
			!g_max_binds.compare_exchange_weak(seen, binds, std::memory_order_relaxed))
		{
		}
		// FAIL SAFE, and it is counted so it cannot pass unnoticed: a frame whose back-buffer
		// bind count never reached the configured ordinal is skipped rather than injected at a
		// guessed point. A missed frame is invisible; injecting into the middle of the HUD is not.
		if (!ran && binds < g_preui_ordinal.load(std::memory_order_relaxed))
		{
			g_frames_without_boundary.fetch_add(1, std::memory_order_relaxed);
			g_reasons[static_cast<int>(nrplan::PlanResult::boundary_not_reached)].fetch_add(1,
				std::memory_order_relaxed);
		}
	}

	// Retire staging allocations whose GPU work has certainly drained.
	for (auto it = g_retired.begin(); it != g_retired.end();)
	{
		if (frame >= it->frame + kRetireLatency)
		{
			release(it->texture);
			it = g_retired.erase(it);
		}
		else
		{
			++it;
		}
	}

	if (mode == nrplan::HookMode::taa || frame == 0 || (frame % kReportInterval) != 0)
		return;

	const Counters c = counters();
	char reasons[256] = "";
	int off = 0;
	for (int i = 0; i < nrplan::kPlanResultCount; ++i)
	{
		if (c.reasons[i] == 0)
			continue;
		const int n = std::snprintf(reasons + off, sizeof(reasons) - static_cast<std::size_t>(off),
			"%s%s=%u", off == 0 ? "" : " ",
			nrplan::plan_result_name(static_cast<nrplan::PlanResult>(i)), c.reasons[i]);
		if (n <= 0 || off + n >= static_cast<int>(sizeof(reasons)))
			break;
		off += n;
	}
	STRAY_LOG_INFO("NR HOOK [%s] frame %llu: triggered=%llu applied=%llu | %s | "
		"backBufferRtvBinds last=%u max=%u (NgxNRPreUiBind=%u) framesWithoutBoundary=%u | "
		"beginEffectsSeen=%llu | staging %llu bytes",
		nrplan::hook_mode_name(mode), static_cast<unsigned long long>(frame),
		static_cast<unsigned long long>(c.triggered), static_cast<unsigned long long>(c.applied),
		reasons[0] != 0 ? reasons : "no refusals",
		c.last_backbuffer_binds, c.max_backbuffer_binds,
		g_preui_ordinal.load(std::memory_order_relaxed), c.frames_without_boundary,
		static_cast<unsigned long long>(c.begin_effects_seen),
		static_cast<unsigned long long>(c.staging_bytes));

	// THE ONE FAILURE THAT LOOKS LIKE NOTHING AT ALL. ReShade calls render_effects only when
	// `!is_loading() && !_techniques.empty()` (v6.8.0 runtime.cpp:737, and again at :3810), so a
	// preset with NO effect files loaded never fires reshade_begin_effects and `present` mode is
	// silently inert. Say so rather than let it read as "NR is broken".
	if (mode == nrplan::HookMode::present && c.begin_effects_seen == 0)
		STRAY_LOG_ERROR("NgxNRHook=present but reshade_begin_effects has NEVER fired. ReShade "
			"only renders effects when at least one effect file is LOADED (v6.8.0 "
			"runtime.cpp:737 / :3810 gate on `!_techniques.empty()`), so with an empty preset "
			"this hook site does not exist. Load any effect (it may stay disabled), or use "
			"NgxNRHook=preui, which does not depend on ReShade's effect runtime at all.");
}

void on_begin_effects(reshade::api::effect_runtime *runtime, reshade::api::command_list *cmd_list,
                      reshade::api::resource_view rtv, reshade::api::resource_view rtv_srgb)
{
	(void)runtime;
	(void)rtv_srgb;
	g_begin_effects.fetch_add(1, std::memory_order_relaxed);

	if (hook_mode() != nrplan::HookMode::present || cmd_list == nullptr || rtv.handle == 0)
		return;
	if (g_ran_this_frame.exchange(true, std::memory_order_relaxed))
	{
		g_reasons[static_cast<int>(nrplan::PlanResult::already_ran)].fetch_add(1,
			std::memory_order_relaxed);
		return;
	}

	// The resource behind the non-sRGB RTV. This is the back buffer on the ordinary path and
	// ReShade's `_back_buffer_resolved` on the MSAA / sRGB-variant path (v6.8.0
	// runtime.cpp:739-747); either is the right target, because ReShade composites the resolved
	// one back into the swapchain afterwards.
	auto *device = cmd_list->get_device();
	if (device == nullptr)
		return;
	const reshade::api::resource res = device->get_resource_from_view(rtv);
	if (res.handle == 0)
	{
		refuse(nrplan::PlanResult::no_colour, "the effect-time RTV resolved to no resource");
		return;
	}

	g_triggered.fetch_add(1, std::memory_order_relaxed);
	run_on_colour(cmd_list, reinterpret_cast<ID3D12Resource *>(res.handle),
		nrplan::HookMode::present);
}

void on_bind_render_targets(reshade::api::command_list *cmd_list, std::uint32_t count,
                            const reshade::api::resource_view *rtvs,
                            reshade::api::resource_view dsv)
{
	(void)dsv;
	// The hot path: this fires on every OMSetRenderTargets in the frame, so the mode check is
	// first and is a relaxed load of one int.
	if (hook_mode() != nrplan::HookMode::preui || cmd_list == nullptr || count == 0 ||
		rtvs == nullptr || rtvs[0].handle == 0)
		return;

	auto *device = cmd_list->get_device();
	if (device == nullptr)
		return;
	const reshade::api::resource res = device->get_resource_from_view(rtvs[0]);
	if (res.handle == 0)
		return;

	{
		std::lock_guard<std::mutex> lock(g_swapchain_mutex);
		bool is_back_buffer = false;
		for (const std::uint64_t h : g_back_buffers)
		{
			if (h == res.handle)
			{
				is_back_buffer = true;
				break;
			}
		}
		if (!is_back_buffer)
			return;
	}

	const std::uint32_t ordinal = g_bind_ordinal.fetch_add(1, std::memory_order_relaxed) + 1;
	const std::uint64_t frame = g_frame.load(std::memory_order_relaxed);
	const bool already = g_ran_this_frame.load(std::memory_order_relaxed);

	// THE CENSUS THAT SETTLES NgxNRPreUiBind. Two frames of it, then silence. One run of this
	// says how many back-buffer render-target binds the frame really has and how big each one's
	// target is, which is the whole question the ordinal encodes — otherwise it is a round trip
	// per guess.
	if (frame < kBindCensusPresents)
	{
		auto *r = reinterpret_cast<ID3D12Resource *>(res.handle);
		const D3D12_RESOURCE_DESC d = r->GetDesc();
		STRAY_LOG_INFO("NR hook census: frame %llu, back-buffer render-target bind #%u — target "
			"%p %llux%u fmt=%d, %u RTV(s) bound. Bind #%u is where NgxNRPreUiBind would inject; "
			"UE4 composites the tonemapped scene in one bind and Slate draws the HUD in the next.",
			static_cast<unsigned long long>(frame), ordinal, static_cast<void *>(r),
			static_cast<unsigned long long>(d.Width), d.Height, static_cast<int>(d.Format), count,
			g_preui_ordinal.load(std::memory_order_relaxed));
	}

	if (!nrplan::preui_boundary(ordinal, g_preui_ordinal.load(std::memory_order_relaxed), already))
		return;

	g_ran_this_frame.store(true, std::memory_order_relaxed);
	g_triggered.fetch_add(1, std::memory_order_relaxed);

	static bool s_first_logged = false;
	if (!s_first_logged)
	{
		s_first_logged = true;
		STRAY_LOG_WARN("NR hook [preui]: injecting before back-buffer render-target bind #%u of "
			"frame %llu — after the scene composite, before Slate draws the HUD. If the HUD "
			"appears processed, the ordinal is too LOW (raise NgxNRPreUiBind); if the image is "
			"unchanged or the composite is missing, it is too HIGH. Read the census lines above. "
			"First occurrence only.",
			ordinal, static_cast<unsigned long long>(frame));
	}

	run_on_colour(cmd_list, reinterpret_cast<ID3D12Resource *>(res.handle),
		nrplan::HookMode::preui);
}

void shutdown()
{
	for (auto &r : g_retired)
		release(r.texture);
	g_retired.clear();
	release(g_staging.texture);
	g_staging = Staging{};
	g_probed_format = DXGI_FORMAT_UNKNOWN;
	g_probe = nrp::TypedUavSupport{};
	{
		std::lock_guard<std::mutex> lock(g_guides_mutex);
		g_guides = Guides{};
		g_guide_sequence = 0;
	}
	g_consumed.store(0, std::memory_order_relaxed);
	{
		std::lock_guard<std::mutex> lock(g_swapchain_mutex);
		g_back_buffers.clear();
	}
}

Counters counters()
{
	Counters c;
	c.triggered = g_triggered.load(std::memory_order_relaxed);
	c.applied = g_applied.load(std::memory_order_relaxed);
	for (int i = 0; i < nrplan::kPlanResultCount; ++i)
		c.reasons[i] = g_reasons[i].load(std::memory_order_relaxed);
	c.begin_effects_seen = g_begin_effects.load(std::memory_order_relaxed);
	c.last_backbuffer_binds = g_last_binds.load(std::memory_order_relaxed);
	c.max_backbuffer_binds = g_max_binds.load(std::memory_order_relaxed);
	c.frames_without_boundary = g_frames_without_boundary.load(std::memory_order_relaxed);
	c.staging_bytes = g_staging.texture != nullptr ? g_staging.bytes : 0;
	return c;
}

} // namespace stray_dlss::nrhook
