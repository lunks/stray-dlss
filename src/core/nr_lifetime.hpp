// WHEN MAY A RESOURCE THE DLSS-NR PATH HANDED TO THE GPU BE FREED?
//
// Pure, so it is unit-tested on Linux — which matters because getting it wrong is invisible.
// EvaluateFeature only RECORDS work; NGX holds no references to anything we pass it (CLAUDE.md
// §5), so a texture, a readback buffer or a descriptor heap released while that command list is
// still executing is a use-after-free with no error return. Under vkd3d-proton there is no debug
// layer to object, and the observable result is a wrong image or a GPU that leaves the bus.
//
// WHY NOW. A sibling port of this same DLSS-NR integration (RemixProjGroup/dxvk-remix, branch
// dlss-nr, commit a69254ab "Neural Uplift: fix the crash on disabling the pass") found two
// use-after-free defects that fire on exactly the enable -> disable transition:
//
//   * its staging colour copy was tracked on the command list by its image but not by its VIEW,
//     so deactivation destroyed a view an executing command buffer still referred to; and
//   * "Releasing the NGX feature on deactivation did not wait for the device. The feature owns
//     GPU resources DXVK cannot see and therefore cannot keep alive."
//
// Our shapes differ — D3D12 descriptors are not DXVK views, and our textures are refcounted —
// but the transition is the same one and the second defect is ours verbatim: ReleaseFeature was
// called inline from whatever thread asked, with in-flight evaluates behind it. This header is
// the rule that both are now decided by.
//
// THE MODEL. One monotonically increasing fence on the swapchain's own queue. At each present we
// Signal the next value; every command recorded before that Signal has therefore completed once
// GetCompletedValue() reaches it. An object retired during frame N is tagged with the value that
// present N will signal, and is freed at the first present whose completed value has passed it.
//
// UNCONFIRMED LIVE: no run on the target has exercised this yet (the box is owned by another
// agent). It is proven in CI by tests/test_nr_lifetime.cpp and, for the D3D12 half, by the
// WARP harness's refcount and validation assertions.
#pragma once

#include <cstdint>

namespace stray_dlss::nrlife {

// Two full frame cycles of presents, the fallback when no fence can be created because the host
// gave us no command queue. Identical to ring::is_safe_to_release's rule and deliberately so —
// this codebase already uses that constant for exactly this hazard everywhere else.
constexpr std::uint32_t kNoFenceFreePresents = 6;

// What an object was retired against. `fence` 0 means no fence existed at retirement (the first
// frames of a session, or a host with no queue), in which case the present ring decides.
struct Tag
{
	std::uint64_t fence = 0;
	std::uint64_t present = 0;
};

struct Timeline
{
	// The value the NEXT present will signal, i.e. the one that covers everything recorded since
	// the previous present.
	std::uint64_t next_signal = 1;
	// High-water mark of the fence's completed value. Never decreases: see on_signalled.
	std::uint64_t completed = 0;
	// Presents observed, for the no-fence fallback.
	std::uint64_t present = 0;
	// Whether a fence has ever been signalled on this timeline.
	bool have_fence = false;
};

// The value that will cover work recorded from now until the next present.
std::uint64_t pending_signal(const Timeline &t);

// Tags work recorded right now. Deliberately never returns a tag that is already free: retirement
// is a request, and the object it names is only released by a later present.
Tag tag_now(const Timeline &t);

// The present boundary, having queued Signal(fence, pending_signal(t)) and read the fence back.
// `completed` is GetCompletedValue(); it is folded in as a high-water mark, so a device-lost
// driver reporting a smaller value cannot make already-freeable work look in-flight again.
void on_signalled(Timeline &t, std::uint64_t completed);

// The present boundary with no fence available.
void on_present_unfenced(Timeline &t);

// THE DECISION. True when the GPU can no longer be reading whatever this tag names.
bool safe_to_free(const Timeline &t, const Tag &tag);

// Feature 18's ReleaseFeature, which is stricter than a resource free in one way that matters:
// the feature owns GPU resources we cannot see and cannot keep alive by refcount, so it must be
// released only after the queue has completed the last evaluate that used it — AND only at the
// present boundary, never on the thread that flipped the ini key or the overlay checkbox.
// `at_present` is what encodes that second half; it is not a stylistic preference.
bool feature_release_ready(bool requested, bool at_present, const Timeline &t,
                           const Tag &last_use);

} // namespace stray_dlss::nrlife
