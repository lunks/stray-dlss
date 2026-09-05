// Frame generation's present path: replacement back buffers, two presents per game frame, the
// pacing worker, and the loud gate that keeps a black or stale generated frame off the screen.
// Owned by the present owner (present_owner.cpp), which calls in from its swapchain hooks; the
// arithmetic is core/fg_plan.hpp (tested), this file is the D3D12/DXGI shell around it.
//
// WHAT IT DOES (facts §32.4 for the UE4 half):
//   * IDXGISwapChain::GetBuffer is hooked: the game receives OUR textures ("replacements",
//     same desc as the real back buffers) and renders into them, keeping its own index counter
//     as UE 4.27's D3D12 RHI does. The real swapchain buffers are never touched by the game.
//   * At each game Present the frame it rendered sits in replacement[mirror.current()]. We
//     produce a GENERATED frame (stage 1: the previous real frame with a magenta band, no NGX;
//     stage 2: DLSS-G's interpolation), then present twice: copy generated -> real[current],
//     Present; wait ~half a frame; copy real -> real[current], Present. The second copy and
//     both presents run on a worker thread ([STRAYDLSS] NgxFGPacing=thread) so the game is
//     not held for the pacing delay, or inline on the game thread (=sync, =none) for bisection.
//   * ResizeBuffers/ResizeBuffers1/SetFullscreenState/ResizeTarget drain the worker first and
//     bump the epoch; ResizeBuffers also drops the replacements, which GetBuffer recreates.
//
// EVERY refusal to present a generated frame is counted by reason and logged once; the status
// file and a periodic [fg] line carry the counters, the pacer's estimate and the histogram of
// the intervals between the presents WE issued (bimodal = back-to-back = not paced).
#pragma once

#include "core/fg_plan.hpp"
#include "core/fg_throttle.hpp"

#include <cstdint>

struct ID3D12Device;
struct ID3D12CommandQueue;
struct ID3D12GraphicsCommandList;
struct ID3D12Resource;
struct IDXGISwapChain;
struct IDXGISwapChain3;

