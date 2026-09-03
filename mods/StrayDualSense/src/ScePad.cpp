#include "ScePad.hpp"

#include "Log.hpp"
#include "Platform.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <cstring>

namespace sds {
namespace {

// Precise signatures rather than the shim's generic 4x uint64 thunk. The shim's `long long`
// return over an int32 API left the upper half undefined; it only worked because every test
// was `== 0`.
using PFN_scePadGetHandle                = int32_t(__cdecl*)(int32_t userId, int32_t type, int32_t index);
using PFN_scePadSetTriggerEffect         = int32_t(__cdecl*)(int32_t handle, const void* param);
using PFN_scePadGetControllerInformation = int32_t(__cdecl*)(int32_t handle, void* info);
using PFN_scePadGetTriggerEffectState    = int32_t(__cdecl*)(int32_t handle, void* state);
// The three audio entry points the retired shim called (PadAudio.hpp). The shim invoked them
// through a generic 4x uint64 thunk; these are the narrow signatures its own comments imply:
// the path is taken BY VALUE (libScePad_shim.c:489) and the gain BY POINTER (:495-497).
using PFN_scePadIsSupportedAudioFunction = int32_t(__cdecl*)(int32_t handle);
using PFN_scePadSetAudioOutPath          = int32_t(__cdecl*)(int32_t handle, int32_t path);
using PFN_scePadSetVolumeGain            = int32_t(__cdecl*)(int32_t handle, const void* gain);

HMODULE                            g_lib              = nullptr;
PFN_scePadGetHandle                g_getHandle        = nullptr;
PFN_scePadSetTriggerEffect         g_setTriggerEffect = nullptr;
PFN_scePadGetControllerInformation g_getInfo          = nullptr;
PFN_scePadGetTriggerEffectState    g_getTriggerState  = nullptr;
PFN_scePadIsSupportedAudioFunction g_audioSupported   = nullptr;
PFN_scePadSetAudioOutPath          g_setAudioOutPath  = nullptr;
PFN_scePadSetVolumeGain            g_setVolumeGain    = nullptr;

template <typename Fn>
Fn Resolve(const char* name)
{
    return reinterpret_cast<Fn>(reinterpret_cast<void*>(::GetProcAddress(g_lib, name)));
}

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

    g_getHandle        = Resolve<PFN_scePadGetHandle>("scePadGetHandle");
    g_setTriggerEffect = Resolve<PFN_scePadSetTriggerEffect>("scePadSetTriggerEffect");
    g_getInfo          = Resolve<PFN_scePadGetControllerInformation>("scePadGetControllerInformation");
    g_getTriggerState  = Resolve<PFN_scePadGetTriggerEffectState>("scePadGetTriggerEffectState");
    g_audioSupported   = Resolve<PFN_scePadIsSupportedAudioFunction>("scePadIsSupportedAudioFunction");
    g_setAudioOutPath  = Resolve<PFN_scePadSetAudioOutPath>("scePadSetAudioOutPath");
    g_setVolumeGain    = Resolve<PFN_scePadSetVolumeGain>("scePadSetVolumeGain");

    wchar_t path[MAX_PATH]{};
    ::GetModuleFileNameW(g_lib, path, MAX_PATH);
    SDS_LOG_INFO("libScePad.dll found at %s", Narrow(path).c_str());
    SDS_LOG_INFO("  scePadGetHandle=%p setTriggerEffect=%p getControllerInformation=%p "
                 "getTriggerEffectState=%p",
                 reinterpret_cast<void*>(g_getHandle), reinterpret_cast<void*>(g_setTriggerEffect),
                 reinterpret_cast<void*>(g_getInfo), reinterpret_cast<void*>(g_getTriggerState));
    // The audio trio is what routes sound to the pad's internal speaker. §1 measured that the
    // GAME resolves none of them, so their presence here is purely our doing — and a build
    // where they are absent must say so once, loudly, rather than leave a silent speaker
    // looking like a playback bug.
    SDS_LOG_INFO("  scePadIsSupportedAudioFunction=%p setAudioOutPath=%p setVolumeGain=%p",
                 reinterpret_cast<void*>(g_audioSupported),
                 reinterpret_cast<void*>(g_setAudioOutPath),
                 reinterpret_cast<void*>(g_setVolumeGain));
    m_audioApi.store(g_setAudioOutPath != nullptr, std::memory_order_release);
    if (g_setAudioOutPath == nullptr)
        SDS_LOG_WARN("libScePad.dll exports no scePadSetAudioOutPath. The pad's internal "
                     "speaker cannot be selected through Sony's API in this build; set "
                     "PadSpeakerRoute=hid to use the raw output-report claim instead.");

    // GetHandle and GetControllerInformation are required: without both we cannot tell an
    // occupied slot from an empty one, and picking blindly is exactly the bug being avoided.
    if (g_getHandle == nullptr || g_getInfo == nullptr)
    {
        SDS_LOG_ERROR("libScePad.dll is mapped but scePadGetHandle/"
                      "scePadGetControllerInformation did not resolve. Refusing to guess a pad.");
        return false;
    }

