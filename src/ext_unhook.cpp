#include "ext_unhook.hpp"

#include "log.hpp"

#include <windows.h>
#include <d3d12.h>

namespace stray_dlss::ext_unhook {

namespace {

bool g_enabled = true;

// The vkd3d extension interface, held alive so the vtable can be re-read each evaluate without
// re-querying. vkd3d hands every caller the same static vtable, which is exactly why one
// component's patch corrupts everyone and why holding one interface suffices to repair it.
IUnknown *g_ext = nullptr;
void **g_vtable = nullptr;

constexpr unsigned int kSlots[] = { 7, 8, 14, 15 };
void *g_original[4] = {};
unsigned int g_slot_count = 0; // 2 when only ID3D12DeviceExt exists, 4 with Ext2

// {11EA7A1A-0F6A-49BF-B612-3E30F8E201DD} — ID3D12DeviceExt (vkd3d-proton)
constexpr GUID kDeviceExt = { 0x11ea7a1a, 0x0f6a, 0x49bf,
	{ 0xb6, 0x12, 0x3e, 0x30, 0xf8, 0xe2, 0x01, 0xdd } };
// {E859C4AC-BA8F-41C4-8EAC-1137FDE6158D} — ID3D12DeviceExt2 (vkd3d-proton ≥ 3.0)
constexpr GUID kDeviceExt2 = { 0xe859c4ac, 0xba8f, 0x41c4,
	{ 0x8e, 0xac, 0x11, 0x37, 0xfd, 0xe6, 0x15, 0x8d } };

// A pointer belongs to ReShade when its module exports the add-on registration entry point.
bool owned_by_reshade(void *fn)
{
	HMODULE owner = nullptr;
	::GetModuleHandleExW(
		GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		reinterpret_cast<LPCWSTR>(fn), &owner);
	return owner != nullptr && ::GetProcAddress(owner, "ReShadeRegisterAddon") != nullptr;
}

} // namespace

void set_enabled(bool enabled) { g_enabled = enabled; }

bool capture(ID3D12Device *native)
{
	if (native == nullptr || g_ext != nullptr)
		return g_ext != nullptr;

	IUnknown *ext = nullptr;
	unsigned int count = 4;
	if (FAILED(native->QueryInterface(kDeviceExt2, reinterpret_cast<void **>(&ext))))
	{
		count = 2; // no Ext2: only slots 7/8 exist
		if (FAILED(native->QueryInterface(kDeviceExt, reinterpret_cast<void **>(&ext))))
		{
			STRAY_LOG_INFO("ext_unhook: no vkd3d extension interface; nothing to protect.");
			return false;
		}
	}

	void **vtable = *reinterpret_cast<void ***>(ext);
	for (unsigned int i = 0; i < count; ++i)
	{
		if (owned_by_reshade(vtable[kSlots[i]]))
		{
			// The pristine pointer is unrecoverable from inside the process; leave the loud
			// per-frame reporting to say so rather than "repair" with a wrong value.
			STRAY_LOG_ERROR("ext_unhook: slot %u is ALREADY ReShade's at capture time — the "
				"originals cannot be learned, repair disabled.", kSlots[i]);
			ext->Release();
			return false;
		}
		g_original[i] = vtable[kSlots[i]];
	}

	g_ext = ext; // keep the reference; released never (process lifetime)
	g_vtable = vtable;
	g_slot_count = count;
	STRAY_LOG_INFO("ext_unhook: captured %u pristine vtable slots (%s).", count,
		count == 4 ? "ID3D12DeviceExt2" : "ID3D12DeviceExt");
	return true;
}

int repair()
{
	if (!g_enabled || g_vtable == nullptr)
		return -1;

	int repaired = 0;
	for (unsigned int i = 0; i < g_slot_count; ++i)
	{
		void *current = g_vtable[kSlots[i]];
		if (current == g_original[i])
			continue;
		// Only ever overwrite ReShade's pointer. Anything else (a vkd3d update mid-session is
		// impossible, but a third overlay is not) is logged and left alone.
		if (!owned_by_reshade(current))
		{
			STRAY_LOG_ERROR("ext_unhook: slot %u changed to %p but not by ReShade — leaving it.",
				kSlots[i], current);
			continue;
		}
		DWORD old_protect = 0;
		if (!::VirtualProtect(&g_vtable[kSlots[i]], sizeof(void *), PAGE_READWRITE,
				&old_protect))
		{
			STRAY_LOG_ERROR("ext_unhook: VirtualProtect failed for slot %u.", kSlots[i]);
			continue;
		}
		g_vtable[kSlots[i]] = g_original[i];
		DWORD ignored = 0;
		::VirtualProtect(&g_vtable[kSlots[i]], sizeof(void *), old_protect, &ignored);
		// The game may re-install the patch every frame; keep the evidence without the spam.
		static unsigned long long s_total = 0;
		++s_total;
		if (s_total <= 8 || s_total % 256 == 0)
			STRAY_LOG_WARN("ext_unhook: slot %u restored %p -> %p (ReShade's patch undone; "
				"repair #%llu).", kSlots[i], current, g_original[i], s_total);
		++repaired;
	}
	return repaired;
}

} // namespace stray_dlss::ext_unhook
