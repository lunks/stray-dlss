// Where each lane's listener goes, and which assets can never reach one.
//
// The defect these pin, measured in the running game on 2026-09-03 (docs §20.1): both
// listeners were registered on the two submixes the reroute re-parents, which made them
// SIBLINGS under one `Submix_unused`. UE 4.27 hands a buffer listener the PARENT's
// accumulation buffer, zeroed once per callback rather than once per child, so the second
// sibling processed read both lanes — and every haptic came out of the pad speaker. Over
// 2,483 non-silent status periods the two lanes' `peak` and `rms` were bit-identical in
// 2,365 of them (95.2%), speaker-greater in 116, coil-greater in 2.
#include "SubmixRouting.hpp"
#include "SubmixWatch.hpp"

#include <cstdio>
#include <string>

namespace {

int g_failures = 0;

void Check(bool ok, const char* what)
{
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failures;
}

const char* kVibMaster = "/Game/Sound/tools/settings/Submix_vibrationMaster.Submix_vibrationMaster";
const char* kVibChild  = "/Game/Sound/tools/settings/Submix_vibration.Submix_vibration";
const char* kSpkMaster = "/Game/Sound/tools/settings/Submix_controllerMaster.Submix_controllerMaster";
const char* kSpkChild  = "/Game/Sound/tools/settings/Submix_controller.Submix_controller";

bool Has(const char* haystack, const char* needle)
{
    return std::string(haystack).find(needle) != std::string::npos;
}

} // namespace