    m_bound.store(true, std::memory_order_release);
    return true;
}

// Restores exactly what tools/dualsense/libScePad_shim.c's `audio_probe` did, in its order,
// with its values. Nothing here writes a HID byte: Sony's implementation owns the report
// layout for this pad and this firmware.
//
// THE COILS CANNOT BE AFFECTED BY THIS FUNCTION. It touches only the audio routing and the
// audio levels; the coils are driven as PCM on the endpoint's RL/RR (AudioPlayer) and their
// mode comes from HidMode's own output report, which this never writes. The one path from
// here to the coils would be libScePad writing a report of its own as a side effect — which
// it does for every trigger call already, and which HidMode's periodic re-assert exists to
// undo (§12).
bool ScePad::ApplyAudioRoute(int outPath, const uint8_t gainBlock[8], AudioRouteResult& out)
{
    out = AudioRouteResult{};
    const int32_t handle = Handle();
    if (!IsBound() || handle <= 0)
        return false;

    out.apiPresent = (g_setAudioOutPath != nullptr);

    // The capability probe runs FIRST and gates NOTHING. §7 measured it returning 0x80920007
    // ("this pad has no audio") on a prefix with no USB audio siblings, and the point of
    // logging it is to separate "this build cannot do pad audio" from "we asked wrong" — not
    // to decide for us. If it says no and the path call then succeeds, the path call wins.
    if (g_audioSupported != nullptr)
    {
        out.supportedRan = true;
        out.supported    = g_audioSupported(handle);
    }

    if (g_setAudioOutPath == nullptr)
    {
        m_audioFailures.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    out.pathRan = true;
    out.path    = g_setAudioOutPath(handle, static_cast<int32_t>(outPath));

    if (g_setVolumeGain != nullptr && gainBlock != nullptr)
    {
        out.gainRan = true;
        out.gain    = g_setVolumeGain(handle, gainBlock);
    }

    // Sony returns 0 for success and 0x809200xx for everything else (§3, §5, §7). Treat only
    // an exact zero as success: a bitmask read would call 0x80920007 a partial win.
    const bool ok = (out.path == 0);
    if (ok)
        m_audioApplies.fetch_add(1, std::memory_order_relaxed);
    else
        m_audioFailures.fetch_add(1, std::memory_order_relaxed);
    return ok;
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

    // Throttle the per-slot dump: this is retried until a pad appears, and at 5 lines a probe
    // it would bury the rest of the log within a minute. The first probe and every 15th carry
    // the full detail; the rest report only the outcome.
    const unsigned long probe = m_probes.fetch_add(1, std::memory_order_relaxed) + 1;
    const bool verbose = (probe == 1 || probe % 15 == 0);

    // MEASURED: the game opens slots 1..4 and every one answers with a positive handle.
    for (int user = 1; user <= 4; ++user)
    {
        const int32_t h = g_getHandle(user, 0, 0);
        if (h <= 0)
        {
            if (verbose)
                SDS_LOG_INFO("pad slot %d: scePadGetHandle -> 0x%X (no handle)", user,
                             static_cast<unsigned>(h));
            continue;
        }

        PadInfo info;
        GetControllerInformation(h, info);

        char hex[160];
        HexDump(info.raw, 16, hex, sizeof(hex));
        if (verbose)
        SDS_LOG_INFO("pad slot %d: handle=0x%X info-ret=0x%08X connected=%u connectedCount=%u "
                     "type=%u class=%u density=%.2f touch=%ux%u dz=%u/%u raw=%s",
                     user, static_cast<unsigned>(h), static_cast<unsigned>(info.result),
                     info.connected, info.connectedCount, info.connectionType, info.deviceClass,
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
        const unsigned long misses = m_probeMisses.fetch_add(1, std::memory_order_relaxed) + 1;
        // The first miss is not a failure: libScePad only knows about a pad the GAME has
        // opened, and our thread starts as soon as the module maps. It becomes a failure only
        // if it keeps missing, so the message escalates rather than crying wolf on probe 1.
        if (misses == 1)
            SDS_LOG_INFO("pad: no slot reports connected=1 on probe %lu. This is EXPECTED "
                         "early - libScePad only knows about a pad the game has opened. "
                         "Retrying.", probe);
        else if (verbose)
            SDS_LOG_ERROR("pad: still no slot reporting connected=1 after %lu probe(s). Every "
                          "slot hands back a handle and an all-zero information struct. Check "
                          "that Steam's *global* PlayStation Controller Support is OFF and "
                          "that the DualSense is visible to the prefix as a HID device.",
                          misses);
        m_handle.store(0, std::memory_order_release);
        m_userId.store(0, std::memory_order_release);
        return false;
    }

    SDS_LOG_INFO("adopted pad: user slot %d, handle 0x%X (connected byte set) on probe %lu",
                 bestUser, static_cast<unsigned>(bestHandle), probe);
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

bool ScePad::SetTriggers(const uint8_t param[kTriggerParamSize], bool left, bool right,
                         const TriggerEffect& effect)
{
    const int32_t h = Handle();
    if (g_setTriggerEffect == nullptr || h <= 0)
        return false;

    const int32_t r = g_setTriggerEffect(h, param);
    if (r == 0) m_trigOk.fetch_add(1, std::memory_order_relaxed);
    else        m_trigFail.fetch_add(1, std::memory_order_relaxed);

    const SonyTriggerMode sony = ToSonyMode(effect.mode);
    SDS_LOG_INFO("TRIGGERS %s L2=%d R2=%d game=%d(%s) sony=%u(%s) v=%u/%u/%u handle=0x%X "
                 "-> 0x%08X (ok=%lu fail=%lu)",
                 (left || right) ? "ENGAGE " : "release", left ? 1 : 0, right ? 1 : 0,
                 effect.mode, GameModeName(effect.mode), static_cast<unsigned>(sony),
                 SonyModeName(sony), effect.value1, effect.value2, effect.value3,
                 static_cast<unsigned>(h), static_cast<unsigned>(r), TriggerOk(), TriggerFail());
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

} // namespace sds
