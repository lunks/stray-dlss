#include "hudless.hpp"

#include "core/dxgi_format.hpp"
#include "core/ring.hpp"
#include "log.hpp"

#include <d3d12.h>

#include <cstdio>
#include <cwchar>
#include <mutex>
#include <vector>

namespace stray_dlss::hudless {
namespace {

struct Ring
{
	ID3D12Resource *tex[kRing] = {};
	std::uint32_t width = 0;
	std::uint32_t height = 0;
	DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
	std::uint64_t bytes = 0;
};

struct Retired
{
	ID3D12Resource *tex[kRing] = {};
	std::uint64_t frame = 0;
};

// The copy is recorded on the RHI thread (inside the engine's RHIBeginRenderPass) and consumed on
// the present thread — the same thread on this title, but nothing here relies on it.
std::mutex g_mutex;
Ring g_ring;
std::vector<Retired> g_retired;
unsigned g_next = 0;
Current g_current;
Stats g_stats;
DumpFn g_dump = nullptr;
WantsFn g_wants = nullptr;
char g_last_error[192] = "";
bool g_first_logged = false;

void release(ID3D12Resource *&p)
{
	if (p != nullptr)
	{
		p->Release();
		p = nullptr;
	}
}

void transition(ID3D12GraphicsCommandList *cmd, ID3D12Resource *res, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
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

std::uint32_t format_bytes(DXGI_FORMAT f)
{
	switch (static_cast<std::uint32_t>(f))
	{
	case dxgi::kR16G16B16A16Float:
	case dxgi::kR16G16B16A16Unorm:
		return 8;
	default:
		return 4;
	}
}

} // namespace

bool ensure(ID3D12Device *device, const D3D12_RESOURCE_DESC &like, std::uint64_t frame)
{
	if (device == nullptr || like.Width == 0 || like.Height == 0)
		return false;
	std::lock_guard<std::mutex> lock(g_mutex);
	const auto w = static_cast<std::uint32_t>(like.Width);
	const std::uint32_t h = like.Height;
	if (g_ring.tex[0] != nullptr && g_ring.width == w && g_ring.height == h && g_ring.format == like.Format)
		return true;
	if (g_ring.tex[0] != nullptr)
	{
		Retired r;
		for (unsigned i = 0; i < kRing; ++i)
		{
			r.tex[i] = g_ring.tex[i];
			g_ring.tex[i] = nullptr;
		}
		r.frame = frame;
		g_retired.push_back(r);
		++g_stats.retired;
		g_current = Current{};
	}
	D3D12_HEAP_PROPERTIES heap = {};
	heap.Type = D3D12_HEAP_TYPE_DEFAULT;
	// PLAIN: single mip, single slice, no MSAA, the swapchain's own format; no flags — the two
	// consumers read it through NGX's own SRVs. (A mipped texture handed to a neural feature is
	// a documented device hang, CLAUDE.md §5.)
	D3D12_RESOURCE_DESC d = {};
	d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	d.Width = w;
	d.Height = h;
	d.DepthOrArraySize = 1;
	d.MipLevels = 1;
	d.Format = like.Format;
	d.SampleDesc.Count = 1;
	d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	d.Flags = D3D12_RESOURCE_FLAG_NONE;
	for (unsigned i = 0; i < kRing; ++i)
	{
		const HRESULT hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COMMON,
			nullptr, IID_PPV_ARGS(&g_ring.tex[i]));
		if (FAILED(hr))
		{
			std::snprintf(g_last_error, sizeof(g_last_error), "CreateCommittedResource(hudless %u %ux%u fmt=%u) hr=0x%08x",
				i, w, h, static_cast<unsigned>(like.Format), static_cast<unsigned>(hr));
			STRAY_LOG_ERROR("HUD-LESS: %s. No copy this session until a later ensure succeeds.", g_last_error);
			for (unsigned k = 0; k < kRing; ++k)
				release(g_ring.tex[k]);
			g_ring = Ring{};
			return false;
		}
		wchar_t name[40];
		std::swprintf(name, 40, L"stray-hudless %u", i);
		g_ring.tex[i]->SetName(name);
	}
	g_ring.width = w;
	g_ring.height = h;
	g_ring.format = like.Format;
	g_ring.bytes = static_cast<std::uint64_t>(w) * h * format_bytes(like.Format) * kRing;
	++g_stats.created;
	g_stats.bytes = g_ring.bytes;
	g_stats.width = w;
	g_stats.height = h;
	g_stats.format = static_cast<unsigned>(like.Format);
	STRAY_LOG_INFO("HUD-LESS: ring of %u %ux%u fmt=%u (%s) created, %llu bytes, resting in COMMON.", kRing, w, h,
		static_cast<unsigned>(like.Format), dxgi_format_name(static_cast<std::uint32_t>(like.Format)),
		static_cast<unsigned long long>(g_ring.bytes));
	return true;
}

ID3D12Resource *record_copy(ID3D12GraphicsCommandList *list, ID3D12Resource *back_buffer, std::uint32_t bb_state, std::uint64_t frame)
{
	if (list == nullptr || back_buffer == nullptr)
		return nullptr;
	std::lock_guard<std::mutex> lock(g_mutex);
	if (g_ring.tex[0] == nullptr)
	{
		++g_stats.not_ready;
		std::snprintf(g_last_error, sizeof(g_last_error), "the ring is not allocated");
		return nullptr;
	}
	ID3D12Resource *dst = g_ring.tex[g_next];
	g_next = (g_next + 1) % kRing;
	const auto before = static_cast<D3D12_RESOURCE_STATES>(bb_state);
	transition(list, back_buffer, before, D3D12_RESOURCE_STATE_COPY_SOURCE);
	transition(list, dst, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
	list->CopyResource(dst, back_buffer);
	transition(list, dst, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
	transition(list, back_buffer, D3D12_RESOURCE_STATE_COPY_SOURCE, before);
	++g_stats.copies;
	g_current.texture = dst;
	g_current.sequence = g_stats.copies;
	g_current.width = g_ring.width;
	g_current.height = g_ring.height;
	g_current.format = static_cast<unsigned>(g_ring.format);
	if (!g_first_logged)
	{
		g_first_logged = true;
		STRAY_LOG_WARN("HUD-LESS: first copy recorded on the game's list %p - back buffer %p (state 0x%X) -> %p, %ux%u, "
			"frame %llu. From here DLSS-G is handed it as DLSSG.HUDLess and the NR stage as DLSSNR.Color; the "
			"indicator's Hudless field and the NgxDumpInputs dump (straydlss_hudless_*.bin) are what say the copy "
			"holds the scene without the HUD.",
			static_cast<void *>(list), static_cast<void *>(back_buffer), bb_state, static_cast<void *>(dst),
			g_ring.width, g_ring.height, static_cast<unsigned long long>(frame));
	}
	if (g_dump != nullptr && g_wants != nullptr && g_wants(g_stats.copies))
	{
		ID3D12Device *device = nullptr;
		list->GetDevice(IID_PPV_ARGS(&device));
		if (device != nullptr)
		{
			if (g_dump(device, list, dst, D3D12_RESOURCE_STATE_COMMON, "hudless", g_stats.copies))
				++g_stats.dumps;
			device->Release();
		}
	}
	return dst;
}

Current current()
{
	std::lock_guard<std::mutex> lock(g_mutex);
	return g_current;
}

void collect(std::uint64_t frame)
{
	std::lock_guard<std::mutex> lock(g_mutex);
	for (std::size_t i = 0; i < g_retired.size();)
	{
		if (ring::is_safe_to_release(frame, g_retired[i].frame))
		{
			for (unsigned k = 0; k < kRing; ++k)
				release(g_retired[i].tex[k]);
			++g_stats.released;
			g_retired.erase(g_retired.begin() + static_cast<std::ptrdiff_t>(i));
		}
		else
			++i;
	}
}

void shutdown()
{
	std::lock_guard<std::mutex> lock(g_mutex);
	for (Retired &r : g_retired)
		for (unsigned k = 0; k < kRing; ++k)
			release(r.tex[k]);
	g_retired.clear();
	for (unsigned k = 0; k < kRing; ++k)
		release(g_ring.tex[k]);
	g_ring = Ring{};
	g_current = Current{};
	g_stats.bytes = 0;
}

void set_dump(DumpFn dump, WantsFn wants)
{
	std::lock_guard<std::mutex> lock(g_mutex);
	g_dump = dump;
	g_wants = wants;
}

Stats stats()
{
	std::lock_guard<std::mutex> lock(g_mutex);
	return g_stats;
}

const char *last_error() { return g_last_error; }

} // namespace stray_dlss::hudless
