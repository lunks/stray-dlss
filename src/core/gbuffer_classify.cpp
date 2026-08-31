#include "gbuffer_classify.hpp"

namespace stray_dlss {
namespace {

// GSystemTextures dummies: 1x1, never a real render target. The base pass never binds one
// as an RTV, but the recorder must not depend on that. (CLAUDE.md §2.3 — "any 1x1 SRV" was
// once misread as a signal; here they are simply ignored.)
bool is_dummy(const BoundTexture &t)
{
	return t.width <= 1 && t.height <= 1;
}

// Both halves of the R32G8X24 depth resource (HARD — CLAUDE.md §2.4): the recorder maps
// every depth/stencil VIEW format onto these two (frame_state.cpp, to_tex_format).
bool is_depth_or_stencil(TexFormat f)
{
	return f == TexFormat::r32_float_x8x24_typeless ||
		f == TexFormat::x32_typeless_g8x24_uint;
}

// The 8-bit-RGBA family covering stock GBufferB/C. UE4 creates them as PF_B8G8R8A8
// (SOFT — GetGBufferBFormat/GetGBufferCFormat), which is DXGI B8G8R8A8; the channel-swapped
// R8G8B8A8 is accepted too so a licensee (or a driver-level swizzle) does not silently
// demote a correct slot to unknown.
bool is_rgba8_class(TexFormat f)
{
	return f == TexFormat::r8g8b8a8_unorm || f == TexFormat::b8g8r8a8_unorm;
}

// Stock scene-colour formats (SOFT — r.SceneColorFormat: default PF_FloatRGBA, option
// PF_FloatR11G11B10). Deliberately the same set is_hdr_colour accepts — scene colour is
// the one target DLSS itself will read.
bool is_stock_scene_colour(TexFormat f)
{
	return f == TexFormat::r16g16b16a16_float || f == TexFormat::r11g11b10_float;
}

} // namespace

const char *gbuffer_role_name(GBufferRole role)
{
	switch (role)
	{
	case GBufferRole::scene_colour: return "scene-colour";
	case GBufferRole::gbuffer_a:    return "GBufferA";
	case GBufferRole::gbuffer_b:    return "GBufferB";
	case GBufferRole::gbuffer_c:    return "GBufferC";
	case GBufferRole::velocity:     return "velocity";
	case GBufferRole::depth:        return "depth";
	case GBufferRole::unknown:      return "unknown";
	}
	return "?";
}

const char *tex_format_name(TexFormat f)
{
	switch (f)
	{
	case TexFormat::r32_float_x8x24_typeless: return "DEPTH";
	case TexFormat::x32_typeless_g8x24_uint:  return "STENCIL";
	case TexFormat::r16g16b16a16_unorm:       return "RGBA16_UNORM";
	case TexFormat::r16g16b16a16_float:       return "RGBA16F";
	case TexFormat::r16g16_float:             return "RG16F";
	case TexFormat::r32g32b32a32_float:       return "RGBA32F";
	case TexFormat::r11g11b10_float:          return "R11G11B10F";
	case TexFormat::r10g10b10a2_unorm:        return "RGB10A2";
	case TexFormat::r32_float:                return "R32F";
	case TexFormat::r16_float:                return "R16F";
	case TexFormat::r8g8b8a8_unorm:           return "RGBA8";
	case TexFormat::b8g8r8a8_unorm:           return "BGRA8";
	case TexFormat::unknown:                  return "other";
	}
	return "?";
}

GBufferClassification classify_render_target_set(const std::vector<BoundTexture> &outputs)
{
	GBufferClassification r;

	// Split the set: dummies dropped, depth/stencil views set aside (the recorder appends
	// the DSV after the RTVs, so slot arithmetic below runs over colour targets only).
	std::vector<const BoundTexture *> colour;
	for (const auto &t : outputs)
	{
		if (is_dummy(t))
			continue;

		GBufferTarget g;
		g.tex = t;
		if (is_depth_or_stencil(t.format))
		{
			g.role = GBufferRole::depth;
			// Both halves of the measured R32G8X24 resource count as the stock depth.
			g.stock_format = true;
			r.targets.push_back(g);
			continue;
		}
		r.targets.push_back(g);
		colour.push_back(&t);
	}

	if (colour.empty())
	{
		r.reason = "no colour render targets (dummies and depth only)";
		return r;
	}

	// Duplicate slots would make every positional claim below meaningless. Never observed;
	// refuse loudly rather than pick one. (CLAUDE.md §0.2)
	for (std::size_t i = 0; i < colour.size(); ++i)
		for (std::size_t j = i + 1; j < colour.size(); ++j)
			if (colour[i]->slot == colour[j]->slot)
			{
				r.reason = "duplicate RT slot in the set, refusing to classify";
				return r;
			}

	// The anchor: exactly one velocity-format target. (HARD — R16G16B16A16_UNORM, §2.5;
	// present in the base pass because r.BasePassOutputsVelocity ships True, §2.3.1.)
	const BoundTexture *velocity = nullptr;
	int velocity_count = 0;
	for (const BoundTexture *t : colour)
		if (t->format == TexFormat::r16g16b16a16_unorm)
		{
			velocity = t;
			++velocity_count;
		}

	if (velocity_count == 0)
	{
		r.reason = "no R16G16B16A16_UNORM velocity target: not the base pass "
		           "(r.BasePassOutputsVelocity signature, CLAUDE.md §2.3.1/§2.5)";
		return r;
	}
	if (velocity_count > 1)
	{
		r.reason = "several R16G16B16A16_UNORM targets: ambiguous, refusing to classify";
		return r;
	}

	r.extent_width = velocity->width;
	r.extent_height = velocity->height;

	// Every colour target of one base-pass MRT set is allocated at the scene-buffer extent
	// (§2.5: velocity is at BufferSizeAndInvSize; the G-buffers share it). A mismatch means
	// this is not one coherent MRT set, whatever the formats say.
	for (const BoundTexture *t : colour)
		if (t->width != velocity->width || t->height != velocity->height)
		{
			r.reason = "colour-target extents differ from the velocity target: not one "
			           "base-pass MRT set";
			return r;
		}

	if (velocity->slot != kStockVelocitySlot)
	{
		// The velocity anchor holds but the SOFT slot order does not, so no positional role
		// can be trusted. Report the members (the recorder logs slot+format for each) and
		// say exactly what failed instead of guessing a licensee layout.
		r.reason = "velocity target is not at stock slot 4 (GetGBufferRenderTargets order, "
		           "SOFT): slot roles are not assignable, layout must be re-derived";
		return r;
	}

	// Slots 0..3 must all be present ahead of the velocity target. D3D12 MRT arrays are
	// dense in practice, but a null RTV in the middle is legal and would shift every claim.
	const BoundTexture *slot_tex[kStockVelocitySlot] = {};
	for (const BoundTexture *t : colour)
		if (t->slot < kStockVelocitySlot)
			slot_tex[t->slot] = t;
	for (std::uint32_t s = 0; s < kStockVelocitySlot; ++s)
		if (slot_tex[s] == nullptr)
		{
			r.reason = "slots 0-3 are not all populated ahead of the velocity target: not "
			           "the stock base-pass layout";
			return r;
		}

	// The set IS a base-pass candidate. Now name the slots — each only when its format
	// fits the stock expectation, otherwise `unknown` with a note. (Header comment carries
	// the SOFT provenance for every expectation.)
	r.is_base_pass = true;
	r.reason = "ok: single full-extent velocity target at stock slot 4 with 4 colour "
	           "targets ahead of it";

	bool has_b = false, has_c = false;
	for (auto &g : r.targets)
	{
		if (g.role == GBufferRole::depth)
			continue;

		const TexFormat f = g.tex.format;
		switch (g.tex.slot)
		{
		case 0:
			if (is_stock_scene_colour(f))
			{
				g.role = GBufferRole::scene_colour;
				g.stock_format = true;
			}
			else
				g.note = "expected RGBA16F or R11G11B10F scene colour (r.SceneColorFormat)";
			break;
		case 1:
			if (f == TexFormat::r10g10b10a2_unorm)
			{
				g.role = GBufferRole::gbuffer_a;
				g.stock_format = true;
			}
			else if (f == TexFormat::r16g16b16a16_float)
				g.note = "RGBA16F where stock GBufferA is RGB10A2 - high-precision "
				         "G-buffer variant? report, do not assume";
			else
				g.note = "expected RGB10A2 world-normal GBufferA";
			break;
		case 2:
			if (is_rgba8_class(f))
			{
				g.role = GBufferRole::gbuffer_b;
				g.stock_format = true;
				has_b = true;
			}
			else
				g.note = "expected 8-bit RGBA GBufferB (metallic/specular/roughness/"
				         "shading-model)";
			break;
		case 3:
			if (is_rgba8_class(f))
			{
				g.role = GBufferRole::gbuffer_c;
				g.stock_format = true;
				has_c = true;
			}
			else
				g.note = "expected 8-bit RGBA GBufferC (base colour + AO) - licensee "
				         "variant? unknown, not misclassified";
			break;
		case kStockVelocitySlot:
			g.role = GBufferRole::velocity;
			g.stock_format = true; // by construction: the anchor IS the format match
			break;
		default:
			g.note = "stock slot for GBufferD/E (custom data / precomputed shadows) - "
			         "reported for completeness";
			break;
		}

		if (g.stock_format && g.role != GBufferRole::velocity &&
			g.role != GBufferRole::unknown)
			++r.stock_roles_matched;
	}

	// B and C carry the same format, so their order is the SOFT slot order and nothing
	// else. Surface that, always, whenever both were assigned.
	r.bc_order_by_slot_only = has_b && has_c;

	return r;
}

} // namespace stray_dlss
