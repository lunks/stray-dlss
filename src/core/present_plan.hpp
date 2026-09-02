// Which command queue a swapchain presents on, decided from plain numbers. Pure; tested.
//
// The native present owner records every ID3D12CommandQueue the game creates (through the
// device hook, so it sees the REAL queue objects even when a ReShade proxy sits above the
// device) and, at swapchain creation, is handed whatever object the game passed as the
// swapchain's device — the real queue when nothing proxies it, ReShade's proxy queue when
// ReShade is loaded (measured design: ReShade wraps queues, never resources). A command list
// of ours must be executed on the REAL queue: a proxy queue expects proxy lists.
#pragma once

#include <cstdint>
#include <vector>

namespace stray_dlss::core {

struct QueueRecord
{
	std::uint64_t id = 0; // ID3D12CommandQueue* as an integer
	int type = 0;         // D3D12_COMMAND_LIST_TYPE: 0 direct, 2 compute, 3 copy
};

// The index into `queues` of the queue to present on: the record whose id IS the object the
// game passed, else the FIRST direct queue (a proxy hid the identity), else -1.
int pick_present_queue(const std::vector<QueueRecord> &queues, std::uint64_t swapchain_device_arg);

} // namespace stray_dlss::core
