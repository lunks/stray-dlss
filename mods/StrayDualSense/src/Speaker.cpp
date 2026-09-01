#include "Speaker.hpp"

#include "Config.hpp"
#include "Log.hpp"

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
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>

namespace sds {
namespace {

std::wstring Widen(const std::string& s)
{
    if (s.empty()) return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<size_t>(n - 1), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

std::string Narrow(const wchar_t* s)
{
    if (s == nullptr) return {};
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string a(static_cast<size_t>(n - 1), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, s, -1, a.data(), n, nullptr, nullptr);
    return a;
}

bool NameIsSafe(const std::string& s)
{
    if (s.empty() || s.size() > 96) return false;
    for (const char c : s)
    {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
        if (!ok) return false;
    }
    return s.find("..") == std::string::npos;
}

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

// KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, spelled out rather than dragged in from ksmedia.h --
// that header wants ks.h and a particular include order, and this is one constant.
constexpr GUID kSubtypeIeeeFloat =
    { 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };

// MEASURED: the mix format is 32-bit float. Anything else and we would be writing floats into
// an integer buffer -- full-scale noise straight into someone's hand. Refuse loudly instead.
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

struct ComReleaser
{
    template <typename T> void operator()(T* p) const { if (p != nullptr) p->Release(); }
};

} // namespace

SpeakerEngine::~SpeakerEngine()
{
    Shutdown();
}

void SpeakerEngine::Start(const Config& config, std::wstring spkDir)
{
    m_config = &config;
    m_spkDir = std::move(spkDir);
    m_running.store(true, std::memory_order_release);
    m_worker = std::thread(&SpeakerEngine::WorkerMain, this);
    SDS_LOG_INFO("speaker: worker started, assets from %ls", m_spkDir.c_str());
}

void SpeakerEngine::Shutdown()
{
    if (!m_running.exchange(false, std::memory_order_acq_rel))
        return;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_requestIsStop = true;
        ++m_requestSeq;
    }
    m_cv.notify_all();
    if (m_worker.joinable())
        m_worker.join();
}

void SpeakerEngine::Play(const std::string& name, float level, bool loop)
{
    if (!NameIsSafe(name))
    {
        SDS_LOG_ERROR("speaker: refusing unsafe asset name '%s'", name.c_str());
        return;
    }
    m_level.store(std::clamp(level, 0.0f, 1.0f), std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_requestName   = name;
        m_requestLoop   = loop;
        m_requestIsStop = false;
        ++m_requestSeq;
    }
    m_cv.notify_all();
}

void SpeakerEngine::Stop()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_requestIsStop = true;
        ++m_requestSeq;
    }
    m_cv.notify_all();
}

void SpeakerEngine::SetLevel(float level)
{
    m_level.store(std::clamp(level, 0.0f, 1.0f), std::memory_order_relaxed);
}

const std::vector<float>* SpeakerEngine::LoadMono(const std::string& name)
{
    {
        std::lock_guard<std::mutex> lock(m_cacheMutex);
        const auto it = m_cache.find(name);
        if (it != m_cache.end())
            return it->second.empty() ? nullptr : &it->second;
    }

    const std::wstring path = m_spkDir + Widen(name) + L".f32";
    std::vector<float> data;

    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") == 0 && f != nullptr)
    {
        std::fseek(f, 0, SEEK_END);
        const long bytes = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        if (bytes > 0)
        {
            data.resize(static_cast<size_t>(bytes) / sizeof(float));
            const size_t got = std::fread(data.data(), sizeof(float), data.size(), f);
            data.resize(got);
        }
        std::fclose(f);
    }

    if (data.empty())
    {
        m_missing.fetch_add(1, std::memory_order_relaxed);
        SDS_LOG_ERROR("speaker: asset MISSING or empty: %ls  (only four _CONTROL assets exist: "
                      "cat_purr_loop_01, cat_backpack_01, cat_backpack_removed_01, "
                      "zurg_sucking_loop_02)", path.c_str());
    }
    else
    {
        SDS_LOG_INFO("speaker: loaded %s (%zu mono frames, %.2fs at %d Hz)", name.c_str(),
                     data.size(), static_cast<double>(data.size()) /
                         (m_config != nullptr ? m_config->speakerAssetRate : 48000),
                     m_config != nullptr ? m_config->speakerAssetRate : 48000);
    }

    std::lock_guard<std::mutex> lock(m_cacheMutex);
    auto& slot = m_cache[name];
    slot = std::move(data);
    return slot.empty() ? nullptr : &slot;
}

