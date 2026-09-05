// ---- the HUD-less copy's D3D12 half, on WARP ----
//
// Included by warp_mv_resolve.cpp after its helpers (Gpu, check, HR, drain_validation, flush)
// and after warp_nr_stage.inc (create_fake_back_buffer, upload_rgba8).
//
// WHAT THIS PROVES. src/hudless.cpp is one committed ring and one CopyResource wrapped in four
// barriers, recorded on the GAME'S list mid-frame with the back buffer in the state the engine
// holds it in (RENDER_TARGET at Slate's pass). The debug layer with GPU-based validation judges
// the barrier sequence - a StateBefore that does not match reality is exactly the class of
// error vkd3d-proton cannot report (CLAUDE.md §5) - and the pixels are read back so "it
// recorded" and "it copied the right bytes" are separate claims. The publication sequence and
// the retire-behind-the-ring rule are checked too, since a consumer that reads a stale copy or
// a ring freed under an in-flight list are the two ways this can go wrong quietly.
//
// WHAT IT CANNOT PROVE: that the engine's Slate pass really has the back buffer in
// RENDER_TARGET, or that the copy lands before the UI's first draw. Those are the box's
// (`markers bad` and the dump).

#include "hudless.hpp"

#include <d3d12.h>

namespace {

bool read_rgba8_texel(Gpu &gpu, ID3D12Resource *tex, UINT w, UINT h, UINT x, UINT y, std::uint32_t &out)
{
	D3D12_RESOURCE_DESC bd = {};
	bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	const UINT row_pitch = (w * 4 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
	bd.Width = static_cast<UINT64>(row_pitch) * h;
	bd.Height = 1;
	bd.DepthOrArraySize = 1;
	bd.MipLevels = 1;
	bd.SampleDesc.Count = 1;
	bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	D3D12_HEAP_PROPERTIES rb = {};
	rb.Type = D3D12_HEAP_TYPE_READBACK;
	ComPtr<ID3D12Resource> readback;
	HR(gpu.device->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)));

	D3D12_RESOURCE_BARRIER b = {};
	b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	b.Transition.pResource = tex;
	b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
	b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
	gpu.list->ResourceBarrier(1, &b);
	D3D12_TEXTURE_COPY_LOCATION dst = {};
	dst.pResource = readback.Get();
	dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	dst.PlacedFootprint.Footprint.Width = w;
	dst.PlacedFootprint.Footprint.Height = h;
	dst.PlacedFootprint.Footprint.Depth = 1;
	dst.PlacedFootprint.Footprint.RowPitch = row_pitch;
	D3D12_TEXTURE_COPY_LOCATION src = {};
	src.pResource = tex;
	src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	gpu.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
	std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
	gpu.list->ResourceBarrier(1, &b);
	if (!flush(gpu))
		return false;
	void *mapped = nullptr;
	const D3D12_RANGE all = { 0, static_cast<SIZE_T>(bd.Width) };
	HR(readback->Map(0, &all, &mapped));
	out = *reinterpret_cast<const std::uint32_t *>(static_cast<const std::uint8_t *>(mapped) + static_cast<std::size_t>(y) * row_pitch + static_cast<std::size_t>(x) * 4);
	const D3D12_RANGE none = { 0, 0 };
	readback->Unmap(0, &none);
	return true;
}

} // namespace

