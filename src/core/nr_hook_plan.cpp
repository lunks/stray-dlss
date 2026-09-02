#include "core/nr_hook_plan.hpp"

namespace stray_dlss::nrplan {

bool latch_guide_extent(GuideExtentLatch &latch, std::uint32_t width, std::uint32_t height)
{
	const bool had_previous = latch.width != 0 || latch.height != 0;
	const bool moved = latch.width != width || latch.height != height;
	latch.width = width;
	latch.height = height;
	return had_previous && moved;
}

} // namespace stray_dlss::nrplan