int main()
{
    using namespace sds;
    using sds::submix::PlanTap;
    using sds::submix::TapPlan;

    // ---- the fix itself -----------------------------------------------------------------
    {
        const TapPlan coils = PlanTap(kVibChild, kVibMaster);
        Check(coils.target == kVibChild && coils.tappingChild,
              "a configured child path is tapped, NOT the reroute target");
        Check(coils.givesUpMasterVolume,
              "and it declares that the master's own volume/effects leave the samples");

        const TapPlan spk = PlanTap(kSpkChild, kSpkMaster);
        Check(spk.target == kSpkChild && spk.tappingChild, "the speaker lane, likewise");
        Check(std::string(coils.target) != std::string(spk.target),
              "THE POINT: the two lanes tap DIFFERENT submixes, so they cannot share a buffer");
    }

    // ---- the two ways of asking for the old, aliasing behaviour --------------------------
    {
        const TapPlan empty = PlanTap("", kVibMaster);
        Check(empty.target == kVibMaster && !empty.tappingChild,
              "an EMPTY tap path means tap the master - the deliberate one-ini A/B");
        Check(!empty.givesUpMasterVolume,
              "a master tap gives up nothing, because it IS the master");
        Check(Has(empty.why, "ALIAS"),
              "and it says ALIASES in the reason, so the log cannot hide it");

        const TapPlan same = PlanTap(kVibMaster, kVibMaster);
        Check(same.target == kVibMaster && !same.tappingChild,
              "a tap path EQUAL to the reroute target is the same request, spelled out");
        Check(Has(same.why, "ALIAS"), "and is reported as aliasing rather than as a child tap");
    }

    // ---- assets that can never reach a tap ----------------------------------------------
    {
        Check(submix::RoutesToDeadEndpoint("DetectZone_VIBE"),
              "DetectZone_VIBE overrides its submix onto the dead endpoint root");
        Check(submix::RoutesToDeadEndpoint("TrolleyImpactCenter_VIBE"),
              "TrolleyImpactCenter_VIBE likewise");
        Check(!submix::RoutesToDeadEndpoint("TrolleyImpactRight_VIBE"),
              "its SIBLING TrolleyImpactRight_VIBE does NOT - the names are one letter apart "
              "and only one of them is routed away");
        Check(!submix::RoutesToDeadEndpoint("Scratch_VIBE") &&
                  !submix::RoutesToDeadEndpoint("cat_purr_loop_01_CONTROL"),
              "ordinary assets are not claimed");
        Check(!submix::RoutesToDeadEndpoint("detectzone_vibe"),
              "the match is case-sensitive, like the pak and the hook's own log");
        Check(!submix::RoutesToDeadEndpoint(""), "and an empty name claims nothing");
        Check(Has(submix::DeadEndpointReason(), "SoundSubmixObject") &&
                  Has(submix::DeadEndpointReason(), "construction"),
              "the reason names the property AND says it is silent by construction");
    }

    // ---- the regression detector ---------------------------------------------------------
    {
        // The broken arrangement: both lanes read one buffer, so every non-silent period
        // reports the same two floats twice.
        LaneAliasWatch aliased;
        bool fired = false;
        for (unsigned i = 0; i < kAliasMinPairs + 5; ++i)
        {
            const float p = 0.1f + static_cast<float>(i) * 0.01f;
            fired = aliased.Observe(p, p * 0.5f, p, p * 0.5f, 1e-4f) || fired;
        }
        const AliasVerdict v = aliased.Verdict();
        Check(fired, "aliasing is REPORTED, once, as soon as there is enough evidence");
        Check(v.aliasing && v.identical == v.pairs && v.rate > 0.99f,
              "and the verdict is 100% identical over every non-silent pair");

        // Reported exactly once, so a session gets one line and not one per second.
        bool again = false;
        for (unsigned i = 0; i < 50; ++i)
            again = aliased.Observe(0.3f, 0.2f, 0.3f, 0.2f, 1e-4f) || again;
        Check(!again, "and it does not re-report every period afterwards");
    }
    {
        // The FIXED arrangement: the speaker lane carries only speaker content, so the two
        // lanes agree only by coincidence, which the threshold must tolerate.
        LaneAliasWatch clean;
        bool fired = false;
        for (unsigned i = 0; i < 200; ++i)
        {
            const float c = 0.2f + static_cast<float>(i % 7) * 0.03f;
            const float s = (i % 5 == 0) ? 0.4f : 0.0f;   // the speaker is usually quiet
            fired = clean.Observe(c, c * 0.5f, s, s * 0.5f, 1e-4f) || fired;
        }
        const AliasVerdict v = clean.Verdict();
        Check(!fired && !v.aliasing, "separate lanes are NOT reported as aliasing");
        Check(v.identical == 0, "nothing agrees bit-for-bit once the lanes are separate");
        Check(v.pairs == 200, "and every non-silent pair was counted");
    }
    {
        // Both lanes silent says nothing about aliasing, and counting it would bury the
        // signal under a ~99% agreement rate in ordinary play.
        LaneAliasWatch quiet;
        for (unsigned i = 0; i < 500; ++i)
            quiet.Observe(0.0f, 0.0f, 0.0f, 0.0f, 1e-4f);
        const AliasVerdict v = quiet.Verdict();
        Check(v.pairs == 0 && !v.aliasing && v.rate == 0.0f,
              "silent periods are ignored entirely - a quiet game is not evidence");
    }
    {
        // Not enough evidence must not convict: one lane active for a few periods can agree
        // by chance.
        LaneAliasWatch tooFew;
        bool fired = false;
        for (unsigned i = 0; i < kAliasMinPairs - 1; ++i)
            fired = tooFew.Observe(0.5f, 0.3f, 0.5f, 0.3f, 1e-4f) || fired;
        Check(!fired && !tooFew.Verdict().aliasing,
              "below the minimum sample nothing is claimed, however identical");
        Check(tooFew.Verdict().rate > 0.99f,
              "though the rate is still reported, so a human can see it building");
    }
    {
        // The direction the real bug had: speaker >= coils, never the reverse, because the
        // speaker lane was the SUPERSET. Both counters exist so a future run can say which
        // lane is contaminated rather than merely that one is.
        LaneAliasWatch mixed;
        mixed.Observe(0.2f, 0.1f, 0.45f, 0.11f, 1e-4f);   // a _CONTROL playing too
        mixed.Observe(0.3f, 0.2f, 0.1f, 0.05f, 1e-4f);    // coils louder
        const AliasVerdict v = mixed.Verdict();
        Check(v.speakerOnly == 1 && v.coilsOnly == 1 && v.identical == 0,
              "the two directions are counted separately, so a partial fix is visible");
    }
    {
        LaneAliasWatch r;
        for (unsigned i = 0; i < kAliasMinPairs + 1; ++i)
            r.Observe(0.5f, 0.3f, 0.5f, 0.3f, 1e-4f);
        r.Reset();
        Check(r.Verdict().pairs == 0 && !r.Verdict().aliasing, "Reset clears the verdict");
    }

    if (g_failures != 0)
    {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("all submix-routing checks passed\n");
    return 0;
}
