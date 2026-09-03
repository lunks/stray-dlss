// The FAudioDevice acceptance ladder — written against the DATA THAT DEFEATED THE FIRST ONE.
//
// Measured in the game 2026-09-02: 8 handle-shaped candidates in the UWorld object, 3 in the
// UEngine object, and no FAudioDevice pointer common to both, so the old "same pointer in both
// objects" rule refused and the tap never registered. The candidate list below is that run's,
// verbatim from the log, with the signals the new scan computes filled in.
//
// Two failures, not one: the shape test admitted every UObject pointer in a UWorld (they all
// carry in-image vtables with plenty of slots), and a world audio device and the main audio
// device are ALLOWED to be different instances — so identical pointers were never guaranteed.
// What is guaranteed is that both are FMixerDevice, hence a shared VTABLE.
#include "SubmixChoice.hpp"

#include <cstdio>
#include <vector>

namespace {

int g_failures = 0;

void Check(bool ok, const char* what)
{
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failures;
}

using sds::submix::DeviceCandidate;

// Distinct fake addresses. Only identity matters to the ladder.
const void* P(unsigned long long v) { return reinterpret_cast<const void*>(v); }

DeviceCandidate Noise(unsigned long long dev, unsigned long long vt, std::size_t off, bool world)
{
    // What a UObject pointer in a UWorld looks like to the scan: it passed the shape test, it
    // IS a UObject, and it holds no sample rate.
    DeviceCandidate c;
    c.device    = P(dev);
    c.vtable    = P(vt);
    c.offset    = off;
    c.fromWorld = world;
    c.isUObject = true;
    return c;
}

DeviceCandidate Device(unsigned long long dev, unsigned long long vt, std::size_t off, bool world)
{
    DeviceCandidate c;
    c.device         = P(dev);
    c.vtable         = P(vt);
    c.offset         = off;
    c.fromWorld      = world;
    c.isUObject      = false;
    c.sampleRate     = 48000;
    c.sampleRateHits = 2;      // FAudioDevice::SampleRate and PlatformSettings.SampleRate
    return c;
}

// The rule that failed on the box, reproduced so the regression can be shown to fire.
bool OldSamePointerRuleWouldAccept(const std::vector<DeviceCandidate>& v)
{
    for (const DeviceCandidate& a : v)
        for (const DeviceCandidate& b : v)
            if (a.fromWorld && !b.fromWorld && a.device == b.device)
                return true;
    return false;
}

} // namespace

