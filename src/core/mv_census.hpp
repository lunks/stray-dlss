// The dense motion-vector census: what the resolve pass ACTUALLY did, per pixel, per frame.
//
// WHY THIS EXISTS. `shaders/mv_resolve.hlsl` has two branches — UE 4.27's own sparse velocity
// where the engine wrote one, and a camera reconstruction from reversed-Z depth and
// `ClipToPrevClip` everywhere else — and until now NOBODY HAD MEASURED THE SPLIT. That number
// bounds every decision downstream of it: if the engine writes 95% of the buffer, the quality
// of the reconstruction barely matters; if it writes 40%, it matters a great deal. Stray ships
// `r.BasePassOutputsVelocity=True` (CLAUDE.md §2.3.1), which broadens coverage beyond stock
// UE4 by an unknown amount, so the number cannot be reasoned out — only counted.
//
// CLAUDE.md's standing rule about this input: "bad motion vectors do not produce one bad frame,
// they compound through the accumulation", and they surface as drift and smearing rather than
// as anything that looks like a motion-vector bug. A census is the only way to tell a
// reconstruction that covers 12% of the frame from one that covers 88%.
//
// This half is PURE so CI pins it. The shader's atomics and the readback live in
// src/mv_resolve.cpp; everything about what the numbers MEAN is here.
#pragma once

#include <cstddef>
#include <cstdint>

namespace stray_dlss::mvcensus {

// The counter slots, in the order the shader writes them. THE SHADER'S ORDER IS THIS ORDER:
// shaders/mv_resolve.hlsl indexes the same buffer by these ordinals, so an insertion here
// without the matching edit there silently relabels every number. Append, never insert.
enum Counter : int
{
	kTotal = 0,       // in-bounds pixels the dispatch covered
	kSparse,          // EncodedVelocity.x > 0: the engine wrote a vector and we took it
	kCamera,          // reconstructed from depth + ClipToPrevClip
	kCameraReject,    // camera branch, prev_clip.w <= 0 -> we emit ZERO
	kFar,             // device_z == 0 exactly: reversed-Z infinite far (sky, or unwritten depth)
	kFarReject,       // of those, how many ALSO hit the w <= 0 rejection
	kSparseSaturated, // the engine's UNORM velocity hit a rail, so the decode is CLAMPED
	kSparseStill,     // sparse branch, decoded |mv| < kStillThresholdPx: written, but ~zero
	kMvLt025,         // final |mv| magnitude buckets, in RENDER pixels, over all branches
	kMvLt1,
	kMvLt4,
	kMvLt32,
	kMvGe32,
	kSparseMoving,    // sparse pixels whose vector differs from the camera reconstruction by
	                  // more than kMovingThresholdPx: geometry that is GENUINELY MOVING, as
	                  // opposed to static geometry the base pass merely happened to write
	kNonFinite,       // a NaN or Inf reached the output. Must be zero.
	kReserved,
	kCounterCount
};

// A pixel is "genuinely moving" when its engine-written vector and the camera reconstruction
// of the same pixel disagree by more than this many render pixels. Below it the engine simply
// wrote what we would have reconstructed anyway, which is what `r.BasePassOutputsVelocity=True`
// is expected to produce for a great deal of static-but-base-pass-velocity geometry.
constexpr float kMovingThresholdPx = 0.5f;

// |mv| below this counts as "written but still" — one sixteenth of a render pixel, well under
// DLSS's own sub-pixel working scale.
constexpr float kStillThresholdPx = 0.0625f;

extern const char *const kCounterNames[kCounterCount];

// Accumulated over one or more census windows. 32-bit on the GPU, widened here.
struct Census
{
	std::uint64_t windows = 0; // completed readbacks folded in
	std::uint64_t frames = 0;  // resolve dispatches those windows covered
	std::uint64_t c[kCounterCount] = {};

	void add(const std::uint32_t *raw, std::uint64_t frames_in_window) noexcept;
	void clear() noexcept;
};

// The GPU counters are uint32 and are zeroed at the end of every window. `kTotal` is the
// largest of them, so the window length is bounded by 2^32 / (render pixels per frame).
// At 1920x1080 that is ~2071 frames; the default window is far below it, but a future edit
// raising the window is exactly the kind of change that would silently wrap — so the live
// path asks this rather than trusting a comment.
constexpr std::uint64_t safe_window_frames(std::uint64_t render_pixels) noexcept
{
	return render_pixels == 0 ? 0 : (0xFFFFFFFFull / render_pixels);
}

// LEVEL 2: the census asserts itself. A readback that raced the reset, a counter ordinal that
// drifted from the shader, or a partially-executed dispatch all produce numbers that still
// LOOK like percentages, and this project's history is full of plausible-looking wrong
// measurements taken seriously. These invariants hold BY CONSTRUCTION in the shader, so a
// violation means the instrument is broken and the numbers must not be believed.
enum class Invariant : int
{
	ok = 0,
	empty,             // no pixels counted at all
	branches_dont_sum, // sparse + camera != total
	reject_exceeds,    // camera_reject > camera, or far_reject > far
	subset_exceeds,    // a subset counter exceeds the set it is drawn from
	buckets_dont_sum,  // the magnitude buckets do not sum to total
	count
};

const char *invariant_name(Invariant i) noexcept;

Invariant check(const Census &c) noexcept;

// Fraction of `den`, in percent. A zero denominator gives 0.0 rather than NaN, because this
// number goes into a log line a human reads under time pressure.
double percent(std::uint64_t num, std::uint64_t den) noexcept;

// One multi-line report. Returns what snprintf would: the length the full text would have had.
int format_report(char *buf, std::size_t size, const Census &c) noexcept;

// The stray-dlss-status.txt form: `mv_<name>=<count>` on separate lines, plus the derived
// percentages the shell helpers actually read.
int format_status(char *buf, std::size_t size, const Census &c) noexcept;

} // namespace stray_dlss::mvcensus
