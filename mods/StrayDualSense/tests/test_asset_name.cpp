// The only piece of this mod that can be proven without the game, the pad or Windows.
// Everything else in StrayDualSense is I/O against hardware we cannot reach from CI, so this
// is deliberately small — but a wrong asset name is a silent "nothing happened", and that is
// the failure mode this project spends the most time chasing.
#include "AssetName.hpp"

#include <cstdio>
#include <string>

namespace {

int g_failures = 0;

void Expect(const char* input, const char* expected)
{
    const std::string got = sds::ShortAssetName(input);
    if (got != expected)
    {
        std::printf("FAIL  ShortAssetName(\"%s\") = \"%s\", expected \"%s\"\n",
                    input, got.c_str(), expected);
        ++g_failures;
    }
    else
    {
        std::printf("ok    \"%s\" -> \"%s\"\n", input, got.c_str());
    }
}

} // namespace

int main()
{
    // The shape UE4SS's GetFullName() produces, and the one the working Lua mod parsed.
    Expect("SoundWave /Game/Sound/SFX/controllers/Vibrations/CatPurr2_VIBE.CatPurr2_VIBE",
           "CatPurr2_VIBE");
    Expect("SoundWave /Game/Sound/SFX/controllers/Vibrations/Scratch_VIBE.Scratch_VIBE",
           "Scratch_VIBE");
    // MEASURED: scratching plays Scratch_VIBE (peak 217), NOT CatScratch_VIBE (peak 7).
    Expect("SoundWave /Game/Sound/SFX/controllers/Vibrations/CatScratch_VIBE.CatScratch_VIBE",
           "CatScratch_VIBE");
    // The four _CONTROL speaker assets.
    Expect("SoundWave /Game/Sound/SFX/controllers/sounds/cat_purr_loop_01_CONTROL."
           "cat_purr_loop_01_CONTROL", "cat_purr_loop_01_CONTROL");
    Expect("SoundWave /Game/Sound/SFX/controllers/sounds/zurg_sucking_loop_02_CONTROL."
           "zurg_sucking_loop_02_CONTROL", "zurg_sucking_loop_02_CONTROL");

    // A bare object name, which is what GetName() returns.
    Expect("CatPurr2_VIBE", "CatPurr2_VIBE");
    // A path with no dotted suffix.
    Expect("/Game/Sound/SFX/controllers/Vibrations/Rain_Loop_VIBE", "Rain_Loop_VIBE");
    // Trailing punctuation must not end up in a filename.
    Expect("Foo.Bar_VIBE'", "Bar_VIBE");

    // Nothing usable: the caller MUST treat "" as an error rather than opening "<dir>/.env".
    Expect("", "");
    Expect("...", "");
    Expect("/", "");

    if (g_failures == 0)
        std::printf("\nall ShortAssetName cases passed\n");
    else
        std::printf("\n%d FAILURE(S)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
