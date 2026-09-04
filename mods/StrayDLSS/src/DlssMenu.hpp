// An in-game DLSS quality menu, built from Stray's own widgets, in C++.
//
//   F10          open / close (closing applies the selection)
//   LEFT/RIGHT   move the selection
//
// WHY C++ AND NOT LUA, and this is a measured reason rather than a preference. Nine crashes on
// 2026-09-04 building this in Lua, all the same shape: a widget we construct renders, then the
// game dies a few seconds later or on the next input. The pattern across them:
//
//   container alone (UMG_DebugMenu_C, UMG_HUD_Debug, UMG_DebugInput)  SURVIVES, even across a
//                                                                     resolution change
//   UMG_ListBox_C parented into our container                         CRASHES
//   BP_HKTextBlock_C parented into our container                      CRASHES
//   either one inside the GAME'S OWN live settings page               renders fine
//
// So it is not the widget type and not the r.ScreenPercentage resize (a crash happened with no
// resolution change at all, which retired that theory). The distinguishing factor is WHAT HOLDS
// A REFERENCE: the viewport roots a top-level widget, the game's own UI hierarchy roots its
// children, and a widget we create and host ourselves is rooted by nothing - so the garbage
// collector takes it and the next tick dereferences freed memory. The timing fits exactly.
//
// The fix is EObjectFlags::RF_MarkAsRootSet (Unreal/UnrealFlags.hpp:69) via UObject::SetFlags,
// which UE4SS exposes to C++ and NOT to Lua. Lua could not express the fix, which is why every
// Lua attempt failed the same way however the widgets were arranged.
//
// WHAT IT DRIVES is already proven and is not in question: r.ScreenPercentage at
// ECVF_SetByConsole outranks both [SystemSettings] and the game's own setting, so DLSS follows
// it without HKGameUserSettings being involved. Verified by NGX recreating the feature and
// naming the presets itself - 1920x1080 Performance, 2228x1253 Balanced, 2573x1448 Quality.
//
// This file includes UE4SS headers, so it is NOT in the mingw lane.
#pragma once

namespace stray_dlss_menu {

// Called once from the mod's on_unreal_init, on the game thread.
void Init();

// Key handlers, wired to CppUserModBase::register_keydown_event.
void OnToggle();
void OnLeft();
void OnRight();

} // namespace stray_dlss_menu
