// Descriptor-heap arithmetic. A shader-visible heap's CPU and GPU handle spaces are two
// affine images of one index space: cpu = cpu_base + i*inc, gpu = gpu_base + i*inc (HARD,
// D3D12 semantics of GetCPUDescriptorHandleForHeapStart / GetGPUDescriptorHandleForHeapStart /
// GetDescriptorHandleIncrementSize). Pure.
#pragma once

#include <cstdint>

namespace stray_dlss::core {

struct HeapSpan
{
	std::uint64_t cpu_base = 0;
	std::uint64_t gpu_base = 0;
	std::uint32_t increment = 0;
	std::uint32_t count = 0;
};

// GPU handle -> CPU handle within one heap. False if `gpu` is outside the heap, not aligned
// to the increment, or the span is degenerate.
bool gpu_to_cpu(std::uint64_t gpu, const HeapSpan &heap, std::uint64_t &cpu);

// CPU handle -> descriptor index within one heap, same rules.
bool cpu_to_index(std::uint64_t cpu, const HeapSpan &heap, std::uint32_t &index);

} // namespace stray_dlss::core
