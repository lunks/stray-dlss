#include "gbuffer_finder.hpp"

#include "frame_state.hpp"
#include "log.hpp"

#include <algorithm>
#include <cstddef>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// EVERY line this module logs contains the token "GBUF" (the block headers spell out
// "GBUFFER FINDER", whose prefix is GBUF; every detail line starts with "GBUF"). Learned
// from the 2026-08-31 observation run: the first FAILED block's tally lines began with
// bare spaces, and a log filtered for the finder's name showed exactly one line — the
// tallies, which WERE the observation, never reached the report. One grep token must
// recover the whole story. (CLAUDE.md §0.1: maximum diagnostic payload per round-trip.)

namespace stray_dlss::gbuffer_finder {
namespace {

// The live base pass binds 6 RTVs (SceneColor + GBufferA/B/C/D/E, measured 2026-08-31;
// stock-with-velocity would be 7 — core/gbuffer_classify.hpp); everything else in a UE4
// frame binds 0-2. Only sets at least this wide are CLASSIFIED (the classifier itself
// requires kMinBasePassColourTargets = 5). 4 rather than 5-6 so a set a target short is
// still classified — and rejected with a reason — instead of being invisible.
constexpr uint32_t kMinMrtCount = 4;

// The identification must hold this many consecutive frames before it is logged. Half a
// second at 60 fps: long enough to outlive transient binds and resolution churn, short
// enough that one short observation run settles it.
constexpr std::uint64_t kStableFrames = 30;

// No base-pass candidate for this long logs the first FAILED block: ten seconds at
// 60 fps, far longer than any loading hitch. Under the corrected velocity-free signature
// even the MENU should classify — measured 2026-08-31 it binds ~600 six-RTV G-buffer
// sets per minute (RESEARCH-RR-GBUFFER.md §1.4) — so a FAILED block anywhere is
// genuinely diagnostic, not expected background noise.
constexpr std::uint64_t kFailFrames = 600;

// While the drought continues, the FAILED block RE-FIRES this often (one minute at
// 60 fps), each time with the event tallies of its own window. Measured 2026-08-31: a
// once-per-session FAILED fired at the menu and then minutes of candidate-less GAMEPLAY
// produced nothing — in a negative world the tallies are the observation, and a single
// menu-time block taught almost nothing about why.
constexpr std::uint64_t kFailRepeatFrames = 3600;

// Log budgets, in the pass_finder mould: enough re-fires to cover a long session (20
// blocks = ~20 minutes of continuous drought after the first), bounded so a pathological
// run cannot drown the log the user pastes back.
constexpr int kReportBudget = 8;
constexpr int kFailBudget = 20;
constexpr int kRejectReasonsLogged = 4;

std::mutex g_mutex;
bool g_enabled = false;

struct ListRecord
{
	GBufferClassification pending; // the qualifying RT set currently bound on this list
	bool has_pending = false;
	std::uint32_t pending_draws = 0;
};
std::unordered_map<reshade::api::command_list *, ListRecord> g_lists;

// One frame's base-pass candidates, coalesced across binds by target identity: the base
// pass re-binds its MRT set several times per frame (per command list, and around the
// early-Z / masked / translucent splits), and those are all the SAME candidate.
struct Candidate
{
	GBufferClassification cls;
	std::uint32_t draws = 0;
	std::uint32_t binds = 0;
};
std::vector<Candidate> g_frame_candidates;

// This frame's SSR-denoiser SRV resources (cross-check, gbuffer_finder.hpp).
std::unordered_set<std::uint64_t> g_denoiser_srvs;
bool g_denoiser_seen_this_frame = false;

// Event-level census. It separates "the bind_render_targets tap never fired"
// (registration or ReShade-event bug) from "it fired but nothing matched the signature",
// and its velocity columns track the separate velocity pass (narrow binds — the expected
// Stray world, RESEARCH-RR-GBUFFER.md §1.3-1.4). Kept twice: per session, and per FAILED
// window so consecutive blocks show what changed between menu and gameplay.
struct EventStats
{
	std::uint64_t rt_events = 0;            // every note_render_targets call, any count
	std::uint64_t histogram[9] = {};        // by RTV count; bucket 8 = "8 or more"
	std::uint32_t max_rtv_count = 0;
	std::uint64_t wide_sets = 0;            // count >= kMinMrtCount
	std::uint64_t velocity_sightings = 0;   // binds with a live R16G16B16A16_UNORM RTV
	std::uint32_t velocity_widest_bind = 0; // widest RTV count among those binds
	std::uint32_t velocity_w = 0;           // extent of the last velocity RTV seen
	std::uint32_t velocity_h = 0;
};
EventStats g_session;
EventStats g_window;
bool g_first_rt_event_logged = false;

// Rejection tallies for classified-but-refused wide sets, session plus window.
std::unordered_map<std::string, std::uint64_t> g_session_rejects;
std::unordered_map<std::string, std::uint64_t> g_window_rejects;

// One-shot slot tables. The 2026-08-31 runs discarded exactly the evidence that would
// have named the once-at-load candidate and the 6-RTV menu sets, because nothing logged a
// set's members until 30 stable frames (RESEARCH-RR-GBUFFER.md §1.4 hypothesis 2, §4.4
// item 2). Now the FIRST wide set and the FIRST accepted candidate each log their full
// slot table immediately, once per session.
bool g_first_wide_logged = false;
bool g_first_candidate_logged = false;

// Cross-frame stability. Identity is the SHAPE — (slot, format, extent) per member —
// deliberately WITHOUT resource pointers: the 2026-08-31 run held the same 8-target shape
// through minutes of menu and gameplay yet "STABLE" never fired and no CHANGED line ever
// appeared, which is what per-frame identity flapping looks like (CHANGED only logs after
// a first report). UE4's pool can hand a different element for the same spec on any frame
// (FindFreeElement; RESEARCH-RR-GBUFFER.md §1.1 describes the per-frame release/reacquire),
// so pointer identity was the wrong key. Pointers are still REPORTED — the table shows the
// current frame's, and rotation is counted so the run measures how stable they really are,
// which phase 3's capture design needs to know.
std::uint64_t g_pointer_rotation_frames = 0; // stable-shape frames where any pointer changed
GBufferClassification g_current;
bool g_have_current = false;
std::uint64_t g_consecutive = 0;
bool g_reported_current = false;
std::uint32_t g_last_draws = 0;
// Sticky over the current run, parallel to g_current.targets: whether the SSR denoiser
// read this resource on at least one frame while the identification held.
std::vector<char> g_corroborated;
bool g_denoiser_seen_in_run = false;

std::uint64_t g_frames_without_candidate = 0;
bool g_failed_in_this_drought = false;
int g_reports_logged = 0;
int g_fail_reports_logged = 0;

// The stability identity: same slot layout, formats and extents. Resource pointers are
// deliberately excluded (see the g_pointer_rotation_frames comment).
bool same_shape(const GBufferClassification &a, const GBufferClassification &b)
{
	if (a.targets.size() != b.targets.size())
		return false;
	for (std::size_t i = 0; i < a.targets.size(); ++i)
	{
		const BoundTexture &x = a.targets[i].tex;
		const BoundTexture &y = b.targets[i].tex;
		if (x.slot != y.slot || x.format != y.format ||
			x.width != y.width || x.height != y.height)
			return false;
	}
	return true;
}

bool same_resources(const GBufferClassification &a, const GBufferClassification &b)
{
	for (std::size_t i = 0; i < a.targets.size() && i < b.targets.size(); ++i)
		if (a.targets[i].tex.resource != b.targets[i].tex.resource)
			return false;
	return true;
}

// Moves a list's accumulated draws into the frame's candidates. Caller holds g_mutex. The
// pending set stays pending — the list may keep drawing into it — only the count is
// harvested.
void flush_pending_locked(ListRecord &lr)
{
	if (!lr.has_pending || lr.pending_draws == 0)
		return;
	for (auto &c : g_frame_candidates)
	{
		if (same_shape(c.cls, lr.pending))
		{
			c.draws += lr.pending_draws;
			++c.binds;
			lr.pending_draws = 0;
			return;
		}
	}
	Candidate c;
	c.cls = lr.pending;
	c.draws = lr.pending_draws;
	c.binds = 1;
	g_frame_candidates.push_back(std::move(c));
	lr.pending_draws = 0;
}

void log_event_stats(const char *label, const EventStats &s)
{
	STRAY_LOG_ERROR("GBUF   %s: rt-bind events=%llu max-RTVs=%u wide(>=%u)=%llu "
		"velocity-RTV sightings=%llu", label,
		static_cast<unsigned long long>(s.rt_events), s.max_rtv_count, kMinMrtCount,
		static_cast<unsigned long long>(s.wide_sets),
		static_cast<unsigned long long>(s.velocity_sightings));
	STRAY_LOG_ERROR("GBUF   %s RTV-count histogram: 0:%llu 1:%llu 2:%llu 3:%llu 4:%llu "
		"5:%llu 6:%llu 7:%llu 8+:%llu", label,
		static_cast<unsigned long long>(s.histogram[0]),
		static_cast<unsigned long long>(s.histogram[1]),
		static_cast<unsigned long long>(s.histogram[2]),
		static_cast<unsigned long long>(s.histogram[3]),
		static_cast<unsigned long long>(s.histogram[4]),
		static_cast<unsigned long long>(s.histogram[5]),
		static_cast<unsigned long long>(s.histogram[6]),
		static_cast<unsigned long long>(s.histogram[7]),
		static_cast<unsigned long long>(s.histogram[8]));
	if (s.velocity_sightings > 0)
		STRAY_LOG_ERROR("GBUF   %s velocity: last RTV %ux%u, widest bind carrying it had "
			"%u targets (narrow velocity binds = the separate velocity pass, expected "
			"under r.SelectiveBasePassOutputs=True)", label, s.velocity_w, s.velocity_h,
			s.velocity_widest_bind);
}

// One line per member, every line GBUF-prefixed, identical to the stable report's rows so
// the tables are directly comparable across a pasted log.
void log_slot_table(const GBufferClassification &c)
{
	for (const auto &t : c.targets)
		STRAY_LOG_INFO("GBUF   slot %u  %-12s res=0x%016llx  %-12s %ux%u  stock=%-3s%s%s",
			t.tex.slot, gbuffer_role_name(t.role),
			static_cast<unsigned long long>(t.tex.resource),
			tex_format_name(t.tex.format), t.tex.width, t.tex.height,
			t.stock_format ? "yes" : "NO",
			t.note[0] != 0 ? "  <- " : "", t.note);
}

} // namespace

void set_enabled(bool value)
{
	g_enabled = value;
	if (value)
		STRAY_LOG_WARN("GBUFFER FINDER enabled ([STRAYDLSS] GBufferFinder): classifying every "
			"render-target set with >= %u targets against the VELOCITY-FREE G-buffer "
			"signature (RGB10A2 anchor at slot 1 - RESEARCH-RR-GBUFFER.md §1.5) and LOGGING "
			"the identification once it holds %llu consecutive frames. Log-only; nothing "
			"acts on it. Grep the log for GBUF to collect every line. (DLSS-RR phase 1)",
			kMinMrtCount, static_cast<unsigned long long>(kStableFrames));
}

bool enabled()
{
	return g_enabled;
}

void note_render_targets(reshade::api::command_list *cmd_list, uint32_t count,
                         const reshade::api::resource_view *rtvs, reshade::api::resource_view dsv)
{
	if (!g_enabled)
		return;

	// Whatever the new set is, the previous one on this list is done receiving draws.
	bool first_event = false;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		const auto it = g_lists.find(cmd_list);
		if (it != g_lists.end())
		{
			flush_pending_locked(it->second);
			it->second.has_pending = false;
		}

		// The event census counts EVERY tap, before any gate: a session total of zero here
		// is the registration-bug verdict, and nothing downstream can produce it.
		const std::size_t bucket = count < 8 ? count : 8;
		for (EventStats *s : { &g_session, &g_window })
		{
			++s->rt_events;
			++s->histogram[bucket];
			if (count > s->max_rtv_count)
				s->max_rtv_count = count;
			if (count >= kMinMrtCount)
				++s->wide_sets;
		}
		if (!g_first_rt_event_logged)
		{
			g_first_rt_event_logged = true;
			first_event = true;
		}
	}
	if (first_event)
		STRAY_LOG_INFO("GBUF first render-target bind event received (count=%u): the event "
			"tap is alive.", count);

