// The whole hooking engine for COM vtable slots: VirtualProtect -> write -> restore, the
// (slot, replacement), chains to the original it found there, and is NEVER restored
// (assessment §1.2: a contested slot restored to a stale pointer is how another component's
// hook gets torn out from under it).
#pragma once

namespace stray_dlss::native {

// Installs `replacement` at `vtable[index]` of `object`'s vtable and returns what was there.
// Idempotent: a second call with the same replacement returns the stored original without
// re-writing. Returns nullptr if the page could not be made writable.
void *patch_slot(void *object, unsigned index, void *replacement, const char *name);

// The original we chained to for THIS object's vtable slot, or nullptr if that slot was never
// patched. Resolves per-vtable, never through a shadow global: two objects of different classes
// (e.g. wine-builtin DXGI and DXVK DXGI, both live in one Proton process) have distinct vtables
// and distinct originals, so a hook MUST look up the original for the `self` it was handed
// rather than trust a single global — forwarding one class's re-entrant call to the other
// class's implementation is a NULL-deref inside vkd3d (facts §20). MEASURED-driven.
void *original_for(void *object, unsigned index);

unsigned patch_count();

// Puts every original back — ONLY where the slot still holds our replacement — and forgets
// the records. This exists for one reason, MEASURED 2026-09-01: ReShade unloads and reloads
// the add-on DLL across the game's device recreate, and a process-global patch that outlives
// its DLL calls a null original (fresh statics) or unmapped code after the reload: the
// address-0 startup crash. "Never restore" (assessment §1.2) is right for contested slots;
// a slot whose replacement is about to be unmapped is not that case.
void restore_all_patches();

} // namespace stray_dlss::native
