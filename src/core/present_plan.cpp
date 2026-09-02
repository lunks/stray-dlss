#include "core/present_plan.hpp"

namespace stray_dlss::core {

int pick_present_queue(const std::vector<QueueRecord> &queues, std::uint64_t swapchain_device_arg)
{
	for (std::size_t i = 0; i < queues.size(); ++i)
		if (swapchain_device_arg != 0 && queues[i].id == swapchain_device_arg)
			return static_cast<int>(i);
	for (std::size_t i = 0; i < queues.size(); ++i)
		if (queues[i].type == 0)
			return static_cast<int>(i);
	return -1;
}

} // namespace stray_dlss::core
