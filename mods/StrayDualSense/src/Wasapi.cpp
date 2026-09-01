#include "Wasapi.hpp"

#include "Log.hpp"
#include "Platform.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
// initguid.h MUST precede functiondiscoverykeys_devpkey.h: without it DEFINE_PROPERTYKEY
// emits an extern declaration only, and PKEY_Device_FriendlyName never links.
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propidl.h>
#include <mmreg.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <memory>

namespace sds {
namespace {

struct ComReleaser
{
    template <typename T> void operator()(T* p) const { if (p != nullptr) p->Release(); }
};
template <typename T> using ComPtr = std::unique_ptr<T, ComReleaser>;

bool ContainsNoCase(const std::string& haystack, const std::string& needle)
{
    if (needle.empty()) return true;
    const auto it = std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(),
                                [](char a, char b) {
                                    return ::tolower(static_cast<unsigned char>(a)) ==
                                           ::tolower(static_cast<unsigned char>(b));
                                });
    return it != haystack.end();
}

// KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, spelled out rather than dragged in from ksmedia.h.
constexpr GUID kSubtypeIeeeFloat =
    { 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };

bool IsFloatFormat(const WAVEFORMATEX* wf)
{
    if (wf == nullptr) return false;
    if (wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT)
        return true;
    if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE && wf->cbSize >= 22)
    {
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wf);
        return std::memcmp(&ext->SubFormat, &kSubtypeIeeeFloat, sizeof(GUID)) == 0;
    }
    return false;
}

std::string FriendlyName(IMMDevice* dev)
{
    IPropertyStore* rawProps = nullptr;
    if (FAILED(dev->OpenPropertyStore(STGM_READ, &rawProps)) || rawProps == nullptr)
        return {};
    ComPtr<IPropertyStore> props(rawProps);
    PROPVARIANT pv;
    ::PropVariantInit(&pv);
    std::string name;
    if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.pwszVal != nullptr)
        name = Narrow(pv.pwszVal);
    ::PropVariantClear(&pv);
    return name;
}

constexpr REFERENCE_TIME kBufferDuration = 10000000;   // 1 s, what the working shim used

} // namespace

void WasapiStream::Close()
{
    if (render != nullptr) { render->Release(); render = nullptr; }
    if (client != nullptr) { client->Stop(); client->Release(); client = nullptr; }
}

bool OpenSharedFloatStream(const std::string& match, const char* tag, bool listEndpoints,
                           WasapiStream& out)
{
    out.Close();

    IMMDeviceEnumerator* rawEnum = nullptr;
    HRESULT hr = ::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                    __uuidof(IMMDeviceEnumerator),
                                    reinterpret_cast<void**>(&rawEnum));
    if (FAILED(hr) || rawEnum == nullptr)
    {
        SDS_LOG_ERROR("%s: CoCreateInstance(MMDeviceEnumerator) failed hr=0x%08X", tag,
                      static_cast<unsigned>(hr));
        return false;
    }
    ComPtr<IMMDeviceEnumerator> enumerator(rawEnum);

    IMMDeviceCollection* rawCol = nullptr;
    hr = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &rawCol);
    if (FAILED(hr) || rawCol == nullptr)
    {
        SDS_LOG_ERROR("%s: EnumAudioEndpoints failed hr=0x%08X", tag, static_cast<unsigned>(hr));
        return false;
    }
    ComPtr<IMMDeviceCollection> collection(rawCol);

    UINT count = 0;
    collection->GetCount(&count);

    ComPtr<IMMDevice> device;
    for (UINT i = 0; i < count && !device; ++i)
    {
        IMMDevice* rawDev = nullptr;
        if (FAILED(collection->Item(i, &rawDev)) || rawDev == nullptr)
            continue;
        ComPtr<IMMDevice> candidate(rawDev);
        const std::string name = FriendlyName(candidate.get());
        if (listEndpoints)
            SDS_LOG_INFO("%s: render endpoint [%u] '%s'", tag, i, name.c_str());
        if (ContainsNoCase(name, match))
        {
            out.endpointName = name;
            device = std::move(candidate);
        }
    }

    if (!device)
    {
        SDS_LOG_ERROR("%s: no ACTIVE render endpoint whose name contains '%s' among %u. "
                      "GE-Proton exposes the pad only with PROTON_SONY_WINDOWS_DEVICE_NAMES=1 "
                      "and PROTON_KEEP_SONY_AUDIO_ENDPOINT_VISIBLE=1.", tag, match.c_str(), count);
        return false;
    }

    IAudioClient* rawClient = nullptr;
    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                          reinterpret_cast<void**>(&rawClient));
    if (FAILED(hr) || rawClient == nullptr)
    {
        SDS_LOG_ERROR("%s: IMMDevice::Activate failed hr=0x%08X", tag, static_cast<unsigned>(hr));
        return false;
    }
    out.client = rawClient;

    WAVEFORMATEX* mix = nullptr;
    hr = out.client->GetMixFormat(&mix);
    if (FAILED(hr) || mix == nullptr)
    {
        SDS_LOG_ERROR("%s: GetMixFormat failed hr=0x%08X", tag, static_cast<unsigned>(hr));
        out.Close();
        return false;
    }
    out.channels   = mix->nChannels;
    out.sampleRate = mix->nSamplesPerSec;
    const bool isFloat = IsFloatFormat(mix);

    if (!isFloat)
    {
        // MEASURED format is 32-bit float. Writing floats into an int buffer is full-scale
        // noise straight into someone's hand — refuse rather than guess.
        SDS_LOG_ERROR("%s: endpoint '%s' mix format is not 32-bit float (tag=%u bits=%u). "
                      "Refusing to write.", tag, out.endpointName.c_str(), mix->wFormatTag,
                      mix->wBitsPerSample);
        ::CoTaskMemFree(mix);
        out.Close();
        return false;
    }

    hr = out.client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, kBufferDuration, 0, mix, nullptr);
    ::CoTaskMemFree(mix);
    if (FAILED(hr))
    {
        SDS_LOG_ERROR("%s: IAudioClient::Initialize(shared, 1 s) failed hr=0x%08X", tag,
                      static_cast<unsigned>(hr));
        out.Close();
        return false;
    }

    out.client->GetBufferSize(&out.bufferFrames);

    IAudioRenderClient* rawRender = nullptr;
    hr = out.client->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&rawRender));
    if (FAILED(hr) || rawRender == nullptr)
    {
        SDS_LOG_ERROR("%s: GetService(IAudioRenderClient) failed hr=0x%08X", tag,
                      static_cast<unsigned>(hr));
        out.Close();
        return false;
    }
    out.render = rawRender;
    return true;
}

} // namespace sds
