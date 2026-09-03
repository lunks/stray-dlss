// The instrument that would have answered 2026-09-03's question in one line.
//
// The failure it exists to prevent: `peak=0.00000` on a one-second window, pasted on its own,
// was read as "the submix delivers nothing" on a session where `live=1` proved the tap had
// carried a real signal. A per-window peak cannot answer "was the submix silent WHILE THE
// GAME WAS ASKING FOR A VIBRATION"; a watch opened by the start can.
#include "SubmixWatch.hpp"

#include <cmath>
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

    // ======================================================================================
    // THE REROUTE WATCHDOG over N lanes. MEASURED shape (docs §17): a lane that rendered at
    // ~47 callbacks/s in BaseMap and read zero new callbacks for the rest of the session in
    // 03_Slums. One re-arm re-submits the reroute for EVERY lane.
    // ======================================================================================
    using sds::StallWatchdog;
    constexpr std::uint64_t kStall = 5000;
    constexpr unsigned long kMax   = 20;

    // ---- never before the first callback: a freshly bound lane reads 0 for a while ---------
    {
        StallWatchdog w;
        const std::uint64_t none[2] = { 0, 0 };
        bool fired = false;
        for (std::uint64_t t = 0; t <= 60000; t += 1000)
            fired = fired || w.Observe(none, 2, t, kStall, kMax);
        Check(!fired, "zero callbacks forever is NOT a stall - registration is asynchronous");
        Check(w.Rearms() == 0, "and no re-arm is counted");
    }

    // ---- the measured shape: rendered, then a level load stops it -------------------------
    {
        StallWatchdog w;
        std::uint64_t cb[2] = { 0, 0 };
        std::uint64_t t = 0;
        bool fired = false;
        for (int i = 0; i < 10; ++i)   // 10 s of both lanes rendering at 47/s
        {
            t += 1000; cb[0] += 47; cb[1] += 47;
            fired = fired || w.Observe(cb, 2, t, kStall, kMax);
        }
        Check(!fired, "advancing callbacks never fire");
        // Both stop (the graph was rebuilt). 4.9 s later: not yet.
        Check(!w.Observe(cb, 2, t + 4900, kStall, kMax), "4.9 s of stall is under the threshold");
        Check(w.Observe(cb, 2, t + 5000, kStall, kMax), "5.0 s of stall FIRES");
        Check(w.Pending(), "the re-arm is pending until the caller submits it");
        Check(w.StalledForMs() == 5000, "it reports how long the lane had been stalled");
        Check(w.StalledAt() == 470, "and the callback count it was stuck at");
        Check(w.Rearms() == 1, "one re-arm counted");
        Check(!w.Observe(cb, 2, t + 20000, kStall, kMax),
              "while pending it stays quiet however long the stall goes on");
        w.Rearmed(cb, 2, t + 20000);
        Check(!w.Pending(), "Rearmed clears the pending state");
        Check(!w.Observe(cb, 2, t + 24000, kStall, kMax), "the clock restarts at Rearmed");
        cb[0] += 47; cb[1] += 47;
        Check(!w.Observe(cb, 2, t + 25000, kStall, kMax), "callbacks resumed: no second re-arm");
    }

    // ---- ANY lane stalling fires, and names the lane ---------------------------------------
    {
        StallWatchdog w;
        std::uint64_t cb[2] = { 47, 47 };
        w.Observe(cb, 2, 1000, kStall, kMax);
        std::uint64_t t = 1000;
        bool fired = false;
        std::size_t lane = 99;
        for (int i = 0; i < 10 && !fired; ++i)
        {
            t += 1000; cb[0] += 47;   // the COIL lane keeps rendering; the SPEAKER lane stops
            fired = w.Observe(cb, 2, t, kStall, kMax);
            if (fired) lane = w.StalledLane();
        }
        Check(fired, "the speaker lane stalling alone is enough");
        Check(lane == 1, "and the verdict names lane 1 (the speaker)");
    }

    // ---- a lane that has never called back cannot mask another that stalled -------------
    {
        StallWatchdog w;
        std::uint64_t cb[2] = { 100, 0 };   // speaker never registered a callback yet
        w.Observe(cb, 2, 1000, kStall, kMax);
        Check(w.Observe(cb, 2, 7000, kStall, kMax) && w.StalledLane() == 0,
              "lane 0 stalls; lane 1 at zero is ignored rather than blocking the verdict");
    }

    // ---- the cap: re-arm N times, then give up LOUDLY, never forever -----------------------
    {
        StallWatchdog w;
        std::uint64_t cb[1] = { 10 };
        std::uint64_t t = 0;
        w.Observe(cb, 1, t, kStall, 3);
        unsigned long fires = 0;
        for (int round = 0; round < 6; ++round)
        {
            t += 6000;
            if (w.Observe(cb, 1, t, kStall, 3))
            {
                ++fires;
                w.Rearmed(cb, 1, t);   // submitted, but the subtree stays dead
            }
        }
        Check(fires == 3, "exactly maxRearms re-arms are issued");
        Check(w.GaveUp(), "then the watchdog gives up");
        Check(!w.Observe(cb, 1, t + 60000, kStall, 3), "and never fires again this session");
    }

    // ---- disabled -----------------------------------------------------------------------
    {
        StallWatchdog w;
        std::uint64_t cb[1] = { 10 };
        w.Observe(cb, 1, 0, 0, kMax);
        Check(!w.Observe(cb, 1, 100000, 0, kMax), "stalledMs 0 disables the watchdog");
    }

    // ======================================================================================
    // THE LANE VERDICT: the submix or NOBODY, with the reason. Two lanes, one rule.
    // ======================================================================================
    using sds::JudgeLane;
    using sds::LaneFacts;
    using sds::LaneOwner;
    using sds::LaneVerdict;
    auto has = [](const char* s, const char* needle) { return std::string(s).find(needle) != std::string::npos; };

    {
        LaneFacts f;
        f.tapCreated = true; f.tapBound = true; f.tapCallbacks = 15739; f.tapLive = true;
        const LaneVerdict v = JudgeLane(f);
        Check(v.owner == LaneOwner::Submix && !v.warn, "live: the SUBMIX drives, no warning");
        Check(has(v.headline, "driven by the SUBMIX"), "live: the headline says so");
    }
    {
        LaneFacts f;
        f.tapCreated = true; f.tapBound = true; f.tapCallbacks = 0;
        const LaneVerdict v = JudgeLane(f);
        Check(v.owner == LaneOwner::Nobody && v.warn, "bound, never called: NOBODY, and a warning");
        Check(has(v.detail, "NEVER called") && has(v.detail, "reroute"),
              "bound, never called: the detail says the subtree is not rendered and points at the reroute");
    }
    {
        LaneFacts f;
        f.tapCreated = true; f.tapBound = true; f.tapCallbacks = 500;
        const LaneVerdict v = JudgeLane(f);
        Check(v.owner == LaneOwner::Nobody && v.warn && has(v.detail, "only ever carried silence"),
              "called but silent: NOBODY, detail says silence (not 'never called')");
    }
    {
        LaneFacts f;
        f.tapCreated = true;
        const LaneVerdict v = JudgeLane(f);
        Check(v.owner == LaneOwner::Nobody && v.warn && has(v.detail, "not registered yet"),
              "unbound: NOBODY, detail says unbound");
    }
    {
        LaneFacts f;
        f.tapCreated = true; f.tapBound = true; f.tapRefused = true;
        Check(has(JudgeLane(f).detail, "REFUSED"), "refused: the detail says refused");
        LaneFacts g;
        Check(has(JudgeLane(g).detail, "allocated"), "no tap: the detail says the listener could not be allocated");
    }
    {
        LaneFacts f;
        f.tapCreated = true; f.tapBound = true; f.tapCallbacks = 10; f.tapLive = true;
        f.enabled = false;
        LaneVerdict v = JudgeLane(f);
        Check(v.owner == LaneOwner::Nobody && !v.warn, "disabled in the ini: NOBODY, and no warning (the user asked)");
        f.enabled = true; f.gameSwitch = false;
        v = JudgeLane(f);
        Check(v.owner == LaneOwner::Nobody && !v.warn && has(v.detail, "PadVibrationEnabled"),
              "the game's own switch off: NOBODY, no warning, and the detail names the setting");
    }
    Check(std::string(sds::LaneOwnerName(LaneOwner::Nobody)) == "NOBODY" &&
              std::string(sds::LaneOwnerName(LaneOwner::Submix)) == "SUBMIX",
          "owner names round-trip for the log");

    if (g_failures != 0)
    {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("all submix-watch checks passed\n");
    return 0;
}