bool test_hudless_copy(Gpu &gpu)
{
	std::printf("\n[hudless] copy on the game's list with the back buffer in RENDER_TARGET, judged by the debug layer\n");
	constexpr UINT w = 64, h = 32;
	ComPtr<ID3D12Resource> bb;
	// ALLOW_RENDER_TARGET: the stand-in is put into RENDER_TARGET below, which the debug layer
	// permits only on a resource created with that flag - as every real swapchain buffer is.
	if (!create_fake_back_buffer(gpu, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, bb,
			D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET))
		return false;
	std::vector<std::uint32_t> texels(static_cast<std::size_t>(w) * h);
	for (UINT y = 0; y < h; ++y)
		for (UINT x = 0; x < w; ++x)
			texels[static_cast<std::size_t>(y) * w + x] = 0xFF000000u | (x << 16) | (y << 8) | 0x40u;
	if (!upload_rgba8(gpu, bb.Get(), w, h, texels))
		return false;

	// The engine holds the back buffer in RENDER_TARGET at the HUD pass; put the stand-in there
	// the way the engine would, so the copy's own StateBefore is judged against a true state.
	D3D12_RESOURCE_BARRIER to_rt = {};
	to_rt.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	to_rt.Transition.pResource = bb.Get();
	to_rt.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	to_rt.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
	to_rt.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
	gpu.list->ResourceBarrier(1, &to_rt);

	const D3D12_RESOURCE_DESC like = bb->GetDesc();
	check(stray_dlss::hudless::ensure(gpu.device.Get(), like, 1), "the ring allocates in the back buffer's format and extent");
	check(stray_dlss::hudless::current().sequence == 0, "no copy has been published yet");
	ID3D12Resource *copy = stray_dlss::hudless::record_copy(gpu.list.Get(), bb.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, 1);
	check(copy != nullptr, "the copy records");
	const stray_dlss::hudless::Current c1 = stray_dlss::hudless::current();
	check(c1.texture == copy && c1.sequence == 1 && c1.width == w && c1.height == h, "the copy is published with sequence 1 and the back buffer's extent");

	// Back to COMMON the way the engine's present would, then execute: every barrier above is
	// now under the debug layer's judgement.
	std::swap(to_rt.Transition.StateBefore, to_rt.Transition.StateAfter);
	gpu.list->ResourceBarrier(1, &to_rt);
	if (!flush(gpu))
		return false;
	drain_validation(gpu, "hudless copy");

	std::uint32_t got = 0;
	if (!read_rgba8_texel(gpu, copy, w, h, 17, 5, got))
		return false;
	check(got == texels[5 * w + 17], "the copy holds the back buffer's bytes");

	// A second copy goes to the other ring slot and advances the sequence; the first is untouched.
	gpu.list->ResourceBarrier(1, &to_rt); // COMMON -> RT? no: to_rt is now RT->COMMON; flip back
	std::swap(to_rt.Transition.StateBefore, to_rt.Transition.StateAfter);
	gpu.list->ResourceBarrier(1, &to_rt);
	ID3D12Resource *copy2 = stray_dlss::hudless::record_copy(gpu.list.Get(), bb.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, 2);
	check(copy2 != nullptr && copy2 != copy, "the second copy lands in the other ring slot");
	check(stray_dlss::hudless::current().sequence == 2, "the sequence advances per copy");
	std::swap(to_rt.Transition.StateBefore, to_rt.Transition.StateAfter);
	gpu.list->ResourceBarrier(1, &to_rt);
	if (!flush(gpu))
		return false;
	drain_validation(gpu, "hudless second copy");

	// A resize retires the ring behind the present ring rather than freeing it now.
	D3D12_RESOURCE_DESC bigger = like;
	bigger.Width = w * 2;
	check(stray_dlss::hudless::ensure(gpu.device.Get(), bigger, 3), "a new extent rebuilds the ring");
	check(stray_dlss::hudless::stats().retired == 1 && stray_dlss::hudless::stats().released == 0, "the old ring is retired, not released");
	check(stray_dlss::hudless::current().sequence == 0, "a rebuilt ring publishes nothing until a copy");
	stray_dlss::hudless::collect(3);
	check(stray_dlss::hudless::stats().released == 0, "not released inside the present ring's window");
	stray_dlss::hudless::collect(3 + static_cast<std::uint64_t>(stray_dlss::ring::kFrameCount) * 2);
	check(stray_dlss::hudless::stats().released == 1, "released once the ring says the GPU is done");
	stray_dlss::hudless::shutdown();
	return true;
}
