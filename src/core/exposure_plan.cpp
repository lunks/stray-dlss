#include "exposure_plan.hpp"

#include <cmath>
#include <cstring>

namespace stray_dlss::exposure {

namespace {

// A finite, strictly positive exposure. DLSS treats a zero/negative/NaN exposure as invalid and
// silently falls back to its own estimate, so anything we synthesise has to be inside this.
constexpr float kMinExposure = 1e-6f;
constexpr float kMaxExposure = 1e6f;

bool finite_positive(float v)
{
	return std::isfinite(v) && v > 0.0f;
}

float clamp_exposure(float v)
{
	if (!std::isfinite(v))
		return 1.0f;
	if (v < kMinExposure)
		return kMinExposure;
	if (v > kMaxExposure)
		return kMaxExposure;
	return v;
}

} // namespace

Mode parse_mode(const char *value)
{
	if (value == nullptr)
		return Mode::automatic;
	if (std::strcmp(value, "texture") == 0)
		return Mode::engine_texture;
	if (std::strcmp(value, "owned") == 0)
		return Mode::owned_texture;
	return Mode::automatic;
}

const char *mode_name(Mode mode)
{
	switch (mode)
	{
	case Mode::engine_texture: return "texture";
	case Mode::owned_texture: return "owned";
	case Mode::automatic: break;
	}
	return "auto";
}

bool wants_texture(Mode mode)
{
	return mode == Mode::engine_texture || mode == Mode::owned_texture;
}

unsigned int create_flag_bits(Mode mode)
{
	// The flag and the texture are mutually exclusive by construction in the official plugin:
	// `bUseAutoExposure ? Flags_AutoExposure : 0` at create (NGXRHI.cpp:550-565) with the
	// texture nulled only under auto (NGXD3D12RHI.cpp:275-276). Reproduce that exactly — a
	// build that sets both would be asking DLSS a question with two answers.
	return wants_texture(mode) ? 0u : kAutoExposureFlag;
}

float effective_pre_exposure(float in_pre_exposure)
{
	return in_pre_exposure == 0.0f ? 1.0f : in_pre_exposure;
}

float effective_exposure_scale(float in_exposure_scale)
{
	return in_exposure_scale == 0.0f ? 1.0f : in_exposure_scale;
}

float sr_pre_exposure(float view_pre_exposure, bool pre_exposure_ok)
{
	if (!pre_exposure_ok || !finite_positive(view_pre_exposure))
		return 1.0f;
	return clamp_exposure(view_pre_exposure);
}

float owned_texel_value(float view_pre_exposure, bool pre_exposure_ok, float multiplier)
{
	const float base = sr_pre_exposure(view_pre_exposure, pre_exposure_ok);
	const float m = finite_positive(multiplier) ? multiplier : 1.0f;
	return clamp_exposure(base * m);
}

int resolved_preset(int preset_hint, DlssQuality quality)
{
	if (preset_hint != 0)
		return preset_hint;
	// NVSDK_NGX_DLSS_Hint_Render_Preset_Default resolves per quality mode, from the header's
	// own comments (nvsdk_ngx_defs.h:82-85).
	switch (quality)
	{
	case DlssQuality::ultra_performance: return 12; // L
	case DlssQuality::max_performance: return 13;   // M
	case DlssQuality::dlaa:
	case DlssQuality::max_quality:
	case DlssQuality::balanced: break;
	}
	return 11; // K
}

const char *preset_letter(int preset)
{
	switch (preset)
	{
	case 10: return "J";
	case 11: return "K";
	case 12: return "L";
	case 13: return "M";
	case 0: return "Default";
	default: break;
	}
	return "?";
}

PresetExposure preset_exposure_support(int preset_hint, DlssQuality quality)
{
	switch (resolved_preset(preset_hint, quality))
	{
	case 10: // J
	case 11: // K
		return PresetExposure::supported;
	case 12: // L — "Preset L always uses AutoExposure" (guide §3.9). HARD.
	case 13: // M — excluded by "only supported by Presets J and K". SOFT, treated as HARD.
		return PresetExposure::not_supported;
	default: break;
	}
	return PresetExposure::unknown;
}

bool exposure_will_be_ignored(Mode mode, int preset_hint, DlssQuality quality)
{
	return wants_texture(mode) &&
		preset_exposure_support(preset_hint, quality) == PresetExposure::not_supported;
}

EvalPlan plan_evaluate(Mode mode,
                       bool texture_available,
                       float view_pre_exposure,
                       bool pre_exposure_ok,
                       float exposure_scale)
{
	EvalPlan plan;
	// InPreExposure is passed in BOTH modes. That is not an oversight copied from us into the
	// reference — DLSSUpscaler.cpp:1111-1115 sets PreExposure unconditionally and keys only the
	// flag and the texture off the mode. Pre-exposure names what is baked into the colour
	// buffer; the exposure texture names the tonemapper's multiplier. NGX separates them.
	plan.pre_exposure = sr_pre_exposure(view_pre_exposure, pre_exposure_ok);

	if (!wants_texture(mode))
	{
		// Byte-identical to the pre-exposure-work build: null texture, scale left 0 so the
		// helper's own 1.0 applies, AutoExposure flag set at create.
		plan.pass_texture = false;
		plan.exposure_scale = 0.0f;
		plan.degraded = false;
		return plan;
	}

	plan.pass_texture = texture_available;
	plan.degraded = !texture_available;
	// Only send a scale when a texture is actually going with it. With no texture the scale
	// has nothing to scale, and sending one would put a value into the indicator's "Exposure
	// level" field that describes nothing.
	plan.exposure_scale = texture_available && finite_positive(exposure_scale) ? exposure_scale
	                                                                          : 0.0f;
	return plan;
}

} // namespace stray_dlss::exposure
