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
    bool          vtableShared    = false;   // computed here: same vtable, other object
    const char*   reject          = nullptr; // set here; nullptr = survived every test
};

struct Choice
{
    int         index   = -1;        // into the candidate vector, -1 = refused
    const char* why     = nullptr;   // which rung accepted it
    const char* refusal = nullptr;   // why nothing was accepted
};

// Is this int32 a sample rate an audio device can actually be running at?
bool IsStandardSampleRate(std::int32_t hz);

// Fills in `vtableShared` and `reject` for every candidate, then walks the acceptance ladder.
// `uobjectTestUsable` comes from the caller's self-check: when the ClassPrivate offset could
// not be confirmed against an object already known to be a UObject, the UObject rejection is
// not applied at all — a wrong offset must not silently discard the right candidate.
Choice ChooseDevice(std::vector<DeviceCandidate>& candidates, bool uobjectTestUsable);

} // namespace submix
} // namespace sds
