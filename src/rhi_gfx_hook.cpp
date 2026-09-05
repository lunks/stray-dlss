#include "rhi_gfx_hook.hpp"

#include "backend_native/backbuffer_state.hpp"
#include "backend_native/native_backend.hpp"
#include "hudless.hpp"
#include "log.hpp"
#include "u0_rhi_hook.hpp"

#include <d3d12.h>
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace stray_dlss::rhigfx {

thread_local bool t_copy_pending = false;

namespace {

// One thunk shape for all four seams: `this` in RCX and up to four more register/stack args
// forwarded verbatim. A callee with fewer parameters ignores the surplus; the caller set them.
//   47 RHIBeginDrawingViewport(this, FRHIViewport*, FRHITexture*)
//   48 RHIEndDrawingViewport(this, FRHIViewport*, bool bPresent, bool bLockToVsync)
//   79 RHIBeginRenderPass(this, const FRHIRenderPassInfo&, const TCHAR*)
//   80 RHIEndRenderPass(this)
using SeamFn = void (*)(void *self, void *a2, void *a3, void *a4, void *a5);

constexpr unsigned kSeams = 4;
constexpr unsigned kSeamSlots[kSeams] = {
	u0::kGfxSlotBeginDrawingViewport, u0::kGfxSlotEndDrawingViewport,
	u0::kGfxSlotBeginRenderPass, u0::kGfxSlotEndRenderPass,
};
enum { kBeginViewport = 0, kEndViewport = 1, kBeginPass = 2, kEndPass = 3 };

std::mutex g_mutex;
Config g_cfg;
fseam::Level g_bb_level = fseam::Level::off;
fseam::Level g_hud_level = fseam::Level::off;
u0::GfxDiscovery g_gfx;
std::atomic<bool> g_hooked{ false };
void **g_patched[kSeams] = {};
SeamFn g_orig[kSeams] = {};
Stats g_stats;

// ---- the per-frame ledger and the latch (RHI thread; the mutex is for the readers) ----
fseam::FrameLedger g_frame;
fseam::PtrLatch g_latch;
std::uint64_t g_frame_res_inside = 0;   // this frame's in-bracket resolve
bool g_frame_res_candidate = false;
FrameIdentity g_identity;               // published at RHIEndDrawingViewport
bool g_identity_consumed = true;
std::uint32_t g_seam_thread = 0;
std::uint64_t g_frames_logged = 0;
bool g_shape_logged[static_cast<int>(fseam::FrameShape::count)] = {};
bool g_copy_refusal_logged[static_cast<int>(fseam::CopyRefusal::count)] = {};
bool g_id_disagree_logged = false;
bool g_id_barrier_disagree_logged = false;
bool g_latch_logged = false;
bool g_carrier_lost_logged = false;
std::uint64_t g_last_reverify_frame = 0;
constexpr std::uint64_t kReverifyEvery = 600;

// ---- the armed copy (RHI thread only) ----
struct Pending
{
	std::uint64_t back_buffer = 0;
	std::uint64_t frame = 0;
};
thread_local Pending t_pending;

// Up to 24 UTF-16 code units of a name whose pointer lies in the module's .rdata, as ASCII, for
// the log ONLY. Never used to identify anything.
void read_name(std::uint64_t name, char *out, std::size_t cap)
{
	out[0] = '\0';
	if (name == 0 || cap < 2)
		return;
	std::size_t n = 0;
	for (unsigned q = 0; q < 6 && n + 1 < cap; ++q)
	{
		std::uint64_t v = 0;
		if (!u0hook::guarded_read_u64(name + 8ull * q, &v))
			break;
		for (unsigned c = 0; c < 4 && n + 1 < cap; ++c)
		{
			const auto ch = static_cast<std::uint16_t>((v >> (16u * c)) & 0xFFFFu);
			if (ch == 0)
			{
				out[n] = '\0';
				return;
			}
			out[n++] = (ch >= 0x20 && ch < 0x7F) ? static_cast<char>(ch) : '?';
		}
	}
	out[n] = '\0';
}

void note_thread()
{
	const DWORD tid = GetCurrentThreadId();
	if (g_seam_thread == 0)
		g_seam_thread = tid;
	else if (g_seam_thread != tid)
		++g_stats.other_thread;
}

// FRHITexture* -> ID3D12Resource* through the engine's own GetNativeResource, then the
// candidate-set test. 0 when refused.
std::uint64_t resolve_and_check(std::uint64_t rhi_texture, bool *candidate)
{
	++g_stats.resolves;
	const std::uint64_t res = u0hook::resolve_texture_native(rhi_texture);
	if (res == 0)
	{
		++g_stats.resolve_failed;
		*candidate = false;
		return 0;
	}
	*candidate = native::bbstate::is_candidate(res);
	if (!*candidate)
		++g_stats.resolve_not_candidate;
	return res;
}

// ---- the four seams ----

void on_begin_viewport(void *self, void *viewport, void *rt)
{
	std::lock_guard<std::mutex> lock(g_mutex);
	note_thread();
	++g_stats.begin_viewport;
	g_frame.begin_bracket();
	(void)self;
	(void)viewport;
	(void)rt; // NULL on every 4.27 caller (UnrealClient.cpp:1418, SlateRHIRenderer.cpp:845) — the identity is in the pass
}

void on_end_viewport(void *self, void *viewport, std::uint64_t present, std::uint64_t vsync)
{
	std::lock_guard<std::mutex> lock(g_mutex);
	note_thread();
	++g_stats.end_viewport;
	g_frame.end_bracket();
	// THE FRAME'S IDENTITY, published for the Present that this call is about to make
	// (FD3D12Viewport::Present runs inside the forward, and our Present hook inside that).
	g_identity.engine_res = g_frame_res_inside;
	g_identity.candidate = g_frame_res_candidate;
	g_identity.frame = ++g_stats.frames;
	g_identity_consumed = false;
	(void)self;
	(void)viewport;
	(void)present;
	(void)vsync;
}

void on_begin_pass(void *self, void *info, void *name)
{
	std::lock_guard<std::mutex> lock(g_mutex);
	note_thread();
	++g_stats.begin_pass;
	if (g_stats.disabled)
		return;
	fseam::Color0 c0;
	const u0::ObjectReader reader{ [](void *, std::uint64_t va, std::uint64_t *out) { return u0hook::guarded_read_u64(va, out); }, nullptr };
	if (!fseam::decode_color0(reader, reinterpret_cast<std::uint64_t>(info), &c0) || c0.render_target == 0)
	{
		++g_stats.pass_decode_failed;
		return;
	}

	// Is RT0 the viewport's back-buffer reference texture? Latched: a pointer compare. Not yet:
	// the engine's own GetNativeResource on the object, held to the candidate set.
	bool is_bb = false;
	std::uint64_t resolved = 0;
	bool candidate = false;
	if (g_latch.latched())
	{
		is_bb = c0.render_target == g_latch.value();
		if (is_bb)
		{
			// THIS frame's buffer: the reference resolves to a different ID3D12Resource every
			// frame, so it is resolved once per bracket half (twice a frame), not per pass.
			const bool need = g_frame.in_bracket ? g_frame_res_inside == 0 : g_frame.frame_res_outside == 0;
			if (need)
			{
				resolved = resolve_and_check(c0.render_target, &candidate);
				if (resolved == 0 || !candidate)
				{
					// The latched texture no longer names a candidate: a resize recreated the
					// reference (WindowsD3D12Viewport.cpp:392) or the candidate set moved.
					++g_stats.relatch;
					g_latch.reset();
					g_stats.latched = false;
					g_stats.latched_texture = 0;
					is_bb = false;
				}
			}
		}
	}
	else
	{
		resolved = resolve_and_check(c0.render_target, &candidate);
		if (resolved != 0 && candidate)
		{
			is_bb = true;
			if (g_latch.observe(c0.render_target))
			{
				g_stats.latched = true;
				g_stats.latched_texture = c0.render_target;
				if (!g_latch_logged)
				{
					g_latch_logged = true;
					STRAY_LOG_INFO("RHI GFX SEAM: the engine's back-buffer texture is FRHITexture %p - latched after %u "
						"frames whose RHIBeginRenderPass named it as RT0 and whose FRHITexture::GetNativeResource "
						"resolved to a registered swapchain-class resource (this frame: %p). It is the viewport's "
						"reference texture (FD3D12BackBufferReferenceTexture2D), one object per viewport, resolved "
						"by the engine to the CURRENT back buffer on the RHI thread (D3D12Texture.h:312-331). From "
						"here every pass is classified by a pointer compare and the resolve runs twice a frame.",
						reinterpret_cast<void *>(c0.render_target), fseam::kLatchAgreements, reinterpret_cast<void *>(resolved));
				}
			}
		}
	}

	const fseam::PassClass pc = g_frame.classify(is_bb, resolved, c0.action);
	++g_stats.pass_class[static_cast<int>(pc)];
	if (is_bb && g_frame.in_bracket && resolved != 0 && g_frame_res_inside == 0)
	{
		g_frame_res_inside = resolved;
		g_frame_res_candidate = candidate;
	}

	// The first frames' back-buffer passes, with the name pointer when it lies in .rdata (the
	// string is printed for the reader; nothing is identified by it).
	if (is_bb && g_frames_logged < static_cast<std::uint64_t>(g_cfg.log_passes))
	{
		const auto name_va = reinterpret_cast<std::uint64_t>(name);
		const bool rdata = u0hook::in_module_rdata(name_va);
		if (rdata)
			++g_stats.name_in_rdata;
		else if (name_va != 0)
			++g_stats.name_elsewhere;
		char text[32];
		read_name(rdata ? name_va : 0, text, sizeof(text));
		STRAY_LOG_INFO("RHI GFX SEAM pass: frame %llu %s RT0=%p action=%u (%s) resolved=%p candidate=%d name=%p%s%s%s "
			"bracket=%d ctx=%p",
			static_cast<unsigned long long>(g_stats.frames), fseam::pass_class_name(pc),
			reinterpret_cast<void *>(c0.render_target), c0.action, fseam::load_action_name(fseam::load_action(c0.action)),
			reinterpret_cast<void *>(resolved), candidate ? 1 : 0, name, rdata ? " (.rdata: \"" : "", rdata ? text : "",
			rdata ? "\")" : "", g_frame.in_bracket ? 1 : 0, self);
	}

	// ---- the HUD-less copy: armed here, recorded by the next hooked command on this thread ----
	fseam::CopyInputs ci;
	ci.level = g_hud_level;
	ci.pass = pc;
	ci.latched = g_latch.latched();
	ci.shape_ok_streak = g_stats.shape_ok_streak;
	const fseam::CopyRefusal cr = fseam::decide_copy(ci);
	if (pc == fseam::PassClass::bb_hud)
	{
		++g_stats.copy_refusal[static_cast<int>(cr)];
		if (cr != fseam::CopyRefusal::none && cr != fseam::CopyRefusal::level_off &&
			!g_copy_refusal_logged[static_cast<int>(cr)])
		{
			g_copy_refusal_logged[static_cast<int>(cr)] = true;
			STRAY_LOG_INFO("HUD-LESS: the HUD pass was seen and the copy was NOT armed (%s). Once per reason; the "
				"rate is `copyRefused:` on the [rhigfx] line.", fseam::copy_refusal_name(cr));
		}
	}
	if (cr == fseam::CopyRefusal::none && resolved != 0)
	{
		t_pending.back_buffer = resolved;
		t_pending.frame = g_stats.frames;
		t_copy_pending = true;
	}
}

void on_end_pass(void *self)
{
	std::lock_guard<std::mutex> lock(g_mutex);
	++g_stats.end_pass;
	if (t_copy_pending)
	{
		// No hooked command arrived between BeginRenderPass and EndRenderPass on this thread:
		// the pass drew nothing we intercept. The copy is dropped, never recorded late.
		t_copy_pending = false;
		++g_stats.carrier_lost;
		if (!g_carrier_lost_logged)
		{
			g_carrier_lost_logged = true;
			STRAY_LOG_WARN("HUD-LESS: a copy was armed at the HUD pass and no SetPipelineState / ResourceBarrier / Draw "
				"reached our hooks on this thread before RHIEndRenderPass, so it was not recorded (ctx %p). Once per "
				"session; the rate is `carrierLost=` on the [rhigfx] line.", self);
		}
	}
}

template <unsigned Seam>
void seam_thunk(void *self, void *a2, void *a3, void *a4, void *a5)
{
	if constexpr (Seam == kBeginViewport)
		on_begin_viewport(self, a2, a3);
	else if constexpr (Seam == kEndViewport)
		on_end_viewport(self, a2, reinterpret_cast<std::uint64_t>(a3), reinterpret_cast<std::uint64_t>(a4));
	else if constexpr (Seam == kBeginPass)
		on_begin_pass(self, a2, a3);
	else
		on_end_pass(self);
	SeamFn orig = g_orig[Seam];
	if (orig != nullptr)
		orig(self, a2, a3, a4, a5);
}

SeamFn thunk_for(unsigned seam)
{
	switch (seam)
	{
	case kBeginViewport: return &seam_thunk<kBeginViewport>;
	case kEndViewport: return &seam_thunk<kEndViewport>;
	case kBeginPass: return &seam_thunk<kBeginPass>;
	default: return &seam_thunk<kEndPass>;
	}
}

void install_locked()
{
	auto **base = reinterpret_cast<void **>(static_cast<std::uintptr_t>(g_gfx.vtable_va));
	unsigned installed = 0;
	for (unsigned i = 0; i < kSeams; ++i)
	{
		void **where = base + kSeamSlots[i];
		void *orig = nullptr;
		if (!u0hook::patch_vtable_slot(where, reinterpret_cast<void *>(thunk_for(i)), &orig))
		{
			STRAY_LOG_ERROR("RHI GFX SEAM: VirtualProtect on vtable slot %u at %p failed; not installed.", kSeamSlots[i], static_cast<void *>(where));
			continue;
		}
		g_patched[i] = where;
		g_orig[i] = reinterpret_cast<SeamFn>(orig);
		++installed;
	}
	if (installed == 0)
		return;
	g_hooked.store(true, std::memory_order_release);
	g_stats.hooked = true;
	STRAY_LOG_INFO("RHI GFX SEAM INSTALLED: %u of %u slots of the FD3D12CommandContext vtable at %#llx forward through "
		"our thunks - RHIBeginDrawingViewport (47), RHIEndDrawingViewport (48), RHIBeginRenderPass (79), "
		"RHIEndRenderPass (80). Every call is forwarded with all its arguments, so the image is unchanged. "
		"EngineSeamBackBuffer=%s, EngineSeamHudless=%s. Read the [rhigfx] line: brackets must equal presents, "
		"bbHud must track them, shape must read ok, idVsMirror/idVsBarrier disagree must stay 0.",
		installed, kSeams, static_cast<unsigned long long>(g_gfx.vtable_va),
		fseam::level_name(g_bb_level), fseam::level_name(g_hud_level));
}

} // namespace

void configure(const Config &cfg)
{
	std::lock_guard<std::mutex> lock(g_mutex);
	g_cfg = cfg;
	g_bb_level = fseam::level_from_int(cfg.backbuffer_level);
	g_hud_level = fseam::level_from_int(cfg.hudless_level);
	STRAY_LOG_INFO("RHI GFX SEAM MODE: EngineSeamBackBuffer=%s (%d), EngineSeamHudless=%s (%d), EngineSeamHudlessNR=%d, "
		"logging the first %d frames' back-buffer passes. Requires U0Hook >= 1 (the vtable discovery); "
		"installs at the moment that discovery succeeds.",
		fseam::level_name(g_bb_level), cfg.backbuffer_level, fseam::level_name(g_hud_level), cfg.hudless_level,
		cfg.hudless_nr ? 1 : 0, cfg.log_passes);
	if (g_hud_level == fseam::Level::authoritative)
		STRAY_LOG_WARN("HUD-LESS LEVEL 2 IS UNCONFIRMED ON THE BOX: once the frame shape has held %u frames, a "
			"CopyResource(hudless <- back buffer) is recorded on the game's own list at the HUD pass, DLSS-G is "
			"handed it as DLSSG.HUDLess (the indicator's Hudless field should read Yes) and NR as DLSSNR.Color "
			"with DLSSNR.Backbuffer = the final frame%s. A HUD that vanishes or doubles is THIS feature; "
			"EngineSeamHudless=1 keeps everything but the copy.",
			fseam::kShapeAgreements, cfg.hudless_nr ? "" : " (NR consumer off: EngineSeamHudlessNR=0)");
	if (g_bb_level == fseam::Level::authoritative)
		STRAY_LOG_WARN("BACK-BUFFER IDENTITY LEVEL 2 IS UNCONFIRMED ON THE BOX: the replacement frame generation "
			"copies and generates from is the one the engine's RHIBeginRenderPass resolved to this frame; "
			"GameIndexMirror is the assertion. A stale presented frame is THIS feature; EngineSeamBackBuffer=1 "
			"keeps the mirror in charge.");
}

const Config &config() { return g_cfg; }
fseam::Level backbuffer_level() { return g_bb_level; }
fseam::Level hudless_level() { return g_hud_level; }
bool hooked() { return g_hooked.load(std::memory_order_acquire); }

void on_context_discovered(const u0::CtxDiscovery &ctx, const seam::Image &image)
{
	std::lock_guard<std::mutex> lock(g_mutex);
	g_gfx = u0::discover_graphics_half(image, ctx);
	if (g_gfx.status != u0::GfxStatus::ok)
	{
		STRAY_LOG_WARN("RHI GFX SEAM: the graphics half of the FD3D12CommandContext vtable REFUSED - %s (failedSlot=%u). "
			"Nothing is installed; EngineSeamBackBuffer and EngineSeamHudless are inert this session. The compute "
			"half (U0Hook) is unaffected.", u0::gfx_status_text(g_gfx.status), g_gfx.failed_slot);
		return;
	}
	g_stats.discovered = true;
	STRAY_LOG_INFO("RHI GFX SEAM FOUND: slots 38..85 of the vtable at %#llx are all inside the game's code, the eleven "
		"virtuals whose only body on FD3D12CommandContext is the base's `{}` (ResummarizeHTile, PollOcclusionQueries, "
		"DiscardRenderTargets, the four Begin/EndUpdateMultiFrameResource, SetStereoViewport, Begin/EndLateLatching, "
		"NextSubpass) each begin with `ret` (%u of them folded onto one address by ICF), and exactly one of the "
		"reversed RHICalibrateTimers pair (slot %u) is empty. Seams: BeginDrawingViewport=%#llx EndDrawingViewport=%#llx "
		"BeginRenderPass=%#llx EndRenderPass=%#llx.",
		static_cast<unsigned long long>(g_gfx.vtable_va), g_gfx.ret_fold, g_gfx.calibrate_ret_slot,
		static_cast<unsigned long long>(g_gfx.slot[u0::kGfxSlotBeginDrawingViewport]),
		static_cast<unsigned long long>(g_gfx.slot[u0::kGfxSlotEndDrawingViewport]),
		static_cast<unsigned long long>(g_gfx.slot[u0::kGfxSlotBeginRenderPass]),
		static_cast<unsigned long long>(g_gfx.slot[u0::kGfxSlotEndRenderPass]));
	if (g_bb_level == fseam::Level::off && g_hud_level == fseam::Level::off)
	{
		STRAY_LOG_INFO("RHI GFX SEAM: both keys are 0, so nothing is installed.");
		return;
	}
	install_locked();
}

FrameIdentity identity()
{
	std::lock_guard<std::mutex> lock(g_mutex);
	FrameIdentity id = g_identity;
	if (g_identity_consumed)
		id.engine_res = 0; // no bracket closed since the last present: nothing to say
	return id;
}

std::uint64_t select_game_frame(std::uint64_t mirror_res)
{
	std::lock_guard<std::mutex> lock(g_mutex);
	fseam::IdentityInputs in;
	in.engine_res = g_identity_consumed ? 0 : g_identity.engine_res;
	in.engine_is_candidate = g_identity.candidate;
	in.mirror_res = mirror_res;
	const fseam::Selection s = fseam::select_game_frame(g_bb_level, in);
	if (s.source == fseam::FrameSource::engine)
		++g_stats.selected_engine;
	else
		++g_stats.selected_mirror;
	if (s.fell_back)
		++g_stats.select_fell_back;
	return s.res;
}

void note_present(std::uint64_t mirror_res, std::uint64_t barrier_res, std::uint64_t present_frame)
{
	std::lock_guard<std::mutex> lock(g_mutex);
	if (!g_hooked.load(std::memory_order_acquire))
		return;
	// ---- deliverable 1: the three witnesses ----
	fseam::IdentityInputs in;
	in.engine_res = g_identity_consumed ? 0 : g_identity.engine_res;
	in.engine_is_candidate = g_identity.candidate;
	in.mirror_res = mirror_res;
	in.barrier_res = barrier_res;
	const fseam::IdentityJudgement j = fseam::judge_identity(in);
	++g_stats.id_vs_mirror[static_cast<int>(j.vs_mirror)];
	++g_stats.id_vs_barrier[static_cast<int>(j.vs_barrier)];
	if (j.vs_mirror == fseam::IdVerdict::disagree && !g_id_disagree_logged)
	{
		g_id_disagree_logged = true;
		STRAY_LOG_WARN("BACK-BUFFER IDENTITY ASSERTION: the engine's RHIBeginRenderPass resolved this frame's back buffer to "
			"%p and the model (GameIndexMirror / DXGI's current index) says %p. THEY DIFFER. %s Once per session; "
			"the rate is `idVsMirror disagree=` on the [rhigfx] line.",
			reinterpret_cast<void *>(in.engine_res), reinterpret_cast<void *>(mirror_res),
			g_bb_level == fseam::Level::authoritative ? "LEVEL 2: the engine's answer was used for this frame."
			                                          : "Level 1: the model's answer was used; a stale presented frame is what a wrong model looks like.");
	}
	if (j.vs_barrier == fseam::IdVerdict::disagree && !g_id_barrier_disagree_logged)
	{
		g_id_barrier_disagree_logged = true;
		STRAY_LOG_WARN("BACK-BUFFER IDENTITY ASSERTION: the engine's RHIBeginRenderPass resolved %p and the engine's own "
			"PRESENT transition (FD3D12Viewport::Present) named %p. Two engine routes DISAGREE - the render-pass "
			"resolve is reading the wrong frame's buffer, or the barrier ledger is a frame behind. Once per "
			"session; the rate is `idVsBarrier disagree=`.",
			reinterpret_cast<void *>(in.engine_res), reinterpret_cast<void *>(barrier_res));
	}
	g_identity_consumed = true;

	// ---- deliverable 4: the frame's shape ----
	const fseam::FrameShape shape = fseam::judge_frame(g_frame);
	++g_stats.shape[static_cast<int>(shape)];
	if (shape == fseam::FrameShape::ok)
		++g_stats.shape_ok_streak;
	else
		g_stats.shape_ok_streak = 0;
	if (!g_shape_logged[static_cast<int>(shape)])
	{
		g_shape_logged[static_cast<int>(shape)] = true;
		STRAY_LOG_INFO("RHI GFX SEAM: first frame with shape %s - brackets=%u bbScene=%u bbInside=%u hud=%d firstInsideAction=%u "
			"(present %llu). Predicted from the source: exactly one RHIBeginDrawingViewport (Slate's), the tonemapper's "
			"pass on the back buffer BEFORE it, one LOAD pass on the back buffer inside it. Once per shape; the "
			"distribution is `shape:` on the [rhigfx] line.",
			fseam::frame_shape_name(shape), g_frame.brackets, g_frame.bb_outside, g_frame.bb_inside, g_frame.hud_seen ? 1 : 0,
			g_frame.first_inside_action, static_cast<unsigned long long>(present_frame));
	}
	if (shape == fseam::FrameShape::ok && g_frames_logged < static_cast<std::uint64_t>(g_cfg.log_passes))
		++g_frames_logged;
	else if (g_frames_logged < static_cast<std::uint64_t>(g_cfg.log_passes) && g_stats.frames > 64)
		g_frames_logged = static_cast<std::uint64_t>(g_cfg.log_passes); // stop logging passes on a title that never shows the shape

	// ---- the periodic re-verification of the latch ----
	if (g_latch.latched() && present_frame - g_last_reverify_frame >= kReverifyEvery)
	{
		g_last_reverify_frame = present_frame;
		bool candidate = false;
		const std::uint64_t r = resolve_and_check(g_latch.value(), &candidate);
		if (r == 0 || !candidate)
		{
			++g_stats.relatch;
			g_latch.reset();
			g_stats.latched = false;
			g_stats.latched_texture = 0;
		}
	}
	g_frame.reset();
	g_frame_res_inside = 0;
	g_frame_res_candidate = false;
}

void note_reconfigure(const char *why)
{
	std::lock_guard<std::mutex> lock(g_mutex);
	if (g_latch.latched())
		STRAY_LOG_INFO("RHI GFX SEAM: latch dropped (%s) - the engine recreates its back-buffer reference on a resize.", why != nullptr ? why : "?");
	g_latch.reset();
	g_stats.latched = false;
	g_stats.latched_texture = 0;
	g_stats.shape_ok_streak = 0;
	g_frame.reset();
	g_frame_res_inside = 0;
	g_frame_res_candidate = false;
	g_identity_consumed = true;
}

void carry(ID3D12GraphicsCommandList *list)
{
	if (!t_copy_pending)
		return;
	t_copy_pending = false;
	const Pending p = t_pending;
	t_pending = Pending{};
	if (list == nullptr || p.back_buffer == 0)
		return;
	ID3D12Device *device = nullptr;
	list->GetDevice(IID_PPV_ARGS(&device));
	if (device == nullptr)
		return;
	auto *bb = reinterpret_cast<ID3D12Resource *>(static_cast<std::uintptr_t>(p.back_buffer));
	ID3D12Resource *dst = nullptr;
	{
		// OURS, not the game's: the copy's own barriers go through the same hooked
		// ResourceBarrier slot and must not be recorded into the ledger as engine statements.
		native::OwnCodeScope own;
		const D3D12_RESOURCE_DESC like = bb->GetDesc();
		const bool ready = hudless::ensure(device, like, p.frame);
		device->Release();
		if (!ready)
			return;
		// RENDER_TARGET by the engine's own construction (hudless.hpp); the ledger judges it when
		// the list executes.
		dst = hudless::record_copy(list, bb, fseam::kStateRenderTarget, p.frame);
	}
	native::bbstate::on_marker(list, p.back_buffer, fseam::kStateRenderTarget);
	std::lock_guard<std::mutex> lock(g_mutex);
	if (dst != nullptr)
		++g_stats.copies;
}

Stats stats()
{
	std::lock_guard<std::mutex> lock(g_mutex);
	return g_stats;
}

int format_report(char *buffer, std::size_t size)
{
	if (buffer == nullptr || size == 0)
		return 0;
	const Stats s = stats();
	const auto pc = [&](fseam::PassClass x) { return static_cast<unsigned long long>(s.pass_class[static_cast<int>(x)]); };
	const auto sh = [&](fseam::FrameShape x) { return static_cast<unsigned long long>(s.shape[static_cast<int>(x)]); };
	const auto im = [&](fseam::IdVerdict x) { return static_cast<unsigned long long>(s.id_vs_mirror[static_cast<int>(x)]); };
	const auto ib = [&](fseam::IdVerdict x) { return static_cast<unsigned long long>(s.id_vs_barrier[static_cast<int>(x)]); };
	const auto cr = [&](fseam::CopyRefusal x) { return static_cast<unsigned long long>(s.copy_refusal[static_cast<int>(x)]); };
	return std::snprintf(buffer, size,
		"rhigfx bb=%s hud=%s discovered=%d hooked=%d frames=%llu brackets(begin=%llu end=%llu) passes=%llu decodeFailed=%llu "
		"| class: bbScene=%llu bbHud=%llu bbLater=%llu bbNoLoad=%llu other=%llu | shape: ok=%llu noBracket=%llu manyBrackets=%llu "
		"noScenePass=%llu noHud=%llu streak=%u | latch=%d tex=%#llx resolves=%llu failed=%llu notCandidate=%llu relatch=%llu "
		"| idVsMirror agree=%llu disagree=%llu engineAbsent=%llu notCandidate=%llu otherAbsent=%llu "
		"| idVsBarrier agree=%llu disagree=%llu engineAbsent=%llu notCandidate=%llu otherAbsent=%llu "
		"| selected engine=%llu mirror=%llu fellBack=%llu | copies=%llu copyRefused: notHud=%llu noLatch=%llu shape=%llu "
		"levelOff=%llu carrierLost=%llu | name rdata=%llu elsewhere=%llu | otherThread=%u faults=%llu off=%d",
		fseam::level_name(g_bb_level), fseam::level_name(g_hud_level), s.discovered ? 1 : 0, s.hooked ? 1 : 0,
		static_cast<unsigned long long>(s.frames), static_cast<unsigned long long>(s.begin_viewport),
		static_cast<unsigned long long>(s.end_viewport), static_cast<unsigned long long>(s.begin_pass),
		static_cast<unsigned long long>(s.pass_decode_failed),
		pc(fseam::PassClass::bb_outside_bracket), pc(fseam::PassClass::bb_hud), pc(fseam::PassClass::bb_in_bracket_later),
		pc(fseam::PassClass::bb_in_bracket_no_load), pc(fseam::PassClass::not_back_buffer),
		sh(fseam::FrameShape::ok), sh(fseam::FrameShape::no_bracket), sh(fseam::FrameShape::many_brackets),
		sh(fseam::FrameShape::no_scene_pass), sh(fseam::FrameShape::no_hud), s.shape_ok_streak,
		s.latched ? 1 : 0, static_cast<unsigned long long>(s.latched_texture), static_cast<unsigned long long>(s.resolves),
		static_cast<unsigned long long>(s.resolve_failed), static_cast<unsigned long long>(s.resolve_not_candidate),
		static_cast<unsigned long long>(s.relatch),
		im(fseam::IdVerdict::agree), im(fseam::IdVerdict::disagree), im(fseam::IdVerdict::engine_absent),
		im(fseam::IdVerdict::not_candidate), im(fseam::IdVerdict::other_absent),
		ib(fseam::IdVerdict::agree), ib(fseam::IdVerdict::disagree), ib(fseam::IdVerdict::engine_absent),
		ib(fseam::IdVerdict::not_candidate), ib(fseam::IdVerdict::other_absent),
		static_cast<unsigned long long>(s.selected_engine), static_cast<unsigned long long>(s.selected_mirror),
		static_cast<unsigned long long>(s.select_fell_back), static_cast<unsigned long long>(s.copies),
		cr(fseam::CopyRefusal::not_hud_pass), cr(fseam::CopyRefusal::no_latch), cr(fseam::CopyRefusal::shape_unproven),
		cr(fseam::CopyRefusal::level_off), static_cast<unsigned long long>(s.carrier_lost),
		static_cast<unsigned long long>(s.name_in_rdata), static_cast<unsigned long long>(s.name_elsewhere),
		s.other_thread, static_cast<unsigned long long>(s.faults), s.disabled ? 1 : 0);
}

void log_report(const char *when)
{
	if (g_bb_level == fseam::Level::off && g_hud_level == fseam::Level::off)
		return;
	char line[1200] = {};
	format_report(line, sizeof(line));
	STRAY_LOG_INFO("[rhigfx] %s: %s  (brackets must equal presents; idVsMirror/idVsBarrier disagree, notCandidate, "
		"carrierLost and otherThread must stay 0; shape must read ok)", when != nullptr ? when : "", line);
}

void shutdown()
{
	std::lock_guard<std::mutex> lock(g_mutex);
	unsigned restored = 0;
	for (unsigned i = 0; i < kSeams; ++i)
	{
		if (g_patched[i] == nullptr)
			continue;
		if (*g_patched[i] == reinterpret_cast<void *>(thunk_for(i)))
		{
			void *ignored = nullptr;
			u0hook::patch_vtable_slot(g_patched[i], reinterpret_cast<void *>(g_orig[i]), &ignored);
			++restored;
		}
		g_patched[i] = nullptr;
	}
	if (restored != 0)
		STRAY_LOG_INFO("RHI GFX SEAM: %u slot(s) restored.", restored);
	g_hooked.store(false, std::memory_order_release);
}

} // namespace stray_dlss::rhigfx
