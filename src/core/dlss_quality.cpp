#include "core/dlss_quality.hpp"

#include <cmath>

namespace stray_dlss {

double dlss_quality_ratio(DlssQuality q)
{
	switch (q)
	{
	case DlssQuality::dlaa: return 1.0;
	case DlssQuality::max_quality: return 1.5;
	case DlssQuality::balanced: return 1.7204650534;  // NVIDIA's documented 58.0% linear
	case DlssQuality::max_performance: return 2.0;
	case DlssQuality::ultra_performance: return 3.0;
	}
	return 1.0;
}

const char *dlss_quality_name(DlssQuality q)
{
	switch (q)
	{
	case DlssQuality::dlaa: return "DLAA";
	case DlssQuality::max_quality: return "Quality";
	case DlssQuality::balanced: return "Balanced";
	case DlssQuality::max_performance: return "Performance";
	case DlssQuality::ultra_performance: return "UltraPerformance";
	}
	return "?";
}

DlssQuality dlss_quality_for(std::uint32_t render_width, std::uint32_t render_height,
                             std::uint32_t output_width, std::uint32_t output_height)
{
	if (render_width == 0 || render_height == 0)
		return DlssQuality::dlaa;
	// Exact 1:1 is DLAA, which is its own NGX mode rather than the bottom of the quality scale.
	if (render_width == output_width && render_height == output_height)
		return DlssQuality::dlaa;

	// Use the larger axis ratio. They agree for every ratio this project runs, and taking the
	// larger errs toward declaring MORE upscaling, which is the safer direction: DLSS given a
	// lower sample expectation than it receives degrades gracefully, the reverse does not.
	const double rx = static_cast<double>(output_width) / static_cast<double>(render_width);
	const double ry = static_cast<double>(output_height) / static_cast<double>(render_height);
	const double ratio = rx > ry ? rx : ry;

	const DlssQuality modes[] = { DlssQuality::max_quality, DlssQuality::balanced,
		DlssQuality::max_performance, DlssQuality::ultra_performance };
	DlssQuality best = DlssQuality::max_quality;
	double best_err = -1.0;
	for (const auto m : modes)
	{
		const double err = std::fabs(dlss_quality_ratio(m) - ratio);
		if (best_err < 0.0 || err < best_err)
		{
			best_err = err;
			best = m;
		}
	}
	return best;
}

} // namespace stray_dlss
