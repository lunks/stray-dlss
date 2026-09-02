#include "core/fg_plan.hpp"

namespace stray_dlss::core::fg {

// ---- GameIndexMirror ----

void GameIndexMirror::reset(std::uint32_t buffer_count)
{
	count = buffer_count;
	index = 0;
	presents = 0;
}

std::uint32_t GameIndexMirror::on_present()
{
	++presents;
	if (count == 0)
		return 0;
	index = (index + 1) % count;
	return index;
}

// ---- Pacer ----

std::uint64_t Pacer::on_game_present(std::uint64_t now_ns)
{
	if (last_present_ns != 0 && now_ns > last_present_ns)
	{
		const std::uint64_t d = now_ns - last_present_ns;
		if (d >= cfg.hitch_ns)
		{
			++hitches; // a stall, not cadence: leave the estimate alone
		}
		else
		{
			++samples;
			interval_ns = interval_ns == 0.0 ? static_cast<double>(d)
			                                 : cfg.alpha * static_cast<double>(d) + (1.0 - cfg.alpha) * interval_ns;
		}
	}
	last_present_ns = now_ns;
	if (interval_ns <= 0.0)
		return 0;
	double delay = cfg.real_fraction * interval_ns;
	if (delay < static_cast<double>(cfg.min_delay_ns))
		delay = static_cast<double>(cfg.min_delay_ns);
	if (delay > static_cast<double>(cfg.max_delay_ns))
		delay = static_cast<double>(cfg.max_delay_ns);
	return static_cast<std::uint64_t>(delay);
}

void Pacer::reset()
{
	interval_ns = 0.0;
	last_present_ns = 0;
	samples = 0;
	hitches = 0;
}

// ---- IntervalHistogram ----

void IntervalHistogram::add(std::uint64_t interval_ns)
{
	std::uint64_t ms = interval_ns / 1'000'000;
	if (ms >= kBuckets - 1)
		ms = kBuckets - 1;
	++bucket[ms];
	++total;
	sum_ns += interval_ns;
}

void IntervalHistogram::clear()
{
	for (std::uint64_t &b : bucket)
		b = 0;
	total = 0;
	sum_ns = 0;
}

std::uint32_t IntervalHistogram::percentile_ms(double fraction) const
{
	if (total == 0)
		return kBuckets - 1;
	const double target = fraction * static_cast<double>(total);
	std::uint64_t seen = 0;
	for (std::uint32_t i = 0; i < kBuckets; ++i)
	{
		seen += bucket[i];
		if (static_cast<double>(seen) >= target)
			return i;
	}
	return kBuckets - 1;
}

double IntervalHistogram::mean_ms() const
{
	return total == 0 ? 0.0 : static_cast<double>(sum_ns) / static_cast<double>(total) / 1e6;
}

int IntervalHistogram::second_peak_ms(double min_share) const
{
	if (total == 0)
		return -1;
	// The tallest bucket, then the tallest bucket at least 2 ms away from it.
	std::uint32_t first = 0;
	for (std::uint32_t i = 1; i < kBuckets; ++i)
		if (bucket[i] > bucket[first])
			first = i;
	int second = -1;
	for (std::uint32_t i = 0; i < kBuckets; ++i)
	{
		const std::uint32_t distance = i > first ? i - first : first - i;
		if (distance < 2)
			continue;
		if (second < 0 || bucket[i] > bucket[static_cast<std::uint32_t>(second)])
			second = static_cast<int>(i);
	}
	if (second < 0)
		return -1;
	const double share_first = static_cast<double>(bucket[first]) / static_cast<double>(total);
	const double share_second = static_cast<double>(bucket[static_cast<std::uint32_t>(second)]) / static_cast<double>(total);
	return (share_first >= min_share && share_second >= min_share) ? second : -1;
}

// ---- decide ----

const char *refusal_name(Refusal r)
{
	switch (r)
	{
	case Refusal::none: return "none";
	case Refusal::off: return "off";
	case Refusal::not_armed: return "not-armed";
	case Refusal::suspended: return "suspended";
	case Refusal::stale_epoch: return "stale-epoch";
	case Refusal::no_previous_frame: return "no-previous-frame";
	case Refusal::not_validated: return "not-validated";
	case Refusal::source_missing: return "source-missing";
	default: return "?";
	}
}

Refusal decide(const PresentState &s)
{
	if (!s.enabled)
		return Refusal::off;
	if (!s.armed)
		return Refusal::not_armed;
	if (s.suspended)
		return Refusal::suspended;
	if (!s.have_previous_frame)
		return Refusal::no_previous_frame;
	if (!s.generated_valid)
		return Refusal::source_missing;
	if (s.generated_epoch != s.epoch)
		return Refusal::stale_epoch;
	if (!s.validated)
		return Refusal::not_validated;
	return Refusal::none;
}

// ---- CropJudge ----

const char *crop_verdict_name(CropVerdict v)
{
	switch (v)
	{
	case CropVerdict::ok: return "ok";
	case CropVerdict::black: return "BLACK";
	case CropVerdict::stale: return "STALE";
	case CropVerdict::identical: return "identical-to-real";
	default: return "?";
	}
}

CropVerdict CropJudge::judge(const CropStats &generated, const CropStats &real)
{
	CropVerdict verdict = CropVerdict::ok;
	if (generated.total == 0 ||
		static_cast<double>(generated.nonzero) < black_fraction * static_cast<double>(generated.total))
	{
		verdict = CropVerdict::black;
	}
	else if (have_prev)
	{
		const bool real_moved = real.hash != prev_real_hash;
		const bool generated_moved = generated.hash != prev_gen_hash;
		if (real_moved && !generated_moved)
			++stale_run;
		else
			stale_run = 0;
		if (stale_run >= stale_limit)
			verdict = CropVerdict::stale;
		else if (generated.hash == real.hash)
			verdict = CropVerdict::identical;
	}
	prev_gen_hash = generated.hash;
	prev_real_hash = real.hash;
	have_prev = true;
	return verdict;
}

void CropJudge::reset()
{
	stale_run = 0;
	prev_gen_hash = prev_real_hash = 0;
	have_prev = false;
}

} // namespace stray_dlss::core::fg
