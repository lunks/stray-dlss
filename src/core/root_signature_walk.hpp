// A root signature as plain data, and the walk that turns "table parameter P, descriptor
// slot i" into a shader register. Pure — the D3D12 deserializer's output is converted into
// these structs by the native backend (Windows), and the walk itself is CI-tested on Linux.
//
// The rules are D3D12's (HARD): ranges within a table occupy consecutive descriptors; an
// OffsetInDescriptorsFromTableStart of D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND (0xffffffff)
// means "immediately after the previous range"; a NumDescriptors of UINT32_MAX is unbounded
// and is reported, never expanded (the old frame_state.cpp:512-513 behaviour — UE4 does not
// use them on these passes).
#pragma once

#include <cstdint>
#include <vector>

namespace stray_dlss::core {

enum class RangeKind : std::uint8_t { srv, uav, cbv, sampler };
enum class ParamKind : std::uint8_t { table, cbv, srv, uav, constants };

constexpr std::uint32_t kRangeOffsetAppend = 0xffffffffu; // D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND
constexpr std::uint32_t kUnboundedCount = 0xffffffffu;

struct RootRange
{
	RangeKind kind = RangeKind::srv;
	std::uint32_t base_register = 0;
	std::uint32_t count = 0;
	std::uint32_t space = 0;
	std::uint32_t offset_in_table = kRangeOffsetAppend;
};

struct RootParam
{
	ParamKind kind = ParamKind::table;
	std::uint32_t shader_register = 0; // root descriptors and constants
	std::uint32_t space = 0;
	std::uint32_t num_32bit_values = 0; // constants
	std::vector<RootRange> ranges;      // tables
};

struct RootLayout
{
	std::vector<RootParam> params;
};

// One descriptor slot of a table, resolved.
struct TableSlot
{
	std::uint32_t param = 0;        // root parameter index
	std::uint32_t table_index = 0;  // descriptor index from the table start
	RangeKind kind = RangeKind::srv;
	std::uint32_t reg = 0;          // shader register (base + i)
	std::uint32_t space = 0;
};

struct WalkResult
{
	std::vector<TableSlot> slots;
	std::uint32_t unbounded_ranges = 0; // seen and skipped
};

// Expands every table parameter of `layout` into its slots. Non-table parameters contribute
// nothing here; callers read them off `layout.params` directly.
WalkResult walk_tables(const RootLayout &layout);

// Expands ONE table parameter; false if `param` is not a table. Same rules.
bool walk_table(const RootLayout &layout, std::uint32_t param, WalkResult &out);

} // namespace stray_dlss::core
