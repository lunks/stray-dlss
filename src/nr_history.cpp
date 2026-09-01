#include "nr_history.hpp"

#include "intercept/backend.hpp"
#include "log.hpp"
#include "ngx_nr.hpp"

#include <d3d12.h>

#include <atomic>
#include <cstdio>
#include <mutex>
#include <vector>

namespace stray_dlss::nrhist {
namespace {

// --- THE TWO RESOURCE STATES THIS MODULE ASSUMES, and the evidence for each ---
//
// 1. `u0` AT THE SNAPSHOT — HARD.
//
//    The snapshot is recorded inside the intercepted TAA dispatch, immediately after the SR/RR
//    evaluate wrote `u0` through a UAV and before nr::apply. ngx_nr.cpp derives the identical
//    constant for its own barriers on the same resource in the same place (`image_state`, the
//    codec branch), and ngx_nr.hpp's Site::taa_dispatch states it in prose: "raw, unbounded,
//    PRE-EXPOSED LINEAR HDR, in D3D12_RESOURCE_STATE_UNORDERED_ACCESS because the SR/RR evaluate
//    has just written it". We put it straight back, so nothing downstream sees a difference.
constexpr D3D12_RESOURCE_STATES kImageStateAtSnapshot = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

// 2. `u0` AT THE RESTORE — [derived], FOUR SOURCE ANCHORS, NO MEASUREMENT. This is the one
//    genuine hypothesis in this file, and it is overridable for exactly that reason
//    ([STRAYDLSS] NgxNRRestoreState).
//
//    The restore happens at present, on a different command list, long after the engine has
//    transitioned `u0` onward — and D3D12 has no way to ASK what state a resource is in. So the
//    state is derived from UE 4.27's own source:
//
//      a. TemporalAA.cpp:969 extracts the history with the TWO-ARGUMENT overload:
//           GraphBuilder.QueueTextureExtraction(NewHistoryTexture[i], &OutputHistory->RT[i]);
//      b. RenderGraphBuilder.h:161 + :209 — that overload uses
//           static const ERHIAccess kDefaultAccessFinal = ERHIAccess::SRVMask;
//         so RDG's epilogue transitions the texture to SRVMask (SRVCompute | SRVGraphics).
//      c. D3D12Commands.cpp:398-414 (FD3D12CommandContext::RHIEndTransitions) — a READABLE
//         AccessAfter on a non-async-compute context maps to `Resource->GetReadableState()`.
//      d. D3D12Resources.h:377 / :389 — for any writable, SRV-capable texture (RTV branch or UAV
//         branch alike, and UE4's TAA output is both),
//           ReadableState = NON_PIXEL_SHADER_RESOURCE | PIXEL_SHADER_RESOURCE
//
//    Hence 0xC0. What could still make it wrong, in order of plausibility: a later pass
//    transitioning `u0` after the graph epilogue; UE 4.27's D3D12 RHI ALSO tracking state
//    implicitly as resources are bound, so a bind after the epilogue moves it; or a second
//    render graph in the same frame touching the texture.
//
//    HOW BADLY A WRONG GUESS BITES, and it is two things, not one:
//
//      i.  The barrier we record has a StateBefore that does not match reality. On a real D3D12
//          runtime that is a debug-layer error; on THIS target it is softened, because `u0`
//          carries ALLOW_UNORDERED_ACCESS and vkd3d-proton keeps such images in
//          VK_IMAGE_LAYOUT_GENERAL permanently rather than running a D3D12 state machine, so the
//          cost is a wrong srcAccessMask/srcStageMask rather than a corrupt layout.
//      ii. THE ONE THAT DOES NOT WASH OUT: we transition BACK to the same value we claimed, so
//          after our pair the resource really is in `assumed` — while UE 4.27's own per-resource
//          CPU-side state tracking still believes whatever it last set. Its next barrier on this
//          texture then has a mismatched StateBefore too, and that one is the engine's, not ours.
//          Getting the constant RIGHT is therefore what makes the pair invisible; getting it
//          wrong desynchronises the engine's bookkeeping rather than merely our own.
//
//    Which is the whole point of the override: a wrong guess costs a config edit, not a round
//    trip through CI and the user's machine.
// As an integer OR rather than the enum's operator|, which is not constexpr in every SDK
// (mingw's, used for the local syntax check, in particular). Same bits either way: 0xC0.
constexpr std::uint32_t kImageStateAtPresent =
	static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) |
	static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

// The state our scratch texture lives in between the snapshot and the restore. COPY_SOURCE,
// because that is what the restore needs — so the restore records NO barrier of its own on the
// scratch, and the only pair we record on it is on the game's command list at the snapshot,
// where its state is entirely ours and cannot be wrong.
constexpr D3D12_RESOURCE_STATES kScratchRestState = D3D12_RESOURCE_STATE_COPY_SOURCE;

// Presents a retired scratch texture is held for before release, matching the discipline in
// mv_resolve / gbuffer_resolve / nr_codec_pass / nr_hook: the GPU may still be reading it.
constexpr std::uint64_t kRetireLatency = 8;

// How often the periodic diagnostic goes out, in presents. Same cadence as the NR hook's.
constexpr std::uint64_t kReportInterval = 600;

// Default OFF — the rationale is on histplan::Config, and it is a risk judgement about an
// inferred resource state, not a doubt about the mechanism.
std::atomic<bool> g_enabled{ false };
std::atomic<int> g_site{ static_cast<int>(nrplan::HookMode::taa) };
std::atomic<std::uint32_t> g_image_state_at_present{
	static_cast<std::uint32_t>(kImageStateAtPresent) };

// --- counters, all relaxed: they are diagnostics, never control flow ---
std::atomic<std::uint64_t> g_snapshots{ 0 };
std::atomic<std::uint64_t> g_restores{ 0 };
std::atomic<std::uint64_t> g_harmful_misses{ 0 };
std::atomic<std::uint64_t> g_overwritten{ 0 };
std::atomic<std::uint32_t> g_snapshot_reasons[histplan::kStepCount] = {};
std::atomic<std::uint32_t> g_restore_reasons[histplan::kStepCount] = {};
std::atomic<std::uint64_t> g_created{ 0 };
std::atomic<std::uint64_t> g_retired_count{ 0 };
std::atomic<std::uint64_t> g_released{ 0 };

// Everything below is written from the recording thread that captured the TAA dispatch and read
// from the present thread. A torn read here would restore one frame's snapshot over another
// frame's image, so it is all under one lock rather than a scatter of atomics.
std::mutex g_mutex;

struct Scratch
{
	ID3D12Resource *texture = nullptr;
	std::uint32_t width = 0;  // ALLOCATED extent, grow-only
	std::uint32_t height = 0;
	DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
	std::uint64_t bytes = 0;
};
Scratch g_scratch;

struct Retired
{
	ID3D12Resource *texture = nullptr;
	std::uint64_t frame = 0;
	std::uint64_t bytes = 0;
};
std::vector<Retired> g_retired;

// The pending snapshot, handed from the TAA hook to the present-time restore.
struct Pending
{
	bool valid = false;         // a snapshot was recorded and not yet consumed
	bool nr_applied = false;    // nr::apply's verdict for the same frame
	ID3D12Resource *image = nullptr; // AddRef'd for as long as we hold it
	std::uint32_t width = 0;    // the RECT that was copied, not the allocation
	std::uint32_t height = 0;
};
Pending g_pending;

// Resources the restore has issued a copy into, held alive until the GPU can no longer be
// touching them. Same keep-alive discipline as mv_resolve's, for the same measured reason.
struct KeepAlive
{
	ID3D12Resource *texture = nullptr;
	std::uint64_t frame = 0;
};
std::vector<KeepAlive> g_keep_alive;

std::uint64_t g_frame = 0;   // last present index seen, under g_mutex
char g_last_error[192] = "";

void release(ID3D12Resource *&p)
{
	if (p != nullptr)
	{
		p->Release();
		p = nullptr;
	}
}

// Counts a step and logs the first occurrence of each reason exactly once, in the shape the TAA
// path's log_gate_refusal and the NR hook's refuse() use. `ok`, `disabled`, `site_inert` and
// `nr_not_applied` are counted but never warned about: the first two are stated once at startup
// with their full rationale, and the last is the steady state of a session with NR off.
void count_step(std::atomic<std::uint32_t> (&bucket)[histplan::kStepCount], histplan::Step step,
           const char *what, const char *detail)
{
	const auto i = static_cast<int>(step);
	if (i < 0 || i >= histplan::kStepCount)
		return;
	const std::uint32_t before = bucket[i].fetch_add(1, std::memory_order_relaxed);
	if (before != 0)
		return;
	if (step == histplan::Step::ok || step == histplan::Step::disabled ||
		step == histplan::Step::site_inert || step == histplan::Step::nr_not_applied)
		return;
	STRAY_LOG_WARN("NR history restore [%s]: refused (%s) — %s. First occurrence of this reason "
		"only; the running total is in the periodic NR HISTORY line.",
		what, histplan::step_name(step), detail);
}

histplan::Config config()
{
	histplan::Config c;
	c.enabled = g_enabled.load(std::memory_order_relaxed);
	c.site = static_cast<nrplan::HookMode>(g_site.load(std::memory_order_relaxed));
	return c;
}

// Grow-only on extent, EXACT on format. Grow-only is the allocation discipline the rest of this
// codebase runs on (mv_resolve, gbuffer_resolve, nr_codec_pass) and exists for a measured
// reason: reallocation churn during loading orphaned GPU-visible resources and OOM'd the
// machine. It works here only because both transfers are CopyTextureRegion with an explicit
// box — CopyResource would demand IDENTICAL dimensions and make grow-only illegal.
//
// The format cannot be grown around: a copy cannot convert, and this title genuinely changes it
// (R11G11B10_FLOAT in the main menu, R16G16B16A16_FLOAT in gameplay — CLAUDE.md §5), so a format
// change retires and reallocates. That happens about once per session.
//
// Caller holds g_mutex.
bool ensure_scratch(ID3D12Device *device, const D3D12_RESOURCE_DESC &src)
{
	const auto need_w = static_cast<std::uint32_t>(src.Width);
	const auto need_h = src.Height;

	if (g_scratch.texture != nullptr && g_scratch.format == src.Format &&
		need_w <= g_scratch.width && need_h <= g_scratch.height)
		return true;

	std::uint32_t want_w = need_w;
	std::uint32_t want_h = need_h;

	if (g_scratch.texture != nullptr)
	{
		// Retire rather than release: a restore recorded a few frames ago may still be reading
		// it. Only carry the old extent forward when the format is unchanged — a different
		// format is a different allocation and inheriting a stale size just wastes VRAM.
		if (g_scratch.format == src.Format)
		{
			want_w = want_w > g_scratch.width ? want_w : g_scratch.width;
			want_h = want_h > g_scratch.height ? want_h : g_scratch.height;
		}
		Retired r;
		r.texture = g_scratch.texture;
		r.frame = g_frame;
		r.bytes = g_scratch.bytes;
		g_retired.push_back(r);
		g_retired_count.fetch_add(1, std::memory_order_relaxed);
		g_scratch.texture = nullptr;
	}

	D3D12_HEAP_PROPERTIES heap = {};
	heap.Type = D3D12_HEAP_TYPE_DEFAULT;

	// PLAIN, TYPED, SINGLE-SUBRESOURCE. Never a mirror of whatever UE4 allocated — the same
	// lesson ngx_nr::ensure_output_texture and nrp::create_resources carry. This texture is
	// never an NGX input and never a UAV: it is only ever a copy source and a copy destination,
	// so it carries no D3D12_RESOURCE_FLAGs at all.
	D3D12_RESOURCE_DESC tex = {};
	tex.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	tex.Width = want_w;
	tex.Height = want_h;
	tex.DepthOrArraySize = 1;
	tex.MipLevels = 1;
	tex.Format = src.Format;
	tex.SampleDesc.Count = 1;
	tex.SampleDesc.Quality = 0;
	tex.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	tex.Flags = D3D12_RESOURCE_FLAG_NONE;

	const HRESULT hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &tex,
		kScratchRestState, nullptr, IID_PPV_ARGS(&g_scratch.texture));
	if (FAILED(hr))
	{
		std::snprintf(g_last_error, sizeof(g_last_error),
			"CreateCommittedResource(history scratch %ux%u fmt=%d) hr=0x%08x", want_w, want_h,
			static_cast<int>(src.Format), static_cast<unsigned int>(hr));
		g_scratch.width = 0;
		g_scratch.height = 0;
		g_scratch.format = DXGI_FORMAT_UNKNOWN;
		g_scratch.bytes = 0;
		return false;
	}

