#include "mv_mask.hpp"

#include "core/ring.hpp"
#include "log.hpp"

#include <d3d12.h>

#include <cstdio>
#include <cstring>

namespace stray_dlss::mvmask {
namespace {

bool g_enabled = false;
float g_value = mvmaskplan::kNeutral;
std::uint32_t g_alternate_period = 0;
int g_format = kDefaultFormat;

struct State
{
	ID3D12Device *device = nullptr;
	ID3D12Resource *texture = nullptr;
	// ClearUnorderedAccessViewFloat needs the SAME descriptor twice — a GPU handle in a
	// shader-visible heap that is bound on the list, and a CPU handle in a non-shader-visible
	// one. Identical to mv_resolve's paint diagnostic, which is where this pattern is proven.
	ID3D12DescriptorHeap *heap_visible = nullptr;
	ID3D12DescriptorHeap *heap_staging = nullptr;
	std::uint32_t descriptor_size = 0;
	std::uint64_t clear_slot = 0;

	std::uint32_t width = 0;
	std::uint32_t height = 0;
	DXGI_FORMAT dxgi_format = DXGI_FORMAT_R8_UNORM;

	// What was last WRITTEN, not what was last asked for. `filled` false means the contents are
	// undefined and the texture must not be bound: per-pixel noise on the one channel whose job
	// is to say which pixels to distrust is worse than no mask at all.
	bool filled = false;
	float filled_value = 0.0f;

	// Ours from creation, so this is known exactly rather than assumed — the difference between
	// this and the back-buffer state constant nr_hook.cpp has to take on trust.
	D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

	Stats stats;
	char error[192] = {};
};

State g;

void set_error(const char *what, HRESULT hr)
{
	std::snprintf(g.error, sizeof(g.error), "mv_mask: %s failed (hr=0x%08X)", what,
		static_cast<unsigned int>(hr));
	STRAY_LOG_ERROR("%s", g.error);
}

template <typename T> void release(T *&p)
{
	if (p != nullptr)
	{
		p->Release();
		p = nullptr;
	}
}

DXGI_FORMAT dxgi_for(int format)
{
	switch (format)
	{
	case kFormatR16Float:   return DXGI_FORMAT_R16_FLOAT;
	case kFormatRgba8Unorm: return DXGI_FORMAT_R8G8B8A8_UNORM;
	case kFormatR8Unorm:
	default:                return DXGI_FORMAT_R8_UNORM;
	}
}

void transition(ID3D12GraphicsCommandList *cmd, D3D12_RESOURCE_STATES after)
{
	if (g.state == after)
		return;
	D3D12_RESOURCE_BARRIER b = {};
	b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	b.Transition.pResource = g.texture;
	b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	b.Transition.StateBefore = g.state;
	b.Transition.StateAfter = after;
	cmd->ResourceBarrier(1, &b);
	g.state = after;
}

} // namespace

void configure(bool enabled_in, float value, std::uint32_t alternate_period, int format)
{
	g_enabled = enabled_in;
	g_value = value;
	g_alternate_period = alternate_period;
	g_format = format;
}

bool enabled() { return g_enabled; }

const Stats &stats() { return g.stats; }

const char *last_error() { return g.error; }

bool initialise(ID3D12Device *device, std::uint32_t width, std::uint32_t height)
{
	if (device == nullptr || width == 0 || height == 0)
		return false;

	const DXGI_FORMAT want = dxgi_for(g_format);

	if (g.device != device || g.dxgi_format != want)
	{
		shutdown();
		g.device = device;
		g.dxgi_format = want;
	}

	// GROW-ONLY, for the reason mv_resolve records at length: the render resolution comes from
	// whichever dispatch matched, so it alternates during a load, and freeing on a frame counter
	// has nothing to do with when the GPU finished. A smaller frame simply uses part of the
	// texture, and the plan's extent check only demands that the mask COVER the render rect.
	if (g.texture != nullptr && width <= g.width && height <= g.height)
		return true;

	if (g.texture != nullptr)
	{
		// Rare — at most a couple of times a session. Nothing is freed; the old texture is
		// leaked deliberately rather than released under a possibly in-flight command list.
		width = width > g.width ? width : g.width;
		height = height > g.height ? height : g.height;
		g.texture = nullptr;
		g.heap_visible = nullptr;
		g.heap_staging = nullptr;
		g.filled = false;
		g.state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
	}

	D3D12_HEAP_PROPERTIES heap = {};
	heap.Type = D3D12_HEAP_TYPE_DEFAULT;

	D3D12_RESOURCE_DESC tex = {};
	tex.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	tex.Width = width;
	tex.Height = height;
	tex.DepthOrArraySize = 1;
	tex.MipLevels = 1;
	tex.Format = g.dxgi_format;
	tex.SampleDesc.Count = 1;
	tex.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	tex.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

	HRESULT hr = g.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &tex,
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&g.texture));
	if (FAILED(hr))
	{
		set_error("CreateCommittedResource(mask)", hr);
		return false;
	}
	g.state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

	D3D12_DESCRIPTOR_HEAP_DESC hd = {};
	hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	hd.NumDescriptors = ring::kFrameCount;
	hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	hr = g.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g.heap_visible));
	if (FAILED(hr))
	{
		set_error("CreateDescriptorHeap(visible)", hr);
		return false;
	}
	hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	hr = g.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g.heap_staging));
	if (FAILED(hr))
	{
		set_error("CreateDescriptorHeap(staging)", hr);
		return false;
	}

	g.descriptor_size =
		g.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	g.width = width;
	g.height = height;
	g.filled = false;
	g.stats.width = width;
	g.stats.height = height;
	g.stats.format = g_format;

	STRAY_LOG_INFO("mv_mask ready: bias-current-colour mask %ux%u fmt=%d (0=R8_UNORM "
		"1=R16_FLOAT 2=RGBA8_UNORM). Nothing is bound until MvMask=1.",
		width, height, g_format);
	return true;
}

