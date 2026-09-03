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
// the index is INFERRED from it by Itanium/MSVC vtable arithmetic.
//
// **Stray is a LICENSEE build.** One virtual inserted into AudioDevice.h by the licensee
// shifts everything, so the index is a CONFIG VALUE (`SubmixRegisterSlot`) whose default is
// 16, and it is logged before it is used.
//
// THE POINTER: `FAudioDeviceHandle` (AudioDeviceManager.h:81-125) is
//
//     TWeakObjectPtr<UWorld> World;     // +0,  8 bytes (int32 index, int32 serial)
//     FAudioDevice*          Device;    // +8
//     Audio::FDeviceId       DeviceId;  // +16, uint32
//
// (`INSTRUMENT_AUDIODEVICE_HANDLES` defaults to 0, so there is no StackWalkID.) One sits in
// `UWorld::AudioDeviceHandle` (World.h:1246) and another in `UEngine::MainAudioDeviceHandle`
// (Engine.h:1734) — neither reflected, both at offsets that shift with `#if` blocks
// (`WITH_EDITOR` in UWorld, `WITH_DYNAMIC_RESOLUTION` in UEngine), which is exactly why we
// SCAN for the shape instead of hardcoding an offset.
//
// A candidate must satisfy ALL of:
//   * Device is non-null, 8-aligned and readable;
//   * *(void**)Device — the vtable pointer — lies inside the main executable's image;
//   * the first `kVtableProbeSlots` vtable entries are all non-null and all inside the image
//     (a stray heap pointer fails this almost immediately);
//   * DeviceId is small;
//   * the weak-pointer half is a plausible (index, serial) pair.
//
// And then the decisive one: **the same Device pointer must be found in BOTH the UWorld and
// the UEngine object.** Two independent structures, two independent offsets, one answer. A
// coincidence that satisfies the shape test in one object will not also satisfy it in the
// other at the same value.
//
// Win32 only. No UE4SS types, so this compiles and links on the mingw lane.
#pragma once

#include <cstddef>
#include <cstdint>

namespace sds {
namespace submix {

// The default derived above. Overridable because Stray is a licensee build.
constexpr int kDefaultRegisterSlot = 16;

// How much of each vtable to validate. FAudioDevice has well over 60 virtuals, so 32 is
// comfortably inside it and still enough that a non-vtable pointer fails.
constexpr int kVtableProbeSlots = 32;

// How far into each UObject to scan for the handle shape.
constexpr std::size_t kDefaultScanBytes = 0x2000;

struct DiscoveryInput
{
    const void* worldObject  = nullptr;   // a UWorld*, from UE4SS reflection
    const void* engineObject = nullptr;   // a UEngine*, from UE4SS reflection
    const void* imageBase    = nullptr;   // the game exe's base address
    std::size_t imageSize    = 0;
    std::size_t scanBytes    = kDefaultScanBytes;
    // "both" is the only configuration that cross-validates. The single-object modes exist
    // so a session where one object could not be found still produces an answer, loudly.
    bool        requireBoth  = true;
};

struct DiscoveryResult
{
    bool          ok             = false;
    const void*   device         = nullptr;
    const void*   vtable         = nullptr;
    std::size_t   worldOffset    = 0;      // where in UWorld the handle was found
    std::size_t   engineOffset   = 0;
    std::uint32_t deviceId       = 0;
    int           worldMatches   = 0;      // distinct devices that passed in each object
    int           engineMatches  = 0;
    const char*   refusal        = nullptr;   // set when !ok; a literal, safe to log
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

} // namespace submix
} // namespace sds
