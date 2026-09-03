// Who drives the coils — the sentence that was missing on 2026-09-03.
//
// The session that motivated this: HapticSource=submix, `submix bound=1` in the log, the pad
// vibrating, and total=0 callbacks. The user concluded the submix worked. The vibration was the
// ASSET fallback. Every case below is a combination of facts a status line must turn into ONE
// unambiguous owner, and the two rules that can never be broken are:
//
//   * a fallback that is driving the coils says so, and warns, until the submix takes over;
//   * strict `submix` NEVER plays an asset, so anything felt in that mode came from the submix.
#include "CoilOwner.hpp"

#include <cstdio>
#include <cstring>
#include <initializer_list>

namespace {

int g_failures = 0;

void Check(bool ok, const char* what)
{
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failures;
}

bool StartsWith(const char* s, const char* prefix)
{
    return std::strncmp(s, prefix, std::strlen(prefix)) == 0;
}

using sds::CoilFacts;
using sds::CoilOwner;
using sds::CoilVerdict;
using sds::HapticSource;
using sds::JudgeCoils;

CoilFacts Facts(HapticSource mode)
{
    CoilFacts f;
    f.mode = mode;
    f.hapticsEnabled = true;
    f.padVibration   = true;
    return f;
}

CoilFacts BoundSilent(HapticSource mode)
{
    CoilFacts f = Facts(mode);
    f.tapCreated = true;
    f.tapBound   = true;
    f.tapCallbacks = 0;
    f.tapLive    = false;
    return f;
}

CoilFacts Live(HapticSource mode)
{
    CoilFacts f = BoundSilent(mode);
    f.tapCallbacks = 15739;
    f.tapLive = true;
    return f;
}

} // namespace

