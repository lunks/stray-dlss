// StrayDualSense — finding `FAudioDevice*` and calling one virtual on it, with no engine
// headers, no symbols and no byte signature.
//
// This is the part of the submix spike that CANNOT be verified from the machine that wrote
// it, so every step is a check that must pass and a line that is logged. Nothing here
// dereferences anything it has not first probed with VirtualQuery, and nothing calls anything
// until the whole chain has been validated.
//
// ---------------------------------------------------------------------------------------
// WHAT WE NEED, AND WHY IT IS NOT A UPROPERTY
// ---------------------------------------------------------------------------------------
// `FAudioDevice::RegisterSubmixBufferListener` is a VIRTUAL member of a plain C++ class
// (AudioDevice.h:863). It is not reflected, no Blueprint-callable function in UE 4.27 reaches
// it, and `FAudioDevice` is not a UObject — so UE4SS's reflection cannot see any of it. Two
// things are therefore required: a pointer to a live `FAudioDevice`, and the vtable index.
//
// THE INDEX: 16. Derived by counting FAudioDevice's virtual member functions in declaration
// order from UE 4.27.2's AudioDevice.h, and the count is unusually trustworthy because NOT
// ONE of them sits inside a preprocessor conditional — the vtable is identical in Shipping,
// Development and Editor builds:
//
//    0  ~FExec / ~FAudioDevice (override)      Exec.h:12   / AudioDevice.h:500
//    1  Exec (override)                        AudioDevice.h:415
//    2  GetAudioDeviceList          505        9   Precache                     605
//    3  UpdateGameThread            528       10   StopAllSounds                653
//    4  CountBytes                  540       11   InitDefaultAudioBuses        845
//    5  FadeOut                     558       12   ShutdownDefaultAudioBuses    848
//    6  FadeIn                      559       13   InitSoundSubmixes            851
//    7  FlushExtended               569       14   RegisterSoundSubmix          854
//    8  FlushAudioRenderingCommands 575       15   UnregisterSoundSubmix        857
//   16  RegisterSubmixBufferListener          863   <-- the one we call
//   17  UnregisterSubmixBufferListener        872
//
// `FAudioDevice : public FExec` is single inheritance with no virtual bases, and FExec has
// exactly two virtuals (a destructor and `Exec`), both unconditional. HARD for the source;
// the index is INFERRED from it by MSVC vtable arithmetic.
//
// **Stray is a LICENSEE build.** One virtual inserted into AudioDevice.h by the licensee
// shifts everything, so the index is a CONFIG VALUE (`SubmixRegisterSlot`) whose default is
// 16, and it is logged before it is used.
//
// ---------------------------------------------------------------------------------------
// THE POINTER — and why the first design failed on the box
// ---------------------------------------------------------------------------------------
// `FAudioDeviceHandle` (AudioDeviceManager.h:81-125) is
//
//     TWeakObjectPtr<UWorld> World;     // +0,  8 bytes (int32 ObjectIndex, int32 SerialNumber)
//     FAudioDevice*          Device;    // +8
//     Audio::FDeviceId       DeviceId;  // +16, uint32
//
// One sits in `UWorld::AudioDeviceHandle` and another in `UEngine::MainAudioDeviceHandle`
// (Engine.h:1735, immediately after `FAudioDeviceManager* AudioDeviceManager` at :1732),
// neither reflected and both at offsets that shift with `#if` blocks — which is why we scan
// for the shape instead of hardcoding an offset.
//
// **MEASURED IN THE GAME, 2026-09-02: that shape alone is far too loose.** It found EIGHT
// candidates in UWorld and three in UEngine, and the original rule — demand the same POINTER
// in both objects — refused, correctly by its own logic and wrongly in fact:
//
//     UWorld +0x0360 device=482C7780 id=0    UWorld +0x0778 device=1673ACC0 id=0
//     UWorld +0x0958 device=4E088C00 id=1    ... 8 in all, 3 in UEngine, none shared
//
// The reason is obvious in hindsight: the shape test was really "a pointer to any polymorphic
// object whose class lives in the exe", and a UWorld is FULL of UObject pointers. Every
// UObject has an in-image vtable with far more than 32 slots, so they all passed. The real
// device was outvoted by noise — and demanding an identical pointer across two objects was
// the wrong cross-check anyway, since a world audio device and the main audio device are
// allowed to be different instances.
//
// So the test is now POSITIVE and PER-CANDIDATE, and every candidate's verdict is logged:
//
//   * NOT A UOBJECT.  `FAudioDevice` is not one. A UObject's `ClassPrivate` (+0x10) is a
//     UClass, whose own ClassPrivate is the `UClass` UClass, which is its OWN class — a fixed
//     point. Walking +0x10 a few times and finding that fixed point identifies a UObject
//     whatever its class, Blueprint-generated ones included. SELF-CHECKED at runtime against
//     the known UWorld and UEngine: if THEY do not test as UObjects then the offset
//     assumption is wrong, and the rejection is disabled and said so, rather than silently
//     discarding every candidate.
//   * A PLAUSIBLE SAMPLE RATE.  `FAudioDevice::SampleRate` is an `int32` (AudioDevice.h:1789)
//     and the `PlatformSettings` right after it carries another copy, so a live audio device
//     contains at least one int32 from the standard-rate set. A random UObject does not.
//   * THE WORLD'S OWN INDEX.  For a candidate found in UWorld, the handle's
//     `TWeakObjectPtr<UWorld>` names the world that requested it — so its `ObjectIndex` must
//     equal the UWorld's own `InternalIndex` (UObjectBase +0x0C). Exact, essentially
//     unforgeable, and the strongest single signal available.
//   * THE MANAGER BEFORE IT.  For a candidate found in UEngine, the qword eight bytes earlier
//     should be `FAudioDeviceManager*` — a readable non-image heap pointer (Engine.h:1732).
//   * A SHARED VTABLE, not a shared pointer. Two different `FAudioDevice` instances are still
//     both `FMixerDevice`, so their vtables match even when the pointers do not. That is the
//     cross-check the first version should have used.
//
// Win32 only. No UE4SS types, so this compiles and links on the mingw lane.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// DeviceCandidate and the acceptance ladder live here: they are pure, so CI proves them.
#include "SubmixChoice.hpp"

