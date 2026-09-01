// StrayDualSense — the runtime.
//
// Everything the mod does, expressed as GAME INTENT ("the player started scratching", "play
// this vibration asset at this level") and DELIBERATELY free of every UE4SS type. The UE4SS
// glue in Mod.cpp does nothing but translate UFunction callbacks into these calls, so the part
// of this mod that depends on an SDK we cannot build against locally is as thin as it can be.
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

#include "AssetName.hpp"
#include "Config.hpp"
#include "Haptics.hpp"
#include "ScePad.hpp"
#include "Speaker.hpp"
#include "Triggers.hpp"

namespace sds {

class Runtime
{
public:
    // `addressInsideThisModule` is any address in the mod DLL — used to find the mod
    // directory as the fallback asset root.
    void Startup(void* addressInsideThisModule);
    void Shutdown();

    // Called from UE4SS's on_update. THAT IS *NOT* THE GAME THREAD: UE4SS fires on_update from
    // its own event-loop jthread (RE-UE4SS UE4SSProgram.cpp:431,
    // `m_event_loop = std::jthread{&UE4SSProgram::update, this}`) which loops with a 5 ms
    // sleep -- roughly 200 Hz, unsynchronised with game frames.
    //
    // Consequences, and both are load-bearing:
    //   * NOTHING reachable from Tick() may read a UObject. UE reflection off the game thread
    //     is an unsynchronised read of state the engine mutates and the GC can move. The
    //     PadVibrationEnabled read therefore happens inside a UFunction hook instead, where the
    //     thread is naturally correct.
    //   * The config hot-reload assigns only SCALARS (bool/int/float). It never touches the
    //     std::strings, which are written once at startup and read by the worker threads
    //     afterwards -- so a reload cannot race a worker into a freed buffer.
    //
    // Cheap: config reload and the periodic status line, both time-gated.
    void Tick();

    bool Started() const { return m_started.load(std::memory_order_acquire); }
    Config&       Cfg()       { return m_config; }
    const Config& Cfg() const { return m_config; }

    // ---- game intent ------------------------------------------------------------------
    // COMP_CatScratchableComponent_C:SetPS5TriggerActivated(State, Side).
    // MEASURED: fires TWICE per event, once per side, ~0.2 ms apart. Track them separately.
    void OnTriggerActivated(bool state, int side);
    void OnUseStarted();
    void OnAfterUseDone();

    // BP_HKPlayerController_C:StartPS5Vibration(SoundVibration, FadeInTime, Level)
    void OnStartVibration(const std::string& assetFullName, float level, bool levelSeen);
    void OnStopVibration();
    void OnSetVibrationLevel(float level);

    // BP_HKPlayerController_C:StartPS5ControllerSound(SoundVibration, FadeInTime, Level)
    void OnStartControllerSound(const std::string& assetFullName, float level, bool levelSeen);
    void OnStopControllerSound();
    void OnSetControllerSoundLevel(float level);

    // HKGameUserSettings.PadVibrationEnabled — the game's only vibration control (§9).
    void OnPadVibrationEnabled(bool enabled);

    // Counters for the status line and for a human reading the log.
    void NoteHookRegistered(const char* name);
    void NoteHookMissing(const char* name);

private:
    void PadThreadMain();
    void LogStatus();

    Config        m_config;
    ScePad        m_pad;
    TriggerEngine m_triggers;
    HapticEngine  m_haptics;
    SpeakerEngine m_speaker;

    std::wstring m_gameDir;
    std::wstring m_modDir;
    std::wstring m_configPath;
    std::wstring m_vibeDir;
    std::wstring m_spkDir;

    std::thread       m_padThread;
    std::atomic<bool> m_padThreadRunning{false};
    std::atomic<bool> m_started{false};

    uint64_t m_lastTickMs   = 0;
    uint64_t m_lastReloadMs = 0;
    uint64_t m_lastStatusMs = 0;

    std::atomic<unsigned long> m_triggerEvents{0};
    std::atomic<unsigned long> m_vibrationStarts{0};
    std::atomic<unsigned long> m_speakerStarts{0};
};

// The single process-wide instance. UE4SS constructs the mod object; everything else reaches
// the runtime through here so a hook callback needs no captured state.
Runtime& Rt();

} // namespace sds
