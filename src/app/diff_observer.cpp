#include "app/diff_observer.hpp"

#include "core/taa_hashes.hpp"
#include "log.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <unordered_map>

namespace stray_dlss::diff {
namespace {

const char *fmt_name(TexFormat f)
{
	switch (f)
	{
	case TexFormat::unknown: return "unknown";
	case TexFormat::r32_float_x8x24_typeless: return "R32_FLOAT_X8X24";
	case TexFormat::x32_typeless_g8x24_uint: return "X32_G8X24_UINT";
	case TexFormat::r16g16b16a16_unorm: return "R16G16B16A16_UNORM";
	case TexFormat::r16g16b16a16_float: return "R16G16B16A16_FLOAT";
	case TexFormat::r16g16_float: return "R16G16_FLOAT";
	case TexFormat::r32g32b32a32_float: return "R32G32B32A32_FLOAT";
	case TexFormat::r11g11b10_float: return "R11G11B10_FLOAT";
	case TexFormat::r10g10b10a2_unorm: return "R10G10B10A2_UNORM";
	case TexFormat::r32_float: return "R32_FLOAT";
	case TexFormat::r16_float: return "R16_FLOAT";
	case TexFormat::r8g8b8a8_unorm: return "R8G8B8A8_UNORM";
	case TexFormat::b8g8r8a8_unorm: return "B8G8R8A8_UNORM";
	}
	return "?";
}

std::string describe(const BoundTexture &t)
{
	char buf[128];
	std::snprintf(buf, sizeof(buf), "res %llx fmt %s %ux%u%s", static_cast<unsigned long long>(t.resource),
		fmt_name(t.format), t.width, t.height, t.is_3d ? " 3d" : "");
	return buf;
}

// The verdict for one differing slot, from what each side named and what each liveness
// tracker says about it. `oracle_res` / `native_res` are 0 where that side had nothing.
struct Judged
{
	Verdict verdict = Verdict::unadjudicated;
	int o_rs = -1, o_reg = -1, o_seen = -1; // the oracle's resource: live per ReShade / live per the registry / ever registered
	int n_rs = -1, n_reg = -1, n_seen = -1; // the native side's resource, same three answers
};

Judged judge(icept::ResourceId oracle_res, icept::ResourceId native_res, const Adjudicator *adj)
{
	Judged j;
	if (adj == nullptr || adj->oracle_live == nullptr || adj->native_live == nullptr || adj->native_seen == nullptr)
		return j;
	if (oracle_res != 0)
	{
		j.o_rs = adj->oracle_live(oracle_res) ? 1 : 0;
		j.o_reg = adj->native_live(oracle_res) ? 1 : 0;
		j.o_seen = adj->native_seen(oracle_res) ? 1 : 0;
	}
	if (native_res != 0)
	{
		j.n_rs = adj->oracle_live(native_res) ? 1 : 0;
		j.n_reg = adj->native_live(native_res) ? 1 : 0;
		j.n_seen = adj->native_seen(native_res) ? 1 : 0;
	}
	// In order of how much each answer proves. ReShade contradicting ITSELF — a view that maps
	// to a resource its own destroy_resource event has retired — is the CLAUDE.md §5 stale map
	// and needs no second tracker to convict. A resource the registry never registered is a
	// blind spot of ours, not evidence about the resource. Only then do the two trackers get
	// compared with each other.
	if (oracle_res != 0 && j.o_rs == 0)
		j.verdict = Verdict::reshade_stale;
	else if (oracle_res != 0 && j.o_seen == 0)
		j.verdict = Verdict::native_blind;
	else if ((oracle_res != 0 && j.o_reg == 0) || (native_res != 0 && (j.n_rs == 0 || j.n_reg == 0)))
		j.verdict = Verdict::liveness_conflict;
	else if (native_res == 0)
		j.verdict = Verdict::native_missed;
	else if (oracle_res == 0)
		j.verdict = Verdict::oracle_missed;
	else
		j.verdict = Verdict::both_live;
	return j;
}

void append_judgement(std::string &line, const Judged &j, Result &out)
{
	++out.verdicts[static_cast<int>(j.verdict)];
	if (j.verdict == Verdict::unadjudicated)
		return;
	char buf[160];
	std::snprintf(buf, sizeof(buf), " [oracle-res live rs=%d reg=%d seen=%d | native-res live rs=%d reg=%d seen=%d] => %s",
		j.o_rs, j.o_reg, j.o_seen, j.n_rs, j.n_reg, j.n_seen, verdict_name(j.verdict));
	line += buf;
}

void compare_slots(const char prefix, const std::vector<BoundTexture> &expected,
                   const std::vector<BoundTexture> &actual, Result &out, const Adjudicator *adj)
{
	for (const BoundTexture &e : expected)
	{
		const auto a = std::find_if(actual.begin(), actual.end(),
			[&](const BoundTexture &t) { return t.slot == e.slot; });
		char buf[256];
		if (a == actual.end())
		{
			std::snprintf(buf, sizeof(buf), "%c%u: oracle=(%s) native=UNKNOWN", prefix, e.slot, describe(e).c_str());
			std::string line = buf;
			append_judgement(line, judge(e.resource, 0, adj), out);
			out.unknown.push_back(line);
			continue;
		}
		if (a->resource != e.resource || a->format != e.format || a->width != e.width ||
			a->height != e.height || a->is_3d != e.is_3d)
		{
			std::snprintf(buf, sizeof(buf), "%c%u: oracle=(%s) native=(%s)", prefix, e.slot,
				describe(e).c_str(), describe(*a).c_str());
			std::string line = buf;
			append_judgement(line, judge(e.resource, a->resource, adj), out);
			out.mismatches.push_back(line);
		}
	}
	for (const BoundTexture &a : actual)
	{
		const bool known = std::any_of(expected.begin(), expected.end(),
			[&](const BoundTexture &e) { return e.slot == a.slot; });
		if (!known)
		{
			char buf[256];
			std::snprintf(buf, sizeof(buf), "%c%u: oracle=ABSENT native=(%s)", prefix, a.slot, describe(a).c_str());
			std::string line = buf;
			append_judgement(line, judge(0, a.resource, adj), out);
			out.extra.push_back(line);
		}
	}
}

} // namespace

const char *verdict_name(Verdict v)
{
	switch (v)
	{
	case Verdict::reshade_stale: return "RESHADE-STALE";
	case Verdict::native_blind: return "NATIVE-BLIND";
	case Verdict::liveness_conflict: return "LIVENESS-CONFLICT";
	case Verdict::native_missed: return "NATIVE-MISSED";
	case Verdict::oracle_missed: return "ORACLE-MISSED";
	case Verdict::both_live: return "BOTH-LIVE";
	case Verdict::unadjudicated: return "unadjudicated";
	}
	return "?";
}

Result compare(const icept::DispatchBindings &expected, const icept::DispatchBindings &actual,
               const Adjudicator *adj)
{
	Result r;
	compare_slots('t', expected.srvs, actual.srvs, r, adj);
	compare_slots('u', expected.uavs, actual.uavs, r, adj);

	// Constant buffers as a multiset of (buffer, offset): the oracle keys root CBVs by root
	// PARAMETER index and table CBVs by register, so `first` is not comparable across
	// backends; the bytes the View CB read will map are.
	std::vector<icept::BufferRange> want, have;
	for (const auto &cb : expected.constant_buffers)
		want.push_back(cb.second);
	for (const auto &cb : actual.constant_buffers)
		have.push_back(cb.second);
	const auto less = [](const icept::BufferRange &a, const icept::BufferRange &b) {
		return a.buffer != b.buffer ? a.buffer < b.buffer : a.offset < b.offset;
	};
	std::sort(want.begin(), want.end(), less);
	std::sort(have.begin(), have.end(), less);
	for (const auto &w : want)
	{
		const auto it = std::find_if(have.begin(), have.end(),
			[&](const icept::BufferRange &h) { return h.buffer == w.buffer && h.offset == w.offset; });
		char buf[160];
		if (it == have.end())
		{
			std::snprintf(buf, sizeof(buf), "cb: oracle=(buf %llx +%llu) native=UNKNOWN",
				static_cast<unsigned long long>(w.buffer), static_cast<unsigned long long>(w.offset));
			std::string line = buf;
			append_judgement(line, judge(w.buffer, 0, adj), r);
			r.unknown.push_back(line);
		}
		else
		{
			have.erase(it);
		}
	}
	for (const auto &h : have)
	{
		char buf[160];
		std::snprintf(buf, sizeof(buf), "cb: oracle=ABSENT native=(buf %llx +%llu)",
			static_cast<unsigned long long>(h.buffer), static_cast<unsigned long long>(h.offset));
		std::string line = buf;
		append_judgement(line, judge(0, h.buffer, adj), r);
		r.extra.push_back(line);
	}

	if (expected.view_cb_valid || actual.view_cb_valid)
	{
		if (expected.view_cb_valid != actual.view_cb_valid ||
			expected.view_cb.buffer != actual.view_cb.buffer ||
			expected.view_cb.offset != actual.view_cb.offset ||
			expected.view_cb_register != actual.view_cb_register)
		{
			char buf[200];
			std::snprintf(buf, sizeof(buf), "view_cb: oracle=(valid %d buf %llx +%llu b%u) native=(valid %d buf %llx +%llu b%u)",
				expected.view_cb_valid ? 1 : 0, static_cast<unsigned long long>(expected.view_cb.buffer),
				static_cast<unsigned long long>(expected.view_cb.offset), expected.view_cb_register,
				actual.view_cb_valid ? 1 : 0, static_cast<unsigned long long>(actual.view_cb.buffer),
				static_cast<unsigned long long>(actual.view_cb.offset), actual.view_cb_register);
			std::string line = buf;
			append_judgement(line, judge(expected.view_cb_valid ? expected.view_cb.buffer : 0,
				actual.view_cb_valid ? actual.view_cb.buffer : 0, adj), r);
			r.mismatches.push_back(line);
		}
	}

	// Heaps as a set.
	for (unsigned i = 0; i < expected.heap_count; ++i)
	{
		bool found = false;
		for (unsigned j = 0; j < actual.heap_count; ++j)
			found = found || actual.heaps[j] == expected.heaps[i];
		if (!found)
		{
			char line[128];
			std::snprintf(line, sizeof(line), "heap: oracle=%p native=%p%s", static_cast<void *>(expected.heaps[i]),
				actual.heap_count > 0 ? static_cast<void *>(actual.heaps[0]) : nullptr,
				actual.heap_count == 0 ? " (none)" : "");
			r.heap_identity.push_back(line);
		}
	}
	return r;
}

// ---- live machinery ----

namespace {

struct Expected
{
	void *list = nullptr;
	std::uint64_t hash = 0;
	std::uint32_t x = 0, y = 0;
	icept::DispatchBindings bindings;
	bool valid = false;
};

thread_local Expected t_expected;

std::atomic<bool> g_enabled{ false };
std::mutex g_mutex;
Summary g_summary;
unsigned g_logged_disagreements = 0;
constexpr unsigned kMaxLoggedDisagreements = 400;
// Per (hash, shape) so a chatty menu pass cannot spend the whole budget before gameplay
// starts (measured: the first 40 lines were two hashes, and 3000 gameplay mismatches went
// unlogged). Keyed on the hash XOR the first kind that differed.
constexpr unsigned kMaxLoggedPerHash = 3;
std::unordered_map<std::uint64_t, unsigned> g_logged_per_hash;
std::uint64_t g_last_native_unknown = 0;

} // namespace

void set_enabled(bool enabled) { g_enabled.store(enabled, std::memory_order_relaxed); }
bool enabled() { return g_enabled.load(std::memory_order_relaxed); }

void publish_expected(void *native_list, std::uint64_t shader_hash, std::uint32_t x, std::uint32_t y,
                      const icept::DispatchBindings &expected)
{
	if (!enabled())
		return;
	if (t_expected.valid)
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		++g_summary.unconsumed;
	}
	t_expected.list = native_list;
	t_expected.hash = shader_hash;
	t_expected.x = x;
	t_expected.y = y;
	t_expected.bindings = expected;
	t_expected.valid = true;
}

