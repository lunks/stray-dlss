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
#include "SubmixDiscovery.hpp"
#include "SubmixDsp.hpp"
#include "SubmixSink.hpp"
#include "SubmixTap.hpp"
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

    // ---- the submix spike ------------------------------------------------------------
    // True while HapticSource is measure|submix and the listener has not been registered.
    // The UE4SS glue polls this from inside a game-thread hook and stops as soon as it is
    // false, so a session that never binds is one WARN per attempt and not a silent nothing.
    bool SubmixWantsBinding() const;
    // WHO DRIVES THE COILS. The facts are gathered here and judged by the pure CoilOwner
    // module; every status line, warning and gate asks this verdict, never the config or the
    // tap's counters on their own (that is how a silent submix passed for a working one).
    CoilFacts   CoilFactsNow() const;
    CoilVerdict Coils() const { return JudgeCoils(CoilFactsNow()); }
    bool        SubmixOwnsCoils() const { return Coils().owner == CoilOwner::Submix; }

    // Called ON THE GAME THREAD by the UE4SS glue, which resolves the UObjects and the
    // executable's image range reflectively and hands them over as raw pointers — the runtime
    // itself stays free of every UE4SS type. `submixObject` may be null, which registers on
    // the engine's MASTER submix (AudioMixerDevice.cpp:2350). The two reroute objects are the
    // USoundSubmix to re-parent and its new parent, both null unless SubmixReroute is on and
    // both resolved (the glue has already written their UPROPERTYs). Returns true when the
    // listener has been handed to the engine; false means "not yet, or refused", and the
    // reason has already been logged.
    bool BindSubmixTap(const void* worldObject, const void* engineObject, void* submixObject,
                       bool submixObjectResolved, void* rerouteMasterObject,
                       void* rerouteParentObject, const void* imageBase, std::size_t imageSize);

private:
    void PadThreadMain();
    void LogStatus();
    void StartSubmix();
    void SubmixStatus();     // the numbers proof: one log line and one status file line
    void SubmixWarnIfDue(uint64_t now);
    void StartSinkAtHandover(float peak);
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

    // The submix spike. The taps are LEAKED on purpose (SubmixTap.hpp): the engine may still
    // call a listener after UnregisterSubmixBufferListener returns, so they are raw pointers
    // that are detached, never deleted.
    submix::SubmixRing m_submixRing;
    submix::SubmixSink m_submixSink;
    submix::Tap*       m_tapVibration = nullptr;
    submix::Tap*       m_tapMaster    = nullptr;
    std::atomic<bool>  m_submixBound{false};
    // The tap has delivered a REAL SIGNAL, not merely callbacks. Until it does, the asset path
    // keeps driving the coils — the 2026-09-03 run left the user with no haptics at all because
    // HapticSource=submix disabled the assets on a tap that never bound.
    std::atomic<bool>  m_submixLive{false};
    std::atomic<bool>  m_submixRefused{false};
    std::atomic<bool>  m_submixRerouted{false};
    bool               m_sinkStarted = false;      // UE4SS update thread only
    uint64_t           m_lastSubmixWarnMs = 0;
    std::atomic<int>   m_submixBindAttempts{0};
    const void*        m_submixDevice = nullptr;
    std::string        m_submixStatusPath;   // narrow, for the log; the wide one is below
    std::wstring       m_submixStatusFile;
    uint64_t           m_lastSubmixStatusMs = 0;
    uint64_t           m_submixStatusWindowMs = 0;
    uint64_t           m_lastSubmixCallbacks = 0;   // for a per-second RATE, not a total

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
