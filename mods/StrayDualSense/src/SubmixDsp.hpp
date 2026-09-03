// StrayDualSense — the pure signal path between an engine submix buffer and the coils.
//
// Everything here is portable, allocation-free once constructed, and unit-tested on the Linux
// lane. It is its own file BECAUSE it is the only part of the submix spike that can be proven
// without the game: the tap itself (finding FAudioDevice, registering the listener) is
// unverifiable from here, so the arithmetic around it must not be.
//
// The chain, in order:
//
//   engine submix buffer (interleaved, N channels, engine rate)
//        -> DownmixToStereo   N -> 2, left grip / right grip
//        -> SubmixRing        the audio render thread must never block, so it only writes
//        -> LinearResampler   engine rate -> endpoint rate (1:1 in the measured setup)
//        -> gain, then SoftClip   never a hard clamp: a haptic waveform that clips squarely
//                                 buzzes, and the coils are wideband enough to hear it
//        -> WASAPI RL/RR      (SubmixSink, not here)
//
// No Windows, no UE4SS, no COM.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace sds {
namespace submix {

// UE 4.27's FAudioPlatformSettings default (AudioMixerTypes.h) and the measured rate of the
// pad's own endpoint (§10). Used only until the first callback reports the engine's real
// rate, which a project ini can change.
constexpr std::uint32_t kSubmixDefaultRate = 48000;

// ---------------------------------------------------------------------------------------
// Channel fold.
//
// UE 4.27 hands ISubmixBufferListener an INTERLEAVED buffer with the submix's own channel
// count, and NumSamples is the TOTAL sample count (frames * NumChannels), not the frame
// count — getting that backwards is a factor-of-N speed error, so the caller converts once
// and this takes frames.
//
// The VIBE assets are stereo because the pad has two coils, one per grip
// (docs/STRAY-DUALSENSE.md §12), so channels 0 and 1 are exactly what we want.
//
//   1 channel  -> the same signal to both grips
//   2 channels -> straight through
//   >2         -> channels 0 and 1 only. Deliberately NOT a surround downmix: folding
//                 centre/LFE/rears into a grip would smear a directional effect across both
//                 hands, and UE's own channel order puts FL/FR first regardless.
// ---------------------------------------------------------------------------------------
void DownmixToStereo(const float* interleaved, std::size_t frames, int numChannels,
                     float* outInterleavedStereo);

// ---------------------------------------------------------------------------------------
// Soft clip.
//
// Unity below the knee, then a tanh shoulder that asymptotes to +/-1. C1 at the knee by
// construction: tanh'(0) == 1 and the shoulder is scaled by exactly (1 - knee), so the slope
// is 1 on both sides. Odd-symmetric. NaN maps to 0.
//
// A hard clamp is fine for content authored to fit; a live submix carrying several
// concurrent haptics has no such guarantee, and square corners on a voice coil are audible
// as a buzz.
// ---------------------------------------------------------------------------------------
constexpr float kSoftClipKnee = 0.75f;
float SoftClip(float x);

// ---------------------------------------------------------------------------------------
// The pad's endpoint, and the two lanes that feed it.
//
// MEASURED (docs/STRAY-DUALSENSE.md §10/§12): "Speakers (DualSense Wireless Controller)" is
// 4ch / 48 kHz / float, channel order FL FR RL RR. FL/FR are the SPEAKER, RL/RR are the two
// COILS (left grip, right grip). Two engine submixes are tapped — Submix_controllerMaster
// for the speaker, Submix_vibrationMaster for the coils — and ONE stream carries both, each
// stereo pair landing on its own channel pair. The engine has already mixed, faded, levelled
// and (for the speaker) run its own SBFX_Boost chain, so no trim of ours belongs here: the
// lane gains exist for the game's PadVibrationEnabled switch and an ini A/B, not as a level.
//
// The internal speaker is fed from the endpoint's RIGHT channel in every routing the kernel
// driver documents (hid-playstation.c:1366-1377). Both speaker channels are written so that
// holds whichever channel the firmware reads; a mono source panned centre by the engine
// arrives equal on both anyway.
// ---------------------------------------------------------------------------------------
constexpr std::uint32_t kEndpointChannelFL = 0;
constexpr std::uint32_t kEndpointChannelFR = 1;
constexpr std::uint32_t kEndpointChannelRL = 2;
constexpr std::uint32_t kEndpointChannelRR = 3;
// The fewest channels an endpoint may have for the coil pair to exist at all. On anything
// narrower the sink REFUSES: a haptic waveform on the speaker is worse than silence.
constexpr std::uint32_t kEndpointChannelsRequired = 4;

struct LanePeaks
{
    float coils   = 0.0f;   // after gain and soft clip, max over the frames written
    float speaker = 0.0f;
};

// Writes `frames` endpoint frames of `channels` samples each into `out`:
//   speaker stereo (post gain, soft-clipped) -> FL/FR
//   coil    stereo (post gain, soft-clipped) -> RL/RR
//   every other channel -> 0
// Either lane may be SHORT: frames beyond `coilFrames` / `speakerFrames` are silence for that
// lane only, and a null lane pointer is an empty lane. Channels the endpoint does not have
// are simply not written (the caller has already refused an endpoint narrower than
// kEndpointChannelsRequired). Returns each lane's peak so the caller can drive the
// silence -> signal edge without a second pass.
LanePeaks InterleaveLanes(const float* coilStereo, std::size_t coilFrames, float coilGain,
                          const float* speakerStereo, std::size_t speakerFrames,
                          float speakerGain, std::size_t frames, std::uint32_t channels,
                          float* out);

// ---------------------------------------------------------------------------------------
// Linear resampler, stereo interleaved, stateful across buffers.
//
// The measured setup has the engine and the pad endpoint both at 48 kHz, so step == 1 and
// this is a copy. It exists because "the endpoint runs at a different rate" is otherwise a
// silent pitch/speed error — the asset path carries exactly that bug, as a warning only.
// ---------------------------------------------------------------------------------------
class LinearResampler
{
public:
    void Reset();

