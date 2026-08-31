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
// (HARD-via-mirror — SceneRenderTargets.cpp:1048-1070); GBufferC's view is sRGB
// (TexCreate_SRGB, :1137) and the recorder collapses unorm/srgb/typeless onto one class
// (frame_state.cpp). The channel-swapped R8G8B8A8 is accepted too so a licensee (or a
// driver-level swizzle) does not silently demote a correct slot to unknown.
bool is_rgba8_class(TexFormat f)
{
	return f == TexFormat::r8g8b8a8_unorm || f == TexFormat::b8g8r8a8_unorm;
}

// Scene-colour formats: Stray ships r.SceneColorFormat=3 = PF_FloatR11G11B10
// (WindowsEngine.ini:69, HARD); the stock default 4 = RGBA16F is accepted so a config
// change does not silently break identification. Deliberately the same set is_hdr_colour
// accepts — scene colour is the one target DLSS itself reads.
bool is_scene_colour_format(TexFormat f)
{
	return f == TexFormat::r11g11b10_float || f == TexFormat::r16g16b16a16_float;
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

	// The G-buffer bind is WIDE: SceneColor + A/B/C + D at minimum (§1.2). Nothing else
	// in a stock deferred frame binds this many colour targets, which is what lets the
	// signature drop the velocity requirement.
	if (colour.size() < kMinBasePassColourTargets)
	{
		r.reason = "fewer than 5 colour targets: not a G-buffer MRT set "
		           "(RESEARCH-RR-GBUFFER.md §1.2)";
		return r;
	}

	// One MRT set = one extent. Every base-pass colour target is allocated at the
	// scene-buffer extent; a mismatch means this is not one coherent set.
	const BoundTexture *first = colour[0];
	for (const BoundTexture *t : colour)
		if (t->width != first->width || t->height != first->height)
		{
			r.reason = "colour-target extents differ: not one base-pass MRT set";
			return r;
		}
	r.extent_width = first->width;
	r.extent_height = first->height;

	// Slots 0..3 must all be present. D3D12 MRT arrays are dense in practice, but a null
	// RTV in the middle is legal and would shift every positional claim.
	const BoundTexture *slot_tex[4] = {};
	for (const BoundTexture *t : colour)
		if (t->slot < 4)
			slot_tex[t->slot] = t;
	for (std::uint32_t s = 0; s < 4; ++s)
		if (slot_tex[s] == nullptr)
		{
			r.reason = "slots 0-3 are not all populated: not the stock G-buffer layout";
			return r;
		}

	// THE anchor: GBufferA's RGB10A2 at slot 1 — the only RGB10A2 render target in a
	// stock deferred frame (§1.5). An off-format slot 1 is the licensee-delta tripwire
	// and must fail loudly, not classify quietly.
	if (slot_tex[1]->format != TexFormat::r10g10b10a2_unorm)
	{
		r.reason = "slot 1 is not RGB10A2 (the GBufferA anchor): not the stock G-buffer "
		           "layout, or a licensee format delta - re-derive before trusting";
		return r;
	}

	// Velocity is CORROBORATION, never a requirement (§1.3-1.5): the live game binds a
	// velocity-free 6-RTV set. If a velocity-format member exists it must be exactly one,
	// at the stock slot 4 (`check(OutVelocityRTIndex == 4)`, SceneRenderTargets.cpp:754).
	const BoundTexture *velocity = nullptr;
	int velocity_count = 0;
	for (const BoundTexture *t : colour)
		if (t->format == TexFormat::r16g16b16a16_unorm)
		{
			velocity = t;
			++velocity_count;
		}
	if (velocity_count > 1)
	{
		r.reason = "several velocity-format targets: ambiguous, refusing to classify";
		return r;
	}
	if (velocity != nullptr && velocity->slot != kStockVelocitySlot)
	{
		r.reason = "a velocity-format target sits away from stock slot 4 "
		           "(SceneRenderTargets.cpp:754): layout not assignable, re-derive";
		return r;
	}

	// The set IS a base-pass candidate. Name the slots — each earns its role only when
	// its format fits the stock expectation, otherwise `unknown` with a note.
	r.is_base_pass = true;
	r.velocity_corroborated = velocity != nullptr;
	r.reason = r.velocity_corroborated
		? "ok: G-buffer MRT set with velocity at stock slot 4 (stock-with-base-pass-"
		  "velocity shape - corroborated)"
		: "ok: velocity-free G-buffer MRT set (RGB10A2 anchor at slot 1; the shape the "
		  "live game binds, RESEARCH-RR-GBUFFER.md §1.4-1.5)";

	// GBufferD/E sit right after the last named slot; velocity, when present, shifts
	// them one up (§1.2 table).
	const std::uint32_t slot_d = r.velocity_corroborated ? 5 : 4;

	bool has_b = false, has_c = false;
	for (auto &g : r.targets)
	{
		if (g.role == GBufferRole::depth)
			continue;

		const TexFormat f = g.tex.format;
		if (g.tex.slot == 0)
		{
			if (is_scene_colour_format(f))
			{
				g.role = GBufferRole::scene_colour;
				g.stock_format = true;
			}
			else
				g.note = "expected R11G11B10F (r.SceneColorFormat=3) or RGBA16F scene "
				         "colour";
		}
		else if (g.tex.slot == 1)
		{
			// By construction: the anchor check above already proved the format.
			g.role = GBufferRole::gbuffer_a;
			g.stock_format = true;
		}
		else if (g.tex.slot == 2)
		{
			if (is_rgba8_class(f))
			{
				g.role = GBufferRole::gbuffer_b;
				g.stock_format = true;
				has_b = true;
			}
			else
				g.note = "expected 8-bit RGBA GBufferB (metallic/specular/roughness/"
				         "shading-model)";
		}
		else if (g.tex.slot == 3)
		{
			if (is_rgba8_class(f))
			{
				g.role = GBufferRole::gbuffer_c;
				g.stock_format = true;
				has_c = true;
			}
			else
				g.note = "expected 8-bit RGBA GBufferC (base colour) - licensee variant? "
				         "unknown, not misclassified";
		}
		else if (velocity != nullptr && g.tex.slot == kStockVelocitySlot)
		{
			g.role = GBufferRole::velocity;
			g.stock_format = true; // by construction: the format IS the match
		}
		else if (g.tex.slot == slot_d)
		{
			g.note = "stock GBufferD position (custom data; 8-bit RGBA expected) - "
			         "reported for completeness";
		}
		else if (g.tex.slot == slot_d + 1)
		{
			g.note = "stock GBufferE position (precomputed shadows, "
			         "r.AllowStaticLighting=True) - reported for completeness";
		}
		else
		{
			g.note = "past the stock G-buffer layout - reported for completeness";
		}

		if (g.stock_format && g.role != GBufferRole::velocity &&
			g.role != GBufferRole::unknown)
			++r.stock_roles_matched;
	}

	// B and C carry the same format, so their order is the stock slot order and nothing
	// else. Surface that, always, whenever both were assigned. (§1.5, §5.4)
	r.bc_order_by_slot_only = has_b && has_c;

	return r;
}

} // namespace stray_dlss
