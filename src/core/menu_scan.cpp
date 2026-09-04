#include "core/menu_scan.hpp"

#include <cstring>

namespace stray_dlss::menuscan {

namespace {

std::uint32_t read_u32(const std::uint8_t *p)
{
	std::uint32_t v = 0;
	std::memcpy(&v, p, sizeof(v));
	return v;
}

std::uint64_t read_u64(const std::uint8_t *p)
{
	std::uint64_t v = 0;
	std::memcpy(&v, p, sizeof(v));
	return v;
}

} // namespace

bool overlaps(const std::vector<Range> &excluded, std::uint32_t offset, std::uint32_t size)
{
	// Half-open [offset, offset+size) against each [r.offset, r.offset+r.size). A zero-sized
	// range covers nothing — a reflected property with size 0 tells us nothing about layout.
	for (const Range &r : excluded)
	{
		if (r.size == 0)
			continue;
		const std::uint64_t a0 = offset, a1 = static_cast<std::uint64_t>(offset) + size;
		const std::uint64_t b0 = r.offset, b1 = static_cast<std::uint64_t>(r.offset) + r.size;
		if (a0 < b1 && b0 < a1)
			return true;
	}
	return false;
}

bool plausible_heap_ptr(std::uint64_t p)
{
	if (p == 0)
		return false;
	if ((p & 7u) != 0)
		return false; // a T* array base is at least 8-aligned
	// Win64 user mode is the low 128 TiB. Below 64 KiB is the null page, which is never mapped.
	return p >= 0x10000ull && p < 0x0000800000000000ull;
}

std::vector<Hit> scan(const std::uint8_t *bytes, std::size_t size,
                      const std::uint32_t *needle, std::size_t needle_len,
                      const std::vector<Range> &excluded)
{
	std::vector<Hit> hits;
	if (bytes == nullptr || needle == nullptr || needle_len == 0)
		return hits;

	// ---- INLINE: needle_len consecutive uint32 equal to the needle. ----
	const std::size_t needle_bytes = needle_len * sizeof(std::uint32_t);
	if (size >= needle_bytes)
	{
		for (std::size_t off = 0; off + needle_bytes <= size; off += 4)
		{
			bool match = true;
			for (std::size_t i = 0; i < needle_len; ++i)
			{
				if (read_u32(bytes + off + i * sizeof(std::uint32_t)) != needle[i])
				{
					match = false;
					break;
				}
			}
			if (!match)
				continue;
			Hit h;
			h.offset = static_cast<std::uint32_t>(off);
			h.kind = HitKind::inline_values;
			h.count = static_cast<std::uint32_t>(needle_len);
			h.inside_reflected =
				overlaps(excluded, h.offset, static_cast<std::uint32_t>(needle_bytes));
			hits.push_back(h);
		}
	}

	// ---- TARRAY: { T* Data; int32 Num; int32 Max; }, 16 bytes, 8-aligned. ----
	//
	// Num must equal what we expect and Max must be at least Num — an array whose capacity is
	// below its count is not an array, it is two unrelated integers that happened to line up.
	if (size >= 16)
	{
		for (std::size_t off = 0; off + 16 <= size; off += 8)
		{
			const std::uint64_t data = read_u64(bytes + off);
			const std::uint32_t num = read_u32(bytes + off + 8);
			const std::uint32_t max = read_u32(bytes + off + 12);
			if (num != needle_len || max < num)
				continue;
			if (!plausible_heap_ptr(data))
				continue;
			Hit h;
			h.offset = static_cast<std::uint32_t>(off);
			h.kind = HitKind::array_header;
			h.count = num;
			h.data_ptr = data;
			h.capacity = max;
			h.inside_reflected = overlaps(excluded, h.offset, 16);
			hits.push_back(h);
		}
	}

	return hits;
}

} // namespace stray_dlss::menuscan
