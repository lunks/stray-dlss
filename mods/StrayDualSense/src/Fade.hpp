// StrayDualSense — fade ramps.
//
// StartPS5Vibration carries FadeInTime (the purr's is 1.0 s) and StopPS5Vibration carries
// FadeOutTime. libScePad has no fade API, but on the coil path WE generate the samples, so a
// fade is nothing more than a gain ramp applied while filling the buffer. Linear, in frames.
//
// Pure and portable.
#pragma once

#include <cstdint>

namespace sds {

// The longest fade honoured. A Blueprint could pass anything; 10 s is generous for a
// controller effect and bounds a bad read.
constexpr float kMaxFadeSeconds = 10.0f;

constexpr uint64_t FadeFrames(float seconds, uint32_t sampleRate)
{
    if (!(seconds > 0.0f)) return 0;        // also rejects NaN
    if (seconds > kMaxFadeSeconds) seconds = kMaxFadeSeconds;
    return static_cast<uint64_t>(seconds * static_cast<float>(sampleRate));
}

// 0 -> 1 over fadeFrames. A zero-length fade is unity from the first frame.
constexpr float FadeInGain(uint64_t framesIn, uint64_t fadeFrames)
{
    if (fadeFrames == 0 || framesIn >= fadeFrames) return 1.0f;
    return static_cast<float>(framesIn) / static_cast<float>(fadeFrames);
}

// 1 -> 0 over fadeFrames. A zero-length fade is silence from the first frame.
constexpr float FadeOutGain(uint64_t framesOut, uint64_t fadeFrames)
{
    if (fadeFrames == 0 || framesOut >= fadeFrames) return 0.0f;
    return 1.0f - static_cast<float>(framesOut) / static_cast<float>(fadeFrames);
}

} // namespace sds
