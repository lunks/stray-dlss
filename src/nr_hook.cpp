#include "nr_hook.hpp"

#include "intercept/backend.hpp"
#include "log.hpp"
#include "mv_resolve.hpp"
#include "ngx_nr.hpp"
#include "nr_stage.hpp"
#include "perf.hpp"

#include <d3d12.h>

#include <atomic>
#include <cstdio>
#include <memory>
#include <mutex>

namespace stray_dlss::nrhook {
namespace {

// D3D12_RESOURCE_STATE_PRESENT == D3D12_RESOURCE_STATE_COMMON == 0. See nr_hook.hpp.
std::atomic<std::uint32_t> g_bb_state{ 0 };

// --- the guides the TAA path publishes, and the lock that carries them across threads ---
//
// note_guides runs on whatever thread recorded the TAA dispatch; the stage runs on the present
// thread. A torn read here would pair one frame's depth with another's motion vectors.
std::mutex g_guides_mutex;
struct Guides
{
	bool published = false;
	// A PUBLICATION COUNTER, not a present index — see nrplan::GuideState. Freshness is "has this
	// capture been consumed yet?", which is independent of the order in which a host fires its
	// present callback relative to the frame's TAA dispatch.
	std::uint64_t sequence = 0;
	ID3D12Resource *depth = nullptr;
	ID3D12Resource *motion_vectors = nullptr;
	std::uint32_t render_width = 0;
	std::uint32_t render_height = 0;
	bool reset = false;
};
Guides g_guides;
std::atomic<std::uint64_t> g_consumed{ 0 }; // highest sequence the stage has used

// The consumer's own reference on a published guide, held for exactly as long as the stage uses
// the pointer.
//
// WITHOUT THIS the publication references are not enough: on_present copies `g_guides` under the
// lock and then uses the pointers with the lock released, and note_guides — on a different thread
// — could publish in that window and drop the last reference to what we are still handing to NGX.
// The window is small and the resources are usually the game's, which is exactly the shape of
// race that shows up once a week as an unexplained fault.
struct GuideRef
{
	ID3D12Resource *p = nullptr;
	explicit GuideRef(ID3D12Resource *r) : p(r)
	{
		if (p != nullptr)
			p->AddRef();
	}
	~GuideRef()
	{
		if (p != nullptr)
			p->Release();
	}
	GuideRef(const GuideRef &) = delete;
	GuideRef &operator=(const GuideRef &) = delete;
};

// --- counters ---
std::atomic<std::uint64_t> g_triggered{ 0 };
std::atomic<std::uint64_t> g_applied{ 0 };
std::atomic<std::uint32_t> g_reasons[nrplan::kPlanResultCount] = {};
std::atomic<std::uint32_t> g_bb_w{ 0 };
std::atomic<std::uint32_t> g_bb_h{ 0 };
std::atomic<std::uint32_t> g_bb_fmt{ 0 };
std::atomic<float> g_scale_x{ 0.0f };
std::atomic<float> g_scale_y{ 0.0f };

// One counted, once-per-reason-logged refusal, in the shape the TAA path's log_gate_refusal uses.
// A stage that never fires must never be indistinguishable from a stage that fired and did
// nothing.
bool refuse(nrplan::PlanResult reason, const char *detail)
{
	const auto i = static_cast<int>(reason);
	if (i >= 0 && i < nrplan::kPlanResultCount)
	{
		const std::uint32_t before = g_reasons[i].fetch_add(1, std::memory_order_relaxed);
		if (before == 0)
			STRAY_LOG_WARN("NR stage: refused (%s) — %s. First occurrence of this reason only; "
				"the running total is in the periodic NR STAGE line.",
				nrplan::plan_result_name(reason), detail);
	}
	return false;
}

// The back buffer's transitions, routed through the seam so the present list is actually
// submitted. See nrstage::BarrierFn for why this indirection exists at all.
struct BarrierCtx
{
	const icept::PresentContext *pc = nullptr;
};

void issue_barrier(void *ctx, ID3D12Resource *res, std::uint32_t before, std::uint32_t after)
{
	auto *bc = static_cast<BarrierCtx *>(ctx);
	if (bc == nullptr || bc->pc == nullptr || res == nullptr)
		return;
	icept::backend()->present_barrier(*bc->pc, reinterpret_cast<icept::ResourceId>(res), before,
		after);
}

} // namespace



void set_back_buffer_state(std::uint32_t states)
{
	g_bb_state.store(states, std::memory_order_relaxed);
}

std::uint32_t back_buffer_state() { return g_bb_state.load(std::memory_order_relaxed); }

void note_guides(ID3D12Resource *depth, ID3D12Resource *motion_vectors,
                 std::uint32_t render_width, std::uint32_t render_height, bool reset)
{
	// NOTHING AT ALL AT THE DEFAULT SITE. The TAA hook calls this every frame so that no new
	// wiring is needed if the mode ever becomes live-toggleable, but at `taa` the promise is that
	// the shipped configuration is byte-identical — and the references taken below, however cheap,
	// are not nothing. One relaxed atomic load is.

	std::lock_guard<std::mutex> lock(g_guides_mutex);

	// A REFERENCE IS TAKEN HERE, and the reason is the gap this site introduces.
	//
	// At the TAA site the guides are used in the same callback that produced them. Here they are
	// published from a recording thread and consumed at Present, and UE4 rotates its pooled render
	// targets constantly — so `depth` could in principle be destroyed in between, and NGX would
	// then build a descriptor from a dead ID3D12Resource*. That is the CPU-side half of
	// CLAUDE.md §5's descriptor hazard, and it faults inside the driver rather than returning an
	// error.
	//
	// nr::apply's keep-alive ring covers the GPU-side half (it AddRefs everything it evaluates
	// with and releases behind the lifetime fence), but it can only do so once the resource has
	// survived long enough to be handed to it. This pair of references covers publish -> consume.
	//
	// The previous frame's are dropped HERE, on the same recording thread that would have
	// destroyed them anyway, and never on the present thread.
	if (depth != nullptr)
		depth->AddRef();
	if (motion_vectors != nullptr)
		motion_vectors->AddRef();
	if (g_guides.depth != nullptr)
		g_guides.depth->Release();
	if (g_guides.motion_vectors != nullptr)
		g_guides.motion_vectors->Release();

	g_guides.published = true;
	++g_guides.sequence;
	g_guides.depth = depth;
	g_guides.motion_vectors = motion_vectors;
	g_guides.render_width = render_width;
	g_guides.render_height = render_height;
	g_guides.reset = reset;
}

void on_present(const icept::PresentContext &pc, ID3D12Device *device)
{
	// Inert and free at every other mode: no counters, no probe, no allocation.

	// Retire staging textures a resize superseded, whatever this frame then decides. Cheap, and it
	// must happen even on a frame the gate refuses.
	nrstage::collect(pc.frame);

	if (!nr::enabled())
		return;

	g_triggered.fetch_add(1, std::memory_order_relaxed);

	ID3D12GraphicsCommandList *cmd = pc.present_list;
	ID3D12Resource *colour = pc.back_buffer;
	if (device == nullptr || cmd == nullptr || colour == nullptr)
	{
		char detail[160];
		std::snprintf(detail, sizeof(detail),
			"the host offered device=%p present_list=%p back_buffer=%p", static_cast<void *>(device),
			static_cast<void *>(cmd), static_cast<void *>(colour));
		refuse(nrplan::PlanResult::no_colour, detail);
		return;
	}

	const D3D12_RESOURCE_DESC src = colour->GetDesc();
	g_bb_w.store(static_cast<std::uint32_t>(src.Width), std::memory_order_relaxed);
	g_bb_h.store(src.Height, std::memory_order_relaxed);
	g_bb_fmt.store(static_cast<std::uint32_t>(src.Format), std::memory_order_relaxed);

	const nrp::TypedUavSupport uav = nrstage::probe(device, static_cast<int>(src.Format));

	nrplan::ColourDesc cd;
	cd.width = static_cast<std::uint32_t>(src.Width);
	cd.height = src.Height;
	cd.mip_levels = src.MipLevels;
	cd.array_size = src.DepthOrArraySize;
	cd.sample_count = src.SampleDesc.Count;
	cd.typed_uav_view = uav.queried && uav.view;
	cd.typed_uav_load = uav.queried && uav.load;
	cd.typed_uav_store = uav.queried && uav.store;
	// Liveness is established by IDENTITY: this resource came from the swapchain's own
	// GetBuffer/get_current_back_buffer moments ago, so it cannot be a recycled descriptor
	// pointing at a destroyed resource — the hazard is_resource_live exists to catch
	// (CLAUDE.md §5, "ReShade's view -> resource map outlives the resource").
	cd.live = true;

	nrplan::GuideState gs;
	Guides guides;
	// Taken INSIDE the lock, so the reference is on the resource the copy names. See GuideRef.
	std::unique_ptr<GuideRef> depth_ref, motion_ref;
	{
		std::lock_guard<std::mutex> lock(g_guides_mutex);
		guides = g_guides;
		depth_ref = std::make_unique<GuideRef>(guides.depth);
		motion_ref = std::make_unique<GuideRef>(guides.motion_vectors);
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
		char detail[256];
		std::snprintf(detail, sizeof(detail),
			"back buffer %ux%u fmt=%u mips=%u slices=%u samples=%u uav(view=%d load=%d store=%d); "
			"guides published=%d seq=%llu (consumed %llu) %ux%u",
			cd.width, cd.height, static_cast<unsigned int>(src.Format), cd.mip_levels,
			cd.array_size, cd.sample_count, cd.typed_uav_view ? 1 : 0, cd.typed_uav_load ? 1 : 0,
			cd.typed_uav_store ? 1 : 0, gs.published ? 1 : 0,
			static_cast<unsigned long long>(gs.sequence),
			static_cast<unsigned long long>(consumed), gs.render_width, gs.render_height);
		refuse(plan.result, detail);
		return;
	}
	// Claim the capture. One attempt per TAA dispatch, whatever nr::apply then decides — a warmup
	// refusal has still spent the frame.
	g_consumed.store(gs.sequence, std::memory_order_relaxed);
	g_scale_x.store(plan.mvec_scale_x, std::memory_order_relaxed);
	g_scale_y.store(plan.mvec_scale_y, std::memory_order_relaxed);

	if (!nrstage::ensure(device, plan.width, plan.height, static_cast<int>(src.Format), pc.frame))
	{
		refuse(nrplan::PlanResult::staging_failed, nrstage::last_error());
		return;
	}
	ID3D12Resource *staging = nrstage::staging();

	BarrierCtx bctx;
	bctx.pc = &pc;
	const std::uint32_t bb_state = back_buffer_state();

	// --- read the frame out of the back buffer ---
	nrstage::record_capture(cmd, colour, bb_state, issue_barrier, &bctx);

	nr::ApplyInputs ni;
	ni.site = nr::Site::post_tonemap;
	ni.image = staging;
	ni.render_color = nullptr;
	ni.depth = guides.depth;
	ni.motion_vectors = guides.motion_vectors;
	ni.render_width = guides.render_width;
	ni.render_height = guides.render_height;
	// The COLOUR rect, which at a post-tonemap site is the BACK BUFFER's and not necessarily the
	// TAA output rect the guides were sized against.
	ni.output_width = plan.width;
	ni.output_height = plan.height;
	// The camera-cut OR from the TAA capture (CLAUDE.md §2.8). Feature 18 keeps its own temporal
	// accumulation, so dropping this on the way to a new site would reintroduce exactly the bug
	// the TAA path already fixed once.
	ni.reset = guides.reset;
	// THE HDR CODEC IS BYPASSED HERE and every codec term with it: the back buffer is
	// R10G10B10A2_UNORM with no SetColorSpace1 anywhere, i.e. SDR display-encoded already
	// (docs/STRAY-RENDERING-FACTS.md §33), which is the network's own domain. Encoding it again
	// would apply a transfer that has already been applied. So one_over_pre_exposure stays 0 and
	// pre_exposure_ok stays false — not "unknown", but "there is no pre-exposure here to undo".
	ni.one_over_pre_exposure = 0.0f;
	ni.pre_exposure_ok = false;
	// 0 = "derive", which reaches the same 1.0 the TAA site's live A/B settled on. The gate's
	// colour/guide ratio is REPORTED (plan.mvec_scale_*, in the periodic line) rather than sent,
	// so the two can be compared on the box before either is changed. See nrplan::Plan.
	ni.mvec_scale_x = 0.0f;
	ni.mvec_scale_y = 0.0f;

	// Our dense RG16_FLOAT motion vectors rest in UNORDERED_ACCESS; NGX wants its inputs in
	// NON_PIXEL_SHADER_RESOURCE. Balanced on every path out, like the TAA hook's pair. Recording
	// them on the present list is safe because the resolve dispatch that wrote them went onto the
	// game's list, which executes earlier on this same queue.
	//
	// The DEPTH is deliberately NOT barriered, and that is the same discipline the TAA path runs
	// on: it is the game's own resource and we do not know its current state, so a transition with
	// a guessed StateBefore would be worse than none. UE4 leaves scene depth shader-readable
	// through the post chain.
	mv::transition_output(cmd, /*to_shader_resource=*/true);

	bool applied = false;
	{
		perf::Scope scope(perf::kNgxNr);
		// ext_unhook::repair() is called inside nr::apply and ensure_feature — the ReShade
		// ext-vtable patch can be reinstalled by any QueryInterface at any time, so the repair is
		// re-applied before every NGX call rather than once (CLAUDE.md §1).
		applied = nr::apply(device, cmd, ni);
	}

	mv::transition_output(cmd, /*to_shader_resource=*/false);

	if (applied)
	{
		nrstage::record_writeback(cmd, colour, bb_state, issue_barrier, &bctx);
		const std::uint64_t n = g_applied.fetch_add(1, std::memory_order_relaxed);
		if (n == 0)
			STRAY_LOG_WARN("NR STAGE: first frame delivered — back buffer %ux%u fmt=%u copied into "
				"our staging texture, DLSS Neural Rendering evaluated over it with the TAA hook's "
				"depth and motion vectors, and the result copied back, all on the present-time "
				"command list after the game's last submission. NO HDR CODEC and no feedback "
				"path: nothing after the tonemapper is carried into the next frame. The assumed "
				"back-buffer state is 0x%X ([STRAYDLSS] NgxNRStageBackBufferState); if the image "
				"or the frame rate goes wrong the moment this line appears, that constant is the "
				"first thing to change. First occurrence only.",
				plan.width, plan.height, static_cast<unsigned int>(src.Format), bb_state);
	}
	// else: nr::apply counted and named its own refusal (warmup, validating, degenerate, ...) and
	// left the staging copy alone. The back buffer was never written, so the frame is the game's
	// own, byte for byte.

	// NO STATE RESTORE, and that is the whole reason this site is viable where `preui` was not.
	// This is OUR command list — the present owner's ring under the native backend
	// (src/backend_native/present_owner.hpp), ReShade's immediate list under the ReShade one — and
	// nothing of the game's is bound on it. There is nothing of the game's to restore. Calling
	// restore_game_compute_state here would be actively WRONG: that list's tracked state is ours
	// or ReShade's, and replaying it is not a restore of anything.
}

void shutdown()
{
	nrstage::shutdown();
	std::lock_guard<std::mutex> lock(g_guides_mutex);
	// The publish-side references (note_guides) go here. Device destruction is the one call site
	// where the caller, not us, has established that the GPU is idle.
	if (g_guides.depth != nullptr)
		g_guides.depth->Release();
	if (g_guides.motion_vectors != nullptr)
		g_guides.motion_vectors->Release();
	g_guides = Guides{};
	g_consumed.store(0, std::memory_order_relaxed);
}

Counters counters()
{
	Counters c;
	c.triggered = g_triggered.load(std::memory_order_relaxed);
	c.applied = g_applied.load(std::memory_order_relaxed);
	for (int i = 0; i < nrplan::kPlanResultCount; ++i)
		c.reasons[i] = g_reasons[i].load(std::memory_order_relaxed);
	c.last_back_buffer_width = g_bb_w.load(std::memory_order_relaxed);
	c.last_back_buffer_height = g_bb_h.load(std::memory_order_relaxed);
	c.last_back_buffer_format = g_bb_fmt.load(std::memory_order_relaxed);
	c.last_mvec_scale_x = g_scale_x.load(std::memory_order_relaxed);
	c.last_mvec_scale_y = g_scale_y.load(std::memory_order_relaxed);
	c.staging_bytes = nrstage::stats().bytes;
	return c;
}

} // namespace stray_dlss::nrhook
