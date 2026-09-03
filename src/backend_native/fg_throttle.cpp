#include "backend_native/fg_throttle.hpp"

#include "log.hpp"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include <atomic>
#include <cstdio>
#include <mutex>

namespace stray_dlss::native::fg::throttle {
namespace {

std::mutex g_mutex;
core::fg::ThrottleConfig g_cfg;
core::fg::ThrottleState g_state;
HANDLE g_handle = nullptr;
std::atomic<bool> g_active{ false }; // fast path for wait_for_slot: armed AND enabled
bool g_probed_dxgi_device = false;
char g_report[320] = "throttle: off";

void refresh_report_locked()
{
	std::snprintf(g_report, sizeof(g_report),
		"throttle: %s (%s) latency %u->%u (asked %u) waits=%llu slots=%llu timeouts=%llu failed=%llu skipped=%llu blocked mean %.2f ms max %.2f ms",
		g_state.armed ? "ARMED" : "inactive", core::fg::throttle_refusal_name(g_state.refusal),
		g_state.max_latency_before, g_state.max_latency_after, g_state.max_latency_requested,
		static_cast<unsigned long long>(g_state.waits), static_cast<unsigned long long>(g_state.slots),
		static_cast<unsigned long long>(g_state.timeouts), static_cast<unsigned long long>(g_state.failures),
		static_cast<unsigned long long>(g_state.skipped),
		core::fg::throttle_blocked_mean_ms(g_state), g_state.blocked_max_ns / 1e6);
}

void close_handle_locked()
{
	if (g_handle != nullptr)
	{
		::CloseHandle(g_handle); // vkd3d-proton hands out a DuplicateHandle'd, SYNCHRONIZE-only copy
		g_handle = nullptr;
	}
}

// D3D12 has no IDXGIDevice — MSDN's own wording for the waitable flag is that the latency
// "must be set with IDXGISwapChain2::SetMaximumFrameLatency INSTEAD OF
// IDXGIDevice1::SetMaximumFrameLatency". The device route is therefore expected to be
// unreachable here, and this probe exists so the log SAYS that once from a measurement rather
// than us asserting it from the documentation. It never changes anything.
void probe_dxgi_device_locked(ID3D12Device *device)
{
	if (g_probed_dxgi_device || device == nullptr)
		return;
	g_probed_dxgi_device = true;
	IDXGIDevice1 *dev1 = nullptr;
	const HRESULT hr = device->QueryInterface(IID_PPV_ARGS(&dev1));
	if (SUCCEEDED(hr) && dev1 != nullptr)
	{
		UINT before = 0;
		const HRESULT ghr = dev1->GetMaximumFrameLatency(&before);
		STRAY_LOG_WARN("fg/throttle: the D3D12 device DOES answer IID_IDXGIDevice1 (unexpected on D3D12); "
			"GetMaximumFrameLatency -> 0x%08lx value %u. Reported, not used: the waitable swapchain is still the route MSDN mandates.",
			static_cast<unsigned long>(ghr), before);
		dev1->Release();
		return;
	}
	STRAY_LOG_INFO("fg/throttle: ID3D12Device does not implement IDXGIDevice1 (QueryInterface -> 0x%08lx), as MSDN implies for D3D12; "
		"the frame latency can only be set through IDXGISwapChain2 on a waitable swapchain.",
		static_cast<unsigned long>(hr));
}

} // namespace

void configure(const core::fg::ThrottleConfig &cfg)
{
	std::lock_guard<std::mutex> lock(g_mutex);
	g_cfg = cfg;
	if (!cfg.enabled)
	{
		g_state.refusal = core::fg::ThrottleRefusal::disabled;
		g_state.armed = false;
		g_active.store(false);
	}
	refresh_report_locked();
	if (cfg.enabled)
		STRAY_LOG_WARN("fg/throttle: ENABLED (maxLatency=%u timeoutMs=%u giveUpAfter=%u). It arms only on a swapchain created with "
			"DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT - see [STRAYDLSS] NgxFGWaitableSwapChain.",
			cfg.max_latency, cfg.timeout_ms, cfg.give_up_after);
}

const core::fg::ThrottleConfig &config() { return g_cfg; }

void arm(IDXGISwapChain *sc, ID3D12Device *device)
{
	std::lock_guard<std::mutex> lock(g_mutex);
	close_handle_locked();
	g_active.store(false);
	g_state.armed = false;
	g_state.max_latency_before = ~0u;
	g_state.max_latency_after = ~0u;
	g_state.max_latency_requested = g_cfg.max_latency;
	// A give-up is for the session: re-arming after a resize must not resurrect a throttle
	// that has already proved it blocks forever.
	if (g_state.refusal == core::fg::ThrottleRefusal::gave_up)
	{
		refresh_report_locked();
		return;
	}
	if (!g_cfg.enabled || sc == nullptr)
	{
		g_state.refusal = core::fg::ThrottleRefusal::disabled;
		refresh_report_locked();
		return;
	}
	probe_dxgi_device_locked(device);

	core::fg::ArmInputs in;
	in.enabled = true;

	IDXGISwapChain2 *sc2 = nullptr;
	if (SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&sc2))) && sc2 != nullptr)
		in.has_swapchain2 = true;

	DXGI_SWAP_CHAIN_DESC desc = {};
	if (SUCCEEDED(sc->GetDesc(&desc)))
		in.waitable_flag = (desc.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) != 0;

	if (sc2 != nullptr && in.waitable_flag)
	{
		UINT before = 0;
		if (SUCCEEDED(sc2->GetMaximumFrameLatency(&before)))
			g_state.max_latency_before = before;
		if (g_cfg.max_latency != 0)
		{
			const HRESULT hr = sc2->SetMaximumFrameLatency(g_cfg.max_latency);
			if (FAILED(hr))
				STRAY_LOG_ERROR("fg/throttle: SetMaximumFrameLatency(%u) -> 0x%08lx; the swapchain keeps its own value",
					g_cfg.max_latency, static_cast<unsigned long>(hr));
		}
		UINT after = 0;
		if (SUCCEEDED(sc2->GetMaximumFrameLatency(&after)))
			g_state.max_latency_after = after;
		g_handle = sc2->GetFrameLatencyWaitableObject();
		in.have_handle = g_handle != nullptr;
	}
	if (sc2 != nullptr)
		sc2->Release();

	g_state.refusal = core::fg::arm_verdict(in);
	g_state.armed = g_state.refusal == core::fg::ThrottleRefusal::none;
	if (!g_state.armed)
		close_handle_locked();
	g_active.store(g_state.armed && g_cfg.enabled);
	refresh_report_locked();
	if (g_state.armed)
		STRAY_LOG_WARN("fg/throttle: ARMED on swapchain %p: waitable handle %p, max frame latency %u -> %u (asked for %u), "
			"timeout %u ms, give up after %u consecutive timeouts. Every present we issue now waits for a flip slot FIRST, "
			"before the pacer's own deadline - the two are max(), not a sum.",
			static_cast<void *>(sc), g_handle, g_state.max_latency_before, g_state.max_latency_after,
			g_cfg.max_latency, g_cfg.timeout_ms, g_cfg.give_up_after);
	else
		STRAY_LOG_ERROR("fg/throttle: REFUSED (%s) on swapchain %p: SwapChain2=%d createdWaitableFlag=%d handle=%d (desc flags 0x%08x). "
			"The presents are UNTHROTTLED and behave exactly as before. %s",
			core::fg::throttle_refusal_name(g_state.refusal), static_cast<void *>(sc),
			in.has_swapchain2 ? 1 : 0, in.waitable_flag ? 1 : 0, in.have_handle ? 1 : 0, desc.Flags,
			g_state.refusal == core::fg::ThrottleRefusal::not_waitable
				? "The game did not ask for a waitable swapchain: set [STRAYDLSS] NgxFGWaitableSwapChain=1 to add the flag at creation."
				: "");
}

