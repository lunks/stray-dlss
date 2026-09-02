// The interception seam's plain data. Buildable on Linux: no d3d12.h, no ReShade, no UE4SS.
//
// Two backends produce these — the ReShade add-on (src/backend_reshade/) and the native
// vtable-hook layer (src/backend_native/) — and one framework-free application consumes them
// (src/app/). Identity types are integers on purpose: a ResourceId is never dereferenced by a
// consumer, and a DescriptorId is always a REAL D3D12_CPU_DESCRIPTOR_HANDLE.ptr (ReShade
// un-mangles before reporting, docs/RESEARCH.md §2.4; the native backend never mints one).
#pragma once

#include "core/taa_signature.hpp"

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

// Global scope on purpose: writing `struct ID3D12DescriptorHeap *` inside the namespace below
// declares a NEW type stray_dlss::icept::ID3D12DescriptorHeap rather than referring to the
// real one. These match d3d12.h's own forward declarations, so including it before or after
// this header is fine either way.
struct ID3D12Device;
struct ID3D12GraphicsCommandList;
struct ID3D12CommandQueue;
struct ID3D12Resource;
struct ID3D12DescriptorHeap;
struct IDXGISwapChain3;

namespace stray_dlss::icept {

using ResourceId = std::uint64_t;   // ID3D12Resource* as an integer. IDENTITY ONLY.
using DescriptorId = std::uint64_t; // D3D12_CPU_DESCRIPTOR_HANDLE::ptr, always a real one.

constexpr std::uint64_t kUnknownSize = ~0ull;

struct BufferRange
{
	ResourceId buffer = 0;
	std::uint64_t offset = 0;
	std::uint64_t size = kUnknownSize; // kUnknownSize = "to the end of the buffer"
};

// What a backend knows about a resource, captured WITHOUT dereferencing it at query time in
// the native backend (creation-time snapshot) and liveness-checked first in the ReShade one.
struct ResourceInfo
{
	bool is_buffer = false;
	std::uint64_t buffer_size = 0;
	std::uint32_t width = 0;
	std::uint32_t height = 0;
	std::uint32_t mip_levels = 0;
	std::uint32_t array_size = 0;
	std::uint32_t sample_count = 0;
	TexFormat format = TexFormat::unknown;
	std::uint32_t dxgi_format = 0; // the raw DXGI_FORMAT, for logging
	bool is_3d = false;
	bool upload_heap = false; // Map() is legal for a CPU read
	bool allow_uav = false;
};

// What a single compute dispatch had bound. Moved verbatim from the old frame_state.hpp.
struct DispatchBindings
{
	std::vector<BoundTexture> srvs;
	std::vector<BoundTexture> uavs;
	// EVERY constant buffer bound, not just the first. Which register carries UE4's View
	// uniform buffer varies between passes (b3, b4 and b5 all seen), so the only reliable
	// way to find it is to try each and keep the one that parses plausibly.
	std::vector<std::pair<std::uint32_t, BufferRange>> constant_buffers;
	BufferRange view_cb{};
	bool view_cb_valid = false;
	std::uint32_t view_cb_register = 0;

	// Table slots the backend could NOT resolve, with why — the native backend fills this so
	// a disagreement can be adjudicated slot by slot (the ReShade backend leaves it empty).
	struct Unresolved
	{
		char kind = 't';           // 't' srv, 'u' uav, 'b' cbv
		std::uint32_t reg = 0;
		DescriptorId descriptor = 0; // the ONLINE slot's CPU handle (0 if the heap span was unknown)
		// native: 0 no heap span, 1 no entry (never written since attach), 2 dead (resource
		// died after the write). ReShade: 3 the view maps to no resource, 4 the resource is
		// dead per ReShade's own liveness (dropped in silence before this field existed).
		std::uint8_t reason = 0;
		ResourceId dead_resource = 0; // reasons 2 and 4: the resource that died
	};
	std::vector<Unresolved> unresolved;

	// The descriptor heaps that owned THIS dispatch's descriptors — i.e. exactly what the game
	// had bound at this moment. Restoring these is precise, where picking the first heap found
	// across every tracked table is a guess that can restore the wrong one.
	::ID3D12DescriptorHeap *heaps[2] = {};
	unsigned int heap_count = 0;
};

// One per GAME command list. Consumers key their per-list state on `native`, which is the
// same object under either backend; `backend_cookie` is the backend's own handle for the
// list (reshade::api::command_list* today) and is opaque to everyone else.
struct CommandContext
{
	::ID3D12GraphicsCommandList *native = nullptr;
	::ID3D12Device *device = nullptr; // the ORIGINAL vkd3d device, both backends
	std::uint64_t backend_cookie = 0;
};

struct PresentContext
{
	::ID3D12CommandQueue *queue = nullptr;
	::IDXGISwapChain3 *swapchain = nullptr;
	::ID3D12Resource *back_buffer = nullptr;
	std::uint64_t frame = 0;
	// A command list that executes on `queue` AFTER every list the game submitted this frame
	// and before the frame is presented. ReShade's immediate list today; ours in Stage 4.
	// May be null when the backend has none.
	::ID3D12GraphicsCommandList *present_list = nullptr;
	std::uint64_t backend_cookie = 0;
};

// Which pipeline stage a hashed shader belongs to. bind_pipeline fires with "all" in D3D12,
// so the kind has to travel with the pipeline itself.
enum class ShaderKind
{
	compute,
	pixel,
};

} // namespace stray_dlss::icept
