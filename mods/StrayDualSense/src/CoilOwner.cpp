#include "CoilOwner.hpp"

namespace sds {

const char* HapticSourceName(HapticSource s)
{
    switch (s)
    {
    case HapticSource::Measure:        return "measure";
    case HapticSource::SubmixFallback: return "submix-fallback";
    case HapticSource::Submix:         return "submix";
    case HapticSource::Assets:
    default:                           return "assets";
    }
}

const char* CoilOwnerName(CoilOwner o)
{
    switch (o)
    {
    case CoilOwner::Submix: return "SUBMIX";
    case CoilOwner::Nobody: return "NOBODY";
    case CoilOwner::Assets:
    default:                return "ASSETS";
    }
}

namespace {

// Why the submix is not delivering, in the order a reader would want to rule things out.
const char* SubmixShortfall(const CoilFacts& f)
{
    if (!f.tapCreated)  return "the listener could not even be allocated";
    if (f.tapRefused)   return "the listener registration was REFUSED (see the ERROR above), so the tap is dead for this session";
    if (!f.tapBound)    return "the tap is not registered yet (no game-thread hook has fired, or FAudioDevice was not found)";
    if (f.tapCallbacks == 0)
        return "the tap is registered but the engine has NEVER called it: this submix is not being rendered at all";
    return "the tap is called but has only ever carried silence (no real signal yet)";
}

} // namespace

CoilVerdict JudgeCoils(const CoilFacts& f)
{
    CoilVerdict v;

    // The game's own switch and ours come first: nothing drives the coils if haptics are off.
    if (!f.hapticsEnabled)
    {
        v.owner = CoilOwner::Nobody; v.assetPathActive = false;
        v.headline = "COILS: NOBODY";
        v.detail   = "haptics are disabled in StrayDualSense.ini (Enabled=0 or Haptics=0)";
        return v;
    }
    if (!f.padVibration)
    {
        v.owner = CoilOwner::Nobody; v.assetPathActive = false;
        v.headline = "COILS: NOBODY";
        v.detail   = "PadVibrationEnabled is OFF in the game's settings menu";
        return v;
    }

    switch (f.mode)
    {
    case HapticSource::Assets:
        v.owner = CoilOwner::Assets; v.assetPathActive = true;
        v.headline = "COILS: driven by the ASSET path";
        v.detail   = "HapticSource=assets: the shipped one-slot player, no submix tap exists";
        return v;

    case HapticSource::Measure:
        v.owner = CoilOwner::Assets; v.assetPathActive = true;
        v.headline = "COILS: driven by the ASSET path";
        v.detail   = f.tapLive
                         ? "HapticSource=measure: the tap is carrying a real signal but only REPORTS it; "
                           "nothing from the submix reaches the pad in this mode"
                         : "HapticSource=measure: the tap only reports, and so far it has nothing to report";
        return v;

    case HapticSource::SubmixFallback:
        if (f.tapLive)
        {
            v.owner = CoilOwner::Submix; v.assetPathActive = false;
            v.headline = "COILS: driven by the SUBMIX";
            v.detail   = "HapticSource=submix-fallback: the tap carried a real signal, so the asset path "
                         "has stood down";
            return v;
        }
        v.owner = CoilOwner::Assets; v.assetPathActive = true; v.warn = true;
        v.headline = "COILS: driven by the ASSET path (FALLBACK) - what you feel is NOT the submix";
        v.detail   = SubmixShortfall(f);
        return v;

    case HapticSource::Submix:
    default:
        if (f.tapLive)
        {
            v.owner = CoilOwner::Submix; v.assetPathActive = false;
            v.headline = "COILS: driven by the SUBMIX";
            v.detail   = "HapticSource=submix: the engine's own mix, no asset path";
            return v;
        }
        v.owner = CoilOwner::Nobody; v.assetPathActive = false; v.warn = true;
        v.headline = "COILS: NOBODY - the pad is SILENT by configuration (HapticSource=submix, no fallback)";
        v.detail   = SubmixShortfall(f);
        return v;
    }
}

} // namespace sds
