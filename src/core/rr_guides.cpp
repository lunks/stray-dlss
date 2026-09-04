#include "core/rr_guides.hpp"

namespace stray_dlss::rrguides {
namespace {

// The DXGI values, spelled out rather than included: this translation unit is built for Linux
// too (CLAUDE.md §6 — the pure lane runs on every push and must not need d3d12.h).
constexpr std::uint32_t kR10G10B10A2Typeless = 23;
constexpr std::uint32_t kR10G10B10A2Unorm = 24;
constexpr std::uint32_t kR8G8B8A8Typeless = 27;
constexpr std::uint32_t kR8G8B8A8Unorm = 28;
constexpr std::uint32_t kR8G8B8A8UnormSrgb = 29;
constexpr std::uint32_t kB8G8R8A8Unorm = 87;
constexpr std::uint32_t kB8G8R8A8Typeless = 90;
constexpr std::uint32_t kB8G8R8A8UnormSrgb = 91;

bool member_ok(const Record &r, Refusal &out)
{
	if (!r.seen)
	{
		out = Refusal::name_missing;
		return false;
	}
	if (!r.status_ok || r.resource == 0)
	{
		out = Refusal::record_bad;
		return false;
	}
	return true;
}

} // namespace

const char *refusal_name(Refusal r)
{
	switch (r)
	{
	case Refusal::none: return "ok";
	case Refusal::not_supplying: return "notSupplying";
	case Refusal::not_hooked: return "notHooked";
	case Refusal::name_missing: return "nameMissing";
	case Refusal::record_bad: return "recordBad";
	case Refusal::epoch_split: return "epochSplit";
	case Refusal::stale: return "stale";
	case Refusal::not_live: return "notLive";
	case Refusal::extent_disagree: return "extentDisagree";
	case Refusal::extent_mismatch: return "extentMismatch";
	case Refusal::extent_too_small: return "extentTooSmall";
	case Refusal::format_a: return "formatA";
	case Refusal::format_bc: return "formatBC";
	default: return "?";
	}
}

bool gbuffer_a_format_ok(std::uint32_t dxgi_format)
{
	return dxgi_format == kR10G10B10A2Unorm || dxgi_format == kR10G10B10A2Typeless;
}

bool gbuffer_bc_format_ok(std::uint32_t dxgi_format)
{
	switch (dxgi_format)
	{
	case kR8G8B8A8Typeless:
	case kR8G8B8A8Unorm:
	case kR8G8B8A8UnormSrgb:
	case kB8G8R8A8Typeless:
	case kB8G8R8A8Unorm:
	case kB8G8R8A8UnormSrgb:
		return true;
	default:
		return false;
	}
}

Refusal judge(const Set &set, const Expect &expect)
{
	if (!expect.supplying)
		return Refusal::not_supplying;
	if (!expect.hooked)
		return Refusal::not_hooked;

	Refusal why = Refusal::none;
	if (!member_ok(set.a, why) || !member_ok(set.b, why) || !member_ok(set.c, why))
		return why;

	// ONE ALLOCATION CYCLE. The three records are written by three separate engine calls; a set
	// assembled from two cycles pairs one frame's normals with another's base colour, which is
	// exactly the kind of wrong-but-plausible input CLAUDE.md §0.2 exists to refuse.
	if (set.a.epoch == 0 || set.a.epoch != set.b.epoch || set.a.epoch != set.c.epoch)
		return Refusal::epoch_split;

	// Freshness, measured from the OLDEST member so a half-refreshed set cannot pass on the
	// strength of its youngest.
	std::uint64_t oldest = set.a.frame;
	if (set.b.frame < oldest) oldest = set.b.frame;
	if (set.c.frame < oldest) oldest = set.c.frame;
	if (expect.frame_now > oldest && expect.frame_now - oldest > kMaxSetAgeFrames)
		return Refusal::stale;

	// LIVENESS, and it is our own registry's answer rather than the engine's. A pooled element
	// can be released and its ID3D12Resource destroyed between the allocation that named it and
	// the dispatch that would read it; D3D12 then reuses the address, so a stale pointer comes
	// back as a DIFFERENT valid resource. Only a set of resources known LIVE (never a set of
	// known-dead ones) can answer that — CLAUDE.md §5, hazard 2.
	if (!set.a.live || !set.b.live || !set.c.live)
		return Refusal::not_live;

	if (set.a.width != set.b.width || set.a.width != set.c.width ||
		set.a.height != set.b.height || set.a.height != set.c.height)
		return Refusal::extent_disagree;

	// View row 132 is BufferSizeAndInvSize = SceneContext.GetBufferSizeXY(), which is the extent
	// every G-buffer is allocated at. It reaches us from the engine's own
	// CachedViewUniformShaderParameters by a route that shares nothing with the pool hook, so a
	// disagreement is two independent engine answers conflicting — a finding, not a nuisance.
	if (expect.buffer_width != 0 && expect.buffer_height != 0 &&
		(set.a.width != expect.buffer_width || set.a.height != expect.buffer_height))
		return Refusal::extent_mismatch;

	// The resolve indexes the G-buffers at ViewRectMin + thread id, so they must COVER the
	// render rect. Equality is not required and must not be demanded: at any screen percentage
	// below 100 the scene buffer is larger than the view.
	if (expect.render_width == 0 || expect.render_height == 0 ||
		set.a.width < expect.render_width || set.a.height < expect.render_height)
		return Refusal::extent_too_small;

	if (!gbuffer_a_format_ok(set.a.dxgi_format))
		return Refusal::format_a;
	if (!gbuffer_bc_format_ok(set.b.dxgi_format) || !gbuffer_bc_format_ok(set.c.dxgi_format))
		return Refusal::format_bc;

	return Refusal::none;
}

} // namespace stray_dlss::rrguides
