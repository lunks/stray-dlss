// StrayDualSense — telling the DualSense to route audio to its INTERNAL SPEAKER.
//
// THE PROBLEM THIS SOLVES (2026-09-03). The coils were confirmed working from the engine's
// own submix (peak 0.708) while the pad's speaker stayed silent, from the SAME purr, with our
// own counters reporting the speaker asset loaded, the endpoint open, zero failures and
// 3.7 MB of PCM streamed. We were writing correct samples into the pad's USB audio endpoint
// and the pad was routing them nowhere.
//
// The asymmetry has a cause, and it is structural: THE COILS ARE A WAVEFORM PATH THAT NEEDS
// NOTHING CLAIMED, WHILE THE SPEAKER IS A ROUTING CHOICE THAT MUST BE CLAIMED EXPLICITLY.
// HidMode deliberately sends `valid_flag0 = 0x00` — claim nothing — because a report that
// re-asserts COMPATIBLE_VIBRATION puts the coils back into rumble emulation (§12). Claiming
// nothing also means never selecting an output path, and the pad's default routing MUTES the
// internal speaker.
//
// =========================================================================================
// TWO WAYS TO SELECT THE ROUTE, AND SONY'S IS THE ONE THAT WAS MEASURED WORKING
// =========================================================================================
//
// THE REGRESSION, identified 2026-09-03 from the artefacts still in the game directory. Our
// retired `libScePad_shim.dll` called SONY'S OWN audio API, and the pad speaker worked:
//
//     tools/dualsense/libScePad_shim.c:477-503, `audio_probe`, verbatim
//         supp = resolve("scePadIsSupportedAudioFunction");   supp(h, 0, 0, 0)
//         path = resolve("scePadSetAudioOutPath");            path(h, 3, 0, 0)
//         gain = resolve("scePadSetVolumeGain");
//         unsigned char g[8] = { 80, 80, 0, 0, 0, 0, 0, 0 };  gain(h, &g, 0, 0)
//       with the shim's own comment for the path enum:
//         0 STEREO_HEADSET  1 MONO_HEADSET  2 MONO_HEADSET_SPEAKER  3 SPEAKER  4 OFF
//       and for the struct:  ScePadVolumeGain { u8 SpeakerVol, JackVol, Reserved, MicGain }
//
// `tools/dualsense/deploy-submix-spike.sh` retires the shim (`libScePad.dll` <-
// `libScePad_orig.dll`) — correctly, because two writers of `valid_flag0` is the §12 trap —
// and NOTHING has called those functions since. The pad fell back to its default routing,
// where the internal speaker is muted. That is the whole "it worked as-is and it is not
// working right now".
//
// So the PRIMARY route is `sony`: resolve those three exports out of the libScePad the game
// has ALREADY mapped and call them with the handle we already hold. Sony's implementation
// owns the report layout for this pad and this firmware; we do not, and §0.5 says a byte we
// inferred is not a byte we measured. Note this needs no proxy DLL and no `scePadOpen`
// interception: `ScePad::SelectPad` already adopts a live handle via `scePadGetHandle`
// (docs §11, "verified to return the same handles"), and the plugin is in the game's own
// process.
//
// The HID route below is the FALLBACK, for the world where Sony's API refuses. It is not
// hypothetical that it might: docs §7 measured `scePadSetAudioOutPath` returning
// `0x80920007` ("this pad has no audio") before the GE-Proton11-6 upgrade, because libScePad
// decides from the device tree and Wine exposed no USB audio siblings. If that comes back,
// `PadSpeakerRoute = hid` needs no rebuild.
//
// ---------------------------------------------------------------------------------------
// THE FALLBACK'S LAYOUT — HARD, read from the Linux kernel's own DualSense driver,
// drivers/hid/hid-playstation.c (torvalds/linux master, read 2026-09-03):
//   :153-168   the DS_OUTPUT_VALID_FLAG* / DS_OUTPUT_AUDIO_FLAGS* macros
//   :270-299   struct dualsense_output_report_common, with static_assert(sizeof == 47)
//   :311-316   struct dualsense_output_report_usb = report id + common + reserved[15] == 63
//   :1361-1401 ds_output_worker's headphone-detect branch: the working speaker claim
//
// THIS SUPERSEDES the bit list that used to sit in HidMode.hpp, which said "bit4/bit5 =
// audio volume / audio path". Both halves were wrong, and the second one mattered: THE PATH
// IS NOT A VALIDITY BIT AT ALL. Claiming bits 4/5 would have claimed headphone and speaker
// volume while supplying zeros, and never selected a path — the §12 two-writers trap exactly,
// with the speaker still silent and a plausible-looking log.
//
//   USB output report 0x02, offsets INCLUDING the report id at 0:
//     0   report id 0x02
//     1   valid_flag0
//     2   valid_flag1
//     3   motor_right          4  motor_left
//     5   headphone_volume     6  speaker_volume      7  mic_volume
//     8   audio_control        9  mute_button_led    10  power_save_control
//     11..37 reserved2[27]
//     38  audio_control2
//     39  valid_flag2   40..41 reserved3   42 lightbar_setup   43 led_brightness
//     44  player_leds   45/46/47 lightbar r/g/b
//   The whole USB report is 63 bytes (DS_OUTPUT_REPORT_USB_SIZE = report id + 47 + 15
//   trailing reserved). Everything used here fits inside the first 39 bytes, which is why
//   the 48-byte report HidMode writes — exactly report id + common — carries all of it.
//
//   valid_flag0:  BIT(0) COMPATIBLE_VIBRATION   BIT(1) HAPTICS_SELECT
//                 BIT(5) SPEAKER_VOLUME_ENABLE  BIT(6) MIC_VOLUME_ENABLE
//                 BIT(7) AUDIO_CONTROL_ENABLE
//                 (bits 2/3 are the trigger FFB fields libScePad drives — §12. The kernel
//                  does not name them because it never writes them; we must never claim them.)
//   valid_flag1:  BIT(7) AUDIO_CONTROL2_ENABLE  (BIT(2) lightbar, BIT(4) player LEDs, ...)
//   audio_control:  OUTPUT_PATH_SEL  = bits 5:4
//   audio_control2: SP_PREAMP_GAIN   = bits 2:0
//
// THE OUTPUT PATH TABLE, the kernel's own comment (hid-playstation.c:1366-1377):
//
//   path | headphone L | headphone R | internal speaker
//   -----+-------------+-------------+------------------
//     0  |      L      |      R      |   MUTED          <- the pad's default. Our problem.
//     1  |      L      |      L      |   MUTED
//     2  |      L      |      L      |   R
//     3  |    muted    |    muted    |   R
//
// TWO THINGS FOLLOW, and both are load-bearing:
//
//  * THE INTERNAL SPEAKER IS FED FROM THE RIGHT CHANNEL ONLY. There is no path that gives it
//    L or a mix. `kSpeakerRoute` writes the mono _CONTROL asset into BOTH FL and FR
//    (AudioPlayer.hpp), so the signal is already on R and nothing else has to change — but
//    "optimising" that route to FL alone would silence the pad speaker completely.
//  * PATH 2 IS THE SAFE DEFAULT, NOT 3. The kernel picks 3 when it detects no headphones,
//    but it KNOWS whether headphones are plugged in and we do not. Path 2 feeds the speaker
//    exactly as path 3 does and leaves the headphone sinks alive, so it wins in the case we
//    can see and cannot lose in the case we cannot.
//
// THE §12 RULE IS HONOURED EXACTLY: claim only the bits whose data this report actually
// carries. Nothing here ever sets BIT(0) (compatible vibration), BIT(1) (haptics select) or
// bits 2/3 (the trigger FFB fields) — so the coil and trigger behaviour measured working in
// the game on 2026-09-03 is untouched BY CONSTRUCTION, and that is what
// tests/test_pad_audio.cpp pins. The claim is a DISJOINT bit set, which is why re-asserting
// it on the same cadence is not the §12 hazard: two writers fighting over ONE bit is what
// killed the triggers twice; two writers owning different bits is how the report is designed
// to be used.
//
// Pure: no Windows, no HID, no UE4SS.
#pragma once

