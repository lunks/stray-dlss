// StrayDualSense — the runtime.
//
// Everything the mod does, expressed as GAME INTENT ("the player started scratching", "the
// game asked for this vibration asset") and DELIBERATELY free of every UE4SS type. The UE4SS
// glue in Mod.cpp does nothing but translate UFunction callbacks into these calls, so the
// part of this mod that depends on an SDK we cannot build against locally is as thin as it
// can be.
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
#include "SubmixWatch.hpp"
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

// The two submixes the engine renders for the pad, and everything one of them owns on our
// side. The same code runs twice: Submix_vibrationMaster -> the coils (RL/RR) and
// Submix_controllerMaster -> the speaker (FL/FR). Both are tapped by an ISubmixBufferListener
// that the engine may still call after we are gone, so `tap` is LEAKED on purpose
// (SubmixTap.hpp): detached, never deleted.
struct Lane
{
    const char* tag   = "";   // the tap's tag and the SUBMIX line's word: "vibration"/"speaker"
    const char* owner = "";   // the status prefix: "COILS" / "SPEAKER"

    submix::Tap*       tap = nullptr;
    submix::SubmixRing ring;
    // The tap has delivered a REAL SIGNAL (>= SubmixLiveThreshold), not merely callbacks.
    std::atomic<bool>  live{false};

    // UE4SS update thread only.
    uint64_t lastCallbacks = 0;     // for a per-second RATE, not a total
    // A per-window peak is 0.00000 whenever nothing is playing, so one pasted status line
    // cannot tell "silent now" from "silent always". These two can, and they cost nothing.
    float    peakEver     = 0.0f;
    uint64_t lastSignalMs = 0;      // 0 = the tap has never carried a signal
    // Opened by a Start hook on the GAME thread, sampled and closed from SubmixStatus on
    // UE4SS's update thread — hence Runtime::m_watchMutex.
    SubmixWatch watch;
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

    // The speaker. The engine plays the sound itself once the Blueprint gate is open; these
    // only LOG the intent and open the speaker lane's watch, so one line per start says what
    // the engine put in Submix_controllerMaster for that asset.
    void OnStartControllerSound(const std::string& soundFullName, float level, bool levelSeen,
                                float fadeIn);
    void OnStopControllerSound(float fadeOut);
    void OnSetControllerSoundLevel(float level);

    // HKGameUserSettings.PadVibrationEnabled — the game's only vibration control (§9).
    void OnPadVibrationEnabled(bool enabled);

    void NoteHookRegistered(const char* name);
    void NoteHookMissing(const char* name);

    // BP_HKPlayerController_C.DebugPS5Haptic, reported by the glue every time it tries to open
    // the gate. `open` is the value READ BACK after the write, so it is the gate's real state
    // and not our intent. A gate that never opens is the single most likely reason for "the
    // game asked for a vibration and the engine mixed nothing", so it belongs in the STATUS
    // line rather than in one INFO line at the top of a 40 MB log.
    void OnHapticGate(bool open, bool wrote);

    // ---- the submix taps ---------------------------------------------------------------
    // True while the taps exist and the listeners have not been registered, or while the
    // reroute watchdog has asked for a re-arm. The UE4SS glue polls this from inside a
    // game-thread hook and stops as soon as it is false, so a session that never binds is one
    // WARN per attempt and not a silent nothing.
    bool SubmixWantsBinding() const;
    // WHO DRIVES THE COILS. The facts are gathered here and judged by the pure CoilOwner
    // module; every status line, warning and gate asks this verdict, never the config or the
    // tap's counters on their own (that is how a silent submix passed for a working one).
    CoilFacts   CoilFactsNow() const;
    CoilVerdict Coils() const { return JudgeCoils(CoilFactsNow()); }
    bool        SubmixOwnsCoils() const { return Coils().owner == CoilOwner::Submix; }
    // WHO DRIVES THE SPEAKER, by the same rule (SubmixWatch.hpp, JudgeLane).
    LaneVerdict Speaker() const;

