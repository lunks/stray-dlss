#pragma once

#include <cstdint>

namespace stray_dlss {

// Which DLSS quality mode to DECLARE to NGX at CreateFeature.
//
// This is not a preference: NGX picks its internal reconstruction parameters from
// InPerfQualityValue, so declaring a mode whose ratio does not match the rect we actually hand
// it tells DLSS to expect a different number of input samples than it gets. We hardcoded
// MaxQuality at every ratio, which at 50% screen percentage (a true 2.0x, i.e. Performance)
// was two modes off — while at 70% (1.43x) it was very nearly right. That matches the measured
// observation that 70% looks sharper than 50%. CLAUDE.md §5 already required deriving this.
enum class DlssQuality
{
	dlaa,              // 1.0x
	max_quality,       // 1.5x
	balanced,          // ~1.72x
	max_performance,   // 2.0x
	ultra_performance, // 3.0x
};

// The upscale ratios NVIDIA documents for each mode. Nearest-ratio selection rather than a
// table of exact rects, because the engine quantises its render rect to 8-pixel tiles and the
// resulting ratio is never exactly the nominal one.
double dlss_quality_ratio(DlssQuality q);

// Nearest mode for an actual render->output ratio. An exact 1:1 is DLAA and nothing else:
// DLAA is a distinct code path in NGX, not merely the 1.0x end of the quality scale.
DlssQuality dlss_quality_for(std::uint32_t render_width, std::uint32_t render_height,
                             std::uint32_t output_width, std::uint32_t output_height);

const char *dlss_quality_name(DlssQuality q);

} // namespace stray_dlss
