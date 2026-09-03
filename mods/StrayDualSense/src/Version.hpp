#pragma once

// Bumped by hand. It appears in the first line of stray-dualsense.log, which is how a pasted
// log is matched to a build — this project's only feedback loop is a user pasting a log back.
//
// 0.3.0: the coil-owner verdict (COILS: ...), HapticSource=submix-fallback vs strict submix,
//        PS glyphs (GetGameControllerType post-hook), the submix reroute + DebugPS5Haptic gate.
// 0.2.0: coil waveforms over WASAPI + HID mode byte, authored trigger effect, per-component
//        stops, fades. 0.1.0 was the envelope-over-scePadSetVibration design (§9), now dead.
#define SDS_VERSION_MAJOR 0
#define SDS_VERSION_MINOR 3
#define SDS_VERSION_PATCH 0
#define SDS_VERSION_STRING "0.3.0"
