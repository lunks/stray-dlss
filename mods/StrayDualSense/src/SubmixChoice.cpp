#include "SubmixChoice.hpp"

namespace sds {
namespace submix {

bool IsStandardSampleRate(std::int32_t hz)
{
    switch (hz)
    {
    case 8000: case 11025: case 16000: case 22050: case 24000: case 32000:
    case 44100: case 48000: case 88200: case 96000: case 176400: case 192000:
        return true;
    default:
        return false;
    }
}

Choice ChooseDevice(std::vector<DeviceCandidate>& candidates, bool uobjectTestUsable)
{
    Choice out;

    // A vtable seen in BOTH objects is the real cross-check. Two FAudioDevice instances are
    // still both FMixerDevice, so their vtables match even when their pointers do not — which
    // is precisely what the old same-pointer rule assumed away.
    for (DeviceCandidate& a : candidates)
    {
        a.vtableShared = false;
        for (const DeviceCandidate& b : candidates)
            if (a.vtable == b.vtable && a.fromWorld != b.fromWorld)
            {
                a.vtableShared = true;
                break;
            }
    }

    for (DeviceCandidate& c : candidates)
    {
        c.reject = nullptr;
        if (uobjectTestUsable && c.isUObject)
            c.reject = "is a UObject (FAudioDevice is not)";
        else if (c.sampleRate == 0)
            c.reject = "holds no standard sample rate";
    }

    const auto pick = [&](int i, const char* why) {
        out.index = i;
        out.why   = why;
    };

    // 1. A UWorld handle whose weak pointer names that very world. Exact and unforgeable: the
    //    handle records the world that requested the device, so inside THAT world the stored
    //    object index must be the world's own.
    for (std::size_t i = 0; i < candidates.size(); ++i)
        if (candidates[i].reject == nullptr && candidates[i].worldIndexMatch)
        {
            pick(static_cast<int>(i),
                 "the UWorld handle's weak pointer names that very world (exact index match)");
            return out;
        }

    // 2. UEngine::MainAudioDeviceHandle, corroborated twice: the audio device manager sits
    //    immediately before it, and its vtable is shared with a UWorld candidate.
    for (std::size_t i = 0; i < candidates.size(); ++i)
        if (candidates[i].reject == nullptr && !candidates[i].fromWorld &&
            candidates[i].managerBefore && candidates[i].vtableShared)
        {
            pick(static_cast<int>(i),
                 "UEngine::MainAudioDeviceHandle: the manager sits before it AND its vtable is "
                 "shared with a UWorld candidate");
            return out;
        }

    // 3. A shared vtable on its own. Prefer the UEngine side — MainAudioDeviceHandle is the
    //    device the engine itself uses, and it is the one that matters for a submix.
    for (std::size_t i = 0; i < candidates.size(); ++i)
        if (candidates[i].reject == nullptr && !candidates[i].fromWorld && candidates[i].vtableShared)
        {
            pick(static_cast<int>(i), "its vtable appears in both UWorld and UEngine");
            return out;
        }
    for (std::size_t i = 0; i < candidates.size(); ++i)
        if (candidates[i].reject == nullptr && candidates[i].vtableShared)
        {
            pick(static_cast<int>(i), "its vtable appears in both UWorld and UEngine");
            return out;
        }

    // 4. A UEngine handle with the manager immediately before it. Positional, and enough on
    //    its own: MainAudioDeviceHandle is the answer we actually want.
    for (std::size_t i = 0; i < candidates.size(); ++i)
        if (candidates[i].reject == nullptr && !candidates[i].fromWorld && candidates[i].managerBefore)
        {
            pick(static_cast<int>(i),
                 "UEngine::MainAudioDeviceHandle: the audio device manager sits immediately "
                 "before it");
            return out;
        }

    // 5. Exactly one survivor anywhere.
    int survivors = 0, only = -1;
    for (std::size_t i = 0; i < candidates.size(); ++i)
        if (candidates[i].reject == nullptr) { ++survivors; only = static_cast<int>(i); }
    if (survivors == 1)
    {
        pick(only, "it is the ONLY candidate that survived every test");
        return out;
    }

    out.refusal = survivors > 1
        ? "several candidates survived and none carries a decisive signal (no world-index "
          "match, no shared vtable, no manager before it)"
        : "no candidate survived: every handle-shaped hit was a UObject or held no standard "
          "sample rate";
    return out;
}

} // namespace submix
} // namespace sds
