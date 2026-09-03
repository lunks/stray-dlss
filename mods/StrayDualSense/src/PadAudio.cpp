#include "PadAudio.hpp"

namespace sds {
namespace {

int Clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

} // namespace

const char* SceAudioOutPathName(int path)
{
    switch (path)
    {
    case kSceAudioOutPathStereoHeadset:      return "STEREO_HEADSET";
    case kSceAudioOutPathMonoHeadset:        return "MONO_HEADSET";
    case kSceAudioOutPathMonoHeadsetSpeaker: return "MONO_HEADSET_SPEAKER";
    case kSceAudioOutPathSpeaker:            return "SPEAKER";
    case kSceAudioOutPathOff:                return "OFF";
    default:                                 return "UNKNOWN";
    }
}

SceVolumeGain BuildSceVolumeGain(int speaker, int jack, int mic)
{
    SceVolumeGain g;
    g.bytes[0] = static_cast<std::uint8_t>(Clamp(speaker, 0, 255));   // speakerVolume
    g.bytes[1] = static_cast<std::uint8_t>(Clamp(jack, 0, 255));      // jackVolume
    g.bytes[2] = 0;                                                   // reserved
    g.bytes[3] = static_cast<std::uint8_t>(Clamp(mic, 0, 255));       // micGain
    return g;
}

} // namespace sds
