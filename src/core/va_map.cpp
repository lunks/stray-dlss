#include "core/va_map.hpp"

namespace stray_dlss::core {

void VaMap::insert(std::uint64_t start, std::uint64_t size, std::uint64_t id)
{
	if (size == 0)
		return;
	m_ranges.insert_or_assign(start, Range{ size, id });
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
	// The first range starting AFTER va, then step back to the one starting at or before it.
	auto it = m_ranges.upper_bound(va);
	if (it == m_ranges.begin())
		return false;
	--it;
	if (va >= it->first + it->second.size)
		return false;
	id = it->second.id;
	offset = va - it->first;
	return true;
}

} // namespace stray_dlss::core
