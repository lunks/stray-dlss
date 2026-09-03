// Sony's speaker-routing call, byte for byte against the retired shim's measured-working
// values (tools/dualsense/libScePad_shim.c, audio_probe). A wrong byte here is INVISIBLE
// until a human listens, so the values that were seen to work are pinned.
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

    // ---- Sony's path enum, against the shim's measured value ---------------------------
    {
        Check(kSceAudioOutPathSpeaker == 3,
              "SPEAKER is 3 - the value libScePad_shim.c passed when the speaker worked");
        Check(std::string(SceAudioOutPathName(3)) == "SPEAKER" &&
                  std::string(SceAudioOutPathName(0)) == "STEREO_HEADSET" &&
                  std::string(SceAudioOutPathName(4)) == "OFF" &&
                  std::string(SceAudioOutPathName(9)) == "UNKNOWN",
              "the path enum names itself, and an out-of-range value says UNKNOWN");
    }

    // ---- the volume-gain block, byte for byte -------------------------------------------
    {
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
        Check(BuildSceVolumeGain(10, 20, 30).bytes[2] == 0,
              "the reserved byte is never written");
    }

    std::printf("%s\n", g_failures == 0 ? "ALL OK" : "FAILURES");
    return g_failures == 0 ? 0 : 1;
}
