#include "PadAudio.hpp"

#include <cstring>

namespace sds {
namespace {

int Clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

bool EqualsIgnoringCase(const char* a, const char* b)
{
    if (a == nullptr || b == nullptr)
        return false;
    for (; *a != '\0' && *b != '\0'; ++a, ++b)
    {
        const char ca = (*a >= 'A' && *a <= 'Z') ? static_cast<char>(*a - 'A' + 'a') : *a;
        const char cb = (*b >= 'A' && *b <= 'Z') ? static_cast<char>(*b - 'A' + 'a') : *b;
        if (ca != cb)
            return false;
    }
    return *a == '\0' && *b == '\0';
}

// audio_control: OUTPUT_PATH_SEL is bits 5:4 (GENMASK(5,4)), so FIELD_PREP is `value << 4`.
std::uint8_t PackPath(int path)
{
    return static_cast<std::uint8_t>((static_cast<unsigned>(Clamp(path, 0, 3)) & 0x3u) << 4);
}

// audio_control2: SP_PREAMP_GAIN is bits 2:0 (GENMASK(2,0)), so FIELD_PREP is the value.
std::uint8_t PackPreamp(int preamp)
{
    return static_cast<std::uint8_t>(
        static_cast<unsigned>(Clamp(preamp, 0, kPadSpeakerPreampMax)) & 0x7u);
}

} // namespace

// ---- Sony's API -------------------------------------------------------------------------

PadSpeakerRoute ParsePadSpeakerRoute(const char* text, PadSpeakerRoute fallback)
{
    if (EqualsIgnoringCase(text, "off") || EqualsIgnoringCase(text, "0") ||
        EqualsIgnoringCase(text, "none"))
        return PadSpeakerRoute::Off;
    if (EqualsIgnoringCase(text, "sony"))  return PadSpeakerRoute::Sony;
    if (EqualsIgnoringCase(text, "hid"))   return PadSpeakerRoute::Hid;
    if (EqualsIgnoringCase(text, "both"))  return PadSpeakerRoute::Both;
    if (EqualsIgnoringCase(text, "auto"))  return PadSpeakerRoute::Auto;
    return fallback;
}

const char* PadSpeakerRouteName(PadSpeakerRoute route)
{
    switch (route)
    {
    case PadSpeakerRoute::Off:  return "off";
    case PadSpeakerRoute::Sony: return "sony";
    case PadSpeakerRoute::Hid:  return "hid";
    case PadSpeakerRoute::Both: return "both";
    case PadSpeakerRoute::Auto: return "auto";
    }
    return "?";
}

bool RouteUsesSony(PadSpeakerRoute route)
{
    return route == PadSpeakerRoute::Sony || route == PadSpeakerRoute::Both ||
           route == PadSpeakerRoute::Auto;
}

bool RouteUsesHidAlways(PadSpeakerRoute route)
{
    return route == PadSpeakerRoute::Hid || route == PadSpeakerRoute::Both;
}

bool RouteShouldWriteHid(PadSpeakerRoute route, bool sonyAttempted, bool sonyOk)
{
    if (RouteUsesHidAlways(route))
        return true;
    // `auto` is the whole reason the escalation is a pure function: the HID claim carries a
    // coil risk that is argued rather than measured, so it must run ONLY in the world where
    // Sony's API has already been tried and failed — never speculatively, and never before.
    if (route == PadSpeakerRoute::Auto)
        return sonyAttempted && !sonyOk;
    return false;
}

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

// ---- the HID fallback --------------------------------------------------------------------

PadAudioClaim BuildPadAudioClaim(bool enable, int path, int volume, int preamp)
{
    PadAudioClaim c;
    if (!enable)
        return c;
    c.claims        = true;
    c.flag0         = kFlag0AudioControlEnable | kFlag0SpeakerVolumeEnable;
    c.flag1         = kFlag1AudioControl2Enable;
    c.audioControl  = PackPath(path);
    c.speakerVolume = static_cast<std::uint8_t>(
        Clamp(volume, kPadSpeakerVolumeMin, kPadSpeakerVolumeMax));
    c.audioControl2 = PackPreamp(preamp);
    return c;
}

PadAudioClaim MutePadAudioClaim()
{
    PadAudioClaim c;
    c.claims       = true;
    c.flag0        = kFlag0AudioControlEnable;
    c.audioControl = PackPath(kPadSpeakerPathMuted);
    return c;
}

std::uint8_t ComposeValidFlag0(std::uint8_t coilBase, const PadAudioClaim& claim)
{
    // The mask is not defensive tidiness: it is the invariant. Whatever a future caller puts
    // in `claim.flag0`, the coil and trigger bits can only ever come from `coilBase`, which
    // is the config's own HapticValidFlag0 and is the §12 measurement.
    const std::uint8_t audioOnly =
        static_cast<std::uint8_t>(claim.flag0 & static_cast<std::uint8_t>(~kFlag0HapticAndTriggerBits));
    return static_cast<std::uint8_t>(coilBase | audioOnly);
}

const char* PadAudioPathName(int path)
{
    switch (path)
    {
    case 0:  return "HP=L/R, SPEAKER MUTED (the pad's default - this is why it was silent)";
    case 1:  return "HP=L/L, SPEAKER MUTED";
    case 2:  return "HP=L/L, SPEAKER=R (speaker on, headphones still alive)";
    case 3:  return "HP MUTED, SPEAKER=R";
    default: return "out of range";
    }
}

} // namespace sds
