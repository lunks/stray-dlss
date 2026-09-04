#include "view_params_hook.hpp"

#include "engine_seam_hook.hpp"
#include "intercept/backend.hpp"
#include "log.hpp"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace stray_dlss::vphook {
namespace {

std::mutex g_mutex;
viewcached::Mode g_mode = viewcached::Mode::off;
viewcached::Latch g_latch;
// Offsets that survived on the announcement that turned the latch ambiguous, for the one WARN.
std::uint32_t g_ambiguous_offsets[4] = {};
unsigned g_ambiguous_count = 0;
// The last scan's stage counts, for the first-scan line and the `absent` line.
viewcached::StageCounts g_last_counts;
std::uint64_t g_scans = 0;
// Level 2 outcomes: dispatches whose View came from the struct, and dispatches at level 2 with
// a live latch that had to fall back to the search (the announcement carried nothing, or it
// carried a View from an offset other than the latched one).
std::uint64_t g_used = 0;
std::uint64_t g_fell_back = 0;
bool g_first_scan_logged = false;
bool g_first_compare_logged = false;
bool g_latch_logged = false;
bool g_ambiguous_logged = false;
bool g_absent_logged = false;
int g_disagree_logged = 0;
constexpr int kDisagreeLogLimit = 8;
bool g_buffer_extent_logged = false;

// A read the guards let through and the CPU refused. Non-zero means the discovered offset is
// wrong on this executable after all, and the mechanism disables itself for the session rather
// than roll the dice again. Its own latch, not L1's: the two are separate mechanisms with
// separate counters, and one turning itself off must not silently switch the other off.
std::atomic<std::uint64_t> g_faults{ 0 };
std::atomic<std::uint64_t> g_fault_va{ 0 };
std::atomic<bool> g_disabled{ false };

// ---------------------------------------------------------------------------------------
// The guarded reader
// ---------------------------------------------------------------------------------------

bool plausible_heap_ptr(std::uint64_t va)
{
	if (va == 0 || (va & 7ull) != 0)
		return false;
	return va >= 0x10000ull && va < 0x0000800000000000ull;
}

// VirtualQuery answers for a whole REGION, so its result is cached per scan and re-used for
// every pointer that lands in the same region. An FViewInfo holds a few hundred pointers, most
// into a handful of heap regions; without this the discovery scan would be a few hundred
// syscalls on the render thread per announcement. Per-scan (reset by `begin_scan`), so a page
// whose protection changed between scans is re-queried.
constexpr unsigned kRegionCacheSlots = 32;
struct RegionCache
{
	std::uint64_t base[kRegionCacheSlots] = {};
	std::uint64_t size[kRegionCacheSlots] = {};
	bool readable[kRegionCacheSlots] = {};
	unsigned next = 0;
	unsigned count = 0;
	void reset() { next = 0; count = 0; }
};
thread_local RegionCache t_regions;

bool query_readable(std::uint64_t va, std::uint64_t &base_out, std::uint64_t &size_out)
{
	MEMORY_BASIC_INFORMATION mbi{};
	const SIZE_T got = VirtualQuery(
		reinterpret_cast<LPCVOID>(static_cast<std::uintptr_t>(va)), &mbi, sizeof(mbi));
	base_out = reinterpret_cast<std::uint64_t>(mbi.BaseAddress);
	size_out = static_cast<std::uint64_t>(mbi.RegionSize);
	if (got != sizeof(mbi) || mbi.State != MEM_COMMIT)
		return false;
	// PAGE_GUARD and PAGE_NOACCESS both fault; a guard page would additionally arm a
	// STATUS_GUARD_PAGE_VIOLATION for whoever owns it, so never touch one.
	if ((mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
		return false;
	constexpr DWORD kReadable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
		PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
	return (mbi.Protect & kReadable) != 0;
}

// Is [va, va+n) committed and readable, right now? A range test on the number is not enough:
// `rdg+16` once came back 0x0000021c000003c0 - two int32s read as one qword - and it passed
// every range guard before the read faulted (docs/RESEARCH-ENGINE-TAA-HOOK.md §12.4).
bool committed_and_readable(std::uint64_t va, std::size_t n)
{
	std::uint64_t remaining = n;
	std::uint64_t at = va;
	// A block may straddle regions; every region it touches must be readable.
	for (int hops = 0; remaining != 0 && hops < 8; ++hops)
	{
		std::uint64_t base = 0;
		std::uint64_t size = 0;
		bool ok = false;
		bool cached = false;
		for (unsigned i = 0; i < t_regions.count; ++i)
		{
			if (at >= t_regions.base[i] && at < t_regions.base[i] + t_regions.size[i])
			{
				base = t_regions.base[i];
				size = t_regions.size[i];
				ok = t_regions.readable[i];
				cached = true;
				break;
			}
		}
		if (!cached)
		{
			ok = query_readable(at, base, size);
			if (base == 0 || size == 0)
				return false;
			const unsigned slot = t_regions.next;
			t_regions.base[slot] = base;
			t_regions.size[slot] = size;
			t_regions.readable[slot] = ok;
			t_regions.next = (slot + 1) % kRegionCacheSlots;
			if (t_regions.count < kRegionCacheSlots)
				++t_regions.count;
		}
		if (!ok || at < base)
			return false;
		const std::uint64_t left_in_region = base + size - at;
		if (left_in_region >= remaining)
			return true;
		remaining -= left_in_region;
		at += left_in_region;
	}
	return false;
}

// The read itself, under SEH. EXPLICIT AND DELIBERATE: this dereferences engine memory through
// an offset that was DISCOVERED, so the honest engineering position is that it CAN fault and
// the process must survive it saying so (CLAUDE.md §0.2). MSVC only; the mingw lane has no
// __try and relies on the VirtualQuery guard above. The function holds no object needing
// unwinding, which is what MSVC requires of __try.
bool read_guarded(std::uint64_t va, std::size_t n, void *out)
{
#if defined(_MSC_VER)
	__try
	{
		std::memcpy(out, reinterpret_cast<const void *>(static_cast<std::uintptr_t>(va)), n);
		return true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return false;
	}
#else
	std::memcpy(out, reinterpret_cast<const void *>(static_cast<std::uintptr_t>(va)), n);
	return true;
#endif
}

bool note_fault(std::uint64_t va)
{
	g_faults.fetch_add(1, std::memory_order_relaxed);
	g_fault_va.store(va, std::memory_order_relaxed);
	return false;
}

bool vp_read_u64(void *, std::uint64_t va, std::uint64_t *out)
{
	if (!plausible_heap_ptr(va))
		return false;
	if (!committed_and_readable(va, sizeof(*out)))
		return false;
	return read_guarded(va, sizeof(*out), out) || note_fault(va);
}

bool vp_read_block(void *, std::uint64_t va, std::size_t n, void *out)
{
	if (!plausible_heap_ptr(va))
		return false;
	if (!committed_and_readable(va, n))
		return false;
	return read_guarded(va, n, out) || note_fault(va);
}

bool faulted() { return g_disabled.load(std::memory_order_acquire); }

// A fault means the offset is wrong, not that this frame was unlucky. Say so once, at ERROR,
// with the address the CPU refused - and stop.
void note_faults_and_latch()
{
	if (g_faults.load(std::memory_order_relaxed) == 0)
		return;
	if (g_disabled.exchange(true, std::memory_order_acq_rel))
		return;
	std::uint32_t offset = 0;
	viewcached::LatchState state = viewcached::LatchState::searching;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		offset = g_latch.offset();
		state = g_latch.state();
	}
	STRAY_LOG_ERROR("ENGINE SEAM VIEW PARAMS DISABLED: a guarded read of engine memory FAULTED "
		"at %#llx inside AddPasses on the render thread (latch=%s, offset=+%u). The guards "
		"caught it - the game is alive and this line exists because of it - but a fault past "
		"VirtualQuery means the memory moved under the read, and the mechanism is off for the "
		"rest of the session: the search supplies the View, counted as fellBack. Paste this "
		"line with the first-scan line.",
		static_cast<unsigned long long>(g_fault_va.load(std::memory_order_relaxed)),
		viewcached::latch_state_name(state), offset);
}

void log_stage_counts(const char *lead, const viewcached::StageCounts &sc)
{
	STRAY_LOG_INFO("%s candidates by stage: qwords=%u pointer-shaped=%u probed=%u readable=%u "
		"plausible=%u row135=%u fitsRect=%u aboveMinFraction=%u bufferSize=%u survivors=%u%s",
		lead, sc.qwords, sc.pointers, sc.probed, sc.readable, sc.plausible, sc.row135, sc.fits,
		sc.fraction, sc.buffer, sc.survivors,
		sc.truncated ? "  <- TRUNCATED: the probe budget ran out before the window was fully "
		               "judged; raise kMaxProbesPerScan" : "");
}

} // namespace

void configure(int level)
{
	viewcached::Mode m = viewcached::mode_from_level(level);
	if (!viewcached::mode_is_implemented(m))
	{
		// A rung that does not exist must say so, not quietly behave like a lower one - the
		// silent-downgrade failure `seam::decide` was written to avoid, applied to this ladder.
		STRAY_LOG_ERROR("ENGINE SEAM VIEW PARAMS: EngineSeamViewParams=%d asks for '%s', which "
			"is NOT BUILT - deleting the search is a separate decision after level 2 runs clean "
			"in gameplay. Running at level 2 (authoritative).", level, viewcached::mode_name(m));
		m = viewcached::Mode::authoritative;
	}
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		g_mode = m;
	}
	switch (m)
	{
	case viewcached::Mode::off:
		STRAY_LOG_INFO("ENGINE SEAM VIEW PARAMS: off ([STRAYDLSS] EngineSeamViewParams=0). The "
			"View constant buffer is located by SEARCH, as it always has been.");
		break;
	case viewcached::Mode::discover:
		STRAY_LOG_INFO("ENGINE SEAM VIEW PARAMS: discover ([STRAYDLSS] EngineSeamViewParams=1). "
			"Inside every AddPasses, scan the engine's own FViewInfo for "
			"CachedViewUniformShaderParameters - the CPU struct View.ViewUniformBuffer is "
			"created from - and carry its 2448-byte prefix in the announcement; at claim, compare "
			"it BYTE FOR BYTE against what the search read from the bound constant buffer. "
			"NOTHING IS GATED ON IT AND THE IMAGE IS UNCHANGED: the search supplies the View, "
			"and the output is a latched offset plus agree/disagree counts. It exists because the "
			"search was measured taking a STALE RING COPY of this very view on 0.33%% of claimed "
			"dispatches, which no structural test can separate (facts §36.20).");
		break;
	default:
		STRAY_LOG_INFO("ENGINE SEAM VIEW PARAMS: authoritative ([STRAYDLSS] "
			"EngineSeamViewParams=2). Once the offset has proven itself by %u byte-exact "
			"agreements with the bound buffer, the struct SUPPLIES the View for every announced "
			"dispatch and the search becomes the assertion: a disagreement is the search reading "
			"a stale ring copy, counted as disagree= and said in one WARN per pass. Until the "
			"latch, and for any announcement that carries nothing, the search supplies it and "
			"fellBack= counts the frame.", viewcached::kLatchAgreements);
		break;
	}
}