bool has_expected(void *native_list)
{
	return enabled() && t_expected.valid && t_expected.list == native_list;
}

bool consume_and_compare(void *native_list, const icept::DispatchBindings &actual,
                         std::uint64_t native_unknown_lookups, const Adjudicator *adj,
                         const char *native_note)
{
	if (!enabled() || !t_expected.valid || t_expected.list != native_list)
		return false;
	Expected e;
	std::swap(e, t_expected);
	t_expected.valid = false;

	const Result r = compare(e.bindings, actual, adj);
	const bool taa = is_known_taa_hash(e.hash);

	std::lock_guard<std::mutex> lock(g_mutex);
	++g_summary.dispatches;
	for (int i = 0; i < kVerdictCount; ++i)
		g_summary.verdicts[i] += r.verdicts[i];
	if (taa)
		++g_summary.taa_dispatches;
	if (r.agree())
	{
		++g_summary.agree;
		if (!r.heap_identity.empty())
			++g_summary.heap_identity;
	}
	else
	{
		if (!r.mismatches.empty()) ++g_summary.mismatch;
		if (!r.unknown.empty()) ++g_summary.unknown;
		if (!r.extra.empty()) ++g_summary.extra;
		if (taa)
			++g_summary.taa_disagree;
		// Which kind led, so one hash with both a mismatch and an unknown gets both logged.
		const std::uint64_t key = e.hash ^ (!r.mismatches.empty() ? 0x1ull : !r.unknown.empty() ? 0x2ull : 0x3ull);
		unsigned &per_hash = g_logged_per_hash[key];
		if (g_logged_disagreements < kMaxLoggedDisagreements && per_hash < kMaxLoggedPerHash)
		{
			++g_logged_disagreements;
			++per_hash;
			STRAY_LOG_WARN("DIFF hash=%016llx %ux%u %s: %zu mismatch, %zu unknown, %zu extra "
				"(native unknown-lookups so far %llu) | oracle srvs=%zu uavs=%zu cbs=%zu | native srvs=%zu uavs=%zu cbs=%zu%s%s%s",
				static_cast<unsigned long long>(e.hash), e.x, e.y, taa ? "TAA" : "dispatch",
				r.mismatches.size(), r.unknown.size(), r.extra.size(),
				static_cast<unsigned long long>(native_unknown_lookups),
				e.bindings.srvs.size(), e.bindings.uavs.size(), e.bindings.constant_buffers.size(),
				actual.srvs.size(), actual.uavs.size(), actual.constant_buffers.size(),
				native_note != nullptr ? " | " : "", native_note != nullptr ? native_note : "",
				g_logged_disagreements == kMaxLoggedDisagreements ? " (last one logged; the summary keeps counting)" : "");
			for (const auto &l : r.mismatches) STRAY_LOG_WARN("  MISMATCH %s", l.c_str());
			for (const auto &l : r.unknown) STRAY_LOG_WARN("  UNKNOWN  %s", l.c_str());
			for (const auto &l : r.extra) STRAY_LOG_WARN("  EXTRA    %s", l.c_str());
			for (const auto &l : r.heap_identity) STRAY_LOG_WARN("  HEAP-ID  %s", l.c_str());
		}
	}
	g_last_native_unknown = native_unknown_lookups;
	return true;
}

