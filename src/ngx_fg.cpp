#include "ngx_fg.hpp"

#include "core/fg_plan.hpp"
#include "ext_unhook.hpp"
#include "log.hpp"
#include "ngx_backend.hpp"

#include <d3d12.h>
#include <dxgi1_4.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>

#if STRAY_DLSS_ENABLE_NGX
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_defs.h>
#include <nvsdk_ngx_params.h>
#endif

namespace stray_dlss::ngxfg {
namespace {

Config g_cfg;
std::mutex g_mutex;
Stats g_stats;
char g_last_error[256] = "";

// ---- the published guides (TAA hook -> present) ----
//
// Two FG-owned copies of each guide alternate so the copy for frame N+1 can record while the
// evaluate for frame N (on the present list, later in the same queue) still reads the other.
struct GuideSet
{
	ID3D12Resource *depth = nullptr; // same desc as the engine's depth (R32G8X24_TYPELESS)
	ID3D12Resource *mvecs = nullptr; // same desc as our resolve output (RG16_FLOAT)
	FrameConstants constants;
	std::uint64_t sequence = 0;      // 0 = never published
};
GuideSet g_guides[2];
unsigned g_publish_next = 0;
std::atomic<std::uint64_t> g_publish_seq{ 0 }; // the newest publish; the present consumes it once
std::uint64_t g_consumed_seq = 0;
D3D12_RESOURCE_DESC g_depth_desc = {}, g_mv_desc = {};
bool g_guides_ok = false;

// ---- the feature ----
#if STRAY_DLSS_ENABLE_NGX
NVSDK_NGX_Parameter *g_params = nullptr;
NVSDK_NGX_Handle *g_feature = nullptr;
#endif
bool g_create_latched = false; // a failed create is not retried every present
std::uint32_t g_feature_w = 0, g_feature_h = 0;
unsigned g_feature_format = 0;
int g_feature_hdr = 0;
ID3D12Resource *g_output_real = nullptr; // optional second output the snippet may copy into
std::uint64_t g_presents_seen = 0;
bool g_pending_reset = false;
ID3D12Fence *g_idle_fence = nullptr;
std::uint64_t g_idle_value = 0;
HANDLE g_idle_event = nullptr;
// The matrices handed to NGX by pointer live here so they outlive the evaluate call.
float g_m_view_to_clip[16], g_m_clip_to_view[16], g_m_clip_to_prev[16], g_m_prev_to_clip[16];

void set_error(const char *what, unsigned result)
{
	std::snprintf(g_last_error, sizeof(g_last_error), "%s: 0x%08x (%s)", what, result, ngx::result_name(result));
	STRAY_LOG_ERROR("fg/ngx: %s", g_last_error);
}

template <typename T>
void release(T *&p)
{
	if (p != nullptr)
	{
		p->Release();
		p = nullptr;
	}
}

void barrier(ID3D12GraphicsCommandList *list, ID3D12Resource *res, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
	D3D12_RESOURCE_BARRIER b = {};
	b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	b.Transition.pResource = res;
	b.Transition.StateBefore = before;
	b.Transition.StateAfter = after;
	b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	list->ResourceBarrier(1, &b);
}

bool same_desc(const D3D12_RESOURCE_DESC &a, const D3D12_RESOURCE_DESC &b)
{
	return a.Width == b.Width && a.Height == b.Height && a.Format == b.Format && a.MipLevels == b.MipLevels &&
		a.DepthOrArraySize == b.DepthOrArraySize && a.SampleDesc.Count == b.SampleDesc.Count;
}

// A committed texture with `like`'s desc (so CopyResource is legal) in COMMON.
ID3D12Resource *create_like(ID3D12Device *device, const D3D12_RESOURCE_DESC &like, D3D12_RESOURCE_FLAGS flags, const wchar_t *name)
{
	D3D12_RESOURCE_DESC d = like;
	d.Flags = flags;
	d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	d.Alignment = 0;
	D3D12_HEAP_PROPERTIES heap = {};
	heap.Type = D3D12_HEAP_TYPE_DEFAULT;
	ID3D12Resource *out = nullptr;
	const HRESULT hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&out));
	if (FAILED(hr))
	{
		STRAY_LOG_ERROR("fg/ngx: CreateCommittedResource(%ls %ux%u fmt %d flags 0x%x) failed 0x%08lx", name,
			static_cast<unsigned>(d.Width), d.Height, static_cast<int>(d.Format), static_cast<unsigned>(d.Flags), static_cast<unsigned long>(hr));
		return nullptr;
	}
	out->SetName(name);
	return out;
}