	g_scratch.width = want_w;
	g_scratch.height = want_h;
	g_scratch.format = src.Format;
	// Ask the device rather than assume a bytes-per-pixel: the accounting figure is the whole
	// point of having it, and a format table that goes stale is worse than no table. This runs
	// once per allocation, not per frame.
	{
		UINT64 total = 0;
		device->GetCopyableFootprints(&tex, 0, 1, 0, nullptr, nullptr, nullptr, &total);
		g_scratch.bytes = total;
	}
	g_created.fetch_add(1, std::memory_order_relaxed);

	STRAY_LOG_INFO("NR history restore: scratch %ux%u fmt=%d created "
		"(single-mip/single-slice/non-MSAA, no resource flags — it is only ever a copy source "
		"and a copy destination). It holds the PRISTINE pre-NR image so the engine's temporal "
		"history never sees the neural residual. ~%llu bytes.",
		want_w, want_h, static_cast<int>(src.Format),
		static_cast<unsigned long long>(g_scratch.bytes));
	return true;
}

// Caller holds g_mutex.
void retire_expired()
{
	for (auto it = g_retired.begin(); it != g_retired.end();)
	{
		if (g_frame >= it->frame + kRetireLatency)
		{
			release(it->texture);
			it = g_retired.erase(it);
			g_released.fetch_add(1, std::memory_order_relaxed);
		}
		else
		{
			++it;
		}
	}
	for (auto it = g_keep_alive.begin(); it != g_keep_alive.end();)
	{
		if (g_frame >= it->frame + kRetireLatency)
		{
			release(it->texture);
			it = g_keep_alive.erase(it);
		}
		else
		{
			++it;
		}
	}
}