	if (count == 0)
		return;

	// Describe the RTVs of EVERY bind, not only wide ones: the velocity census tracks the
	// SEPARATE velocity pass — one RTV plus depth, the shape Stray's
	// r.SelectiveBasePassOutputs=True forces (RESEARCH-RR-GBUFFER.md §1.3) — whose narrow
	// binds during gameplay are the expected home of the R16G16B16A16_UNORM target the
	// MRT sets lack. describe_bound_view is liveness-checked
	// FIRST, like every view this project touches (frame_state.hpp, CLAUDE.md §5 "Two
	// descriptor hazards"), and runs outside our lock — it takes frame_state's own.
	reshade::api::device *device = cmd_list->get_device();
	std::vector<BoundTexture> rts;
	for (uint32_t i = 0; i < count; ++i)
		describe_bound_view(device, rtvs[i], i, rts);
	if (count >= kMinMrtCount && dsv.handle != 0)
		describe_bound_view(device, dsv, count, rts);

	bool has_velocity = false;
	std::uint32_t vel_w = 0, vel_h = 0;
	for (const auto &t : rts)
		if (t.format == TexFormat::r16g16b16a16_unorm && (t.width > 1 || t.height > 1))
		{
			has_velocity = true;
			vel_w = t.width;
			vel_h = t.height;
		}