viewcached::Mode mode()
{
	std::lock_guard<std::mutex> lock(g_mutex);
	return g_mode;
}

void scan_at_announce(const void *view, seam::Announcement &a, std::uint64_t depth_res)
{
	a.view_carried = false;
	a.view_survivors = 0;
	a.view_offset = 0;

	viewcached::Mode m = viewcached::Mode::off;
	viewcached::LatchState state = viewcached::LatchState::searching;
	std::uint32_t latched = 0;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		m = g_mode;
		state = g_latch.state();
		latched = g_latch.offset();
	}
	if (m == viewcached::Mode::off || view == nullptr || faulted())
		return;
	// THE VERDICT IS TERMINAL, SO STOP WORKING FOR IT. `ambiguous` and `absent` never recover
	// within a session by construction (the state machine is sticky, and this call site
	// merely mirrors that), so scanning every frame would be pure render-thread cost for a
	// number that cannot change.
	if (state == viewcached::LatchState::ambiguous || state == viewcached::LatchState::absent)
		return;

	viewcached::Expectation exp;
	exp.out_width = a.out_width;
	exp.out_height = a.out_height;
	// Prediction 5: the extent of the depth texture L1 resolved for this very announcement. A
	// creation-time snapshot from our own registry - no dereference of the engine's object.
	if (depth_res != 0)
	{
		icept::ResourceInfo ri{};
		icept::Backend *b = icept::backend();
		if (b != nullptr && b->describe_resource(static_cast<icept::ResourceId>(depth_res), ri) &&
			!ri.is_buffer && ri.width != 0 && ri.height != 0)
		{
			exp.buffer_width = ri.width;
			exp.buffer_height = ri.height;
		}
	}

	viewcached::Reader r;
	r.read_u64 = &vp_read_u64;
	r.read_block = &vp_read_block;
	r.ctx = nullptr;
	t_regions.reset();

	// Up to four candidates are collected only so ambiguity can be REPORTED with every offset;
	// the announcement carries at most one.
	viewcached::Candidate got[4];
	viewcached::StageCounts sc;
	unsigned found = 0;
	const std::uint64_t view_va = reinterpret_cast<std::uint64_t>(view);
	if (state == viewcached::LatchState::latched)
	{
		// ONCE LATCHED, VERIFY RATHER THAN SEARCH: one guarded qword, one guarded 2448-byte
		// block, the same predictions. A failed verification carries nothing and the search
		// supplies that frame (counted `unverified`); it is never a demotion, because the
		// offset proved itself by bytes.
		if (viewcached::check_offset(r, view_va, latched, exp, &got[0]) == viewcached::Refusal::accepted)
			found = 1;
		sc.survivors = found;
	}
	else
	{
		found = viewcached::scan(r, view_va, exp, got, 4, &sc);
	}
	note_faults_and_latch();
	if (faulted())
		return;

	a.view_survivors = found;
	if (found == 1)
	{
		a.view_carried = true;
		a.view_offset = got[0].offset;
		a.view = got[0].params;
		std::memcpy(a.view_prefix, got[0].prefix, sizeof(a.view_prefix));
	}

	bool log_first = false;
	bool log_extent = false;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		++g_scans;
		g_last_counts = sc;
		if (found > 1 && g_ambiguous_count == 0)
		{
			g_ambiguous_count = found < 4 ? found : 4;
			for (unsigned i = 0; i < g_ambiguous_count; ++i)
				g_ambiguous_offsets[i] = got[i].offset;
		}
		if (!g_first_scan_logged)
		{
			g_first_scan_logged = true;
			log_first = true;
		}
		if (!g_buffer_extent_logged && exp.buffer_width != 0)
		{
			g_buffer_extent_logged = true;
			log_extent = true;
		}
	}
	if (log_first)
	{
		STRAY_LOG_INFO("ENGINE SEAM VIEW PARAMS: first scan of the engine's own FViewInfo at %p, "
			"%zu-byte window, announcement rect %ux%u, depth extent %ux%u%s - %u candidate "
			"offset(s) survived predictions 1-5%s.",
			const_cast<void *>(view), viewcached::kScanWindowBytes, a.out_width, a.out_height,
			exp.buffer_width, exp.buffer_height,
			exp.buffer_width == 0 ? " (unknown: L1 resolved no depth, so row 132 was not tested)" : "",
			found,
			found == 0 ? " - NONE, and the stage counts below say which prediction refused"
			           : (found == 1 ? " - exactly one, which is what a clean answer looks like"
			                         : " - MORE THAN ONE, so the latch will refuse for the session"));
		log_stage_counts("ENGINE SEAM VIEW PARAMS:", sc);
		if (found == 1)
			STRAY_LOG_INFO("ENGINE SEAM VIEW PARAMS: FViewInfo+%u -> FViewUniformShaderParameters at "
				"%p: %.0fx%.0f view, buffer %.0fx%.0f, jitter (%.4f, %.4f), PreExposure %.4f, "
				"CameraCut %.0f. DISCOVERED, NOT DERIVED - sizeof(FSceneView) is unobservable from "
				"outside. Prediction 6 (byte-equality with the search's read) is judged at claim "
				"and is what latches it; nothing changes until then.",
				got[0].offset, reinterpret_cast<void *>(got[0].pointee),
				static_cast<double>(got[0].params.view_size_and_inv_size.x),
				static_cast<double>(got[0].params.view_size_and_inv_size.y),
				static_cast<double>(got[0].params.buffer_size_and_inv_size.x),
				static_cast<double>(got[0].params.buffer_size_and_inv_size.y),
				static_cast<double>(got[0].params.temporal_aa_params.z),
				static_cast<double>(got[0].params.temporal_aa_params.w),
				static_cast<double>(got[0].params.pre_exposure),
				static_cast<double>(got[0].params.camera_cut));
	}
	if (log_extent && !log_first)
		STRAY_LOG_INFO("ENGINE SEAM VIEW PARAMS: L1's depth extent is now known (%ux%u), so row 132 "
			"is tested from this announcement on.", exp.buffer_width, exp.buffer_height);
}

