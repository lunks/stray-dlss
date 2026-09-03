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
//     bit5 SPEAKER_VOLUME_ENABLE  bit6 MIC_VOLUME_ENABLE  bit7 AUDIO_CONTROL_ENABLE
//
// CORRECTED 2026-09-03. That list used to read "bit4/bit5 audio volume / audio path,
// bit6/bit7 mic LED / mute", and BOTH halves were wrong — checked against the Linux kernel's
// own DualSense driver, drivers/hid/hid-playstation.c:154-164. The second half mattered:
// there is NO "audio path" validity bit. The path lives in the `audio_control` BYTE (offset
// 8, bits 5:4), gated by bit7. Anyone acting on the old comment would have claimed headphone
// and speaker volume while supplying zeros — the §12 two-writers trap — and never selected a
// path. The full corrected layout, its citations and the routing table are in PadAudio.hpp.
//
// A report whose flag does NOT claim compatible-vibration stops the firmware re-asserting
// emulation, and the coils take the waveform. We send 0x00 (claim nothing) with byte 2 = 0.
// NOT 0xFC: that claims the trigger fields while supplying zeros, which first killed the
// adaptive triggers and then latched them on, depending on whose write landed last.
//
// The optional PAD-SPEAKER CLAIM (PadAudio.hpp, `PadSpeakerRoute = hid`) is OR'd into the
// same report rather than written by a second writer. It contributes bits 5 and 7 only —
// disjoint from every bit above — and `ComposeValidFlag0` masks bits 0..3 out of it
// unconditionally, so the coil behaviour measured working on 2026-09-03 is untouched by
// construction. It is OFF unless Sony's own API has been tried and refused.
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

#include "PadAudio.hpp"

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

    // The pad-audio claim to fold into every report from here on. A default-constructed
    // claim (`claims == false`) makes the report byte-identical to what this class wrote
    // before the claim existed — so turning the feature off is a true revert, not a
    // different set of bytes that happens to look inert. Set by Runtime, and ONLY when
    // Sony's own API is unavailable or has refused (PadAudio.hpp).
    void SetAudioClaim(const PadAudioClaim& claim);
    bool AudioClaimActive() const { return m_audioClaimActive.load(std::memory_order_relaxed); }

    bool          Opened() const   { return m_opened.load(std::memory_order_relaxed); }
    unsigned long Writes() const   { return m_writes.load(std::memory_order_relaxed); }
    unsigned long Failures() const { return m_failures.load(std::memory_order_relaxed); }

private:
    void WorkerMain();
    bool EnsureOpenLocked();                          // m_mutex held
    // `flag0` is the COIL-MODE base; the audio claim (if any) is OR'd in by
    // ComposeValidFlag0, which masks the coil and trigger bits out of it unconditionally.
    bool WriteLocked(uint8_t flag0, const PadAudioClaim& claim, const char* why); // m_mutex held
    void CloseLocked();

    const Config* m_config = nullptr;

    std::thread       m_worker;
    std::atomic<bool> m_running{false};

    std::mutex    m_mutex;
    PadAudioClaim m_audioClaim{};              // guarded by m_mutex
    std::atomic<bool> m_audioClaimActive{false};
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
