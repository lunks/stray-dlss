// StrayDualSense — the runtime.
//
// Everything the mod does, expressed as GAME INTENT ("the player started scratching", "play
// this vibration asset at this level, fading in over a second") and DELIBERATELY free of
// every UE4SS type. The UE4SS glue in Mod.cpp does nothing but translate UFunction callbacks
// into these calls, so the part of this mod that depends on an SDK we cannot build against
// locally is as thin as it can be.
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#include "AudioPlayer.hpp"
#include "Config.hpp"
#include "HidMode.hpp"
#include "LoopList.hpp"
#include "ScePad.hpp"
#include "TriggerEffect.hpp"
#include "Triggers.hpp"

namespace sds {

// StartPS5Vibration(SoundVibration, FadeInTime, Level) and
// StartPS5VibrationOnAudioComponent(AudioComponent, SoundVibration, FadeInTime, Level,
// VibrationComponent), already resolved BY TYPE by the hook (§ "two hook shapes").
struct VibrationStart
{
    std::string soundFullName;       // "SoundWave /Game/.../CatPurr2_VIBE.CatPurr2_VIBE"
    std::string componentFullName;   // the AudioComponent, empty on the plain path
    float       level     = 1.0f;
    bool        levelSeen = false;   // false = no float parameter was found at all
    float       fadeIn    = 0.0f;
};

class Runtime
{
public:
    // `addressInsideThisModule` is any address in the mod DLL — used to find the mod
    // directory as the fallback asset root.
    void Startup(const void* addressInsideThisModule);
    void Shutdown();

    // Called from UE4SS's on_update. THAT IS *NOT* THE GAME THREAD: UE4SS fires on_update from
    // its own event-loop jthread (RE-UE4SS UE4SSProgram.cpp:431), looping with a 5 ms sleep.
    // Nothing reachable from here may read a UObject. The config hot-reload assigns only
    // scalars, so it cannot race a worker into a freed string.
    void Tick();

    bool          Started() const { return m_started.load(std::memory_order_acquire); }
    Config&       Cfg()           { return m_config; }
    const Config& Cfg() const     { return m_config; }

    // ---- game intent: all called from UFunction hooks on the GAME thread ------------------
    // HKPlayerController::m_scratchablePS5TriggerEffect, in the GAME's enum space. `ok` false
    // means the struct could not be read and the fallback stays in force.
    void OnTriggerEffectRead(const TriggerEffect& effect, bool ok);
    // COMP_CatScratchableComponent_C:SetPS5TriggerActivated(State, Side). Accumulates per side.
    void OnTriggerActivated(bool state, int side);
    void OnUseStarted();
    void OnAfterUseDone();

    void OnStartVibration(const VibrationStart& start);
    void OnStopVibration(float fadeOut);                                       // global
    void OnStopVibrationOnComponent(const std::string& componentFullName, float fadeOut);
    void OnSetVibrationLevel(float level);                                     // ~60 Hz

    void OnStartControllerSound(const std::string& soundFullName, float level, bool levelSeen,
                                float fadeIn);
    void OnStopControllerSound(float fadeOut);
    void OnSetControllerSoundLevel(float level);

    // HKGameUserSettings.PadVibrationEnabled — the game's only vibration control (§9).
    void OnPadVibrationEnabled(bool enabled);

    void NoteHookRegistered(const char* name);
    void NoteHookMissing(const char* name);

private:
    void PadThreadMain();
    void LogStatus();
    bool LoadLoopList(LoopList& list, const std::string& fileName, const char* what);
    void LoadLoopLists();

    Config        m_config;
    ScePad        m_pad;
    HidMode       m_hidMode;
    TriggerEngine m_triggers;
    AudioPlayer   m_haptics;
    AudioPlayer   m_speaker;
    LoopList      m_hapticLoops;
    LoopList      m_spkLoops;

    std::wstring m_gameDir;
    std::wstring m_modDir;
    std::wstring m_configPath;
    std::wstring m_hapticDir;
    std::wstring m_spkDir;

    std::thread       m_padThread;
    std::atomic<bool> m_padThreadRunning{false};
    std::atomic<bool> m_started{false};

    // Which AudioComponent owns the haptic in flight, if any. Only a stop naming THIS
    // component may end it: StopPS5VibrationOnAudioComponent fires ~700 times a session as
    // routine housekeeping and wiring it to a global stop killed the purr after 262 ms.
    std::mutex  m_componentMutex;
    std::string m_playingComponent;

    std::atomic<bool> m_padVibrationEnabled{true};
    std::atomic<bool> m_effectFromGame{false};

    uint64_t m_lastReloadMs = 0;
    uint64_t m_lastStatusMs = 0;

    std::atomic<unsigned long> m_triggerEvents{0};
    std::atomic<unsigned long> m_vibrationStarts{0};
    std::atomic<unsigned long> m_componentStopsHonoured{0};
    std::atomic<unsigned long> m_componentStopsIgnored{0};
    std::atomic<unsigned long> m_speakerStarts{0};
};

// The single process-wide instance. UE4SS constructs the mod object; everything else reaches
// the runtime through here so a hook callback needs no captured state.
Runtime& Rt();

} // namespace sds
