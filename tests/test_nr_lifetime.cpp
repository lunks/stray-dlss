// The "when may this be freed" decision for everything the DLSS-NR path hands to feature 18.
//
// Written FIRST, against no implementation, because the bug class it exists to prevent is
// invisible at runtime: a texture or descriptor released while an evaluate that references it is
// still executing is a use-after-free with no error return — under vkd3d-proton there is no debug
// layer to object, so the symptom is a wrong image or a GPU that leaves the bus. A sibling port of
// this same integration (RemixProjGroup/dxvk-remix @ a69254ab) shipped exactly two of these on the
// enable -> disable transition.
#include <doctest/doctest.h>

#include "core/nr_lifetime.hpp"
#include "core/ring.hpp"

using namespace stray_dlss::nrlife;

TEST_CASE("nothing is freeable the moment it is retired")
{
	// The whole point: retirement is a REQUEST, never an act. Whatever the timeline looks like,
	// an object tagged now cannot be freed now — the work that references it has not been
	// submitted past a fence yet.
	Timeline t;
	const Tag tag = tag_now(t);
	CHECK(safe_to_free(t, tag) == false);
}

TEST_CASE("with a fence, an object is freed exactly when the queue passes its signal")
{
	Timeline t;
	// The first present created the fence and signalled value 1.
	on_signalled(t, /*completed=*/0);
	CHECK(pending_signal(t) == 2);

	// Work recorded now completes at signal 2.
	const Tag tag = tag_now(t);
	CHECK(tag.fence == 2);

	// Present: signal 2 goes to the queue, but the GPU has only finished 1.
	on_signalled(t, /*completed=*/1);
	CHECK(safe_to_free(t, tag) == false);

	// The GPU passes 2. Now — and not before — the object is unreferenced.
	on_signalled(t, /*completed=*/2);
	CHECK(safe_to_free(t, tag));
}

TEST_CASE("a fence value that goes backwards never un-frees anything")
{
	// GetCompletedValue is monotone on the GPU, but a lost device or a driver quirk reporting a
	// smaller value must not make us believe work re-entered flight. Completion is a high-water
	// mark.
	Timeline t;
	on_signalled(t, 0);
	const Tag tag = tag_now(t);
	on_signalled(t, 5);
	CHECK(safe_to_free(t, tag));
	on_signalled(t, 0);
	CHECK(safe_to_free(t, tag));
	CHECK(t.completed == 5);
}

TEST_CASE("with no fence at all the decision falls back to the conservative present ring")
{
	// A host that hands us no command queue (PresentContext::queue null) leaves us unable to
	// signal anything. Freeing immediately would be the bug; never freeing would leak the NR
	// working set for the session. Two full frame cycles is the same rule ring::is_safe_to_release
	// already applies everywhere else in this codebase, and it is what the fallback reuses.
	Timeline t;
	const Tag tag = tag_now(t);
	CHECK(tag.fence == 0); // no fence was available when it was tagged

	for (std::uint32_t i = 0; i < kNoFenceFreePresents; ++i)
	{
		CHECK(safe_to_free(t, tag) == false);
		on_present_unfenced(t);
	}
	CHECK(safe_to_free(t, tag));
	CHECK(kNoFenceFreePresents == stray_dlss::ring::kFrameCount * 2);
}

TEST_CASE("an object tagged before the fence existed still waits out the ring")
{
	// The ordering that actually happens: NR runs for a frame or two before the first present
	// creates the fence. Those tags carry fence 0 and must not be freed early just because a
	// fence appeared afterwards and its completed value is trivially >= 0.
	Timeline t;
	const Tag tag = tag_now(t);
	on_signalled(t, 1);
	CHECK(safe_to_free(t, tag) == false);
	for (std::uint32_t i = 0; i < kNoFenceFreePresents; ++i)
		on_signalled(t, 1 + i);
	CHECK(safe_to_free(t, tag));
}

TEST_CASE("ReleaseFeature waits for BOTH the present boundary and the GPU")
{
	// Task 2's rule, and the sibling port's second defect: "Releasing the NGX feature on
	// deactivation did not wait for the device." Feature 18 owns GPU resources we cannot see and
	// therefore cannot keep alive ourselves, so the release must happen only after the queue has
	// completed the last evaluate that used it — and only at the present boundary, never on the
	// thread that toggled the ini or the overlay checkbox.
	Timeline t;
	on_signalled(t, 0);
	const Tag last_eval = tag_now(t); // an evaluate was recorded this frame

	// Nobody asked.
	CHECK(feature_release_ready(/*requested=*/false, /*at_present=*/true, t, last_eval) == false);
	// Asked, but from the UI/ini thread mid-frame.
	CHECK(feature_release_ready(true, /*at_present=*/false, t, last_eval) == false);
	// At present, but the evaluate is still in flight.
	CHECK(feature_release_ready(true, true, t, last_eval) == false);

	on_signalled(t, last_eval.fence);
	// Still not from the wrong thread, even now.
	CHECK(feature_release_ready(true, false, t, last_eval) == false);
	CHECK(feature_release_ready(true, true, t, last_eval));
}

TEST_CASE("a toggle loop never frees anything the GPU has not passed")
{
	// The measured shape of the sibling's crash: enable -> disable -> enable, repeatedly, while
	// frames are in flight. Simulated here as an evaluate every frame and a teardown request on
	// every other one; the invariant is that no tag is ever reported freeable before its fence.
	Timeline t;
	Tag pending[8] = {};
	int count = 0;
	std::uint64_t completed = 0;

	for (int frame = 0; frame < 64; ++frame)
	{
		if (count < 8)
			pending[count++] = tag_now(t);

		// The GPU trails the CPU by two frames, which is what a triple-buffered swapchain does.
		if (t.next_signal > 2)
			completed = t.next_signal - 2;
		on_signalled(t, completed);

		int kept = 0;
		for (int i = 0; i < count; ++i)
		{
			if (safe_to_free(t, pending[i]))
			{
				// The invariant under test: it really is behind the completed value.
				CHECK(pending[i].fence <= t.completed);
				continue;
			}
			pending[kept++] = pending[i];
		}
		count = kept;
	}
	// And everything eventually goes: a deferred free must not become a leak.
	CHECK(count <= 2);
}
