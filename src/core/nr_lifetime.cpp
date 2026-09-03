#include "core/nr_lifetime.hpp"

#include "core/ring.hpp"

static_assert(stray_dlss::nrlife::kNoFenceFreePresents == stray_dlss::ring::kFrameCount * 2,
	"the no-fence fallback must be the same two-frame-cycle rule ring::is_safe_to_release uses");

namespace stray_dlss::nrlife {

std::uint64_t pending_signal(const Timeline &t) { return t.next_signal; }

Tag tag_now(const Timeline &t)
{
	Tag tag;
	// Without a fence there is no value to wait on, so the tag records only the present index and
	// the ring decides. Writing next_signal here anyway would be worse than useless: nothing will
	// ever signal it, so the object would never be freed at all.
	tag.fence = t.have_fence ? t.next_signal : 0;
	tag.present = t.present;
	return tag;
}

void on_signalled(Timeline &t, std::uint64_t completed)
{
	t.have_fence = true;
	if (completed > t.completed)
		t.completed = completed;
	++t.next_signal;
	++t.present;
}

void on_present_unfenced(Timeline &t) { ++t.present; }

bool safe_to_free(const Timeline &t, const Tag &tag)
{
	if (tag.fence != 0)
		return t.completed >= tag.fence;
	// Tagged before any fence existed. A fence appearing later says nothing about work that was
	// never covered by one, so this stays on the conservative present ring.
	return t.present >= tag.present + kNoFenceFreePresents;
}

bool feature_release_ready(bool requested, bool at_present, const Timeline &t, const Tag &last_use)
{
	return requested && at_present && safe_to_free(t, last_use);
}

} // namespace stray_dlss::nrlife
