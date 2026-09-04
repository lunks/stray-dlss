#include "MenuScan.hpp"

#include "Host.hpp"
#include "core/menu_scan.hpp"
#include "host/config.hpp"
#include "log.hpp"

#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace stray_menu_scan {
namespace {

// The shipped Screen Percentage values, MEASURED on the box 2026-09-04 by reading the TSet
// m_screenPercentages through UE4SS's ForEach: sixteen entries, 50..200 in steps of ten.
// This is the needle precisely because we already know it is in there.
const std::uint32_t kNeedle[] = { 50, 60, 70, 80, 90, 100, 110, 120,
                                  130, 140, 150, 160, 170, 180, 190, 200 };
constexpr std::size_t kNeedleLen = sizeof(kNeedle) / sizeof(kNeedle[0]);

bool g_done = false;
int g_attempts = 0;

// THE WATCH, and it is deliberately ONE object and ONE range.
//
// Offline analysis of the game's own UHT descriptors (docs/RESEARCH-STRAY-MENU-SCREENPCT.md)
// bounds the non-reflected region of GraphicsSettingsWidget to 0x4B0..0x530 - class size minus
// the last reflected property, so it is arithmetic rather than a guess. That analysis and this
// file's own runtime scan agree independently on sizeof = 1488 and on m_screenPercentages at
// +0x580, which is why both numbers are trusted.
//
// The scan answered "where are the sixteen values" with "nowhere reflected". This answers what
// that leaves: WHICH BYTES MOVE when the row is operated. Sample once a second; the bytes that
// change when the user presses left/right are the row's index.
//
// SCOPED DOWN ON PURPOSE. An earlier draft also watched three ListBoxWidget rows, which meant
// looking each one up by property NAME - and UE4SS's own docs give conflicting answers for
// whether FProperty::GetName() returns FName or FString at this SHA. One object and one range
// needs no name lookup at all, so that ambiguity cannot bite. If the page's region turns out
// not to hold the index, adding the rows is a second, informed step.
//
// THREADING, stated honestly rather than assumed away: on_update is UE4SS's own ~200 Hz
// jthread, NOT the game thread (Mod.cpp, from UE4SSProgram.cpp:1205,1322-1341). Reading a live
// UObject from it can give a TORN sample. For a diagnostic that is a spurious log line, not a
// wrong conclusion - we are looking for bytes that change consistently across a button press.
// The real hazard is the widget being freed when the menu closes, and the SEH guard turns that
// into a caught fault that drops the watch rather than taking the process down. This would NOT
// be acceptable in a path that ACTED on the result.
constexpr std::uint32_t kPageWatchBegin = 0x4B0, kPageWatchEnd = 0x530;

// Defined below, next to the other guarded read; poll_watch calls it.
bool copy_object_bytes(const void *base, std::size_t size, std::vector<std::uint8_t> &out);

const void *g_watch_base = nullptr;
std::vector<std::uint8_t> g_watch_last;
bool g_watch_primed = false;
int g_poll_divider = 0;
int g_changes = 0;

std::string hex_of(const std::vector<std::uint8_t> &b, std::size_t from, std::size_t len)
{
	static const char *d = "0123456789ABCDEF";
	std::string s;
	s.reserve(len * 3);
	for (std::size_t i = from; i < from + len && i < b.size(); ++i)
	{
		s.push_back(d[b[i] >> 4]);
		s.push_back(d[b[i] & 0xF]);
		s.push_back(' ');
	}
	return s;
}

// One sample, diffed against the last. Only the runs that actually moved are printed: a full
// 128-byte dump every second would bury the one line that matters.
void poll_watch()
{
	if (g_watch_base == nullptr)
		return;
	const std::size_t len = kPageWatchEnd - kPageWatchBegin;
	std::vector<std::uint8_t> now;
	if (!copy_object_bytes(static_cast<const std::uint8_t *>(g_watch_base) + kPageWatchBegin,
			len, now))
	{
		STRAY_LOG_WARN("menu-scan: watch read FAULTED (the page was probably freed when the menu "
			"closed). Watch dropped; reopen Options -> Graphics to re-arm.");
		g_watch_base = nullptr;
		g_watch_primed = false;
		return;
	}
	if (!g_watch_primed)
	{
		g_watch_last = now;
		g_watch_primed = true;
		STRAY_LOG_WARN("menu-scan: watch primed over 0x%X..0x%X. Press LEFT/RIGHT on Screen "
			"Percentage now.", kPageWatchBegin, kPageWatchEnd);
		return;
	}
	if (now == g_watch_last)
		return;

	++g_changes;
	for (std::size_t i = 0; i < len;)
	{
		if (now[i] == g_watch_last[i]) { ++i; continue; }
		std::size_t j = i;
		while (j < len && now[j] != g_watch_last[j]) ++j;
		STRAY_LOG_WARN("menu-scan CHANGE #%d at +0x%X (%zu bytes): %s-> %s", g_changes,
			static_cast<unsigned>(kPageWatchBegin + i), j - i,
			hex_of(g_watch_last, i, j - i).c_str(), hex_of(now, i, j - i).c_str());
		i = j;
	}
	g_watch_last = now;
}

// Reading a live object's bytes cannot fault on a correct size, but "correct" here rests on
// GetPropertiesSize() being the real instance size, which is an assumption about a shipped
// game's reflection data. SEH costs nothing and turns a wrong assumption into a logged refusal
// instead of taking the process down - the discipline the two crashes today earned.
bool copy_object_bytes(const void *base, std::size_t size, std::vector<std::uint8_t> &out)
{
	__try
	{
		out.assign(static_cast<const std::uint8_t *>(base),
			static_cast<const std::uint8_t *>(base) + size);
		return true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return false;
	}
}

// Following a TArray header's pointer. Separately guarded because this one genuinely can fault:
// the "pointer" is bytes that merely looked like one.
bool read_u32_array(std::uint64_t addr, std::uint32_t count, std::vector<std::uint32_t> &out)
{
	__try
	{
		const std::uint32_t *p = reinterpret_cast<const std::uint32_t *>(addr);
		out.assign(p, p + count);
		return true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return false;
	}
}

} // namespace

