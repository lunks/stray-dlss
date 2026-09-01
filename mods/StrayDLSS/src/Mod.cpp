// StrayDLSS — the UE4SS glue, and NOTHING else.
//
// This is the only file in the mod that includes a UE4SS header, exactly as in the sibling
// mods/StrayDualSense/src/Mod.cpp whose scaffolding this lifts. Everything it does is call into
// plain-Win32 code that is compiled and link-tested without the SDK.
//
// 0.0.1 is the attach-timing PROBE (plan Task 2): it hooks nothing the game renders with and
// changes nothing on screen. It answers, from inside the process, whether start_mod() runs
// before the game's D3D12CreateDevice and what an early export hook sees with ReShade loaded.
//
// PROVENANCE, read out of RE-UE4SS at commit 68caddcf (the build on the target box):
//   CppUserModBase, on_unreal_init/on_update     UE4SS/include/Mod/CppUserModBase.hpp   HARD
//   start_mod / uninstall_mod by literal name    UE4SS/src/Mod/CppMod.cpp               HARD
//   start_cpp_mods() runs in the UE4SSProgram    UE4SSProgram.cpp:386-420               HARD
//     constructor, on the game's main thread,
//     BEFORE setup_unreal()
//   on_update is UE4SS's own jthread, ~200 Hz,   UE4SSProgram.cpp:1205,1322-1341        HARD
//     NOT the game thread

#include <DynamicOutput/DynamicOutput.hpp>
#include <Mod/CppUserModBase.hpp>

#include <string>

#include "Log.hpp"
#include "Platform.hpp"
#include "Probe.hpp"
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
        ModDescription = STR("DLSS Super Resolution for Stray (attach-timing probe build)");
        ModAuthors     = STR("stray-dlss");

        // <game>/stray-dlss-plugin.log, next to ReShade.log and stray-dlss.log so the three
        // can be laid against each other by timestamp.
        sds::Log::Open(sds::GameBinariesDir() + L"stray-dlss-plugin.log", sds::LogLevel::Debug);
        SDS_LOG_INFO("StrayDLSS %s attaching (UE4SS C++ mod)", STRAY_DLSS_PLUGIN_VERSION_STRING);
        stray_dlss::probe::Start();
        Say(STR("[StrayDLSS] ") + Widen(STRAY_DLSS_PLUGIN_VERSION_STRING) +
            STR(" loaded; log is <game>/stray-dlss-plugin.log"));
    }

    ~StrayDlssMod() override
    {
        stray_dlss::probe::Stop();
        SDS_LOG_INFO("StrayDLSS detaching");
        sds::Log::Close();
    }

    auto on_unreal_init() -> void override
    {
        SDS_LOG_INFO("on_unreal_init: the Unreal module is up");
    }

    // NOT the game thread: UE4SS's own event-loop jthread, ~200 Hz.
    auto on_update() -> void override
    {
        stray_dlss::probe::Tick();
        for (const std::string& line : sds::Log::TakeMirrorLines())
            Say(STR("[StrayDLSS] ") + Widen(line));
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
