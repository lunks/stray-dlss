#include "ScePad.hpp"

#include "Log.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <cstring>

namespace sds {
namespace {

// Precise signatures rather than the old shim's generic 4x uint64 thunk. The shim's
// `long long` return over an int32 API left the upper half undefined; it only worked because
// every test was `== 0`.
using PFN_scePadGetHandle              = int32_t(__cdecl*)(int32_t userId, int32_t type, int32_t index);
using PFN_scePadSetTriggerEffect       = int32_t(__cdecl*)(int32_t handle, const void* param);
using PFN_scePadSetVibration           = int32_t(__cdecl*)(int32_t handle, const void* param);
using PFN_scePadGetControllerInformation = int32_t(__cdecl*)(int32_t handle, void* info);
using PFN_scePadGetTriggerEffectState  = int32_t(__cdecl*)(int32_t handle, void* state);

HMODULE                              g_lib = nullptr;
PFN_scePadGetHandle                  g_getHandle = nullptr;
PFN_scePadSetTriggerEffect           g_setTriggerEffect = nullptr;
PFN_scePadSetVibration               g_setVibration = nullptr;
PFN_scePadGetControllerInformation   g_getInfo = nullptr;
PFN_scePadGetTriggerEffectState      g_getTriggerState = nullptr;

void HexDump(const uint8_t* p, size_t n, char* out, size_t outSize)
{
    size_t w = 0;
    for (size_t i = 0; i < n && w + 3 < outSize; ++i)
    {
        const int got = std::snprintf(out + w, outSize - w, "%02x", p[i]);
        if (got <= 0) break;
        w += static_cast<size_t>(got);
    }
    if (w < outSize) out[w] = '\0';
}

} // namespace

bool ScePad::Bind()
{
    if (m_bound.load(std::memory_order_acquire))
        return true;

    // The game maps it; we never LoadLibrary it ourselves. If it is absent, the game has not
    // reached its delay-load site yet (or Steam Input is holding the pad, §4) — poll, do not
    // force it, because loading a second copy would create a second device table.
    if (g_lib == nullptr)
        g_lib = ::GetModuleHandleW(L"libScePad.dll");
    if (g_lib == nullptr)
        return false;

    g_getHandle        = reinterpret_cast<PFN_scePadGetHandle>(
                             reinterpret_cast<void*>(::GetProcAddress(g_lib, "scePadGetHandle")));
    g_setTriggerEffect = reinterpret_cast<PFN_scePadSetTriggerEffect>(
                             reinterpret_cast<void*>(::GetProcAddress(g_lib, "scePadSetTriggerEffect")));
    g_setVibration     = reinterpret_cast<PFN_scePadSetVibration>(
                             reinterpret_cast<void*>(::GetProcAddress(g_lib, "scePadSetVibration")));
    g_getInfo          = reinterpret_cast<PFN_scePadGetControllerInformation>(
                             reinterpret_cast<void*>(::GetProcAddress(g_lib, "scePadGetControllerInformation")));
    g_getTriggerState  = reinterpret_cast<PFN_scePadGetTriggerEffectState>(
                             reinterpret_cast<void*>(::GetProcAddress(g_lib, "scePadGetTriggerEffectState")));

    wchar_t path[MAX_PATH]{};
    ::GetModuleFileNameW(g_lib, path, MAX_PATH);
    char narrow[MAX_PATH]{};
    ::WideCharToMultiByte(CP_UTF8, 0, path, -1, narrow, MAX_PATH, nullptr, nullptr);

    SDS_LOG_INFO("libScePad.dll found at %s", narrow);
    SDS_LOG_INFO("  scePadGetHandle=%p setTriggerEffect=%p setVibration=%p "
                 "getControllerInformation=%p getTriggerEffectState=%p",
                 reinterpret_cast<void*>(g_getHandle),
                 reinterpret_cast<void*>(g_setTriggerEffect),
                 reinterpret_cast<void*>(g_setVibration),
                 reinterpret_cast<void*>(g_getInfo),
                 reinterpret_cast<void*>(g_getTriggerState));

    // GetHandle and GetControllerInformation are required: without both we cannot tell an
    // occupied slot from an empty one, and picking blindly is exactly the bug being fixed.
    if (g_getHandle == nullptr || g_getInfo == nullptr)
    {
        SDS_LOG_ERROR("libScePad.dll is mapped but scePadGetHandle/"
                      "scePadGetControllerInformation did not resolve. Refusing to guess a pad.");
        return false;
    }

    m_bound.store(true, std::memory_order_release);
    return true;
}

bool ScePad::GetControllerInformation(int32_t handle, PadInfo& out) const
{
    out = PadInfo{};
    if (g_getInfo == nullptr || handle <= 0)
        return false;

    // Sony's struct is well under 64 bytes; the extra is slack so a longer struct on some
    // library revision cannot smash our stack.
    out.result = g_getInfo(handle, out.raw);

    std::memcpy(&out.pixelDensity,   out.raw + 0x00, sizeof(float));
    std::memcpy(&out.touchpadWidth,  out.raw + 0x04, sizeof(uint16_t));
    std::memcpy(&out.touchpadHeight, out.raw + 0x06, sizeof(uint16_t));
    out.deadZoneLeft   = out.raw[0x08];
    out.deadZoneRight  = out.raw[0x09];
    out.connectionType = out.raw[0x0a];
    out.connectedCount = out.raw[0x0b];
    out.connected      = out.raw[0x0c];
    out.deviceClass    = out.raw[0x0d];
    return true;
}

bool ScePad::SelectPad(int forceUserId)
{
    if (!Bind())
        return false;

    if (forceUserId > 0)
    {
        const int32_t h = g_getHandle(forceUserId, 0, 0);
        SDS_LOG_WARN("PadUserId=%d forced: scePadGetHandle -> 0x%X. The connected-byte probe "
                     "is being SKIPPED, so a wrong slot here is silent.", forceUserId,
                     static_cast<unsigned>(h));
        if (h > 0)
        {
            m_handle.store(h, std::memory_order_release);
            m_userId.store(forceUserId, std::memory_order_release);
            return true;
        }
        SDS_LOG_ERROR("forced user slot %d returned no handle", forceUserId);
        return false;
    }

    int     bestUser   = 0;
    int32_t bestHandle = 0;

    // MEASURED: the game opens slots 1..4 and every one answers with a positive handle.
    for (int user = 1; user <= 4; ++user)
    {
        const int32_t h = g_getHandle(user, 0, 0);
        if (h <= 0)
        {
            SDS_LOG_INFO("pad slot %d: scePadGetHandle -> 0x%X (no handle)", user,
                         static_cast<unsigned>(h));
            continue;
        }

        PadInfo info;
        GetControllerInformation(h, info);

        char hex[160];
        HexDump(info.raw, 16, hex, sizeof(hex));
        SDS_LOG_INFO("pad slot %d: handle=0x%X info-ret=0x%08X connected=%u connectedCount=%u "
                     "type=%u class=%u density=%.2f touch=%ux%u dz=%u/%u raw=%s",
                     user, static_cast<unsigned>(h), static_cast<unsigned>(info.result),
                     info.connected,
                     info.connectedCount, info.connectionType, info.deviceClass,
                     static_cast<double>(info.pixelDensity), info.touchpadWidth,
                     info.touchpadHeight, info.deadZoneLeft, info.deadZoneRight, hex);

        // THE discriminator. Not the return code — empty slots return 0 too.
        if (info.connected != 0 && bestHandle == 0)
        {
            bestHandle = h;
            bestUser   = user;
        }
    }

    if (bestHandle == 0)
    {
        // Loud, and specifically about the thing that is most likely wrong: Steam's global
        // PlayStation Controller Support hides the pad from libScePad (§4/§7).
        SDS_LOG_ERROR("no pad slot reports connected=1. Every slot handed back a handle and "
                      "an all-zero information struct. Check that Steam's *global* "
                      "PlayStation Controller Support is OFF and that the DualSense is "
                      "visible to the prefix as a HID device.");
        m_handle.store(0, std::memory_order_release);
        m_userId.store(0, std::memory_order_release);
        return false;
    }

    SDS_LOG_INFO("adopted pad: user slot %d, handle 0x%X (connected byte set)",
                 bestUser, static_cast<unsigned>(bestHandle));
    m_handle.store(bestHandle, std::memory_order_release);
    m_userId.store(bestUser, std::memory_order_release);
    return true;
}

bool ScePad::RefreshIfLost()
{
    const int32_t h = Handle();
    if (h > 0)
    {
        PadInfo info;
        if (GetControllerInformation(h, info) && info.connected != 0)
            return true;
        SDS_LOG_WARN("pad handle 0x%X stopped reporting connected; re-probing all slots",
                     static_cast<unsigned>(h));
        m_handle.store(0, std::memory_order_release);
    }
    return SelectPad(0);
}

bool ScePad::SetTriggers(bool left, bool right, uint8_t position, uint8_t strength)
{
    const int32_t h = Handle();
    if (g_setTriggerEffect == nullptr || h <= 0)
        return false;

    // ScePadTriggerEffectParam, from the game's own construction site (§3):
    //   +0x00 triggerMask   +0x08 cmd0.mode   +0x10 cmd0 data
    //                       +0x40 cmd1.mode   +0x48 cmd1 data     (command stride 0x38)
    // Four other strides were rejected with 0x80920001 INVALID_ARG; this one is accepted.
    // The block is ~120 bytes; 256 zeroed is slack.
    alignas(16) uint8_t param[256]{};

    // BOTH triggers stay addressed. Dropping a side from the mask leaves it stuck at whatever
    // the previous call set — that is the §8 trap.
    param[0x00] = 0x03;

    const uint32_t modeL = left  ? static_cast<uint32_t>(TriggerMode::Feedback)
                                 : static_cast<uint32_t>(TriggerMode::Off);
    const uint32_t modeR = right ? static_cast<uint32_t>(TriggerMode::Feedback)
                                 : static_cast<uint32_t>(TriggerMode::Off);
    std::memcpy(param + 0x08, &modeL, sizeof(modeL));
    param[0x10] = position;
    param[0x11] = strength;
    std::memcpy(param + 0x40, &modeR, sizeof(modeR));
    param[0x48] = position;
    param[0x49] = strength;

    const int32_t r = g_setTriggerEffect(h, param);
    if (r == 0) m_trigOk.fetch_add(1, std::memory_order_relaxed);
    else        m_trigFail.fetch_add(1, std::memory_order_relaxed);

    SDS_LOG_INFO("TRIGGERS L=%d R=%d pos=%u str=%u handle=0x%X -> 0x%08X (ok=%lu fail=%lu)",
                 left ? 1 : 0, right ? 1 : 0, position, strength, static_cast<unsigned>(h),
                 static_cast<unsigned>(r), TriggerOk(), TriggerFail());
    return r == 0;
}

bool ScePad::SetVibration(uint8_t large, uint8_t small)
{
    const int32_t h = Handle();
    if (g_setVibration == nullptr || h <= 0)
        return false;
    const uint8_t param[2] = { large, small };
    const int32_t r = g_setVibration(h, param);
    if (r != 0) m_vibeFail.fetch_add(1, std::memory_order_relaxed);
    return r == 0;
}

bool ScePad::GetTriggerState(uint32_t& outLeft, uint32_t& outRight)
{
    outLeft = outRight = 0;
    const int32_t h = Handle();
    if (g_getTriggerState == nullptr || h <= 0)
        return false;
    uint32_t st[8]{};
    const int32_t r = g_getTriggerState(h, st);
    outLeft  = st[0];
    outRight = st[1];
    return r == 0;
}

std::wstring GameBinariesDir()
{
    wchar_t path[MAX_PATH]{};
    const DWORD n = ::GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
        return {};
    std::wstring s(path, n);
    const size_t slash = s.find_last_of(L"\\/");
    if (slash == std::wstring::npos)
        return {};
    return s.substr(0, slash + 1);
}

std::wstring ModuleDir(void* addressInsideThisModule)
{
    HMODULE self = nullptr;
    if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                  GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              reinterpret_cast<LPCWSTR>(addressInsideThisModule), &self))
        return {};
    wchar_t path[MAX_PATH]{};
    const DWORD n = ::GetModuleFileNameW(self, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
        return {};
    std::wstring s(path, n);
    const size_t slash = s.find_last_of(L"\\/");
    if (slash == std::wstring::npos)
        return {};
    return s.substr(0, slash + 1);
}

} // namespace sds