void shutdown()
{
	release(g.texture);
	release(g.heap_visible);
	release(g.heap_staging);
	g.device = nullptr;
	g.width = 0;
	g.height = 0;
	g.filled = false;
	g.state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
}

ID3D12Resource *record(ID3D12GraphicsCommandList *cmd, std::uint32_t render_width,
	std::uint32_t render_height, std::uint64_t frame)
{
	const bool ready = cmd != nullptr && g.texture != nullptr && g.heap_visible != nullptr &&
		g.heap_staging != nullptr;

	// Planned TWICE, deliberately. The first call decides whether this frame wants a mask at
	// all; the fill then happens; the second call decides whether to bind, against the state
	// the fill left behind. That is why `unfilled` should read zero in a healthy session —
	// the live path repairs it within the same call — while the pure gate still refuses to
	// hand DLSS a texture whose contents are undefined.
	const mvmaskplan::Plan want = mvmaskplan::plan(g_enabled, ready, /*filled=*/true, g.width,
		g.height, render_width, render_height, g_value, g_alternate_period, frame);

	g.stats.phase_on = want.phase_on;

	if (!want.bind)
	{
		g.stats.refused[static_cast<int>(want.refusal)]++;
		return nullptr;
	}

	const mvmaskplan::Plan &p = want;

	if (!g.filled || g.filled_value != p.value)
	{
		const std::uint32_t slot = ring::slot_for_frame(g.clear_slot++);
		const SIZE_T off = static_cast<SIZE_T>(slot) * g.descriptor_size;

		D3D12_CPU_DESCRIPTOR_HANDLE cpu_vis = g.heap_visible->GetCPUDescriptorHandleForHeapStart();
		cpu_vis.ptr += off;
		D3D12_GPU_DESCRIPTOR_HANDLE gpu_vis = g.heap_visible->GetGPUDescriptorHandleForHeapStart();
		gpu_vis.ptr += off;
		D3D12_CPU_DESCRIPTOR_HANDLE cpu_stage =
			g.heap_staging->GetCPUDescriptorHandleForHeapStart();
		cpu_stage.ptr += off;

		D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
		uav.Format = g.dxgi_format;
		uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
		g.device->CreateUnorderedAccessView(g.texture, nullptr, &uav, cpu_vis);
		g.device->CreateUnorderedAccessView(g.texture, nullptr, &uav, cpu_stage);

		transition(cmd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

		// SetDescriptorHeaps is required for the GPU handle to be valid, and it clobbers
		// whatever the game had bound — which is why this runs on the same list, in the same
		// window, as the resolve and the NGX evaluate, both of which already do it and after
		// which the caller restores the game's state anyway.
		ID3D12DescriptorHeap *heaps[] = { g.heap_visible };
		cmd->SetDescriptorHeaps(1, heaps);

		const float fill[4] = { p.value, p.value, p.value, p.value };
		cmd->ClearUnorderedAccessViewFloat(gpu_vis, cpu_stage, g.texture, fill, 0, nullptr);

		g.filled = true;
		g.filled_value = p.value;
		g.stats.fills++;
		g.stats.value = p.value;

		STRAY_LOG_INFO("MV MASK PHASE %s at frame %llu: bias-current-colour mask filled with "
			"%.4f (0 = neutral = the same as binding nothing)",
			p.phase_on ? "ACTIVE" : "neutral",
			static_cast<unsigned long long>(frame), static_cast<double>(p.value));
	}

	const mvmaskplan::Plan bind = mvmaskplan::plan(g_enabled, ready, g.filled, g.width, g.height,
		render_width, render_height, g_value, g_alternate_period, frame);
	if (!bind.bind)
	{
		g.stats.refused[static_cast<int>(bind.refusal)]++;
		return nullptr;
	}

	// DLSS requires every input in NON_PIXEL_SHADER_RESOURCE. NVIDIA's guide is explicit that a
	// wrong usage state can produce a black output with no error at all.
	transition(cmd, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

	g.stats.bound++;
	return g.texture;
}

int format_report(char *buf, std::size_t size)
{
	if (buf == nullptr || size == 0)
		return 0;
	return std::snprintf(buf, size,
		"MV MASK: enabled=%d phase=%s value=%.4f %ux%u fmt=%d bound=%llu fills=%llu "
		"refused: disabled=%llu notReady=%llu unfilled=%llu extent=%llu",
		g_enabled ? 1 : 0, g.stats.phase_on ? "ACTIVE" : "neutral",
		static_cast<double>(g.stats.value), g.stats.width, g.stats.height, g.stats.format,
		static_cast<unsigned long long>(g.stats.bound),
		static_cast<unsigned long long>(g.stats.fills),
		static_cast<unsigned long long>(g.stats.refused[
			static_cast<int>(mvmaskplan::Refusal::disabled)]),
		static_cast<unsigned long long>(g.stats.refused[
			static_cast<int>(mvmaskplan::Refusal::not_ready)]),
		static_cast<unsigned long long>(g.stats.refused[
			static_cast<int>(mvmaskplan::Refusal::unfilled)]),
		static_cast<unsigned long long>(g.stats.refused[
			static_cast<int>(mvmaskplan::Refusal::extent)]));
}

int format_status(char *buf, std::size_t size)
{
	if (buf == nullptr || size == 0)
		return 0;
	int written = std::snprintf(buf, size,
		"mv_mask_enabled=%d\nmv_mask_phase_on=%d\nmv_mask_value=%.4f\nmv_mask_bound=%llu\n"
		"mv_mask_fills=%llu\n",
		g_enabled ? 1 : 0, g.stats.phase_on ? 1 : 0, static_cast<double>(g.stats.value),
		static_cast<unsigned long long>(g.stats.bound),
		static_cast<unsigned long long>(g.stats.fills));
	if (written < 0)
		return written;
	for (int i = 0; i < static_cast<int>(mvmaskplan::Refusal::count); ++i)
	{
		const std::size_t used = static_cast<std::size_t>(written);
		if (used >= size)
			break;
		const int n = std::snprintf(buf + used, size - used, "mv_mask_refused_%s=%llu\n",
			mvmaskplan::refusal_name(static_cast<mvmaskplan::Refusal>(i)),
			static_cast<unsigned long long>(g.stats.refused[i]));
		if (n < 0)
			return n;
		written += n;
	}
	return written;
}

} // namespace stray_dlss::mvmask
