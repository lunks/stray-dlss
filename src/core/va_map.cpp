#include "core/va_map.hpp"

namespace stray_dlss::core {

void VaMap::insert(std::uint64_t start, std::uint64_t size, std::uint64_t id)
{
	if (size == 0)
		return;
	m_ranges.insert_or_assign(start, Range{ size, id });
	if (size > m_max_size)
		m_max_size = size;
}

void VaMap::erase(std::uint64_t id)
{
	for (auto it = m_ranges.begin(); it != m_ranges.end();)
	{
		if (it->second.id == id)
			it = m_ranges.erase(it);
		else
			++it;
	}
}

bool VaMap::find(std::uint64_t va, std::uint64_t &id, std::uint64_t &offset) const
{
	// The first range starting AFTER va, then walk BACK over the ranges starting at or before
	// it. The first one that contains va is the innermost. No range starting more than
	// m_max_size before va can contain it, which bounds the walk without a second index.
	auto it = m_ranges.upper_bound(va);
	while (it != m_ranges.begin())
	{
		--it;
		if (va < it->first + it->second.size)
		{
			id = it->second.id;
			offset = va - it->first;
			return true;
		}
		if (it->first + m_max_size <= va)
			break;
	}
	return false;
}

} // namespace stray_dlss::core