int main()
{
    // ---- assets: the shipped path, never a warning -------------------------------------
    {
        const CoilVerdict v = JudgeCoils(Facts(HapticSource::Assets));
        Check(v.owner == CoilOwner::Assets, "assets: the asset path owns the coils");
        Check(v.assetPathActive, "assets: the asset player may play");
        Check(!v.warn, "assets: no warning");
        Check(StartsWith(v.headline, "COILS: driven by the ASSET path"), "assets: headline names the asset path");
    }

    // ---- measure: the tap only reports, whatever it carries ------------------------------
    {
        const CoilVerdict silent = JudgeCoils(BoundSilent(HapticSource::Measure));
        Check(silent.owner == CoilOwner::Assets && silent.assetPathActive, "measure/silent: assets own the coils");
        Check(!silent.warn, "measure/silent: a silent submix is a RESULT here, not a warning");

        const CoilVerdict live = JudgeCoils(Live(HapticSource::Measure));
        Check(live.owner == CoilOwner::Assets && live.assetPathActive, "measure/live: assets STILL own the coils");
        Check(!live.warn, "measure/live: no warning");
        Check(std::strstr(live.detail, "only REPORTS") != nullptr, "measure/live: the detail says the signal does not reach the pad");
    }

    // ---- THE 2026-09-03 SESSION: bound, zero callbacks, assets vibrating -------------------
    {
        const CoilVerdict v = JudgeCoils(BoundSilent(HapticSource::SubmixFallback));
        Check(v.owner == CoilOwner::Assets, "fallback/bound-silent: the ASSET path owns the coils");
        Check(v.assetPathActive, "fallback/bound-silent: the asset player keeps playing");
        Check(v.warn, "fallback/bound-silent: WARNS - the configuration asked for the submix and got nothing");
        Check(StartsWith(v.headline, "COILS: driven by the ASSET path (FALLBACK)"),
              "fallback/bound-silent: the headline says FALLBACK and that it is not the submix");
        Check(std::strstr(v.headline, "NOT the submix") != nullptr,
              "fallback/bound-silent: nobody can read this line as 'the submix works'");
        Check(std::strstr(v.detail, "NEVER called") != nullptr,
              "fallback/bound-silent: the detail distinguishes 'never called' from 'silent'");
    }
    {
        CoilFacts f = BoundSilent(HapticSource::SubmixFallback);
        f.tapCallbacks = 500;   // called, but only silence
        const CoilVerdict v = JudgeCoils(f);
        Check(v.owner == CoilOwner::Assets && v.warn, "fallback/called-silent: assets own it, still a warning");
        Check(std::strstr(v.detail, "only ever carried silence") != nullptr,
              "fallback/called-silent: the detail says silence, not 'never called'");
    }
    {
        const CoilVerdict v = JudgeCoils(Live(HapticSource::SubmixFallback));
        Check(v.owner == CoilOwner::Submix, "fallback/live: the SUBMIX owns the coils after the handover");
        Check(!v.assetPathActive, "fallback/live: the asset player stands down");
        Check(!v.warn, "fallback/live: no warning once the submix delivers");
    }

    // ---- strict submix: the submix or nothing ------------------------------------------
    {
        const CoilVerdict v = JudgeCoils(BoundSilent(HapticSource::Submix));
        Check(v.owner == CoilOwner::Nobody, "submix/bound-silent: NOBODY drives the coils");
        Check(!v.assetPathActive, "submix/bound-silent: the asset player must NOT play - no silent fallback");
        Check(v.warn, "submix/bound-silent: warns on a cadence");
        Check(std::strstr(v.headline, "SILENT by configuration") != nullptr,
              "submix/bound-silent: the headline says the pad is silent by configuration");
    }
    {
        CoilFacts f = Facts(HapticSource::Submix);
        f.tapCreated = true;   // not bound yet
        const CoilVerdict v = JudgeCoils(f);
        Check(v.owner == CoilOwner::Nobody && !v.assetPathActive && v.warn, "submix/unbound: nobody, no assets, warning");
        Check(std::strstr(v.detail, "not registered yet") != nullptr, "submix/unbound: the detail says unbound");
    }
    {
        CoilFacts f = BoundSilent(HapticSource::Submix);
        f.tapRefused = true;
        const CoilVerdict v = JudgeCoils(f);
        Check(v.owner == CoilOwner::Nobody && v.warn, "submix/refused: nobody, warning");
        Check(std::strstr(v.detail, "REFUSED") != nullptr, "submix/refused: the detail says refused");
    }
    {
        CoilFacts f = Facts(HapticSource::Submix);
        f.tapCreated = false;
        const CoilVerdict v = JudgeCoils(f);
        Check(std::strstr(v.detail, "allocated") != nullptr, "submix/no-tap: the detail says the listener could not be allocated");
    }
    {
        const CoilVerdict v = JudgeCoils(Live(HapticSource::Submix));
        Check(v.owner == CoilOwner::Submix && !v.assetPathActive && !v.warn, "submix/live: the submix drives, no warning");
        Check(StartsWith(v.headline, "COILS: driven by the SUBMIX"), "submix/live: headline names the submix");
    }

    // ---- the two switches above every mode ---------------------------------------------
    for (HapticSource mode : { HapticSource::Assets, HapticSource::Measure,
                               HapticSource::SubmixFallback, HapticSource::Submix })
    {
        CoilFacts f = Live(mode);
        f.hapticsEnabled = false;
        CoilVerdict v = JudgeCoils(f);
        Check(v.owner == CoilOwner::Nobody && !v.assetPathActive && !v.warn,
              "Haptics=0: nobody drives, no asset, and no submix warning (the user turned it off)");

        f = Live(mode);
        f.padVibration = false;
        v = JudgeCoils(f);
        Check(v.owner == CoilOwner::Nobody && !v.assetPathActive && !v.warn,
              "PadVibrationEnabled off: nobody drives, and the detail names the game setting");
        Check(std::strstr(v.detail, "PadVibrationEnabled") != nullptr, "PadVibrationEnabled off: detail");
    }

    // ---- every headline starts with COILS: so a grep finds the owner in one line --------
    for (HapticSource mode : { HapticSource::Assets, HapticSource::Measure,
                               HapticSource::SubmixFallback, HapticSource::Submix })
    {
        Check(StartsWith(JudgeCoils(Facts(mode)).headline, "COILS:"), "headline starts with COILS: (fresh)");
        Check(StartsWith(JudgeCoils(BoundSilent(mode)).headline, "COILS:"), "headline starts with COILS: (bound-silent)");
        Check(StartsWith(JudgeCoils(Live(mode)).headline, "COILS:"), "headline starts with COILS: (live)");
    }

    // ---- names round-trip for the log ---------------------------------------------------
    Check(std::strcmp(sds::HapticSourceName(HapticSource::SubmixFallback), "submix-fallback") == 0, "name: submix-fallback");
    Check(std::strcmp(sds::HapticSourceName(HapticSource::Submix), "submix") == 0, "name: submix");
    Check(std::strcmp(sds::CoilOwnerName(CoilOwner::Nobody), "NOBODY") == 0, "name: NOBODY");

    if (g_failures != 0)
    {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("all coil-owner checks passed\n");
    return 0;
}
