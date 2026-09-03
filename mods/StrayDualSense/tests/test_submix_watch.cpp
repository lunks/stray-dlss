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
        w.Sample(0.0f, 1024);
        w.Sample(0.0f, 1024);
        WatchVerdict out;
        Check(!w.Poll(3999, out), "the watch does not close before its window elapses");
        w.Sample(0.0f, 1024);
        Check(w.Poll(4000, out), "the watch closes at exactly startMs + windowMs");
        Check(!out.Carried(), "silence is NOT carried");
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
        w.Sample(0.0f, 1024);
        w.Sample(0.70795f, 1024);   // MEASURED 2026-09-03, run A, docs/STRAY-DUALSENSE.md §14
        w.Sample(0.0f, 1024);
        Check(w.Poll(3000, out), "the watch closes");
        Check(out.Carried(), "a real signal IS carried");
        Check(std::fabs(out.peak - 0.70795f) < 1e-6f, "the verdict reports the PEAK, not the last sample");
    }

    // ---- the threshold is a boundary, and it is inclusive ---------------------------------
    {
        SubmixWatch  w;
        WatchVerdict closed, out;
        w.Start("x", 0, 100, kThreshold, closed);
        w.Sample(kThreshold, 1024);
        Check(w.Poll(100, out) && out.Carried(), "peak == threshold counts as carried");

        w.Start("y", 0, 100, kThreshold, closed);
        w.Sample(kThreshold * 0.5f, 1024);
        Check(w.Poll(100, out) && !out.Carried(), "peak below the threshold does not");
    }

    // ---- a NaN reading can never fabricate a positive verdict -----------------------------
    {
        SubmixWatch  w;
        WatchVerdict closed, out;
        w.Start("nan", 0, 100, kThreshold, closed);
        w.Sample(std::nanf(""), 1024);
        Check(w.Poll(100, out) && !out.Carried(), "NaN is not a signal");
    }

    // ---- a start while one is in flight reports the one it replaced -----------------------
    // The purr and the scratch can overlap; swallowing the first would lose exactly the
    // measurement the overlap makes interesting.
    {
        SubmixWatch  w;
        WatchVerdict closed, out;
        w.Start("CatPurr2_VIBE", 0, 3000, kThreshold, closed);
        w.Sample(0.5f, 1024);
        Check(w.Start("Scratch_VIBE", 1000, 3000, kThreshold, closed),
              "a start during an open watch closes it");
        Check(closed.asset == "CatPurr2_VIBE" && closed.Carried(), "the replaced watch is reported");
        Check(closed.ms == 1000, "the replaced watch reports the time it actually got");
        Check(w.Open(), "the new watch is open");
        w.Sample(0.0f, 1024);
        Check(w.Poll(4000, out), "the new watch closes on its OWN deadline");
        Check(out.asset == "Scratch_VIBE" && !out.Carried() && out.peak == 0.0f,
              "the new watch starts from zero, not from the replaced one's peak");
    }

    // ---- samples outside a watch are dropped, not banked ----------------------------------
    {
        SubmixWatch  w;
        WatchVerdict closed, out;
        w.Sample(1.0f, 1024);
        w.Start("z", 0, 100, kThreshold, closed);
        Check(w.Poll(100, out) && !out.Carried() && out.windows == 0,
              "a sample taken before the watch opened is not counted");
    }

    // ---- THE THIRD STATE: no data is not silence -----------------------------------------
    //
    // This is the bug the 2026-09-03 03_Slums session hit. The watch reported "the engine
    // mixed NOTHING - peak 0.00000 over 1.2s (0 window(s))" and it was read as a mixing
    // failure. It was not a measurement of the mixer at all: the tap delivered no frames,
    // because the subtree had stopped being rendered. Conflating the two sent the whole
    // diagnosis upstream to the Blueprint gate, which was working fine.
    {
        SubmixWatch  w;
        WatchVerdict out, closed;
        w.Start("generic_hit_01_VIBE", 0, 1200, kThreshold, closed);
        Check(w.Poll(1200, out), "a watch with no samples at all still closes");
        Check(out.result == sds::WatchResult::NoData,
              "NO SAMPLES is NoData, never Silent - it says nothing about the mixer");
        Check(!out.Carried(), "and NoData is not 'carried'");
        Check(out.frames == 0, "NoData reports zero frames");
    }
    // The realistic shape: the status tick DID run, and the tap handed it nothing each time.
    {
        SubmixWatch  w;
        WatchVerdict out, closed;
        w.Start("CatPurr2_VIBE", 0, 3000, kThreshold, closed);
        w.Sample(0.0f, 0);
        w.Sample(0.0f, 0);
        w.Sample(0.0f, 0);
        Check(w.Poll(3000, out), "closes");
        Check(out.result == sds::WatchResult::NoData && out.windows == 3 && out.frames == 0,
              "three FRAMELESS windows are still NoData - the windows are counted so 'we were "
              "looking and got nothing' stays visible, but the verdict is not about mixing");
    }
    // A frameless window must not be able to launder a stale meter reading into a signal.
    {
        SubmixWatch  w;
        WatchVerdict out, closed;
        w.Start("Scratch_VIBE", 0, 1000, kThreshold, closed);
        w.Sample(0.9f, 0);       // a peak with no frames behind it: whatever the meter held
        Check(w.Poll(1000, out), "closes");
        Check(out.result == sds::WatchResult::NoData && out.peak == 0.0f,
              "a frameless window cannot contribute a peak, so it cannot fabricate a signal");
    }
    // Mixed windows: some frames, some not. Only the ones with frames count toward the peak.
    {
        SubmixWatch  w;
        WatchVerdict out, closed;
        w.Start("Rain_Loop_VIBE", 0, 3000, kThreshold, closed);
        w.Sample(0.9f, 0);          // ignored for the peak
        w.Sample(0.25548f, 1024);   // MEASURED: the handover peak, docs §15
        Check(w.Poll(3000, out), "closes");
        Check(out.result == sds::WatchResult::Mixed && out.peak == 0.25548f && out.frames == 1024,
              "one real window among frameless ones still yields Mixed, at ITS peak");
    }
    // And the real negative survives: frames arrived, they were quiet.
    {
        SubmixWatch  w;
        WatchVerdict out, closed;
        w.Start("generic_hit_02_VIBE", 0, 1500, kThreshold, closed);
        w.Sample(0.0f, 2048);
        Check(w.Poll(1500, out), "closes");
        Check(out.result == sds::WatchResult::Silent && out.frames == 2048,
              "frames delivered and quiet IS Silent - the one verdict that licenses "
              "'the engine mixed nothing'");
    }

    if (g_failures != 0)
    {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("all submix-watch checks passed\n");
    return 0;
}
