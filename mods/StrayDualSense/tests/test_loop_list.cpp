// The loop list is the game's own bLooping flag. A parse that trims wrongly turns a
// looping purr into a one-shot or a one-shot bump into a buzz that never ends.
#include "LoopList.hpp"

#include <cstdio>

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
    sds::LoopList list;
    Check(!list.Loaded() && list.Count() == 0, "fresh list is empty and not loaded");
    Check(!list.Contains("CatPurr2_VIBE"), "fresh list contains nothing");

    // The shape wavegen.sh writes, with CRLF endings as a Windows editor would leave them,
    // a trailing space, a blank line, a comment and no final newline.
    list.Parse("CatPurr2_VIBE\r\nRain_Loop_VIBE \r\n\r\n# comment\nScratch_VIBE");
    Check(list.Loaded(), "Parse marks the list loaded");
    Check(list.Count() == 3, "three names parsed");
    Check(list.Contains("CatPurr2_VIBE"),  "CRLF line");
    Check(list.Contains("Rain_Loop_VIBE"), "trailing space trimmed");
    Check(list.Contains("Scratch_VIBE"),   "last line without newline");
    Check(!list.Contains("# comment"),     "comment skipped");
    Check(!list.Contains(""),              "empty name never matches");
    Check(!list.Contains("CatScratch_VIBE"), "a one-shot is not a loop");
    Check(!list.Contains("catpurr2_vibe"),   "match is exact, not case-folded");

    // Re-parsing replaces.
    list.Parse("");
    Check(list.Loaded() && list.Count() == 0, "empty text yields an empty, loaded list");

    std::printf(g_failures == 0 ? "\nall LoopList cases passed\n" : "\n%d FAILURE(S)\n",
                g_failures);
    return g_failures == 0 ? 0 : 1;
}
