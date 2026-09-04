// Finding the Screen Percentage row's list by searching the widget's own memory.
//
// This is the UE4SS-facing half of src/core/menu_scan.hpp; everything decidable from bytes lives
// there and is unit-tested, and everything here is the part only the engine can supply: which
// object, how big it is, and which regions its reflected properties already occupy.
//
// It is a DIAGNOSTIC. It reads, it logs, it changes nothing. What it produces is an OFFSET, and
// an offset is what UE4SS's RegisterCustomProperty needs in order to reach a native member that
// has no UPROPERTY - which is the only route left after reflection was exhausted
// (docs/RESEARCH-STRAY-MENU-OPTIONS.md and its sequel).
//
// This file includes UE4SS headers, so it is NOT in the mingw lane. Keep it that way: the whole
// point of the Host.cpp/Mod.cpp split is that the plain-Win32 half compiles in two minutes
// without the SDK.
#pragma once

namespace stray_menu_scan {

// Called from the UE4SS update tick. Does nothing at all unless [STRAYDLSS] MenuScan is set,
// and stops for good after it has reported once.
void Tick();

} // namespace stray_menu_scan
