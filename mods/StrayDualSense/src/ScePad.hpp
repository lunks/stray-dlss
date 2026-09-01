// StrayDualSense — the libScePad binding.
//
// Stray already loads Sony's own libScePad.dll (delay-load import #9; docs/STRAY-DUALSENSE.md
// §1). The old mod replaced that DLL with a shim so it could reach the pad. A UE4SS plugin is
// in the same process, so it can simply resolve the exports out of the module the game has
// already mapped — no rename, no forwarding, no proxy DLL.
//
// Nothing here touches UE4SS types; this compiles against Win32 alone.
#pragma once

#include <atomic>
#include <cstdint>
#include <string>

namespace sds {

// ScePadControllerInformation, as measured (docs/STRAY-DUALSENSE.md and the task brief):
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
    // The raw bytes, so a slot that decodes oddly can still be diagnosed from the log.
    uint8_t  raw[64]{};
    int32_t  result = 0;
};

// Sony's own numbering, cross-checked twice against Stray's dispatcher
// (docs/STRAY-DUALSENSE.md §3/§6). Do NOT re-derive it from sorted enum strings.
enum class TriggerMode : uint32_t
{
    Off      = 0,
    Feedback = 1,   // {position, strength}         <-- what Stray authors
    Weapon   = 2,   // {start, end, strength}
    Vibration = 3,  // {position, amplitude, frequency}
};

class ScePad
{
public:
    // Resolve the exports out of the already-loaded libScePad.dll. Safe to call repeatedly;
    // returns true once the module is present and every REQUIRED export resolved.
    // The module is delay-loaded, so early calls are expected to fail — poll.
    bool Bind();

    bool IsBound() const { return m_bound.load(std::memory_order_acquire); }

    // Probe user slots 1..4 and adopt the one whose `connected` byte is set.
    //
    // MEASURED: every slot returns a positive handle (0x101, 0x202, 0x303, 0x404) and
    // GetControllerInformation returns 0 for empty slots too. Binding to the first positive
    // handle — what the old shim did — is correct only by luck.
    //
    // `forceUserId` != 0 skips the probe and adopts that slot unconditionally (escape hatch).
    // Returns true if a pad was adopted. Logs every slot's decoded struct either way.
    bool SelectPad(int forceUserId = 0);

    // Re-run SelectPad if the current pad has stopped reporting `connected`. Cheap enough to
    // call once a second from a worker; never call it from the game thread.
    bool RefreshIfLost();

    int32_t Handle() const { return m_handle.load(std::memory_order_acquire); }
    int     UserId() const { return m_userId.load(std::memory_order_acquire); }
    bool    HasPad() const { return Handle() > 0; }

    // ---- the three calls this mod actually makes -------------------------------------
    // Per-side adaptive triggers. Both sides stay in the trigger mask (0x03) and the MODE
    // varies per side — masking a side out leaves it stuck stiff (§8).
    bool SetTriggers(bool left, bool right, uint8_t position, uint8_t strength);

    // ScePadVibrationParam is {uint8 largeMotor, uint8 smallMotor}.
    bool SetVibration(uint8_t large, uint8_t small);

    // Diagnostic only: the pad's own readback of its trigger state, derived from the
    // controller's input report. The FIRST engage after open reads 0/0 because no input
    // report has arrived yet (§5) — never judge on one sample.
    bool GetTriggerState(uint32_t& outLeft, uint32_t& outRight);

    bool GetControllerInformation(int32_t handle, PadInfo& out) const;

    // Counters for the periodic status line.
    unsigned long TriggerOk() const   { return m_trigOk.load(); }
    unsigned long TriggerFail() const { return m_trigFail.load(); }
    unsigned long VibeFail() const    { return m_vibeFail.load(); }

private:
    std::atomic<bool>    m_bound{false};
    std::atomic<int32_t> m_handle{0};
    std::atomic<int>     m_userId{0};

    std::atomic<unsigned long> m_trigOk{0};
    std::atomic<unsigned long> m_trigFail{0};
    std::atomic<unsigned long> m_vibeFail{0};
};

// The directory Stray's own binaries live in — where `vibe/` and `spk/` are expected.
// Derived from the running executable, never hardcoded. Empty on failure.
std::wstring GameBinariesDir();

// The directory this mod's DLL lives in, used as the fallback asset root.
std::wstring ModuleDir(void* addressInsideThisModule);

} // namespace sds
