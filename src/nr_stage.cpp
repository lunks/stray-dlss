#include "nr_stage.hpp"

#include "core/dxgi_format.hpp"
#include "core/ring.hpp"
#include "log.hpp"

#include <d3d12.h>

#include <cstddef>
#include <cstdio>
#include <mutex>
#include <vector>

namespace stray_dlss::nrstage {
namespace {

struct Staging
{
	ID3D12Resource *texture = nullptr;
	std::uint32_t width = 0;
	std::uint32_t height = 0;
	DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
	std::uint64_t bytes = 0;
};

struct Retired
{
	ID3D12Resource *texture = nullptr;
	std::uint64_t frame = 0;
};

// Everything below is touched from the present thread only (ensure/record/collect all run inside
// icept::Sink::on_present) EXCEPT shutdown(), which runs on device destruction. The mutex costs
// nothing at one lock per present and removes the need to reason about that one exception.
std::mutex g_mutex;
Staging g_staging;
std::vector<Retired> g_retired;
Stats g_stats;

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

// Bytes per pixel for the formats a swapchain can plausibly present. Only used for the allocation
// report, so an unknown format costs a 0 in a log line and nothing else.
std::uint32_t format_bytes(DXGI_FORMAT f)
{
	switch (static_cast<std::uint32_t>(f))
	{
	case dxgi::kR8G8B8A8Unorm:
	case dxgi::kR8G8B8A8UnormSrgb:
	case dxgi::kB8G8R8A8Unorm:
	case dxgi::kB8G8R8A8UnormSrgb:
	case dxgi::kR10G10B10A2Unorm:
	case dxgi::kR11G11B10Float:
		return 4;
	case dxgi::kR16G16B16A16Float:
	case dxgi::kR16G16B16A16Unorm:
		return 8;
	default:
		return 0;
	}
}

void transition(ID3D12GraphicsCommandList *cmd, ID3D12Resource *res, D3D12_RESOURCE_STATES from,
                D3D12_RESOURCE_STATES to)
{
	if (res == nullptr || from == to)
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

nrp::TypedUavSupport probe(ID3D12Device *device, int dxgi_format)
{
	const auto fmt = static_cast<DXGI_FORMAT>(dxgi_format);
	std::lock_guard<std::mutex> lock(g_mutex);
	if (g_probed_format == fmt && g_probe.queried)
		return g_probe;

	g_probed_format = fmt;
	g_probe = nrp::probe_typed_uav(device, dxgi_format);
	STRAY_LOG_INFO("NR stage: typed-UAV probe on the back buffer's format %u (%s) — queried=%d "
		"typedUavView=%d typedLoad=%d typedStore=%d. NGX writes DLSSNR.Output through a typed "
		"UAV, so VIEW and STORE decide; a missing typed LOAD is harmless here because the runtime "
		"reads DLSSNR.Color through its own path.",
		static_cast<unsigned int>(dxgi_format),
		dxgi_format_name(static_cast<std::uint32_t>(dxgi_format)), g_probe.queried ? 1 : 0,
		g_probe.view ? 1 : 0, g_probe.load ? 1 : 0, g_probe.store ? 1 : 0);
	if (g_probe.queried && g_probe.view && g_probe.store && !g_probe.load)
		STRAY_LOG_WARN("NR stage: the back buffer's format supports typed UAV STORE but not typed "
			"LOAD. Expected for some formats, not a refusal — noted only so a later problem is "
			"not blamed on it.");
	if (g_probe.queried && (!g_probe.view || !g_probe.store))
		STRAY_LOG_ERROR("NR stage: this device cannot write format %u through a typed UAV, so the "
			"staging pair cannot be allocated in the back buffer's own format and NGX could not "
			"fill DLSSNR.Output. The stage refuses every frame (no-typed-uav-store) rather than "
			"allocate a texture the driver will reject or the runtime will silently leave black. "
			"Converting through an FP16 intermediate would need a compute pass in each direction "
			"and is deliberately NOT built — see src/nr_stage.hpp.",
			static_cast<unsigned int>(dxgi_format));
	return g_probe;
}

bool ensure(ID3D12Device *device, std::uint32_t width, std::uint32_t height, int dxgi_format,
            std::uint64_t frame)
{
	if (device == nullptr || width == 0 || height == 0)
		return false;

	const auto fmt = static_cast<DXGI_FORMAT>(dxgi_format);
	std::lock_guard<std::mutex> lock(g_mutex);
	if (g_staging.texture != nullptr && g_staging.width == width && g_staging.height == height &&
		g_staging.format == fmt)
		return true;

	// Retire rather than release: the GPU may still be reading the old one several frames back.
	if (g_staging.texture != nullptr)
	{
		Retired r;
		r.texture = g_staging.texture;
		r.frame = frame;
		g_retired.push_back(r);
		g_staging.texture = nullptr;
		++g_stats.retired;
		g_stats.live_retired = static_cast<std::uint32_t>(g_retired.size());
	}

	D3D12_HEAP_PROPERTIES heap = {};
	heap.Type = D3D12_HEAP_TYPE_DEFAULT;

	// PLAIN, TYPED, SINGLE-SUBRESOURCE, and never a mirror of what the swapchain allocated. A
	// mipped texture handed to feature 18 is a documented DXGI_ERROR_DEVICE_HUNG arriving seconds
	// later rather than an error return; the same lesson as ngx_nr::ensure_output_texture, which
	// clones THIS desc for DLSSNR.Output.
	D3D12_RESOURCE_DESC tex = {};
	tex.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	tex.Width = width;
	tex.Height = height;
	tex.DepthOrArraySize = 1;
	tex.MipLevels = 1;
	tex.Format = fmt;
	tex.SampleDesc.Count = 1;
	tex.SampleDesc.Quality = 0;
	tex.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	// ALLOW_UNORDERED_ACCESS because ngx_nr clones this desc for DLSSNR.Output, which NGX writes
	// through a UAV — and a missing flag there is a black result with no error at all
	// (CLAUDE.md §5, "Missing that flag can produce a black output with no error").
	tex.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

	const HRESULT hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &tex,
		static_cast<D3D12_RESOURCE_STATES>(kStagingRestState), nullptr,
		IID_PPV_ARGS(&g_staging.texture));
	if (FAILED(hr))
	{
		std::snprintf(g_last_error, sizeof(g_last_error),
			"CreateCommittedResource(NR stage staging %ux%u fmt=%u) hr=0x%08x", width, height,
			static_cast<unsigned int>(dxgi_format), static_cast<unsigned int>(hr));
		STRAY_LOG_ERROR("NR stage: %s. The stage stays off this frame; the back buffer is "
			"untouched.", g_last_error);
		g_staging.texture = nullptr;
		g_staging.width = 0;
		g_staging.height = 0;
		return false;
	}

	g_staging.width = width;
	g_staging.height = height;
	g_staging.format = fmt;
	const std::uint32_t bpp = format_bytes(fmt);
	g_staging.bytes = static_cast<std::uint64_t>(width) * height * bpp;
	++g_stats.created;
	g_stats.bytes = g_staging.bytes;
	STRAY_LOG_INFO("NR stage: staging colour texture %ux%u fmt=%u (%s) created "
		"(single-mip/single-slice/non-MSAA, ALLOW_UNORDERED_ACCESS, %llu bytes). It is a plain "
		"same-format copy of the back buffer — no conversion, no shader of ours in the path. "
		"ngx_nr allocates a second one of these as DLSSNR.Output.",
		width, height, static_cast<unsigned int>(dxgi_format),
		dxgi_format_name(static_cast<std::uint32_t>(dxgi_format)),
		static_cast<unsigned long long>(g_staging.bytes));
	return true;
}

ID3D12Resource *staging()
{
	std::lock_guard<std::mutex> lock(g_mutex);
	return g_staging.texture;
}

void record_capture(ID3D12GraphicsCommandList *cmd, ID3D12Resource *colour,
                    std::uint32_t colour_state, BarrierFn barrier, void *ctx)
{
	std::lock_guard<std::mutex> lock(g_mutex);
	if (cmd == nullptr || colour == nullptr || g_staging.texture == nullptr || barrier == nullptr)
		return;

	const auto rest = static_cast<D3D12_RESOURCE_STATES>(kStagingRestState);
	barrier(ctx, colour, colour_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
	transition(cmd, g_staging.texture, rest, D3D12_RESOURCE_STATE_COPY_DEST);
	cmd->CopyResource(g_staging.texture, colour);
	transition(cmd, g_staging.texture, D3D12_RESOURCE_STATE_COPY_DEST, rest);
	// Put the back buffer back IMMEDIATELY. Holding it in COPY_SOURCE across the evaluate would
	// work, but every early return between here and the write-back — and there are several — would
	// leave the frame in a state Present does not expect.
	barrier(ctx, colour, D3D12_RESOURCE_STATE_COPY_SOURCE, colour_state);
}

void record_writeback(ID3D12GraphicsCommandList *cmd, ID3D12Resource *colour,
                      std::uint32_t colour_state, BarrierFn barrier, void *ctx)
{
	std::lock_guard<std::mutex> lock(g_mutex);
	if (cmd == nullptr || colour == nullptr || g_staging.texture == nullptr || barrier == nullptr)
		return;

	const auto rest = static_cast<D3D12_RESOURCE_STATES>(kStagingRestState);
	transition(cmd, g_staging.texture, rest, D3D12_RESOURCE_STATE_COPY_SOURCE);
	barrier(ctx, colour, colour_state, D3D12_RESOURCE_STATE_COPY_DEST);
	cmd->CopyResource(colour, g_staging.texture);
	barrier(ctx, colour, D3D12_RESOURCE_STATE_COPY_DEST, colour_state);
	transition(cmd, g_staging.texture, D3D12_RESOURCE_STATE_COPY_SOURCE, rest);
}

void collect(std::uint64_t frame)
{
	std::lock_guard<std::mutex> lock(g_mutex);
	for (std::size_t i = 0; i < g_retired.size();)
	{
		if (ring::is_safe_to_release(frame, g_retired[i].frame))
		{
			release(g_retired[i].texture);
			++g_stats.released;
			g_retired.erase(g_retired.begin() + static_cast<std::ptrdiff_t>(i));
		}
		else
		{
			++i;
		}
	}
	g_stats.live_retired = static_cast<std::uint32_t>(g_retired.size());
}

void shutdown()
{
	std::lock_guard<std::mutex> lock(g_mutex);
	if (!g_retired.empty())
		STRAY_LOG_WARN("NR stage: %u retired staging texture(s) were still inside the release "
			"window at shutdown and are being freed now. This is the one call site where the "
			"caller, not the ring, has to have made the GPU idle.",
			static_cast<unsigned int>(g_retired.size()));
	for (Retired &r : g_retired)
	{
		release(r.texture);
		++g_stats.released;
	}
	g_retired.clear();
	release(g_staging.texture);
	g_staging.width = 0;
	g_staging.height = 0;
	g_staging.format = DXGI_FORMAT_UNKNOWN;
	g_staging.bytes = 0;
	g_stats.live_retired = 0;
	g_stats.bytes = 0;
	g_probed_format = DXGI_FORMAT_UNKNOWN;
	g_probe = nrp::TypedUavSupport{};
}

Stats stats()
{
	std::lock_guard<std::mutex> lock(g_mutex);
	return g_stats;
}

const char *last_error() { return g_last_error; }

} // namespace stray_dlss::nrstage
