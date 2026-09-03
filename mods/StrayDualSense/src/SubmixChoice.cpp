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
        // FAudioDevice::SampleRate is the second data member, so on a stock layout it lands at
        // +0x0C. A candidate holding a standard rate a thousand bytes in is a rate table or an
        // unrelated object that happens to contain the number.
        c.rateAtHead = c.sampleRate != 0 && c.sampleRateAt <= kSampleRateOffset + kSampleRateOffsetSlack;

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

    // Rungs, strongest first. Rungs 1-3 need UWorld corroboration and are DEAD on a build
    // where the world uses the main audio device (MEASURED 2026-09-03) — they are kept because
    // they cost nothing and are the best evidence when they do exist, but nothing may depend
    // on them firing.

    // 1. A UWorld handle whose weak pointer names that very world. Exact and unforgeable when
    //    the world has its own device at all.
    for (std::size_t i = 0; i < candidates.size(); ++i)
        if (candidates[i].reject == nullptr && candidates[i].worldIndexMatch)
        {
            pick(static_cast<int>(i),
                 "the UWorld handle's weak pointer names that very world (exact index match)");
            return out;
        }

    // 2. The sample rate sits where FAudioDevice::SampleRate must be, AND something
    //    corroborates it positionally or across objects. This is the rung that identifies the
    //    device on the measured build.
    // UEngine first: MainAudioDeviceHandle is the device the engine itself uses, and a world's
    // own device — when it has one at all — is the lesser answer for a submix.
    for (int wantSingleRate = 1; wantSingleRate >= 0; --wantSingleRate)
        for (int engineFirst = 1; engineFirst >= 0; --engineFirst)
            for (std::size_t i = 0; i < candidates.size(); ++i)
            {
                const DeviceCandidate& c = candidates[i];
                if (c.reject != nullptr || !c.rateAtHead)
                    continue;
                if (engineFirst && c.fromWorld)
                    continue;
                if (wantSingleRate && c.distinctRates > 1)
                    continue;      // several DIFFERENT rates is a table, not a device
                if (!c.managerBefore && !c.vtableShared)
                    continue;
                pick(static_cast<int>(i),
                     "its sample rate sits where FAudioDevice::SampleRate must be (+0x0C), "
                     "corroborated by the manager before it or a shared vtable");
                return out;
            }

    // 3. The head-offset rate alone, when it is the ONLY candidate with one. On the measured
    //    build eleven candidates hold a standard rate and exactly one holds it at the head, so
    //    this is decisive without needing a UWorld that has no device.
    for (int wantSingleRate = 1; wantSingleRate >= 0; --wantSingleRate)
    {
        int found = 0, which = -1;
        for (std::size_t i = 0; i < candidates.size(); ++i)
        {
            const DeviceCandidate& c = candidates[i];
            if (c.reject == nullptr && c.rateAtHead && !(wantSingleRate && c.distinctRates > 1))
            {
                ++found;
                which = static_cast<int>(i);
            }
        }
        if (found == 1)
        {
            pick(which,
                 wantSingleRate
                     ? "it is the ONLY candidate whose sample rate sits at FAudioDevice's own "
                       "+0x0C and holds just one distinct rate"
                     : "it is the ONLY candidate whose sample rate sits at FAudioDevice's own "
                       "+0x0C");
            return out;
        }
        if (found > 1)
            break;   // ambiguous at this strictness; loosening cannot help
    }

    // 4. UEngine::MainAudioDeviceHandle, corroborated twice.
    for (std::size_t i = 0; i < candidates.size(); ++i)
        if (candidates[i].reject == nullptr && !candidates[i].fromWorld &&
            candidates[i].managerBefore && candidates[i].vtableShared)
        {
            pick(static_cast<int>(i),
                 "UEngine::MainAudioDeviceHandle: the manager sits before it AND its vtable is "
                 "shared with a UWorld candidate");
            return out;
        }

    // 5. A shared vtable on its own. Prefer the UEngine side — MainAudioDeviceHandle is the
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

    // 6. A UEngine handle with the manager immediately before it.
    for (std::size_t i = 0; i < candidates.size(); ++i)
        if (candidates[i].reject == nullptr && !candidates[i].fromWorld && candidates[i].managerBefore)
        {
            pick(static_cast<int>(i),
                 "UEngine::MainAudioDeviceHandle: the audio device manager sits immediately "
                 "before it");
            return out;
        }

    // 7. Exactly one survivor anywhere.
    int survivors = 0, only = -1;
    for (std::size_t i = 0; i < candidates.size(); ++i)
        if (candidates[i].reject == nullptr) { ++survivors; only = static_cast<int>(i); }
    if (survivors == 1)
    {
        pick(only, "it is the ONLY candidate that survived every test");
        return out;
    }

    out.refusal = survivors > 1
        ? "several candidates survived and none carries a decisive signal (no rate at "
          "FAudioDevice's own +0x0C, no world-index match, no shared vtable, no manager "
          "before it)"
        : "no candidate survived: every one was a UObject or held no standard sample rate";
    return out;
}

} // namespace submix
} // namespace sds
