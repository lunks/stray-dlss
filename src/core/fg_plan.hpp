// Frame generation's pure arithmetic: everything the present-twice path decides from plain
// numbers, kept out of the D3D12 code so it runs in the Linux doctest lane. Nothing here
// touches a device; the present owner (src/backend_native/fg_present.cpp) is the thin shell.
//
// The design it serves (facts §31.4): the game is handed REPLACEMENT back buffers through a
// hooked IDXGISwapChain::GetBuffer, renders into them believing they are the swapchain's, and
// at each of its Presents we copy the frame it rendered into the REAL current back buffer and
// present — twice per game frame once a generated frame exists: the generated one first, the
// real one after a pacing delay. The real ring therefore advances twice per game frame while
// the game's own counter advances once; UE 4.27's D3D12 RHI never asks the swapchain for its
// index (zero GetCurrentBackBufferIndex call sites), so the two never have to agree — but WE
// have to know which replacement the game rendered into, and that is the mirror below.
#pragma once

#include <cstdint>

namespace stray_dlss::core::fg {

// ---- the game's back-buffer index, mirrored (facts §31.4, HARD from UE 4.27.2 source) ----
//
// FD3D12Viewport keeps CurrentBackBufferIndex_RHIThread: 0 after every Resize (which follows
// every ResizeBuffers and every SetFullscreenState), +1 modulo NumBackBuffers after every
// Present that reached IDXGISwapChain::Present, whatever the HRESULT (only the three fatal
// device-removed codes are inspected, and those end the process). So the mirror is exact as
// long as it advances on every Present/Present1 that reaches the hook and resets on every
// ResizeBuffers/ResizeBuffers1.
struct GameIndexMirror
{
	std::uint32_t count = 0;    // the buffer count the game asked for (its NumBackBuffers)
	std::uint32_t index = 0;    // the replacement the game is rendering into NOW
	std::uint64_t presents = 0; // Presents since the last reset (diagnostics)

	// Creation and every ResizeBuffers*: the game re-fetches every buffer and starts at 0.
	void reset(std::uint32_t buffer_count);
	// The replacement holding the frame the game is presenting right now.
	std::uint32_t current() const { return index; }
	// Called once per Present/Present1 that reached the hook, AFTER current() was consumed.
	// Returns the new index.
	std::uint32_t on_present();
	bool valid() const { return count != 0; }
};

// ---- pacing ----
//
// The generated frame is presented at the game's Present; the real frame follows after a
// delay that lands it in the middle of the game's frame interval. The interval is an EMA of
// the game's own present cadence (the only clock we have without Reflex). A hitch — an
// interval far above the estimate — must not drag the EMA (a 300 ms load stall would then
// hold the real frame back for 150 ms on the next frame), so intervals above `hitch_ns` are
// counted, not folded in.
struct PacerConfig
{
	double alpha = 0.1;                       // EMA weight of the newest interval
	double real_fraction = 0.5;               // delay = fraction * interval
	std::uint64_t min_delay_ns = 0;
	std::uint64_t max_delay_ns = 25'000'000;  // never hold the real frame more than 25 ms
	std::uint64_t hitch_ns = 100'000'000;     // an interval above this is a hitch, not cadence
};

struct Pacer
{
	PacerConfig cfg;
	double interval_ns = 0.0;        // the EMA; 0 = no estimate yet
	std::uint64_t last_present_ns = 0;
	std::uint64_t samples = 0;
	std::uint64_t hitches = 0;

	// Feed the game's Present timestamp; returns the delay (ns) to hold the REAL frame for
	// after the generated one, 0 when there is no estimate yet.
	std::uint64_t on_game_present(std::uint64_t now_ns);
	void reset();
};

// A 1 ms histogram of present-to-present intervals. Two of them are kept: the game's own
// cadence and the intervals between the presents WE issue — a correctly paced 2x sequence
// makes the second unimodal around half the first; back-to-back presents make it bimodal
// (near 0 and near the game interval). That distinction is stage 2's pacing verdict.
struct IntervalHistogram
{
	static constexpr std::uint32_t kBuckets = 65; // 0..63 ms, plus 64 = ">= 64 ms"
	std::uint64_t bucket[kBuckets] = {};
	std::uint64_t total = 0;
	std::uint64_t sum_ns = 0;

	void add(std::uint64_t interval_ns);
	void clear();
	// The bucket (ms) at or below which `fraction` (0..1) of samples fall; kBuckets-1 if none.
	std::uint32_t percentile_ms(double fraction) const;
	double mean_ms() const;
	// Bimodality: the share of samples in the two most populated non-adjacent buckets is
	// large AND both hold at least `min_share` — the back-to-back signature. Returns the
	// second peak's bucket, or -1 when unimodal.
	int second_peak_ms(double min_share = 0.15) const;
};

// ---- the resize / fullscreen epoch ----
//
// Every ResizeBuffers, ResizeBuffers1, SetFullscreenState and ResizeTarget invalidates the
// replacement buffers and anything in flight. An epoch counter lets work armed under one
// epoch be refused under the next instead of touching a dead texture.
struct Epoch
{
	std::uint64_t value = 0;
	bool suspended = false; // a reconfiguration is in progress: nothing may present