void disarm(const char *why)
{
	std::lock_guard<std::mutex> lock(g_mutex);
	const bool was = g_state.armed;
	g_state.armed = false;
	g_active.store(false);
	close_handle_locked();
	refresh_report_locked();
	if (was)
		STRAY_LOG_INFO("fg/throttle: disarmed (%s)", why != nullptr ? why : "?");
}

void wait_for_slot(bool hurried)
{
	// The fast path costs one relaxed atomic load per present when the knob is off, which is
	// the shipped configuration.
	if (!g_active.load(std::memory_order_relaxed))
		return;
	HANDLE h = nullptr;
	unsigned timeout_ms = 0;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		if (!core::fg::should_wait(g_cfg, g_state) || g_handle == nullptr)
			return;
		if (hurried)
		{
			++g_state.skipped;
			return;
		}
		h = g_handle;
		timeout_ms = g_cfg.timeout_ms;
	}
	LARGE_INTEGER f, t0, t1;
	::QueryPerformanceFrequency(&f);
	::QueryPerformanceCounter(&t0);
	const DWORD r = ::WaitForSingleObjectEx(h, timeout_ms, FALSE);
	::QueryPerformanceCounter(&t1);
	const std::uint64_t blocked_ns = f.QuadPart != 0
		? static_cast<std::uint64_t>((t1.QuadPart - t0.QuadPart) * 1'000'000'000ll / f.QuadPart)
		: 0;
	const core::fg::WaitOutcome outcome = r == WAIT_OBJECT_0 ? core::fg::WaitOutcome::slot_freed
		: r == WAIT_TIMEOUT ? core::fg::WaitOutcome::timed_out
		: core::fg::WaitOutcome::failed;
	bool gave_up = false;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		gave_up = core::fg::note_wait(g_cfg, g_state, outcome, blocked_ns);
		if (gave_up)
		{
			g_active.store(false);
			close_handle_locked();
		}
		refresh_report_locked();
	}
	if (gave_up)
		STRAY_LOG_ERROR("fg/throttle: GIVING UP after %u consecutive results that were not a freed flip slot "
			"(last WaitForSingleObject -> 0x%08lx, last block %.2f ms). The throttle is disarmed for the rest of the session and "
			"the presents revert to the unthrottled path; nothing else changes. Raise [STRAYDLSS] NgxFGThrottleTimeoutMs or set "
			"NgxFGThrottle=0 if this repeats.",
			g_cfg.give_up_after, static_cast<unsigned long>(r), blocked_ns / 1e6);
}

void note_bypassed()
{
	if (!g_cfg.enabled)
		return;
	std::lock_guard<std::mutex> lock(g_mutex);
	core::fg::note_bypassed(g_state);
}

core::fg::ThrottleState state()
{
	std::lock_guard<std::mutex> lock(g_mutex);
	return g_state;
}

const char *report() { return g_report; }

} // namespace stray_dlss::native::fg::throttle
