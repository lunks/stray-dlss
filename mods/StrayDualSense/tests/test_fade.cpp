#include "Fade.hpp"

#include <cmath>
#include <cstdio>

namespace {

int g_failures = 0;

void Check(bool ok, const char* what)
{
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failures;
}

bool Near(float a, float b) { return std::fabs(a - b) < 1e-5f; }

} // namespace

int main()
{
    using sds::FadeFrames;
    using sds::FadeInGain;
    using sds::FadeOutGain;

    Check(FadeFrames(1.0f, 48000) == 48000,      "1 s at 48 kHz is 48000 frames (the purr's fade-in)");
    Check(FadeFrames(0.0f, 48000) == 0,          "0 s is no fade");
    Check(FadeFrames(-3.0f, 48000) == 0,         "negative is no fade");
    Check(FadeFrames(NAN, 48000) == 0,           "NaN is no fade");
    Check(FadeFrames(1000.0f, 48000) == 480000,  "clamped to 10 s");

    Check(Near(FadeInGain(0, 0), 1.0f),          "no fade-in: unity at frame 0");
    Check(Near(FadeInGain(0, 100), 0.0f),        "fade-in starts silent");
    Check(Near(FadeInGain(50, 100), 0.5f),       "fade-in midpoint");
    Check(Near(FadeInGain(100, 100), 1.0f),      "fade-in reaches unity");
    Check(Near(FadeInGain(5000, 100), 1.0f),     "fade-in stays at unity");

    Check(Near(FadeOutGain(0, 0), 0.0f),         "no fade-out: silent at once");
    Check(Near(FadeOutGain(0, 100), 1.0f),       "fade-out starts at unity");
    Check(Near(FadeOutGain(25, 100), 0.75f),     "fade-out quarter");
    Check(Near(FadeOutGain(100, 100), 0.0f),     "fade-out reaches silence");
    Check(Near(FadeOutGain(999, 100), 0.0f),     "fade-out stays silent");

    std::printf(g_failures == 0 ? "\nall Fade cases passed\n" : "\n%d FAILURE(S)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
