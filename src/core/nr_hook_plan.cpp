#include "core/nr_hook_plan.hpp"

#include "core/nr_codec.hpp"

#include <cmath>

namespace stray_dlss::nrplan {

bool latch_guide_extent(GuideExtentLatch &latch, std::uint32_t width, std::uint32_t height)
{
	const bool had_previous = latch.width != 0 || latch.height != 0;
	const bool moved = latch.width != width || latch.height != height;
	latch.width = width;
	latch.height = height;
	return had_previous && moved;
}

CodecGate codec_gate(const CodecGateInputs &in)
{
	// Ordered most-fundamental first, so the reason a frame is refused names the thing that is
	// actually wrong rather than a consequence of it.
	if (!in.codec_site)
		return CodecGate::no_codec;
	if (in.track_exposure && !in.exposure_known)
		return CodecGate::exposure_unknown;
	// A scale sitting ON a clamp is degenerate, not merely extreme: nrc::proxy_scale only lands
	// there by saturating, and the resulting proxy carries no signal.
	if (!std::isfinite(in.scale) || in.scale <= nrc::kScaleMin || in.scale >= nrc::kScaleMax)
		return CodecGate::degenerate_scale;
	if (!in.encode_recorded)
		return CodecGate::encode_failed;
	return CodecGate::evaluate;
}

void note_evaluate_gap(EvaluateGapLatch &latch) { latch.reset_pending = true; }

bool take_evaluate_reset(EvaluateGapLatch &latch)
{
	const bool pending = latch.reset_pending;
	latch.reset_pending = false;
	return pending;
}

} // namespace stray_dlss::nrplan
