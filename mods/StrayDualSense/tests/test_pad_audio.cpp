// The pad speaker claim. One wrong bit here is INVISIBLE until a human listens — and the
// most expensive wrong bit is not "the speaker stayed silent" but "the coils stopped
// working", which is what claiming a haptic or trigger validity bit does (§12, twice).
//
// So the first and last tests in this file are the same invariant from both ends: the audio
// claim contributes NOTHING to valid_flag0 bits 0..3, for every path/volume/preamp the
// config can produce.
#include "PadAudio.hpp"

#include <cstdio>
#include <string>

namespace {

int g_failures = 0;

void Check(bool ok, const char* what)
{
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failures;
}

} // namespace

int main()
{
    using namespace sds;

    // ---- THE INVARIANT: the claim can never disturb the coils or the triggers -----------
    {
        bool everTouched = false;
        for (int path = -3; path <= 6; ++path)
            for (int vol = -10; vol <= 300; vol += 7)
                for (int pre = -2; pre <= 12; ++pre)
                {
                    const PadAudioClaim c = BuildPadAudioClaim(true, path, vol, pre);
                    if ((c.flag0 & kFlag0HapticAndTriggerBits) != 0)
                        everTouched = true;
                    // And through the one place the two halves are combined: whatever the
                    // coil base is, the claim must not change bits 0..3 of the result.
                    for (unsigned base = 0; base < 16; ++base)
                    {
                        const std::uint8_t b   = static_cast<std::uint8_t>(base);
                        const std::uint8_t out = ComposeValidFlag0(b, c);
                        if ((out & kFlag0HapticAndTriggerBits) !=
                            (b & kFlag0HapticAndTriggerBits))
                            everTouched = true;
                    }
                }
        Check(!everTouched,
              "the audio claim NEVER sets valid_flag0 bits 0..3 (coil mode + trigger FFB), "
              "for any path/volume/preamp and any coil base");
    }

    // ComposeValidFlag0 must be a strict superset test: even a claim that has somehow been
    // handed the forbidden bits cannot smuggle them through.
    {
        PadAudioClaim hostile;
        hostile.claims = true;
        hostile.flag0  = 0xFF;
        Check(ComposeValidFlag0(0x00, hostile) == 0xF0,
              "a hostile claim of 0xFF is masked down to the audio/mic bits only");
        Check(ComposeValidFlag0(kFlag0CompatibleVibration, hostile) == (0xF0 | 0x01),
              "the coil base's own bit0 survives the mask");
    }

    // ---- disabled is a TRUE no-op ------------------------------------------------------
    {
        const PadAudioClaim off = BuildPadAudioClaim(false, 2, 0x64, 2);
        Check(!off.claims && off.flag0 == 0 && off.flag1 == 0 && off.speakerVolume == 0 &&
                  off.audioControl == 0 && off.audioControl2 == 0,
              "enable=false yields an all-zero claim: the report is byte-identical to before");
        Check(ComposeValidFlag0(0x00, off) == 0x00,
              "and valid_flag0 stays exactly the 0x00 that §12 measured working");
    }

    // ---- the working claim, byte for byte against the kernel's own values ---------------
    {
        const PadAudioClaim c = BuildPadAudioClaim(true, kPadSpeakerPathDefault,
                                                   kPadSpeakerVolumeDefault,
                                                   kPadSpeakerPreampDefault);
        Check(c.claims, "the default claim claims something");
        // hid-playstation.c:1391-1393 — valid_flag0 |= AUDIO_CONTROL_ENABLE|SPEAKER_VOLUME_ENABLE
        Check(c.flag0 == 0xA0, "valid_flag0 claim is BIT(7)|BIT(5) == 0xA0");
        // hid-playstation.c:1395-1396 — valid_flag1 = AUDIO_CONTROL2_ENABLE
        Check(c.flag1 == 0x80, "valid_flag1 claim is BIT(7) == 0x80");
        // FIELD_PREP(GENMASK(5,4), 2) == 0x20
        Check(c.audioControl == 0x20, "path 2 packs into audio_control bits 5:4 as 0x20");
        Check(c.speakerVolume == 0x64, "speaker_volume is the kernel's 0x64 (100%)");
        // FIELD_PREP(GENMASK(2,0), 2) == 0x02
        Check(c.audioControl2 == 0x02, "audio_control2 preamp gain is 0x02 (+6 dB)");
        Check(ComposeValidFlag0(0x00, c) == 0xA0,
              "with the shipped coil base 0x00 the report's valid_flag0 is 0xA0");
    }

    // The path the kernel itself picks when it knows there are no headphones.
    {
        const PadAudioClaim c = BuildPadAudioClaim(true, 3, 0x64, 2);
        Check(c.audioControl == 0x30, "path 3 packs as 0x30 (HP muted, speaker=R)");
    }

    // ---- clamps: a typo in the ini must not write nonsense to the hardware --------------
    {
        Check(BuildPadAudioClaim(true, 9, 0x64, 2).audioControl == 0x30,
              "path above 3 clamps to 3, never wraps into a neighbouring field");
        Check(BuildPadAudioClaim(true, -5, 0x64, 2).audioControl == 0x00,
              "negative path clamps to 0");
        Check(BuildPadAudioClaim(true, 2, 0xFF, 2).speakerVolume == kPadSpeakerVolumeMax,
              "volume clamps to the accepted maximum 0x64, not 0xFF");
        Check(BuildPadAudioClaim(true, 2, 0, 2).speakerVolume == kPadSpeakerVolumeMin,
              "volume clamps UP to 0x3d: below it the hardware is documented as not accepting "
              "the value, and a silent speaker is the bug we are fixing");
        Check(BuildPadAudioClaim(true, 2, 0x64, 99).audioControl2 == 0x07,
              "preamp clamps to the 3-bit field's maximum");
        Check(BuildPadAudioClaim(true, 2, 0x64, -1).audioControl2 == 0x00,
              "negative preamp clamps to 0");
    }

    // ---- shutdown restores the pad's default routing ------------------------------------
    {
        const PadAudioClaim m = MutePadAudioClaim();
        Check(m.claims && m.flag0 == kFlag0AudioControlEnable,
              "the mute claim claims ONLY the audio control byte");
        Check(m.audioControl == 0x00, "and selects path 0, the pad's own default");
        Check(m.flag1 == 0 && m.speakerVolume == 0 && m.audioControl2 == 0,
              "it re-claims neither the speaker volume nor the preamp: a claim whose data we "
              "no longer mean is the §12 mistake");
        Check((ComposeValidFlag0(kFlag0CompatibleVibration, m) & kFlag0CompatibleVibration) != 0,
              "shutdown still hands the coils back to rumble emulation");
    }

    // ---- the offsets the HID writer indexes ---------------------------------------------
    {
        Check(kReportOffValidFlag0 == 1 && kReportOffValidFlag1 == 2 &&
                  kReportOffSpeakerVolume == 6 && kReportOffAudioControl == 8 &&
                  kReportOffAudioControl2 == 38,
              "report offsets match struct dualsense_output_report_common with the report id "
              "at 0 (hid-playstation.c:270-299)");
        Check(kReportMinLenForAudio == 39,
              "39 bytes is the shortest report carrying every byte we set");
    }

    // ---- SONY'S API: the route selector and the escalation rule -------------------------
    {
        Check(ParsePadSpeakerRoute("sony", PadSpeakerRoute::Off) == PadSpeakerRoute::Sony &&
                  ParsePadSpeakerRoute("HID", PadSpeakerRoute::Off) == PadSpeakerRoute::Hid &&
                  ParsePadSpeakerRoute("Both", PadSpeakerRoute::Off) == PadSpeakerRoute::Both &&
                  ParsePadSpeakerRoute("auto", PadSpeakerRoute::Off) == PadSpeakerRoute::Auto &&
                  ParsePadSpeakerRoute("off", PadSpeakerRoute::Sony) == PadSpeakerRoute::Off,
              "PadSpeakerRoute parses case-insensitively");
        Check(ParsePadSpeakerRoute("banana", PadSpeakerRoute::Sony) == PadSpeakerRoute::Sony,
              "an unknown route keeps the fallback rather than silently disabling the speaker");
        Check(ParsePadSpeakerRoute(nullptr, PadSpeakerRoute::Auto) == PadSpeakerRoute::Auto,
              "a null route text keeps the fallback");

        Check(RouteUsesSony(PadSpeakerRoute::Sony) && RouteUsesSony(PadSpeakerRoute::Both) &&
                  RouteUsesSony(PadSpeakerRoute::Auto) &&
                  !RouteUsesSony(PadSpeakerRoute::Hid) && !RouteUsesSony(PadSpeakerRoute::Off),
              "sony/both/auto call Sony's API; hid and off do not");

        // THE POINT OF `auto`: the HID claim carries a coil risk that is argued, not measured,
        // so it must never run speculatively.
        Check(!RouteShouldWriteHid(PadSpeakerRoute::Auto, true, true),
              "auto: Sony succeeded -> the HID claim is NOT written, so the coils are never "
              "exposed to it in the working case");
        Check(RouteShouldWriteHid(PadSpeakerRoute::Auto, true, false),
              "auto: Sony was tried and failed -> escalate to the HID claim");
        Check(!RouteShouldWriteHid(PadSpeakerRoute::Auto, false, false),
              "auto: Sony not yet attempted -> nothing is written; escalation is never a guess");
        Check(RouteShouldWriteHid(PadSpeakerRoute::Hid, false, false) &&
                  RouteShouldWriteHid(PadSpeakerRoute::Both, true, true),
              "hid and both write the claim regardless of Sony's result");
        Check(!RouteShouldWriteHid(PadSpeakerRoute::Off, true, false) &&
                  !RouteShouldWriteHid(PadSpeakerRoute::Sony, true, false),
              "off and sony NEVER write the HID claim, even when Sony's API refuses");
    }

    // ---- Sony's path enum and volume-gain block, against the shim's measured values ------
    {
        Check(kSceAudioOutPathSpeaker == 3,
              "SPEAKER is 3 — the value libScePad_shim.c passed when the speaker worked");
        Check(std::string(SceAudioOutPathName(3)) == "SPEAKER" &&
                  std::string(SceAudioOutPathName(0)) == "STEREO_HEADSET" &&
                  std::string(SceAudioOutPathName(9)) == "UNKNOWN",
              "the path enum names itself, and an out-of-range value says UNKNOWN");

        const SceVolumeGain g = BuildSceVolumeGain(kSceVolumeGainDefault, kSceVolumeGainDefault, 0);
        Check(g.bytes[0] == 80 && g.bytes[1] == 80 && g.bytes[2] == 0 && g.bytes[3] == 0,
              "the gain block reproduces the shim's { 80, 80, 0, 0 } byte for byte");
        Check(sizeof(g.bytes) == 8,
              "the block is 8 bytes as the shim passed: larger than the struct is safe, "
              "smaller would let the callee read our stack");
        for (std::size_t i = 4; i < sizeof(g.bytes); ++i)
            Check(g.bytes[i] == 0, "the tail of the gain block is zeroed");
        Check(BuildSceVolumeGain(999, -5, 300).bytes[0] == 255 &&
                  BuildSceVolumeGain(999, -5, 300).bytes[1] == 0 &&
                  BuildSceVolumeGain(999, -5, 300).bytes[3] == 255,
              "gain values clamp to a byte instead of wrapping");
    }

    // ---- the log line must not lie about which row is selected --------------------------
    {
        Check(std::string(PadAudioPathName(0)).find("MUTED") != std::string::npos,
              "path 0 names itself as the muted default");
        Check(std::string(PadAudioPathName(2)).find("SPEAKER=R") != std::string::npos,
              "path 2 names the speaker as fed from R");
        Check(std::string(PadAudioPathName(7)) == "out of range",
              "an out-of-range path says so rather than naming a row");
    }

    std::printf("%s\n", g_failures == 0 ? "ALL OK" : "FAILURES");
    return g_failures == 0 ? 0 : 1;
}