#include <cstddef>
#include <cstdint>

namespace sds {

// =========================================================================================
// SONY'S API — the primary route
// =========================================================================================

// Which mechanism selects the pad's audio routing.
enum class PadSpeakerRoute
{
    Off,    // nothing is written; the pad keeps whatever routing it has (today's behaviour)
    Sony,   // scePadSetAudioOutPath / scePadSetVolumeGain — what the shim did, measured working
    Hid,    // the raw output-report claim below — for when Sony's API refuses
    Both,   // Sony first, then the HID claim regardless of the result
    Auto,   // Sony first; escalate to the HID claim ONLY if Sony's call failed
};

PadSpeakerRoute ParsePadSpeakerRoute(const char* text, PadSpeakerRoute fallback);
const char*     PadSpeakerRouteName(PadSpeakerRoute route);
// Does this route call Sony's API / write the HID claim unconditionally?
bool RouteUsesSony(PadSpeakerRoute route);
bool RouteUsesHidAlways(PadSpeakerRoute route);
// After Sony's calls have been attempted, should the HID claim be written too?
// `sonyOk` is false when the API was missing or returned a failure code.
bool RouteShouldWriteHid(PadSpeakerRoute route, bool sonyAttempted, bool sonyOk);

// ScePadAudioOutPath, from the shim's own comment (libScePad_shim.c:489-491). SOFT: it is the
// reference implementation's reading of Sony's enum, not something read out of the DLL — but
// value 3 is what was measured driving the pad speaker on this exact hardware, which is the
// only evidence that matters here.
constexpr int kSceAudioOutPathStereoHeadset      = 0;
constexpr int kSceAudioOutPathMonoHeadset        = 1;
constexpr int kSceAudioOutPathMonoHeadsetSpeaker = 2;
constexpr int kSceAudioOutPathSpeaker            = 3;   // THE MEASURED-WORKING VALUE
constexpr int kSceAudioOutPathOff                = 4;
const char* SceAudioOutPathName(int path);

// ScePadVolumeGain { u8 speakerVolume, jackVolume, reserved, micGain } (libScePad_shim.c:495).
// The shim passed an 8-byte zeroed buffer with the first two bytes at 80 and that is
// reproduced exactly: the struct's true size is not known from any source we hold, and a
// buffer LARGER than the struct is safe where a smaller one would let the callee read our
// stack.
constexpr int kSceVolumeGainDefault = 80;   // the shim's measured value
struct SceVolumeGain
{
    std::uint8_t bytes[8]{};
};
// `speaker` and `jack` are clamped to 0..255; `mic` likewise. The shim set speaker and jack
// alike and left mic at 0.
SceVolumeGain BuildSceVolumeGain(int speaker, int jack, int mic);

// =========================================================================================
// THE HID FALLBACK
// =========================================================================================

// valid_flag0 bits — HARD, hid-playstation.c:154-158.
constexpr std::uint8_t kFlag0CompatibleVibration = 1u << 0;
constexpr std::uint8_t kFlag0HapticsSelect       = 1u << 1;
constexpr std::uint8_t kFlag0RightTriggerFfb     = 1u << 2;   // §12; not named by the kernel
constexpr std::uint8_t kFlag0LeftTriggerFfb      = 1u << 3;   // §12; not named by the kernel
constexpr std::uint8_t kFlag0SpeakerVolumeEnable = 1u << 5;
constexpr std::uint8_t kFlag0MicVolumeEnable     = 1u << 6;
constexpr std::uint8_t kFlag0AudioControlEnable  = 1u << 7;
// valid_flag1 — HARD, hid-playstation.c:159-164.
constexpr std::uint8_t kFlag1AudioControl2Enable = 1u << 7;

// The bits this module must NEVER touch: the coil mode and the two trigger FFB fields.
// Claiming any of them is the failure that killed the adaptive triggers twice (§12).
constexpr std::uint8_t kFlag0HapticAndTriggerBits =
    kFlag0CompatibleVibration | kFlag0HapticsSelect | kFlag0RightTriggerFfb |
    kFlag0LeftTriggerFfb;

// Byte offsets in the USB report INCLUDING the report id at 0. Named so a wrong index is a
// visible mistake rather than a magic number into a zeroed buffer.
constexpr std::size_t kReportOffValidFlag0    = 1;
constexpr std::size_t kReportOffValidFlag1    = 2;
constexpr std::size_t kReportOffSpeakerVolume = 6;
constexpr std::size_t kReportOffAudioControl  = 8;
constexpr std::size_t kReportOffAudioControl2 = 38;
// The shortest report that carries every byte above.
constexpr std::size_t kReportMinLenForAudio = kReportOffAudioControl2 + 1;

// The kernel's own choice for "no headphones": speaker volume 0x64, with its comment
// "the accepted range seems to be [0x3d..0x64]"; SP preamp gain 0x2 == +6 dB
// (hid-playstation.c:1383-1400).
constexpr int kPadSpeakerVolumeMin     = 0x3d;
constexpr int kPadSpeakerVolumeMax     = 0x64;
constexpr int kPadSpeakerVolumeDefault = 0x64;
constexpr int kPadSpeakerPreampDefault = 0x2;
constexpr int kPadSpeakerPreampMax     = 0x7;   // SP_PREAMP_GAIN is GENMASK(2,0)
constexpr int kPadSpeakerPathDefault   = 2;     // see the table above: speaker on, HP alive
constexpr int kPadSpeakerPathMuted     = 0;     // the pad's default: HP L/R, speaker muted

// What one output report should carry on top of whatever HidMode already puts in it.
struct PadAudioClaim
{
    std::uint8_t flag0         = 0;   // OR'd into valid_flag0 (offset 1)
    std::uint8_t flag1         = 0;   // OR'd into valid_flag1 (offset 2)
    std::uint8_t speakerVolume = 0;   // offset 6
    std::uint8_t audioControl  = 0;   // offset 8
    std::uint8_t audioControl2 = 0;   // offset 38
    bool         claims        = false;
};

// `enable` false returns an all-zero claim: the report is byte-identical to what HidMode
// wrote before this existed, so the feature is a true no-op when off.
//
// `path` selects the routing table above and is clamped to 0..3; `volume` is clamped to the
// hardware's accepted [0x3d..0x64]; `preamp` to 0..7.
PadAudioClaim BuildPadAudioClaim(bool enable, int path, int volume, int preamp);

// The claim that puts the pad back to its default routing (path 0, speaker muted) at
// shutdown. Only the path is being restored, so only AUDIO_CONTROL_ENABLE is claimed:
// re-claiming the speaker volume here would be the §12 mistake in miniature — a claim whose
// data we no longer mean.
PadAudioClaim MutePadAudioClaim();

// The final valid_flag0 byte for one report: the coil-mode base OR the audio claim. This is
// the ONLY place the two are combined, so the "the claim never contributes bits 0..3"
// invariant has exactly one site to test.
std::uint8_t ComposeValidFlag0(std::uint8_t coilBase, const PadAudioClaim& claim);

// The routing row as English, for the one log line that must make a wrong path obvious.
const char* PadAudioPathName(int path);

} // namespace sds
