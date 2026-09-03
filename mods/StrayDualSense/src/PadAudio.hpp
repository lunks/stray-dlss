// StrayDualSense — telling the DualSense to route audio to its INTERNAL SPEAKER, the way
// Sony's own libScePad does it.
//
// THE PROBLEM THIS SOLVES (2026-09-03, docs/STRAY-DUALSENSE.md §16). The coils were confirmed
// working from the engine's own submix (peak 0.708) while the pad's speaker stayed silent
// from the SAME purr, with correct samples streamed into the pad's USB audio endpoint. The
// pad was routing them nowhere: THE COILS ARE A WAVEFORM PATH THAT NEEDS NOTHING CLAIMED,
// WHILE THE SPEAKER IS A ROUTING CHOICE THAT MUST BE CLAIMED EXPLICITLY. HidMode sends
// `valid_flag0 = 0x00` — claim nothing — because a report that re-asserts
// COMPATIBLE_VIBRATION puts the coils back into rumble emulation (§12). Claiming nothing also
// means never selecting an output path, and the pad's default routing MUTES the internal
// speaker.
//
// THE MEASURED-WORKING RECIPE is the retired libScePad shim's `audio_probe`
// (tools/dualsense/libScePad_shim.c, `audio_probe`, verbatim):
//
//     supp = resolve("scePadIsSupportedAudioFunction");   supp(h, 0, 0, 0)
//     path = resolve("scePadSetAudioOutPath");            path(h, 3, 0, 0)
//     gain = resolve("scePadSetVolumeGain");
//     unsigned char g[8] = { 80, 80, 0, 0, 0, 0, 0, 0 };  gain(h, &g, 0, 0)
//   with the shim's own comment for the path enum:
//     0 STEREO_HEADSET  1 MONO_HEADSET  2 MONO_HEADSET_SPEAKER  3 SPEAKER  4 OFF
//   and for the struct:  ScePadVolumeGain { u8 SpeakerVol, JackVol, Reserved, MicGain }
//
// The shim retired, nothing called those functions, and the speaker went silent. The plugin
// makes the same calls itself (Runtime::ApplySpeakerRoute, db83a32, user-confirmed on the
// pad): resolved out of the libScePad the game has ALREADY mapped and called with the handle
// scePadGetHandle already gives us. No proxy DLL, no scePadOpen interception, and Sony's dll
// stays the SINGLE writer of pad output reports — we ask it to change a setting rather than
// writing bytes beside it, so the §12 two-writers hazard never arises. A refusal is logged
// with its status code and that is the end of it; there is no fallback writer.
//
// One hardware fact worth keeping beside the enum: in every routing the Linux kernel's own
// DualSense driver documents (hid-playstation.c:1366-1377) the internal speaker is fed from
// the RIGHT channel and never from a mix. The sink writes the speaker lane to BOTH FL and FR
// (SubmixDsp.hpp) so that holds whatever the firmware reads.
//
// Pure: no Windows, no HID, no UE4SS.
#pragma once

#include <cstdint>

namespace sds {

// ScePadAudioOutPath, from the shim's own comment. SOFT: it is the reference implementation's
// reading of Sony's enum, not something read out of the DLL — but value 3 is what was
// measured driving the pad speaker on this exact hardware, which is the only evidence that
// matters here.
constexpr int kSceAudioOutPathStereoHeadset      = 0;
constexpr int kSceAudioOutPathMonoHeadset        = 1;
constexpr int kSceAudioOutPathMonoHeadsetSpeaker = 2;
constexpr int kSceAudioOutPathSpeaker            = 3;   // THE MEASURED-WORKING VALUE
constexpr int kSceAudioOutPathOff                = 4;
const char* SceAudioOutPathName(int path);

// ScePadVolumeGain { u8 speakerVolume, jackVolume, reserved, micGain }. The shim passed an
// 8-byte zeroed buffer with the first two bytes at 80 and that is reproduced exactly: the
// struct's true size is not known from any source we hold, and a buffer LARGER than the
// struct is safe where a smaller one would let the callee read our stack.
constexpr int kSceVolumeGainDefault = 80;   // the shim's measured value
struct SceVolumeGain
{
    std::uint8_t bytes[8]{};
};
// `speaker` and `jack` are clamped to 0..255; `mic` likewise. The shim set speaker and jack
// alike and left mic at 0.
SceVolumeGain BuildSceVolumeGain(int speaker, int jack, int mic);

} // namespace sds