void release_guides()
{
	for (GuideSet &g : g_guides)
	{
		release(g.depth);
		release(g.mvecs);
		g.sequence = 0;
	}
	g_guides_ok = false;
}

// Caller holds g_mutex.
bool ensure_guides_locked(ID3D12Device *device, ID3D12Resource *depth, ID3D12Resource *mvecs)
{
	const D3D12_RESOURCE_DESC dd = depth->GetDesc(), md = mvecs->GetDesc();
	if (g_guides_ok && same_desc(dd, g_depth_desc) && same_desc(md, g_mv_desc))
		return true;
	release_guides();
	// The depth copy keeps the engine's flags minus UAV (a depth format cannot carry it);
	// ALLOW_DEPTH_STENCIL stays because a D32S8-class typeless format requires it on some
	// runtimes. The MV copy needs nothing beyond what an SRV read wants.
	const D3D12_RESOURCE_FLAGS depth_flags = (dd.Flags & ~D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) & ~D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
	for (unsigned i = 0; i < 2; ++i)
	{
		wchar_t n1[40], n2[40];
		std::swprintf(n1, 40, L"stray-fg depth %u", i);
		std::swprintf(n2, 40, L"stray-fg mvecs %u", i);
		g_guides[i].depth = create_like(device, dd, depth_flags, n1);
		g_guides[i].mvecs = create_like(device, md, D3D12_RESOURCE_FLAG_NONE, n2);
		if (g_guides[i].depth == nullptr || g_guides[i].mvecs == nullptr)
		{
			release_guides();
			return false;
		}
	}
	g_depth_desc = dd;
	g_mv_desc = md;
	g_guides_ok = true;
	g_pending_reset = true; // the guide grid moved: DLSSG.Reset once (the NR lesson, CLAUDE.md §5)
	STRAY_LOG_WARN("fg/ngx: guide copies (re)created: depth %ux%u fmt %d flags 0x%x, mvecs %ux%u fmt %d",
		static_cast<unsigned>(dd.Width), dd.Height, static_cast<int>(dd.Format), static_cast<unsigned>(depth_flags),
		static_cast<unsigned>(md.Width), md.Height, static_cast<int>(md.Format));
	return true;
}

void wait_idle(ID3D12CommandQueue *queue)
{
	if (queue == nullptr || g_idle_fence == nullptr)
		return;
	queue->Signal(g_idle_fence, ++g_idle_value);
	if (g_idle_fence->GetCompletedValue() < g_idle_value && g_idle_event != nullptr)
	{
		g_idle_fence->SetEventOnCompletion(g_idle_value, g_idle_event);
		::WaitForSingleObject(g_idle_event, 5000);
	}
}

void release_feature(ID3D12CommandQueue *queue)
{
#if STRAY_DLSS_ENABLE_NGX
	if (g_feature != nullptr)
	{
		wait_idle(queue);
		ext_unhook::repair();
		const NVSDK_NGX_Result r = NVSDK_NGX_D3D12_ReleaseFeature(g_feature);
		STRAY_LOG_INFO("fg/ngx: ReleaseFeature 0x%08x (%s)", static_cast<unsigned>(r), ngx::result_name(static_cast<unsigned>(r)));
		g_feature = nullptr;
	}
	if (g_params != nullptr)
	{
		NVSDK_NGX_D3D12_DestroyParameters(g_params);
		g_params = nullptr;
	}
#else
	(void)queue;
#endif
	release(g_output_real);
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		g_stats.created = false;
	}
}