Summary summary()
{
	std::lock_guard<std::mutex> lock(g_mutex);
	return g_summary;
}

void log_summary(const char *when)
{
	const Summary s = summary();
	STRAY_LOG_INFO("DIFF SUMMARY [%s] dispatches=%llu agree=%llu mismatch=%llu unknown=%llu extra=%llu "
		"heap-identity-only=%llu unconsumed=%llu | TAA dispatches=%llu disagree=%llu | disagreements=%llu",
		when, static_cast<unsigned long long>(s.dispatches), static_cast<unsigned long long>(s.agree),
		static_cast<unsigned long long>(s.mismatch), static_cast<unsigned long long>(s.unknown),
		static_cast<unsigned long long>(s.extra), static_cast<unsigned long long>(s.heap_identity),
		static_cast<unsigned long long>(s.unconsumed),
		static_cast<unsigned long long>(s.taa_dispatches), static_cast<unsigned long long>(s.taa_disagree),
		static_cast<unsigned long long>(s.dispatches - s.agree));
	// The adjudication, by differing SLOT: which side each convicts.
	char verdicts[320];
	int n = 0;
	for (int i = 0; i < kVerdictCount && n < static_cast<int>(sizeof(verdicts)) - 48; ++i)
		n += std::snprintf(verdicts + n, sizeof(verdicts) - n, "%s%s=%llu", i ? " " : "",
			verdict_name(static_cast<Verdict>(i)), static_cast<unsigned long long>(s.verdicts[i]));
	STRAY_LOG_INFO("DIFF VERDICTS [%s] slots: %s", when, verdicts);
}

} // namespace stray_dlss::diff
