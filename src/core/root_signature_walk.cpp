#include "core/root_signature_walk.hpp"

namespace stray_dlss::core {

bool walk_table(const RootLayout &layout, std::uint32_t param, WalkResult &out)
{
	if (param >= layout.params.size() || layout.params[param].kind != ParamKind::table)
		return false;

	std::uint32_t next = 0; // where an APPEND range lands
	for (const RootRange &range : layout.params[param].ranges)
	{
		const std::uint32_t start = range.offset_in_table == kRangeOffsetAppend ? next : range.offset_in_table;
		if (range.count == kUnboundedCount)
		{
			++out.unbounded_ranges;
			// An unbounded range is always last in a table (D3D12 requires it), so `next`
			// no longer matters; nothing after it can be expanded either.
			break;
		}
		for (std::uint32_t i = 0; i < range.count; ++i)
		{
			TableSlot s;
			s.param = param;
			s.table_index = start + i;
			s.kind = range.kind;
			s.reg = range.base_register + i;
			s.space = range.space;
			out.slots.push_back(s);
		}
		next = start + range.count;
	}
	return true;
}

WalkResult walk_tables(const RootLayout &layout)
{
	WalkResult out;
	for (std::uint32_t p = 0; p < layout.params.size(); ++p)
		walk_table(layout, p, out);
	return out;
}

} // namespace stray_dlss::core
