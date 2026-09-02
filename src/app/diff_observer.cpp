#include "app/diff_observer.hpp"

#include "core/taa_hashes.hpp"
#include "log.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <mutex>

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

void compare_slots(const char prefix, const std::vector<BoundTexture> &expected,
                   const std::vector<BoundTexture> &actual, Result &out)
{
	for (const BoundTexture &e : expected)
	{
		const auto a = std::find_if(actual.begin(), actual.end(),
			[&](const BoundTexture &t) { return t.slot == e.slot; });
		char line[256];
		if (a == actual.end())
		{
			std::snprintf(line, sizeof(line), "%c%u: oracle=(%s) native=UNKNOWN", prefix, e.slot, describe(e).c_str());
			out.unknown.push_back(line);
			continue;
		}
		if (a->resource != e.resource || a->format != e.format || a->width != e.width ||
			a->height != e.height || a->is_3d != e.is_3d)
		{
			std::snprintf(line, sizeof(line), "%c%u: oracle=(%s) native=(%s)", prefix, e.slot,
				describe(e).c_str(), describe(*a).c_str());
			out.mismatches.push_back(line);
		}
	}
	for (const BoundTexture &a : actual)
	{
		const bool known = std::any_of(expected.begin(), expected.end(),
			[&](const BoundTexture &e) { return e.slot == a.slot; });
		if (!known)
		{
			char line[256];
			std::snprintf(line, sizeof(line), "%c%u: oracle=ABSENT native=(%s)", prefix, a.slot, describe(a).c_str());
			out.extra.push_back(line);
		}
	}
}

} // namespace

Result compare(const icept::DispatchBindings &expected, const icept::DispatchBindings &actual)
{
	Result r;
	compare_slots('t', expected.srvs, actual.srvs, r);
	compare_slots('u', expected.uavs, actual.uavs, r);

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
		char line[160];
		if (it == have.end())
		{
			std::snprintf(line, sizeof(line), "cb: oracle=(buf %llx +%llu) native=UNKNOWN",
				static_cast<unsigned long long>(w.buffer), static_cast<unsigned long long>(w.offset));
			r.unknown.push_back(line);
		}
		else
		{
			have.erase(it);
		}
	}
	for (const auto &h : have)
	{
		char line[160];
		std::snprintf(line, sizeof(line), "cb: oracle=ABSENT native=(buf %llx +%llu)",
			static_cast<unsigned long long>(h.buffer), static_cast<unsigned long long>(h.offset));
		r.extra.push_back(line);
	}

	if (expected.view_cb_valid || actual.view_cb_valid)
	{
		if (expected.view_cb_valid != actual.view_cb_valid ||
			expected.view_cb.buffer != actual.view_cb.buffer ||
			expected.view_cb.offset != actual.view_cb.offset ||
			expected.view_cb_register != actual.view_cb_register)
		{
			char line[200];
			std::snprintf(line, sizeof(line), "view_cb: oracle=(valid %d buf %llx +%llu b%u) native=(valid %d buf %llx +%llu b%u)",
				expected.view_cb_valid ? 1 : 0, static_cast<unsigned long long>(expected.view_cb.buffer),
				static_cast<unsigned long long>(expected.view_cb.offset), expected.view_cb_register,
				actual.view_cb_valid ? 1 : 0, static_cast<unsigned long long>(actual.view_cb.buffer),
				static_cast<unsigned long long>(actual.view_cb.offset), actual.view_cb_register);
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
constexpr unsigned kMaxLoggedDisagreements = 40;
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
                         std::uint64_t native_unknown_lookups)
{
	if (!enabled() || !t_expected.valid || t_expected.list != native_list)
		return false;
	Expected e;
	std::swap(e, t_expected);
	t_expected.valid = false;

	const Result r = compare(e.bindings, actual);
	const bool taa = is_known_taa_hash(e.hash);

	std::lock_guard<std::mutex> lock(g_mutex);
	++g_summary.dispatches;
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
		if (g_logged_disagreements < kMaxLoggedDisagreements)
		{
			++g_logged_disagreements;
			STRAY_LOG_WARN("DIFF hash=%016llx %ux%u %s: %zu mismatch, %zu unknown, %zu extra "
				"(native unknown-lookups so far %llu)%s",
				static_cast<unsigned long long>(e.hash), e.x, e.y, taa ? "TAA" : "dispatch",
				r.mismatches.size(), r.unknown.size(), r.extra.size(),
				static_cast<unsigned long long>(native_unknown_lookups),
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
}

} // namespace stray_dlss::diff