void SpeakerEngine::PlayFile(const std::string& name, bool loop, uint64_t seq)
{
    const std::vector<float>* mono = LoadMono(name);
    if (mono == nullptr)
        return;

    IMMDeviceEnumerator* rawEnum = nullptr;
    HRESULT hr = ::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                    __uuidof(IMMDeviceEnumerator),
                                    reinterpret_cast<void**>(&rawEnum));
    if (FAILED(hr) || rawEnum == nullptr)
    {
        m_failures.fetch_add(1, std::memory_order_relaxed);
        SDS_LOG_ERROR("speaker: CoCreateInstance(MMDeviceEnumerator) failed hr=0x%08X",
                      static_cast<unsigned>(hr));
        return;
    }
    std::unique_ptr<IMMDeviceEnumerator, ComReleaser> enumerator(rawEnum);

    IMMDeviceCollection* rawCol = nullptr;
    hr = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &rawCol);
    if (FAILED(hr) || rawCol == nullptr)
    {
        m_failures.fetch_add(1, std::memory_order_relaxed);
        SDS_LOG_ERROR("speaker: EnumAudioEndpoints failed hr=0x%08X", static_cast<unsigned>(hr));
        return;
    }
    std::unique_ptr<IMMDeviceCollection, ComReleaser> collection(rawCol);

    const std::string match = m_config != nullptr ? m_config->speakerDeviceMatch : "DualSense";
    UINT count = 0;
    collection->GetCount(&count);

    std::unique_ptr<IMMDevice, ComReleaser> device;
    for (UINT i = 0; i < count && !device; ++i)
    {
        IMMDevice* rawDev = nullptr;
        if (FAILED(collection->Item(i, &rawDev)) || rawDev == nullptr)
            continue;
        std::unique_ptr<IMMDevice, ComReleaser> candidate(rawDev);

        IPropertyStore* rawProps = nullptr;
        std::string friendly;
        if (SUCCEEDED(candidate->OpenPropertyStore(STGM_READ, &rawProps)) && rawProps != nullptr)
        {
            std::unique_ptr<IPropertyStore, ComReleaser> props(rawProps);
            PROPVARIANT pv;
            ::PropVariantInit(&pv);
            if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.pwszVal != nullptr)
                friendly = Narrow(pv.pwszVal);
            ::PropVariantClear(&pv);
        }
        if (!m_endpointFound.load(std::memory_order_relaxed))
            SDS_LOG_DEBUG("speaker: endpoint [%u] '%s'", i, friendly.c_str());
        if (ContainsNoCase(friendly, match))
        {
            SDS_LOG_INFO("speaker: matched endpoint [%u] '%s' on '%s'", i, friendly.c_str(),
                         match.c_str());
            device = std::move(candidate);
        }
    }

    if (!device)
    {
        m_failures.fetch_add(1, std::memory_order_relaxed);
        m_endpointFound.store(false, std::memory_order_relaxed);
        SDS_LOG_ERROR("speaker: no render endpoint whose name contains '%s' among %u active "
                      "endpoints. GE-Proton exposes the pad only with "
                      "PROTON_SONY_WINDOWS_DEVICE_NAMES=1 and "
                      "PROTON_KEEP_SONY_AUDIO_ENDPOINT_VISIBLE=1.", match.c_str(), count);
        return;
    }
    m_endpointFound.store(true, std::memory_order_relaxed);

    IAudioClient* rawClient = nullptr;
    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                          reinterpret_cast<void**>(&rawClient));
    if (FAILED(hr) || rawClient == nullptr)
    {
        m_failures.fetch_add(1, std::memory_order_relaxed);
        SDS_LOG_ERROR("speaker: IMMDevice::Activate failed hr=0x%08X", static_cast<unsigned>(hr));
        return;
    }
    std::unique_ptr<IAudioClient, ComReleaser> client(rawClient);

    WAVEFORMATEX* mix = nullptr;
    hr = client->GetMixFormat(&mix);
    if (FAILED(hr) || mix == nullptr)
    {
        m_failures.fetch_add(1, std::memory_order_relaxed);
        SDS_LOG_ERROR("speaker: GetMixFormat failed hr=0x%08X", static_cast<unsigned>(hr));
        return;
    }
    const UINT  channels   = mix->nChannels;
    const DWORD deviceRate = mix->nSamplesPerSec;
    const bool  isFloat    = IsFloatFormat(mix);

    SDS_LOG_INFO("speaker: endpoint mix %uch %luHz %ubit float=%d", channels,
                 static_cast<unsigned long>(deviceRate), mix->wBitsPerSample, isFloat ? 1 : 0);

    if (!isFloat)
    {
        // MEASURED format is 32-bit float. Writing floats into an int buffer is full-scale
        // noise straight into someone's hand — refuse rather than guess.
        m_failures.fetch_add(1, std::memory_order_relaxed);
        SDS_LOG_ERROR("speaker: endpoint mix format is not 32-bit float (tag=%u bits=%u). "
                      "Refusing to write - see docs/STRAY-DUALSENSE.md §10.",
                      mix->wFormatTag, mix->wBitsPerSample);
        ::CoTaskMemFree(mix);
        return;
    }

    const REFERENCE_TIME bufferDuration = 10000000;   // 1 s, as the working shim used
    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, bufferDuration, 0, mix, nullptr);
    ::CoTaskMemFree(mix);
    mix = nullptr;
    if (FAILED(hr))
    {
        m_failures.fetch_add(1, std::memory_order_relaxed);
        SDS_LOG_ERROR("speaker: IAudioClient::Initialize failed hr=0x%08X",
                      static_cast<unsigned>(hr));
        return;
    }

    UINT32 bufferFrames = 0;
    client->GetBufferSize(&bufferFrames);

    IAudioRenderClient* rawRender = nullptr;
    hr = client->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&rawRender));
    if (FAILED(hr) || rawRender == nullptr)
    {
        m_failures.fetch_add(1, std::memory_order_relaxed);
        SDS_LOG_ERROR("speaker: GetService(IAudioRenderClient) failed hr=0x%08X",
                      static_cast<unsigned>(hr));
        return;
    }
    std::unique_ptr<IAudioRenderClient, ComReleaser> render(rawRender);

    // The .f32 assets are 48 kHz. If the endpoint runs at another rate, resample rather than
    // play at the wrong pitch — and say so, because it means the measured 48000 has moved.
    const int    assetRate = m_config != nullptr ? m_config->speakerAssetRate : 48000;
    const double step      = static_cast<double>(assetRate) / static_cast<double>(deviceRate);
    if (deviceRate != static_cast<DWORD>(assetRate))
        SDS_LOG_WARN("speaker: endpoint runs at %lu Hz but the assets are %d Hz; resampling "
                     "(ratio %.4f). The measured endpoint rate was 48000.",
                     static_cast<unsigned long>(deviceRate), assetRate, step);

    const float boost = m_config != nullptr ? m_config->speakerBoost : 1.7783f;
    m_started.fetch_add(1, std::memory_order_relaxed);
    SDS_LOG_INFO("speaker: play %s (%zu frames, %.2fs) -> %uch buf=%u loop=%d boost=%.4f",
                 name.c_str(), mono->size(),
                 static_cast<double>(mono->size()) / assetRate, channels, bufferFrames,
                 loop ? 1 : 0, static_cast<double>(boost));

    client->Start();

    double       pos    = 0.0;
    const size_t frames = mono->size();
    const float* src    = mono->data();
    bool         ended  = false;

    for (;;)
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_requestSeq != seq) break;
        }
        if (!m_running.load(std::memory_order_acquire))
            break;

        UINT32 padding = 0;
        if (FAILED(client->GetCurrentPadding(&padding)))
            break;
        const UINT32 avail = bufferFrames - padding;
        if (avail == 0)
        {
            ::Sleep(5);
            continue;
        }

        BYTE* raw = nullptr;
        if (FAILED(render->GetBuffer(avail, &raw)) || raw == nullptr)
            break;
        float* out = reinterpret_cast<float*>(raw);

        const float gain = m_level.load(std::memory_order_relaxed) * boost;
        for (UINT32 i = 0; i < avail; ++i)
        {
            float v = 0.0f;
            size_t idx = static_cast<size_t>(pos);
            if (idx >= frames)
            {
                if (loop) { pos = 0.0; idx = 0; }
                else      { ended = true; }
            }
            if (!ended && idx < frames)
                v = std::clamp(src[idx] * gain, -1.0f, 1.0f);
            pos += step;

            // FL/FR only. RL/RR are the voice coils — writing them never reached them, and
            // the rumble path already covers that side (§10).
            for (UINT c = 0; c < channels; ++c)
                out[i * channels + c] = (c < 2) ? v : 0.0f;
        }
        render->ReleaseBuffer(avail, 0);

        if (ended)
        {
            ::Sleep(120);   // let the endpoint drain what we just queued
            break;
        }
    }

    client->Stop();
    SDS_LOG_INFO("speaker: '%s' ended (ended=%d superseded=%d)", name.c_str(), ended ? 1 : 0,
                 ended ? 0 : 1);
}

void SpeakerEngine::WorkerMain()
{
    // MTA: this thread owns every COM object it creates and never pumps a message loop.
    const HRESULT coInit = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(coInit))
        SDS_LOG_ERROR("speaker: CoInitializeEx failed hr=0x%08X", static_cast<unsigned>(coInit));

    while (m_running.load(std::memory_order_acquire))
    {
        std::string name;
        bool        loop = false;
        uint64_t    seq  = 0;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait(lock, [this] {
                return !m_running.load(std::memory_order_acquire) || m_requestSeq != m_servedSeq;
            });
            if (!m_running.load(std::memory_order_acquire))
                break;
            m_servedSeq = m_requestSeq;
            seq         = m_requestSeq;
            if (m_requestIsStop)
                continue;
            name = m_requestName;
            loop = m_requestLoop;
        }
        PlayFile(name, loop, seq);
    }

    if (SUCCEEDED(coInit))
        ::CoUninitialize();
    SDS_LOG_INFO("speaker: worker exiting");
}

} // namespace sds
