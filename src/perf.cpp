#include "perf.hpp"

#include "log.hpp"

#include <atomic>
#include <chrono>

namespace stray_dlss::perf {
namespace {

// 600 presents: ~10s at 60fps, ~20s at 30fps. Long enough that one hitch does not dominate the
// average, short enough that a menu/gameplay transition shows up as its own interval.
constexpr std::uint64_t kReportInterval = 600;

using Clock = std::chrono::steady_clock;

std::atomic<bool> g_enabled{ true };

// Bucket accumulators, nanoseconds, reset each interval.
std::atomic<std::uint64_t> g_bucket_ns[kBucketCount] = {};
std::atomic<std::uint64_t> g_counter[kCounterCount] = {};

// Frame timing. Written only by the present thread; atomics so a report can never read a torn
// value if presents ever move threads.
std::atomic<std::uint64_t> g_frames_in_interval{ 0 };
std::atomic<std::uint64_t> g_worst_frame_ns{ 0 };
// Frame-time histogram, 1 ms bins 0..127 plus a 128+ overflow bin, so a hitch shows up as a
// PERCENTILE rather than being hidden by the average. Written only by the present thread.
constexpr int kHistBins = 129;
std::uint64_t g_frame_hist[kHistBins] = {};
std::uint64_t g_interval_start_ns = 0;
std::uint64_t g_last_present_ns = 0;
std::uint64_t g_present_count = 0;
std::uint64_t g_total_frames_reported = 0;
std::uint64_t g_last_dispatches = 0;
std::uint64_t g_last_large_dispatches = 0;
bool g_have_baseline = false;

std::uint64_t now_ns()
{
	return static_cast<std::uint64_t>(
		std::chrono::duration_cast<std::chrono::nanoseconds>(
			Clock::now().time_since_epoch()).count());
}

} // namespace

void set_enabled(bool enabled)
{
	g_enabled.store(enabled, std::memory_order_relaxed);
	if (enabled)
		STRAY_LOG_INFO("[perf] frame-time and CPU-share instrumentation ON ([STRAYDLSS] "
			"PerfLog): reports every %llu presents. Grep '[perf]'.",
			static_cast<unsigned long long>(kReportInterval));
}

bool enabled() { return g_enabled.load(std::memory_order_relaxed); }

void count(Counter c, std::uint64_t n)
{
	if (c >= 0 && c < kCounterCount)
		g_counter[c].fetch_add(n, std::memory_order_relaxed);
}

void add(Bucket bucket, std::uint64_t nanos)
{
	if (bucket >= 0 && bucket < kBucketCount)
		g_bucket_ns[bucket].fetch_add(nanos, std::memory_order_relaxed);
}

Scope::Scope(Bucket bucket) noexcept
	: bucket_(bucket), active_(g_enabled.load(std::memory_order_relaxed)),
	  start_ns_(active_ ? now_ns() : 0)
{
}

Scope::~Scope() noexcept
{
	if (!active_)
		return;
	const std::uint64_t end = now_ns();
	// Monotonic by construction (steady_clock), but guard anyway: a backwards interval would
	// wrap the unsigned accumulator into a nonsense total — exactly the "a wrong number is
	// worse than a coarse one" failure this module exists to avoid.
	if (end > start_ns_)
		add(bucket_, end - start_ns_);
}

void on_present(std::uint64_t dispatches_total, std::uint64_t large_dispatches_total)
{
	if (!g_enabled.load(std::memory_order_relaxed))
		return;

	const std::uint64_t now = now_ns();

	// The first present only establishes the baseline: there is no previous present to
	// measure against, so it contributes no frame time.
	if (!g_have_baseline)
	{
		g_have_baseline = true;
		g_interval_start_ns = now;
		g_last_present_ns = now;
		g_last_dispatches = dispatches_total;
		g_last_large_dispatches = large_dispatches_total;
		return;
	}

	const std::uint64_t frame_ns = now - g_last_present_ns;
	g_last_present_ns = now;
	g_frames_in_interval.fetch_add(1, std::memory_order_relaxed);
	if (frame_ns > g_worst_frame_ns.load(std::memory_order_relaxed))
		g_worst_frame_ns.store(frame_ns, std::memory_order_relaxed);
	{
		const std::uint64_t ms = frame_ns / 1000000ull;
		g_frame_hist[ms < 128 ? static_cast<int>(ms) : 128]++;
	}

	if (++g_present_count < kReportInterval)
		return;

	// ---- interval report ----
	const std::uint64_t frames = g_frames_in_interval.exchange(0, std::memory_order_relaxed);
	const std::uint64_t worst = g_worst_frame_ns.exchange(0, std::memory_order_relaxed);
	const std::uint64_t elapsed_ns = now - g_interval_start_ns;
	g_interval_start_ns = now;
	g_present_count = 0;

	std::uint64_t bucket_ns[kBucketCount];
	for (int i = 0; i < kBucketCount; ++i)
		bucket_ns[i] = g_bucket_ns[i].exchange(0, std::memory_order_relaxed);
	std::uint64_t cnt[kCounterCount];
	for (int i = 0; i < kCounterCount; ++i)
		cnt[i] = g_counter[i].exchange(0, std::memory_order_relaxed);

	const std::uint64_t dispatches = dispatches_total - g_last_dispatches;
	const std::uint64_t large = large_dispatches_total - g_last_large_dispatches;
	g_last_dispatches = dispatches_total;
	g_last_large_dispatches = large_dispatches_total;

	if (frames == 0 || elapsed_ns == 0)
		return;

	const double elapsed_ms = static_cast<double>(elapsed_ns) / 1e6;
	const double avg_frame_ms = elapsed_ms / static_cast<double>(frames);
	const double fps = 1000.0 / avg_frame_ms;
	const std::uint64_t first_frame = g_total_frames_reported;
	g_total_frames_reported += frames;

	// Percentiles from the histogram (1 ms resolution): the p99 and p99.9 are the frame-pacing
	// numbers the average hides. Then clear the bins for the next interval.
	std::uint64_t hist_total = 0;
	for (int i = 0; i < kHistBins; ++i)
		hist_total += g_frame_hist[i];
	const auto pctile = [&](double q) -> double {
		if (hist_total == 0) return 0.0;
		const std::uint64_t target = static_cast<std::uint64_t>(q * static_cast<double>(hist_total));
		std::uint64_t cum = 0;
		for (int i = 0; i < kHistBins; ++i) { cum += g_frame_hist[i]; if (cum >= target) return static_cast<double>(i); }
		return static_cast<double>(kHistBins - 1);
	};
	const double p50 = pctile(0.50), p95 = pctile(0.95), p99 = pctile(0.99), p999 = pctile(0.999);
	std::uint64_t hitches16 = 0, hitches33 = 0;
	for (int i = 0; i < kHistBins; ++i) { if (i >= 16) hitches16 += g_frame_hist[i]; if (i >= 33) hitches33 += g_frame_hist[i]; }
	for (int i = 0; i < kHistBins; ++i) g_frame_hist[i] = 0;

	STRAY_LOG_INFO("[perf] frames %llu-%llu: %.1f fps avg (%.1fms), worst %.1fms | p50 %.0fms p95 %.0fms p99 %.0fms p99.9 %.0fms | frames>16ms %llu >33ms %llu",
		static_cast<unsigned long long>(first_frame),
		static_cast<unsigned long long>(g_total_frames_reported),
		fps, avg_frame_ms, static_cast<double>(worst) / 1e6,
		p50, p95, p99, p999,
		static_cast<unsigned long long>(hitches16), static_cast<unsigned long long>(hitches33));

	// Non-overlapping attribution: kDispatchPath CONTAINS the nested buckets, so subtract them
	// to get the intercept-only share, leaving the parts summing to the total instead of
	// double-counting. Clamped at zero — nesting makes it non-negative, but a future call site
	// outside intercept_dispatch would otherwise print a wrapped unsigned.
	const double per_frame_ms = 1.0 / (1e6 * static_cast<double>(frames));
	const double mv_ms = static_cast<double>(bucket_ns[kMvResolve]) * per_frame_ms;
	const double gbuf_ms = static_cast<double>(bucket_ns[kGBufferResolve]) * per_frame_ms;
	const double sr_ms = static_cast<double>(bucket_ns[kNgxSr]) * per_frame_ms;
	const double rr_ms = static_cast<double>(bucket_ns[kNgxRr]) * per_frame_ms;
	const double nr_ms = static_cast<double>(bucket_ns[kNgxNr]) * per_frame_ms;
	const double restore_ms = static_cast<double>(bucket_ns[kRestore]) * per_frame_ms;
	const double total_ms = static_cast<double>(bucket_ns[kDispatchPath]) * per_frame_ms;
	double intercept_ms = total_ms - (mv_ms + gbuf_ms + sr_ms + rr_ms + nr_ms + restore_ms);
	if (intercept_ms < 0.0)
		intercept_ms = 0.0;

	const auto pct = [avg_frame_ms](double ms) {
		return avg_frame_ms > 0.0 ? (100.0 * ms / avg_frame_ms) : 0.0;
	};

	STRAY_LOG_INFO("[perf] our CPU/frame: intercept %.2fms (%.0f%%), mv_resolve %.2fms, "
		"gbuf_resolve %.2fms, ngx_sr %.2fms (%.0f%%), ngx_rr %.2fms (%.0f%%), ngx_nr %.2fms "
		"(%.0f%%), restore %.2fms - total %.2fms (%.0f%% of %.1fms)",
		intercept_ms, pct(intercept_ms), mv_ms, gbuf_ms,
		sr_ms, pct(sr_ms), rr_ms, pct(rr_ms), nr_ms, pct(nr_ms), restore_ms,
		total_ms, pct(total_ms), avg_frame_ms);

	STRAY_LOG_INFO("[perf] counts/frame: %.0f dispatches, %.1f size-gated (only these are "
		"timed). CPU-side only: GPU time is NOT measured here, and the finder event taps sit "
		"outside these buckets.",
		static_cast<double>(dispatches) / static_cast<double>(frames),
		static_cast<double>(large) / static_cast<double>(frames));

	const double present_ms = static_cast<double>(bucket_ns[kPresentOwner]) * per_frame_ms;
	const double wait_ms = static_cast<double>(bucket_ns[kPresentWait]) * per_frame_ms;
	STRAY_LOG_INFO("[perf] present owner/frame: mechanics %.3fms (%.0f%%), fence-wait %.3fms (%.0f%%) "
		"- the native host's per-present ring work; nil under the ReShade host.",
		present_ms, pct(present_ms), wait_ms, pct(wait_ms));

	// The native hooks' per-call-site CPU, summed across every recording thread (UE4's RHI
	// threads included), so a figure can exceed the single-threaded share of the frame.
	const double pf = 1.0 / static_cast<double>(frames);
	const double w_ms = static_cast<double>(bucket_ns[kShadowWrite]) * per_frame_ms;
	const double c_ms = static_cast<double>(bucket_ns[kShadowCopy]) * per_frame_ms;
	const double h_ms = static_cast<double>(bucket_ns[kHeapBind]) * per_frame_ms;
	const double r_ms = static_cast<double>(bucket_ns[kRootBind]) * per_frame_ms;
	const double v_ms = static_cast<double>(bucket_ns[kResolve]) * per_frame_ms;
	STRAY_LOG_INFO("[perf] native hooks/frame: shadow-write %.3fms (%.0f views), shadow-copy %.3fms (%.0f calls, %.0f descs), "
		"heap-bind %.3fms (%.0f), root-bind %.3fms (%.0f), resolve %.3fms (%.1f) - total %.3fms (%.0f%% of %.1fms; summed over threads)",
		w_ms, static_cast<double>(cnt[kCntViews]) * pf, c_ms, static_cast<double>(cnt[kCntCopyCalls]) * pf,
		static_cast<double>(cnt[kCntCopyDescs]) * pf, h_ms, static_cast<double>(cnt[kCntHeapBinds]) * pf,
		r_ms, static_cast<double>(cnt[kCntRootBinds]) * pf, v_ms, static_cast<double>(cnt[kCntResolves]) * pf,
		w_ms + c_ms + h_ms + r_ms + v_ms, pct(w_ms + c_ms + h_ms + r_ms + v_ms), avg_frame_ms);
}

} // namespace stray_dlss::perf
