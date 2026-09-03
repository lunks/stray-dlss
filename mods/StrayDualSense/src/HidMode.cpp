#include "HidMode.hpp"

#include "Config.hpp"
#include "Log.hpp"
#include "Platform.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <setupapi.h>
// mingw-w64's hidsdi.h/hidpi.h lack the `extern "C"` guard the Windows SDK copies have, so
// the HidD_* imports mangle as C++ and fail to link there. Nesting the guard is harmless on
// MSVC, whose headers already carry one.
extern "C" {
#include <hidsdi.h>
#include <hidpi.h>
}

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

namespace sds {
namespace {

constexpr uint16_t kSonyVid      = 0x054C;
constexpr uint16_t kDualSensePid = 0x0CE6;

// USB output report 0x02 is 48 bytes including the report id (the length the working shim
// wrote). Bluetooth uses report 0x31 with a CRC-32 trailer and is NOT handled here; its caps
// report a different length, which is logged so the mismatch is visible.
constexpr uint8_t  kOutputReportId     = 0x02;
constexpr uint32_t kUsbOutputReportLen = 48;

constexpr uint64_t kOpenRetryMs = 1000;

// Open the first HID interface whose attributes say DualSense. Returns INVALID_HANDLE_VALUE
// when none does; `interfacesSeen` says how many HID interfaces were enumerated, which
// distinguishes "no HID at all in this prefix" from "HID present but no DualSense".
HANDLE OpenDualSense(std::string& outPath, uint32_t& outReportLen, unsigned& interfacesSeen)
{
    interfacesSeen = 0;
    GUID hidGuid;
    ::HidD_GetHidGuid(&hidGuid);

    HDEVINFO set = ::SetupDiGetClassDevsW(&hidGuid, nullptr, nullptr,
                                          DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE)
        return INVALID_HANDLE_VALUE;

    HANDLE found = INVALID_HANDLE_VALUE;
    SP_DEVICE_INTERFACE_DATA ifd{};
    ifd.cbSize = sizeof(ifd);
    for (DWORD i = 0; found == INVALID_HANDLE_VALUE &&
                      ::SetupDiEnumDeviceInterfaces(set, nullptr, &hidGuid, i, &ifd); ++i)
    {
        ++interfacesSeen;
        DWORD need = 0;
        ::SetupDiGetDeviceInterfaceDetailW(set, &ifd, nullptr, 0, &need, nullptr);
        if (need == 0)
            continue;
        std::vector<uint8_t> raw(need);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(raw.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!::SetupDiGetDeviceInterfaceDetailW(set, &ifd, detail, need, nullptr, nullptr))
            continue;

        HANDLE h = ::CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0,
                                 nullptr);
        if (h == INVALID_HANDLE_VALUE)
            continue;

        HIDD_ATTRIBUTES attr{};
        attr.Size = sizeof(attr);
        if (::HidD_GetAttributes(h, &attr) && attr.VendorID == kSonyVid &&
            attr.ProductID == kDualSensePid)
        {
            outPath      = Narrow(detail->DevicePath);
            outReportLen = 0;
            PHIDP_PREPARSED_DATA pre = nullptr;
            if (::HidD_GetPreparsedData(h, &pre) && pre != nullptr)
            {
                HIDP_CAPS caps{};
                if (::HidP_GetCaps(pre, &caps) == HIDP_STATUS_SUCCESS)
                    outReportLen = caps.OutputReportByteLength;
                ::HidD_FreePreparsedData(pre);
            }
            found = h;
        }
        else
        {
            ::CloseHandle(h);
        }
    }
    ::SetupDiDestroyDeviceInfoList(set);
    return found;
}

} // namespace

HidMode::~HidMode()
{
    Shutdown();
}

void HidMode::Start(const Config& config)
{
    m_config = &config;
    m_running.store(true, std::memory_order_release);
    m_worker = std::thread(&HidMode::WorkerMain, this);
    SDS_LOG_INFO("hidmode: worker started; valid_flag0=0x%02X will be re-asserted every "
                 "%.1f s and before each waveform",
                 static_cast<unsigned>(config.hapticValidFlag0),
                 static_cast<double>(config.hapticReassertSeconds));
}

void HidMode::Shutdown()
{
    if (!m_running.exchange(false, std::memory_order_acq_rel))
        return;
    if (m_worker.joinable())
        m_worker.join();

    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_handle != nullptr && m_handle != INVALID_HANDLE_VALUE)
    {
        // Leave the pad as the game expects it: emulation on, as libScePad assumes, and —
        // only if we ever claimed the audio routing — its own default routing back.
        const PadAudioClaim restore = m_audioClaimActive.load(std::memory_order_relaxed)
                                          ? MutePadAudioClaim()
                                          : PadAudioClaim{};
        WriteLocked(kValidFlag0Rumble, restore,
                    "shutdown: handing the coils back to rumble emulation");
        CloseLocked();
    }
}

