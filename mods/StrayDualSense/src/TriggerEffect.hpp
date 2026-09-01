// StrayDualSense — the adaptive-trigger effect, as data.
//
// Pure and portable: no Windows, no UE4SS, no pad. Everything here is proven in CI, which
// matters because this is where two enums in DIFFERENT ORDERS meet (docs/STRAY-DUALSENSE.md
// §13) and a mistake produces dead triggers with no error code.
//
//   value | game EPS5TriggerEffectMode | Sony ScePadTriggerEffectMode
//   ------+----------------------------+-----------------------------
//     0   | None                       | Off
//     1   | Weapon                     | Feedback
//     2   | Vibration                  | Weapon
//     3   | Feedback                   | Vibration
//
// The game's authored effect (HKPlayerController::m_scratchablePS5TriggerEffect) is stored in
// the GAME's numbering. Sony's API wants Sony's. Translate at exactly one place — here — and
// state every default in the game's space, because a default written as "1 = Feedback"
// silently became Weapon the moment translation existed.
#pragma once

#include <cstddef>
#include <cstdint>

namespace sds {

// The game's enum, in binary DECLARATION order (never derive this from sorted strings).
enum class GameTriggerMode : int
{
    None      = 0,
    Weapon    = 1,
    Vibration = 2,
    Feedback  = 3,
};

// Sony's enum (public SDK, cross-checked against the game's own dispatcher, §3).
enum class SonyTriggerMode : uint32_t
{
    Off       = 0,
    Feedback  = 1,   // {position, strength}
    Weapon    = 2,   // {start, end, strength}
    Vibration = 3,   // {position, amplitude, frequency}
};

// PS5TriggerEffectData {Mode, Value1, Value2, Value3}, in the GAME's enum space.
struct TriggerEffect
{
    int     mode   = static_cast<int>(GameTriggerMode::Feedback);
    uint8_t value1 = 0;
    uint8_t value2 = 2;
    uint8_t value3 = 0;

    bool operator==(const TriggerEffect& o) const
    {
        return mode == o.mode && value1 == o.value1 && value2 == o.value2 && value3 == o.value3;
    }
    bool operator!=(const TriggerEffect& o) const { return !(*this == o); }
};

// What the game AUTHORS, measured live (§6/§13): mode=3 (game Feedback) v1=0 v2=2 v3=0, i.e.
// Sony FEEDBACK{position 0, strength 2}. Used only when the authored struct cannot be read,
// and logged as a fallback when it is.
constexpr TriggerEffect kFallbackTriggerEffect{ static_cast<int>(GameTriggerMode::Feedback), 0, 2, 0 };

// True for the four values the game's enum defines.
constexpr bool IsKnownGameMode(int gameMode)
{
    return gameMode >= 0 && gameMode <= 3;
}

// Game -> Sony. An UNKNOWN game value maps to Sony Feedback, the one effect known to work
// here, rather than to Off — a dead trigger is the silent failure this file exists to avoid.
// Callers must log the translation when the input is unknown.
constexpr SonyTriggerMode ToSonyMode(int gameMode)
{
    switch (gameMode)
    {
    case static_cast<int>(GameTriggerMode::None):      return SonyTriggerMode::Off;
    case static_cast<int>(GameTriggerMode::Weapon):    return SonyTriggerMode::Weapon;
    case static_cast<int>(GameTriggerMode::Vibration): return SonyTriggerMode::Vibration;
    case static_cast<int>(GameTriggerMode::Feedback):  return SonyTriggerMode::Feedback;
    default:                                           return SonyTriggerMode::Feedback;
    }
}

// How many of Value1..3 the Sony mode consumes. Feedback takes two; Weapon and Vibration
// take three; Off takes none. Bytes beyond the count are left ZERO in the param block so a
// stale Value3 cannot leak into a two-parameter effect.
constexpr int SonyParamCount(SonyTriggerMode mode)
{
    switch (mode)
    {
    case SonyTriggerMode::Feedback:  return 2;
    case SonyTriggerMode::Weapon:    return 3;
    case SonyTriggerMode::Vibration: return 3;
    case SonyTriggerMode::Off:
    default:                         return 0;
    }
}

const char* GameModeName(int gameMode);
const char* SonyModeName(SonyTriggerMode mode);

// ScePadTriggerEffectParam, from the game's own construction site (§3), accepted by the
// library (0x00000000) where four guessed strides returned 0x80920001:
//   +0x00 triggerMask (bit0 L2, bit1 R2)
//   +0x08 command[0].mode (uint32)     +0x10 command[0] data bytes
//   +0x40 command[1].mode (uint32)     +0x48 command[1] data bytes     stride 0x38
// The real block is ~120 bytes; 256 zeroed is slack.
constexpr size_t kTriggerParamSize   = 256;
constexpr size_t kTriggerParamMask   = 0x00;
constexpr size_t kTriggerParamModeL  = 0x08;
constexpr size_t kTriggerParamDataL  = 0x10;
constexpr size_t kTriggerParamModeR  = 0x40;
constexpr size_t kTriggerParamDataR  = 0x48;
constexpr uint8_t kTriggerMaskBoth   = 0x03;

// Build the block. BOTH triggers are always in the mask and the per-side MODE varies —
// masking a side out leaves it stuck at whatever the previous call set (§8). A side that is
// off gets Sony Off with zeroed data.
void BuildTriggerParam(bool left, bool right, const TriggerEffect& effect,
                       uint8_t out[kTriggerParamSize]);

} // namespace sds
