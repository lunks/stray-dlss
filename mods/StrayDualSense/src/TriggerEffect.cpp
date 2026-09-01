#include "TriggerEffect.hpp"

#include <cstring>

namespace sds {

const char* GameModeName(int gameMode)
{
    switch (gameMode)
    {
    case static_cast<int>(GameTriggerMode::None):      return "None";
    case static_cast<int>(GameTriggerMode::Weapon):    return "Weapon";
    case static_cast<int>(GameTriggerMode::Vibration): return "Vibration";
    case static_cast<int>(GameTriggerMode::Feedback):  return "Feedback";
    default:                                           return "UNKNOWN";
    }
}

const char* SonyModeName(SonyTriggerMode mode)
{
    switch (mode)
    {
    case SonyTriggerMode::Off:       return "Off";
    case SonyTriggerMode::Feedback:  return "Feedback";
    case SonyTriggerMode::Weapon:    return "Weapon";
    case SonyTriggerMode::Vibration: return "Vibration";
    default:                         return "UNKNOWN";
    }
}

namespace {

void WriteCommand(uint8_t* block, size_t modeAt, size_t dataAt, bool on,
                  const TriggerEffect& effect)
{
    const SonyTriggerMode sony = on ? ToSonyMode(effect.mode) : SonyTriggerMode::Off;
    const uint32_t        mode = static_cast<uint32_t>(sony);
    std::memcpy(block + modeAt, &mode, sizeof(mode));

    const int count = on ? SonyParamCount(sony) : 0;
    const uint8_t values[3] = { effect.value1, effect.value2, effect.value3 };
    for (int i = 0; i < count && i < 3; ++i)
        block[dataAt + static_cast<size_t>(i)] = values[i];
}

} // namespace

void BuildTriggerParam(bool left, bool right, const TriggerEffect& effect,
                       uint8_t out[kTriggerParamSize])
{
    std::memset(out, 0, kTriggerParamSize);
    out[kTriggerParamMask] = kTriggerMaskBoth;
    WriteCommand(out, kTriggerParamModeL, kTriggerParamDataL, left,  effect);
    WriteCommand(out, kTriggerParamModeR, kTriggerParamDataR, right, effect);
}

} // namespace sds
