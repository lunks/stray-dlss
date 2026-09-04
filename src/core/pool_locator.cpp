#include "core/pool_locator.hpp"

#include <cstring>

namespace stray_dlss::pool {

// ---------------------------------------------------------------------------------------
// The name table. Every entry is a literal passed to GRenderTargetPool.FindFreeElement in
// SceneRenderTargets.cpp @ 4.27/306a7e9 unless marked otherwise; the line numbers are that
// file's and the enclosing function is what gives the scan its independence.
// ---------------------------------------------------------------------------------------
const Name kNames[] = {
	{ "GBufferA", Target::gbuffer_a, true, "AllocGBufferTargets" },                       // :1125
	{ "GBufferB", Target::gbuffer_b, true, "AllocGBufferTargets" },                       // :1132
	{ "GBufferC", Target::gbuffer_c, true, "AllocGBufferTargets" },                       // :1139
	{ "GBufferD", Target::gbuffer_d, true, "AllocGBufferTargets" },                       // :1146
	{ "GBufferE", Target::gbuffer_e, true, "AllocGBufferTargets" },                       // :1153
	{ "GBufferF", Target::gbuffer_f, true, "AllocateAnisotropyTarget" },                  // :1741
	{ "SceneDepthZ", Target::scene_depth_z, true, "AllocateCommonDepthTargets" },         // :1543 — UEVR's own anchor
	{ "SceneDepthAux", Target::scene_depth_aux, true, "AllocGBufferTargets" },            // :1161
	{ "GBufferVelocity", Target::gbuffer_velocity, true, "AllocGBufferTargets + AllocateDeferredShadingPathRenderTargets" }, // :1172, :1722
	// NOT an anchor: AllocSceneColor passes GetSceneColorTargetName(CurrentShadingPath),
	// which returns the literal out of a table (SceneRenderTargets.cpp:315-324), so the only
	// reference to it is inside that accessor and nothing useful follows it.
	{ "SceneColorDeferred", Target::scene_color_deferred, false, "GetSceneColorTargetName (returned, not passed)" }, // :1005/:320
	{ "SmallDepthZ", Target::small_depth_z, true, "AllocateDeferredShadingPathRenderTargets" },      // :1601
	{ "ScreenSpaceAO", Target::screen_space_ao, true, "AllocateDeferredShadingPathRenderTargets" },  // :1618
	{ "LightAccumulation", Target::light_accumulation, true, "AllocateDeferredShadingPathRenderTargets" }, // :1714
	{ "DirectionalOcclusion", Target::directional_occlusion, true, "AllocateDeferredShadingPathRenderTargets" }, // :1704
	{ "QuadOverdrawBuffer", Target::quad_overdraw_buffer, true, "AllocateDebugViewModeTargets" },    // :1496
	{ "SkySHIrradianceMap", Target::sky_sh_irradiance_map, true, "AllocateReflectionTargets" },      // :1459
	{ "ReflectionColorScratchCubemap0", Target::reflection_scratch_0, true, "AllocateReflectionTargets" }, // :1444
	{ "ReflectionColorScratchCubemap1", Target::reflection_scratch_1, true, "AllocateReflectionTargets" }, // :1445
	{ "DiffuseIrradianceScratchCubemap0", Target::diffuse_irradiance_0, true, "AllocateReflectionTargets" }, // :1453
	{ "DiffuseIrradianceScratchCubemap1", Target::diffuse_irradiance_1, true, "AllocateReflectionTargets" }, // :1454
};
static_assert(sizeof(kNames) / sizeof(kNames[0]) == kNameCount,
	"kNames must carry exactly one entry per Target, in Target order");

// The UE_LOG format string inside FindFreeElementInternal (RenderTargetPool.cpp:403). Its
// PRESENCE settles whether this build ships with USE_LOGGING_IN_SHIPPING; its absence closes
// the direct-anchor route to the internal and is itself the answer.
constexpr char kInternalLogLiteral[] = "%d MB, NewRT %s %s";

int match_name(const char *ascii)
{
	if (ascii == nullptr)
		return -1;
	for (std::size_t i = 0; i < kNameCount; ++i)
	{
		if (std::strcmp(ascii, kNames[i].text) == 0)
			return static_cast<int>(i);
	}
	return -1;
}

const char *target_name(Target t)
{
	const std::size_t i = static_cast<std::size_t>(t);
	return i < kNameCount ? kNames[i].text : "?";
}

Level level_from_int(int v)
{
	if (v <= 0)
		return Level::off;
	if (v == 1)
		return Level::discover;
	if (v == 2)
		return Level::observe;
	return Level::supply;
}

const char *level_name(Level l)
{
	switch (l)
	{
	case Level::off: return "off";
	case Level::discover: return "discover";
	case Level::observe: return "observe";
	case Level::supply: return "supply";
	}
	return "?";
}

const char *locate_status_text(LocateStatus s)
{
	switch (s)
	{
	case LocateStatus::ok: return "ok";
	case LocateStatus::no_regions: return "no regions to scan";
	case LocateStatus::no_literals: return "no pooled-render-target name literal is in this image";
	case LocateStatus::no_references: return "the literals are there, but nothing loads one with lea reg,[rip+d]";
	case LocateStatus::no_candidates: return "no rel32 CALL near any name reference lands on a .pdata function start";
	case LocateStatus::insufficient: return "a best candidate exists and it does not clear the agreement bar";
	case LocateStatus::ambiguous: return "two candidates tie on distinct enclosing functions - refusing rather than picking";
	case LocateStatus::count: break;
	}
	return "?";
}

const char *record_status_text(RecordStatus s)
{
	switch (s)
	{
	case RecordStatus::ok: return "ok";
	case RecordStatus::unknown_name: return "unknown-name";
	case RecordStatus::name_unreadable: return "name-unreadable";
	case RecordStatus::out_null: return "out-null";
	case RecordStatus::pooled_unreadable: return "pooled-unreadable";
	case RecordStatus::rhi_null: return "rhi-null";
	case RecordStatus::rhi_unreadable: return "rhi-unreadable";
	case RecordStatus::fn_not_code: return "fn-not-code";
	case RecordStatus::not_registered: return "not-registered";
	case RecordStatus::count: break;
	}
	return "?";
}

const char *assert_verdict_text(AssertVerdict v)
{
	switch (v)
	{
	case AssertVerdict::absent: return "absent";
	case AssertVerdict::agree: return "agree";
	case AssertVerdict::disagree: return "disagree";
	case AssertVerdict::count: break;
	}
	return "?";
}

AssertVerdict judge_resource(std::uint64_t pool_a, std::uint64_t pool_b, std::uint64_t other)
{
	if (other == 0 || (pool_a == 0 && pool_b == 0))
		return AssertVerdict::absent;
	if (other == pool_a || other == pool_b)
		return AssertVerdict::agree;
	return AssertVerdict::disagree;
}

AssertVerdict judge_extent(std::uint32_t got_w, std::uint32_t got_h,
                           std::uint32_t expected_w, std::uint32_t expected_h)
{
	if (got_w == 0 || got_h == 0 || expected_w == 0 || expected_h == 0)
		return AssertVerdict::absent;
	return (got_w == expected_w && got_h == expected_h) ? AssertVerdict::agree
	                                                    : AssertVerdict::disagree;
}

bool extent_is_predicted(Target t)
{
	switch (t)
	{
	case Target::gbuffer_a:
	case Target::gbuffer_b:
	case Target::gbuffer_c:
	case Target::gbuffer_d:
	case Target::gbuffer_e:
	case Target::gbuffer_f:
	case Target::scene_depth_z:
	case Target::gbuffer_velocity:
	case Target::scene_color_deferred:
		return true;
	default:
		return false;
	}
}

// ---------------------------------------------------------------------------------------
// Scanning
// ---------------------------------------------------------------------------------------

namespace {

std::int32_t read_i32(const unsigned char *p)
{
	std::int32_t v = 0;
	std::memcpy(&v, p, sizeof(v));
	return v;
}

const seam::Region *region_of(const seam::Image &image, std::uint64_t va)
{
	for (std::size_t i = 0; i < image.count; ++i)
	{
		const seam::Region &r = image.regions[i];
		if (r.bytes != nullptr && va >= r.va && va < r.va + r.size)
			return &r;
	}
	return nullptr;
}

// The last .pdata entry with begin <= rva, if it covers rva. A private copy of the binary
// search u0_rhi_uav.cpp keeps in its own anonymous namespace; ten lines is cheaper than
// widening that file's interface for one caller.
const u0::RuntimeFunction *pdata_entry(const u0::FunctionTable &t, std::uint64_t va)
{
	if (t.entries == nullptr || t.count == 0 || t.image_base == 0 || va < t.image_base)
		return nullptr;
	const std::uint64_t rel = va - t.image_base;
	if (rel > 0xFFFFFFFFull)
		return nullptr;
	const std::uint32_t rva = static_cast<std::uint32_t>(rel);
	std::size_t lo = 0;
	std::size_t hi = t.count;
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
	const u0::RuntimeFunction *e = &t.entries[lo - 1];
	return (rva >= e->begin && rva < e->end) ? e : nullptr;
}

} // namespace

void find_name_literals(const seam::Image &image,
                        std::uint64_t out[][kMaxLiteralHitsPerName], unsigned *hits)
{
	for (std::size_t i = 0; i < kNameCount; ++i)
		hits[i] = 0;
	if (image.regions == nullptr)
		return;

	// One needle per name, UTF-16LE INCLUDING the terminator so "GBufferATexture" cannot match
	// "GBufferA". Built once and matched in a single pass over the image, because fifteen
	// separate walks of a ~200 MB shipping executable on the device-creation thread is a
	// startup stall this project has already paid for once.
	constexpr std::size_t kMaxNeedle = 80;
	unsigned char needle[kNameCount][kMaxNeedle] = {};
	std::size_t needle_len[kNameCount] = {};
	std::size_t shortest = 0;
	for (std::size_t i = 0; i < kNameCount; ++i)
	{
		std::size_t n = 0;
		for (const char *p = kNames[i].text; *p != '\0' && n + 4 <= kMaxNeedle; ++p)
		{
			needle[i][n++] = static_cast<unsigned char>(*p);
			needle[i][n++] = 0;
		}
		needle[i][n++] = 0;
		needle[i][n++] = 0;
		needle_len[i] = n;
		if (shortest == 0 || n < shortest)
			shortest = n;
	}
	// First-byte dispatch: most names begin with a different letter, and the ones that do not
	// ('G', 'S', 'D', 'R') collapse to a handful of memcmps rather than kNameCount.
	unsigned char by_first[256][kNameCount] = {};
	unsigned by_first_count[256] = {};
	for (std::size_t i = 0; i < kNameCount; ++i)
	{
		const unsigned char c = needle[i][0];
		by_first[c][by_first_count[c]++] = static_cast<unsigned char>(i);
	}

	for (std::size_t ri = 0; ri < image.count; ++ri)
	{
		const seam::Region &r = image.regions[ri];
		if (r.bytes == nullptr || r.size < shortest)
			continue;
		// Wide literals are 2-byte aligned by construction.
		for (std::size_t off = 0; off + 2 <= r.size; off += 2)
		{
			const unsigned char c = r.bytes[off];
			const unsigned n = by_first_count[c];
			if (n == 0 || r.bytes[off + 1] != 0)
				continue;
			for (unsigned k = 0; k < n; ++k)
			{
				const std::size_t i = by_first[c][k];
				if (off + needle_len[i] > r.size)
					continue;
				if (std::memcmp(r.bytes + off, needle[i], needle_len[i]) != 0)
					continue;
				if (hits[i] < kMaxLiteralHitsPerName)
					out[i][hits[i]] = r.va + off;
				++hits[i];
			}
		}
	}
}

std::size_t find_lea_rip_refs(const seam::Image &image,
                              const std::uint64_t *targets, const std::uint32_t *owners,
                              std::size_t target_count, LeaSite *out, std::size_t max_out)
{
	if (image.regions == nullptr || targets == nullptr || target_count == 0)
		return 0;
	std::uint64_t lo = targets[0];
	std::uint64_t hi = targets[0];
	for (std::size_t i = 1; i < target_count; ++i)
	{
		if (targets[i] < lo)
			lo = targets[i];
		if (targets[i] > hi)
			hi = targets[i];
	}

	std::size_t found = 0;
	for (std::size_t ri = 0; ri < image.count; ++ri)
	{
		const seam::Region &r = image.regions[ri];
		if (!r.executable || r.bytes == nullptr || r.size < 7)
			continue;
		for (std::size_t off = 0; off + 7 <= r.size; ++off)
		{
			// REX.W (or REX.WR for r8-r15) + 8D /r with mod=00, rm=101: rip-relative lea.
			const unsigned char b0 = r.bytes[off];
			if (b0 != 0x48 && b0 != 0x4C)
				continue;
			if (r.bytes[off + 1] != 0x8D)
				continue;
			const unsigned char modrm = r.bytes[off + 2];
			if ((modrm & 0xC7) != 0x05)
				continue;
			const std::uint64_t va = r.va + off;
			// The displacement is relative to the END of the 7-byte instruction.
			const std::uint64_t target =
				va + 7u + static_cast<std::uint64_t>(static_cast<std::int64_t>(read_i32(r.bytes + off + 3)));
			if (target < lo || target > hi)
				continue;
			for (std::size_t t = 0; t < target_count; ++t)
			{
				if (targets[t] != target)
					continue;
				if (found < max_out && out != nullptr)
				{
					out[found].va = va;
					out[found].which = owners != nullptr ? owners[t] : static_cast<std::uint32_t>(t);
				}
				++found;
				break;
			}
		}
	}
	return found;
}

std::size_t collect_calls_after(const seam::Image &image, const u0::FunctionTable &table,
                                std::uint64_t from, CallHit *out, std::size_t max_out)
{
	const seam::Region *r = region_of(image, from);
	if (r == nullptr || !r->executable)
		return 0;
	const std::size_t start = static_cast<std::size_t>(from - r->va);
	std::size_t limit = start + kForwardBytes;
	if (limit > r->size)
		limit = r->size;

	std::size_t found = 0;
	// Skip the seven bytes of the lea itself: its own displacement must never be read as an
	// opcode, and nothing before the name load can be the call that consumes it.
	for (std::size_t at = start + 7; at + 5 <= limit; ++at)
	{
		if (r->bytes[at] != 0xE8)
			continue;
		const std::uint64_t site = r->va + at;
		const std::uint64_t target =
			site + 5u + static_cast<std::uint64_t>(static_cast<std::int64_t>(read_i32(r->bytes + at + 1)));
		if (!image.is_code(target))
			continue;
		// THE FILTER THAT MAKES A BYTE SCAN HONEST. Without decoding instructions we may read
		// an 0xE8 that is really a displacement byte inside some other instruction; such a
		// target is effectively random, and a random address landing EXACTLY on a
		// RUNTIME_FUNCTION::BeginAddress is a ~1e-5 event per candidate. Every real call
		// target is a function start by construction.
		if (u0::function_start(table, target) != target)
			continue;
		if (found < max_out && out != nullptr)
		{
			out[found].target = target;
			out[found].distance = static_cast<std::uint32_t>(site - from);
		}
		++found;
	}
	return found;
}

bool function_calls(const seam::Image &image, const u0::FunctionTable &table,
                    std::uint64_t container, std::uint64_t callee)
{
	const u0::RuntimeFunction *e = pdata_entry(table, container);
	if (e == nullptr || callee == 0)
		return false;
	const std::uint64_t begin = table.image_base + e->begin;
	const std::uint64_t end = table.image_base + e->end;
	const seam::Region *r = region_of(image, begin);
	if (r == nullptr || !r->executable)
		return false;
	std::size_t at = static_cast<std::size_t>(begin - r->va);
	std::size_t limit = static_cast<std::size_t>(end > r->va + r->size ? r->size : end - r->va);
	for (; at + 5 <= limit; ++at)
	{
		if (r->bytes[at] != 0xE8)
			continue;
		const std::uint64_t site = r->va + at;
		const std::uint64_t target =
			site + 5u + static_cast<std::uint64_t>(static_cast<std::int64_t>(read_i32(r->bytes + at + 1)));
		if (target == callee)
			return true;
	}
	return false;
}

// ---------------------------------------------------------------------------------------
// The verdict
// ---------------------------------------------------------------------------------------

namespace {

void note_group(Candidate &c, std::uint64_t group)
{
	if (group == 0)
		return;
	for (unsigned i = 0; i < c.group_count; ++i)
	{
		if (c.groups[i] == group)
			return;
	}
	if (c.group_count < kMaxGroupsPerCandidate)
		c.groups[c.group_count++] = group;
}

} // namespace

Locate locate(const seam::Image &image, const u0::FunctionTable &table)
{
	Locate v;
	if (image.regions == nullptr || image.count == 0)
	{
		v.status = LocateStatus::no_regions;
		return v;
	}

	std::uint64_t literals[kNameCount][kMaxLiteralHitsPerName] = {};
	unsigned literal_hits[kNameCount] = {};
	find_name_literals(image, literals, literal_hits);

	// Flatten the ANCHOR literals into one target list for the single lea pass.
	std::uint64_t targets[kNameCount * kMaxLiteralHitsPerName] = {};
	std::uint32_t owners[kNameCount * kMaxLiteralHitsPerName] = {};
	std::size_t target_count = 0;
	for (std::size_t i = 0; i < kNameCount; ++i)
	{
		if (literal_hits[i] != 0)
			++v.literals_found;
		if (!kNames[i].anchor)
			continue;
		const unsigned n = literal_hits[i] < kMaxLiteralHitsPerName ? literal_hits[i]
		                                                            : kMaxLiteralHitsPerName;
		for (unsigned k = 0; k < n; ++k)
		{
			targets[target_count] = literals[i][k];
			owners[target_count] = static_cast<std::uint32_t>(i);
			++target_count;
		}
	}
	if (target_count == 0)
	{
		v.status = LocateStatus::no_literals;
		return v;
	}

	LeaSite sites[kMaxLeaSites] = {};
	const std::size_t site_total = find_lea_rip_refs(image, targets, owners, target_count,
		sites, kMaxLeaSites);
	v.lea_sites = static_cast<unsigned>(site_total);
	v.lea_sites_overflowed = site_total > kMaxLeaSites;
	const std::size_t site_count = site_total < kMaxLeaSites ? site_total : kMaxLeaSites;
	if (site_count == 0)
	{
		v.status = LocateStatus::no_references;
		return v;
	}
	{
		std::uint32_t seen = 0;
		for (std::size_t i = 0; i < site_count; ++i)
			seen |= (1u << sites[i].which);
		for (std::size_t i = 0; i < kNameCount; ++i)
		{
			if ((seen & (1u << i)) != 0)
				++v.refs_found;
		}
	}

	Candidate cands[kMaxCandidates] = {};
	std::size_t cand_count = 0;
	for (std::size_t i = 0; i < site_count; ++i)
	{
		CallHit calls[kMaxCallsPerSite] = {};
		const std::size_t n = collect_calls_after(image, table, sites[i].va, calls, kMaxCallsPerSite);
		const std::size_t take = n < kMaxCallsPerSite ? n : kMaxCallsPerSite;
		// The ENCLOSING function of the reference, derived from the exe's own .pdata rather
		// than from kNames::source_fn. Two anchors in one function are one piece of evidence.
		const std::uint64_t group = u0::function_start(table, sites[i].va);
		for (std::size_t k = 0; k < take; ++k)
		{
			Candidate *c = nullptr;
			for (std::size_t j = 0; j < cand_count; ++j)
			{
				if (cands[j].target == calls[k].target)
				{
					c = &cands[j];
					break;
				}
			}
			if (c == nullptr)
			{
				if (cand_count >= kMaxCandidates)
				{
					v.candidates_overflowed = true;
					continue;
				}
				c = &cands[cand_count++];
				c->target = calls[k].target;
			}
			c->name_mask |= (1u << sites[i].which);
			note_group(*c, group);
			++c->sites;
			if (calls[k].distance < c->min_distance)
				c->min_distance = calls[k].distance;
		}
	}
	v.candidates = static_cast<unsigned>(cand_count);
	if (cand_count == 0)
	{
		v.status = LocateStatus::no_candidates;
		return v;
	}
	for (std::size_t j = 0; j < cand_count; ++j)
	{
		unsigned n = 0;
		for (unsigned b = 0; b < kNameCount; ++b)
		{
			if ((cands[j].name_mask & (1u << b)) != 0)
				++n;
		}
		cands[j].name_count = n;
	}

	// Rank on DISTINCT ENCLOSING FUNCTIONS first: a helper shared by several call sites inside
	// one function is reached by many names and only ever one group, which is exactly the
	// false positive this ordering refuses. Names, then proximity, break ties for reporting.
	std::size_t best = 0;
	std::size_t second = cand_count;
	for (std::size_t j = 1; j < cand_count; ++j)
	{
		const Candidate &a = cands[j];
		const Candidate &b = cands[best];
		const bool better = a.group_count > b.group_count ||
			(a.group_count == b.group_count && a.name_count > b.name_count) ||
			(a.group_count == b.group_count && a.name_count == b.name_count &&
				a.min_distance < b.min_distance);
		if (better)
			best = j;
	}
	for (std::size_t j = 0; j < cand_count; ++j)
	{
		if (j == best)
			continue;
		if (second == cand_count || cands[j].group_count > cands[second].group_count ||
			(cands[j].group_count == cands[second].group_count &&
				cands[j].name_count > cands[second].name_count))
			second = j;
	}

	v.target = cands[best].target;
	v.names = cands[best].name_count;
	v.groups = cands[best].group_count;
	v.sites = cands[best].sites;
	v.name_mask = cands[best].name_mask;
	v.min_distance = cands[best].min_distance;
	if (second != cand_count)
	{
		v.runner_up = cands[second].target;
		v.runner_up_names = cands[second].name_count;
		v.runner_up_groups = cands[second].group_count;
	}
	v.entry_read = image.read(v.target, v.entry, sizeof(v.entry));

	// The residual, settled: is FindFreeElementInternal's own UE_LOG format string still here?
	{
		unsigned char needle[64] = {};
		std::size_t n = 0;
		for (const char *p = kInternalLogLiteral; *p != '\0' && n + 4 <= sizeof(needle); ++p)
		{
			needle[n++] = static_cast<unsigned char>(*p);
			needle[n++] = 0;
		}
		needle[n++] = 0;
		needle[n++] = 0;
		for (std::size_t ri = 0; ri < image.count && v.internal_log_hits == 0; ++ri)
		{
			const seam::Region &r = image.regions[ri];
			if (r.bytes == nullptr || r.size < n)
				continue;
			for (std::size_t off = 0; off + n <= r.size; off += 2)
			{
				if (r.bytes[off] != needle[0] || std::memcmp(r.bytes + off, needle, n) != 0)
					continue;
				v.internal_log_literal_va = r.va + off;
				++v.internal_log_hits;
				break;
			}
		}
		if (v.internal_log_literal_va != 0)
		{
			LeaSite refs[8] = {};
			const std::uint32_t owner0 = 0;
			const std::size_t rn = find_lea_rip_refs(image, &v.internal_log_literal_va, &owner0,
				1, refs, 8);
			const std::size_t take = rn < 8 ? rn : 8;
			std::uint64_t fns[8] = {};
			unsigned fn_count = 0;
			for (std::size_t i = 0; i < take; ++i)
			{
				const std::uint64_t f = u0::function_start(table, refs[i].va);
				if (f == 0)
					continue;
				bool seen = false;
				for (unsigned k = 0; k < fn_count; ++k)
					seen = seen || fns[k] == f;
				if (!seen && fn_count < 8)
					fns[fn_count++] = f;
			}
			v.internal_fn_candidates = fn_count;
			if (fn_count == 1)
			{
				v.internal_fn = fns[0];
				// RenderTargetPool.cpp:703 — FindFreeElement's last act is to call
				// FindFreeElementInternal. When the literal survives, this is a fourth and
				// completely independent confirmation that `target` is the right function.
				v.internal_called_by_target = function_calls(image, table, v.target, v.internal_fn);
			}
		}
	}

	if (second != cand_count && cands[second].group_count >= cands[best].group_count)
	{
		v.status = LocateStatus::ambiguous;
		return v;
	}
	if (v.groups < kMinAgreeingGroups || v.names < kMinAgreeingNames)
	{
		v.status = LocateStatus::insufficient;
		return v;
	}
	v.status = LocateStatus::ok;
	return v;
}

} // namespace stray_dlss::pool
