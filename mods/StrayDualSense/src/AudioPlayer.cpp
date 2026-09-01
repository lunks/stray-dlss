#include "AudioPlayer.hpp"

#include "Fade.hpp"
#include "Log.hpp"
#include "Platform.hpp"
#include "Wasapi.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <audioclient.h>

#include <algorithm>
#include <cstring>

namespace sds {
namespace {

// How far ahead of the play cursor we keep the endpoint fed. The client buffer is 1 s, but
// filling all of it would make a Stop or a level change wait a second to be heard. 100 ms is
// far more than the 5 ms poll needs and keeps everything responsive.
constexpr uint32_t QueueAheadFrames(uint32_t rate) { return rate / 10; }
constexpr DWORD    kPollMs = 5;

} // namespace

AudioPlayer::~AudioPlayer()
{
    Shutdown();
}

void AudioPlayer::Start(const AudioRoute& route, std::string endpointMatch, std::wstring dir,
                        BeforePlayFn beforePlay)
{
    m_route         = route;
    m_endpointMatch = std::move(endpointMatch);
    m_dir           = std::move(dir);
    m_beforePlay    = std::move(beforePlay);
    m_running.store(true, std::memory_order_release);
    m_worker = std::thread(&AudioPlayer::WorkerMain, this);
    SDS_LOG_INFO("%s: worker started; %u-channel assets from %ls -> endpoint channels "
                 "0x%X/0x%X, gain %.4f",
                 m_route.tag, m_route.sourceChannels, m_dir.c_str(), m_route.outputMask[0],
                 m_route.outputMask[1], static_cast<double>(m_route.gain));
}

void AudioPlayer::Shutdown()
{
    if (!m_running.exchange(false, std::memory_order_acq_rel))
        return;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_request.isStop  = true;
        m_request.fadeOut = 0.0f;
        ++m_request.seq;
    }
    m_cv.notify_all();
    if (m_worker.joinable())
        m_worker.join();
}

void AudioPlayer::Play(const std::string& name, float level, float fadeInSeconds, bool loop)
{
    if (!AssetNameIsSafe(name))
    {
        SDS_LOG_ERROR("%s: refusing unsafe asset name '%s'", m_route.tag, name.c_str());
        return;
    }
    m_level.store(std::clamp(level, 0.0f, 1.0f), std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_request.name    = name;
        m_request.fadeIn  = fadeInSeconds;
        m_request.fadeOut = 0.0f;
        m_request.loop    = loop;
        m_request.isStop  = false;
        ++m_request.seq;
    }
    m_cv.notify_all();
}

void AudioPlayer::Stop(float fadeOutSeconds)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_request.isStop  = true;
        m_request.fadeOut = fadeOutSeconds;
        ++m_request.seq;
    }
    m_cv.notify_all();
}

void AudioPlayer::SetLevel(float level)
{
    m_level.store(std::clamp(level, 0.0f, 1.0f), std::memory_order_relaxed);
}

std::string AudioPlayer::CurrentName() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_currentName;
}

bool AudioPlayer::LoadAsset(const std::string& name, std::vector<float>& out, size_t& outFrames)
{
    // No cache: a 38 s stereo asset is ~15 MB and there are 64 of them. A per-play read of a
    // few MB on the worker is nothing next to the playback itself.
    const std::wstring   path = m_dir + Widen(name) + L".f32";
    std::vector<uint8_t> bytes;
    const bool opened = ReadWholeFile(path, bytes);
    const size_t frameBytes = sizeof(float) * m_route.sourceChannels;
    outFrames = bytes.size() / frameBytes;

    if (!opened || outFrames == 0)
    {
        m_missing.fetch_add(1, std::memory_order_relaxed);
        // Loud: a silent miss here is indistinguishable from "the game never asked".
        SDS_LOG_ERROR("%s: asset %s: %ls (run tools/dualsense/extract_assets.sh + wavegen.sh "
                      "to populate it)", m_route.tag, opened ? "EMPTY" : "MISSING", path.c_str());
        return false;
    }
    out.resize(outFrames * m_route.sourceChannels);
    std::memcpy(out.data(), bytes.data(), out.size() * sizeof(float));
    return true;
}