// Caller holds g_mutex. Drops a pending snapshot's reference to the engine image.
void drop_pending()
{
	if (g_pending.image != nullptr)
	{
		KeepAlive ka;
		ka.texture = g_pending.image;
		ka.frame = g_frame;
		g_keep_alive.push_back(ka);
		g_pending.image = nullptr;
	}
	g_pending = Pending{};
}

void copy_rect(ID3D12GraphicsCommandList *cmd, ID3D12Resource *dst, ID3D12Resource *src,
               std::uint32_t width, std::uint32_t height)
{
	D3D12_TEXTURE_COPY_LOCATION d = {};
	d.pResource = dst;
	d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	d.SubresourceIndex = 0;

	D3D12_TEXTURE_COPY_LOCATION s = d;
	s.pResource = src;

	D3D12_BOX box = {};
	box.left = 0;
	box.top = 0;
	box.front = 0;
	box.right = width;
	box.bottom = height;
	box.back = 1;

	cmd->CopyTextureRegion(&d, 0, 0, 0, &s, &box);
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

} // namespace

void set_enabled(bool value)
{
	g_enabled.store(value, std::memory_order_relaxed);
}

bool enabled()
{
	return g_enabled.load(std::memory_order_relaxed);
}

void set_site(nrplan::HookMode site)
{
	g_site.store(static_cast<int>(site), std::memory_order_relaxed);
}

