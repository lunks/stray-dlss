// StrayDualSense — the ISubmixBufferListener we hand to UE 4.27's audio mixer.
//
// VERIFIED against the UE 4.27.2 source (public mirror AlexMercer-MA/UnrealEngine-4.27,
// Engine/Source/Runtime/Engine/Public/AudioDevice.h:394-407). The interface has EXACTLY ONE
// virtual member and NO virtual destructor:
//
//   class ENGINE_API ISubmixBufferListener
//   {
//   public:
//       virtual void OnNewSubmixBuffer(const USoundSubmix* OwningSubmix, float* AudioData,
//                                      int32 NumSamples, int32 NumChannels,
//                                      const int32 SampleRate, double AudioClock) = 0;
//   };
//
// There is no `IsRenderingAudio()` in 4.27 — that is a UE5 addition, and assuming it would
// have put a second entry in the vtable and sent the engine into the wrong function. HARD.
//
// So the object is one pointer wide: a vtable pointer, and a vtable with one slot.
//
// WHY IT IS HAND-BUILT INSTEAD OF A C++ CLASS
// -------------------------------------------
// `FMixerSubmix::UnregisterBufferListener` is dispatched with
// `AsyncTask(ENamedThreads::AudioThread, ...)` (AudioMixerDevice.cpp:2405), so unregistering
// is ASYNCHRONOUS: the engine may still hold and call our pointer after the call returns. A
// plain C++ object in the mod DLL would then be a call into unmapped code the moment UE4SS
// unloads the mod.
//
// Therefore BOTH the vtable AND the code it points at live in pages allocated with
// VirtualAlloc and DELIBERATELY NEVER FREED. The vtable slot points at a 15-byte trampoline
// that loads a target pointer from the same leaked page and tail-jumps to it — or returns
// immediately when that pointer is null. `Detach()` nulls it with one aligned 8-byte store.
// After that the engine can call us forever and reach nothing but a `ret`.
//
// The precedent in this repo is `src/backend_native/resource_registry.cpp`'s sentinel: an
// object whose lifetime the other side controls, so ours must outlive us.
//
// THREADING
// ---------
// `OnNewSubmixBuffer` runs on the AUDIO RENDER THREAD (AudioMixerSubmix.cpp:1026's
// AUDIO_MIXER_CHECK_AUDIO_PLAT_THREAD), which is neither the game thread nor UE4SS's update
// thread, and whose thread id is explicitly allowed to change between calls
// (AudioMixerDevice.cpp:704). The engine holds `BufferListenerCriticalSection` across the
// whole listener loop (AudioMixerSubmix.cpp:1376), so we are never re-entered for one submix
// — but blocking in here stalls the game's own audio. So: no locks, no allocation, no I/O.
//
// Win32 only (VirtualAlloc). No UE4SS types, no COM.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "SubmixDsp.hpp"

namespace sds {
namespace submix {

// What one callback told us. Every field comes from the engine rather than from an
// assumption, because a project's ini can change the callback size and the sample rate
// (FAudioPlatformSettings defaults are 1024 frames / 48000 Hz, but they are ini-overridable).
struct TapStats
{
    std::uint64_t callbacks       = 0;
    std::uint64_t frames          = 0;
    std::uint64_t badCallbacks    = 0;   // numChannels <= 0, null data, absurd sizes
    std::int32_t  lastNumSamples  = 0;   // as handed to us: frames * channels, NOT frames
    std::int32_t  lastNumChannels = 0;
    std::int32_t  lastSampleRate  = 0;
    std::uint64_t lastCallbackMs  = 0;   // GetTickCount64 at the last callback, 0 = never
    const void*   lastSubmix      = nullptr;
    double        lastAudioClock  = 0.0;
};

class Tap
{
public:
    // Allocates the two leaked pages and builds the vtable and trampoline. Returns nullptr
    // only if VirtualAlloc failed. `tag` is borrowed and must outlive the process (a literal).
    static Tap* Create(const char* tag);

    // The pointer to hand to FAudioDevice::RegisterSubmixBufferListener. Never null once
    // Create() succeeded, and valid for the life of the PROCESS, not of this object.
    void* ListenerPointer() const { return m_listener; }

    // Attach a ring to receive the stereo fold. nullptr = meter only, which is what the
    // master-submix probe uses: it answers "does the tap mechanism work at all" without
    // putting the game's whole soundtrack on the coils.
    void SetRing(SubmixRing* ring);

    // Stop doing anything, forever, without freeing the object the engine still holds.
    void Detach();

    TapStats     Stats() const;
    LevelReading TakeLevels() { return m_meter.Take(); }
    const char*  Tag() const { return m_tag; }

private:
    Tap() = default;

    // The trampoline's target. MS x64: (self, OwningSubmix, AudioData, NumSamples,
    // NumChannels, SampleRate, AudioClock).
    static void Dispatch(void* self, const void* owningSubmix, float* audioData,
                         std::int32_t numSamples, std::int32_t numChannels,
                         std::int32_t sampleRate, double audioClock);
    void OnBuffer(const void* owningSubmix, const float* audioData, std::int32_t numSamples,
                  std::int32_t numChannels, std::int32_t sampleRate, double audioClock);

    const char* m_tag = "submix";

    void*  m_listener  = nullptr;   // page A + 0: the object we hand the engine
    void** m_targetPtr = nullptr;   // page A + 0x20: what the trampoline jumps to

    std::atomic<SubmixRing*> m_ring{nullptr};
    LevelMeter               m_meter;

    // Preallocated on the creating thread; the audio thread only ever reads and writes into
    // it. Sized far above the engine's 1024-frame default so a project that raised it still
    // needs no allocation; anything larger is processed in chunks rather than growing.
    std::vector<float> m_scratch;

    std::atomic<std::uint64_t> m_callbacks{0};
    std::atomic<std::uint64_t> m_framesSeen{0};
    std::atomic<std::uint64_t> m_bad{0};
    std::atomic<std::int32_t>  m_lastNumSamples{0};
    std::atomic<std::int32_t>  m_lastNumChannels{0};
    std::atomic<std::int32_t>  m_lastSampleRate{0};
    std::atomic<std::uint64_t> m_lastCallbackMs{0};
    std::atomic<const void*>   m_lastSubmix{nullptr};
    std::atomic<std::uint64_t> m_lastClockBits{0};
};

// The scratch size, in stereo frames. 16384 is 16x the engine's default callback and 128 KB
// of memory; a bigger callback is chunked, never allocated for.
constexpr std::size_t kTapScratchFrames = 16384;

} // namespace submix
} // namespace sds
