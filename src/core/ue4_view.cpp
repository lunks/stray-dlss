#include "ue4_view.hpp"

#include <algorithm>
#include <cmath>

namespace stray_dlss::ue4 {

int jitter_phase_count(std::uint32_t render_height, std::uint32_t output_height) noexcept
{
	if (render_height == 0)
		return kBaseJitterPhases;

	const float ratio = std::max(1.0f, static_cast<float>(output_height) / static_cast<float>(render_height));
	return static_cast<int>(std::lround(static_cast<float>(kBaseJitterPhases) * ratio * ratio));
}

float mip_lod_bias(std::uint32_t render_height, std::uint32_t output_height) noexcept
{
	if (render_height == 0)
		return 0.0f;

	const float render = static_cast<float>(render_height);
	const float output = static_cast<float>(output_height);
	return std::log2(render / std::max(render, output)) - 1.0f;
}

float halton(unsigned int index, unsigned int base) noexcept
{
	// +1 so index 0 does not degenerate to 0 and skew the sequence toward the origin.
	index += 1;

	float result = 0.0f;
	const float inv_base = 1.0f / static_cast<float>(base);
	float fraction = inv_base;
	while (index > 0)
	{
		result += static_cast<float>(index % base) * fraction;
		index /= base;
		fraction *= inv_base;
	}
	return result - 0.5f;
}

} // namespace stray_dlss::ue4
