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

#include <DynamicOutput/DynamicOutput.hpp>
#include <Mod/CppUserModBase.hpp>

#include <string>

#include "Host.hpp"
#include "Platform.hpp"
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
        Say(STR("[StrayDLSS] ") + Widen(STRAY_DLSS_PLUGIN_VERSION_STRING) +
            STR(" loaded; log is <game>/stray-dlss-plugin.log"));
    }

    ~StrayDlssMod() override
    {
        stray_dlss::plugin::Stop();
    }

    auto on_unreal_init() -> void override {}

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