	// Narrow sets: census only, no classification.
	if (count < kMinMrtCount)
	{
		if (has_velocity)
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			for (EventStats *s : { &g_session, &g_window })
			{
				++s->velocity_sightings;
				if (count > s->velocity_widest_bind)
					s->velocity_widest_bind = count;
				s->velocity_w = vel_w;
				s->velocity_h = vel_h;
			}
		}
		return;
	}

	GBufferClassification cls = classify_render_target_set(rts);

	// One-shot evidence: the first wide set (whatever the verdict) and the first accepted
	// candidate each get their full slot table logged IMMEDIATELY — waiting for stability
	// discarded exactly the once-at-load evidence last time (RESEARCH-RR-GBUFFER.md §1.4).
	enum class FirstLog { none, wide, candidate };
	FirstLog first_log = FirstLog::none;
	GBufferClassification table_snap;

	{
		std::lock_guard<std::mutex> lock(g_mutex);

		if (has_velocity)
		{
			for (EventStats *s : { &g_session, &g_window })
			{
				++s->velocity_sightings;
				if (count > s->velocity_widest_bind)
					s->velocity_widest_bind = count;
				s->velocity_w = vel_w;
				s->velocity_h = vel_h;
			}
		}

		if (cls.is_base_pass && !g_first_candidate_logged)
		{
			g_first_candidate_logged = true;
			g_first_wide_logged = true; // a candidate table supersedes the plain wide one
			first_log = FirstLog::candidate;
			table_snap = cls;
		}
		else if (!g_first_wide_logged)
		{
			g_first_wide_logged = true;
			first_log = FirstLog::wide;
			table_snap = cls;
		}

		if (!cls.is_base_pass)
		{
			++g_session_rejects[cls.reason];
			++g_window_rejects[cls.reason];
		}
		else
		{
			ListRecord &lr = g_lists[cmd_list];
			lr.pending = std::move(cls);
			lr.has_pending = true;
			lr.pending_draws = 0;
		}
	}

	if (first_log == FirstLog::candidate)
	{
		STRAY_LOG_INFO("GBUF FIRST base-pass candidate (logged immediately, stability "
			"pending): %s", table_snap.reason);
		log_slot_table(table_snap);
	}
	else if (first_log == FirstLog::wide)
	{
		STRAY_LOG_INFO("GBUF first >=%u-RTV set observed (once per session), %zu described "
			"targets, verdict: %s", kMinMrtCount, table_snap.targets.size(),
			table_snap.reason);
		log_slot_table(table_snap);
	}
}

