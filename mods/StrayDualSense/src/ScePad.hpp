// StrayDualSense — the libScePad binding.
//
// Stray already loads Sony's own libScePad.dll (delay-load import #9; docs/STRAY-DUALSENSE.md
// §1). A UE4SS plugin is in the same process, so it resolves the exports out of the module the
// game has already mapped — no rename, no forwarding, no proxy DLL.
//
// Only the TRIGGER path goes through libScePad. Haptics do not: scePadSetVibration reads
// exactly two amplitude bytes and structurally cannot carry a waveform (§12, RVA 0xDC50), so
// the coils are driven as audio through WASAPI instead (AudioPlayer) once HidMode has taken
// them out of motor emulation. There is no SetVibration here on purpose.
//
// Nothing here touches UE4SS types; this compiles against Win32 alone.
#pragma once

#include <atomic>
#include <cstdint>

#include "TriggerEffect.hpp"

namespace sds {

// ScePadControllerInformation, as measured (§11):
//   +0x00 float  touchPadInfo.pixelDensity      44.86 on a real pad
//   +0x04 uint16 touchPadInfo.resolution.x      1920
//   +0x06 uint16 touchPadInfo.resolution.y      1080
//   +0x08 uint8  stickInfo.deadZoneLeft         13
//   +0x09 uint8  stickInfo.deadZoneRight        13
//   +0x0a uint8  connectionType
//   +0x0b uint8  connectedCount                 1
//   +0x0c uint8  connected                      1   <-- THE ONLY RELIABLE DISCRIMINATOR
//   +0x0d uint8  deviceClass
// An EMPTY slot returns all zeroes *and a success return code*, so the return code proves
// nothing. Read byte 12.
struct PadInfo
{
    float    pixelDensity   = 0.0f;
    uint16_t touchpadWidth  = 0;
    uint16_t touchpadHeight = 0;
    uint8_t  deadZoneLeft   = 0;
    uint8_t  deadZoneRight  = 0;
    uint8_t  connectionType = 0;
    uint8_t  connectedCount = 0;
    uint8_t  connected      = 0;
    uint8_t  deviceClass    = 0;
    uint8_t  raw[64]{};      // so a slot that decodes oddly can still be diagnosed from the log
    int32_t  result = 0;
};

class ScePad
{
public:
    // Resolve the exports out of the already-loaded libScePad.dll. Safe to call repeatedly;
    // true once the module is present and every REQUIRED export resolved. The module is
    // delay-loaded, so early calls are expected to fail — poll.
    bool Bind();
    bool IsBound() const { return m_bound.load(std::memory_order_acquire); }

    // Probe user slots 1..4 and adopt the one whose `connected` byte is set. `forceUserId`
    // != 0 skips the probe (escape hatch).
    //
    // MEASURED 2026-09-02: this RACES the game. On one launch slot 1 answered connected=1 and
    // the triggers worked; on the next, every slot handed back a handle and an all-zero
    // information struct. libScePad only knows about a pad the GAME has opened, and our pad
    // thread starts as soon as the module is mapped — so a probe can simply be too early.
    // It is therefore retried, and the per-slot dump is throttled rather than dropped: at
    // ~2 s it is 5 log lines a probe, which over a session buries everything else.
    bool SelectPad(int forceUserId = 0);

    // Re-run SelectPad if the current pad has stopped reporting `connected`. Worker only.
    bool RefreshIfLost();

    int32_t Handle() const { return m_handle.load(std::memory_order_acquire); }
    int     UserId() const { return m_userId.load(std::memory_order_acquire); }
    bool    HasPad() const { return Handle() > 0; }

    // scePadSetTriggerEffect with a block built by BuildTriggerParam. Worker thread only.
    bool SetTriggers(const uint8_t param[kTriggerParamSize], bool left, bool right,
                     const TriggerEffect& effect);

    // Diagnostic only: the pad's own readback, derived from its input report (§5). The FIRST
    // engage after open reads 0/0 because no report has arrived yet — never judge on one.
    bool GetTriggerState(uint32_t& outLeft, uint32_t& outRight);

    bool GetControllerInformation(int32_t handle, PadInfo& out) const;

    unsigned long TriggerOk() const   { return m_trigOk.load(); }
    unsigned long TriggerFail() const { return m_trigFail.load(); }
    // How many times the slots have been probed, and how many of those found nothing. A pad
    // that never appears must be distinguishable from a probe that never ran.
    unsigned long Probes() const      { return m_probes.load(); }
    unsigned long ProbeMisses() const { return m_probeMisses.load(); }

private:
    std::atomic<bool>    m_bound{false};
    std::atomic<int32_t> m_handle{0};
    std::atomic<int>     m_userId{0};

    std::atomic<unsigned long> m_trigOk{0};
    std::atomic<unsigned long> m_trigFail{0};
    std::atomic<unsigned long> m_probes{0};
    std::atomic<unsigned long> m_probeMisses{0};
};

} // namespace sds