namespace sds {
namespace submix {

// The default derived above. Overridable because Stray is a licensee build.
constexpr int kDefaultRegisterSlot = 16;

// How much of each vtable to validate. FAudioDevice has well over 60 virtuals, so 32 is
// comfortably inside it and still enough that a non-vtable pointer fails.
constexpr int kVtableProbeSlots = 32;

// How far into each UObject to scan for the handle shape.
constexpr std::size_t kDefaultScanBytes = 0x2000;

// How far into a CANDIDATE to look for its int32 SampleRate. FAudioDevice is a large object
// and SampleRate sits well down it, so this is deliberately generous.
constexpr std::size_t kFingerprintBytes = 0x1800;

// UObjectBase's layout, UE 4.27: vtable(8) ObjectFlags(4) InternalIndex(4) ClassPrivate(8).
// Both offsets are SELF-CHECKED at runtime against objects already known to be UObjects.
constexpr std::size_t kUObjectInternalIndexOffset = 0x0C;
constexpr std::size_t kUObjectClassOffset         = 0x10;

struct DiscoveryInput
{
    const void* worldObject  = nullptr;   // a UWorld*, from UE4SS reflection
    const void* engineObject = nullptr;   // a UEngine*, from UE4SS reflection
    const void* imageBase    = nullptr;   // the game exe's base address
    std::size_t imageSize    = 0;
    std::size_t scanBytes    = kDefaultScanBytes;
    // Which objects may supply the answer. Both by default: the ladder decides, and
    // UEngine::MainAudioDeviceHandle is a perfectly good answer on its own.
    bool        useWorld     = true;
    bool        useEngine    = true;
    // Print every candidate and its verdict. The caller retries once a second until it binds,
    // so leaving this on for a session that never binds would drown the log — which is this
    // project's only feedback channel.
    bool        logCandidates = true;
};

struct DiscoveryResult
{
    bool          ok         = false;
    const void*   device     = nullptr;
    const void*   vtable     = nullptr;
    std::size_t   offset     = 0;
    bool          fromWorld  = false;
    std::uint32_t deviceId   = 0;
    std::int32_t  sampleRate = 0;
    const char*   why        = nullptr;    // which rung of the ladder accepted it
    const char*   refusal    = nullptr;    // set when !ok
    bool          uobjectTestUsable = false;   // did the self-check pass
    std::vector<DeviceCandidate> candidates;   // every one, with its verdict
};

// Pure inspection: reads memory, calls nothing.
DiscoveryResult FindAudioDevice(const DiscoveryInput& in);

// Writes the first `kVtableProbeSlots` entries of `device`'s vtable to the log as RVAs, with
// duplicates marked. This is the artefact that makes a wrong `SubmixRegisterSlot` fixable
// from one pasted log rather than from another round trip: MSVC folds identical empty virtual
// bodies (/OPT:ICF), so the base FAudioDevice stubs cluster on one address and an override
// stands out.
void LogVtable(const void* device, const void* imageBase, std::size_t imageSize);

// Calls `device->vtable[slot](listener, submix)` after re-checking every invariant. Returns
// false WITHOUT calling if anything is off; `whyNot` then names the check that failed.
bool CallRegisterSubmixBufferListener(const void* device, int slot, void* listener,
                                      void* submix, const void* imageBase,
                                      std::size_t imageSize, const char** whyNot);

// Committed memory that allows reads. Used before every dereference in this file.
bool Readable(const void* p, std::size_t bytes);

// Does `p` sit at the head of a UObject? Walks `ClassPrivate` looking for the `UClass` fixed
// point. Exposed so the caller can self-check it against an object it KNOWS is a UObject
// before trusting the rejection it drives.
bool LooksLikeUObject(const void* p);

} // namespace submix
} // namespace sds