void note_draw(reshade::api::command_list *cmd_list)
{
	if (!g_enabled)
		return;
	std::lock_guard<std::mutex> lock(g_mutex);
	const auto it = g_lists.find(cmd_list);
	if (it != g_lists.end() && it->second.has_pending)
		++it->second.pending_draws;
}

void note_dispatch(reshade::api::command_list *cmd_list, std::uint64_t shader_hash)
{
	// Only the known SSR-denoiser look-alike is worth a descriptor-table resolve — the one
	// expensive operation in this module, and it runs at most a handful of times per frame.
	if (!g_enabled || shader_hash != kDenoiserLookalikeHash)
		return;

	DispatchBindings b;
	if (!resolve_compute_bindings(cmd_list, b)) // outside our lock; takes frame_state's
		return;

	std::lock_guard<std::mutex> lock(g_mutex);
	g_denoiser_seen_this_frame = true;
	for (const auto &s : b.srvs)
		if (s.width > 1 || s.height > 1)
			g_denoiser_srvs.insert(s.resource);
}

void forget_command_list(reshade::api::command_list *cmd_list)
{
	if (!g_enabled)
		return;
	std::lock_guard<std::mutex> lock(g_mutex);
	const auto it = g_lists.find(cmd_list);
	if (it != g_lists.end())
	{
		// A reset list was almost always executed first, so harvest its draws rather than
		// dropping them. A list abandoned unexecuted would over-count by one bind — noise
		// in a diagnostic that only compares identities and orders of magnitude.
		flush_pending_locked(it->second);
		g_lists.erase(it);
	}
}