void AudioPlayer::PlayOne(const Request& req)
{
    std::vector<float> pcm;
    size_t             frames = 0;
    if (!LoadAsset(req.name, pcm, frames))
        return;

    WasapiStream stream;
    if (!OpenSharedFloatStream(m_endpointMatch, m_route.tag, !m_endpointsListed, stream))
    {
        m_endpointsListed = true;
        m_endpointFound.store(false, std::memory_order_relaxed);
        m_failures.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    m_endpointsListed = true;
    m_endpointFound.store(true, std::memory_order_relaxed);

    // The route's channels must exist on this endpoint. The coils are RL/RR = 2/3; a 2-channel
    // endpoint has no coils to reach, and writing FL/FR instead would put the haptic waveform
    // on the SPEAKER — audible, wrong, and hard to diagnose. Refuse.
    const uint32_t needed = m_route.outputMask[0] | m_route.outputMask[1];
    if (stream.channels >= 32 || (needed >> stream.channels) != 0)
    {
        m_failures.fetch_add(1, std::memory_order_relaxed);
        SDS_LOG_ERROR("%s: endpoint '%s' has %u channel(s) but this route needs mask 0x%X "
                      "(FL=0 FR=1 RL=2 RR=3). Refusing to play %s on the wrong channels.",
                      m_route.tag, stream.endpointName.c_str(), stream.channels, needed,
                      req.name.c_str());
        return;
    }
    if (stream.sampleRate != kAssetSampleRate)
        SDS_LOG_WARN("%s: endpoint runs at %u Hz but the assets are %u Hz; playing at the "
                     "wrong pitch/speed. The measured endpoint rate was 48000.", m_route.tag,
                     stream.sampleRate, kAssetSampleRate);

    const uint32_t rate         = stream.sampleRate;
    const uint32_t channels     = stream.channels;
    const uint64_t fadeInFrames = FadeFrames(req.fadeIn, rate);
    const uint32_t queueAhead   = std::min(QueueAheadFrames(rate), stream.bufferFrames);

    m_started.fetch_add(1, std::memory_order_relaxed);
    SDS_LOG_INFO("%s: play %s (%zu frames, %.2fs, loop=%d, fadeIn=%.2fs, level=%.2f) -> '%s' "
                 "%uch %uHz buf=%u", m_route.tag, req.name.c_str(), frames,
                 static_cast<double>(frames) / rate, req.loop ? 1 : 0,
                 static_cast<double>(req.fadeIn), static_cast<double>(m_level.load()),
                 stream.endpointName.c_str(), channels, rate, stream.bufferFrames);

    if (m_beforePlay)
        m_beforePlay();

    HRESULT hr = stream.client->Start();
    if (FAILED(hr))
    {
        m_failures.fetch_add(1, std::memory_order_relaxed);
        SDS_LOG_ERROR("%s: IAudioClient::Start failed hr=0x%08X", m_route.tag,
                      static_cast<unsigned>(hr));
        return;
    }
    m_playing.store(true, std::memory_order_relaxed);

    size_t   pos        = 0;      // frame index into the asset
    uint64_t played     = 0;      // frames generated (drives the fade-in)
    bool     fadingOut  = false;
    uint64_t fadeOutLen = 0;
    uint64_t fadeOutPos = 0;
    uint64_t fadeSeq    = req.seq;
    bool     ended      = false;  // no more audio to generate
    bool     superseded = false;
    bool     stopped    = false;
    const float* src    = pcm.data();
    const uint32_t sc   = m_route.sourceChannels;

    for (;;)
    {
        // A newer request? A Play supersedes at once. A Stop starts (or restarts) a fade-out;
        // with no fade it ends at once.
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_request.seq != fadeSeq)
            {
                fadeSeq = m_request.seq;
                if (!m_request.isStop) { superseded = true; break; }
                stopped    = true;
                fadeOutLen = FadeFrames(m_request.fadeOut, rate);
                fadeOutPos = 0;
                fadingOut  = true;
                if (fadeOutLen == 0) { ended = true; }
            }
        }
        if (!m_running.load(std::memory_order_acquire)) { stopped = true; break; }
        if (ended) break;

        UINT32 padding = 0;
        if (FAILED(stream.client->GetCurrentPadding(&padding)))
        {
            m_failures.fetch_add(1, std::memory_order_relaxed);
            SDS_LOG_ERROR("%s: GetCurrentPadding failed mid-stream", m_route.tag);
            break;
        }
        if (padding >= queueAhead)
        {
            ::Sleep(kPollMs);
            continue;
        }
        const UINT32 want = std::min(queueAhead - padding, stream.bufferFrames - padding);

        BYTE* raw = nullptr;
        if (FAILED(stream.render->GetBuffer(want, &raw)) || raw == nullptr)
        {
            m_failures.fetch_add(1, std::memory_order_relaxed);
            SDS_LOG_ERROR("%s: GetBuffer(%u) failed mid-stream", m_route.tag, want);
            break;
        }
        float* out = reinterpret_cast<float*>(raw);

        const float level = m_level.load(std::memory_order_relaxed) * m_route.gain;
        for (UINT32 i = 0; i < want; ++i)
        {
            float l = 0.0f, r = 0.0f;
            if (!ended)
            {
                if (pos >= frames)
                {
                    if (req.loop) pos = 0;
                    else          ended = true;
                }
            }
            if (!ended)
            {
                float g = level * FadeInGain(played, fadeInFrames);
                if (fadingOut)
                {
                    g *= FadeOutGain(fadeOutPos, fadeOutLen);
                    if (++fadeOutPos >= fadeOutLen) ended = true;
                }
                l = src[pos * sc] * g;
                r = (sc == 2) ? src[pos * sc + 1] * g : l;
                ++pos;
                ++played;
            }
            l = std::clamp(l, -1.0f, 1.0f);
            r = std::clamp(r, -1.0f, 1.0f);
            for (uint32_t c = 0; c < channels; ++c)
            {
                float v = 0.0f;
                if (m_route.outputMask[0] & (1u << c))      v = l;
                else if (m_route.outputMask[1] & (1u << c)) v = r;
                out[i * channels + c] = v;
            }
        }
        stream.render->ReleaseBuffer(want, 0);

        if (ended)
        {
            // Let what we just queued drain before the stream is torn down.
            ::Sleep(static_cast<DWORD>(queueAhead * 1000ull / rate) + 20);
            break;
        }
    }

    m_playing.store(false, std::memory_order_relaxed);
    stream.client->Stop();
    m_finished.fetch_add(1, std::memory_order_relaxed);
    SDS_LOG_INFO("%s: %s ended after %.2fs (%s)", m_route.tag, req.name.c_str(),
                 static_cast<double>(played) / rate,
                 superseded ? "superseded by a newer request"
                 : stopped  ? (fadeOutLen ? "stopped with fade-out" : "stopped")
                            : "played to the end");
}

void AudioPlayer::WorkerMain()
{
    // MTA: this thread owns every COM object it creates and never pumps a message loop.
    const HRESULT coInit = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(coInit))
        SDS_LOG_ERROR("%s: CoInitializeEx failed hr=0x%08X", m_route.tag,
                      static_cast<unsigned>(coInit));

    while (m_running.load(std::memory_order_acquire))
    {
        Request req;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait(lock, [this] {
                return !m_running.load(std::memory_order_acquire) || m_request.seq != m_servedSeq;
            });
            if (!m_running.load(std::memory_order_acquire))
                break;
            m_servedSeq = m_request.seq;
            if (m_request.isStop)
            {
                m_currentName.clear();
                continue;       // nothing in flight on this thread: the stop already landed
            }
            req           = m_request;
            m_currentName = req.name;
        }
        PlayOne(req);
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_request.seq == req.seq)
                m_currentName.clear();
        }
    }

    if (SUCCEEDED(coInit))
        ::CoUninitialize();
    SDS_LOG_INFO("%s: worker exiting", m_route.tag);
}

} // namespace sds