int resolve_hdr(unsigned format, unsigned color_space)
{
	if (g_cfg.hdr >= 0)
		return g_cfg.hdr;
	// SL guide §11.0: HDR means RGB10 + HDR10/BT.2100. R10G10B10A2 with the PQ colour space is
	// HDR for certain; R10G10B10A2 with no colour space ever set is HDR by this title's
	// configuration (gamescope --hdr-enabled, CLAUDE.md §1); anything else is SDR.
	// MEASURED on the box (facts §32.7): Stray never calls SetColorSpace1 - UE 4.27's default
	// back buffer is the 10-bit R10G10B10A2 in SDR (r.DefaultBackBufferPixelFormat), so a
	// 10-bit format with no colour space ever set is SDR, not HDR10.
	if (format == DXGI_FORMAT_R10G10B10A2_UNORM && color_space == static_cast<unsigned>(DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020))
		return 1;
	return 0;
}

#if STRAY_DLSS_ENABLE_NGX
// Every name here is in facts §32.2's PRESENT set. Keep it that way.
bool create_feature(ID3D12GraphicsCommandList *list, ID3D12CommandQueue *queue, ID3D12Device *device,
                    std::uint32_t w, std::uint32_t h, unsigned format, const FrameConstants &c)
{
	release_feature(queue);
	ext_unhook::repair();
	NVSDK_NGX_Result r = NVSDK_NGX_D3D12_AllocateParameters(&g_params);
	if (NVSDK_NGX_FAILED(r) || g_params == nullptr)
	{
		set_error("AllocateParameters", static_cast<unsigned>(r));
		return false;
	}
	const int hdr = resolve_hdr(format, native::fg::stats().color_space);
	g_params->Set(NVSDK_NGX_Parameter_Width, w);
	g_params->Set(NVSDK_NGX_Parameter_Height, h);
	g_params->Set(NVSDK_NGX_Parameter_OutWidth, w);
	g_params->Set(NVSDK_NGX_Parameter_OutHeight, h);
	g_params->Set(NVSDK_NGX_Parameter_CreationNodeMask, 1u);
	g_params->Set(NVSDK_NGX_Parameter_VisibilityNodeMask, 1u);
	g_params->Set("DLSSG.Width", w);
	g_params->Set("DLSSG.Height", h);
	g_params->Set("DLSSG.InternalWidth", c.render_width);
	g_params->Set("DLSSG.InternalHeight", c.render_height);
	g_params->Set("DLSSG.BackbufferFormat", format);
	g_params->Set("DLSSG.ColorBuffersHDR", static_cast<unsigned>(hdr));
	g_params->Set("DLSSG.DynamicResolution", 0u);
	g_params->Set("DLSSG.MultiFrameCount", 1u);
	g_params->Set("DLSSG.MultiFrameIndex", 0u);
	g_params->Set("DLSSG.UserInterfaceRecompositionEnabled", 0u);
	g_params->Set("DLSSG.MenuDetectionEnabled", 0u);
	g_params->Set("DLSSG.DepthInverted", 1u);
	g_params->Set("DLSSG.EvalFlags", 0u);
	if (g_cfg.provide_output_real)
	{
		D3D12_RESOURCE_DESC like = {};
		like.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		like.Width = w;
		like.Height = h;
		like.DepthOrArraySize = 1;
		like.MipLevels = 1;
		like.Format = static_cast<DXGI_FORMAT>(format);
		like.SampleDesc.Count = 1;
		g_output_real = create_like(device, like, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, L"stray-fg output-real");
	}
	r = NVSDK_NGX_D3D12_CreateFeature(list, NVSDK_NGX_Feature_FrameGeneration, g_params, &g_feature);
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		g_stats.create_result = static_cast<unsigned>(r);
	}
	if (NVSDK_NGX_FAILED(r) || g_feature == nullptr)
	{
		set_error("CreateFeature(FrameGeneration)", static_cast<unsigned>(r));
		release_feature(queue);
		return false;
	}
	unsigned mfc = 0;
	const bool have_mfc = NVSDK_NGX_SUCCEED(g_params->Get("DLSSG.MultiFrameCountMax", &mfc));
	unsigned must_call_eval = 0;
	const bool have_mce = NVSDK_NGX_SUCCEED(g_params->Get("DLSSG.MustCallEval", &must_call_eval));
	g_feature_w = w;
	g_feature_h = h;
	g_feature_format = format;
	g_feature_hdr = hdr;
	g_pending_reset = true;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		g_stats.created = true;
		g_stats.width = w;
		g_stats.height = h;
		g_stats.render_width = c.render_width;
		g_stats.render_height = c.render_height;
		g_stats.hdr = hdr;
		g_stats.multi_frame_count_max = have_mfc ? mfc : 0;
	}
	char mfc_text[24], mce_text[24];
	std::snprintf(mfc_text, sizeof(mfc_text), have_mfc ? "%u" : "absent", mfc);
	std::snprintf(mce_text, sizeof(mce_text), have_mce ? "%u" : "absent", must_call_eval);
	STRAY_LOG_WARN("fg/ngx: DLSS-G feature CREATED: %ux%u fmt %u hdr=%d internal %ux%u | snippet says MultiFrameCountMax=%s MustCallEval=%s | output-real=%s",
		w, h, format, hdr, c.render_width, c.render_height, mfc_text, mce_text, g_output_real != nullptr ? "provided" : "none");
	return true;
}
#endif

