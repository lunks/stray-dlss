// NR at MODEL RESOLUTION - the pure half.
//
// Feature 18 cannot upscale (docs/RESEARCH-DLSSNR-PARAM-AUDIT.md: ScalingRatio is clobbered to
// 1.0 at both call sites), and it costs what it costs per pixel it is shown. So the lever on its
// cost is not where it runs but HOW BIG a picture it is shown: run the network on an exact
// area-average of the 4K frame at a fraction of its size, and lay only the network's DIFFERENCE
// back on the full-resolution frame. The frame's own detail never goes through the model; what
// comes up from small is the edit. OptiScaler_DLSSNR ships this as "Model Resolution" with the
// "matched residual" transfer (hhkbble/Dagherbou, 2026-09), and it is the one lever their users
// found against the 50-60% cost NVIDIA quotes. At scale 0.5 on a 3840x2160 back buffer the
// model sees 1920x1080 - exactly the extent our depth and motion-vector guides already have, so
// the guide ratio is 1.0 with nothing resampled but colour.
//
// Everything decided here is decided from numbers so it can be tested without a GPU: the small
// extent, whether the path is on at all, and why it is off when it is off.
#pragma once

#include <cstdint>

namespace stray_dlss::nrmodel_plan {

struct Config
{
	// [STRAYDLSS] NgxNRModelScale. 1.0 (or anything >= kOffAbove) = the model sees the full
	// frame, i.e. today's present stage exactly. 0.5 = the model sees a quarter of the pixels.
	// Default 1.0: the user judged 0.5 worse on image quality (2026-09-05) and the cost side was
	// never validly measured. The path stays as an instrument, off unless asked for.
	float scale = 1.0f;
	// [STRAYDLSS] NgxNRModelTransfer: how much of the model's difference lands. 0 = the frame
	// comes back untouched (the honest A/B), 1 = the full edit, >1 exaggerates.
	float transfer_strength = 1.0f;
	// [STRAYDLSS] NgxNRModelGuided: how the edit comes up from the small extent
	// (shaders/nr_resolve.hlsl). 0 bilinear, 1 joint bilateral (Kopf 2007), 2 local affine
	// (guided filter / Bilateral Guided Upsampling). Anything else is clamped to 2.
	int guided = 2;
	// Mode 1: luminance range sigma in display units. Mode 2: the guided filter's epsilon,
	// which regularises the fitted slope where the small patch is flat.
	float guided_sigma = 0.1f;
	float guided_epsilon = 0.01f;
};

enum class Result
{
	off,            // scale says full-resolution: the stage behaves as before, nothing recorded
	ok,             // downsample -> model at the small extent -> matched residual up
	bad_scale,      // scale below kMinScale: refused, the full-resolution path runs instead
	no_typed_uav,   // the resolve writes the full frame as a typed UAV and this format cannot
	too_small,      // the small extent would be under kMinExtent on a side
};
constexpr int kResultCount = 5;

struct Plan
{
	Result result = Result::off;
	std::uint32_t width = 0;   // the model's extent when ok
	std::uint32_t height = 0;
	float transfer_strength = 1.0f;
	int guided = 2;              // the resolve mode, clamped to [0, 2]
	float guided_param = 0.01f;  // sigma for mode 1, epsilon for mode 2, unused for 0
};

constexpr float kOffAbove = 0.999f;    // >= this is "full resolution", not a downsample
constexpr float kMinScale = 0.25f;     // below this the model has nothing left to see
constexpr std::uint32_t kMinExtent = 64;

inline const char *result_name(Result r)
{
	switch (r)
	{
	case Result::off:          return "off";
	case Result::ok:           return "ok";
	case Result::bad_scale:    return "bad-scale";
	case Result::no_typed_uav: return "no-typed-uav";
	case Result::too_small:    return "too-small";
	}
	return "?";
}

// Round half-up to an even pixel count: the downsample integrates exact footprints so any
// extent is correct, but an even one keeps a 2:1 case bit-exact (3840 * 0.5 = 1920).
inline std::uint32_t scaled_extent(std::uint32_t full, float scale)
{
	if (full == 0 || scale <= 0.0f)
		return 0;
	const float f = static_cast<float>(full) * scale;
	std::uint32_t v = static_cast<std::uint32_t>(f + 0.5f);
	v &= ~1u;
	return v;
}

// `typed_uav_load` / `typed_uav_store` describe the FULL frame's format (the staging copy of
// the back buffer): the resolve reads and writes it in place as a typed UAV, so both must hold.
inline Plan plan(const Config &cfg, std::uint32_t full_width, std::uint32_t full_height,
	bool typed_uav_load, bool typed_uav_store)
{
	Plan p;
	p.transfer_strength = cfg.transfer_strength < 0.0f ? 0.0f : cfg.transfer_strength;
	p.guided = cfg.guided < 0 ? 0 : (cfg.guided > 2 ? 2 : cfg.guided);
	p.guided_param = p.guided == 1
		? (cfg.guided_sigma > 1e-4f ? cfg.guided_sigma : 1e-4f)
		: (cfg.guided_epsilon > 1e-6f ? cfg.guided_epsilon : 1e-6f);
	if (!(cfg.scale < kOffAbove))
	{
		p.result = Result::off;
		return p;
	}
	if (cfg.scale < kMinScale)
	{
		p.result = Result::bad_scale;
		return p;
	}
	if (!typed_uav_load || !typed_uav_store)
	{
		p.result = Result::no_typed_uav;
		return p;
	}
	p.width = scaled_extent(full_width, cfg.scale);
	p.height = scaled_extent(full_height, cfg.scale);
	if (p.width < kMinExtent || p.height < kMinExtent)
	{
		p.result = Result::too_small;
		return p;
	}
	p.result = Result::ok;
	return p;
}

} // namespace stray_dlss::nrmodel_plan
