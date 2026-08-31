#include "taa_signature.hpp"

#include <algorithm>

namespace stray_dlss {
namespace {

std::uint32_t group_count(std::uint32_t extent)
{
	return (extent + kTaaTileSize - 1) / kTaaTileSize;
}

bool is_dummy(const BoundTexture &t)
{
	// GSystemTextures::BlackDummy, substituted for velocity and history on a camera cut.
	return t.width <= 1 && t.height <= 1;
}

} // namespace

bool is_hdr_colour(TexFormat f)
{
	// Scene colour is not necessarily RGBA16_FLOAT. UE4 commonly uses R11G11B10_FLOAT for the
	// scene-colour target, and the TAA history can differ from the input, so both must count.
	return f == TexFormat::r16g16b16a16_float || f == TexFormat::r11g11b10_float;
}

MatchResult match_taa_dispatch(const DispatchSignature &sig,
                               std::uint32_t view_width,
                               std::uint32_t view_height)
{
	MatchResult r;

	if (sig.shader_hash == kDenoiserLookalikeHash)
	{
		r.verdict = MatchVerdict::excluded;
		r.reason = "0x1708ec956099e259 is a reprojecting denoiser (cb1[126]), not FTAAStandaloneCS";
		return r;
	}
	if (sig.shader_hash == kSecondCandidateHash)
	{
		r.verdict = MatchVerdict::excluded;
		r.reason = "0x52101a15e1a0c5cc is not FTAAStandaloneCS (cb1[131], 11 SRVs, 2 UAVs)";
		return r;
	}

	if (sig.group_count_z != 1)
	{
		r.reason = "dispatch is not 2D";
		return r;
	}

	// The strongest discriminator available at runtime, and the one the original heuristic
	// mistook for a disqualifier: FTAAStandaloneCS binds the depth-stencil resource TWICE,
	// once as R32_FLOAT_X8X24 (SceneDepthTexture) and once as X32_G8X24_UINT (StencilTexture).
	// No other pass in this title does that.
	const BoundTexture *depth = nullptr;
	const BoundTexture *stencil = nullptr;
	for (const auto &a : sig.srvs)
	{
		if (a.format != TexFormat::r32_float_x8x24_typeless)
			continue;
		for (const auto &b : sig.srvs)
		{
			if (b.format == TexFormat::x32_typeless_g8x24_uint && b.resource == a.resource)
			{
				depth = &a;
				stencil = &b;
				break;
			}
		}
		if (depth != nullptr)
			break;
	}

	if (depth == nullptr)
	{
		r.reason = "no depth+stencil SRV pair over one resource";
		return r;
	}

	// Exactly one full-size colour UAV: OutComputeTex_0.
	const BoundTexture *output = nullptr;
	for (const auto &t : sig.uavs)
	{
		if (is_hdr_colour(t.format) && !is_dummy(t))
		{
			if (output != nullptr && t.resource != output->resource)
				continue; // keep the first; extra colour UAVs are aliases of the inputs
			output = &t;
		}
	}

	if (output == nullptr)
	{
		r.reason = "no HDR float colour output UAV";
		return r;
	}

	// The dispatch covers the OUTPUT rect. Under temporal upsampling that is the display
	// resolution while the inputs are at render resolution, so matching against the render
	// rect would reject the real pass. (docs/RESEARCH.md §4.12)
	if (sig.group_count_x != group_count(output->width) ||
		sig.group_count_y != group_count(output->height))
	{
		r.reason = "dispatch size does not cover the output UAV at 8x8";
		return r;
	}

	r.depth_srv = depth->slot;
	r.stencil_srv = stencil->slot;
	r.output_uav = output->slot;
	r.output_width = output->width;
	r.output_height = output->height;
	// Compare against the DEPTH SRV's extent, which is the true render resolution, rather than
	// the passed-in view rect: that comes from the View constant buffer, which is not always
	// readable, and falling back to the dispatch size makes every pass look 1:1.
	(void)view_width;
	(void)view_height;
	r.is_upsampling = output->width > depth->width || output->height > depth->height;
	r.render_width = depth->width;
	r.render_height = depth->height;

	// On a camera cut UE4 swaps velocity and the history colour for the 1x1 BlackDummy.
	// EyeAdaptationTexture is ALSO 1x1 and is present on every frame, so a blanket "any 1x1
	// SRV" test reports a cut every frame — only the velocity and colour slots count.
	bool velocity_present = false;
	for (const auto &t : sig.srvs)
	{
		if (t.format == TexFormat::r16g16b16a16_unorm && !is_dummy(t))
		{
			r.velocity_srv = t.slot;
			velocity_present = true;
		}
	}

	std::uint32_t colours[2] = { 0, 0 };
	std::uint32_t n = 0;
	bool dummy_colour = false;
	for (const auto &t : sig.srvs)
	{
		if (!is_hdr_colour(t.format))
			continue;
		if (is_dummy(t))
			dummy_colour = true;
		else if (n < 2)
			colours[n++] = t.slot;
	}
	r.colour_srv_a = colours[0];
	r.colour_srv_b = colours[1];

	r.camera_cut_dummies = !velocity_present || dummy_colour;

	r.has_downsample_uav = sig.uavs.size() > 1;

	r.verdict = (sig.shader_hash == kTaaMainHash) ? MatchVerdict::hash_and_structural
	                                              : MatchVerdict::structural_only;
	r.reason = (r.verdict == MatchVerdict::hash_and_structural)
	               ? "hash and structure agree"
	               : "structure matches, hash unfamiliar (AA quality or resolution changed?)";
	return r;
}

} // namespace stray_dlss