void on_present(std::uint64_t frame)
{
	if (!g_enabled)
		return;

	// Gather under the lock, log after — the log sink has its own lock and holding two at
	// once is how deadlocks are built.
	bool log_stable = false;
	bool log_changed = false;
	bool log_failed = false;
	bool log_recovered = false;
	GBufferClassification snap;
	std::vector<char> corro;
	std::uint64_t consecutive = 0;
	std::uint64_t pointer_rotations = 0;
	std::uint32_t draws = 0;
	bool denoiser_run = false;
	std::uint64_t old_stable = 0;
	std::uint64_t fail_frames = 0;
	std::uint64_t recovered_after = 0;
	int fail_report_no = 0;
	EventStats session_snap;
	EventStats window_snap;
	std::vector<std::pair<std::string, std::uint64_t>> rejects;

	{
		std::lock_guard<std::mutex> lock(g_mutex);

		// Harvest every open list, so draws recorded but not yet flushed count for THIS
		// frame. A list executed after the present would attribute to the next frame —
		// tolerable in a diagnostic that needs identity, not exact counts.
		for (auto &entry : g_lists)
			flush_pending_locked(entry.second);

		// The frame's base pass is the candidate with the most draws: a base pass is
		// hundreds to thousands of draws, anything else that sneaks past the classifier
		// (a clear quad, a small custom pass) is a handful.
		const Candidate *best = nullptr;
		for (const auto &c : g_frame_candidates)
			if (best == nullptr || c.draws > best->draws)
				best = &c;

		if (best == nullptr)
		{
			++g_frames_without_candidate;
			g_consecutive = 0;

			// First block after kFailFrames, then a re-fire every kFailRepeatFrames of
			// CONTINUED drought — each with its own window's tallies, so menu-time and
			// gameplay-time windows are separately observable. Measured 2026-08-31 that a
			// single once-per-session block was nearly worthless.
			const bool due = g_frames_without_candidate == kFailFrames ||
				(g_frames_without_candidate > kFailFrames &&
				 (g_frames_without_candidate - kFailFrames) % kFailRepeatFrames == 0);
			if (due && g_fail_reports_logged < kFailBudget)
			{
				++g_fail_reports_logged;
				g_failed_in_this_drought = true;
				log_failed = true;
				fail_report_no = g_fail_reports_logged;
				fail_frames = g_frames_without_candidate;
				session_snap = g_session;
				window_snap = g_window;
				rejects.assign(g_window_rejects.begin(), g_window_rejects.end());
				g_window = EventStats{};
				g_window_rejects.clear();
			}
		}
		else
		{
			// A candidate after a reported drought is news in itself — it timestamps the
			// menu-to-gameplay (or load) transition against the FAILED windows.
			if (g_failed_in_this_drought)
			{
				g_failed_in_this_drought = false;
				log_recovered = true;
				recovered_after = g_frames_without_candidate;
				g_window = EventStats{};
				g_window_rejects.clear();
			}
			g_frames_without_candidate = 0;

			if (g_have_current && same_shape(best->cls, g_current))
			{
				++g_consecutive;
				// The measured answer to "does the pool rotate the pointers": count the
				// stable-shape frames whose resources differ from the previous frame's.
				if (!same_resources(best->cls, g_current))
					++g_pointer_rotation_frames;
				// Keep the CURRENT frame's pointers for the report and the corroboration
				// lookups (indices align: same shape implies same target count).
				g_current = best->cls;
			}
			else
			{
				if (g_reported_current)
				{
					log_changed = true;
					old_stable = g_consecutive;
				}
				g_current = best->cls;
				g_have_current = true;
				g_consecutive = 1;
				g_reported_current = false;
				g_corroborated.assign(g_current.targets.size(), 0);
				g_denoiser_seen_in_run = false;
				g_pointer_rotation_frames = 0;
			}

			// Sticky corroboration: one denoiser read of a target's resource at any point
			// during the run is enough — the denoiser does not necessarily bind every
			// G-buffer every frame.
			if (g_denoiser_seen_this_frame)
			{
				g_denoiser_seen_in_run = true;
				for (std::size_t i = 0; i < g_current.targets.size(); ++i)
					if (g_denoiser_srvs.count(g_current.targets[i].tex.resource) != 0)
						g_corroborated[i] = 1;
			}
			g_last_draws = best->draws;

			if (g_consecutive >= kStableFrames && !g_reported_current &&
				g_reports_logged < kReportBudget)
			{
				g_reported_current = true;
				++g_reports_logged;
				log_stable = true;
				snap = g_current;
				corro = g_corroborated;
				consecutive = g_consecutive;
				draws = g_last_draws;
				denoiser_run = g_denoiser_seen_in_run;
				pointer_rotations = g_pointer_rotation_frames;
			}
		}

		g_frame_candidates.clear();
		g_denoiser_srvs.clear();
		g_denoiser_seen_this_frame = false;
	}

	if (log_recovered)
		STRAY_LOG_WARN("GBUF base-pass candidate APPEARED at frame %llu after %llu "
			"candidate-less frames.", static_cast<unsigned long long>(frame),
			static_cast<unsigned long long>(recovered_after));

	if (log_changed)
		STRAY_LOG_WARN("GBUF identification CHANGED at frame %llu after %llu stable frames "
			"(resolution change, or a different scene's allocation). The new set reports "
			"when it has held %llu frames.", static_cast<unsigned long long>(frame),
			static_cast<unsigned long long>(old_stable),
			static_cast<unsigned long long>(kStableFrames));

	if (log_stable)
	{
		STRAY_LOG_INFO("======== GBUFFER FINDER  frame %llu: identification STABLE for %llu "
			"frames ========", static_cast<unsigned long long>(frame),
			static_cast<unsigned long long>(consecutive));
		STRAY_LOG_INFO("GBUF base pass: %zu targets at %ux%u (scene-buffer extent), %u draws "
			"this frame, stock roles matched %d/4", snap.targets.size(), snap.extent_width,
			snap.extent_height, draws, snap.stock_roles_matched);
		for (std::size_t i = 0; i < snap.targets.size(); ++i)
		{
			const GBufferTarget &t = snap.targets[i];
			STRAY_LOG_INFO("GBUF   slot %u  %-12s res=0x%016llx  %-12s %ux%u  stock=%-3s "
				"ssr-srv=%s%s%s", t.tex.slot, gbuffer_role_name(t.role),
				static_cast<unsigned long long>(t.tex.resource),
				tex_format_name(t.tex.format), t.tex.width, t.tex.height,
				t.stock_format ? "yes" : "NO",
				(i < corro.size() && corro[i] != 0) ? "YES" : "no",
				t.note[0] != 0 ? "  <- " : "", t.note);
		}
		if (snap.bc_order_by_slot_only)
			STRAY_LOG_INFO("GBUF   B/C order: both 8-bit RGBA - assigned by the stock slot "
				"order alone (HARD-via-mirror in stock source, but a licensee reorder is "
				"conceivable); the consumption cross-check or a guide dump must confirm "
				"which is base colour.");
		STRAY_LOG_INFO("GBUF   velocity member: %s", snap.velocity_corroborated
			? "present at stock slot 4 - corroborates the stock-with-velocity shape"
			: "absent - the velocity-free layout the live game binds (separate velocity "
			  "pass under r.SelectiveBasePassOutputs=True; expected, NOT a failure)");
		STRAY_LOG_INFO("GBUF   ssr cross-check: denoiser 0x%016llx dispatched during this "
			"run: %s%s", static_cast<unsigned long long>(kDenoiserLookalikeHash),
			denoiser_run ? "yes" : "no",
			denoiser_run ? "" :
			" (its hash is configuration-specific, CLAUDE.md §2.3; absence proves nothing)");
		STRAY_LOG_INFO("GBUF   pointer stability: resources rotated on %llu of %llu stable "
			"frames (identity keys on slot+format+extent; the table shows THIS frame's "
			"pointers - 0 rotations means the pool reuses the same textures, high counts "
			"mean phase 3 must re-capture pointers every frame)",
			static_cast<unsigned long long>(pointer_rotations),
			static_cast<unsigned long long>(consecutive));
		STRAY_LOG_INFO("GBUF   log-only: nothing acts on this identification yet (DLSS-RR "
			"phase 1)");
		STRAY_LOG_INFO("GBUF ===============================================");
	}

	if (log_failed)
	{
		STRAY_LOG_ERROR("GBUFFER FINDER FAILED: no base-pass candidate for %llu consecutive "
			"frames (at frame %llu, report %d).",
			static_cast<unsigned long long>(fail_frames),
			static_cast<unsigned long long>(frame), fail_report_no);
		log_event_stats("window", window_snap);
		log_event_stats("session", session_snap);

		// The verdict lines: which failure world is this? Ordered from the most to the
		// least fundamental, on the SESSION totals. Velocity is deliberately NOT part of
		// the ladder any more: its absence from every MRT set is the EXPECTED Stray shape
		// (velocity renders in its own thin pass - RESEARCH-RR-GBUFFER.md §1.3-1.4), so
		// the sightings above are context, never a verdict.
		if (session_snap.rt_events == 0)
			STRAY_LOG_ERROR("GBUF   ZERO render-target bind events ever received: the event "
				"tap never fired. Registration bug, or this ReShade build does not dispatch "
				"bind_render_targets/begin_render_pass - the classifier never saw anything.");
		else if (session_snap.wide_sets == 0)
			STRAY_LOG_ERROR("GBUF   no >=%u-RTV set was ever bound: no deferred G-buffer "
				"pass is visible here (measured 2026-08-31 both menu and gameplay DO bind "
				"6-RTV sets, so this outcome now points at the tap or the scene, not at "
				"the signature).", kMinMrtCount);
		else
			STRAY_LOG_ERROR("GBUF   wide sets exist but none matched the velocity-free "
				"G-buffer signature (RGB10A2 anchor at slot 1, >=5 colour targets, one "
				"extent - RESEARCH-RR-GBUFFER.md §1.5): the rejection reasons below name "
				"the failing rule. Velocity is NOT required.");
		std::sort(rejects.begin(), rejects.end(),
			[](const auto &a, const auto &b) { return a.second > b.second; });
		const std::size_t n = rejects.size() < static_cast<std::size_t>(kRejectReasonsLogged)
			? rejects.size() : static_cast<std::size_t>(kRejectReasonsLogged);
		for (std::size_t i = 0; i < n; ++i)
			STRAY_LOG_ERROR("GBUF   rejected %llux: %s",
				static_cast<unsigned long long>(rejects[i].second),
				rejects[i].first.c_str());
		if (rejects.empty())
			STRAY_LOG_ERROR("GBUF   (no wide set reached classification in this window)");
	}
}

} // namespace stray_dlss::gbuffer_finder
