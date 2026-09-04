#include "SubmixRouting.hpp"

namespace sds {
namespace submix {

TapPlan PlanTap(const std::string& tapPath, const std::string& masterPath)
{
    TapPlan plan;
    if (tapPath.empty())
    {
        plan.target       = masterPath;
        plan.tappingChild = false;
        plan.why          = "no tap path configured, so the MASTER is tapped - this is the "
                            "pre-0.4.1 behaviour and it ALIASES with the other lane "
                            "(docs §20.1)";
        return plan;
    }
    if (tapPath == masterPath)
    {
        plan.target       = masterPath;
        plan.tappingChild = false;
        plan.why          = "the tap path IS the reroute target, so the MASTER is tapped - "
                            "this ALIASES with the other lane (docs §20.1)";
        return plan;
    }
    plan.target              = tapPath;
    plan.tappingChild        = true;
    plan.givesUpMasterVolume = true;
    plan.why                 = "tapping the CHILD, so the buffer we are handed is this "
                               "master's own accumulation and cannot contain the other lane "
                               "(docs §20.1)";
    return plan;
}

namespace {

// The two, by short name. Kept as a plain list rather than a set: it is two entries, it is
// read once per vibration start, and a reader should be able to see the whole fact at once.
const char* const kDeadEndpointAssets[] = {
    "DetectZone_VIBE",
    "TrolleyImpactCenter_VIBE",
};

} // namespace

bool RoutesToDeadEndpoint(const std::string& shortAssetName)
{
    for (const char* name : kDeadEndpointAssets)
    {
        if (shortAssetName == name)
            return true;
    }
    return false;
}

const char* DeadEndpointReason()
{
    return "this asset serialises SoundSubmixObject=VibrationEndpointSubmix, overriding the "
           "sound class, so it plays into the DEAD endpoint root - the submix UE 4.27 skips on "
           "Windows because there is no 'Vibration Output' factory. It CANNOT reach our tap, "
           "whatever the gate, the level or the reroute do. Silent by construction, not a "
           "fault (docs §20.11)";
}

} // namespace submix
} // namespace sds
