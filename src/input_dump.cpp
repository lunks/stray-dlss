#include "input_dump.hpp"

#include "log.hpp"

#include <cstdio>
#include <d3d12.h>
#include <mutex>
#include <vector>

namespace stray_dlss::input_dump {

namespace {

bool g_enabled = false;

// Steady-state gameplay moments, far apart enough that a live scene colour must differ.
constexpr std::uint64_t kDumpPoints[] = { 600, 900 };

struct Pending
{
	ID3D12Resource *readback = nullptr;
	std::uint64_t total_bytes = 0;
	D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
	char file[128] = {};
	std::uint64_t presents_left = 0;
};

std::mutex g_mutex;
std::vector<Pending> g_pending;

} // namespace

void set_enabled(bool enabled) { g_enabled = enabled; }
bool enabled() { return g_enabled; }

bool wants(std::uint64_t evaluate_count)
{
	if (!g_enabled)
		return false;
	for (const std::uint64_t p : kDumpPoints)
		if (evaluate_count == p)
			return true;
	return false;
}

bool capture(ID3D12Device *device, ID3D12GraphicsCommandList *cmd, ID3D12Resource *resource,
             unsigned int state_before, const char *name, std::uint64_t evaluate_count)
{
	if (device == nullptr || cmd == nullptr || resource == nullptr)
		return false;

	const D3D12_RESOURCE_DESC desc = resource->GetDesc();
	D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
	UINT64 total_bytes = 0;
	device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &total_bytes);
	if (total_bytes == 0)
		return false;

	D3D12_HEAP_PROPERTIES heap = {};
	heap.Type = D3D12_HEAP_TYPE_READBACK;
	D3D12_RESOURCE_DESC buf = {};
	buf.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	buf.Width = total_bytes;
	buf.Height = 1;
	buf.DepthOrArraySize = 1;
	buf.MipLevels = 1;
	buf.SampleDesc.Count = 1;
	buf.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	ID3D12Resource *readback = nullptr;
	HRESULT hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buf,
		D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback));
	if (FAILED(hr))
	{
		STRAY_LOG_ERROR("input_dump: readback alloc failed for %s (hr=0x%08x)", name,
			static_cast<unsigned int>(hr));
		return false;
	}

	D3D12_RESOURCE_BARRIER barrier = {};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Transition.pResource = resource;
	barrier.Transition.Subresource = 0;
	barrier.Transition.StateBefore = static_cast<D3D12_RESOURCE_STATES>(state_before);
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
	cmd->ResourceBarrier(1, &barrier);

	D3D12_TEXTURE_COPY_LOCATION src = {};
	src.pResource = resource;
	src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	src.SubresourceIndex = 0;
	D3D12_TEXTURE_COPY_LOCATION dst = {};
	dst.pResource = readback;
	dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	dst.PlacedFootprint = footprint;
	cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
	barrier.Transition.StateAfter = static_cast<D3D12_RESOURCE_STATES>(state_before);
	cmd->ResourceBarrier(1, &barrier);

	Pending p;
	p.readback = readback;
	p.total_bytes = total_bytes;
	p.footprint = footprint;
	p.presents_left = 5;
	std::snprintf(p.file, sizeof(p.file), "straydlss_%s_%llu.bin", name,
		static_cast<unsigned long long>(evaluate_count));
	STRAY_LOG_INFO("input_dump: captured %s at evaluate %llu — %ux%u fmt=%d pitch=%u (%llu bytes)",
		p.file, static_cast<unsigned long long>(evaluate_count),
		static_cast<unsigned int>(footprint.Footprint.Width), footprint.Footprint.Height,
		static_cast<int>(footprint.Footprint.Format), footprint.Footprint.RowPitch,
		static_cast<unsigned long long>(total_bytes));

	const std::lock_guard<std::mutex> lock(g_mutex);
	g_pending.push_back(p);
	return true;
}

void on_present()
{
	if (!g_enabled)
		return;
	const std::lock_guard<std::mutex> lock(g_mutex);
	for (auto it = g_pending.begin(); it != g_pending.end();)
	{
		if (--it->presents_left > 0)
		{
			++it;
			continue;
		}
		void *data = nullptr;
		const D3D12_RANGE read_range = { 0, static_cast<SIZE_T>(it->total_bytes) };
		if (SUCCEEDED(it->readback->Map(0, &read_range, &data)) && data != nullptr)
		{
			std::FILE *f = nullptr;
			fopen_s(&f, it->file, "wb");
			if (f != nullptr)
			{
				std::fwrite(data, 1, static_cast<size_t>(it->total_bytes), f);
				std::fclose(f);
				STRAY_LOG_INFO("input_dump: wrote %s", it->file);
			}
			else
			{
				STRAY_LOG_ERROR("input_dump: fopen failed for %s", it->file);
			}
			const D3D12_RANGE no_write = { 0, 0 };
			it->readback->Unmap(0, &no_write);
		}
		else
		{
			STRAY_LOG_ERROR("input_dump: map failed for %s", it->file);
		}
		it->readback->Release();
		it = g_pending.erase(it);
	}
}

} // namespace stray_dlss::input_dump
