// Per-frame versioning arithmetic for resources the CPU rewrites while the GPU may still be
// reading them. Pure, so it is unit-tested on Linux — which matters because getting any of it
// wrong corrupts silently rather than failing.
//
// The hazard this exists to manage: a dispatch recorded one or two frames ago may still be
// executing. Rewriting the descriptors or constants it reads is undefined behaviour, and the
// symptom is a wrong image or a device fault, never a clean error.
#pragma once

#include <cstddef>
#include <cstdint>

namespace stray_dlss::ring {

// How many versions of each per-frame resource to keep. Three covers a typical D3D12 frame
// pipeline depth; the swapchain here is triple-buffered.
constexpr std::uint32_t kFrameCount = 3;

// Descriptors per frame slice: t0 depth, t1 velocity, u0 output.
constexpr std::uint32_t kDescriptorsPerFrame = 3;

constexpr std::uint32_t kDescriptorCount = kDescriptorsPerFrame * kFrameCount;

constexpr std::uint32_t slot_for_frame(std::uint64_t frame) noexcept
{
	return static_cast<std::uint32_t>(frame % kFrameCount);
}

// Byte offset of a frame slice's first descriptor within the heap.
constexpr std::size_t descriptor_offset(std::uint32_t slot, std::uint32_t descriptor_size) noexcept
{
	return static_cast<std::size_t>(slot) * kDescriptorsPerFrame * descriptor_size;
}

// Byte offset of a descriptor within a slice. `index` is 0 depth, 1 velocity, 2 output.
constexpr std::size_t descriptor_offset_in_slot(std::uint32_t slot,
                                                std::uint32_t index,
                                                std::uint32_t descriptor_size) noexcept
{
	return (static_cast<std::size_t>(slot) * kDescriptorsPerFrame + index) * descriptor_size;
}

// Constant buffers must be bound at 256-byte aligned offsets.
constexpr std::uint32_t kConstantAlignment = 256;

constexpr std::uint32_t aligned_constant_stride(std::size_t struct_size) noexcept
{
	return static_cast<std::uint32_t>((struct_size + kConstantAlignment - 1) &
	                                  ~static_cast<std::size_t>(kConstantAlignment - 1));
}

constexpr std::size_t constant_offset(std::uint32_t slot, std::uint32_t stride) noexcept
{
	return static_cast<std::size_t>(slot) * stride;
}

// Whether a resource retired at `retire_frame` can now be released.
//
// Deliberately conservative: two full ring cycles rather than one. Releasing a descriptor heap
// or texture that an in-flight command list still binds is a GPU fault, and the extra frames of
// VRAM cost far less than that failure — which showed up exactly at the load-to-gameplay
// transition, where the resolution changes and buffers churn.
constexpr bool is_safe_to_release(std::uint64_t current_frame, std::uint64_t retire_frame) noexcept
{
	return current_frame >= retire_frame + static_cast<std::uint64_t>(kFrameCount) * 2;
}

} // namespace stray_dlss::ring
