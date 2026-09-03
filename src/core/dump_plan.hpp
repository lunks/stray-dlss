// WHEN the NGX input dump fires. Pure; tested.
//
// WHY THIS IS ITS OWN MODULE. The dump is a once-per-session instrument that costs a whole
// round trip to the user's machine, and the only thing that decides whether it captures
// anything useful is an integer: which evaluate counts it triggers on. The shipped pair (600
// and 900) was chosen in 2026-08-31 for a session that reached gameplay quickly; a run that is
// still on a loading screen at evaluate 600 dumps a black frame and answers nothing, which is
// the same trap CLAUDE.md records for `NR CODEC LUMINANCE` (§5, "must not run during a loading
// screen"). Making the point configurable turns a wasted run into a retry, and keeping the
// arithmetic here means CI pins it rather than the box discovering it.
//
// The second point exists so a dump can be COMPARED against itself: two captures of the same
// input with identical bytes prove a stale texture, which is exactly how the ext-vtable
// no-write bug was convicted (CLAUDE.md §1).
#pragma once

#include <cstdint>

namespace stray_dlss::core {

// The two evaluate counts at which every dumped resource is captured.
struct DumpPlan
{
	std::uint64_t first = 0;
	std::uint64_t second = 0;
};

// Shipped points, unchanged since 2026-08-31: steady-state gameplay moments far enough apart
// that a live scene colour must differ between them.
inline constexpr std::uint64_t kDefaultFirstDumpPoint = 600;

// Gap between the two captures, in evaluates. At ~55 fps that is a little over five seconds —
// long enough that a stale texture cannot alias as a still scene.
inline constexpr std::uint64_t kDumpPointGap = 300;

// `configured` is [STRAYDLSS] NgxDumpAt: 0 (or anything negative) keeps the shipped points, so
// an absent key can never move them. A configured value is the FIRST point and the second
// follows a fixed gap later, because "dump twice, far apart" is the property that matters and
// asking a user for two numbers through an ini that cannot carry a list (CLAUDE.md §5) buys
// nothing.
constexpr DumpPlan plan_dump_points(int configured)
{
	DumpPlan p;
	p.first = configured > 0 ? static_cast<std::uint64_t>(configured) : kDefaultFirstDumpPoint;
	p.second = p.first + kDumpPointGap;
	return p;
}

constexpr bool dump_wants(const DumpPlan &plan, std::uint64_t evaluate_count)
{
	return evaluate_count == plan.first || evaluate_count == plan.second;
}

} // namespace stray_dlss::core
