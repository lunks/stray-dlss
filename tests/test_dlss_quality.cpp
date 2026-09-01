#include <doctest/doctest.h>

#include "core/dlss_quality.hpp"

using namespace stray_dlss;

// InPerfQualityValue was hardcoded to MaxQuality at every ratio. NGX selects its internal
// reconstruction parameters from it, so at 50% screen percentage (a true 2.0x) we were telling
// DLSS to expect 1.5x the samples it was actually getting — two modes off. At 70% (1.43x) the
// hardcoded value happened to be nearly right, which is consistent with the measured
// observation that 70% looks sharper than 50%.
TEST_CASE("the two ratios this project runs pick the right mode")
{
	// 50% of 4K: exactly 2.0x -> Performance, NOT the hardcoded Quality.
	CHECK(dlss_quality_for(1920, 1080, 3840, 2160) == DlssQuality::max_performance);
	// 70% of 4K: 1.4286x -> nearest is Quality at 1.5x.
	CHECK(dlss_quality_for(2688, 1512, 3840, 2160) == DlssQuality::max_quality);
}

TEST_CASE("an exact 1:1 is DLAA, which is its own NGX mode")
{
	CHECK(dlss_quality_for(3840, 2160, 3840, 2160) == DlssQuality::dlaa);
	CHECK(dlss_quality_for(2560, 1440, 2560, 1440) == DlssQuality::dlaa);
}

TEST_CASE("each nominal ratio selects its own mode")
{
	CHECK(dlss_quality_for(2560, 1440, 3840, 2160) == DlssQuality::max_quality);       // 1.5x
	CHECK(dlss_quality_for(2232, 1256, 3840, 2160) == DlssQuality::balanced);          // ~1.72x
	CHECK(dlss_quality_for(1280, 720, 3840, 2160) == DlssQuality::ultra_performance);  // 3.0x
}

TEST_CASE("a degenerate rect does not divide by zero")
{
	CHECK(dlss_quality_for(0, 0, 3840, 2160) == DlssQuality::dlaa);
}
