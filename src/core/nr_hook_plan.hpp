// NR's OWN temporal history, and the guide grid it was accumulated against. Pure; tested.
//
// This header once also carried the three-site choice for DLSS Neural Rendering (`taa` /
// `present` / `preui`). The two post-tonemap sites were REMOVED on 2026-09-02: neither ever
// produced a correct frame on the box (`preui` wrecked one, `present` depends on a ReShade
// event that never fires with an empty preset), NR runs at exactly one site — inside the
// intercepted TAA dispatch — and that site is what every measured session used.
#pragma once

#include <cstdint>

namespace stray_dlss::nrplan {

// Feature 18 is NOT a per-frame spatial filter. It keeps an internal accumulator: it consumes
// motion vectors and depth (which a spatial-only network would not need) and exposes
// `DLSSNR.Reset`, named `settings.resetAccumulation` in the reference deployment.
//
// The consequence, verbatim from that deployment
// (github.com/lunks/dxvk-remix-plus-dlssnr @ fc4de144,
//  src/dxvk/rtx_render/rtx_neural_rendering.cpp:220-230):
//
//   "The NGX feature is keyed on the colour grid alone, so switching DLSS quality at a fixed
//    output resolution moves the guide grid --- and DLSSNR.MVecScaleX/Y with it --- underneath a
//    temporal history that was accumulated against the old one. Nothing else notices, so latch
//    the guide extent here and force a single reset frame when it moves."
//
// This is a LIVE bug for us and not a hypothetical: the feature is created for the OUTPUT rect,
// which does not change when the screen percentage does, so nothing else in our code notices that
// 1920x1080 guides became 2688x1512 guides and that MVecScale went from 2.0 to 1.42857 under an
// accumulated history.
struct GuideExtentLatch
{
	std::uint32_t width = 0;
	std::uint32_t height = 0;
};

// Records this frame's guide extent and answers whether NR's accumulation must be reset.
//
// Returns FALSE on the first observation, exactly as the reference does (`resetGuideHistory =
// (latched != 0)`): there is no history to invalidate before the first evaluate, and treating the
// first frame as a change would put a spurious reset into every session.
bool latch_guide_extent(GuideExtentLatch &latch, std::uint32_t width, std::uint32_t height);

// ---------------------------------------------------------------------------------------
// NO CODEC, NO EVALUATE.
//
// Feature 18 is a DISPLAY-REFERRED image network. Our hook point — the intercepted TAA dispatch
// — carries raw, unbounded, PRE-EXPOSED LINEAR HDR, which is not in its domain, and the HDR
// codec's soft clip and exact sRGB encode are precisely what put it there. The proxy is not a
// tuning stage that can be skipped; it IS the input contract. Measured with it missing: a neural
// output whose max luminance read 0.0026 and red noise on the screen.
//
// A sibling port of this integration reached the same rule from the other direction. It moved
// its pass to after the runtime's sRGB encode because feeding the network a linear image made
// the runtime apply a second gamma curve on top — "lifted blacks and washed out greys in dark
// scenes, exactly as reported" — and it declines the frames where that encode is suppressed
// "rather than evaluating on an input domain the model was not trained on"
// (RemixProjGroup/dxvk-remix, branch dlss-nr @ 2df9c812). Two ports, two hook points, one rule.
//
// So every way of arriving at EvaluateFeature without a correct proxy is enumerated here and
// answered with a refusal rather than a fall-through.
// ---------------------------------------------------------------------------------------
enum class CodecGate
{
	evaluate,
	// This call site does not run the encode/decode pair at all. The post-tonemap sites that
	// legitimately bypassed the codec were removed on 2026-09-02, so reaching this now means an
	// un-encoded image would have been handed to the network.
	no_codec,
	// The encode dispatch did not record, so the proxy holds whatever the last frame left.
	encode_failed,
	// NgxNRTrackExposure is on and the engine's exposure has never decoded, so the scale — which
	// DEFINES the display-referred units — is unknown. Substituting the static scale silently
	// moves the network's input domain, and feature 18's own temporal history was accumulated in
	// the other one.
	exposure_unknown,
	// The scale is pinned at one of nrc's clamps (or is zero/negative/NaN). The proxy is then
	// flat black or flat white: an image in the right FORMAT carrying no signal.
	degenerate_scale,
};

struct CodecGateInputs
{
	bool codec_site = true;
	bool encode_recorded = false;
	bool track_exposure = true;
	// Whether a plausible engine exposure has EVER been read this session, not whether this
	// particular frame's View CB decoded: the smoothed factor legitimately carries across a bad
	// frame, and one unreadable constant buffer is not an unknown operating point.
	bool exposure_known = false;
	float scale = 0.0f;
};

CodecGate codec_gate(const CodecGateInputs &in);

// Any frame NR declines is a hole in feature 18's own temporal continuity: it reprojects its
// accumulation with motion vectors describing one frame of motion, and a skipped frame makes
// that reprojection wrong. The next evaluate must therefore carry DLSSNR.Reset — once.
struct EvaluateGapLatch
{
	bool reset_pending = false;
};

void note_evaluate_gap(EvaluateGapLatch &latch);
bool take_evaluate_reset(EvaluateGapLatch &latch);

// THE HOLE THE REFUSAL PATH CANNOT SEE.
//
// note_evaluate_gap() is reached only from inside nr::apply(), so it covers exactly the frames NR
// was ASKED about and declined. It cannot cover the frames NR was never asked about at all, and
// those are not rare: apply() is called only when the TAA pass was intercepted AND the SR/RR
// evaluate succeeded (src/taa_hook.cpp), and NgxNR=0 -> 1 deliberately KEEPS the existing feature
// and its accumulated history across a gap of arbitrary length (nr::set_enabled). On every one of
// those frames feature 18's history stands still while the world moves, and the next evaluate
// reprojects across the whole gap with motion vectors that describe one frame.
//
// The sibling port does not need this because its pass is a fixed pipeline stage that runs every
// frame, so its sticky flag sees every frame by construction:
//
//   "The snippet's temporal history is only meaningful if the evaluation before this one produced
//    it [...] Sticky rather than a one-shot, so a run of failures keeps it armed until one
//    succeeds."   (Kim2091/dxvk-remix @ gta4-atmos-dlss5, rtx_neural_uplift.h:214-226)
//
// Ours is a hook, not a stage, so the equivalent has to be asserted at the frame boundary: one
// call per present, saying whether an evaluate actually happened in it. Idempotent with
// note_evaluate_gap — a frame that declined has already armed the latch, and arming it twice is
// still one reset.
void note_frame_boundary(EvaluateGapLatch &latch, bool evaluated_this_frame);

} // namespace stray_dlss::nrplan
