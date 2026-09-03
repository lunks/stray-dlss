// Reflex for the present-twice path, through whatever nvapi64.dll the process has: DXVK-NVAPI
// v0.9.2 on the target (CLAUDE.md §1), which implements the low-latency entry points over
// VK_NV_low_latency2 when vkd3d-proton exposes it — SOFT until the box says so. Streamline's
// DLSS-G REQUIRES its own Reflex (migration doc §7.4); the snippet driven directly needs none
// (facts §32.1), so this is a measurement plus a latency aid, never a gate: every call's NvAPI
// status is logged once and the presents proceed whatever it says.
//
// Function ids and struct layouts are NVIDIA's public NVAPI headers (github.com/NVIDIA/nvapi,
// nvapi_interface.h / nvapi.h, fetched 2026-09-02): NvAPI_Initialize 0x0150e828,
// NvAPI_D3D_SetSleepMode 0xac1ca9e0, NvAPI_D3D_Sleep 0x852cd1d2, NvAPI_D3D_GetSleepStatus
// 0xaef96ca1, NvAPI_D3D_SetLatencyMarker 0xd9984c05, NvAPI_D3D12_SetAsyncFrameMarker 0x13c98f73,
// NvAPI_D3D12_NotifyOutOfBandCommandQueue 0x03d6e8cb.
// HARD for the header; whether DXVK-NVAPI answers each is what the log measures.
#pragma once

#include <cstdint>

struct ID3D12Device;
struct ID3D12CommandQueue;

namespace stray_dlss::native::fg::reflex {

enum class Marker : int
{
	simulation_start = 0,
	simulation_end = 1,
	rendersubmit_start = 2,
	rendersubmit_end = 3,
	present_start = 4,
	present_end = 5,
	input_sample = 6,
	trigger_flash = 7,
	out_of_band_rendersubmit_start = 9,
	out_of_band_rendersubmit_end = 10,
	out_of_band_present_start = 11,
	out_of_band_present_end = 12,
};

// Loads nvapi64.dll (or finds it already loaded), resolves the entry points, initialises NVAPI
// and requests low-latency mode on `device`. Logs every status. Returns true when SetSleepMode
// was accepted; false leaves everything below inert.
bool initialise(ID3D12Device *device, bool low_latency, bool boost);
// NvAPI_D3D_Sleep: the frame-rate-limiter / latency sleep. Called once per game present.
void sleep();
// A latency marker with the game's frame id (the present index). Inert when unavailable.
void marker(Marker m, std::uint64_t frame_id);
// The async (present-thread) marker on the presenting queue. Inert when unavailable.
void async_marker(ID3D12CommandQueue *queue, Marker m, std::uint64_t frame_id, std::uint64_t present_frame_id);

// NV_OUT_OF_BAND_CQ_TYPE (nvapi.h, fetched 2026-09-03): the values are NOT in the order the
// names suggest, so they are spelled out rather than guessed.
enum class OutOfBandType : int
{
	render = 0,
	present = 1,        // the one that matches our generated-frame presents
	ignore = 2,
	render_present = 3,
	explicit_copy = 4,
};

// NvAPI_D3D12_NotifyOutOfBandCommandQueue: tells the driver this queue carries work OUTSIDE
// the application's frame cadence, which is exactly what our present owner's queue does when
// it issues a generated frame. Streamline's sl.dlss_g makes this call and we did not; the
// migration audit (docs/RESEARCH-STREAMLINE-INTERNALS.md, recommendation 1) calls it the ONLY
// concrete capability Streamline has that we lack.
//
// HARD that Streamline makes the call, that NVIDIA's header declares it, and that DXVK-NVAPI
// implements it for real (src/nvapi_d3d12.cpp:1088, forwarding to vkd3d-proton's
// ID3D12DeviceExt-style vendor interface rather than stubbing).
// UNCONFIRMED that omitting it has ANY consequence: no source read on either audit states one,
// and nothing we have measured changes when it is absent. It is adopted as tidiness with a
// plausible latency-accounting benefit, never as a fix for an observed symptom.
//
// Returns the NvAPI status (0 = ok); logs the first result and every distinct one after that.
// Inert (and says so once) when this nvapi64.dll does not export the entry point.
int notify_out_of_band_queue(ID3D12CommandQueue *queue, OutOfBandType type);

void shutdown();

struct Status
{
	bool dll_found = false;
	bool initialised = false;      // NvAPI_Initialize ok
	int init_status = 0;
	int set_sleep_mode_status = 0; // NvAPI status of SetSleepMode (0 = ok, -3 = NO_IMPLEMENTATION, ...)
	bool sleep_mode_set = false;
	bool have_sleep = false, have_marker = false, have_async_marker = false;
	int last_sleep_status = 0, last_marker_status = 0, last_async_marker_status = 0;
	std::uint64_t sleeps = 0, markers = 0, async_markers = 0;
	bool have_out_of_band = false;   // the entry point resolved
	bool out_of_band_called = false; // we made the call at least once
	int out_of_band_status = 0;      // its last NvAPI status
	int out_of_band_type = -1;       // the NV_OUT_OF_BAND_CQ_TYPE we sent, -1 = never called
};
Status status();

} // namespace stray_dlss::native::fg::reflex
