// StrayDLSS — the UE4SS glue, and NOTHING else.
//
// This is the only file in the mod that includes a UE4SS header, exactly as in the sibling
// mods/StrayDualSense/src/Mod.cpp whose scaffolding this lifts. Everything it does is call into
// plain-Win32 code (Host.cpp) that is compiled without the SDK.
//
// PROVENANCE, read out of RE-UE4SS at commit 68caddcf (the build on the target box):
//   CppUserModBase, on_unreal_init/on_update     UE4SS/include/Mod/CppUserModBase.hpp   HARD
//   start_mod / uninstall_mod by literal name    UE4SS/src/Mod/CppMod.cpp               HARD
//   start_cpp_mods() runs in the UE4SSProgram    UE4SSProgram.cpp:386-420               HARD
//     constructor, on the game's main thread,
//     BEFORE setup_unreal() — and, measured on the box, ~970 ms before the game's
//     D3D12CreateDevice (docs/STRAY-RENDERING-FACTS.md §12)
//   on_update is UE4SS's own jthread, ~200 Hz,   UE4SSProgram.cpp:1205,1322-1341        HARD
//     NOT the game thread
//   register_tab(StringViewType,                 UE4SS/include/Mod/CppUserModBase.hpp:  HARD
//     GUI::GUITab::RenderFunctionType)             the callback type is a PLAIN function
//     and on_ui_init + UE4SS_ENABLE_IMGUI          pointer `void(*)(CppUserModBase*)`,
//                                                  not a std::function
//   the debug GUI renders into its OWN OS window  UE4SS/src/GUI/GUI.cpp:510              HARD
//     in every RenderMode; [Debug] RenderMode        (DebuggingGUI::setup ends with
//     only picks WHICH THREAD pumps it              m_os_backend->create_window)
//   Ctrl + [Debug] ToggleGUIKey (default O)       UE4SSProgram.cpp:1103-1130             HARD
//     opens it, and only when GuiConsoleEnabled=1

#include <DynamicOutput/DynamicOutput.hpp>
#include <Mod/CppUserModBase.hpp>
#include <UE4SSProgram.hpp>

#include <imgui.h>

#include <string>

#include "Host.hpp"
#include "Platform.hpp"
#include "TweakUi.hpp"
#include "Version.hpp"

namespace {

RC::StringType Widen(const std::string& s)
{
    RC::StringType out;
    out.reserve(s.size());
    for (const unsigned char c : s)
        out.push_back(static_cast<RC::StringType::value_type>(c));
    return out;
}

// Output::send THROWS when no output device is open; the file log is the real one.
void Say(const RC::StringType& line)
{
    try
    {
        RC::Output::send<RC::LogLevel::Verbose>(line + STR("\n"));
    }
    catch (...)
    {
    }
}

class StrayDlssMod : public RC::CppUserModBase
{
  public:
    StrayDlssMod() : CppUserModBase()
    {
        ModName        = STR("StrayDLSS");
        ModVersion     = Widen(STRAY_DLSS_PLUGIN_VERSION_STRING);
        ModDescription = STR("DLSS Super Resolution for Stray (UE 4.27, D3D12), no ReShade required");
        ModAuthors     = STR("stray-dlss");

        stray_dlss::plugin::Start(sds::ModuleDir(reinterpret_cast<const void*>(&Widen)), sds::GameBinariesDir());

        // The live-tuning tab. Registered here because register_tab's own cleanup is tied to
        // CppUserModBase's destructor, and gated on [STRAYDLSS] TweakUi so a UI fault can be
        // switched off on the box without a 20-minute rebuild. UE4SS_ENABLE_IMGUI must NOT go
        // here: RE-UE4SS's changelog records a crash from exactly that race, and on_ui_init is
        // the documented place.
        if (stray_dlss::plugin::TweakUiEnabled())
            register_tab(STR("StrayDLSS"), &stray_dlss::plugin::RenderTweakTab);
        Say(STR("[StrayDLSS] ") + Widen(STRAY_DLSS_PLUGIN_VERSION_STRING) +
            STR(" loaded; log is <game>/stray-dlss-plugin.log"));
    }

    ~StrayDlssMod() override
    {
        stray_dlss::plugin::Stop();
    }

    auto on_unreal_init() -> void override {}

    // Fired when UE4SS's debug GUI initialises (i.e. when someone first opens it). Without the
    // macro the mod's own imgui translation unit has a NULL context and a different allocator,
    // and the first widget we draw crashes the process.
    auto on_ui_init() -> void override
    {
        UE4SS_ENABLE_IMGUI()
        Say(STR("[StrayDLSS] imgui context adopted; the StrayDLSS tab is live"));
    }

    // NOT the game thread: UE4SS's own event-loop jthread, ~200 Hz.
    auto on_update() -> void override
    {
        stray_dlss::plugin::Tick();
    }
};

} // namespace

#define STRAY_DLSS_API __declspec(dllexport)
extern "C"
{
    // Resolved by literal name in UE4SS/src/Mod/CppMod.cpp; if either is missing UE4SS
    // FreeLibrary's the DLL and logs a warning.
    STRAY_DLSS_API RC::CppUserModBase* start_mod()
    {
        return new StrayDlssMod();
    }

    STRAY_DLSS_API void uninstall_mod(RC::CppUserModBase* mod)
    {
        delete mod;
    }
}