void set_image_state_at_present(std::uint32_t state)
{
	g_image_state_at_present.store(state != 0 ? state
		: static_cast<std::uint32_t>(kImageStateAtPresent), std::memory_order_relaxed);
}

std::uint32_t image_state_at_present()
{
	return g_image_state_at_present.load(std::memory_order_relaxed);
}

void snapshot(ID3D12Device *device, ID3D12GraphicsCommandList *cmd, ID3D12Resource *image,
              std::uint32_t width, std::uint32_t height)
{
	const histplan::Config cfg = config();

	histplan::SnapshotInputs in;
	in.nr_enabled = nr::enabled();
	// NR's own readiness, asked here rather than passed in so the TAA hook's call site stays one
	// line. `validated()` is the deferred readback verdict and it flips inside nr::on_present,
	// never inside nr::apply — so on the first frame apply() can succeed this has already been
	// true for a present, and there is no frame where NR applies while we declined to snapshot.
	in.nr_can_apply = in.nr_enabled && nr::validated();
	in.have_image = image != nullptr && device != nullptr && cmd != nullptr;
	in.width = width;
	in.height = height;

	D3D12_RESOURCE_DESC src = {};
	if (in.have_image)
	{
		src = image->GetDesc();
		in.mip_levels = src.MipLevels;
		in.array_size = src.DepthOrArraySize;
		in.sample_count = src.SampleDesc.Count;
		// Never copy past the allocation. taa_hook reports the rect the DISPATCH writes, which
		// the engine guarantees fits inside the UAV — but a caller bug must not become an
		// out-of-bounds CopyTextureRegion, which is a device fault rather than an error return.
		if (in.width > src.Width)
			in.width = static_cast<std::uint32_t>(src.Width);
		if (in.height > src.Height)
			in.height = src.Height;
	}

	const histplan::Step step = histplan::plan_snapshot(cfg, in);
	if (step != histplan::Step::ok)
	{
		char detail[192];
		std::snprintf(detail, sizeof(detail),
			"image=%p rect %ux%u mips=%u slices=%u samples=%u site=%s",
			static_cast<void *>(image), in.width, in.height, in.mip_levels, in.array_size,
			in.sample_count, nrplan::hook_mode_name(cfg.site));
		count_step(g_snapshot_reasons, step, "snapshot", detail);
		return;
	}

	std::lock_guard<std::mutex> lock(g_mutex);

	if (!ensure_scratch(device, src))
	{
		count_step(g_snapshot_reasons, histplan::Step::alloc_failed, "snapshot", g_last_error);
		STRAY_LOG_ERROR("NR history restore: %s. The pristine copy cannot be taken, so DLSS "
			"Neural Rendering's residual WILL reach the engine's temporal history this frame and "
			"the slow SSR/history drift returns. Set [STRAYDLSS] NgxNR=0 or NgxNRRestoreHistory=0 "
			"to make that a deliberate choice rather than a surprise.", g_last_error);
		return;
	}

	// A second TAA dispatch in the same frame would overwrite the scratch content anyway, so
	// last-wins is the only coherent behaviour — but it means the FIRST dispatch's residual
	// reaches the history unrestored, which must not be silent. DLSS is pinned to one pass
	// (CLAUDE.md §5) so this is not expected to fire at all.
	if (g_pending.valid)
	{
		const std::uint64_t before = g_overwritten.fetch_add(1, std::memory_order_relaxed);
		if (before == 0)
			STRAY_LOG_WARN("NR history restore: a second TAA dispatch in one frame overwrote the "
				"pending pristine snapshot. Only the LAST dispatch's history is restored; the "
				"earlier one's neural residual reaches the engine. DLSS is pinned to a single "
				"pass, so this should not happen — if the count climbs, this title renders more "
				"than one view per frame. First occurrence only.");
		drop_pending();
	}

	// --- the pristine copy, on the GAME's command list, in states we know exactly ---
	transition(cmd, image, kImageStateAtSnapshot, D3D12_RESOURCE_STATE_COPY_SOURCE);
	transition(cmd, g_scratch.texture, kScratchRestState, D3D12_RESOURCE_STATE_COPY_DEST);
	copy_rect(cmd, g_scratch.texture, image, in.width, in.height);
	transition(cmd, g_scratch.texture, D3D12_RESOURCE_STATE_COPY_DEST, kScratchRestState);
	// Put `u0` back immediately: nr::apply is about to be handed it and documents
	// UNORDERED_ACCESS as both the state it arrives in and the state it is left in.
	transition(cmd, image, D3D12_RESOURCE_STATE_COPY_SOURCE, kImageStateAtSnapshot);

	image->AddRef(); // held until the restore has been recorded AND the GPU has drained
	g_pending.valid = true;
	g_pending.nr_applied = false;
	g_pending.image = image;
	g_pending.width = in.width;
	g_pending.height = in.height;

	const std::uint64_t n = g_snapshots.fetch_add(1, std::memory_order_relaxed);
	count_step(g_snapshot_reasons, histplan::Step::ok, "snapshot", "");
	if (n == 0)
		STRAY_LOG_WARN("NR history restore ARMED: the pre-NR image of u0 (%p, rect %ux%u fmt=%d) "
			"is now copied aside every frame and copied back at present, so UE 4.27's "
			"TemporalAAHistory.RT[0] — which ScreenSpaceRayTracing.cpp:596-620 reads on the NEXT "
			"frame — never contains the neural residual. The DISPLAYED frame is unchanged: every "
			"same-frame consumer of u0 has already run by Present. [STRAYDLSS] "
			"NgxNRRestoreHistory=0 disables it. First occurrence only.",
			static_cast<void *>(image), in.width, in.height, static_cast<int>(src.Format));
}

