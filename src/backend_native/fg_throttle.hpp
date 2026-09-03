// The flip-queue throttle's DXGI shell. The decisions are core/fg_throttle.hpp (tested on the
// Linux lane); this file only does what needs a swapchain: QueryInterface(IDXGISwapChain2),
// SetMaximumFrameLatency, GetFrameLatencyWaitableObject, and one bounded WaitForSingleObject
// before each present we issue.
//
// It is OFF unless [STRAYDLSS] NgxFGThrottle=1, and it can only arm on a swapchain that was
// CREATED with DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT — the game's own creation
// call, which is why [STRAYDLSS] NgxFGWaitableSwapChain exists to add that flag. Everything
// that stops it arming is a NAMED refusal, counted and logged once: an inert throttle and a
// refused one must be distinguishable from the log alone.
//
// NOTHING HERE HAS RUN IN THE GAME. The DXGI mechanics are exercised on WARP in CI
// (tests/warp/warp_fg_present.inc, test_fg_flip_throttle) and read from vkd3d-proton's source;
// neither is the same as a frame on the user's screen.
#pragma once

#include "core/fg_throttle.hpp"

#include <cstdint>

struct IDXGISwapChain;
struct ID3D12Device;

namespace stray_dlss::native::fg::throttle {

void configure(const core::fg::ThrottleConfig &cfg);
const core::fg::ThrottleConfig &config();

// Arms against `sc` (the game's swapchain, already finalised). Reads the created desc's flags,
// applies NgxFGMaxLatency through IDXGISwapChain2::SetMaximumFrameLatency (logging the value
// before and after), and takes the waitable handle. Idempotent per swapchain; safe to call
// again after a reconfigure, which is required — ResizeBuffers may reset the flag, and the
// handle vkd3d-proton hands out is a duplicate that must be re-taken.
// `device` is used only for the one-off IDXGIDevice1 probe (see the .cpp); it may be null.
void arm(IDXGISwapChain *sc, ID3D12Device *device);
// Releases the handle. Safe when never armed.
void disarm(const char *why);

// Blocks until a flip slot is free, or the configured timeout, or immediately when the
// throttle is not armed. MUST be called BEFORE the pacer's wait_until for the same present:
// the two are max(), never a sum (core/fg_throttle.hpp, "THE PACER INTERACTION").
// `hurried` is the presenter's own abandon flag — a drain or a teardown skips the wait
// entirely and counts it under `skipped` rather than paying the timeout.
void wait_for_slot(bool hurried);

core::fg::ThrottleState state();
// The [fg] line's throttle section. Never null; "off" when the knob was never set.
const char *report();

} // namespace stray_dlss::native::fg::throttle
