#include "pass_walk.hpp"

namespace stray_dlss {
namespace {

// GSystemTextures::BlackDummy and friends: substituted for real inputs on a camera cut, and
// never a real output. (CLAUDE.md §2.3)
bool is_dummy(const BoundTexture &t)
{
	return t.width <= 1 && t.height <= 1;
}

std::uint64_t area(const BoundTexture &t)
{
	return std::uint64_t{ t.width } * std::uint64_t{ t.height };
}

bool is_depth_format(TexFormat f)
{
	return f == TexFormat::r32_float_x8x24_typeless;
}

bool is_stencil_format(TexFormat f)
{
	return f == TexFormat::x32_typeless_g8x24_uint;
}

// The largest non-dummy HDR colour input, which is the "scene image" flowing down the post
// chain — as opposed to bloom (smaller), velocity/depth (not colour), LUTs (3D) and the eye
// adaptation texture (1x1).
const BoundTexture *largest_hdr_input(const FrameEvent &e)
{
	const BoundTexture *best = nullptr;
	for (const auto &t : e.srvs)
		if (!t.is_3d && !is_dummy(t) && is_hdr_colour(t.format))
			if (best == nullptr || area(t) > area(*best))
				best = &t;
	return best;
}

// Which input the walk follows OUT of an event, towards its producer.
const BoundTexture *followable_input(const FrameEvent &e)
{
	// A copy has exactly one meaningful input: its source, whatever the format.
	if (e.kind == FrameEvent::Kind::copy)
		return e.srvs.empty() ? nullptr : &e.srvs[0];

	if (const BoundTexture *hdr = largest_hdr_input(e))
		return hdr;

	// Below the tonemapper the scene image is LDR (the Slate composite reads R10G10B10A2),
	// so when no HDR input exists follow the largest non-dummy 2D input instead. A wrong
	// turn here cannot produce a wrong ANSWER — the stop condition still has to hold — only
	// a "not found".
	const BoundTexture *best = nullptr;
	for (const auto &t : e.srvs)
		if (!t.is_3d && !is_dummy(t))
			if (best == nullptr || area(t) > area(*best))
				best = &t;
	return best;
}

// The last event STRICTLY BEFORE `before` that writes `resource`. Position-aware on purpose:
// UE4's transient allocator reuses render targets within a frame, so a frame-global "last
// writer" can name a later scratch pass that aliased the resource after our reader ran.
const FrameEvent *last_writer(const std::vector<FrameEvent> &frame,
                              std::uint64_t resource, std::uint64_t before)
{
	const FrameEvent *best = nullptr;
	for (const auto &e : frame)
	{
		if (e.sequence >= before)
			continue;
		for (const auto &o : e.outputs)
		{
			if (o.resource == resource)
			{
				if (best == nullptr || e.sequence > best->sequence)
					best = &e;
				break;
			}
		}
	}
	return best;
}

bool reads_live_velocity(const FrameEvent &e)
{
	// UE4's GBuffer velocity format, and only a real texture — the 1x1 BlackDummy UE4
	// substitutes on a camera cut does not count (and is not even in this format).
	for (const auto &t : e.srvs)
		if (t.format == TexFormat::r16g16b16a16_unorm && !is_dummy(t))
			return true;
	return false;
}

// The tonemapper: the only pass in an Unreal frame that binds a 3D texture SRV — the
// colour-grading LUT (PostProcessTonemap.cpp:533,765). The LUT-BUILDING pass (CombineLUTs)
// can also bind 3D source LUTs, which is why a 3D SRV alone is not enough: the anchor must
// also read a real HDR scene image, which CombineLUTs never does. Among qualifiers, the one
// with the largest HDR input wins; ties go to the latest in the frame.
const FrameEvent *find_tonemap_anchor(const std::vector<FrameEvent> &frame)
{
	const FrameEvent *best = nullptr;
	std::uint64_t best_area = 0;
	for (const auto &e : frame)
	{
		bool has_3d = false;
		for (const auto &t : e.srvs)
			has_3d = has_3d || t.is_3d;
		if (!has_3d)
			continue;

		const BoundTexture *in = largest_hdr_input(e);
		if (in == nullptr)
			continue;

		const std::uint64_t a = area(*in);
		if (best == nullptr || a > best_area ||
			(a == best_area && e.sequence > best->sequence))
		{
			best = &e;
			best_area = a;
		}
	}
	return best;
}

WalkHop make_hop(const FrameEvent &e, const BoundTexture *via, const char *note)
{
	WalkHop h;
	h.sequence = e.sequence;
	h.kind = e.kind;
	h.pipeline_hash = e.pipeline_hash;
	if (via != nullptr)
	{
		h.resource = via->resource;
		h.format = via->format;
		h.width = via->width;
		h.height = via->height;
	}
	h.note = note;
	return h;
}

} // namespace

bool is_replaceable_write_set(const std::vector<BoundTexture> &outputs, const char **why)
{
	const char *sink = "";
	if (why == nullptr)
		why = &sink;
	*why = "";

	// 1x1 bindings are dummies, not outputs. (UE4 pads unused UAV slots with them.)
	std::vector<const BoundTexture *> real;
	for (const auto &t : outputs)
		if (!is_dummy(t))
			real.push_back(&t);

	if (real.empty())
	{
		*why = "writes nothing a replacement could reproduce";
		return false;
	}

	// Anything that is not an HDR colour target is a write we would not perform. Depth and
	// stencil are named separately because that is the measured 0xd2e4d8c23c362ed1 failure:
	// suppressed, its depth and stencil went unwritten and everything downstream composited
	// against stale depth. (CLAUDE.md §1)
	for (const BoundTexture *t : real)
	{
		if (is_depth_format(t->format))
		{
			*why = "writes depth";
			return false;
		}
		if (is_stencil_format(t->format))
		{
			*why = "writes stencil";
			return false;
		}
		if (!is_hdr_colour(t->format))
		{
			*why = "writes a target in a non-HDR-colour format we would not fill";
			return false;
		}
	}

	// Exactly one full-res colour target. FTAAStandaloneCS's only other output is the
	// optional HALF-res OutComputeTex_1, so companions may be at most half the full target
	// in each dimension; a second full-res target is one we would leave unwritten.
	const BoundTexture *full = real[0];
	for (const BoundTexture *t : real)
		if (area(*t) > area(*full))
			full = t;

	const std::uint32_t companion_w = (full->width + 1) / 2;
	const std::uint32_t companion_h = (full->height + 1) / 2;
	for (const BoundTexture *t : real)
	{
		if (t == full)
			continue;
		if (t->width > companion_w || t->height > companion_h)
		{
			*why = "writes a second full-res target we would not fill";
			return false;
		}
	}

	return true;
}

WalkResult find_temporal_pass(const std::vector<FrameEvent> &frame,
                              std::uint64_t presented_resource,
                              const std::function<bool(std::uint64_t)> &owns_history)
{
	WalkResult r;

	if (frame.empty())
	{
		r.reason = "empty frame: nothing was recorded";
		return r;
	}

	// Anchor. Prefer the tonemapper; fall back to whatever wrote the presented image.
	const FrameEvent *current = find_tonemap_anchor(frame);
	if (current != nullptr)
	{
		r.chain.push_back(make_hop(*current, nullptr,
			"anchor: tonemapper (3D colour-grading LUT SRV)"));
	}
	else if (presented_resource != 0 &&
		(current = last_writer(frame, presented_resource, UINT64_MAX)) != nullptr)
	{
		// Fill the anchor hop with the back buffer it was found through.
		const BoundTexture *via = nullptr;
		for (const auto &o : current->outputs)
			if (o.resource == presented_resource)
				via = &o;
		r.chain.push_back(make_hop(*current, via,
			"anchor: last writer of the back buffer (no 3D LUT SRV found)"));
	}
	else
	{
		r.reason = "no anchor: no tonemapper (3D LUT SRV) in the frame and no writer for "
		           "the back buffer";
		return r;
	}

	for (std::uint32_t hop = 0; hop < kMaxWalkHops; ++hop)
	{
		const BoundTexture *in = followable_input(*current);
		if (in == nullptr)
		{
			r.reason = "chain broke: pass has no followable input";
			return r;
		}

		const FrameEvent *w = last_writer(frame, in->resource, current->sequence);
		if (w == nullptr)
		{
			r.reason = "chain broke: nothing earlier in the frame writes the followed "
			           "resource (recording started mid-frame, or it is a cross-frame "
			           "resource)";
			return r;
		}

		// The stop condition. Only a compute pass can be FTAAStandaloneCS, and the history
		// round-trip — accumulated by the caller across frames — is what separates the TAA
		// from motion blur, which matches every per-frame structural test but never rebinds
		// its own previous output.
		const bool candidate = w->kind == FrameEvent::Kind::dispatch &&
			owns_history != nullptr && owns_history(w->pipeline_hash);

		if (candidate)
		{
			// The two positively identified non-TAA shaders. (CLAUDE.md §2.3)
			if (w->pipeline_hash == kDenoiserLookalikeHash ||
				w->pipeline_hash == kSecondCandidateHash)
			{
				r.chain.push_back(make_hop(*w, in, "on the exclusion list"));
				r.reason = "candidate is on the exclusion list (CLAUDE.md §2.3), refusing "
				           "to select it";
				return r;
			}

			// A cut frame is not identifiable: velocity (and history) are the 1x1
			// BlackDummy, so the confirming velocity read is missing. Hopping THROUGH the
			// pass instead would put the next history-owning pass upstream — an SSR-style
			// denoiser — at the stop position, which is a wrong answer. Stop and say why.
			if (!reads_live_velocity(*w))
			{
				r.chain.push_back(make_hop(*w, in, "history-owning, but no live velocity"));
				r.reason = "candidate owns the temporal history but reads no live velocity "
				           "this frame (camera cut?); not selecting, retry on a steady frame";
				return r;
			}

			// The write-set gate: a pass we cannot fully reproduce must never be selected,
			// however well it matches — the measured 0xd2e4d8c23c362ed1 lesson.
			const char *why = "";
			if (!is_replaceable_write_set(w->outputs, &why))
			{
				r.chain.push_back(make_hop(*w, in, why));
				r.reason = "temporal pass found, but its write set is not replaceable - "
				           "suppressing it would leave outputs unwritten";
				return r;
			}

			r.chain.push_back(make_hop(*w, in, "selected"));
			r.taa_hash = w->pipeline_hash;
			r.reason = "ok: first history-owning velocity-reading compute pass on the "
			           "anchor chain";
			return r;
		}

		r.chain.push_back(make_hop(*w, in, ""));
		current = w;
	}

	r.reason = "hop budget exhausted without a history-owning velocity-reading compute pass";
	return r;
}

} // namespace stray_dlss
