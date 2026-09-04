#include "view_cached.hpp"

#include "ue4_view.hpp"

#include <cstdio>
#include <cstring>

namespace stray_dlss::viewcached {
namespace {

// The same range test L1 uses before any guarded read: reject small integers, unaligned values
// and sign-extended garbage. It says the number COULD be a pointer, never that anything is
// mapped there - that is the reader's job, and the reader is the one with VirtualQuery.
bool plausible_heap_ptr(std::uint64_t va)
{
	if (va == 0 || (va & 7ull) != 0)
		return false;
	return va >= 0x10000ull && va < 0x0000800000000000ull;
}

bool rows_equal(const unsigned char *a, const unsigned char *b, std::uint32_t first_row,
                std::uint32_t rows)
{
	return std::memcmp(a + ue4::row_to_byte(first_row), b + ue4::row_to_byte(first_row),
		std::size_t{ rows } * 16) == 0;
}

} // namespace

const char *refusal_name(Refusal r)
{
	switch (r)
	{
	case Refusal::accepted: return "accepted";
	case Refusal::not_a_pointer: return "not-a-pointer";
	case Refusal::unreadable: return "unreadable";
	case Refusal::implausible: return "implausible";
	case Refusal::row135_failed: return "row135-failed";
	case Refusal::does_not_fit: return "does-not-fit";
	case Refusal::fraction: return "below-min-fraction";
	case Refusal::buffer_mismatch: return "buffer-size-mismatch";
	default: return "?";
	}
}

const char *latch_state_name(LatchState s)
{
	switch (s)
	{
	case LatchState::searching: return "searching";
	case LatchState::latched: return "latched";
	case LatchState::ambiguous: return "ambiguous";
	case LatchState::absent: return "absent";
	default: return "?";
	}
}

Refusal check_offset(const Reader &r, std::uint64_t view, std::uint32_t offset,
                     const Expectation &exp, Candidate *out)
{
	if (r.read_u64 == nullptr || r.read_block == nullptr)
		return Refusal::unreadable;
	if (!plausible_heap_ptr(view))
		return Refusal::not_a_pointer;

	// 1. The TUniquePtr's one pointer, then the pointee for the whole prefix.
	std::uint64_t pointee = 0;
	if (!r.read_u64(r.ctx, view + offset, &pointee))
		return Refusal::unreadable;
	if (!plausible_heap_ptr(pointee))
		return Refusal::not_a_pointer;
	// The struct's first member is a 16-byte-aligned FMatrix; an unaligned pointee is not it.
	if ((pointee & 15ull) != 0)
		return Refusal::not_a_pointer;

	unsigned char prefix[ue4::kViewPrefixBytes];
	if (!r.read_block(r.ctx, pointee, sizeof(prefix), prefix))
		return Refusal::unreadable;

	// 2-3. The contents. `view_params_plausible` rejects a block that is not a View at all
	// (rows 130 and 152 must be reciprocals and sub-pixel jitter); row 135 rejects one read at
	// a slipped offset - (denormal, P, 1/P, 0.0) with y*z == 1.0 true by construction.
	ue4::ViewParams params{};
	if (!ue4::parse_view_params(prefix, sizeof(prefix), params))
		return Refusal::unreadable;
	if (!ue4::view_params_plausible(params))
		return Refusal::implausible;
	if (!ue4::pre_exposure_plausible(params))
		return Refusal::row135_failed;

	// 4. The engine's own rect for this announcement, from above and from below. Skipped when
	// it was not readable: inventing a refusal from a missing input is worse than not testing.
	if (exp.out_width != 0 && exp.out_height != 0)
	{
		if (!ue4::view_fits_dispatch(params, exp.out_width, exp.out_height))
			return Refusal::does_not_fit;
		if (!ue4::view_fraction_plausible(params, exp.out_width, exp.out_height))
			return Refusal::fraction;
	}

	// 5. Row 132 against the depth texture L1 resolved for the same announcement. HARD that
	// BufferSizeAndInvSize is SceneContext.GetBufferSizeXY() (FViewInfo::SetupUniformBuffer-
	// Parameters, 4.27.2 mirror); [derived] that SceneDepthZ is allocated at that extent. A
	// mismatch is therefore a refusal WITH ITS OWN STAGE COUNTER, so a wrong derivation reads
	// as "every candidate dies at `buffer`" rather than as silence.
	if (exp.buffer_width != 0 && exp.buffer_height != 0)
	{
		const float bw = params.buffer_size_and_inv_size.x;
		const float bh = params.buffer_size_and_inv_size.y;
		if (bw != static_cast<float>(exp.buffer_width) || bh != static_cast<float>(exp.buffer_height))
			return Refusal::buffer_mismatch;
	}

	if (out != nullptr)
	{
		out->offset = offset;
		out->pointee = pointee;
		out->params = params;
		std::memcpy(out->prefix, prefix, sizeof(prefix));
	}
	return Refusal::accepted;
}

unsigned scan(const Reader &r, std::uint64_t view, const Expectation &exp,
              Candidate *out, unsigned max_out, StageCounts *counts, std::size_t window)
{
	StageCounts sc;
	unsigned found = 0;
	if (r.read_u64 != nullptr && r.read_block != nullptr && plausible_heap_ptr(view))
	{
		for (std::size_t o = 0; o + sizeof(std::uint64_t) <= window; o += 8)
		{
			++sc.qwords;
			// Prune before the probe: most of the window is not a pointer at all, and the
			// pointee's readability check is the expensive part of the whole scan.
			std::uint64_t q = 0;
			if (!r.read_u64(r.ctx, view + o, &q) || !plausible_heap_ptr(q) || (q & 15ull) != 0)
				continue;
			++sc.pointers;
			if (sc.probed >= kMaxProbesPerScan)
			{
				sc.truncated = true;
				break;
			}
			++sc.probed;

			Candidate c;
			const Refusal why = check_offset(r, view, static_cast<std::uint32_t>(o), exp, &c);
			// Each stage counts the candidates that PASSED it, so the last non-zero stage
			// is the one that refused everything.
			if (why == Refusal::unreadable || why == Refusal::not_a_pointer)
				continue;
			++sc.readable;
			if (why == Refusal::implausible)
				continue;
			++sc.plausible;
			if (why == Refusal::row135_failed)
				continue;
			++sc.row135;
			if (why == Refusal::does_not_fit)
				continue;
			++sc.fits;
			if (why == Refusal::fraction)
				continue;
			++sc.fraction;
			if (why == Refusal::buffer_mismatch)
				continue;
			++sc.buffer;
			if (why != Refusal::accepted)
				continue;
			++sc.survivors;
			if (found < max_out && out != nullptr)
				out[found] = c;
			++found;
			// Keep scanning. Knowing there is MORE THAN ONE survivor is the point.
		}
	}
	if (counts != nullptr)
		*counts = sc;
	return found;
}

FieldDiff compare_prefix(const unsigned char *engine, const unsigned char *search, std::size_t bytes)
{
	FieldDiff d;
	if (engine == nullptr || search == nullptr || bytes == 0)
	{
		d.any = true;
		return d;
	}
	for (std::size_t i = 0; i < bytes; ++i)
	{
		if (engine[i] != search[i])
		{
			d.any = true;
			d.first_row = static_cast<std::uint32_t>(i / 16);
			break;
		}
	}
	if (!d.any)
		return d;
	if (bytes < ue4::kViewPrefixBytes)
		return d; // only the whole-prefix verdict is meaningful on a short compare
	d.clip_to_prev_clip = !rows_equal(engine, search, ue4::ViewRow::kClipToPrevClip, 4);
	d.jitter = !rows_equal(engine, search, ue4::ViewRow::kTemporalAAJitter, 1) ||
		!rows_equal(engine, search, ue4::ViewRow::kTemporalAAParams, 1);
	d.camera_cut = !rows_equal(engine, search, ue4::ViewRow::kCameraCutRow, 1);
	d.pre_exposure = !rows_equal(engine, search, ue4::ViewRow::kPreExposureRow, 1);
	d.rect = !rows_equal(engine, search, ue4::ViewRow::kViewRectMin, 1) ||
		!rows_equal(engine, search, ue4::ViewRow::kViewSizeAndInvSize, 1) ||
		!rows_equal(engine, search, ue4::ViewRow::kBufferSizeAndInvSize, 1);
	return d;
}

int describe_diff(const FieldDiff &d, char *buffer, std::size_t size)
{
	if (buffer == nullptr || size == 0)
		return 0;
	if (!d.any)
		return std::snprintf(buffer, size, "identical");
	// Appends never run past the buffer: a truncated snprintf reports the length it WANTED, so
	// the cursor is clamped to the terminator's slot before the next append.
	std::size_t n = 0;
	auto append = [&](const char *text) {
		if (n >= size - 1)
			return;
		const int wrote = std::snprintf(buffer + n, size - n, "%s", text);
		if (wrote < 0)
			return;
		n += static_cast<std::size_t>(wrote);
		if (n > size - 1)
			n = size - 1;
	};
	char head[48];
	std::snprintf(head, sizeof(head), "first differing row %u;", d.first_row);
	append(head);
	const bool named = d.clip_to_prev_clip || d.jitter || d.camera_cut || d.pre_exposure || d.rect;
	if (!named)
		append(" no consumer field differs (rows a temporal consumer never reads)");
	if (d.clip_to_prev_clip)
		append(" ClipToPrevClip");
	if (d.jitter)
		append(" jitter");
	if (d.camera_cut)
		append(" CameraCut");
	if (d.pre_exposure)
		append(" PreExposure");
	if (d.rect)
		append(" rect");
	return static_cast<int>(n);
}

void Latch::observe(const Observation &o)
{
	++observations_;

	if (o.survivors > 1)
	{
		// TWO ANSWERS IS NOT AN ANSWER. Sticky: a mechanism that resolved ambiguity by scan
		// order would reproduce, one level down, exactly the bug it was built to remove.
		++ambiguous_;
		state_ = LatchState::ambiguous;
		agreements_ = 0;
		return;
	}
	// BOTH TERMINAL STATES ARE STICKY HERE, in the state machine, rather than by the caller
	// declining to call. Every way to reach `absent` is a structural property of this
	// executable and this process - the field lies outside the window, or a reader refuses
	// everything - and none of those becomes true again after kAbsentAfter announcements.
	if (state_ == LatchState::ambiguous || state_ == LatchState::absent)
		return;

	if (o.survivors == 0)
	{
		++empty_;
		if (state_ == LatchState::latched)
		{
			// The latched offset did not verify on this announcement. The search supplied
			// that frame; the offset is not in doubt (it proved itself by bytes), so this is a
			// count and not a demotion.
			++unverified_;
			return;
		}
		agreements_ = 0;
		if (++consecutive_empty_ >= kAbsentAfter)
			state_ = LatchState::absent;
		return;
	}
	consecutive_empty_ = 0;

	if (!o.compared)
	{
		// The search found no View for this dispatch, so there is nothing to compare against.
		// Absence of evidence is not disagreement: the run is neither extended nor reset.
		++uncompared_;
		return;
	}

	if (state_ == LatchState::latched)
	{
		if (o.offset != offset_)
		{
			// Cannot happen from the live half, which only offers the latched offset once
			// latched - but the model must say what it would mean: ambiguity arriving late.
			++ambiguous_;
			state_ = LatchState::ambiguous;
			agreements_ = 0;
			return;
		}
		if (o.bytes_equal)
			++agree_;
		else
			++disagree_; // THE 0.33%: the search read a stale ring copy. The struct is right.
		return;
	}

	if (!o.bytes_equal)
	{
		// Before the latch, a wrong offset must never accumulate agreements. This also costs
		// nothing when it is the SEARCH that is stale: the run restarts and the latch lands
		// a few frames later.
		++pre_disagree_;
		agreements_ = 0;
		return;
	}
	if (agreements_ != 0 && o.offset != offset_)
		agreements_ = 0; // a different single answer: start the run again, do not average
	offset_ = o.offset;
	++agreements_;
	if (agreements_ >= kLatchAgreements)
		state_ = LatchState::latched;
}

Mode mode_from_level(int level)
{
	if (level <= 0)
		return Mode::off;
	if (level == 1)
		return Mode::discover;
	if (level == 2)
		return Mode::authoritative;
	return Mode::exclusive;
}

const char *mode_name(Mode m)
{
	switch (m)
	{
	case Mode::off: return "off";
	case Mode::discover: return "discover";
	case Mode::authoritative: return "authoritative";
	case Mode::exclusive: return "exclusive";
	default: return "?";
	}
}

bool mode_is_implemented(Mode m)
{
	return m == Mode::off || m == Mode::discover || m == Mode::authoritative;
}

bool use_engine_view(const DecisionInputs &in)
{
	if (in.mode != Mode::authoritative && in.mode != Mode::exclusive)
		return false;
	if (in.latch != LatchState::latched)
		return false;
	if (!in.carried)
		return false;
	return in.carried_offset == in.latched_offset;
}

} // namespace stray_dlss::viewcached
