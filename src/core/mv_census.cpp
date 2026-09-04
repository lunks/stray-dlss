#include "core/mv_census.hpp"

#include <cstdio>

namespace stray_dlss::mvcensus {

const char *const kCounterNames[kCounterCount] = {
	"total",
	"sparse",
	"camera",
	"camera_reject",
	"far",
	"far_reject",
	"sparse_saturated",
	"sparse_still",
	"mv_lt_025",
	"mv_lt_1",
	"mv_lt_4",
	"mv_lt_32",
	"mv_ge_32",
	"sparse_moving",
	"non_finite",
	"reserved",
};

void Census::clear() noexcept
{
	windows = 0;
	frames = 0;
	for (int i = 0; i < kCounterCount; ++i)
		c[i] = 0;
}

void Census::add(const std::uint32_t *raw, std::uint64_t frames_in_window) noexcept
{
	if (raw == nullptr)
		return;
	for (int i = 0; i < kCounterCount; ++i)
		c[i] += raw[i];
	frames += frames_in_window;
	++windows;
}

const char *invariant_name(Invariant i) noexcept
{
	switch (i)
	{
	case Invariant::ok:                return "ok";
	case Invariant::empty:             return "empty";
	case Invariant::branches_dont_sum: return "branches-dont-sum";
	case Invariant::reject_exceeds:    return "reject-exceeds";
	case Invariant::subset_exceeds:    return "subset-exceeds";
	case Invariant::buckets_dont_sum:  return "buckets-dont-sum";
	case Invariant::count:             break;
	}
	return "?";
}

Invariant check(const Census &c) noexcept
{
	const std::uint64_t total = c.c[kTotal];
	if (total == 0)
		return Invariant::empty;

	// Every in-bounds pixel takes exactly one of the two branches. The shader increments
	// kTotal once and then exactly one of kSparse/kCamera, so this is not an approximation.
	if (c.c[kSparse] + c.c[kCamera] != total)
		return Invariant::branches_dont_sum;

	if (c.c[kCameraReject] > c.c[kCamera] || c.c[kFarReject] > c.c[kFar])
		return Invariant::reject_exceeds;

	if (c.c[kSparseSaturated] > c.c[kSparse] || c.c[kSparseStill] > c.c[kSparse] ||
		c.c[kSparseMoving] > c.c[kSparse] || c.c[kFar] > total || c.c[kNonFinite] > total)
		return Invariant::subset_exceeds;

	// The magnitude buckets partition every in-bounds pixel, including the rejected ones
	// (which land in the smallest bucket because a rejection emits an exact zero).
	const std::uint64_t buckets = c.c[kMvLt025] + c.c[kMvLt1] + c.c[kMvLt4] + c.c[kMvLt32] +
		c.c[kMvGe32];
	if (buckets != total)
		return Invariant::buckets_dont_sum;

	return Invariant::ok;
}

double percent(std::uint64_t num, std::uint64_t den) noexcept
{
	if (den == 0)
		return 0.0;
	return 100.0 * static_cast<double>(num) / static_cast<double>(den);
}

int format_report(char *buf, std::size_t size, const Census &c) noexcept
{
	if (buf == nullptr || size == 0)
		return 0;

	const Invariant inv = check(c);
	if (inv != Invariant::ok)
	{
		// Say nothing about the split. A broken instrument that prints percentages anyway is
		// worse than one that refuses: the percentages get quoted.
		return std::snprintf(buf, size,
			"MV CENSUS UNUSABLE (%s): windows=%llu frames=%llu total=%llu sparse=%llu "
			"camera=%llu. The counters contradict the shader's own construction, so no "
			"density number can be read out of this window.",
			invariant_name(inv),
			static_cast<unsigned long long>(c.windows),
			static_cast<unsigned long long>(c.frames),
			static_cast<unsigned long long>(c.c[kTotal]),
			static_cast<unsigned long long>(c.c[kSparse]),
			static_cast<unsigned long long>(c.c[kCamera]));
	}

	const std::uint64_t total = c.c[kTotal];
	return std::snprintf(buf, size,
		"MV CENSUS over %llu frames (%llu px):\n"
		"  engine-written %.2f%%   reconstructed %.2f%%\n"
		"  of engine-written: genuinely moving %.3f%%  still %.2f%%  UNORM-clamped %.4f%%\n"
		"  of reconstructed: w<=0 rejected (emitted ZERO) %.4f%%\n"
		"  far/sky (device_z==0) %.2f%% of frame, %.4f%% of those rejected\n"
		"  non-finite %llu (must be 0)\n"
		"  |mv| px: <0.25 %.2f%%  <1 %.2f%%  <4 %.2f%%  <32 %.2f%%  >=32 %.3f%%",
		static_cast<unsigned long long>(c.frames),
		static_cast<unsigned long long>(total),
		percent(c.c[kSparse], total),
		percent(c.c[kCamera], total),
		percent(c.c[kSparseMoving], c.c[kSparse]),
		percent(c.c[kSparseStill], c.c[kSparse]),
		percent(c.c[kSparseSaturated], c.c[kSparse]),
		percent(c.c[kCameraReject], c.c[kCamera]),
		percent(c.c[kFar], total),
		percent(c.c[kFarReject], c.c[kFar]),
		static_cast<unsigned long long>(c.c[kNonFinite]),
		percent(c.c[kMvLt025], total),
		percent(c.c[kMvLt1], total),
		percent(c.c[kMvLt4], total),
		percent(c.c[kMvLt32], total),
		percent(c.c[kMvGe32], total));
}

int format_status(char *buf, std::size_t size, const Census &c) noexcept
{
	if (buf == nullptr || size == 0)
		return 0;

	int written = std::snprintf(buf, size,
		"mv_census_windows=%llu\nmv_census_frames=%llu\nmv_census_invariant=%s\n",
		static_cast<unsigned long long>(c.windows),
		static_cast<unsigned long long>(c.frames), invariant_name(check(c)));
	if (written < 0)
		return written;

	for (int i = 0; i < kCounterCount; ++i)
	{
		if (i == kReserved)
			continue;
		const std::size_t used = static_cast<std::size_t>(written);
		if (used >= size)
			break;
		const int n = std::snprintf(buf + used, size - used, "mv_%s=%llu\n", kCounterNames[i],
			static_cast<unsigned long long>(c.c[i]));
		if (n < 0)
			return n;
		written += n;
	}

	// The two derived numbers the shell helpers actually grep for, so no reader has to divide.
	const std::size_t used = static_cast<std::size_t>(written);
	if (used < size)
	{
		const int n = std::snprintf(buf + used, size - used,
			"mv_engine_written_pct=%.3f\nmv_reconstructed_pct=%.3f\n",
			percent(c.c[kSparse], c.c[kTotal]), percent(c.c[kCamera], c.c[kTotal]));
		if (n > 0)
			written += n;
	}
	return written;
}

} // namespace stray_dlss::mvcensus