    // Consumes `inFrames` stereo frames from `in`, writes at most `maxOutFrames` to `out` and
    // returns how many it wrote. `step` is srcRate / dstRate (1.0 = passthrough). A step that
    // is not finite and positive is treated as 1.0.
    //
    // IT CONSUMES THE WHOLE BUFFER. Anything it did not need is DROPPED, not carried — so the
    // caller must hand it exactly what InputFramesFor() asks for. Feeding it a couple of extra
    // frames per call (the obvious "+2 for the interpolator" reflex) silently drains the
    // producer faster than it fills, which reads as a slow leak into permanent underruns plus
    // a discontinuity click every buffer.
    std::size_t Process(const float* in, std::size_t inFrames, double step,
                        float* out, std::size_t maxOutFrames);

    // Exactly how many input frames are needed to produce `outFrames` output frames from the
    // current phase. Depends on the resampler's state, so call it immediately before Process.
    std::size_t InputFramesFor(std::size_t outFrames, double step) const;

private:
    double m_phase  = 0.0;    // fractional read position within the incoming buffer
    float  m_lastL  = 0.0f;   // last frame of the PREVIOUS buffer, to interpolate across the seam
    float  m_lastR  = 0.0f;
    bool   m_primed = false;
};

// ---------------------------------------------------------------------------------------
// Level meter: peak and RMS over whatever has been pushed since the last Take().
//
// THIS IS THE PROOF INSTRUMENT for the spike. Silence when nothing plays, signal during rain,
// and MORE signal when rain and a purr overlap, is the whole argument that the engine is
// mixing concurrent haptics for us. It must therefore be honest about the window it measured,
// which is why Take() reports the frame count alongside the levels: a reading over 3 frames
// is not a reading.
// ---------------------------------------------------------------------------------------
struct LevelReading
{
    float         peak   = 0.0f;
    float         rms    = 0.0f;
    std::uint64_t frames = 0;
};

class LevelMeter
{
public:
    void Push(const float* interleavedStereo, std::size_t frames);
    // Reads and resets. Called from a different thread than Push; the three values are read
    // and cleared independently, so a reading taken mid-callback can mix two windows by a
    // fraction of a buffer. That is acceptable for a diagnostic and is stated rather than
    // pretended away.
    LevelReading Take();

private:
    std::atomic<std::uint64_t> m_frames{0};
    std::atomic<std::uint64_t> m_sumSquaresBits{0};   // double, bit-cast
    std::atomic<std::uint32_t> m_peakBits{0};         // float, bit-cast; non-negative floats
                                                      // compare as their bit patterns do, so
                                                      // a CAS-max on the bits is a max on the
                                                      // values
};

// ---------------------------------------------------------------------------------------
// Single-producer single-consumer ring of interleaved stereo frames.
//
// The producer is THE ENGINE'S AUDIO RENDER THREAD. It must never block, never allocate and
// never take a lock: a stalled audio callback is a glitch in the game's own output, which is
// a far worse bug than a silent coil. So the ring drops rather than waits, and COUNTS what it
// dropped — an uncounted drop looks exactly like a tap that never fired, and this project has
// been burned by that before (CLAUDE.md, "build the counter before you need it").
//
// Capacity is rounded up to a power of two.
// ---------------------------------------------------------------------------------------
class SubmixRing
{
public:
    void Init(std::size_t capacityFrames);

    // Producer side. Overwrites the OLDEST frames when full — a haptic wants to be current,
    // not complete — and counts the loss.
    void Write(const float* interleavedStereo, std::size_t frames);

    // Consumer side. Fills `frames`; returns how many were real. The remainder is zeroed and
    // counted as an underrun.
    std::size_t Read(float* interleavedStereo, std::size_t frames);

    std::size_t   Available() const;
    std::size_t   CapacityFrames() const { return m_capacity; }
    std::uint64_t Written() const   { return m_written.load(std::memory_order_relaxed); }
    std::uint64_t Dropped() const   { return m_dropped.load(std::memory_order_relaxed); }
    std::uint64_t Underruns() const { return m_underruns.load(std::memory_order_relaxed); }
    void          ResetCounters();

private:
    std::vector<float>         m_buffer;        // 2 floats per frame
    std::size_t                m_capacity = 0;  // in frames, a power of two
    std::size_t                m_mask     = 0;
    std::atomic<std::uint64_t> m_writePos{0};
    std::atomic<std::uint64_t> m_readPos{0};
    std::atomic<std::uint64_t> m_written{0};
    std::atomic<std::uint64_t> m_dropped{0};
    std::atomic<std::uint64_t> m_underruns{0};
};

} // namespace submix
} // namespace sds
