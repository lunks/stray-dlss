#include "core/nr_mask_plan.hpp"

namespace stray_dlss::nrmaskplan {
namespace {

const char *const kMaskResultNames[kMaskResultCount] = {
	"ok",
	"disabled",
	"zero-extent",
	"no-typed-uav-store",
	"integer-format",
	"alloc-failed",
};

// The runtime's own comparison is `comiss xmm1, 0.0` followed by `jae` — a SIGN test, so any
// negative AND any NaN falls through to the inherit. Reproduced rather than "cleaned up": writing
// `skin >= 0.0f` gives false for NaN in C++ too, which is the same answer for the same reason,
// but the reason is worth stating because a `!(skin < 0.0f)` would have been wrong.
bool takes_own_skin(float skin_raw)
{
	return skin_raw >= 0.0f;
}

} // namespace

// Every DXGI_FORMAT whose SRV is read as integers. Enumerated rather than derived, because the
// numeric values are the contract and a clever predicate over them would be a second thing to get
// wrong. (dxgiformat.h; the typeless members of each family are absent on purpose — the runtime's
// canonicalizer maps them to FLOAT or UNORM before the SRV is created.)
bool format_is_integer(int dxgi_format)
{
	switch (dxgi_format)
	{
	case 3:   // R32G32B32A32_UINT
	case 4:   // R32G32B32A32_SINT
	case 7:   // R32G32B32_UINT
	case 8:   // R32G32B32_SINT
	case 13:  // R16G16B16A16_UINT
	case 14:  // R16G16B16A16_SINT
	case 17:  // R32G32_UINT
	case 18:  // R32G32_SINT
	case 22:  // X32_TYPELESS_G8X24_UINT
	case 23:  // R10G10B10A2_UINT
	case 30:  // R8G8B8A8_UINT
	case 32:  // R8G8B8A8_SINT
	case 36:  // R16G16_UINT
	case 38:  // R16G16_SINT
	case 42:  // R32_UINT
	case 43:  // R32_SINT
	case 47:  // X24_TYPELESS_G8_UINT
	case 50:  // R8G8_UINT
	case 52:  // R8G8_SINT
	case 57:  // R16_UINT
	case 59:  // R16_SINT
	case 62:  // R8_UINT
	case 64:  // R8_SINT
		return true;
	default:
		return false;
	}
}

const char *mask_result_name(MaskResult result)
{
	const auto i = static_cast<int>(result);
	if (i < 0 || i >= kMaskResultCount)
		return "?";
	return kMaskResultNames[i];
}

ResolvedStructure resolve_structure(bool mask_bound, unsigned int use_auto_mask, float skin_raw,
                                    float local_raw)
{
	ResolvedStructure out;

	// 0x18001aa4b: a bound ControlMask zeroes UseAutoMask before anything reads it.
	const unsigned int effective = mask_bound ? 0u : use_auto_mask;
	out.effective_auto_mask = effective;

	if (effective == 0u)
	{
		// 0x18001aa84: both halves take the sentinel. Note that `local_raw` does NOT survive here
		// either — a reading that has it passing through would make binding a mask look free.
		out.skin = kStructureSentinel;
		out.local = kStructureSentinel;
		return out;
	}

	// 0x18001aa62-0x18001aa7a: local is passed through; skin inherits local when it is negative.
	out.local = local_raw;
	out.skin = takes_own_skin(skin_raw) ? skin_raw : local_raw;
	return out;
}

Plan plan_mask(const Config &cfg, std::uint32_t colour_width, std::uint32_t colour_height,
               int dxgi_format, const FormatSupport &support)
{
	Plan plan;
	if (!cfg.enabled)
	{
		plan.result = MaskResult::disabled;
		return plan;
	}
	if (colour_width == 0 || colour_height == 0)
	{
		plan.result = MaskResult::zero_extent;
		return plan;
	}
	// Checked BEFORE the typed-UAV probe, so the log names the real problem: a *_UINT format is
	// perfectly storable through a typed UAV and would sail past that gate on its way to a float
	// fetch against an integer texture.
	if (format_is_integer(dxgi_format))
	{
		plan.result = MaskResult::integer_format;
		return plan;
	}
	// We fill the mask with our own compute shader, so a format the device cannot STORE to
	// through a typed UAV leaves the texture uninitialised — and the runtime would sample it
	// anyway, because it validates nothing. `queried == false` means CheckFeatureSupport itself
	// failed, which is not evidence of support and is treated as its absence.
	if (!support.queried || !support.view || !support.store)
	{
		plan.result = MaskResult::no_typed_uav_store;
		return plan;
	}

	plan.result = MaskResult::ok;
	plan.width = colour_width;
	plan.height = colour_height;
	plan.value_r = cfg.channel_r ? cfg.value_r : kNeutral;
	plan.value_g = cfg.channel_g ? cfg.value_g : kNeutral;
	plan.value_b = cfg.channel_b ? cfg.value_b : kNeutral;
	plan.value_a = kNeutral;
	plan.is_identity = plan.value_r == kNeutral && plan.value_g == kNeutral
		&& plan.value_b == kNeutral;
	return plan;
}

Subrect subrect_for(const Plan &plan)
{
	Subrect r;
	if (plan.result != MaskResult::ok)
		return r;
	r.present = true;
	r.base_x = 0;
	r.base_y = 0;
	r.width = plan.width;
	r.height = plan.height;
	return r;
}

} // namespace stray_dlss::nrmaskplan