	void begin_reconfigure() { suspended = true; }
	void end_reconfigure() { ++value; suspended = false; }
	bool live(std::uint64_t armed_epoch) const { return !suspended && armed_epoch == value; }
};

// ---- the per-present decision ----
enum class Refusal
{
	none = 0,
	off,                // FG disabled
	not_armed,          // no replacement buffers (the game has not called GetBuffer yet)
	suspended,          // a resize / fullscreen change is in progress
	stale_epoch,        // the generated frame was made before a reconfiguration
	no_previous_frame,  // the first frame after a reset: nothing to interpolate from
	not_validated,      // the generated frame has not passed the black/stale check yet
	source_missing,     // stage 2: the evaluate did not run this frame
	count
};
const char *refusal_name(Refusal r);

struct PresentState
{
	bool enabled = false;
	bool armed = false;
	bool suspended = false;
	std::uint64_t epoch = 0;
	std::uint64_t generated_epoch = 0;
	bool have_previous_frame = false;
	bool generated_valid = false; // the generated image exists for THIS present
	bool validated = false;       // the black/stale gate passed (stage 2) or is not required
};

// Whether to present a generated frame before the real one this Present, and why not.
Refusal decide(const PresentState &s);

// ---- the generated-frame gate (prime directive 2: never present black or stale silently) ----
//
// A crop of the generated frame and of the real frame is read back a few presents later and
// reduced to these numbers. The judge is pure so its thresholds are tested.
struct CropStats
{
	std::uint64_t hash = 0;    // FNV-1a over the crop's bytes
	std::uint32_t nonzero = 0; // pixels whose RGB is not all zero
	std::uint32_t total = 0;
};

enum class CropVerdict
{
	ok = 0,      // the generated crop moved along with the real one (or neither moved)
	black,       // (nearly) every pixel is zero: the evaluate wrote nothing
	stale,       // the real frame changed but the generated one did not, `stale_limit` times running
	identical,   // generated == real byte for byte (a copy, not an interpolation) — informational, counts as ok
	first,       // the first look: nothing to compare against; neutral
	suspect,     // the real frame moved and the generated one did not, but not yet `stale_limit` times; neutral
	dark,        // BOTH crops are black (a loading screen, a fade): a black generated frame is correct; neutral
	count
};
// Whether a verdict counts TOWARD validation (ok, identical), AGAINST it (black, stale) or
// neither (first, suspect). A stale output must never validate on the looks that precede its
// STALE verdict — CI caught exactly that (a stale generated frame reached the screen after two
// "ok" looks), hence the neutral class.
enum class CropWeight { good, neutral, bad };
CropWeight crop_weight(CropVerdict v);
const char *crop_verdict_name(CropVerdict v);

struct CropJudge
{
	std::uint32_t stale_run = 0;       // consecutive "real moved, generated did not"
	std::uint32_t stale_limit = 3;     // stale after this many in a row
	double black_fraction = 0.01;      // black when nonzero/total is below this
	std::uint64_t prev_gen_hash = 0;
	std::uint64_t prev_gen_hash2 = 0; // the look before that: two alternating output textures must not read as motion
	std::uint64_t prev_real_hash = 0;
	bool have_prev = false;
	bool have_prev2 = false;

	// "Generated moved" means the crop differs from BOTH previous generated looks: the present
	// path alternates two output textures, so a generator that writes nothing shows two stale
	// images in turn, and a one-look memory would call that motion (CI caught it).
	CropVerdict judge(const CropStats &generated, const CropStats &real);
	void reset();
};

} // namespace stray_dlss::core::fg

// ---- the camera constants DLSS-G takes (facts §31.2), from the View CB's matrices ----
namespace stray_dlss::core::fg {

// Row-major 4x4 inverse by cofactors. False (out untouched) when singular.
bool invert4x4(const float m[16], float out[16]);

// Vertical field of view in RADIANS from a row-major perspective ViewToClip whose [1][1] is
// 1/tan(fov/2) (UE4's, jitter-free). 0 when the entry is not positive.
float vertical_fov_radians(const float view_to_clip[16]);

// The camera's world-space right/up/forward from UE4's TranslatedWorldToView (row-major,
// row-vector convention: view = world * M, so the COLUMNS of the 3x3 are the view axes in
// world space). Translated world is centred on the camera, so the position is the origin.
struct CameraBasis
{
	float right[3] = {};
	float up[3] = {};
	float fwd[3] = {};
};
CameraBasis camera_basis(const float translated_world_to_view[16]);

} // namespace stray_dlss::core::fg
