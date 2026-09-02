#include "core/fg_plan.hpp"

#include <cmath>

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
	case CropVerdict::first: return "first-look";
	case CropVerdict::suspect: return "suspect-unchanged";
	default: return "?";
	}
}

CropWeight crop_weight(CropVerdict v)
{
	switch (v)
	{
	case CropVerdict::ok:
	case CropVerdict::identical: return CropWeight::good;
	case CropVerdict::black:
	case CropVerdict::stale: return CropWeight::bad;
	default: return CropWeight::neutral;
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
	else if (!have_prev)
	{
		verdict = CropVerdict::first;
	}
	else
	{
		const bool real_moved = real.hash != prev_real_hash;
		const bool generated_moved = generated.hash != prev_gen_hash && (!have_prev2 || generated.hash != prev_gen_hash2);
		if (real_moved && !generated_moved)
		{
			++stale_run;
			verdict = stale_run >= stale_limit ? CropVerdict::stale : CropVerdict::suspect;
		}
		else
		{
			stale_run = 0;
			verdict = generated.hash == real.hash ? CropVerdict::identical : CropVerdict::ok;
		}
	}
	if (verdict != CropVerdict::black)
	{
		// A black look is not a reference for the next one: the first valid look after it is
		// a first look again.
		prev_gen_hash2 = prev_gen_hash;
		have_prev2 = have_prev;
		prev_gen_hash = generated.hash;
		prev_real_hash = real.hash;
		have_prev = true;
	}
	return verdict;
}

void CropJudge::reset()
{
	stale_run = 0;
	prev_gen_hash = prev_gen_hash2 = prev_real_hash = 0;
	have_prev = have_prev2 = false;
}

} // namespace stray_dlss::core::fg

namespace stray_dlss::core::fg {

bool invert4x4(const float m[16], float out[16])
{
	float inv[16];
	inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
	inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
	inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
	inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
	inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
	inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
	inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
	inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
	inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
	inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
	inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
	inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
	inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
	inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
	inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
	inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
	const double det = static_cast<double>(m[0]) * inv[0] + static_cast<double>(m[1]) * inv[4] + static_cast<double>(m[2]) * inv[8] + static_cast<double>(m[3]) * inv[12];
	if (det == 0.0 || det != det)
		return false;
	const double r = 1.0 / det;
	for (int i = 0; i < 16; ++i)
		out[i] = static_cast<float>(inv[i] * r);
	return true;
}

float vertical_fov_radians(const float view_to_clip[16])
{
	const float m11 = view_to_clip[5];
	if (!(m11 > 0.0f))
		return 0.0f;
	// 2 * atan(1 / m11), without <cmath> dependencies in the header: use a series-free
	// identity through the standard library here.
	return 2.0f * static_cast<float>(std::atan(1.0 / static_cast<double>(m11)));
}

CameraBasis camera_basis(const float m[16])
{
	CameraBasis b;
	for (int r = 0; r < 3; ++r)
	{
		b.right[r] = m[r * 4 + 0];
		b.up[r] = m[r * 4 + 1];
		b.fwd[r] = m[r * 4 + 2];
	}
	return b;
}

} // namespace stray_dlss::core::fg
