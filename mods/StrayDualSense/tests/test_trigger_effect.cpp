// The enum translation and the param block are the two places a wrong byte yields dead
// triggers with a SUCCESS return code (docs/STRAY-DUALSENSE.md §13). Pin both.
#include "TriggerEffect.hpp"

#include <cstdio>
#include <cstring>

namespace {

int g_failures = 0;

void Check(bool ok, const char* what)
{
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failures;
}

uint32_t U32At(const uint8_t* p, size_t at)
{
    uint32_t v;
    std::memcpy(&v, p + at, sizeof(v));
    return v;
}

} // namespace

int main()
{
    using sds::GameTriggerMode;
    using sds::SonyTriggerMode;
    using sds::ToSonyMode;

    // The table from §13, both directions of the trap.
    Check(ToSonyMode(0) == SonyTriggerMode::Off,       "game None      -> Sony Off");
    Check(ToSonyMode(1) == SonyTriggerMode::Weapon,    "game Weapon    -> Sony Weapon (NOT Feedback)");
    Check(ToSonyMode(2) == SonyTriggerMode::Vibration, "game Vibration -> Sony Vibration");
    Check(ToSonyMode(3) == SonyTriggerMode::Feedback,  "game Feedback  -> Sony Feedback (NOT Vibration)");
    Check(ToSonyMode(7) == SonyTriggerMode::Feedback,  "unknown        -> Sony Feedback, the known-working effect");
    Check(!sds::IsKnownGameMode(4) && sds::IsKnownGameMode(3), "IsKnownGameMode bounds");

    // The fallback is stated in the GAME's space and is what the game authors.
    Check(sds::kFallbackTriggerEffect.mode == static_cast<int>(GameTriggerMode::Feedback),
          "fallback mode is game Feedback (3), never Sony's 1");
    Check(ToSonyMode(sds::kFallbackTriggerEffect.mode) == SonyTriggerMode::Feedback,
          "fallback translates to Sony Feedback");
    Check(sds::kFallbackTriggerEffect.value1 == 0 && sds::kFallbackTriggerEffect.value2 == 2,
          "fallback is FEEDBACK{position 0, strength 2}");

    Check(sds::SonyParamCount(SonyTriggerMode::Feedback) == 2, "Feedback consumes 2 values");
    Check(sds::SonyParamCount(SonyTriggerMode::Weapon) == 3,   "Weapon consumes 3 values");
    Check(sds::SonyParamCount(SonyTriggerMode::Off) == 0,      "Off consumes 0 values");

    // The measured layout, both sides ON with the authored effect.
    uint8_t p[sds::kTriggerParamSize];
    const sds::TriggerEffect authored{ 3, 0, 2, 9 };   // v3=9 must NOT leak into Feedback
    sds::BuildTriggerParam(true, true, authored, p);
    Check(p[0x00] == 0x03,                       "mask addresses L2|R2");
    Check(U32At(p, 0x08) == 1,                   "cmd0.mode = Sony Feedback (1)");
    Check(p[0x10] == 0 && p[0x11] == 2,          "cmd0 data = {position 0, strength 2}");
    Check(p[0x12] == 0,                          "cmd0 third byte stays ZERO for a 2-param mode");
    Check(U32At(p, 0x40) == 1,                   "cmd1.mode = Sony Feedback (1)");
    Check(p[0x48] == 0 && p[0x49] == 2 && p[0x4A] == 0, "cmd1 data = {0, 2, 0}");

    // Left only: right stays IN THE MASK with mode Off.
    sds::BuildTriggerParam(true, false, authored, p);
    Check(p[0x00] == 0x03,                       "left-only keeps both in the mask");
    Check(U32At(p, 0x08) == 1 && U32At(p, 0x40) == 0, "left-only: cmd0 Feedback, cmd1 Off");
    Check(p[0x48] == 0 && p[0x49] == 0,          "off side carries zeroed data");

    // Both off.
    sds::BuildTriggerParam(false, false, authored, p);
    Check(p[0x00] == 0x03 && U32At(p, 0x08) == 0 && U32At(p, 0x40) == 0, "release = mask 3, both Off");

    // A 3-parameter game mode writes three bytes.
    const sds::TriggerEffect weapon{ 1, 2, 7, 8 };
    sds::BuildTriggerParam(true, true, weapon, p);
    Check(U32At(p, 0x08) == 2,                   "game Weapon(1) -> Sony Weapon(2)");
    Check(p[0x10] == 2 && p[0x11] == 7 && p[0x12] == 8, "Weapon carries {start, end, strength}");

    // Everything past the block is zero (slack must be inert).
    bool tailZero = true;
    for (size_t i = 0x50; i < sds::kTriggerParamSize; ++i) tailZero = tailZero && p[i] == 0;
    Check(tailZero, "bytes beyond cmd1 data are zero");

    std::printf(g_failures == 0 ? "\nall TriggerEffect cases passed\n" : "\n%d FAILURE(S)\n",
                g_failures);
    return g_failures == 0 ? 0 : 1;
}
