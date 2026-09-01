#include "core/heap_math.hpp"

namespace stray_dlss::core {

bool gpu_to_cpu(std::uint64_t gpu, const HeapSpan &heap, std::uint64_t &cpu)
{
	if (heap.increment == 0 || heap.count == 0 || heap.gpu_base == 0 || gpu < heap.gpu_base)
		return false;
	const std::uint64_t delta = gpu - heap.gpu_base;
	if (delta % heap.increment != 0)
		return false;
	const std::uint64_t index = delta / heap.increment;
	if (index >= heap.count)
		return false;
	cpu = heap.cpu_base + index * heap.increment;
	return true;
}

bool cpu_to_index(std::uint64_t cpu, const HeapSpan &heap, std::uint32_t &index)
{
	if (heap.increment == 0 || heap.count == 0 || cpu < heap.cpu_base)
		return false;
	const std::uint64_t delta = cpu - heap.cpu_base;
	if (delta % heap.increment != 0)
		return false;
	const std::uint64_t i = delta / heap.increment;
	if (i >= heap.count)
		return false;
	index = static_cast<std::uint32_t>(i);
	return true;
}

} // namespace stray_dlss::core