class NgxGenerator final : public native::fg::Generator
{
public:
	bool generate(ID3D12GraphicsCommandList *list, ID3D12Resource *real_current, ID3D12Resource *out,
	              std::uint32_t width, std::uint32_t height, unsigned dxgi_format, std::uint64_t frame,
	              const char **why) override
	{
#if !STRAY_DLSS_ENABLE_NGX
		(void)list; (void)real_current; (void)out; (void)width; (void)height; (void)dxgi_format; (void)frame;
		*why = "built without NGX";
		std::lock_guard<std::mutex> lock(g_mutex);
		++g_stats.refused_not_ready;
		return false;
#else
		++g_presents_seen;
		if (!ngx::status().initialised)
		{
			*why = "NGX is not initialised yet (it initialises lazily at frame 120)";
			std::lock_guard<std::mutex> lock(g_mutex);
			++g_stats.refused_not_ready;
			return false;
		}
		if (g_presents_seen < static_cast<std::uint64_t>(g_cfg.warmup_presents))
		{
			*why = "warming up (NgxFGWarmupFrames)";
			std::lock_guard<std::mutex> lock(g_mutex);
			++g_stats.refused_warmup;
			return false;
		}
		// The newest publish, consumed once: a present with no TAA dispatch since the last one
		// (a loading screen, a menu without the pass) has no guides and gets no generated frame.
		GuideSet *g = nullptr;
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			const std::uint64_t seq = g_publish_seq.load(std::memory_order_acquire);
			if (seq == 0 || seq == g_consumed_seq || !g_guides_ok)
			{
				++g_stats.refused_no_publish;
				*why = "no fresh guides published by the TAA hook this frame";
				return false;
			}
			g_consumed_seq = seq;
			g = &g_guides[(seq - 1) % 2];
		}
		ID3D12Device *device = nullptr;
		list->GetDevice(IID_PPV_ARGS(&device));
		if (device != nullptr)
			device->Release(); // the device outlives us; a borrowed pointer is enough
		ID3D12CommandQueue *queue = g_queue;
		if (g_idle_fence == nullptr && device != nullptr)
		{
			device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_idle_fence));
			g_idle_event = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
		}
		const bool size_changed = g_feature != nullptr && (g_feature_w != width || g_feature_h != height || g_feature_format != dxgi_format);
		if (size_changed)
		{
			STRAY_LOG_WARN("fg/ngx: swapchain %ux%u fmt %u -> %ux%u fmt %u: recreating the DLSS-G feature", g_feature_w, g_feature_h, g_feature_format, width, height, dxgi_format);
			release_feature(queue);
			g_create_latched = false;
		}
		if (g_feature == nullptr)
		{
			if (g_create_latched)
			{
				*why = "CreateFeature failed earlier (latched; see the fg/ngx ERROR line)";
				std::lock_guard<std::mutex> lock(g_mutex);
				++g_stats.refused_not_ready;
				return false;
			}
			if (device == nullptr || !create_feature(list, queue, device, width, height, dxgi_format, g->constants))
			{
				g_create_latched = true;
				*why = "CreateFeature(FrameGeneration) failed";
				return false;
			}
		}

		// ---- the evaluate ----
		ext_unhook::repair();
		const FrameConstants &c = g->constants;
		std::memcpy(g_m_view_to_clip, c.view_to_clip_no_aa, sizeof(g_m_view_to_clip));
		std::memcpy(g_m_clip_to_prev, c.clip_to_prev_clip, sizeof(g_m_clip_to_prev));
		const bool inv_ok = core::fg::invert4x4(g_m_view_to_clip, g_m_clip_to_view) && core::fg::invert4x4(g_m_clip_to_prev, g_m_prev_to_clip);
		if (!inv_ok)
		{
			*why = "a View matrix is singular (ViewToClip or ClipToPrevClip)";
			std::lock_guard<std::mutex> lock(g_mutex);
			++g_stats.refused_not_ready;
			return false;
		}
		const core::fg::CameraBasis basis = core::fg::camera_basis(c.translated_world_to_view);
		const float fov = core::fg::vertical_fov_radians(c.view_to_clip_no_aa);
		const float aspect = height != 0 ? static_cast<float>(width) / static_cast<float>(height) : 1.0f;
		const bool reset = c.reset || g_pending_reset;
		g_pending_reset = false;

		NVSDK_NGX_Parameter *p = g_params;
		p->Set("DLSSG.Backbuffer", real_current);
		p->Set("DLSSG.BackbufferSubrectBaseX", 0u);
		p->Set("DLSSG.BackbufferSubrectBaseY", 0u);
		p->Set("DLSSG.BackbufferSubrectWidth", width);
		p->Set("DLSSG.BackbufferSubrectHeight", height);
		p->Set("DLSSG.BackbufferFrameID", static_cast<unsigned>(frame & 0xFFFFFFFFu));
		p->Set("DLSSG.Depth", g->depth);
		p->Set("DLSSG.DepthSubrectBaseX", 0u);
		p->Set("DLSSG.DepthSubrectBaseY", 0u);
		p->Set("DLSSG.DepthSubrectWidth", c.render_width);
		p->Set("DLSSG.DepthSubrectHeight", c.render_height);
		p->Set("DLSSG.MVecs", g->mvecs);
		p->Set("DLSSG.MVecsSubrectBaseX", 0u);
		p->Set("DLSSG.MVecsSubrectBaseY", 0u);
		p->Set("DLSSG.MVecsSubrectWidth", c.render_width);
		p->Set("DLSSG.MVecsSubrectHeight", c.render_height);
		p->Set("DLSSG.OutputInterpolated", out);
		p->Set("DLSSG.OutputInterpolatedSubrectBaseX", 0u);
		p->Set("DLSSG.OutputInterpolatedSubrectBaseY", 0u);
		p->Set("DLSSG.OutputInterpolatedSubrectWidth", width);
		p->Set("DLSSG.OutputInterpolatedSubrectHeight", height);
		if (g_output_real != nullptr)
		{
			p->Set("DLSSG.OutputReal", g_output_real);
			p->Set("DLSSG.OutputRealSubrectBaseX", 0u);
			p->Set("DLSSG.OutputRealSubrectBaseY", 0u);
			p->Set("DLSSG.OutputRealSubrectWidth", width);
			p->Set("DLSSG.OutputRealSubrectHeight", height);
		}
		// Motion vectors: render-resolution pixels (CLAUDE.md §5), NGX sign. SL's guide §7.0:
		// pixel space -> mvecScale = 1/renderWidth, 1/renderHeight. Mode 2 sends 1.0 for A/B.
		const float sx = g_cfg.mvec_scale_mode == 2 || c.render_width == 0 ? 1.0f : 1.0f / static_cast<float>(c.render_width);
		const float sy = g_cfg.mvec_scale_mode == 2 || c.render_height == 0 ? 1.0f : 1.0f / static_cast<float>(c.render_height);
		p->Set("DLSSG.MvecScaleX", sx);
		p->Set("DLSSG.MvecScaleY", sy);
		p->Set("DLSSG.MvecJittered", 0u);
		p->Set("DLSSG.MvecDilated", 0u);
		p->Set("DLSSG.CameraMotionIncluded", 1u);
		p->Set("DLSSG.JitterOffsetX", c.jitter_x);
		p->Set("DLSSG.JitterOffsetY", c.jitter_y);
		p->Set("DLSSG.Reset", reset ? 1u : 0u);
		p->Set("DLSSG.DepthInverted", 1u);
		p->Set("DLSSG.OrthoProjection", 0u);
		p->Set("DLSSG.ColorBuffersHDR", static_cast<unsigned>(g_feature_hdr));
		p->Set("DLSSG.CameraViewToClip", static_cast<void *>(g_m_view_to_clip));
		p->Set("DLSSG.ClipToCameraView", static_cast<void *>(g_m_clip_to_view));
		p->Set("DLSSG.ClipToPrevClip", static_cast<void *>(g_m_clip_to_prev));
		p->Set("DLSSG.PrevClipToClip", static_cast<void *>(g_m_prev_to_clip));
		p->Set("DLSSG.CameraNear", c.near_plane);
		p->Set("DLSSG.CameraFar", g_cfg.camera_far);
		p->Set("DLSSG.CameraFOV", fov);
		p->Set("DLSSG.CameraAspectRatio", aspect);
		p->Set("DLSSG.CameraPinholeOffsetX", 0.0f);
		p->Set("DLSSG.CameraPinholeOffsetY", 0.0f);
		// Translated world is centred on the camera: its position is the origin there.
		p->Set("DLSSG.CameraPosX", 0.0f);
		p->Set("DLSSG.CameraPosY", 0.0f);
		p->Set("DLSSG.CameraPosZ", 0.0f);
		p->Set("DLSSG.CameraRightX", basis.right[0]);
		p->Set("DLSSG.CameraRightY", basis.right[1]);
		p->Set("DLSSG.CameraRightZ", basis.right[2]);
		p->Set("DLSSG.CameraUpX", basis.up[0]);
		p->Set("DLSSG.CameraUpY", basis.up[1]);
		p->Set("DLSSG.CameraUpZ", basis.up[2]);
		p->Set("DLSSG.CameraFwdX", basis.fwd[0]);
		p->Set("DLSSG.CameraFwdY", basis.fwd[1]);
		p->Set("DLSSG.CameraFwdZ", basis.fwd[2]);
		p->Set("DLSSG.MinRelativeLinearDepthObjectSeparation", 40.0f); // sl_consts.h default
		p->Set("DLSSG.NotRenderingGameFrames", 0u);
		// MEASURED on the box (facts §32.7): the index is 1-BASED. With index 0 the snippet
		// refuses every evaluate with FAIL_InvalidParameter and logs "Multi frame is not
		// supported on this device. Found index (0) but expected (1)"
		// (EndpointCoreInputs::ComputeAndValidateTimeFactor:418). One generated frame per real
		// frame is count 1, index 1.
		p->Set("DLSSG.MultiFrameCount", 1u);
		p->Set("DLSSG.MultiFrameIndex", 1u);

		// States: inputs NON_PIXEL_SHADER_RESOURCE, outputs UNORDERED_ACCESS (the NGX convention
		// SR uses, CLAUDE.md §5). Everything is COMMON on entry and left COMMON.
		barrier(list, real_current, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		barrier(list, g->depth, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		barrier(list, g->mvecs, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		barrier(list, out, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		if (g_output_real != nullptr)
			barrier(list, g_output_real, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		const NVSDK_NGX_Result r = NVSDK_NGX_D3D12_EvaluateFeature(list, g_feature, g_params, nullptr);
		barrier(list, real_current, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
		barrier(list, g->depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
		barrier(list, g->mvecs, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
		barrier(list, out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
		if (g_output_real != nullptr)
			barrier(list, g_output_real, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			g_stats.last_evaluate_result = static_cast<unsigned>(r);
			if (NVSDK_NGX_FAILED(r))
				++g_stats.evaluate_failures;
			else
				++g_stats.generates;
		}
		if (NVSDK_NGX_FAILED(r))
		{
			set_error("EvaluateFeature(FrameGeneration)", static_cast<unsigned>(r));
			*why = "EvaluateFeature failed";
			return false;
		}
		static std::atomic<int> s_logged{ 0 };
		if (s_logged.fetch_add(1) == 0)
			STRAY_LOG_WARN("fg/ngx: first DLSS-G evaluate OK: backbuffer %ux%u, guides %ux%u, jitter=%.4f,%.4f reset=%d fov=%.4f rad aspect=%.4f near=%.3f far=%.3f mvecScale=%.6f,%.6f hdr=%d",
				width, height, c.render_width, c.render_height, c.jitter_x, c.jitter_y, reset ? 1 : 0, fov, aspect, c.near_plane, g_cfg.camera_far, sx, sy, g_feature_hdr);
		return true;
#endif
	}

	void on_reconfigure() override
	{
		g_pending_reset = true;
	}

	ID3D12CommandQueue *g_queue = nullptr;
};

NgxGenerator g_generator;

} // namespace

void configure(const Config &cfg)
{
	g_cfg = cfg;
	STRAY_LOG_INFO("fg/ngx: hdr=%d warmup=%d cameraFar=%.3f mvecScaleMode=%d outputReal=%d", cfg.hdr, cfg.warmup_presents, cfg.camera_far, cfg.mvec_scale_mode, cfg.provide_output_real ? 1 : 0);
}

const Config &config() { return g_cfg; }

void publish(ID3D12Device *device, ID3D12GraphicsCommandList *game_list, ID3D12Resource *depth,
             ID3D12Resource *motion_vectors, const FrameConstants &c)
{
	if (device == nullptr || game_list == nullptr || depth == nullptr || motion_vectors == nullptr)
		return;
	std::lock_guard<std::mutex> lock(g_mutex);
	if (!ensure_guides_locked(device, depth, motion_vectors))
		return;
	GuideSet &g = g_guides[g_publish_next];
	g_publish_next ^= 1;
	// Depth is in NON_PIXEL_SHADER_RESOURCE (the engine transitioned it for the TAA dispatch we
	// replaced; taa_hook.cpp) and our motion vectors sit in the SRV state the evaluate used.
	barrier(game_list, depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
	barrier(game_list, g.depth, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
	game_list->CopyResource(g.depth, depth);
	barrier(game_list, g.depth, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
	barrier(game_list, depth, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	barrier(game_list, motion_vectors, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
	barrier(game_list, g.mvecs, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
	game_list->CopyResource(g.mvecs, motion_vectors);
	barrier(game_list, g.mvecs, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
	barrier(game_list, motion_vectors, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	g.constants = c;
	g.sequence = g_publish_seq.load(std::memory_order_relaxed) + 1;
	++g_stats.publishes;
	g_publish_seq.store(g.sequence, std::memory_order_release);
}

native::fg::Generator &generator() { return g_generator; }

void shutdown()
{
	release_feature(g_generator.g_queue);
	std::lock_guard<std::mutex> lock(g_mutex);
	release_guides();
	release(g_idle_fence);
	if (g_idle_event != nullptr)
	{
		::CloseHandle(g_idle_event);
		g_idle_event = nullptr;
	}
	g_consumed_seq = 0;
	g_publish_seq.store(0);
	g_create_latched = false;
}

Stats stats()
{
	std::lock_guard<std::mutex> lock(g_mutex);
	return g_stats;
}

const char *last_error() { return g_last_error; }

} // namespace stray_dlss::ngxfg
