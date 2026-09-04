// Finding a NON-REFLECTED native member by what it CONTAINS, rather than by disassembly.
//
// THE PROBLEM, measured on the box 2026-09-04. Stray's Screen Percentage row offers 50..200 in
// steps of ten, and we want it to offer the five DLSS ratios. Reflection is exhausted:
// `GraphicsSettingsWidget` and `ListBoxWidget` expose properties and ZERO UFunctions between
// them, `ListBoxWidget` holds no list and no index at all, and the one screen-percentage-shaped
// property — `m_screenPercentages`, a TSet<uint32> — is provably NOT what the row reads: a page
// constructed from a class default object carrying our five values still offered the old
// sixteen (docs/RESEARCH-STRAY-MENU-OPTIONS.md, and its sequel).
//
// So the list lives in native state with no UPROPERTY. UE4SS can still reach such a member —
// `RegisterCustomProperty` declares a property at a byte OFFSET — but only if we know the
// offset. This module finds it the empirical way: search the object's own memory for the values
// we already know are in there.
//
// WHY THIS IS A BOUNDED SEARCH AND NOT A BLIND MEMORY SWEEP, which matters because a wrong read
// in this process is an access violation and this project has already paid for two today:
//
//   * The UPPER BOUND is the engine's own: `UStruct::GetPropertiesSize()` is the instance size
//     of the class, so [base, base+size) is exactly the object and nothing past it is touched.
//   * The EXCLUSIONS are the engine's own too: every reflected property's
//     `GetOffset_Internal()` and `GetSize()` marks a region already accounted for. A hit inside
//     one of those is a coincidence in known data, not a discovery, and is dropped.
//   * The NEEDLE is specific. Sixteen consecutive uint32 reading 50,60,...,200 is not a pattern
//     that occurs by chance in a few kilobytes of object.
//
// Two shapes are searched for, because the list may be stored either way:
//
//   INLINE   the values sit in the object, one after another (a fixed C array member).
//   TARRAY   the object holds a TArray header — { T* Data; int32 Num; int32 Max; } — and the
//            values are in a heap allocation it points at. We cannot follow the pointer here
//            (this module is pure and takes only bytes), so a header whose Num matches the
//            expected count and whose Max is at least Num is reported as a CANDIDATE for the
//            caller to dereference under SEH.
//
// Pure on purpose: the caller supplies bytes, the engine supplies the bound and the exclusions,
// and CI proves the search itself. Nothing here dereferences anything.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace stray_dlss::menuscan {

// A region of the instance already described by a reflected property.
struct Range
{
	std::uint32_t offset = 0;
	std::uint32_t size = 0;
};

enum class HitKind
{
	// The needle's values were found laid out consecutively at this offset.
	inline_values,
	// A TArray-shaped header whose Num equals the expected count. The caller must dereference
	// `data_ptr` to confirm; this module never does.
	array_header,
};

struct Hit
{
	std::uint32_t offset = 0;
	HitKind kind = HitKind::inline_values;
	std::uint32_t count = 0;
	// array_header only: the pointer the header carries, for the caller to check and follow.
	std::uint64_t data_ptr = 0;
	std::uint32_t capacity = 0;
	// True when this offset lies inside a region a reflected property already owns. Kept
	// rather than dropped so the log can say "found, but it is m_screenPercentages" — which is
	// exactly the answer we expect for the TSet and is evidence the search works.
	bool inside_reflected = false;
};

// Is `offset` inside any excluded range?
bool overlaps(const std::vector<Range> &excluded, std::uint32_t offset, std::uint32_t size);

// Search `bytes` for the needle in both shapes. `expected_count` is the needle length for the
// TARRAY shape; a header is a candidate when Num == expected_count and Max >= Num.
//
// Alignment: uint32 members are 4-byte aligned and TArray headers 8-byte aligned in any sane
// layout, so both searches step by 4. That is a deliberate restriction — scanning every byte
// offset would multiply false positives for no gain.
std::vector<Hit> scan(const std::uint8_t *bytes, std::size_t size,
                      const std::uint32_t *needle, std::size_t needle_len,
                      const std::vector<Range> &excluded);

// A pointer plausible enough to be worth dereferencing under SEH: non-null, 8-aligned, and
// inside the range user-mode heap allocations actually occupy on win64. This only decides
// whether to TRY a read; the read itself is still guarded by the caller.
bool plausible_heap_ptr(std::uint64_t p);

} // namespace stray_dlss::menuscan
