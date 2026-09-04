#include "MenuScan.hpp"

#include "Host.hpp"
#include "core/menu_scan.hpp"
#include "host/config.hpp"
#include "log.hpp"

#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UClass.hpp>
#include <Unreal/FProperty.hpp>
#include <Unreal/FField.hpp>

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
	if (g_done)
		return;
	if (!host::cfg::get_bool("MenuScan", false))
		return;

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

	STRAY_LOG_WARN("menu-scan: done. It changed nothing; it only looked.");
	g_done = true;
}

} // namespace stray_menu_scan