    // Called ON THE GAME THREAD by the UE4SS glue, which resolves the UObjects and the
    // executable's image range reflectively and hands them over as raw pointers — the runtime
    // itself stays free of every UE4SS type. The three submix objects are the two masters to
    // re-parent (their ParentSubmix already written by the glue) and their new parent (its
    // OutputVolume already written to 0); ALL THREE must resolve before anything is
    // submitted, because the parent's re-registration must be followed by every master's or
    // a master re-parented earlier is dropped from the parent's child list. Returns true when
    // the listeners have been handed to the engine; false means "not yet, or refused", and the
    // reason has already been logged.
    bool BindSubmixTap(const void* worldObject, const void* engineObject,
                       void* vibrationMasterObject, void* speakerMasterObject,
                       void* rerouteParentObject, const void* imageBase, std::size_t imageSize);

private:
    void PadThreadMain();
    void LogStatus();
    void StartSubmix();
    void SubmixStatus();     // the numbers proof: one log line per lane and one status file
    void LaneStatus(Lane& lane, double seconds, uint64_t nowMs, char* line, std::size_t lineSize);
    void SubmixWarnIfDue(uint64_t now);
    // Detects "bound, but a subtree has stopped rendering" and re-arms the reroute. NOT a
    // fallback: it never gives the coils back to the asset path.
    void RerouteWatchdog(uint64_t now);
    // Opens the sink and attaches the lane's ring, on that lane's first real signal.
    void StartLaneAtHandover(Lane& lane, float peak);
    // Config::submixLiveThreshold, clamped to something a float comparison can mean.
    float LiveThreshold() const;
    // Opens a lane's correlation watch for the asset the game just asked for.
    void  OpenWatch(Lane& lane, const std::string& asset);
    // One line per closed watch: what the engine mixed while the game was asking for an asset.
    void  ReportWatch(const Lane& lane, const WatchVerdict& v);
    bool LoadLoopList(LoopList& list, const std::string& fileName, const char* what);
    void LoadLoopLists();
    void ApplySinkGains();

    // ---- the controller speaker's ROUTING -----------------------------------------------
    // Selects where the pad sends the audio we stream into its endpoint. Runs on the pad
    // thread — the only thread that owns a libScePad handle. Re-applies when the pad
    // changes, when the settings change (hot reload), or on the optional cadence.
    void ApplySpeakerRoute(const char* why);
    // Everything that decides what is written, folded into one value so "has anything
    // changed" is a comparison rather than six.
    uint64_t SpeakerRouteSignature() const;

    Config        m_config;
    ScePad        m_pad;
    HidMode       m_hidMode;
    TriggerEngine m_triggers;
    AudioPlayer   m_haptics;
    LoopList      m_hapticLoops;

    // The two lanes and the one sink. `m_tapMaster` is a meter-only probe on the engine's
    // MASTER submix, never attached to a ring: it is what turns a null result into a
    // diagnosis (master firing while a lane is silent = the tap works and the engine is not
    // rendering that subtree; neither firing = the tap itself is broken).
    Lane               m_coils;
    Lane               m_speaker;
    submix::SubmixSink m_submixSink;
    submix::Tap*       m_tapMaster    = nullptr;
    std::atomic<bool>  m_submixBound{false};
    std::atomic<bool>  m_submixRefused{false};
    std::atomic<bool>  m_submixRerouted{false};
    // The reroute re-arm. The reroute is submitted from the game thread by the glue, so the
    // watchdog (UE4SS update thread) can only ASK; SubmixWantsBinding() then returns true and
    // the glue's next pass re-writes the UObject links and re-calls BindSubmixTap.
    std::atomic<bool>  m_rerouteRearmWanted{false};
    StallWatchdog      m_watchdog;                 // update thread only
    bool               m_sinkStarted = false;      // UE4SS update thread only
    uint64_t           m_lastSubmixWarnMs = 0;
    std::atomic<int>   m_submixBindAttempts{0};
    const void*        m_submixDevice = nullptr;
    std::string        m_submixStatusPath;   // narrow, for the log; the wide one is below
    std::wstring       m_submixStatusFile;
    uint64_t           m_lastSubmixStatusMs = 0;
    uint64_t           m_submixStatusWindowMs = 0;
    std::mutex         m_watchMutex;   // both lanes' watches

    // The pad-speaker routing, all touched only by the pad thread except the atomics the
    // STATUS line reads.
    uint64_t                   m_speakerRouteApplied = 0;   // signature last written; 0 = never
    int32_t                    m_speakerRouteHandle  = 0;   // the handle it was written to
    uint64_t                   m_lastSpeakerRouteMs  = 0;
    std::atomic<bool>          m_speakerRouteSonyOk{false};
    std::atomic<bool>          m_speakerRouteHidOn{false};
    std::atomic<int32_t>       m_speakerRouteLastPathResult{0};
    std::atomic<unsigned long> m_speakerRouteApplies{0};

    std::wstring m_gameDir;
    std::wstring m_modDir;
    std::wstring m_configPath;
    std::wstring m_hapticDir;

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
    std::atomic<unsigned long> m_speakerStops{0};

    std::atomic<bool>          m_hapticGateOpen{false};
    std::atomic<unsigned long> m_hapticGateWrites{0};
    std::atomic<unsigned long> m_hapticGateMisses{0};
};

// The single process-wide instance. UE4SS constructs the mod object; everything else reaches
// the runtime through here so a hook callback needs no captured state.
Runtime& Rt();

} // namespace sds
