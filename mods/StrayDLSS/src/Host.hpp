// StrayDLSS — the plain-Win32 host (plan Stage 4). Everything the UE4SS glue needs to call, and
// nothing that needs a UE4SS header: this file and Host.cpp are compiled under mingw by CI as
// the fast proxy for the MSVC build.
//
// What a host provides, per the seam (src/intercept/): a config Source, a log sink, the seam's
// Backend (the native one, alone), the D3D12 device the moment the game creates it, and the
// present boundary (the native present owner). ReShade provided all of that until now; this is
// the replacement, and it is the end state the plan was written for.
#pragma once

#include <string>

namespace stray_dlss::plugin {

// From start_mod(): opens the log at <game>/stray-dlss-plugin.log, loads <mod>/StrayDLSS.ini,
// configures the application, and detours D3D12CreateDevice (loading d3d12.dll itself if the
// game has not yet — measured: it has not, §12).
void Start(const std::wstring &mod_dir, const std::wstring &game_dir);
// From on_update (UE4SS's own thread, ~200 Hz): hot-reloads the ini, emits nothing else.
void Tick();
// From uninstall_mod: the final census, the native hooks and the present owner undone, the
// detour removed, the log closed.
void Stop();

// The StrayDLSS.ini that Start() actually found, or "" if it found none. The live-tuning tab
// saves back into THIS file rather than re-deriving the path, so a save can never land beside
// the file the session is reading (Start tries the mod root and then the dlls/ directory, and
// which one won is a runtime fact).
const std::string &IniPath();

// [STRAYDLSS] TweakUi, default 1. Gates BOTH halves of the live-tuning path: the UE4SS tab and
// the hot-apply of its knobs on an ini reload. It exists so a UI bug can be switched off on the
// box without a rebuild, which is the only remedy available at a 20-minute build round trip.
bool TweakUiEnabled();

} // namespace stray_dlss::plugin
