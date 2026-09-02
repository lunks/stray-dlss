// The whole hooking engine for COM vtable slots: VirtualProtect -> write -> restore, the
// dance src/ext_unhook.cpp already does on this exact target. A patch is installed ONCE per
// (slot, replacement), chains to the original it found there, and is NEVER restored
// (assessment §1.2: a contested slot restored to a stale pointer is how another component's
// hook gets torn out from under it).
#pragma once

namespace stray_dlss::native {

// Installs `replacement` at `vtable[index]` of `object`'s vtable and returns what was there.
// Idempotent: a second call with the same replacement returns the stored original without
// re-writing. Returns nullptr if the page could not be made writable.
void *patch_slot(void *object, unsigned index, void *replacement, const char *name);

unsigned patch_count();

} // namespace stray_dlss::native
