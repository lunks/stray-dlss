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
// 0xaef96ca1, NvAPI_D3D_SetLatencyMarker 0xd9984c05, NvAPI_D3D12_SetAsyncFrameMarker 0x13c98f73.
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
};
Status status();

} // namespace stray_dlss::native::fg::reflex