void HidMode::CloseLocked()
{
    if (m_handle != nullptr && m_handle != INVALID_HANDLE_VALUE)
        ::CloseHandle(m_handle);
    m_handle = nullptr;
    m_opened.store(false, std::memory_order_relaxed);
}

bool HidMode::EnsureOpenLocked()
{
    if (m_handle != nullptr && m_handle != INVALID_HANDLE_VALUE)
        return true;

    const uint64_t now = NowMs();
    if (m_lastOpenAttemptMs != 0 && now - m_lastOpenAttemptMs < kOpenRetryMs)
        return false;
    m_lastOpenAttemptMs = now;

    unsigned seen = 0;
    HANDLE   h    = OpenDualSense(m_devicePath, m_reportLength, seen);
    if (h == INVALID_HANDLE_VALUE)
    {
        m_handle = nullptr;
        // Loud once, then every ~30 s, not every second.
        if (m_openFailuresLogged == 0 || m_openFailuresLogged % 30 == 0)
            SDS_LOG_WARN("hidmode: no DualSense HID interface (VID 054C PID 0CE6) among %u HID "
                         "interface(s). The coils stay in rumble emulation and waveforms will "
                         "be INAUDIBLE. Check that Steam's *global* PlayStation Controller "
                         "Support is OFF and the pad is a HID device in the prefix.", seen);
        ++m_openFailuresLogged;
        return false;
    }

    m_handle = h;
    m_openFailuresLogged = 0;
    m_opened.store(true, std::memory_order_relaxed);
    SDS_LOG_INFO("hidmode: opened %s (OutputReportByteLength=%u, USB report 0x02 is %u)",
                 m_devicePath.c_str(), m_reportLength, kUsbOutputReportLen);
    if (m_reportLength != 0 && m_reportLength != kUsbOutputReportLen)
        SDS_LOG_WARN("hidmode: output report length %u differs from the USB length %u. A "
                     "Bluetooth pad uses report 0x31 with a CRC, which this plugin does NOT "
                     "write; expect the mode write to be ignored.", m_reportLength,
                     kUsbOutputReportLen);
    return true;
}

bool HidMode::WriteLocked(uint8_t flag0, const PadAudioClaim& claim, const char* why)
{
    if (!EnsureOpenLocked())
        return false;

    // WriteFile on a HID handle wants exactly the device's output report length. Use the
    // caps value when we have it and the measured USB length otherwise. A claim needs the
    // report to reach `audio_control2` at offset 38, so it also sets a floor — a report too
    // short for the claim would silently drop the preamp byte.
    const uint32_t floor = claim.claims ? static_cast<uint32_t>(kReportMinLenForAudio) : 3u;
    const uint32_t len =
        std::max<uint32_t>(m_reportLength != 0 ? m_reportLength : kUsbOutputReportLen, floor);
    std::vector<uint8_t> report(len, 0);
    report[0] = kOutputReportId;
    // valid_flag0: the coil mode, plus the audio claim's bits 5/7 only. ComposeValidFlag0
    // masks bits 0..3 out of the claim, so nothing here can disturb the coils or triggers.
    report[kReportOffValidFlag0] = ComposeValidFlag0(flag0, claim);
    report[kReportOffValidFlag1] = claim.flag1;   // 0 unless the claim wants audio_control2
    if (claim.claims && len > kReportOffAudioControl2)
    {
        report[kReportOffSpeakerVolume] = claim.speakerVolume;
        report[kReportOffAudioControl]  = claim.audioControl;
        report[kReportOffAudioControl2] = claim.audioControl2;
    }

    DWORD wrote = 0;
    if (!::WriteFile(m_handle, report.data(), len, &wrote, nullptr))
    {
        const DWORD err = ::GetLastError();
        m_failures.fetch_add(1, std::memory_order_relaxed);
        SDS_LOG_ERROR("hidmode: WriteFile(valid_flag0=0x%02X, %u bytes) failed err=%lu (%s); "
                      "closing and reopening on the next attempt",
                      static_cast<unsigned>(report[kReportOffValidFlag0]), len,
                      static_cast<unsigned long>(err), why);
        CloseLocked();
        return false;
    }

    // The AUDIO half of the line is printed only when a claim is live, so an ordinary
    // session's log is unchanged and a claiming session says exactly which bytes went out —
    // a wrong claim must be readable from the log, never inferred from a silent speaker.
    const unsigned long n = m_writes.fetch_add(1, std::memory_order_relaxed) + 1;
    const int claimPath = static_cast<int>((claim.audioControl >> 4) & 0x3);
    // 256, not 160: PadAudioPathName's longest row is 69 characters and the surrounding text is
    // ~95, so 160 silently truncated the very line whose whole job is to make a wrong claim
    // readable. SDS_LOG_* takes no format attribute, so nothing would have warned.
    char audio[256] = "";
    if (claim.claims)
        std::snprintf(audio, sizeof(audio),
                      " | AUDIO CLAIM flag1=0x%02X audio_control=0x%02X (path %d: %s) "
                      "speaker_volume=0x%02X audio_control2=0x%02X",
                      static_cast<unsigned>(claim.flag1),
                      static_cast<unsigned>(claim.audioControl), claimPath,
                      PadAudioPathName(claimPath),
                      static_cast<unsigned>(claim.speakerVolume),
                      static_cast<unsigned>(claim.audioControl2));
    if (n <= 3 || n % 300 == 0)
        SDS_LOG_INFO("hidmode: wrote valid_flag0=0x%02X (coil base 0x%02X, %lu bytes) [#%lu] "
                     "%s%s",
                     static_cast<unsigned>(report[kReportOffValidFlag0]),
                     static_cast<unsigned>(flag0), static_cast<unsigned long>(wrote), n, why,
                     audio);
    else
        SDS_LOG_DEBUG("hidmode: wrote valid_flag0=0x%02X [#%lu] %s%s",
                      static_cast<unsigned>(report[kReportOffValidFlag0]), n, why, audio);
    return true;
}

