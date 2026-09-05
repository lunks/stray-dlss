#include "backend_native/backbuffer_state.hpp"

#include "log.hpp"

#include <d3d12.h>

#include <atomic>
#include <cstdio>
#include <mutex>
#include <unordered_set>

namespace stray_dlss::native::bbstate {
namespace {

std::mutex g_mutex;
std::unordered_set<std::uint64_t> g_own_lists; // present owner + FG lists: not the engine (hpp)
fseam::Level g_level = fseam::Level::off;
fseam::Candidates g_candidates;
fseam::StateLedger g_ledger;
Stats g_stats;

// g_mutex held. True (and counted) when `list` was registered by mark_own_list.
bool is_own_list_locked(const void *list)
{
	if (g_own_lists.empty() || g_own_lists.count(reinterpret_cast<std::uint64_t>(list)) == 0)
		return false;
	++g_stats.own_skipped;
	return true;
}
bool g_disagree_logged = false;
bool g_unknown_logged = false;
bool g_not_tracked_logged = false;
bool g_mismatch_logged = false;
bool g_first_present_logged = false;

} // namespace

void configure(int level)
{
	std::lock_guard<std::mutex> lock(g_mutex);
	g_level = fseam::level_from_int(level);
	STRAY_LOG_INFO("BACK-BUFFER STATE MODE: %s ([STRAYDLSS] EngineSeamBackBufferState=%d). %s",
		fseam::level_name(g_level), level,
		g_level == fseam::Level::off ? "Nothing is recorded; the NR stage names its assumed constant."
		: g_level == fseam::Level::observe
			? "The engine's ResourceBarrier calls on the swapchain-class resources are replayed in ExecuteCommandLists "
			  "order and ASSERTED against the assumed constant at every present the NR stage runs; the constant "
			  "is still what the stage names. Read `[bbstate]`: verdict disagree/unknown/notTracked and beforeMismatch "
			  "must stay 0."
			: "The recorded state is what the NR stage names (UNCONFIRMED on the box); the assumed constant is the "
			  "fallback while the engine has not transitioned the resource. Read `[bbstate]`: usedRecorded should "
			  "track the stage's applied count, usedAssumed should stay near 0.");
}

fseam::Level level()
{
	std::lock_guard<std::mutex> lock(g_mutex);
	return g_level;
}

void set_candidates(const std::uint64_t *res, unsigned n, const char *why)
{
	std::lock_guard<std::mutex> lock(g_mutex);
	g_candidates.set(res, n, g_candidates.epoch + 1);
	g_ledger.set_tracked(g_candidates);
	g_stats.candidates = g_candidates.count;
	g_stats.candidate_epoch = g_candidates.epoch;
	if (g_level != fseam::Level::off)
		STRAY_LOG_INFO("BACK-BUFFER STATE: tracking %u swapchain-class resource(s) (%s), epoch %llu, first %p. Every "
			"state is unknown until the engine transitions it.",
			g_candidates.count, why != nullptr ? why : "?", static_cast<unsigned long long>(g_candidates.epoch),
			reinterpret_cast<void *>(g_candidates.count != 0 ? g_candidates.res[0] : 0));
}

void clear_candidates(const char *why)
{
	set_candidates(nullptr, 0, why);
}

fseam::Candidates candidates()
{
	std::lock_guard<std::mutex> lock(g_mutex);
	return g_candidates;
}

bool is_candidate(std::uint64_t res)
{
	std::lock_guard<std::mutex> lock(g_mutex);
	return g_candidates.contains(res);
}

void on_barriers(ID3D12GraphicsCommandList *list, unsigned n, const D3D12_RESOURCE_BARRIER *barriers)
{
	if (list == nullptr || barriers == nullptr || n == 0)
		return;
	std::lock_guard<std::mutex> lock(g_mutex);
	if (g_level == fseam::Level::off || g_candidates.count == 0)
		return;
	if (is_own_list_locked(list))
		return;
	const auto lid = reinterpret_cast<std::uint64_t>(list);
	for (unsigned i = 0; i < n; ++i)
	{
		const D3D12_RESOURCE_BARRIER &b = barriers[i];
		if (b.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION)
			continue;
		const auto res = reinterpret_cast<std::uint64_t>(b.Transition.pResource);
		if (!g_candidates.contains(res))
			continue;
		++g_stats.barriers_seen;
		g_ledger.on_barrier(lid, res, static_cast<std::uint32_t>(b.Transition.StateBefore),
			static_cast<std::uint32_t>(b.Transition.StateAfter));
	}
}

void on_list_reset(ID3D12GraphicsCommandList *list)
{
	if (list == nullptr)
		return;
	std::lock_guard<std::mutex> lock(g_mutex);
	if (g_level == fseam::Level::off)
		return;
	if (is_own_list_locked(list))
		return;
	g_ledger.on_reset(reinterpret_cast<std::uint64_t>(list));
}

void mark_own_list(ID3D12GraphicsCommandList *list)
{
	if (list == nullptr)
		return;
	std::lock_guard<std::mutex> lock(g_mutex);
	if (g_own_lists.insert(reinterpret_cast<std::uint64_t>(list)).second)
		g_stats.own_lists = static_cast<unsigned>(g_own_lists.size());
}

void on_execute(unsigned n, ID3D12CommandList *const *lists)
{
	if (lists == nullptr || n == 0)
		return;
	std::lock_guard<std::mutex> lock(g_mutex);
	if (g_level == fseam::Level::off)
		return;
	++g_stats.executes;
	bool log_mismatch = false;
	std::uint64_t mismatch_res = 0;
	for (unsigned i = 0; i < n; ++i)
	{
		if (lists[i] == nullptr)
			continue;
		if (is_own_list_locked(lists[i]))
			continue;
		++g_stats.lists_executed;
		const fseam::ExecuteResult r = g_ledger.on_execute(reinterpret_cast<std::uint64_t>(lists[i]));
		g_stats.applied += r.applied;
		g_stats.before_mismatch += r.before_mismatch;
		g_stats.markers_ok += r.markers_ok;
		g_stats.markers_bad += r.markers_bad;
		if (r.present_res != 0)
			++g_stats.present_seen;
		if (r.before_mismatch != 0 && !g_mismatch_logged)
		{
			g_mismatch_logged = true;
			log_mismatch = true;
			mismatch_res = r.present_res;
		}
	}
	if (log_mismatch)
		STRAY_LOG_WARN("BACK-BUFFER STATE ASSERTION: the engine wrote a ResourceBarrier whose StateBefore differs from "
			"the state this ledger held for that swapchain-class resource (last PRESENT witness %p). UE 4.27 "
			"tracks per-resource state itself, so one of the two is wrong - and if it is ours, the level-2 "
			"state the NR stage names is wrong too. Once per session; the rate is `beforeMismatch=` on the "
			"[bbstate] line.", reinterpret_cast<void *>(mismatch_res));
}

void on_marker(ID3D12GraphicsCommandList *list, std::uint64_t res, std::uint32_t assumed)
{
	if (list == nullptr)
		return;
	std::lock_guard<std::mutex> lock(g_mutex);
	if (g_level == fseam::Level::off)
		return;
	if (is_own_list_locked(list))
		return;
	g_ledger.on_marker(reinterpret_cast<std::uint64_t>(list), res, assumed);
}

std::uint32_t state_of(std::uint64_t res, bool *known)
{
	std::lock_guard<std::mutex> lock(g_mutex);
	return g_ledger.state_of(res, known);
}

std::uint64_t last_present_resource()
{
	std::lock_guard<std::mutex> lock(g_mutex);
	return g_ledger.last_present_resource();
}

std::uint32_t present_state(std::uint64_t back_buffer, std::uint32_t assumed)
{
	std::lock_guard<std::mutex> lock(g_mutex);
	if (g_level == fseam::Level::off)
		return assumed;
	const bool tracked = g_candidates.contains(back_buffer);
	bool known = false;
	const std::uint32_t recorded = g_ledger.state_of(back_buffer, &known);
	const fseam::StateVerdict v = fseam::judge_present_state(tracked, known, recorded, assumed);
	++g_stats.verdict[static_cast<int>(v)];
	const std::uint32_t use = fseam::present_state_to_use(g_level, tracked, known, recorded, assumed);
	if (use == recorded && tracked && known && g_level == fseam::Level::authoritative)
		++g_stats.used_recorded;
	else
		++g_stats.used_assumed;
	if (!g_first_present_logged && v == fseam::StateVerdict::agree)
	{
		g_first_present_logged = true;
		STRAY_LOG_INFO("BACK-BUFFER STATE: first present-time check AGREES - the engine's last executed transition "
			"of %p is 0x%X, the constant the NR stage assumes. The ledger and the API's requirement say the "
			"same thing; from here `verdict agree=` on the [bbstate] line should track the stage's triggered count.",
			reinterpret_cast<void *>(back_buffer), recorded);
	}
	if (v == fseam::StateVerdict::disagree && !g_disagree_logged)
	{
		g_disagree_logged = true;
		STRAY_LOG_WARN("BACK-BUFFER STATE ASSERTION: at present the engine's last executed transition left %p in "
			"0x%X, and the NR stage assumes 0x%X ([STRAYDLSS] NgxNRStageBackBufferState). %s Once per session; "
			"the rate is `verdict disagree=` on the [bbstate] line.",
			reinterpret_cast<void *>(back_buffer), recorded, assumed,
			g_level == fseam::Level::authoritative
				? "LEVEL 2: the stage names the RECORDED state for this frame."
				: "Level 1: the stage still names the constant; set EngineSeamBackBufferState=2 to name the recorded one.");
	}
	if (v == fseam::StateVerdict::unknown && !g_unknown_logged)
	{
		g_unknown_logged = true;
		STRAY_LOG_WARN("BACK-BUFFER STATE: the NR stage ran on %p before the engine had transitioned it once since it "
			"was registered; the assumed constant 0x%X is used. Expected on the first frame after a (re)registration; "
			"a rate that keeps climbing means the ResourceBarrier hook is not seeing the engine's transitions. Once "
			"per session; the rate is `verdict unknown=`.", reinterpret_cast<void *>(back_buffer), assumed);
	}
	if (v == fseam::StateVerdict::not_tracked && !g_not_tracked_logged)
	{
		g_not_tracked_logged = true;
		STRAY_LOG_WARN("BACK-BUFFER STATE: the NR stage was handed %p, which is NOT a registered swapchain-class "
			"resource (%u tracked, epoch %llu). The assumed constant is used. Once per session; the rate is "
			"`verdict notTracked=`.", reinterpret_cast<void *>(back_buffer), g_candidates.count,
			static_cast<unsigned long long>(g_candidates.epoch));
	}
	return use;
}

Stats stats()
{
	std::lock_guard<std::mutex> lock(g_mutex);
	return g_stats;
}

int format_report(char *buffer, std::size_t size)
{
	if (buffer == nullptr || size == 0)
		return 0;
	const Stats s = stats();
	const fseam::Level lvl = level();
	const auto v = [&](fseam::StateVerdict x) { return static_cast<unsigned long long>(s.verdict[static_cast<int>(x)]); };
	return std::snprintf(buffer, size,
		"bbstate=%s candidates=%u epoch=%llu barriers=%llu executes=%llu lists=%llu applied=%llu beforeMismatch=%llu "
		"presentSeen=%llu markers(ok=%llu bad=%llu) | verdict agree=%llu disagree=%llu unknown=%llu notTracked=%llu "
		"| usedRecorded=%llu usedAssumed=%llu | ownLists=%u ownSkipped=%llu",
		fseam::level_name(lvl), s.candidates, static_cast<unsigned long long>(s.candidate_epoch),
		static_cast<unsigned long long>(s.barriers_seen), static_cast<unsigned long long>(s.executes),
		static_cast<unsigned long long>(s.lists_executed), static_cast<unsigned long long>(s.applied),
		static_cast<unsigned long long>(s.before_mismatch), static_cast<unsigned long long>(s.present_seen),
		static_cast<unsigned long long>(s.markers_ok), static_cast<unsigned long long>(s.markers_bad),
		v(fseam::StateVerdict::agree), v(fseam::StateVerdict::disagree), v(fseam::StateVerdict::unknown),
		v(fseam::StateVerdict::not_tracked), static_cast<unsigned long long>(s.used_recorded),
		static_cast<unsigned long long>(s.used_assumed), s.own_lists,
		static_cast<unsigned long long>(s.own_skipped));
}

void log_report(const char *when)
{
	if (level() == fseam::Level::off)
		return;
	char line[640] = {};
	format_report(line, sizeof(line));
	STRAY_LOG_INFO("[bbstate] %s: %s  (disagree, unknown, notTracked, beforeMismatch and markers bad must stay 0)",
		when != nullptr ? when : "", line);
}

} // namespace stray_dlss::native::bbstate