namespace stray_dlss::native::fg {

enum class Mode
{
	experiment = 1, // stage 1: the generated frame is the previous real frame with a magenta band; no NGX
	ngx = 2,        // stage 2: the generated frame comes from the installed Generator (DLSS-G)
};

enum class Pacing
{
	none = 0,   // both presents back to back on the game thread (the bimodal control)
	thread = 1, // a worker thread presents: generated now, real after the pacer's delay (default)
	sync = 2,   // both on the game thread with NgxFGWaitMs between them (bisection: no second thread)
};

struct Config
{
	bool enabled = false;      // [STRAYDLSS] NgxFG
	Mode mode = Mode::ngx;     // [STRAYDLSS] NgxFGMode: 1 experiment (present-twice, no NGX), 2 ngx
	Pacing pacing = Pacing::thread; // [STRAYDLSS] NgxFGPacing: 0 none, 1 thread, 2 sync
	int wait_ms = -1;          // [STRAYDLSS] NgxFGWaitMs: sync/thread delay override; -1 = the pacer's half interval
	bool band = true;          // [STRAYDLSS] NgxFGBand: paint the magenta band on the generated frame (experiment mode)
	int validate = 1;          // [STRAYDLSS] NgxFGValidate: gate ngx-mode output on the crop readback (0 = present unvalidated, logged loudly)
	int reflex = 1;            // [STRAYDLSS] NgxFGReflex: 0 off, 1 low-latency mode + Sleep per game present + markers (fg_reflex.hpp), 2 = 1 plus boost
	int trace = 0;             // [STRAYDLSS] NgxFGTrace: N > 0 dumps a per-present timestamp trace of N consecutive presents once the first generated frame went out (pacing diagnosis)
	// The flip-queue throttle (fg_throttle.hpp). Bounds how far the PRESENT QUEUE runs ahead;
	// the pacer only decides WHEN we issue. Off by default: the shipped pacing behaviour is
	// byte-identical until NgxFGThrottle is set.
	core::fg::ThrottleConfig throttle;
	// [STRAYDLSS] NgxFGWaitableSwapChain: OR DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT
	// into the desc the GAME hands DXGI, because the throttle's waitable object cannot be added
	// after creation. Changes the swapchain the game is handed, so it is its own opt-in.
	bool waitable_swapchain = false;
	// [STRAYDLSS] NgxFGOutOfBandQueue: 0 off, 1 NvAPI OUT_OF_BAND_PRESENT (the matching type),
	// 3 OUT_OF_BAND_RENDER_PRESENT. Tidiness with a plausible benefit, never a fix (fg_reflex.hpp).
	int out_of_band_queue = 0;
};

// The stage-2 producer of generated frames. Records onto `list` (executes on the presenting
// queue after every list the game submitted this frame) whatever writes the generated image
// into `out` (a texture in the swapchain's format and size, UAV- and RT-capable, COMMON state
// on entry and exit). `real_current` is the replacement the game rendered THIS frame into
// (COMMON state; leave it so). Return false and set `why` (a static string) when nothing was
// produced — the present then refuses under `source-missing` and shows the real frame only.
struct Generator
{
	virtual ~Generator() = default;
	virtual bool generate(ID3D12GraphicsCommandList *list, ID3D12Resource *real_current,
	                      ID3D12Resource *out, std::uint32_t width, std::uint32_t height,
	                      unsigned dxgi_format, std::uint64_t frame, const char **why) = 0;
	// The game's present is the frame boundary DLSS-G keys its history on; a generator that
	// keeps state hears about every reconfiguration so it can reset.
	virtual void on_reconfigure() {}
};

void configure(const Config &cfg);
const Config &config();
bool enabled();
void set_generator(Generator *g); // null = the built-in experiment generator

// ---- from the present owner's hooks ----

// From the present owner's swapchain CREATION hooks: what core::fg::plan_creation_flags decided
// about DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT for this swapchain. Recorded so the
// [fg] line can say whether the flag we asked for was actually asked FOR, separately from
// whether the throttle then found it on the created swapchain.
void note_creation_flags(core::fg::FlagVerdict verdict);
// Whether we (not the game) added the waitable flag - the resize hooks must re-assert exactly
// what we added and nothing else.
bool added_waitable_flag();

// A swapchain was recorded (creation hook): patches GetBuffer / SetFullscreenState /
// ResizeTarget / SetColorSpace1 on its vtable when FG is enabled. Safe mid-creation (only the
// vtable is touched).
void on_swapchain_recorded(IDXGISwapChain *sc, ID3D12Device *device);
// First Present (the owner's finalisation): the device, the real queue and the SwapChain3.
void on_swapchain_finalised(IDXGISwapChain *sc, IDXGISwapChain3 *sc3, ID3D12Device *device, ID3D12CommandQueue *queue);
// Around ResizeBuffers/ResizeBuffers1 (drop=true) and SetFullscreenState/ResizeTarget (drop=false).
void before_reconfigure(IDXGISwapChain *sc, const char *what);
void after_reconfigure(IDXGISwapChain *sc, const char *what, bool drop_replacements, std::uint32_t new_count, long hr);

// The replacement the game rendered this frame into, or null when FG is not armed for `sc`.
// Non-owning. Used by the present owner as the PresentContext's back buffer. The MIRROR's answer
// below EngineSeamBackBuffer=2; the engine's above it (rhi_gfx_hook.hpp).
ID3D12Resource *game_frame(IDXGISwapChain *sc);
// The mirror's own answer regardless of level - the model, for the assertion. Null when unarmed.
ID3D12Resource *mirror_frame(IDXGISwapChain *sc);

// The game-thread half of a present: records the generated frame's production onto `list`
// (the owner's present list, executed on `queue` before anything else FG does). Returns true
// when a generated frame was produced for this present; false with the refusal counted.
bool record(IDXGISwapChain *sc, ID3D12GraphicsCommandList *list, std::uint64_t frame);

struct PresentArgs
{
	IDXGISwapChain *sc = nullptr;
	unsigned sync = 0;
	unsigned flags = 0;
	std::uint64_t frame = 0;
	void *orig_present = nullptr; // the per-vtable original IDXGISwapChain::Present
};
// The present half: issues the generated present (if record() produced one) and the real
// present, on the worker or inline. Returns true when it presented (the hook returns
// `*hr_out`); false when FG is off / not armed for `sc` and the hook must present normally.
bool present(const PresentArgs &args, long *hr_out);

void uninstall(); // drains the worker, releases every texture; idempotent

struct Stats
{
	std::uint64_t game_presents = 0;     // Present/Present1 calls that reached the hook with FG armed
	std::uint64_t presents_issued = 0;   // presents we made (real + generated)
	std::uint64_t generated_presented = 0;
	std::uint64_t refused[static_cast<int>(core::fg::Refusal::count)] = {};
	std::uint64_t reconfigures = 0;
	std::uint64_t epoch = 0;
	std::uint32_t replacement_count = 0;
	std::uint32_t width = 0, height = 0;
	unsigned format = 0;
	unsigned color_space = ~0u;          // the last SetColorSpace1 seen (DXGI_COLOR_SPACE_TYPE), ~0 = never
	double pacer_interval_ms = 0.0;   // the schedule's median interval (thread mode)
	std::uint64_t pacer_hitches = 0;
	std::uint64_t sched_reanchors = 0, sched_holds = 0, sched_catchups = 0;
	std::uint32_t issued_p50_ms = 0, issued_p99_ms = 0; // intervals between the presents WE issued
	int issued_second_peak_ms = -1;                     // >= 0: bimodal (back-to-back)
	std::uint64_t worker_waits = 0;      // game-thread waits on a busy worker
	std::uint64_t crop_black = 0, crop_stale = 0, crop_ok = 0, crop_identical = 0, crop_suspect = 0, crop_dark = 0;
	bool validated = false;
	core::fg::ThrottleState throttle;    // the flip-queue throttle: armed / refused / what it cost
	// What we did to the game's creation desc, and what the throttle then found. These are two
	// different facts and a mismatch between them is diagnostic on its own.
	core::fg::FlagVerdict flag_verdict = core::fg::FlagVerdict::disabled;
};
Stats stats();
const char *report();
// The periodic line ("[fg] ..."), also written by the owner every 600 presents.
void log_stats(const char *when);

} // namespace stray_dlss::native::fg
