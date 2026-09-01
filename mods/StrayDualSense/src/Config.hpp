// StrayDualSense — configuration.
//
// INI, not JSON: it is hand-edited on a machine that is mid-session, over ssh, by someone who
// wants to change one gain and relaunch. It is also hot-reloaded, because this mod cannot be
// built or tested locally — the only tuning loop is the user editing a value on the box, and
// making that not require a relaunch is worth the code.
//
// EVERY default here is the value the working two-part mod shipped. Changing one is a
// deliberate act, not a tidy-up.
//
// No UE4SS types here.
#pragma once

#include <cstdint>
#include <string>

#include "Log.hpp"

namespace sds {

struct Config
{
    // ---- lifecycle -------------------------------------------------------------------
    bool     enabled          = true;
    LogLevel logLevel         = LogLevel::Info;

    // ---- pad selection ---------------------------------------------------------------
    // 0 = probe slots 1..4 and adopt the one whose `connected` byte is set. MEASURED: every
    // slot answers with a positive handle and a success code, so anything else is a guess.
    // Non-zero forces that user slot and SKIPS the probe (escape hatch only).
    int      padUserId        = 0;
    // Seconds between "is the pad still there" checks on the worker thread.
    float    padPollSeconds   = 2.0f;

    // ---- triggers --------------------------------------------------------------------
    bool     triggers         = true;
    // GAME-AUTHORED, read out of HKPlayerController.m_scratchablePS5TriggerEffect at +0x730:
    // FEEDBACK{position 0, strength 2}. `position=2, strength=8` was an early guess and feels
    // like a hard block. Do not "improve" these (docs/STRAY-DUALSENSE.md §6).
    uint8_t  triggerPosition  = 0;
    uint8_t  triggerStrength  = 2;
    // Diagnostic hardware readback after each change. Costs a 120 ms sleep ON THE WORKER
    // thread, never the game thread. Off by default; §5 explains why one sample proves
    // nothing (the first engage after open always reads 0/0).
    bool     triggerReadback  = false;

    // ---- haptics ---------------------------------------------------------------------
    bool     haptics          = true;
    // Master gain, 0..255. 255 = unity. MEASURED: the game's vibration chain is unity gain
    // end to end (§9), so unity is the faithful value and anything else is colouring.
    int      hapticGain       = 255;
    // The envelope sample rate the .env files were generated at: one uint8 per 5 ms.
    int      envelopeStepMs   = 5;
    // Runaway cap. ~60 s at 5 ms. Reaching it means a Stop went missing, and is logged loudly.
    int      maxEnvelopeSteps = 12000;
    // The working Lua mod passed loop=1 for EVERY StartPS5Vibration ("play %s %d 1") and
    // relied on StopPS5Vibration to end it. Ported faithfully rather than "corrected", but
    // exposed so it can be flipped on the box without a rebuild if a one-shot ever latches.
    bool     hapticLoop       = true;

    // ---- controller speaker ----------------------------------------------------------
    bool     speaker          = true;
    // SBFX_Boost: InputGainDb=+5.0, Ratio 1:1, Threshold 0 — a level trim, not a compressor.
    // 10^(5/20) = 1.7783 (§10).
    float    speakerBoost     = 1.7783f;
    // Substring of the WASAPI endpoint's friendly name. MEASURED:
    // "Speakers (DualSense Wireless Controller)", 4ch/48000/32-bit float.
    std::string speakerDeviceMatch = "DualSense";
    // The .f32 assets are mono float32 at this rate. If the endpoint's mix rate differs we
    // resample rather than play at the wrong pitch, and say so in the log.
    int      speakerAssetRate = 48000;
    // As above: the Lua mod passed loop=1 for every StartPS5ControllerSound.
    bool     speakerLoop      = true;

    // ---- assets ----------------------------------------------------------------------
    // Relative paths are resolved against the game's Binaries/Win64 directory first and the
    // mod's own directory second. Never hardcoded (§11).
    std::string vibeDir       = "vibe";
    std::string spkDir        = "spk";

    // ---- diagnostics -----------------------------------------------------------------
    float    hookRetrySeconds  = 3.0f;   // MEASURED: registration fails until the BP loads.
    float    statusSeconds     = 30.0f;
    float    configReloadSeconds = 5.0f; // 0 disables hot reload.

    // Parse `path`. Missing keys keep their defaults; unknown keys are logged and ignored.
    // Returns false only if the file could not be opened.
    bool Load(const std::wstring& path);

    // Re-read if the file's mtime moved. Only the LIVE fields are applied (gains, trigger
    // shape, enables, log level); pad selection and device matching need a relaunch and are
    // logged as such rather than half-applied.
    bool ReloadIfChanged(const std::wstring& path);

    void LogSummary(const char* what) const;

private:
    uint64_t m_lastWriteTime = 0;
};

} // namespace sds