void Tick()
{
	if (!stray_dlss::host::cfg::get_bool("MenuScan", false))
		return;

	if (g_done)
	{
		if (++g_poll_divider < 200)
			return;
		g_poll_divider = 0;
		poll_watch();
		return;
	}

	using namespace RC::Unreal;

	// The page only exists while the user has the settings menu open, so this retries until it
	// does - but not forever, because a scan that never finds its object is a result too.
	UObject *page = UObjectGlobals::FindFirstOf(STR("GraphicsSettingsWidget"));
	if (page == nullptr)
	{
		if (++g_attempts % 600 == 0)
			STRAY_LOG_INFO("menu-scan: waiting for a GraphicsSettingsWidget (open Options -> "
				"Graphics). attempts=%d", g_attempts);
		if (g_attempts > 60000)
		{
			STRAY_LOG_WARN("menu-scan: gave up; no GraphicsSettingsWidget was ever constructed.");
			g_done = true;
		}
		return;
	}

	UClass *cls = page->GetClassPrivate();
	if (cls == nullptr)
	{
		STRAY_LOG_ERROR("menu-scan: the page has no class; refusing to read it.");
		g_done = true;
		return;
	}

	const std::int32_t props_size = cls->GetPropertiesSize();
	if (props_size <= 0 || props_size > (4 << 20))
	{
		STRAY_LOG_ERROR("menu-scan: GetPropertiesSize() = %d, which is not a believable instance "
			"size. Refusing to read - the BOUND is the whole reason this is safe.", props_size);
		g_done = true;
		return;
	}

	// THE EXCLUSIONS. Every reflected property, including inherited ones, marks a region whose
	// contents we can already explain. A needle hit inside one of these is m_screenPercentages
	// (or another known property) rather than a discovery - and we WANT to see that reported,
	// because it proves the search works.
	std::vector<stray_dlss::menuscan::Range> excluded;
	std::size_t prop_count = 0;
	for (FProperty *prop : TFieldRange<FProperty>(cls, EFieldIterationFlags::IncludeSuper))
	{
		if (prop == nullptr)
			continue;
		++prop_count;
		stray_dlss::menuscan::Range r;
		r.offset = static_cast<std::uint32_t>(prop->GetOffset_Internal());
		r.size = static_cast<std::uint32_t>(prop->GetSize());
		excluded.push_back(r);
	}

	std::vector<std::uint8_t> bytes;
	if (!copy_object_bytes(page, static_cast<std::size_t>(props_size), bytes))
	{
		STRAY_LOG_ERROR("menu-scan: reading the object's %d bytes FAULTED. GetPropertiesSize() "
			"is not the instance size on this build; the scan is off for the session.", props_size);
		g_done = true;
		return;
	}

	STRAY_LOG_WARN("menu-scan: GraphicsSettingsWidget at %p, instance size %d, %zu reflected "
		"properties excluded. Searching for the sixteen shipped screen percentages.",
		static_cast<void *>(page), props_size, prop_count);

	const std::vector<stray_dlss::menuscan::Hit> hits =
		stray_dlss::menuscan::scan(bytes.data(), bytes.size(), kNeedle, kNeedleLen, excluded);

	if (hits.empty())
		STRAY_LOG_WARN("menu-scan: NOTHING FOUND. The values are not stored in this object as a "
			"plain uint32 run or a TArray<uint32> header - so the row's list is somewhere else "
			"entirely (another object, a different element type, or built on demand).");

	for (const stray_dlss::menuscan::Hit &h : hits)
	{
		if (h.kind == stray_dlss::menuscan::HitKind::inline_values)
		{
			STRAY_LOG_WARN("menu-scan: INLINE run of %u values at offset +0x%X (%u). "
				"insideReflectedProperty=%d%s", h.count, h.offset, h.offset,
				h.inside_reflected ? 1 : 0,
				h.inside_reflected
					? " - this is a property we already know about, which is the expected"
					  " control result rather than the find."
					: " - THIS IS THE ONE: an offset no reflected property explains.");
			continue;
		}

		// A TArray candidate is only a candidate until its pointer is followed and the values
		// come back right. This is the one dereference of an address that came out of a byte
		// search, hence the separate guard.
		std::vector<std::uint32_t> vals;
		const bool ok = read_u32_array(h.data_ptr, h.count, vals);
		bool matches = ok && vals.size() == kNeedleLen;
		if (matches)
			for (std::size_t i = 0; i < kNeedleLen; ++i)
				if (vals[i] != kNeedle[i])
					matches = false;

		STRAY_LOG_WARN("menu-scan: TARRAY header at offset +0x%X (%u): Data=%016llX Num=%u "
			"Max=%u insideReflectedProperty=%d deref=%s contentsMatch=%d%s",
			h.offset, h.offset, static_cast<unsigned long long>(h.data_ptr), h.count,
			h.capacity, h.inside_reflected ? 1 : 0, ok ? "ok" : "FAULTED", matches ? 1 : 0,
			(matches && !h.inside_reflected)
				? " - THIS IS THE ONE: a TArray of the shipped values at an offset no reflected"
				  " property explains. RegisterCustomProperty can reach it."
				: "");
	}

	g_watch_base = page;
	g_watch_primed = false;
	STRAY_LOG_WARN("menu-scan: scan done, it changed nothing. WATCH ARMED on the page's "
		"non-reflected region 0x%X..0x%X, sampling once a second.",
		kPageWatchBegin, kPageWatchEnd);
	g_done = true;
}

} // namespace stray_menu_scan
