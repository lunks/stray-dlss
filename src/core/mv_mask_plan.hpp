// The bias-current-colour mask: when to hand DLSS SR one, and what to fill it with.
//
// WHAT IT IS. `NVSDK_NGX_D3D12_DLSS_Eval_Params::pInBiasCurrentColorMask`
// (third_party/ngx/include/nvsdk_ngx_helpers.h:391, parameter name
// "DLSS.Input.Bias.Current.Color.Mask" at nvsdk_ngx_defs.h:815) is DLSS's own per-pixel
// "do not trust the history here" channel. We have never sent one — grep for
// `BiasCurrentColor` across src/ was empty before this file.
//
// WHY IT IS THE HONEST ANSWER TO THE REFLECTION PROBLEM. CLAUDE.md §5 records that our motion
// vectors describe the SURFACE's motion while a screen-space reflection moves with the
// REFLECTED geometry, and calls the mismatch structural rather than a bug, with the available
// moves being "reduce strength, disable SSR, or accept it". The bias mask is a fourth move, and
// it is the one NVIDIA built for this: it does not claim to know where the reflection went, it
// only says the history is not to be believed here. That is exactly and only what we can
// honestly assert about those pixels.
//
// STAGE 1 IS A CONSTANT FILL, DELIBERATELY. `shaders/nr_mask.hlsl` already records the reason
// in this repository: "a mask that is SILENTLY IGNORED and a mask that is bound and doing
// nothing look identical on a screenshot", so the plumbing, format and registration must be
// proven with a value whose effect is known BEFORE any heuristic is written. A full-strength
// constant fill that changes nothing on screen is a clean negative that kills the whole idea
// for one launch's cost; a content-driven mask that changes nothing is indistinguishable from
// a content signal that found nothing.
#pragma once

#include <cstdint>

namespace stray_dlss::mvmaskplan {

// The mask's neutral value. 0 means "no bias", i.e. behave exactly as an unbound mask.
// The one working third-party integration we know of (docs/RESEARCH-DLSS5-FEEDER-OPTIONS.md
// §7) writes its failing pixels INTO the mask, so non-zero is the distrust direction.
constexpr float kNeutral = 0.0f;

enum class Refusal : int
{
	none = 0,
	disabled,    // [STRAYDLSS] MvMask=0 — the default, and byte-identical to before this existed
	not_ready,   // the texture does not exist yet (first frames, or creation failed)
	unfilled,    // never cleared, so its contents are undefined; binding it would be a lie
	extent,      // the mask is smaller than the render rect DLSS will sample
	count
};

const char *refusal_name(Refusal r) noexcept;

struct Plan
{
	bool bind = false;
	Refusal refusal = Refusal::disabled;
	// What the fill should be THIS frame. Under alternation this flips between kNeutral and
	// the configured value.
	float value = kNeutral;
	// True on the frames the alternation is holding the configured (non-neutral) value.
	bool phase_on = false;
};

// The A/B, run INSIDE one session rather than across builds.
//
// CLAUDE.md §5's own conclusion after three false starts: a moving camera makes two runs
// incomparable, so the comparison has to happen within one session with the state flipping on
// a known frame boundary and logged, which is what `DryRunAlternate` does for pass
// suppression. `period` 0 disables alternation and holds the configured value.
bool alternate_phase_on(std::uint64_t frame, std::uint32_t period) noexcept;

// `render_w/h` is the rect DLSS samples (InRenderSubrectDimensions); `mask_w/h` is what we
// allocated. The mask is bound with InBiasCurrentColorSubrectBase (0,0), so it must COVER the
// render rect — being larger is fine under the resolve's grow-only allocation policy.
Plan plan(bool enabled, bool ready, bool filled, std::uint32_t mask_w, std::uint32_t mask_h,
	std::uint32_t render_w, std::uint32_t render_h, float configured_value,
	std::uint32_t alternate_period, std::uint64_t frame) noexcept;

} // namespace stray_dlss::mvmaskplan