void note_nr_applied(bool applied)
{
	std::lock_guard<std::mutex> lock(g_mutex);
	if (g_pending.valid)
	{
		g_pending.nr_applied = applied;
		return;
	}
	// No snapshot but NR applied: the residual reached the history. plan_restore would report
	// this as `no-snapshot`, but recording it HERE names the frame it happened on rather than
	// the present that noticed. Only possible if the snapshot refused while nr::apply succeeded.
	if (applied)
	{
		histplan::RestoreInputs ri;
		ri.have_snapshot = false;
		ri.nr_applied = true;
		if (histplan::restore_miss_is_harmful(config(), ri))
		{
			const std::uint64_t before = g_harmful_misses.fetch_add(1, std::memory_order_relaxed);
			if (before == 0)
				STRAY_LOG_WARN("NR history restore: nr::apply MODIFIED u0 on a frame with no "
					"pristine snapshot, so that frame's neural residual reached the engine's "
					"temporal history. Read the snapshot refusal above for the reason. First "
					"occurrence only; the running total is in the periodic NR HISTORY line.");
		}
	}
}

void on_present(const icept::PresentContext &pc)
{
	const std::uint64_t frame = pc.frame;
	const histplan::Config cfg = config();

	// The pending record has to be consumed on EVERY present, whatever the verdict — otherwise a
	// frame that refuses leaves last frame's snapshot pending and the restore lands one frame
	// late, over a different image.
	Pending pending;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		g_frame = frame;
		retire_expired();
		pending = g_pending;
		// Deliberately not drop_pending() yet: the copy below still needs `image`, and the
		// reference belongs to whoever ends up releasing it (below, under the same lock).
		g_pending.valid = false;
		g_pending.nr_applied = false;
		g_pending.image = nullptr;
	}

	histplan::RestoreInputs ri;
	ri.have_snapshot = pending.valid;
	ri.nr_applied = pending.nr_applied;
	const histplan::Step step = histplan::plan_restore(cfg, ri);

	if (step == histplan::Step::ok)
	{
		// --- WHICH COMMAND LIST, AND WHY THE ORDERING IS GUARANTEED (ReShade v6.8.0, HARD) ---
		//
		// dxgi_swapchain.cpp's DXGISwapChain::on_present, d3d12 case, in this exact order:
		//     invoke_addon_event<addon_event::present>(...)   <-- we are here
		//     reshade::present_effect_runtime(_impl);
		//     static_cast<D3D12CommandQueue *>(_direct3d_command_queue)
		//         ->flush_immediate_command_list();
		// and all of that runs inside the hooked IDXGISwapChain::Present, i.e. AFTER the game has
		// submitted every command list of the frame to this same queue. The flush is a plain
		// ExecuteCommandLists on that queue (d3d12_impl_command_list_immediate.cpp:154), and work
		// on one queue executes in submission order — so this copy executes after every same-frame
		// consumer of `u0` and before the next frame's SSR reads the history. That is the whole
		// correctness argument, and it needs no assumption about ReShade's effect runtime.
		//
		// THE ONE TRAP: flush() early-outs on `!_has_commands`
		// (d3d12_impl_command_list_immediate.cpp:122-124), and `_has_commands` is set by ReShade's
		// command_list API, NOT by recording onto the native list behind get_native(). With an
		// empty preset nothing else records here, so a purely native restore would sit unflushed
		// in an open list and execute at an arbitrary later moment. So the two barriers go through
		// ReShade's own barrier() (d3d12_impl_command_list.cpp:54-59 sets _has_commands and then
		// records a real D3D12_RESOURCE_BARRIER; api::resource_usage is a bit-for-bit passthrough
		// of D3D12_RESOURCE_STATES, convert_usage_to_resource_states:224-255) and only the copy
		// itself is native. No descriptors are involved anywhere in this path, so none of the
		// ReShade descriptor-conversion hazards of CLAUDE.md §1 apply.
		// THREAD SAFETY, since api::command_queue's own doc says it "may NOT be used concurrently
		// from multiple threads": the D3D12 branch of DXGISwapChain::on_present takes
		// `unique_direct3d_device_lock` on the D3D12CommandQueue's own _mutex before invoking any
		// event, precisely so "the immediate command list may be accessed" (its comment). We are
		// inside that lock for the whole of this callback.
		ID3D12GraphicsCommandList *native = pc.present_list;

		std::lock_guard<std::mutex> lock(g_mutex);
		if (native == nullptr || g_scratch.texture == nullptr)
		{
			count_step(g_restore_reasons, histplan::Step::no_command_list, "restore",
				"the present queue offered no immediate command list, or the scratch was gone");
			const std::uint64_t before = g_harmful_misses.fetch_add(1, std::memory_order_relaxed);
			if (before == 0)
				STRAY_LOG_ERROR("NR history restore: no immediate command list at present, so the "
					"pristine image cannot be put back and the neural residual reaches the "
					"engine's temporal history. This should be impossible on a D3D12 DIRECT queue "
					"(d3d12_impl_command_queue.cpp:20 creates one for every graphics queue). "
					"First occurrence only.");
		}
		else
		{
			const std::uint32_t assumed = image_state_at_present();
			// The barriers go through the BACKEND, not straight onto `native`: ReShade's immediate
			// list only flushes what its own API recorded (CLAUDE.md §5), and its barrier is the
			// call that marks it. The backend's present_barrier is that call under ReShade and a
			// plain ResourceBarrier under the native backend; the bits are D3D12_RESOURCE_STATES
			// either way (api::resource_usage is a bit-for-bit passthrough).
			const auto res = reinterpret_cast<icept::ResourceId>(pending.image);

			icept::backend()->present_barrier(pc, res, assumed, D3D12_RESOURCE_STATE_COPY_DEST);
			// The scratch needs no barrier: it RESTS in COPY_SOURCE, and the snapshot's own
			// COPY_DEST -> COPY_SOURCE transition (recorded on the game's list, which executes
			// earlier on this same queue) is both the layout change and the write-before-read
			// dependency this copy needs.
			copy_rect(native, pending.image, g_scratch.texture, pending.width, pending.height);
			icept::backend()->present_barrier(pc, res, D3D12_RESOURCE_STATE_COPY_DEST, assumed);

			const std::uint64_t n = g_restores.fetch_add(1, std::memory_order_relaxed);
			count_step(g_restore_reasons, histplan::Step::ok, "restore", "");
			if (n == 0)
				STRAY_LOG_INFO("NR history restore: first restore recorded on ReShade's immediate "
					"command list at present — rect %ux%u back into u0 (%p). ASSUMED STATE for u0 "
					"here is 0x%X ([STRAYDLSS] NgxNRRestoreState). That is [derived], not "
					"measured: TemporalAA.cpp:969 extracts the history with RDG's default "
					"AccessFinal (RenderGraphBuilder.h:209, ERHIAccess::SRVMask), which "
					"D3D12Commands.cpp:398-414 maps to Resource->GetReadableState(), which "
					"D3D12Resources.h:377/389 defines as NON_PIXEL|PIXEL_SHADER_RESOURCE = 0xC0. "
					"If the image or the frame rate goes wrong the moment this line appears, this "
					"constant is the first thing to change.",
					pending.width, pending.height, static_cast<void *>(pending.image),
					static_cast<unsigned int>(assumed));
		}

		// Whatever happened, the copy is recorded (or was refused) and the reference can go into
		// the keep-alive queue rather than being released under an in-flight command list.
		if (pending.image != nullptr)
		{
			KeepAlive ka;
			ka.texture = pending.image;
			ka.frame = frame;
			g_keep_alive.push_back(ka);
			pending.image = nullptr;
		}
	}
	else
	{
		char detail[160];
		std::snprintf(detail, sizeof(detail), "haveSnapshot=%d nrApplied=%d site=%s",
			ri.have_snapshot ? 1 : 0, ri.nr_applied ? 1 : 0, nrplan::hook_mode_name(cfg.site));
		count_step(g_restore_reasons, step, "restore", detail);
		if (histplan::restore_miss_is_harmful(cfg, ri))
			g_harmful_misses.fetch_add(1, std::memory_order_relaxed);

		std::lock_guard<std::mutex> lock(g_mutex);
		if (pending.image != nullptr)
		{
			KeepAlive ka;
			ka.texture = pending.image;
			ka.frame = frame;
			g_keep_alive.push_back(ka);
			pending.image = nullptr;
		}
	}

	// The periodic diagnostic. Only at the site where this mechanism does anything, and only
	// once it has done something — a line that says "0 0 0" every 600 frames in every session
	// that never enabled NR is noise, and noise is what makes a real line easy to miss.
	if (cfg.site != nrplan::HookMode::taa || frame == 0 || (frame % kReportInterval) != 0)
		return;
	const Counters c = counters();
	if (c.snapshots == 0 && c.restores == 0 && c.harmful_misses == 0)
		return;

	char reasons[224] = "";
	int off = 0;
	for (int i = 0; i < histplan::kStepCount; ++i)
	{
		const std::uint32_t total = c.snapshot_reasons[i] + c.restore_reasons[i];
		if (total == 0 || i == static_cast<int>(histplan::Step::ok))
			continue;
		const int n = std::snprintf(reasons + off, sizeof(reasons) - static_cast<std::size_t>(off),
			"%s%s=%u", off == 0 ? "" : " ",
			histplan::step_name(static_cast<histplan::Step>(i)), total);
		if (n <= 0 || off + n >= static_cast<int>(sizeof(reasons)))
			break;
		off += n;
	}
	STRAY_LOG_INFO("NR HISTORY frame %llu: snapshots=%llu restores=%llu harmfulMisses=%llu "
		"overwritten=%llu | %s | scratch created=%llu retired=%llu released=%llu liveRetired=%u "
		"bytes=%llu | u0 assumed state at present 0x%X",
		static_cast<unsigned long long>(frame),
		static_cast<unsigned long long>(c.snapshots),
		static_cast<unsigned long long>(c.restores),
		static_cast<unsigned long long>(c.harmful_misses),
		static_cast<unsigned long long>(c.overwritten),
		reasons[0] != 0 ? reasons : "no refusals",
		static_cast<unsigned long long>(c.scratch_created),
		static_cast<unsigned long long>(c.scratch_retired),
		static_cast<unsigned long long>(c.scratch_released), c.live_retired,
		static_cast<unsigned long long>(c.bytes_live), image_state_at_present());
	// The one number that must stay zero. Anything else means frames whose neural residual DID
	// reach the engine's temporal history, i.e. the drift is partially back.
	if (c.harmful_misses != 0)
		STRAY_LOG_WARN("NR HISTORY: %llu frame(s) modified u0 with no pristine copy to restore. "
			"The SSR/history drift is proportional to that count.",
			static_cast<unsigned long long>(c.harmful_misses));
}

