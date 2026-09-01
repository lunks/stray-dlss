// StrayDualSense — the pad's WASAPI render endpoint.
//
// MEASURED (docs/STRAY-DUALSENSE.md §10/§12): under GE-Proton with
// PROTON_SONY_WINDOWS_DEVICE_NAMES=1 and PROTON_KEEP_SONY_AUDIO_ENDPOINT_VISIBLE=1 the pad
// enumerates as "Speakers (DualSense Wireless Controller)", 4ch / 48000 Hz / 32-bit float,
// shared mode, channel order FL FR RL RR. FL/FR are the speaker; RL/RR are the two coils.
//
// Each caller opens its OWN IAudioClient so that haptics and speaker audio MIX on the
// endpoint instead of displacing each other (the purr plays both at once).
//
// Win32/COM only. The calling thread must have CoInitializeEx'd.
#pragma once

#include <cstdint>
#include <string>

struct IAudioClient;
struct IAudioRenderClient;

namespace sds {

struct WasapiStream
{
    IAudioClient*       client  = nullptr;
    IAudioRenderClient* render  = nullptr;
    uint32_t            channels     = 0;
    uint32_t            sampleRate   = 0;
    uint32_t            bufferFrames = 0;
    std::string         endpointName;

    WasapiStream() = default;
    WasapiStream(const WasapiStream&) = delete;
    WasapiStream& operator=(const WasapiStream&) = delete;
    ~WasapiStream() { Close(); }
    void Close();
};

// Find the first ACTIVE render endpoint whose friendly name contains `match`
// (case-insensitive), activate an IAudioClient on it, insist on a 32-bit float mix format,
// Initialize in shared mode with a 1 s buffer and fetch the render client. Every failure is
// logged with `tag` and returns false; `listEndpoints` additionally logs every candidate,
// which is the diagnostic for "no endpoint matched".
bool OpenSharedFloatStream(const std::string& match, const char* tag, bool listEndpoints,
                           WasapiStream& out);

} // namespace sds
