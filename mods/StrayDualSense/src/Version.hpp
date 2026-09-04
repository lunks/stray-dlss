#pragma once

// Bumped by hand. It appears in the first line of stray-dualsense.log, which is how a pasted
// log is matched to a build — this project's only feedback loop is a user pasting a log back.
//
// 0.4.1: THE TWO LANES STOP SHARING A BUFFER. Both listeners were on the two submixes the
//        reroute re-parents, which made them SIBLINGS under one Submix_unused - and UE 4.27
//        hands a buffer listener the PARENT's accumulation (AudioMixerSubmix.cpp:1364,:1380),
//        zeroed once per callback rather than once per child, so the second sibling processed
//        read BOTH lanes. Measured in the game: the lanes' peak AND rms were bit-identical in
//        95.2% of 2,483 non-silent status periods, and every haptic was emitted on the pad
//        speaker too. Each lane now taps its master's CHILD (SubmixTapPath /
//        SubmixSpeakerTapPath), which is clean by construction. Plus the detector that found
//        it, kept as a regression alarm ('alias' on the SUBMIX line), and a named verdict for
//        the two assets that route themselves to the dead endpoint and can never be tapped.
//        NO gain changed: Level is applied by the engine upstream of the tap (docs §20.4).
// 0.4.0: THE THIN SHIM. The speaker comes from Submix_controllerMaster through the same tap
//        and sink as the coils (FL/FR beside RL/RR of one stream); the asset replay path, the
//        loop lists, the fades, HapticSource and its fallback, the HID speaker-route claim and
//        the PadSpeakerRoute escalation are all deleted. Sony's scePadSetAudioOutPath(3) +
//        SetVolumeGain, HidMode, the reroute, the watchdog and the three-state watch stay.
// 0.3.1: the submix WATCH (one verdict line per StartPS5Vibration, so a per-second peak of
//        0.00000 can no longer be mistaken for a dead submix), peakEver/lastSignal on the
//        SUBMIX line, gate[] on the STATUS line, and the DebugPS5Haptic gate written on the
//        hook's OWN Context object instead of FindFirstOf (which returns the LAST derived
//        match in the whole object array — see docs/STRAY-DUALSENSE.md §15).
// 0.3.0: the coil-owner verdict (COILS: ...), HapticSource=submix-fallback vs strict submix,
//        PS glyphs (GetGameControllerType post-hook), the submix reroute + DebugPS5Haptic gate.
// 0.2.0: coil waveforms over WASAPI + HID mode byte, authored trigger effect, per-component
//        stops, fades. 0.1.0 was the envelope-over-scePadSetVibration design (§9), now dead.
#define SDS_VERSION_MAJOR 0
#define SDS_VERSION_MINOR 4
#define SDS_VERSION_PATCH 1
#define SDS_VERSION_STRING "0.4.1"