int main()
{
    using sds::submix::ChooseDevice;
    using sds::submix::Choice;
    using sds::submix::IsStandardSampleRate;

    Check(IsStandardSampleRate(48000),  "48000 is a sample rate");
    Check(IsStandardSampleRate(44100),  "44100 is a sample rate");
    Check(!IsStandardSampleRate(0),     "0 is not (and zeroes are everywhere in memory)");
    Check(!IsStandardSampleRate(1),     "1 is not");
    Check(!IsStandardSampleRate(60),    "60 is not - a frame rate is not a sample rate");

    // -------------------------------------------------------------------------------
    // THE FIELD CASE. Eight UWorld candidates at the offsets the log printed, three in
    // UEngine, no shared pointer. One of the UWorld ones is the real device: its handle's
    // weak pointer names the world it sits in.
    // -------------------------------------------------------------------------------
    {
        std::vector<DeviceCandidate> v = {
            Noise(0x482C7780, 0xA001, 0x0360, true),
            Noise(0x1673ACC0, 0xA002, 0x0778, true),
            Noise(0x4E088C00, 0xA003, 0x0958, true),
            Noise(0x40ADAF00, 0xA004, 0x0EF0, true),
            Noise(0x9655E500, 0xA005, 0x1408, true),
            Noise(0x7F8CD9C0, 0xA006, 0x1428, true),
            Noise(0x33DDD900, 0xA007, 0x17E8, true),
            Noise(0x1111AAA0, 0xA008, 0x1990, true),
            Noise(0x2222BBB0, 0xB001, 0x0100, false),
            Noise(0x3333CCC0, 0xB002, 0x0288, false),
            Noise(0x4444DDD0, 0xB003, 0x0400, false),
        };
        // The real one, hiding among them: a UWorld handle whose weak pointer is that world.
        DeviceCandidate real = Device(0x55550000, 0xF11E, 0x0AA0, true);
        real.worldIndexMatch = true;
        v.push_back(real);

        Check(!OldSamePointerRuleWouldAccept(v),
              "THE OLD RULE REFUSES THIS EXACT DATA - the regression can fire");

        Choice c = ChooseDevice(v, true);
        Check(c.index >= 0, "the new ladder ACCEPTS where the old one refused");
        Check(c.index >= 0 && v[static_cast<std::size_t>(c.index)].device == P(0x55550000),
              "and it picks the device whose handle names its own world");
        Check(c.refusal == nullptr && c.why != nullptr, "with a reason, not a shrug");

        int rejectedNoise = 0;
        for (const DeviceCandidate& x : v)
            if (x.isUObject && x.reject != nullptr) ++rejectedNoise;
        Check(rejectedNoise == 11, "all 11 UObject candidates are rejected by name");
    }

    // -------------------------------------------------------------------------------
    // The coordinator's suggestion: take the UEngine candidate alone when it passes the
    // strong test. MainAudioDeviceHandle is the one that matters for a submix.
    // -------------------------------------------------------------------------------
    {
        std::vector<DeviceCandidate> v = { Noise(0x1000, 0xA001, 0x0360, true),
                                           Noise(0x2000, 0xA002, 0x0778, true) };
        DeviceCandidate eng = Device(0x3000, 0xF11E, 0x0288, false);
        eng.managerBefore = true;      // FAudioDeviceManager* sits at offset-8 (Engine.h:1732)
        v.push_back(eng);

        Check(!OldSamePointerRuleWouldAccept(v), "no shared pointer here either");
        Choice c = ChooseDevice(v, true);
        Check(c.index >= 0 && v[static_cast<std::size_t>(c.index)].device == P(0x3000),
              "a UEngine handle with the manager before it is accepted ALONE");
    }

    // -------------------------------------------------------------------------------
    // Different instances, same class: the cross-check that actually holds. Both are
    // FMixerDevice, so the vtables match even though the pointers do not.
    // -------------------------------------------------------------------------------
    {
        std::vector<DeviceCandidate> v = {
            Noise(0x1000, 0xA001, 0x0360, true),
            Device(0x2000, 0xF11E, 0x0958, true),      // the world's own audio device
            Device(0x3000, 0xF11E, 0x0288, false),     // the MAIN audio device, a different one
        };
        Check(!OldSamePointerRuleWouldAccept(v),
              "two different FAudioDevice INSTANCES share no pointer");
        Choice c = ChooseDevice(v, true);
        Check(c.index >= 0, "but a shared vtable accepts them");
        Check(c.index >= 0 && !v[static_cast<std::size_t>(c.index)].fromWorld,
              "and the UEngine side wins - MainAudioDeviceHandle is what the engine uses");
        Check(v[1].vtableShared && v[2].vtableShared, "both are marked as sharing a vtable");
        Check(!v[0].vtableShared, "the noise is not");
    }

    // -------------------------------------------------------------------------------
    // The self-check that keeps a wrong offset from discarding the right answer. If the
    // ClassPrivate offset is wrong for this build, EVERYTHING tests as "not a UObject" —
    // so the rejection must be disabled rather than trusted.
    // -------------------------------------------------------------------------------
    {
        std::vector<DeviceCandidate> v = { Noise(0x1000, 0xA001, 0x0360, true),
                                           Noise(0x2000, 0xA002, 0x0778, true) };
        v[0].sampleRate = 48000;   // a UObject that happens to hold one, with the test unusable
        Choice off = ChooseDevice(v, false);
        Check(off.index == 0, "with the UObject test UNUSABLE, isUObject is not a rejection");
        Choice on = ChooseDevice(v, true);
        Check(on.index < 0, "with it usable, the same candidate IS rejected");
    }

    // -------------------------------------------------------------------------------
    // Refusals must stay refusals. Guessing here means calling a virtual on a random
    // heap object, which is a crash in someone's game.
    // -------------------------------------------------------------------------------
    {
        std::vector<DeviceCandidate> none;
        Choice c = ChooseDevice(none, true);
        Check(c.index < 0 && c.refusal != nullptr, "no candidates -> a refusal with a reason");
    }
    {
        // Two plausible devices, different vtables, no decisive signal on either.
        std::vector<DeviceCandidate> v = { Device(0x1000, 0xAAAA, 0x0360, true),
                                           Device(0x2000, 0xBBBB, 0x0778, true) };
        Choice c = ChooseDevice(v, true);
        Check(c.index < 0, "two undistinguished survivors -> REFUSE rather than guess");
        Check(c.refusal != nullptr, "and say why");
    }
    {
        // Exactly one survivor is enough, even with no positional corroboration.
        std::vector<DeviceCandidate> v = { Noise(0x1000, 0xA001, 0x0360, true),
                                           Device(0x2000, 0xBBBB, 0x0778, true) };
        Choice c = ChooseDevice(v, true);
        Check(c.index == 1, "a single survivor is accepted");
    }
    {
        // A candidate with no sample rate anywhere in it is not an audio device.
        std::vector<DeviceCandidate> v = { Device(0x1000, 0xAAAA, 0x0360, true) };
        v[0].sampleRate = 0;
        Choice c = ChooseDevice(v, true);
        Check(c.index < 0, "no standard sample rate -> not an FAudioDevice");
    }

    // -------------------------------------------------------------------------------
    // Ordering: the strongest signal must win even when a weaker one is listed first.
    // -------------------------------------------------------------------------------
    {
        std::vector<DeviceCandidate> v;
        DeviceCandidate weaker = Device(0x1000, 0xF11E, 0x0100, false);
        weaker.managerBefore = true;
        v.push_back(weaker);
        DeviceCandidate strongest = Device(0x2000, 0xCCCC, 0x0AA0, true);
        strongest.worldIndexMatch = true;
        v.push_back(strongest);

        Choice c = ChooseDevice(v, true);
        Check(c.index == 1, "the exact world-index match outranks a positional UEngine match");
    }

    std::printf(g_failures == 0 ? "\nall SubmixChoice cases passed\n" : "\n%d FAILURE(S)\n",
                g_failures);
    return g_failures == 0 ? 0 : 1;
}
