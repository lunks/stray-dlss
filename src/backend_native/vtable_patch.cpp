#include "backend_native/vtable_patch.hpp"

#include "log.hpp"

#include <windows.h>

#include <mutex>
#include <vector>

namespace stray_dlss::native {
namespace {

struct Patch
{
	void **slot = nullptr;
	void *original = nullptr;
	void *replacement = nullptr;
};

std::mutex g_mutex;
std::vector<Patch> g_patches;

} // namespace

void *patch_slot(void *object, unsigned index, void *replacement, const char *name)
{
	if (object == nullptr || replacement == nullptr)
		return nullptr;
	void **vtable = *static_cast<void ***>(object);
	void **slot = &vtable[index];

	std::lock_guard<std::mutex> lock(g_mutex);
	for (const Patch &p : g_patches)
		if (p.slot == slot && p.replacement == replacement)
			return p.original;

	void *const original = *slot;
	if (original == replacement)
	{
		// Already ours (installed on another object sharing this vtable, recorded under a
		// different pointer), so there is nothing to chain to that we do not already hold.
		for (const Patch &p : g_patches)
			if (p.replacement == replacement)
				return p.original;
		return nullptr;
	}

	DWORD old = 0;
	if (!::VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &old))
	{
		STRAY_LOG_ERROR("vtable_patch: %s slot %u at %p: VirtualProtect failed (error %lu)",
			name, index, static_cast<void *>(slot), ::GetLastError());
		return nullptr;
	}
	*slot = replacement;
	DWORD ignored = 0;
	::VirtualProtect(slot, sizeof(void *), old, &ignored);

	g_patches.push_back(Patch{ slot, original, replacement });
	STRAY_LOG_INFO("vtable_patch: %s slot %u %p -> %p (vtable %p)", name, index, original, replacement,
		static_cast<void *>(vtable));
	return original;
}

void restore_all_patches()
{
	std::lock_guard<std::mutex> lock(g_mutex);
	unsigned restored = 0, skipped = 0;
	for (auto it = g_patches.rbegin(); it != g_patches.rend(); ++it)
	{
		if (*it->slot != it->replacement)
		{
			++skipped; // someone patched over us; leave their chain intact
			continue;
		}
		DWORD old = 0;
		if (::VirtualProtect(it->slot, sizeof(void *), PAGE_READWRITE, &old))
		{
			*it->slot = it->original;
			DWORD ignored = 0;
			::VirtualProtect(it->slot, sizeof(void *), old, &ignored);
			++restored;
		}
	}
	STRAY_LOG_INFO("vtable_patch: restored %u slot(s), left %u patched by someone else", restored, skipped);
	g_patches.clear();
}

unsigned patch_count()
{
	std::lock_guard<std::mutex> lock(g_mutex);
	return static_cast<unsigned>(g_patches.size());
}

} // namespace stray_dlss::native