void shutdown()
{
	std::lock_guard<std::mutex> lock(g_mutex);
	drop_pending();
	for (auto &r : g_retired)
		release(r.texture);
	g_retired.clear();
	for (auto &k : g_keep_alive)
		release(k.texture);
	g_keep_alive.clear();
	release(g_scratch.texture);
	g_scratch = Scratch{};
	g_frame = 0;
	g_last_error[0] = '\0';
}

Counters counters()
{
	Counters c;
	c.snapshots = g_snapshots.load(std::memory_order_relaxed);
	c.restores = g_restores.load(std::memory_order_relaxed);
	c.harmful_misses = g_harmful_misses.load(std::memory_order_relaxed);
	c.overwritten = g_overwritten.load(std::memory_order_relaxed);
	for (int i = 0; i < histplan::kStepCount; ++i)
	{
		c.snapshot_reasons[i] = g_snapshot_reasons[i].load(std::memory_order_relaxed);
		c.restore_reasons[i] = g_restore_reasons[i].load(std::memory_order_relaxed);
	}
	c.scratch_created = g_created.load(std::memory_order_relaxed);
	c.scratch_retired = g_retired_count.load(std::memory_order_relaxed);
	c.scratch_released = g_released.load(std::memory_order_relaxed);
	// The live figures need the lock: they are read off the containers themselves.
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		c.live_retired = static_cast<std::uint32_t>(g_retired.size());
		c.bytes_live = g_scratch.texture != nullptr ? g_scratch.bytes : 0;
		for (const auto &r : g_retired)
			c.bytes_live += r.bytes;
	}
	return c;
}

} // namespace stray_dlss::nrhist