ViewSource resolve_at_claim(const seamhook::Verdict &v, bool search_ok,
                            const unsigned char *search_prefix, const ue4::ViewParams &search_view)
{
	(void)search_view;
	ViewSource out;
	viewcached::Mode m = viewcached::Mode::off;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		m = g_mode;
	}
	if (m == viewcached::Mode::off || !v.announced || faulted())
		return out;

	viewcached::Observation o;
	o.survivors = v.view_survivors;
	o.offset = v.view_offset;
	out.carried = v.view_carried;
	out.offset = v.view_offset;
	viewcached::FieldDiff diff;
	if (v.view_carried && search_ok && search_prefix != nullptr)
	{
		o.compared = true;
		diff = viewcached::compare_prefix(v.view_prefix, search_prefix);
		o.bytes_equal = !diff.any;
		out.compared = true;
		out.bytes_equal = o.bytes_equal;
	}

	bool log_first_compare = false;
	bool log_latch = false;
	bool log_ambiguous = false;
	bool log_absent = false;
	bool log_disagree = false;
	viewcached::LatchState before = viewcached::LatchState::searching;
	viewcached::LatchState after = viewcached::LatchState::searching;
	std::uint64_t observations = 0;
	std::uint64_t agree = 0;
	std::uint64_t disagree = 0;
	std::uint64_t pre_disagree = 0;
	std::uint32_t latched = 0;
	std::uint32_t amb_offsets[4] = {};
	unsigned amb_count = 0;
	viewcached::StageCounts last_counts;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		before = g_latch.state();
		g_latch.observe(o);
		after = g_latch.state();
		latched = g_latch.offset();
		observations = g_latch.observations();
		agree = g_latch.agree();
		disagree = g_latch.disagree();
		pre_disagree = g_latch.pre_latch_disagreements();
		last_counts = g_last_counts;
		amb_count = g_ambiguous_count;
		for (unsigned i = 0; i < amb_count; ++i)
			amb_offsets[i] = g_ambiguous_offsets[i];

		viewcached::DecisionInputs in;
		in.mode = m;
		in.latch = after;
		in.latched_offset = latched;
		in.carried = v.view_carried;
		in.carried_offset = v.view_offset;
		out.use_engine = viewcached::use_engine_view(in);
		if (out.use_engine)
			++g_used;
		else if (m == viewcached::Mode::authoritative && after == viewcached::LatchState::latched)
			++g_fell_back;

		if (o.compared && !g_first_compare_logged)
		{
			g_first_compare_logged = true;
			log_first_compare = true;
		}
		if (after == viewcached::LatchState::latched && before != viewcached::LatchState::latched &&
			!g_latch_logged)
		{
			g_latch_logged = true;
			log_latch = true;
		}
		if (after == viewcached::LatchState::ambiguous && !g_ambiguous_logged)
		{
			g_ambiguous_logged = true;
			log_ambiguous = true;
		}
		if (after == viewcached::LatchState::absent && !g_absent_logged)
		{
			g_absent_logged = true;
			log_absent = true;
		}
		if (after == viewcached::LatchState::latched && o.compared && !o.bytes_equal &&
			g_disagree_logged < kDisagreeLogLimit)
		{
			++g_disagree_logged;
			log_disagree = true;
		}
	}
	if (out.use_engine)
		out.params = v.view;

	char what[192] = {};
	if (log_first_compare || log_disagree)
		viewcached::describe_diff(diff, what, sizeof(what));

	if (log_first_compare)
		STRAY_LOG_INFO("ENGINE SEAM VIEW PARAMS: first byte comparison - the struct carried from "
			"FViewInfo+%u against the 2448 bytes the search read from the bound constant buffer "
			"for the dispatch that claimed it: %s (%s). %s",
			v.view_offset, o.bytes_equal ? "IDENTICAL" : "DIFFERENT", what,
			o.bytes_equal
				? "That is prediction 6 holding: the struct IS the bound buffer. The latch needs "
				  "it to hold for several announcements running."
				: "One disagreement proves nothing either way - it is also what the search "
				  "reading a stale ring copy looks like. Read the latch line, or its absence, "
				  "after a few hundred frames.");

	if (log_latch)
		STRAY_LOG_INFO("ENGINE SEAM VIEW PARAMS LATCHED after %llu claimed announcements: "
			"FViewInfo+%u is CachedViewUniformShaderParameters. The bytes it points at equalled "
			"the bound View buffer %u announcements running (preDisagree=%llu before the run). "
			"%s Paste the offset: it is what level 3 would pin.",
			static_cast<unsigned long long>(observations), latched, viewcached::kLatchAgreements,
			static_cast<unsigned long long>(pre_disagree),
			m == viewcached::Mode::authoritative
				? "FROM HERE ON THE STRUCT SUPPLIES THE VIEW and the search is the assertion; "
				  "ambClaimed can no longer count because the search no longer chooses."
				: "Level 1: nothing changes; the search still supplies the View. agree= and "
				  "disagree= on the [viewParams] line are the search being judged.");

	if (log_ambiguous)
	{
		char offs[96];
		int n = 0;
		offs[0] = '\0';
		for (unsigned i = 0; i < amb_count && n >= 0 && n < static_cast<int>(sizeof(offs)) - 1; ++i)
			n += std::snprintf(offs + n, sizeof(offs) - static_cast<std::size_t>(n), " +%u", amb_offsets[i]);
		STRAY_LOG_WARN("ENGINE SEAM VIEW PARAMS AMBIGUOUS: more than one offset survived every "
			"prediction on one announcement (%s), so the scan REFUSES TO LATCH for the session "
			"rather than pick by scan order - picking one of two possible answers is the exact "
			"bug this mechanism exists to remove, one level down. Nothing changed; the search "
			"supplies the View as before. Paste this line and the first-scan line.", offs);
	}

	if (log_absent)
	{
		STRAY_LOG_ERROR("ENGINE SEAM VIEW PARAMS: NO candidate offset survived on %llu consecutive "
			"claimed announcements. The last scan's stage counts say which prediction refused; "
			"a `bufferSize` refusal means the row-132 derivation is wrong on this executable and "
			"the mechanism cannot be built on it until that is settled. Nothing else is affected.",
			static_cast<unsigned long long>(viewcached::kAbsentAfter));
		log_stage_counts("ENGINE SEAM VIEW PARAMS: last scan", last_counts);
	}

	if (log_disagree)
		STRAY_LOG_WARN("ENGINE SEAM VIEW PARAMS ASSERTION: the search's View for this dispatch "
			"DIFFERS from the engine's struct - %s. The search read a stale ring copy (facts "
			"§36.20's shape: same rect, different motion fields). %s agree=%llu disagree=%llu so "
			"far. Logged %d times; the rate is disagree= on the [viewParams] line.",
			what,
			out.use_engine ? "The ENGINE's is used."
			               : "Level 1: the search's is still used, so this frame carried the "
			                 "stale copy to DLSS.",
			static_cast<unsigned long long>(agree), static_cast<unsigned long long>(disagree),
			kDisagreeLogLimit);
	return out;
}

