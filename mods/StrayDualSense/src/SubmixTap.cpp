#include "SubmixTap.hpp"

#include "Log.hpp"
#include "Platform.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstring>

namespace sds {
namespace submix {
namespace {

// The leaked page. One VirtualAlloc region so the trampoline's RIP-relative load can reach
// the target slot with a 32-bit displacement: two separate allocations could land more than
// 2 GB apart and the encoding would silently be wrong.
constexpr std::size_t kPageSize   = 4096;
constexpr std::size_t kOffObject  = 0x00;   // qword: the vtable pointer. THIS is what the engine holds.
constexpr std::size_t kOffSelf    = 0x08;   // qword: Tap*, read by the dispatcher
constexpr std::size_t kOffTarget  = 0x20;   // qword: where the trampoline jumps, or 0
constexpr std::size_t kOffVtable  = 0x40;   // qword: vtable slot 0 -> the trampoline
constexpr std::size_t kOffCode    = 0x80;   // the trampoline itself

// x86-64:
//     48 8B 05 <rel32>   mov  rax, [rip + rel32]     ; the target slot, in this same page
//     48 85 C0           test rax, rax
//     74 02              je   +2                     ; skip the jmp, land on the ret
//     FF E0              jmp  rax                    ; TAIL call: rcx/rdx/r8/r9 and the stack
//                                                    ; are untouched, so the callee sees the
//                                                    ; engine's own arguments
//     C3                 ret                         ; detached: do nothing at all
//
// rel32 is measured from the END of the mov, i.e. kOffCode + 7.
const unsigned char kTrampoline[] = {
    0x48, 0x8B, 0x05, 0x00, 0x00, 0x00, 0x00,
    0x48, 0x85, 0xC0,
    0x74, 0x02,
    0xFF, 0xE0,
    0xC3,
};
constexpr std::size_t kTrampolineRel32At = 3;

std::uint64_t DoubleBitsOf(double v)
{
    std::uint64_t b = 0;
    std::memcpy(&b, &v, sizeof(b));
    return b;
}

double DoubleFromBits(std::uint64_t b)
{
    double v = 0.0;
    std::memcpy(&v, &b, sizeof(v));
    return v;
}

} // namespace

Tap* Tap::Create(const char* tag)
{
    unsigned char* page = static_cast<unsigned char*>(
        ::VirtualAlloc(nullptr, kPageSize, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (page == nullptr)
    {
        SDS_LOG_ERROR("submix tap '%s': VirtualAlloc for the listener page failed (err %lu). "
                      "Without it the engine would be calling into this DLL, which unloads.",
                      tag, static_cast<unsigned long>(::GetLastError()));
        return nullptr;
    }
    std::memset(page, 0, kPageSize);

    Tap* tap = new Tap();
    tap->m_tag = tag;
    tap->m_scratch.assign(kTapScratchFrames * 2, 0.0f);

    // The trampoline, with its displacement patched to reach the target slot in this page.
    std::memcpy(page + kOffCode, kTrampoline, sizeof(kTrampoline));
    const std::int64_t rel =
        static_cast<std::int64_t>(kOffTarget) - static_cast<std::int64_t>(kOffCode + 7);
    const std::int32_t rel32 = static_cast<std::int32_t>(rel);
    std::memcpy(page + kOffCode + kTrampolineRel32At, &rel32, sizeof(rel32));

    void* trampoline = page + kOffCode;
    void* vtable     = page + kOffVtable;
    void* dispatch   = reinterpret_cast<void*>(&Tap::Dispatch);

    std::memcpy(page + kOffVtable, &trampoline, sizeof(trampoline));   // vtable[0]
    std::memcpy(page + kOffObject, &vtable,     sizeof(vtable));       // object -> vtable
    Tap* selfPtr = tap;
    std::memcpy(page + kOffSelf,   &selfPtr,    sizeof(selfPtr));
    std::memcpy(page + kOffTarget, &dispatch,   sizeof(dispatch));

    ::FlushInstructionCache(::GetCurrentProcess(), page, kPageSize);

    tap->m_listener  = page + kOffObject;
    tap->m_targetPtr = reinterpret_cast<void**>(page + kOffTarget);

    SDS_LOG_INFO("submix tap '%s': listener %p (vtable %p -> trampoline %p -> %p), page LEAKED "
                 "on purpose - UnregisterSubmixBufferListener is asynchronous, so the engine "
                 "may still call this after we are gone.",
                 tag, tap->m_listener, vtable, trampoline, dispatch);
    return tap;
}

void Tap::SetRing(SubmixRing* ring)
{
    m_ring.store(ring, std::memory_order_release);
}

void Tap::Detach()
{
    if (m_targetPtr == nullptr)
        return;
    m_ring.store(nullptr, std::memory_order_release);
    // One aligned 8-byte store, atomic on x64. From here the trampoline returns immediately
    // and nothing in this DLL is ever reached again.
    *m_targetPtr = nullptr;

    // Wait for a callback that is ALREADY inside OnBuffer. The ring it may be writing into
    // belongs to the Runtime and is about to be destroyed; the target store above stops new
    // calls but says nothing about one in flight.
    int waited = 0;
    while (m_inFlight.load(std::memory_order_acquire) > 0 && waited < kTapDetachWaitMs)
    {
        ::Sleep(5);
        waited += 5;
    }
    const int stuck = m_inFlight.load(std::memory_order_acquire);

    SDS_LOG_INFO("submix tap '%s': detached after %llu callback(s) (waited %d ms; in flight "
                 "at the end: %d%s); the leaked trampoline now returns immediately.",
                 m_tag,
                 static_cast<unsigned long long>(m_callbacks.load(std::memory_order_relaxed)),
                 waited, stuck,
                 stuck > 0 ? " - TIMED OUT, the audio render thread did not leave" : "");
}

TapStats Tap::Stats() const
{
    TapStats s;
    s.callbacks       = m_callbacks.load(std::memory_order_relaxed);
    s.frames          = m_framesSeen.load(std::memory_order_relaxed);
    s.badCallbacks    = m_bad.load(std::memory_order_relaxed);
    s.lastNumSamples  = m_lastNumSamples.load(std::memory_order_relaxed);
    s.lastNumChannels = m_lastNumChannels.load(std::memory_order_relaxed);
    s.lastSampleRate  = m_lastSampleRate.load(std::memory_order_relaxed);
    s.lastCallbackMs  = m_lastCallbackMs.load(std::memory_order_relaxed);
    s.lastSubmix      = m_lastSubmix.load(std::memory_order_relaxed);
    s.lastAudioClock  = DoubleFromBits(m_lastClockBits.load(std::memory_order_relaxed));
    return s;
}

void Tap::Dispatch(void* self, const void* owningSubmix, float* audioData,
                   std::int32_t numSamples, std::int32_t numChannels,
                   std::int32_t sampleRate, double audioClock)
{
    if (self == nullptr)
        return;
    // The object is one qword (the vtable pointer); the Tap* sits in the next one.
    Tap* tap = *reinterpret_cast<Tap**>(static_cast<unsigned char*>(self) + (kOffSelf - kOffObject));
    if (tap == nullptr)
        return;
    tap->OnBuffer(owningSubmix, audioData, numSamples, numChannels, sampleRate, audioClock);
}

void Tap::OnBuffer(const void* owningSubmix, const float* audioData, std::int32_t numSamples,
                   std::int32_t numChannels, std::int32_t sampleRate, double audioClock)
{
    // THE AUDIO RENDER THREAD. No lock, no allocation, no I/O, no logging: the engine holds
    // BufferListenerCriticalSection across this call, so stalling here stalls the game's own
    // audio (AudioMixerSubmix.cpp:1376).
    //
    // The in-flight count is what Detach() waits on. Its scope must cover every use of the
    // borrowed ring pointer, so it is raised first and dropped last, on every path.
    struct InFlight
    {
        std::atomic<int>& n;
        explicit InFlight(std::atomic<int>& c) : n(c) { n.fetch_add(1, std::memory_order_acq_rel); }
        ~InFlight() { n.fetch_sub(1, std::memory_order_acq_rel); }
    } inFlight(m_inFlight);

    m_callbacks.fetch_add(1, std::memory_order_relaxed);
    m_lastNumSamples.store(numSamples, std::memory_order_relaxed);
    m_lastNumChannels.store(numChannels, std::memory_order_relaxed);
    m_lastSampleRate.store(sampleRate, std::memory_order_relaxed);
    m_lastSubmix.store(owningSubmix, std::memory_order_relaxed);
    m_lastClockBits.store(DoubleBitsOf(audioClock), std::memory_order_relaxed);
    m_lastCallbackMs.store(NowMs(), std::memory_order_relaxed);

    // NumSamples is INTERLEAVED SAMPLES (frames * channels), not frames — AudioDevice.h:400
    // and AudioMixerSubmix.cpp:1082. Treating it as frames is a factor-of-N speed error, so
    // the division happens here, once, and everything downstream takes frames.
    if (audioData == nullptr || numChannels <= 0 || numSamples <= 0 ||
        numChannels > 64 || (numSamples % numChannels) != 0)
    {
        m_bad.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const std::size_t frames = static_cast<std::size_t>(numSamples / numChannels);
    m_framesSeen.fetch_add(frames, std::memory_order_relaxed);

    SubmixRing* ring = m_ring.load(std::memory_order_acquire);
    float*      out  = m_scratch.data();
    const std::size_t stride = static_cast<std::size_t>(numChannels);

    // Chunked rather than grown: an allocation on this thread is exactly the stall we must
    // never cause, and a project ini can legally raise the callback size.
    for (std::size_t done = 0; done < frames; done += kTapScratchFrames)
    {
        const std::size_t n = (frames - done) < kTapScratchFrames ? (frames - done) : kTapScratchFrames;
        DownmixToStereo(audioData + done * stride, n, numChannels, out);
        m_meter.Push(out, n);
        if (ring != nullptr)
            ring->Write(out, n);
    }
}

} // namespace submix
} // namespace sds
