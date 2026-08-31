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

namespace stray_dlss::gbuffer_finder {
namespace {

// The stock base pass binds SceneColor + GBufferA/B/C + velocity = 5 RTVs
// (core/gbuffer_classify.hpp); everything else in a UE4 frame binds 0-2. Gating on the
// COUNT argument alone means no view is ever described for the frame's hundreds of narrow
// binds. 4 rather than 5 so a licensee set one target short is still classified — and
// rejected with a reason — instead of being invisible.
constexpr uint32_t kMinMrtCount = 4;

// The identification must hold this many consecutive frames before it is logged. Half a
// second at 60 fps: long enough to outlive transient binds and resolution churn, short
// enough that one short observation run settles it.
constexpr std::uint64_t kStableFrames = 30;

// No base-pass candidate for this long logs a FAILED line: ten seconds at 60 fps, far
// longer than any loading hitch. The main menu is EXPECTED to trip it (no base pass runs
// there — the shader census is ~150 against ~728 in gameplay, CLAUDE.md §2.3.1), so the
// line says so rather than crying wolf.
constexpr std::uint64_t kFailFrames = 600;

// Log budgets, in the pass_finder mould: a verdict repeated every stability window would
// drown the very log the user pastes back.
constexpr int kReportBudget = 8;
constexpr int kFailBudget = 3;
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

// Cross-frame stability. Identity is (slot, resource, format) per member — by register
// first, never by pointer alone (CLAUDE.md §2.9).
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
int g_reports_logged = 0;
int g_fail_reports_logged = 0;

// Cumulative diagnostics so the FAILED report can name what was missing.
std::uint64_t g_wide_sets_seen = 0;          // binds with >= kMinMrtCount RTVs
std::uint64_t g_wide_sets_with_velocity = 0; // of those, containing the velocity format
std::unordered_map<std::string, std::uint64_t> g_reject_tally;

bool same_targets(const GBufferClassification &a, const GBufferClassification &b)
{
	if (a.targets.size() != b.targets.size())
		return false;
	for (std::size_t i = 0; i < a.targets.size(); ++i)
	{
		const BoundTexture &x = a.targets[i].tex;
		const BoundTexture &y = b.targets[i].tex;
		if (x.slot != y.slot || x.resource != y.resource || x.format != y.format)
			return false;
	}
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
		if (same_targets(c.cls, lr.pending))
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

} // namespace

void set_enabled(bool value)
{
	g_enabled = value;
	if (value)
		STRAY_LOG_WARN("GBUFFER FINDER enabled ([STRAYDLSS] GBufferFinder): classifying every "
			"render-target set with >= %u targets against the stock UE 4.27 base-pass layout "
			"and LOGGING the identification once it holds %llu consecutive frames. Log-only; "
			"nothing acts on it. (DLSS-RR phase 1)", kMinMrtCount,
			static_cast<unsigned long long>(kStableFrames));
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
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		const auto it = g_lists.find(cmd_list);
		if (it != g_lists.end())
		{
			flush_pending_locked(it->second);
			it->second.has_pending = false;
		}
	}

	if (count < kMinMrtCount)
		return;

	// describe_bound_view is liveness-checked FIRST, like every view this project touches:
	// ReShade's view->resource map outlives the resource on D3D12. (frame_state.hpp,
	// CLAUDE.md §5 "Two descriptor hazards".) Done outside our lock — it takes
	// frame_state's own.
	reshade::api::device *device = cmd_list->get_device();
	std::vector<BoundTexture> rts;
	for (uint32_t i = 0; i < count; ++i)
		describe_bound_view(device, rtvs[i], i, rts);
	if (dsv.handle != 0)
		describe_bound_view(device, dsv, count, rts);

	GBufferClassification cls = classify_render_target_set(rts);

	std::lock_guard<std::mutex> lock(g_mutex);

	++g_wide_sets_seen;
	for (const auto &t : rts)
		if (t.format == TexFormat::r16g16b16a16_unorm && (t.width > 1 || t.height > 1))
		{
			++g_wide_sets_with_velocity;
			break;
		}

	if (!cls.is_base_pass)
	{
		++g_reject_tally[cls.reason];
		return;
	}

	ListRecord &lr = g_lists[cmd_list];
	lr.pending = std::move(cls);
	lr.has_pending = true;
	lr.pending_draws = 0;
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
	GBufferClassification snap;
	std::vector<char> corro;
	std::uint64_t consecutive = 0;
	std::uint32_t draws = 0;
	bool denoiser_run = false;
	std::uint64_t old_stable = 0;
	std::uint64_t wide = 0, wide_vel = 0;
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
			if (g_frames_without_candidate == kFailFrames &&
				g_fail_reports_logged < kFailBudget)
			{
				++g_fail_reports_logged;
				log_failed = true;
				wide = g_wide_sets_seen;
				wide_vel = g_wide_sets_with_velocity;
				rejects.assign(g_reject_tally.begin(), g_reject_tally.end());
			}
		}
		else
		{
			g_frames_without_candidate = 0;

			if (g_have_current && same_targets(best->cls, g_current))
			{
				++g_consecutive;
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
			}
		}

