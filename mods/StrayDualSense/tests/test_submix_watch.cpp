// The instrument that would have answered 2026-09-03's question in one line.
//
// The failure it exists to prevent: `peak=0.00000` on a one-second window, pasted on its own,
// was read as "the submix delivers nothing" on a session where `live=1` proved the tap had
// carried a real signal. A per-window peak cannot answer "was the submix silent WHILE THE
// GAME WAS ASKING FOR A VIBRATION"; a watch opened by the start can.
#include "SubmixWatch.hpp"

#include <cmath>
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
    using sds::SubmixWatch;
    using sds::WatchVerdict;

    constexpr float kThreshold = 1.0e-4f;

    // ---- the NEGATIVE verdict: the game asked, the submix stayed silent ------------------
    {
        SubmixWatch  w;
        WatchVerdict closed;
        Check(!w.Start("Scratch_VIBE", 1000, 3000, kThreshold, closed),
              "the first start closes nothing");
        Check(w.Open(), "the watch is open");
        w.Sample(0.0f);
        w.Sample(0.0f);
        WatchVerdict out;
        Check(!w.Poll(3999, out), "the watch does not close before its window elapses");
        w.Sample(0.0f);
        Check(w.Poll(4000, out), "the watch closes at exactly startMs + windowMs");
        Check(!out.carried, "silence is NOT carried");
        Check(out.asset == "Scratch_VIBE", "the verdict names the asset the game asked for");
        Check(out.windows == 3, "every sampled window is counted");
        Check(out.ms == 3000, "the verdict reports the window it measured");
        Check(!w.Open(), "the watch is closed afterwards");
        Check(!w.Poll(9000, out), "a closed watch never reports twice");
    }

    // ---- the POSITIVE verdict, at the measured level -------------------------------------
    {
        SubmixWatch  w;
        WatchVerdict closed, out;
        w.Start("Scratch_VIBE", 0, 3000, kThreshold, closed);
        w.Sample(0.0f);
        w.Sample(0.70795f);   // MEASURED 2026-09-03, run A, docs/STRAY-DUALSENSE.md §14
        w.Sample(0.0f);
        Check(w.Poll(3000, out), "the watch closes");
        Check(out.carried, "a real signal IS carried");
        Check(std::fabs(out.peak - 0.70795f) < 1e-6f, "the verdict reports the PEAK, not the last sample");
    }

    // ---- the threshold is a boundary, and it is inclusive ---------------------------------
    {
        SubmixWatch  w;
        WatchVerdict closed, out;
        w.Start("x", 0, 100, kThreshold, closed);
        w.Sample(kThreshold);
        Check(w.Poll(100, out) && out.carried, "peak == threshold counts as carried");

        w.Start("y", 0, 100, kThreshold, closed);
        w.Sample(kThreshold * 0.5f);
        Check(w.Poll(100, out) && !out.carried, "peak below the threshold does not");
    }

    // ---- a NaN reading can never fabricate a positive verdict -----------------------------
    {
        SubmixWatch  w;
        WatchVerdict closed, out;
        w.Start("nan", 0, 100, kThreshold, closed);
        w.Sample(std::nanf(""));
        Check(w.Poll(100, out) && !out.carried, "NaN is not a signal");
    }

    // ---- a start while one is in flight reports the one it replaced -----------------------
    // The purr and the scratch can overlap; swallowing the first would lose exactly the
    // measurement the overlap makes interesting.
    {
        SubmixWatch  w;
        WatchVerdict closed, out;
        w.Start("CatPurr2_VIBE", 0, 3000, kThreshold, closed);
        w.Sample(0.5f);
        Check(w.Start("Scratch_VIBE", 1000, 3000, kThreshold, closed),
              "a start during an open watch closes it");
        Check(closed.asset == "CatPurr2_VIBE" && closed.carried, "the replaced watch is reported");
        Check(closed.ms == 1000, "the replaced watch reports the time it actually got");
        Check(w.Open(), "the new watch is open");
        w.Sample(0.0f);
        Check(w.Poll(4000, out), "the new watch closes on its OWN deadline");
        Check(out.asset == "Scratch_VIBE" && !out.carried && out.peak == 0.0f,
              "the new watch starts from zero, not from the replaced one's peak");
    }

    // ---- samples outside a watch are dropped, not banked ----------------------------------
    {
        SubmixWatch  w;
        WatchVerdict closed, out;
        w.Sample(1.0f);
        w.Start("z", 0, 100, kThreshold, closed);
        Check(w.Poll(100, out) && !out.carried && out.windows == 0,
              "a sample taken before the watch opened is not counted");
    }

    if (g_failures != 0)
    {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("all submix-watch checks passed\n");
    return 0;
}