int format_report(char *buffer, std::size_t size)
{
	if (buffer == nullptr || size == 0)
		return 0;
	viewcached::Mode m = viewcached::Mode::off;
	viewcached::LatchState s = viewcached::LatchState::searching;
	std::uint32_t offset = 0;
	std::uint64_t scans = 0;
	std::uint64_t obs = 0;
	std::uint64_t agree = 0;
	std::uint64_t disagree = 0;
	std::uint64_t pre = 0;
	std::uint64_t uncompared = 0;
	std::uint64_t unverified = 0;
	std::uint64_t amb = 0;
	std::uint64_t empty = 0;
	std::uint64_t used = 0;
	std::uint64_t fell_back = 0;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		m = g_mode;
		s = g_latch.state();
		offset = g_latch.offset();
		scans = g_scans;
		obs = g_latch.observations();
		agree = g_latch.agree();
		disagree = g_latch.disagree();
		pre = g_latch.pre_latch_disagreements();
		uncompared = g_latch.uncompared();
		unverified = g_latch.unverified();
		amb = g_latch.ambiguous_observations();
		empty = g_latch.empty_observations();
		used = g_used;
		fell_back = g_fell_back;
	}
	return std::snprintf(buffer, size,
		"viewParams: mode=%s latch=%s offset=%u latched=%d scans=%llu observed=%llu agree=%llu "
		"disagree=%llu preDisagree=%llu uncompared=%llu unverified=%llu ambiguous=%llu empty=%llu "
		"faults=%llu off=%d used=%llu fellBack=%llu",
		viewcached::mode_name(m), viewcached::latch_state_name(s), offset,
		s == viewcached::LatchState::latched ? 1 : 0,
		static_cast<unsigned long long>(scans), static_cast<unsigned long long>(obs),
		static_cast<unsigned long long>(agree), static_cast<unsigned long long>(disagree),
		static_cast<unsigned long long>(pre), static_cast<unsigned long long>(uncompared),
		static_cast<unsigned long long>(unverified), static_cast<unsigned long long>(amb),
		static_cast<unsigned long long>(empty),
		static_cast<unsigned long long>(g_faults.load(std::memory_order_relaxed)),
		g_disabled.load(std::memory_order_relaxed) ? 1 : 0,
		static_cast<unsigned long long>(used), static_cast<unsigned long long>(fell_back));
}

void log_report(const char *when)
{
	if (mode() == viewcached::Mode::off)
		return;
	char line[512] = {};
	format_report(line, sizeof(line));
	// `disagree` is the number this mechanism exists to name: each one is a claimed dispatch on
	// which the search read a stale ring copy. At level 1 that frame still reached DLSS with the
	// stale View (ambClaimed on the [view] line counts the same events from the search's side);
	// at level 2 the struct's View went instead, and ambClaimed reads 0 by construction.
	STRAY_LOG_INFO("[viewParams] %s: %s", when != nullptr ? when : "", line);
}

} // namespace stray_dlss::vphook
