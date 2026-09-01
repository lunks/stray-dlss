// StrayDualSense — putting the coils into WAVEFORM mode.
//
// THE SINGLE MOST IMPORTANT THING IN THIS PLUGIN (docs/STRAY-DUALSENSE.md §12, measured
// working 2026-09-01). The DualSense has no rumble motors: "rumble" is synthesised by the
// firmware on the same two voice coils from two amplitude bytes. While the firmware is in that
// emulation mode, audio sent to the coils goes NOWHERE, which is why every earlier audio
// attempt silently failed.
//
// USB output report 0x02, byte 1 is `valid_flag0`, whose bits are VALIDITY CLAIMS about the
// rest of the report:
//     bit0 COMPATIBLE_VIBRATION   bit1 HAPTICS_SELECT
//     bit2 / bit3 right / left trigger FFB data is valid
//     bit4 / bit5 audio volume / audio path        bit6 / bit7 mic LED / mute
// A report whose flag does NOT claim compatible-vibration stops the firmware re-asserting
// emulation, and the coils take the waveform. We send 0x00 (claim nothing) with byte 2 = 0.
// NOT 0xFC: that claims the trigger fields while supplying zeros, which first killed the
// adaptive triggers and then latched them on, depending on whose write landed last.
//
// libScePad writes its own output reports for the triggers and they carry the same flag byte,
// so one write at startup is undone the moment the game touches the pad. The mode is
// RE-ASSERTED on a cadence and again immediately before each waveform starts.
//
// The device is opened directly (SetupAPI + HidD_GetAttributes, VID 054C PID 0CE6): libScePad
// has no haptic-audio API at all. Nothing here touches UE4SS; Win32 only.
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#ifndef _WINDOWS_
using HANDLE = void*;
#endif

namespace sds {

struct Config;

// The two flag values that matter, named so the intent is readable at the call site.
constexpr uint8_t kValidFlag0Waveform = 0x00;   // claim nothing: coils take the waveform
constexpr uint8_t kValidFlag0Rumble   = 0x01;   // bit0 only: hand the coils back to emulation

class HidMode
{
public:
    ~HidMode();

    void Start(const Config& config);
    // Stops the re-assert thread and writes kValidFlag0Rumble back, so the pad is left in
    // the state the game expects.
    void Shutdown();

    // Re-assert the configured flag NOW. Called by the haptic worker immediately before a
    // waveform starts. Thread-safe; never blocks on anything but the HID write itself.
    void AssertNow(const char* why);

    bool          Opened() const   { return m_opened.load(std::memory_order_relaxed); }
    unsigned long Writes() const   { return m_writes.load(std::memory_order_relaxed); }
    unsigned long Failures() const { return m_failures.load(std::memory_order_relaxed); }

private:
    void WorkerMain();
    bool EnsureOpenLocked();                          // m_mutex held
    bool WriteLocked(uint8_t flag0, const char* why); // m_mutex held
    void CloseLocked();

    const Config* m_config = nullptr;

    std::thread       m_worker;
    std::atomic<bool> m_running{false};

    std::mutex   m_mutex;
    HANDLE       m_handle = nullptr;
    std::string  m_devicePath;
    uint32_t     m_reportLength = 0;      // OutputReportByteLength from the HID caps
    uint64_t     m_lastOpenAttemptMs = 0;
    int          m_openFailuresLogged = 0;

    std::atomic<bool>          m_opened{false};
    std::atomic<unsigned long> m_writes{0};
    std::atomic<unsigned long> m_failures{0};
};

} // namespace sds
