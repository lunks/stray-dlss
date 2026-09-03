#include "exposure_texture.hpp"

#include "log.hpp"

#include <cmath>
#include <cstring>
#include <d3d12.h>
#include <limits>

namespace stray_dlss::exposure_texture {

namespace {

// The upload ring. A frame's 4 bytes must not be rewritten while the GPU may still be reading
// them, and D3D12 gives us no cheap fence here, so rotate far enough that it cannot matter: the
// keep-alive ring elsewhere in this add-on holds resources for 6 frames on the same reasoning,
// and 8 is comfortably past any plausible queue depth for a 4-byte copy.
constexpr unsigned int kRing = 8;

// D3D12_TEXTURE_DATA_PITCH_ALIGNMENT. A 1x1 R32_FLOAT is 4 bytes of content but its copy
// footprint's row pitch is still 256, and CopyTextureRegion reads the footprint, not the
// content — so the buffer must be at least this big or the copy walks off the end.
constexpr unsigned int kUploadBytes = 256;

struct State
{
	ID3D12Device *device = nullptr;
	ID3D12Resource *texture = nullptr;
	ID3D12Resource *upload[kRing] = {};
	void *mapped[kRing] = {};
	unsigned int next = 0;
	bool alloc_failed = false;   // one failure is permanent; do not retry every frame
	float last = std::numeric_limits<float>::quiet_NaN();
};

State g_state;

void free_all()
{
	for (unsigned int i = 0; i < kRing; ++i)
	{
		if (g_state.upload[i] != nullptr)
		{
			if (g_state.mapped[i] != nullptr)
				g_state.upload[i]->Unmap(0, nullptr);
			g_state.upload[i]->Release();
		}
		g_state.upload[i] = nullptr;
		g_state.mapped[i] = nullptr;
	}
	if (g_state.texture != nullptr)
		g_state.texture->Release();
	g_state.texture = nullptr;
	g_state.device = nullptr;
	g_state.next = 0;
}

bool allocate(ID3D12Device *device)
{
	D3D12_HEAP_PROPERTIES def = {};
	def.Type = D3D12_HEAP_TYPE_DEFAULT;

	D3D12_RESOURCE_DESC tex = {};
	tex.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	tex.Width = 1;
	tex.Height = 1;
	tex.DepthOrArraySize = 1;
	tex.MipLevels = 1;
	// Single channel on purpose. The guide (§3.9) says only the first channel is sampled and
	// names R16F as preferred; R32_FLOAT is the same shape at the precision the engine's own
	// eye-adaptation buffer already carries, and it removes any question about which channel.
	tex.Format = DXGI_FORMAT_R32_FLOAT;
	tex.SampleDesc.Count = 1;
	tex.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	tex.Flags = D3D12_RESOURCE_FLAG_NONE;

	// Created directly in the state NGX needs, so the very first frame's transition pair is as
	// well-defined as every later one. Nothing here is ever guessed: this resource is only ever
	// in NON_PIXEL_SHADER_RESOURCE or COPY_DEST, and we are the only writer of both.
	HRESULT hr = device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &tex,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
		IID_PPV_ARGS(&g_state.texture));
	if (FAILED(hr) || g_state.texture == nullptr)
	{
		STRAY_LOG_ERROR("exposure_texture: CreateCommittedResource(1x1 R32_FLOAT) failed "
			"hr=0x%08x — NgxExposure=owned cannot supply a texture; frames will be marked "
			"degraded rather than given a substitute.", static_cast<unsigned int>(hr));
		return false;
	}

	D3D12_HEAP_PROPERTIES up = {};
	up.Type = D3D12_HEAP_TYPE_UPLOAD;
	D3D12_RESOURCE_DESC buf = {};
	buf.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	buf.Width = kUploadBytes;
	buf.Height = 1;
	buf.DepthOrArraySize = 1;
	buf.MipLevels = 1;
	buf.SampleDesc.Count = 1;
	buf.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

	for (unsigned int i = 0; i < kRing; ++i)
	{
		hr = device->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &buf,
			D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&g_state.upload[i]));
		if (FAILED(hr) || g_state.upload[i] == nullptr)
		{
			STRAY_LOG_ERROR("exposure_texture: upload buffer %u alloc failed hr=0x%08x", i,
				static_cast<unsigned int>(hr));
			return false;
		}
		// Persistently mapped. An UPLOAD buffer is CPU-write-only, so the read range is empty.
		D3D12_RANGE no_read = { 0, 0 };
		if (FAILED(g_state.upload[i]->Map(0, &no_read, &g_state.mapped[i])) ||
			g_state.mapped[i] == nullptr)
		{
			STRAY_LOG_ERROR("exposure_texture: upload buffer %u Map failed", i);
			return false;
		}
		std::memset(g_state.mapped[i], 0, kUploadBytes);
	}

	g_state.device = device;
	STRAY_LOG_INFO("exposure_texture: 1x1 R32_FLOAT created with a %u-deep upload ring "
		"(NgxExposure=owned). State is NON_PIXEL_SHADER_RESOURCE at rest, per DLSS "
		"Programming Guide 310.6.0 §3.4.", kRing);
	return true;
}

} // namespace

ID3D12Resource *update(ID3D12Device *device, ID3D12GraphicsCommandList *cmd, float value)
{
	if (device == nullptr || cmd == nullptr)
		return nullptr;
	if (g_state.alloc_failed)
		return nullptr;

	// A device change means a new session's resources; drop the old ones rather than recording
	// a copy onto a list that belongs to a different device.
	if (g_state.device != nullptr && g_state.device != device)
		free_all();

	if (g_state.texture == nullptr)
	{
		if (!allocate(device))
		{
			free_all();
			g_state.alloc_failed = true;
			return nullptr;
		}
	}

	const unsigned int slot = g_state.next;
	g_state.next = (g_state.next + 1) % kRing;
	std::memcpy(g_state.mapped[slot], &value, sizeof(float));
	g_state.last = value;

	D3D12_RESOURCE_BARRIER barrier = {};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Transition.pResource = g_state.texture;
	barrier.Transition.Subresource = 0;
	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
	cmd->ResourceBarrier(1, &barrier);

	D3D12_TEXTURE_COPY_LOCATION src = {};
	src.pResource = g_state.upload[slot];
	src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	src.PlacedFootprint.Offset = 0;
	src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R32_FLOAT;
	src.PlacedFootprint.Footprint.Width = 1;
	src.PlacedFootprint.Footprint.Height = 1;
	src.PlacedFootprint.Footprint.Depth = 1;
	src.PlacedFootprint.Footprint.RowPitch = kUploadBytes;

	D3D12_TEXTURE_COPY_LOCATION dst = {};
	dst.pResource = g_state.texture;
	dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	dst.SubresourceIndex = 0;
	cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
	cmd->ResourceBarrier(1, &barrier);

	return g_state.texture;
}

float last_value() { return g_state.last; }

void release()
{
	free_all();
	g_state.alloc_failed = false;
	g_state.last = std::numeric_limits<float>::quiet_NaN();
}

} // namespace stray_dlss::exposure_texture
