#include "backend_native/fg_reflex.hpp"

#include "log.hpp"

#include <windows.h>
#include <d3d12.h>

#include <atomic>
#include <cstring>
#include <mutex>

namespace stray_dlss::native::fg::reflex {
namespace {

// NVAPI's public layout (nvapi.h), with NvU32 = unsigned int, NvBool = unsigned char,
// NvU64 = unsigned long long, the marker enum = int. MAKE_NVAPI_VERSION(T, v) = sizeof(T) | (v << 16)
// (nvapi_lite_common.h). The reserved bytes must be zero.
struct SetSleepModeParams
{
	unsigned int version;
	unsigned char bLowLatencyMode;
	unsigned char bLowLatencyBoost;
	unsigned int minimumIntervalUs;
	unsigned char bUseMarkersToOptimize;
	unsigned char bUseMinQueueTime;
	unsigned char rsvd[30];
};
static_assert(sizeof(SetSleepModeParams) == 44, "NV_SET_SLEEP_MODE_PARAMS_V1 is 44 bytes in nvapi.h");

struct LatencyMarkerParams
{
	unsigned int version;
	unsigned long long frameID;
	int markerType;
	unsigned long long rsvd0;
	unsigned char rsvd[56];
};
static_assert(sizeof(LatencyMarkerParams) == 88, "NV_LATENCY_MARKER_PARAMS_V1 is 88 bytes in nvapi.h");

struct AsyncFrameMarkerParams
{
	unsigned int version;
	unsigned long long frameID;
	int markerType;
	unsigned long long presentFrameID;
	unsigned char vendorInternal;
	unsigned char rsvd[55];
};
static_assert(sizeof(AsyncFrameMarkerParams) == 88, "NV_ASYNC_FRAME_MARKER_PARAMS_V1 is 88 bytes with nvapi.h's member types (4+pad, 8, 4+pad, 8, 1+55)");

constexpr unsigned int make_version(unsigned int size, unsigned int ver) { return size | (ver << 16); }

using PFN_QueryInterface = void *(__cdecl *)(unsigned int id);
using PFN_Initialize = int(__cdecl *)();
using PFN_SetSleepMode = int(__cdecl *)(IUnknown *, SetSleepModeParams *);
using PFN_Sleep = int(__cdecl *)(IUnknown *);
using PFN_SetLatencyMarker = int(__cdecl *)(IUnknown *, LatencyMarkerParams *);
using PFN_SetAsyncFrameMarker = int(__cdecl *)(ID3D12CommandQueue *, AsyncFrameMarkerParams *);
using PFN_NotifyOutOfBandCommandQueue = int(__cdecl *)(ID3D12CommandQueue *, int);

constexpr unsigned int kId_Initialize = 0x0150e828;
constexpr unsigned int kId_SetSleepMode = 0xac1ca9e0;
constexpr unsigned int kId_Sleep = 0x852cd1d2;
constexpr unsigned int kId_SetLatencyMarker = 0xd9984c05;
constexpr unsigned int kId_SetAsyncFrameMarker = 0x13c98f73;
constexpr unsigned int kId_NotifyOutOfBandCommandQueue = 0x03d6e8cb;

std::mutex g_mutex;
Status g_status;
ID3D12Device *g_device = nullptr;
PFN_SetSleepMode g_set_sleep_mode = nullptr;
PFN_Sleep g_sleep = nullptr;
PFN_SetLatencyMarker g_set_marker = nullptr;
PFN_SetAsyncFrameMarker g_set_async_marker = nullptr;
PFN_NotifyOutOfBandCommandQueue g_notify_oob = nullptr;
bool g_nvapi_ready = false; // NvAPI_Initialize returned 0 (whichever path got there first)
std::atomic<bool> g_sleep_logged{ false }, g_marker_logged{ false }, g_async_logged{ false };
std::atomic<bool> g_oob_logged{ false }, g_oob_absent_logged{ false };

const char *status_name(int s)
{
	switch (s)
	{
	case 0: return "NVAPI_OK";
	case -1: return "NVAPI_ERROR";
	case -2: return "NVAPI_LIBRARY_NOT_FOUND";
	case -3: return "NVAPI_NO_IMPLEMENTATION";
	case -4: return "NVAPI_API_NOT_INITIALIZED";
	case -5: return "NVAPI_INVALID_ARGUMENT";
	case -6: return "NVAPI_NVIDIA_DEVICE_NOT_FOUND";
	case -9: return "NVAPI_INVALID_HANDLE";
	case -104: return "NVAPI_NOT_SUPPORTED";
	case -190: return "NVAPI_D3D_DEVICE_NOT_REGISTERED";
	default: return "?";
	}
}

// Loads nvapi64.dll and calls NvAPI_Initialize if nobody has yet, and resolves the entry
// points this file uses. Separate from initialise() so the out-of-band queue hint works with
// [STRAYDLSS] NgxFGReflex=0: it is an independent knob and must not silently need another one.
// Caller holds g_mutex. Idempotent.
bool ensure_nvapi_locked()
{
	if (g_nvapi_ready)
		return true;
	HMODULE nvapi = ::GetModuleHandleW(L"nvapi64.dll");
	if (nvapi == nullptr)
		nvapi = ::LoadLibraryW(L"nvapi64.dll");
	if (nvapi == nullptr)
		return false;
	g_status.dll_found = true;
	auto qi = reinterpret_cast<PFN_QueryInterface>(reinterpret_cast<void *>(::GetProcAddress(nvapi, "nvapi_QueryInterface")));
	if (qi == nullptr)
		return false;
	auto init = reinterpret_cast<PFN_Initialize>(qi(kId_Initialize));
	if (init == nullptr)
		return false;
	g_status.init_status = init();
	g_status.initialised = g_status.init_status == 0;
	g_nvapi_ready = g_status.initialised;
	if (!g_nvapi_ready)
		return false;
	if (g_notify_oob == nullptr)
	{
		g_notify_oob = reinterpret_cast<PFN_NotifyOutOfBandCommandQueue>(qi(kId_NotifyOutOfBandCommandQueue));
		g_status.have_out_of_band = g_notify_oob != nullptr;
	}
	return true;
}

} // namespace

bool initialise(ID3D12Device *device, bool low_latency, bool boost)
{
	std::lock_guard<std::mutex> lock(g_mutex);
	g_status = Status{};
	g_device = device;
	HMODULE nvapi = ::GetModuleHandleW(L"nvapi64.dll");
	if (nvapi == nullptr)
		nvapi = ::LoadLibraryW(L"nvapi64.dll");
	if (nvapi == nullptr)
	{
		STRAY_LOG_WARN("fg/reflex: nvapi64.dll is not loadable; Reflex is inert");
		return false;
	}
	g_status.dll_found = true;
	char path[MAX_PATH] = "?";
	::GetModuleFileNameA(nvapi, path, MAX_PATH);
	auto qi = reinterpret_cast<PFN_QueryInterface>(reinterpret_cast<void *>(::GetProcAddress(nvapi, "nvapi_QueryInterface")));
	if (qi == nullptr)
	{
		STRAY_LOG_WARN("fg/reflex: %s has no nvapi_QueryInterface; Reflex is inert", path);
		return false;
	}
	auto init = reinterpret_cast<PFN_Initialize>(qi(kId_Initialize));
	g_set_sleep_mode = reinterpret_cast<PFN_SetSleepMode>(qi(kId_SetSleepMode));
	g_sleep = reinterpret_cast<PFN_Sleep>(qi(kId_Sleep));
	g_set_marker = reinterpret_cast<PFN_SetLatencyMarker>(qi(kId_SetLatencyMarker));
	g_set_async_marker = reinterpret_cast<PFN_SetAsyncFrameMarker>(qi(kId_SetAsyncFrameMarker));
	g_notify_oob = reinterpret_cast<PFN_NotifyOutOfBandCommandQueue>(qi(kId_NotifyOutOfBandCommandQueue));
	g_status.have_out_of_band = g_notify_oob != nullptr;
	g_status.have_sleep = g_sleep != nullptr;
	g_status.have_marker = g_set_marker != nullptr;
	g_status.have_async_marker = g_set_async_marker != nullptr;
	STRAY_LOG_INFO("fg/reflex: %s: Initialize=%p SetSleepMode=%p Sleep=%p SetLatencyMarker=%p SetAsyncFrameMarker=%p", path,
		reinterpret_cast<void *>(init), reinterpret_cast<void *>(g_set_sleep_mode), reinterpret_cast<void *>(g_sleep),
		reinterpret_cast<void *>(g_set_marker), reinterpret_cast<void *>(g_set_async_marker));
	if (init == nullptr || g_set_sleep_mode == nullptr)
	{
		STRAY_LOG_WARN("fg/reflex: this nvapi64.dll does not export the Reflex entry points; Reflex is inert");
		return false;
	}
	g_status.init_status = init();
	g_status.initialised = g_status.init_status == 0;
	g_nvapi_ready = g_status.initialised;
	if (!g_status.initialised)
	{
		STRAY_LOG_WARN("fg/reflex: NvAPI_Initialize -> %d (%s); Reflex is inert", g_status.init_status, status_name(g_status.init_status));
		return false;
	}
	SetSleepModeParams p = {};
	p.version = make_version(sizeof(p), 1);
	p.bLowLatencyMode = low_latency ? 1 : 0;
	p.bLowLatencyBoost = boost ? 1 : 0;
	p.minimumIntervalUs = 0;
	p.bUseMarkersToOptimize = 1;
	g_status.set_sleep_mode_status = g_set_sleep_mode(reinterpret_cast<IUnknown *>(device), &p);
	g_status.sleep_mode_set = g_status.set_sleep_mode_status == 0;
	STRAY_LOG_WARN("fg/reflex: NvAPI_D3D_SetSleepMode(lowLatency=%d boost=%d markers=1) -> %d (%s) on device %p; Sleep=%s markers=%s async=%s",
		low_latency ? 1 : 0, boost ? 1 : 0, g_status.set_sleep_mode_status, status_name(g_status.set_sleep_mode_status), static_cast<void *>(device),
		g_status.have_sleep ? "available" : "absent", g_status.have_marker ? "available" : "absent", g_status.have_async_marker ? "available" : "absent");
	return g_status.sleep_mode_set;
}

void sleep()
{
	if (g_sleep == nullptr || g_device == nullptr)
		return;
	const int s = g_sleep(reinterpret_cast<IUnknown *>(g_device));
	std::lock_guard<std::mutex> lock(g_mutex);
	g_status.last_sleep_status = s;
	++g_status.sleeps;
	if (!g_sleep_logged.exchange(true))
		STRAY_LOG_INFO("fg/reflex: first NvAPI_D3D_Sleep -> %d (%s)", s, status_name(s));
}

void marker(Marker m, std::uint64_t frame_id)
{
	if (g_set_marker == nullptr || g_device == nullptr)
		return;
	LatencyMarkerParams p = {};
	p.version = make_version(sizeof(p), 1);
	p.frameID = frame_id;
	p.markerType = static_cast<int>(m);
	const int s = g_set_marker(reinterpret_cast<IUnknown *>(g_device), &p);
	std::lock_guard<std::mutex> lock(g_mutex);
	g_status.last_marker_status = s;
	++g_status.markers;
	if (!g_marker_logged.exchange(true))
		STRAY_LOG_INFO("fg/reflex: first NvAPI_D3D_SetLatencyMarker(%d, frame %llu) -> %d (%s)", static_cast<int>(m), static_cast<unsigned long long>(frame_id), s, status_name(s));
}

void async_marker(ID3D12CommandQueue *queue, Marker m, std::uint64_t frame_id, std::uint64_t present_frame_id)
{
	if (g_set_async_marker == nullptr || queue == nullptr)
		return;
	AsyncFrameMarkerParams p = {};
	p.version = make_version(sizeof(p), 1);
	p.frameID = frame_id;
	p.markerType = static_cast<int>(m);
	p.presentFrameID = present_frame_id;
	const int s = g_set_async_marker(queue, &p);
	std::lock_guard<std::mutex> lock(g_mutex);
	g_status.last_async_marker_status = s;
	++g_status.async_markers;
	if (!g_async_logged.exchange(true))
		STRAY_LOG_INFO("fg/reflex: first NvAPI_D3D12_SetAsyncFrameMarker(%d) -> %d (%s)", static_cast<int>(m), s, status_name(s));
}

int notify_out_of_band_queue(ID3D12CommandQueue *queue, OutOfBandType type)
{
	if (queue == nullptr)
		return -5; // NVAPI_INVALID_ARGUMENT
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		if (!ensure_nvapi_locked())
			return -2; // NVAPI_LIBRARY_NOT_FOUND
		if (g_notify_oob == nullptr)
		{
			if (!g_oob_absent_logged.exchange(true))
				STRAY_LOG_WARN("fg/reflex: this nvapi64.dll does not export NvAPI_D3D12_NotifyOutOfBandCommandQueue (0x%08x); "
					"the out-of-band queue hint is inert. Nothing else changes - the audit calls this call tidiness, not a fix.",
					kId_NotifyOutOfBandCommandQueue);
			return -3; // NVAPI_NO_IMPLEMENTATION
		}
	}
	const int s = g_notify_oob(queue, static_cast<int>(type));
	std::lock_guard<std::mutex> lock(g_mutex);
	g_status.out_of_band_called = true;
	g_status.out_of_band_status = s;
	g_status.out_of_band_type = static_cast<int>(type);
	if (!g_oob_logged.exchange(true))
		STRAY_LOG_WARN("fg/reflex: NvAPI_D3D12_NotifyOutOfBandCommandQueue(queue %p, NV_OUT_OF_BAND_CQ_TYPE %d) -> %d (%s). "
			"Streamline makes this call on its present queue and we did not; whether omitting it EVER mattered here is UNCONFIRMED.",
			static_cast<void *>(queue), static_cast<int>(type), s, status_name(s));
	return s;
}

void shutdown()
{
	std::lock_guard<std::mutex> lock(g_mutex);
	g_device = nullptr;
	g_set_sleep_mode = nullptr;
	g_sleep = nullptr;
	g_set_marker = nullptr;
	g_set_async_marker = nullptr;
	g_notify_oob = nullptr;
	g_nvapi_ready = false;
}

Status status()
{
	std::lock_guard<std::mutex> lock(g_mutex);
	return g_status;
}

} // namespace stray_dlss::native::fg::reflex