void HidMode::SetAudioClaim(const PadAudioClaim& claim)
{
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        changed = claim.claims != m_audioClaim.claims || claim.flag0 != m_audioClaim.flag0 ||
                  claim.flag1 != m_audioClaim.flag1 ||
                  claim.speakerVolume != m_audioClaim.speakerVolume ||
                  claim.audioControl != m_audioClaim.audioControl ||
                  claim.audioControl2 != m_audioClaim.audioControl2;
        m_audioClaim = claim;
    }
    m_audioClaimActive.store(claim.claims, std::memory_order_relaxed);
    if (!changed)
        return;
    if (claim.claims)
        SDS_LOG_WARN("hidmode: pad-audio claim ENABLED - every report from now on also claims "
                     "valid_flag0 0x%02X (SPEAKER_VOLUME|AUDIO_CONTROL) and selects path %d. "
                     "The coil and trigger bits 0..3 are masked out of it, so the waveform "
                     "path is unchanged; if the coils stop working, set PadSpeakerRoute=off.",
                     static_cast<unsigned>(claim.flag0),
                     static_cast<int>((claim.audioControl >> 4) & 0x3));
    else
        SDS_LOG_INFO("hidmode: pad-audio claim disabled; reports are byte-identical to the "
                     "coil-only shape again.");
    AssertNow("pad-audio claim changed");
}

void HidMode::AssertNow(const char* why)
{
    if (m_config == nullptr)
        return;
    std::lock_guard<std::mutex> lock(m_mutex);
    // A live audio claim is reason enough to write even with Haptics=0. The coil base stays
    // exactly what the config asks for either way, so this cannot change the coils' mode.
    if (!m_config->haptics && !m_audioClaim.claims)
        return;
    WriteLocked(static_cast<uint8_t>(m_config->hapticValidFlag0), m_audioClaim, why);
}

void HidMode::WorkerMain()
{
    // A short grace so the game's own pad bring-up is not raced at startup; the shim waited
    // 3 s before its first write and that worked.
    for (int i = 0; i < 30 && m_running.load(std::memory_order_acquire); ++i)
        ::Sleep(100);

    while (m_running.load(std::memory_order_acquire))
    {
        if (m_config != nullptr)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_config->haptics || m_audioClaim.claims)
                WriteLocked(static_cast<uint8_t>(m_config->hapticValidFlag0), m_audioClaim,
                            "periodic re-assert");
        }
        const float    seconds = m_config != nullptr ? m_config->hapticReassertSeconds : 2.0f;
        const uint64_t waitMs  = static_cast<uint64_t>(std::clamp(seconds, 0.25f, 60.0f) * 1000.0f);
        for (uint64_t slept = 0; slept < waitMs && m_running.load(std::memory_order_acquire);
             slept += 100)
            ::Sleep(100);
    }
    SDS_LOG_INFO("hidmode: worker exiting (writes=%lu failures=%lu)", Writes(), Failures());
}

} // namespace sds
