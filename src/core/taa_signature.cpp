#include "taa_signature.hpp"

#include <algorithm>

namespace stray_dlss {
namespace {

const BoundTexture *find_format(const std::vector<BoundTexture> &v, TexFormat f)
{
	const auto it = std::find_if(v.begin(), v.end(),
		[f](const BoundTexture &t) { return t.format == f; });
	return it == v.end() ? nullptr : &*it;
}

std::uint32_t group_count(std::uint32_t extent)
{
	return (extent + kTaaTileSize - 1) / kTaaTileSize;
}

} // namespace

MatchResult match_taa_dispatch(const DispatchSignature &sig,
                               std::uint32_t view_width,
                               std::uint32_t view_height)
{
	MatchResult r;

	// Exclusions win over everything. These are hashes we have positively identified as not
	// being the pass we want, and no amount of structural resemblance may override them.
	if (sig.shader_hash == kKnownFalsePositiveHash)
	{
		r.verdict = MatchVerdict::excluded;
		r.reason = "known false positive 0x901e041a7cadc9db";
		return r;
	}
	if (sig.shader_hash == kSecondCandidateHash)
	{
		r.verdict = MatchVerdict::excluded;
		r.reason = "second candidate 0x52101a15e1a0c5cc, almost certainly motion blur";
		return r;
	}

	if (!sig.has_view_cb)
	{
		r.reason = "no View constant buffer bound";
		return r;
	}

	if (sig.group_count_z != 1 ||
		sig.group_count_x != group_count(view_width) ||
		sig.group_count_y != group_count(view_height))
	{
		r.reason = "dispatch size is not ceil(viewrect / 8)";
		return r;
	}

	const BoundTexture *depth = find_format(sig.srvs, TexFormat::r32_float_x8x24_typeless);
	if (depth == nullptr)
	{
		r.reason = "no R32_FLOAT_X8X24_TYPELESS depth SRV";
		return r;
	}

	const BoundTexture *velocity = find_format(sig.srvs, TexFormat::r16g16b16a16_unorm);
	if (velocity == nullptr)
	{
		r.reason = "no R16G16B16A16_UNORM velocity SRV";
		return r;
	}

	// Exactly two full-size R16G16B16A16_FLOAT colour SRVs: scene colour and history.
	std::vector<const BoundTexture *> colour;
	for (const auto &t : sig.srvs)
		if (t.format == TexFormat::r16g16b16a16_float)
			colour.push_back(&t);

	if (colour.size() != 2)
	{
		r.reason = "expected exactly two R16G16B16A16_FLOAT colour SRVs";
		return r;
	}

	const BoundTexture *output = find_format(sig.uavs, TexFormat::r16g16b16a16_float);
	if (output == nullptr)
	{
		r.reason = "no R16G16B16A16_FLOAT output UAV";
		return r;
	}

	r.depth_srv = depth->slot;
	r.velocity_srv = velocity->slot;
	r.colour_srv_a = colour[0]->slot;
	r.colour_srv_b = colour[1]->slot;
	r.output_uav = output->slot;

	// The stencil view is the other SRV over the same resource as depth.
	r.stencil_srv = depth->slot;
	for (const auto &t : sig.srvs)
	{
		if (t.resource == depth->resource && t.format == TexFormat::x32_typeless_g8x24_uint)
		{
			r.stencil_srv = t.slot;
			break;
		}
	}

	// u1 (OutComputeTexDownsampled) exists only under TAA_DOWNSAMPLE. When it is present we
	// are obliged to write it, or downstream bloom and DOF read garbage.
	r.has_downsample_uav = sig.uavs.size() > 1;

	r.verdict = (sig.shader_hash == kTaaMainHash) ? MatchVerdict::hash_and_structural
	                                              : MatchVerdict::structural_only;
	r.reason = (r.verdict == MatchVerdict::hash_and_structural)
	               ? "hash and structure agree"
	               : "structure matches, hash unfamiliar (AA quality setting changed?)";
	return r;
}

} // namespace stray_dlss