		g_frame_candidates.clear();
		g_denoiser_srvs.clear();
		g_denoiser_seen_this_frame = false;
	}

	if (log_changed)
		STRAY_LOG_WARN("GBUFFER FINDER: identification CHANGED at frame %llu after %llu "
			"stable frames (resolution change, or a different scene's allocation). The new "
			"set reports when it has held %llu frames.",
			static_cast<unsigned long long>(frame),
			static_cast<unsigned long long>(old_stable),
			static_cast<unsigned long long>(kStableFrames));

	if (log_stable)
	{
		STRAY_LOG_INFO("======== GBUFFER FINDER  frame %llu: identification STABLE for %llu "
			"frames ========", static_cast<unsigned long long>(frame),
			static_cast<unsigned long long>(consecutive));
		STRAY_LOG_INFO("base pass: %zu targets at %ux%u (scene-buffer extent), %u draws this "
			"frame, stock roles matched %d/4", snap.targets.size(), snap.extent_width,
			snap.extent_height, draws, snap.stock_roles_matched);
		for (std::size_t i = 0; i < snap.targets.size(); ++i)
		{
			const GBufferTarget &t = snap.targets[i];
			STRAY_LOG_INFO("  slot %u  %-12s res=0x%016llx  %-12s %ux%u  stock=%-3s "
				"ssr-srv=%s%s%s", t.tex.slot, gbuffer_role_name(t.role),
				static_cast<unsigned long long>(t.tex.resource),
				tex_format_name(t.tex.format), t.tex.width, t.tex.height,
				t.stock_format ? "yes" : "NO",
				(i < corro.size() && corro[i] != 0) ? "YES" : "no",
				t.note[0] != 0 ? "  <- " : "", t.note);
		}
		if (snap.bc_order_by_slot_only)
			STRAY_LOG_INFO("  B/C order: both 8-bit RGBA - assigned by the stock slot order "
				"ALONE (SOFT); formats cannot distinguish them. The observation run must "
				"confirm which is base colour.");
		STRAY_LOG_INFO("  ssr cross-check: denoiser 0x%016llx dispatched during this run: "
			"%s%s", static_cast<unsigned long long>(kDenoiserLookalikeHash),
			denoiser_run ? "yes" : "no",
			denoiser_run ? "" :
			" (its hash is configuration-specific, CLAUDE.md §2.3; absence proves nothing)");
		STRAY_LOG_INFO("  log-only: nothing acts on this identification yet (DLSS-RR "
			"phase 1)");
		STRAY_LOG_INFO("====================================================");
	}

	if (log_failed)
	{
		STRAY_LOG_ERROR("GBUFFER FINDER FAILED: no base-pass candidate for %llu consecutive "
			"frames (at frame %llu).", static_cast<unsigned long long>(kFailFrames),
			static_cast<unsigned long long>(frame));
		STRAY_LOG_ERROR("  binds with >=%u render targets so far: %llu; of those containing "
			"an R16G16B16A16_UNORM member: %llu", kMinMrtCount,
			static_cast<unsigned long long>(wide),
			static_cast<unsigned long long>(wide_vel));
		if (wide == 0)
			STRAY_LOG_ERROR("  no wide MRT set was ever bound: main menu or loading screen "
				"(no base pass runs there), or this configuration does not render a "
				"deferred base pass.");
		else if (wide_vel == 0)
			STRAY_LOG_ERROR("  wide MRT sets exist but none carries the velocity format: "
				"r.BasePassOutputsVelocity is not in effect here, or velocity uses a "
				"different format than the measured R16G16B16A16_UNORM (CLAUDE.md §2.5).");
		std::sort(rejects.begin(), rejects.end(),
			[](const auto &a, const auto &b) { return a.second > b.second; });
		const std::size_t n = rejects.size() < static_cast<std::size_t>(kRejectReasonsLogged)
			? rejects.size() : static_cast<std::size_t>(kRejectReasonsLogged);
		for (std::size_t i = 0; i < n; ++i)
			STRAY_LOG_ERROR("  rejected %llux: %s",
				static_cast<unsigned long long>(rejects[i].second),
				rejects[i].first.c_str());
	}
}

} // namespace stray_dlss::gbuffer_finder
