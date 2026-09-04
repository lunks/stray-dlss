#include "core/u0_rhi_uav.hpp"

#include <cstring>

namespace stray_dlss::u0 {

// ---------------------------------------------------------------------------------------
// .pdata
// ---------------------------------------------------------------------------------------

namespace {

// UNWIND_INFO, x64 (winnt.h): Version:3 | Flags:5, SizeOfProlog, CountOfCodes, FrameRegister:4 |
// FrameOffset:4, then CountOfCodes UNWIND_CODE (2 bytes each, padded to an even count), then
// either an exception handler RVA or, under UNW_FLAG_CHAININFO, a chained RUNTIME_FUNCTION.
constexpr std::uint8_t kUnwFlagChainInfo = 0x4;
constexpr unsigned kMaxChain = 8;

const RuntimeFunction *lookup(const FunctionTable &t, std::uint32_t rva)
{
	if (t.entries == nullptr || t.count == 0)
		return nullptr;
	std::size_t lo = 0;
	std::size_t hi = t.count;
	// Last entry with begin <= rva.
	while (lo < hi)
	{
		const std::size_t mid = lo + (hi - lo) / 2;
		if (t.entries[mid].begin <= rva)
			lo = mid + 1;
		else
			hi = mid;
	}
	if (lo == 0)
		return nullptr;
	const RuntimeFunction *e = &t.entries[lo - 1];
	if (rva < e->begin || rva >= e->end)
		return nullptr;
	return e;
}

} // namespace

std::uint64_t function_start(const FunctionTable &t, std::uint64_t va)
{
	if (t.image_base == 0 || va < t.image_base || va - t.image_base > 0xFFFFFFFFull)
		return 0;
	const RuntimeFunction *e = lookup(t, static_cast<std::uint32_t>(va - t.image_base));
	if (e == nullptr)
		return 0;
	for (unsigned hop = 0; hop < kMaxChain; ++hop)
	{
		if (t.image == nullptr || e->unwind == 0)
			break;
		unsigned char head[4] = {};
		if (!t.image->read(t.image_base + e->unwind, head, sizeof(head)))
			break;
		const std::uint8_t flags = static_cast<std::uint8_t>(head[0] >> 3);
		if ((flags & kUnwFlagChainInfo) == 0)
			break;
		const unsigned codes = head[2];
		const std::uint64_t chained = t.image_base + e->unwind + 4u + ((codes + 1u) & ~1u) * 2u;
		RuntimeFunction parent{};
		if (!t.image->read(chained, &parent, sizeof(parent)))
			break;
		if (parent.begin == 0 || parent.end <= parent.begin)
			break;
		const RuntimeFunction *p = lookup(t, parent.begin);
		if (p == nullptr || p == e)
			break;
		e = p;
	}
	return t.image_base + e->begin;
}

// ---------------------------------------------------------------------------------------
// The vtable
// ---------------------------------------------------------------------------------------

const char *ctx_status_text(CtxStatus s)
{
	switch (s)
	{
	case CtxStatus::ok: return "ok";
	case CtxStatus::no_seed: return "no seed (the Dispatch hook's return address never resolved to a function)";
	case CtxStatus::seed_not_code: return "the seed is not inside an executable section of the game module";
	case CtxStatus::no_vtable: return "no read-only qword in the module equals the seed";
	case CtxStatus::ambiguous: return "more than one vtable candidate passed every prediction";
	case CtxStatus::slot_not_code: return "a slot in the checked range does not point into code";
	case CtxStatus::prediction_failed: return "a slot predicted to be an empty body is not one";
	default: return "?";
	}
}

namespace {

bool begins_with(const seam::Image &image, std::uint64_t va, const unsigned char *bytes, std::size_t n)
{
	unsigned char got[4] = {};
	if (n > sizeof(got) || !image.read(va, got, n))
		return false;
	return std::memcmp(got, bytes, n) == 0;
}

bool expectation_holds(const seam::Image &image, const std::uint64_t *slot, std::uint64_t seed,
                       const SlotExpectation &e)
{
	static const unsigned char kRet[] = { 0xC3 };
	static const unsigned char kXorRet[] = { 0x33, 0xC0, 0xC3 };
	const std::uint64_t fn = slot[e.slot];
	switch (e.expect)
	{
	case Expect::code: return image.is_code(fn);
	case Expect::ret: return begins_with(image, fn, kRet, sizeof(kRet));
	case Expect::xorret: return begins_with(image, fn, kXorRet, sizeof(kXorRet));
	case Expect::seed: return fn == seed;
	}
	return false;
}

} // namespace

CtxDiscovery discover_context_vtable(const seam::Image &image, std::uint64_t seed)
{
	CtxDiscovery d;
	d.seed = seed;
	if (seed == 0)
	{
		d.status = CtxStatus::no_seed;
		return d;
	}
	if (!image.is_code(seed))
	{
		d.status = CtxStatus::seed_not_code;
		return d;
	}

	constexpr std::size_t kMaxHits = 16;
	std::uint64_t hits[kMaxHits] = {};
	const std::size_t n = seam::find_qword(image, seed, hits, kMaxHits);
	d.qword_hits = static_cast<std::uint32_t>(n);
	if (n == 0)
	{
		d.status = CtxStatus::no_vtable;
		return d;
	}

	CtxStatus last_failure = CtxStatus::no_vtable;
	std::uint32_t last_failed_slot = 0;
	for (std::size_t i = 0; i < n && i < kMaxHits; ++i)
	{
		const std::uint64_t vtable = hits[i] - 8ull * kSlotDispatchComputeShader;
		std::uint64_t slot[kSlotsChecked] = {};
		if (!image.read(vtable, slot, sizeof(slot)))
			continue;

		bool ok = true;
		for (unsigned s = 0; s < kSlotsChecked && ok; ++s)
		{
			if (!image.is_code(slot[s]))
			{
				ok = false;
				last_failure = CtxStatus::slot_not_code;
				last_failed_slot = s;
			}
		}
		std::uint32_t mask = 0;
		for (std::size_t k = 0; k < kSlotExpectationCount && ok; ++k)
		{
			const SlotExpectation &e = kSlotExpectations[k];
			const bool held = expectation_holds(image, slot, seed, e);
			if (held)
				mask |= 1u << k;
			if (!held && e.required)
			{
				ok = false;
				last_failure = e.expect == Expect::seed ? CtxStatus::no_vtable : CtxStatus::prediction_failed;
				last_failed_slot = e.slot;
			}
		}
		if (!ok)
			continue;

		++d.survivors;
		if (d.survivors == 1)
		{
			d.vtable_va = vtable;
			std::memcpy(d.slot, slot, sizeof(slot));
			// The reported ones, evaluated for the record now that the required ones held.
			for (std::size_t k = 0; k < kSlotExpectationCount; ++k)
			{
				const SlotExpectation &e = kSlotExpectations[k];
				if (!e.required && expectation_holds(image, slot, seed, e))
					mask |= 1u << k;
			}
			d.expectation_mask = mask;
			// How many of the required `ret` slots share one address (ICF folding).
			std::uint32_t fold = 0;
			for (std::size_t k = 0; k < kSlotExpectationCount; ++k)
			{
				const SlotExpectation &e = kSlotExpectations[k];
				if (e.required && e.expect == Expect::ret &&
					slot[e.slot] == slot[kSlotSetAsyncComputeBudget])
					++fold;
			}
			d.ret_fold = fold;
		}
	}

	if (d.survivors == 0)
	{
		d.status = last_failure;
		d.failed_slot = last_failed_slot;
		return d;
	}
	if (d.survivors > 1)
	{
		d.status = CtxStatus::ambiguous;
		return d;
	}
	d.status = CtxStatus::ok;
	return d;
}

// ---------------------------------------------------------------------------------------
// The object scan
// ---------------------------------------------------------------------------------------

const char *scan_status_text(ScanStatus s)
{
	switch (s)
	{
	case ScanStatus::ok: return "ok";
	case ScanStatus::no_hit: return "no-hit";
	case ScanStatus::ambiguous: return "ambiguous";
	case ScanStatus::dead: return "dead";
	case ScanStatus::unreadable: return "unreadable";
	default: return "?";
	}
}

ScanResult scan_object_for_handle(const ObjectReader &r, const HandleLookup &l,
                                  std::uint64_t object)
{
	ScanResult out;
	if (object == 0 || r.read_u64 == nullptr || l.probe == nullptr)
		return out;
	unsigned readable = 0;
	for (unsigned q = kScanFirstQword; q < kScanQwords; ++q)
	{
		std::uint64_t v = 0;
		if (!r.read_u64(r.ctx, object + 8ull * q, &v))
			break; // past the end of the allocation; what was read still counts
		++readable;
		if (v == 0)
			continue;
		HandleKind kind = HandleKind::none;
		std::uint64_t res = 0;
		bool dead = false;
		if (!l.probe(l.ctx, v, &kind, &res, &dead))
			continue;
		if (kind != HandleKind::uav && kind != HandleKind::srv)
			continue;
		++out.hits;
		if (out.hits == 1)
		{
			out.offset = 8u * q;
			out.kind = kind;
			out.handle = v;
			out.resource = res;
			out.status = dead ? ScanStatus::dead : ScanStatus::ok;
		}
	}
	if (readable == 0)
	{
		out.status = ScanStatus::unreadable;
		return out;
	}
	if (out.hits == 0)
		out.status = ScanStatus::no_hit;
	else if (out.hits > 1)
	{
		out.status = ScanStatus::ambiguous;
		out.resource = 0;
	}
	return out;
}

ScanResult read_handle_at(const ObjectReader &r, const HandleLookup &l, std::uint64_t object,
                          unsigned offset, HandleKind want)
{
	ScanResult out;
	out.offset = offset;
	if (object == 0 || r.read_u64 == nullptr || l.probe == nullptr)
		return out;
	std::uint64_t v = 0;
	if (!r.read_u64(r.ctx, object + offset, &v))
		return out;
	HandleKind kind = HandleKind::none;
	std::uint64_t res = 0;
	bool dead = false;
	if (v == 0 || !l.probe(l.ctx, v, &kind, &res, &dead) || kind != want)
	{
		out.status = ScanStatus::no_hit;
		return out;
	}
	out.hits = 1;
	out.kind = kind;
	out.handle = v;
	out.resource = dead ? 0 : res;
	out.status = dead ? ScanStatus::dead : ScanStatus::ok;
	return out;
}

bool OffsetLatch::observe(unsigned offset)
{
	if (m_latched)
		return false;
	if (m_run != 0 && offset != m_offset)
	{
		m_run = 0;
	}
	m_offset = offset;
	++m_run;
	if (m_run >= kLatchAgreements)
	{
		m_latched = true;
		return true;
	}
	return false;
}

void OffsetLatch::reset()
{
	m_latched = false;
	m_offset = 0;
	m_run = 0;
}

// ---------------------------------------------------------------------------------------
// Slot roles
// ---------------------------------------------------------------------------------------

const char *slot_role_name(SlotRole r)
{
	switch (r)
	{
	case SlotRole::unknown: return "unknown";
	case SlotRole::uav: return "uav";
	case SlotRole::srv: return "srv";
	case SlotRole::silent: return "silent";
	default: return "?";
	}
}

SlotRole classify_slot(const SlotCensus &c)
{
	if (c.classified < kClassifySamples)
		return SlotRole::unknown;
	if (c.uav_hits > c.srv_hits && c.uav_hits > c.no_hits)
		return SlotRole::uav;
	if (c.srv_hits > c.uav_hits && c.srv_hits > c.no_hits)
		return SlotRole::srv;
	if (c.no_hits >= c.uav_hits && c.no_hits >= c.srv_hits)
		return SlotRole::silent;
	return SlotRole::unknown;
}

// ---------------------------------------------------------------------------------------
// The assertion
// ---------------------------------------------------------------------------------------

const char *verdict_name(Verdict v)
{
	switch (v)
	{
	case Verdict::agree: return "agree";
	case Verdict::disagree: return "DISAGREE";
	case Verdict::no_bind: return "noBind";
	case Verdict::unresolved: return "unresolved";
	case Verdict::not_live: return "notLive";
	case Verdict::walk_absent: return "walkAbsent";
	case Verdict::desc_mismatch: return "descMismatch";
	default: return "?";
	}
}

Judgement judge(bool bind_present, ScanStatus chain, std::uint64_t rhi_u0,
                std::uint64_t walk_u0, const DescFacts &d,
                std::uint32_t out_width, std::uint32_t out_height)
{
	Judgement j;
	if (!bind_present)
	{
		j.verdict = Verdict::no_bind;
		return j;
	}
	if (chain != ScanStatus::ok || rhi_u0 == 0)
	{
		j.verdict = Verdict::unresolved;
		return j;
	}
	if (!d.live)
	{
		j.verdict = Verdict::not_live;
		return j;
	}
	// The engine's u0 is a 2D, UAV-capable, HDR-float texture at least as large as the rect the
	// engine announced it would write (the pooled element may be larger — GetOutputExtent()'s
	// Max(), CLAUDE.md §5 — so >= is the test and equality is reported beside it).
	if (d.is_buffer || d.is_3d || !d.allow_uav || !d.hdr_colour ||
		d.width < out_width || d.height < out_height)
	{
		j.verdict = Verdict::desc_mismatch;
		return j;
	}
	j.extent_equal = d.width == out_width && d.height == out_height;
	if (walk_u0 == 0)
	{
		j.verdict = Verdict::walk_absent;
		return j;
	}
	j.verdict = rhi_u0 == walk_u0 ? Verdict::agree : Verdict::disagree;
	return j;
}

} // namespace stray_dlss::u0
