// StrayDualSense — which of the FAudioDevice candidates to believe.
//
// Split out of SubmixDiscovery for one reason: THIS IS THE PART THAT GOT IT WRONG IN THE GAME
// (2026-09-02), and it is pure, so CI can prove the replacement instead of the box.
//
// The scan hands us every `{TWeakObjectPtr<UWorld>; FAudioDevice*; FDeviceId}`-shaped triple
// it found in the UWorld and UEngine objects, each annotated with what could be checked about
// it. Deciding between them is arithmetic over that table and nothing else — no memory reads,
// no Windows, no engine.
//
// The rule this replaces demanded the SAME POINTER in both objects. Measured on the box it
// found 8 candidates in UWorld, 3 in UEngine, and no pointer in common, so it refused. Two
// separate mistakes: the shape test admitted every UObject pointer in a UWorld (they all have
// in-image vtables), and a world audio device and the main audio device are allowed to be
// DIFFERENT INSTANCES, so identical pointers were never guaranteed. What IS guaranteed is
// that both are `FMixerDevice`, hence a shared VTABLE.
//
// MEASURED AGAIN 2026-09-03, this time by reading the live process through /proc/<pid>/mem
// while the game ran — which beats any amount of deriving from stock headers:
//
//   * UWorld does not point at the audio device AT ALL. Every pointer-shaped qword in its
//     first 4 KB was followed and none of the targets holds an aligned standard sample rate.
//     `UWorld::AudioDeviceHandle.Device` is null on this build because the world uses the MAIN
//     audio device rather than requesting its own — so the handle is empty and there is
//     nothing to find. Every rung that needed UWorld corroboration (the world-index match, a
//     shared vtable) is therefore DEAD HERE, and a ladder that relied on them refuses forever.
//   * UEngine has ELEVEN pointees holding an aligned standard rate, so "survives the rate
//     test" cannot be the deciding rung either.
//   * The one that is shaped like an audio device: `Engine+0x0A88 -> vt at +0, 48000 at +0x0C`.
//
// That last offset is the discriminator, and it is PRINCIPLED rather than fitted:
// `FAudioDevice`'s first two data members are `int32 NumStoppingSources; int32 SampleRate;`
// (AudioDevice.h:1786-1789), and `FMixerDevice` inherits it as the primary base, so the
// FAudioDevice subobject starts at offset 0. vtable(8) + NumStoppingSources(4) puts SampleRate
// at exactly **+0x0C** — which is where the live process has it. The other ten hold their rate
// hundreds or thousands of bytes in: those are rate tables and unrelated objects, and two of
// them hold 48000 AND 44100, which no single device does.
//
// Portable: no Windows, no UE4SS, no engine headers.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace sds {
namespace submix {

struct DeviceCandidate
{
    const void*   device    = nullptr;
    const void*   vtable    = nullptr;
    std::size_t   offset    = 0;      // where in its owning object the handle was found
    std::uint32_t id        = 0;      // the handle's DeviceId
    bool          fromWorld = false;  // else from the UEngine object

    // Verdict signals, all logged by the caller.
    bool          isUObject       = false;   // FAudioDevice is not one
    bool          worldIndexMatch = false;   // UWorld only, and the strongest signal there is
    bool          managerBefore   = false;   // UEngine only: FAudioDeviceManager* sits at -8
    std::int32_t  sampleRate      = 0;       // 0 = no standard rate found inside the object
    int           sampleRateHits  = 0;
    std::size_t   sampleRateAt    = 0;       // MEASURED offset of the first hit, so a run can
                                             // compare it against the stock header
    int           distinctRates   = 0;       // >1 means it holds SEVERAL different standard
                                             // rates, which is a rate table, not a device
    bool          rateAtHead      = false;   // computed here: the rate sits where
                                             // FAudioDevice::SampleRate must be
    bool          viaContainer    = false;   // reached through a second hop rather than directly
    std::size_t   containerOffset = 0;       // where that container's pointer sat in the owning
                                             // object. UEngine::AudioDeviceManager is the qword
                                             // immediately before MainAudioDeviceHandle
                                             // (Engine.h:1732), so a candidate reached through a
                                             // container at offset X names the manager at X.
    bool          vtableShared    = false;   // computed here: same vtable, other object
    const char*   reject          = nullptr; // set here; nullptr = survived every test
};

struct Choice
{
    int         index   = -1;        // into the candidate vector, -1 = refused
    const char* why     = nullptr;   // which rung accepted it
    const char* refusal = nullptr;   // why nothing was accepted
};

// vtable(8) + int32 NumStoppingSources(4). Where FAudioDevice::SampleRate must be, and where
// the live process has it (MEASURED 2026-09-03).
constexpr std::size_t kSampleRateOffset = 0x0C;
// Room for a licensee to have inserted a member or two ahead of it. Wide enough to be
// forgiving, far too narrow to admit a rate table hundreds of bytes in.
constexpr std::size_t kSampleRateOffsetSlack = 0x40;

// Is this int32 a sample rate an audio device can actually be running at?
bool IsStandardSampleRate(std::int32_t hz);

// Fills in `vtableShared` and `reject` for every candidate, then walks the acceptance ladder.
// `uobjectTestUsable` comes from the caller's self-check: when the ClassPrivate offset could
// not be confirmed against an object already known to be a UObject, the UObject rejection is
// not applied at all — a wrong offset must not silently discard the right candidate.
Choice ChooseDevice(std::vector<DeviceCandidate>& candidates, bool uobjectTestUsable);

} // namespace submix
} // namespace sds
