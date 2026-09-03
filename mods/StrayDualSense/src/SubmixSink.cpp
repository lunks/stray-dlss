#include "SubmixSink.hpp"

#include "Config.hpp"
#include "Log.hpp"
#include "Platform.hpp"
#include "Wasapi.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <audioclient.h>

#include <algorithm>
#include <cstring>

namespace sds {
namespace submix {
namespace {

constexpr DWORD kPollMs           = 3;
constexpr DWORD kReopenDelayMs    = 2000;
constexpr std::uint64_t kAssertCooldownMs = 1000;
// Below this peak the buffer counts as silence, so the HID re-assert only fires when
// something is actually about to be felt.
constexpr float kSilenceFloor = 1.0e-4f;

// Pull EXACTLY the frames the resampler will consume — it drops whatever it is handed beyond
// that, so an approximate figure drains the ring faster than the tap fills it (see
// LinearResampler::Process). `step` is srcRate / dstRate: 1.0 in the measured setup, where
// the resampler is a bit-exact copy (tests/test_submix_dsp.cpp).
std::size_t PullLane(SubmixRing* ring, LinearResampler& resampler, double step, UINT32 want,
                     std::vector<float>& pull, std::vector<float>& out)
{
    const std::size_t needFrames = resampler.InputFramesFor(want, step);
    if (pull.size() < needFrames * 2)
        pull.assign(needFrames * 2, 0.0f);
    if (out.size() < static_cast<std::size_t>(want) * 2)
        out.assign(static_cast<std::size_t>(want) * 2, 0.0f);
    if (ring == nullptr)
    {
        std::memset(pull.data(), 0, needFrames * 2 * sizeof(float));
    }
    else
    {
        ring->Read(pull.data(), needFrames);   // short reads are zero-filled and counted
    }
    return resampler.Process(pull.data(), needFrames, step, out.data(), want);
}

} // namespace

SubmixSink::~SubmixSink()
{
    Shutdown();
}

void SubmixSink::Start(SubmixRing* coilRing, SubmixRing* speakerRing, std::string endpointMatch,
                       const Config& config, BeforePlayFn beforeCoils)
{
    m_coilRing      = coilRing;
    m_speakerRing   = speakerRing;
    m_endpointMatch = std::move(endpointMatch);
    m_beforeCoils   = std::move(beforeCoils);
    m_queueAheadMs  = static_cast<std::uint32_t>(config.submixQueueAheadMs);
    m_coilGain.store(config.submixGain, std::memory_order_relaxed);
    m_speakerGain.store(config.speaker ? config.speakerGain : 0.0f, std::memory_order_relaxed);
    m_running.store(true, std::memory_order_release);
    m_worker = std::thread(&SubmixSink::WorkerMain, this);
    SDS_LOG_INFO("submix sink: worker started; endpoint match '%s', queue-ahead %u ms, coil "
                 "gain %.3f -> RL/RR (channels %u/%u), speaker gain %.3f -> FL/FR (channels "
                 "%u/%u)",
                 m_endpointMatch.c_str(), m_queueAheadMs,
                 static_cast<double>(m_coilGain.load()), kEndpointChannelRL, kEndpointChannelRR,
                 static_cast<double>(m_speakerGain.load()), kEndpointChannelFL,
                 kEndpointChannelFR);
}

void SubmixSink::Shutdown()
{
    if (!m_running.exchange(false, std::memory_order_acq_rel))
        return;
    if (m_worker.joinable())
        m_worker.join();
}

void SubmixSink::SetSourceRate(std::uint32_t rate)
{
    if (rate >= 8000 && rate <= 384000)
        m_sourceRate.store(rate, std::memory_order_relaxed);
}

void SubmixSink::SetCoilGain(float gain)
{
    if (gain >= 0.0f && gain <= 8.0f)
        m_coilGain.store(gain, std::memory_order_relaxed);
}

void SubmixSink::SetSpeakerGain(float gain)
{
    if (gain >= 0.0f && gain <= 8.0f)
        m_speakerGain.store(gain, std::memory_order_relaxed);
}

std::string SubmixSink::EndpointName() const
{
    std::lock_guard<std::mutex> lock(m_nameMutex);
    return m_endpointName;
}

void SubmixSink::WorkerMain()
{
    const HRESULT coInit = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(coInit))
        SDS_LOG_ERROR("submix sink: CoInitializeEx failed hr=0x%08X",
                      static_cast<unsigned>(coInit));

    bool listed = false;
    while (m_running.load(std::memory_order_acquire))
    {
        if (!RunStream())
            break;
        if (!m_running.load(std::memory_order_acquire))
            break;
        // RunStream returning true without having played means the endpoint was not there.
        // The pad can be plugged in mid-session, so keep trying — but say so once.
        if (!listed)
        {
            listed = true;
            SDS_LOG_WARN("submix sink: retrying the endpoint every %lu ms until it appears.",
                         static_cast<unsigned long>(kReopenDelayMs));
        }
        ::Sleep(kReopenDelayMs);
    }

    if (SUCCEEDED(coInit))
        ::CoUninitialize();
    SDS_LOG_INFO("submix sink: worker exiting after %llu frame(s)",
                 static_cast<unsigned long long>(m_framesWritten.load()));
}

bool SubmixSink::RunStream()
{
    WasapiStream stream;
    const bool listEndpoints = m_restarts.load(std::memory_order_relaxed) == 0;
    if (!OpenSharedFloatStream(m_endpointMatch, "submix sink", listEndpoints, stream))
    {
        m_streamOpen.store(false, std::memory_order_relaxed);
        m_failures.fetch_add(1, std::memory_order_relaxed);
        return true;     // retry
    }
    m_restarts.fetch_add(1, std::memory_order_relaxed);

    // The coils are RL/RR = channels 2/3. On a 2-channel endpoint there are no coils to reach,
    // and writing FL/FR instead would put the haptic waveform on the SPEAKER — audible, wrong,
    // and hard to diagnose.
    if (stream.channels < kEndpointChannelsRequired)
    {
        m_failures.fetch_add(1, std::memory_order_relaxed);
        SDS_LOG_ERROR("submix sink: endpoint '%s' has %u channel(s) but the pad needs %u "
                      "(FL=0 FR=1 RL=2 RR=3). REFUSING - a haptic waveform on the speaker is "
                      "worse than silence.",
                      stream.endpointName.c_str(), stream.channels, kEndpointChannelsRequired);
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(m_nameMutex);
        m_endpointName = stream.endpointName;
    }
    m_endpointRate.store(stream.sampleRate, std::memory_order_relaxed);
    m_endpointChannels.store(stream.channels, std::memory_order_relaxed);

    const std::uint32_t rate     = stream.sampleRate;
    const std::uint32_t channels = stream.channels;
    const std::uint32_t queueAhead =
        std::min<std::uint32_t>(std::max<std::uint32_t>(rate * m_queueAheadMs / 1000u, 64u),
                                stream.bufferFrames);

    SDS_LOG_INFO("submix sink: '%s' %uch %u Hz buf=%u, queue-ahead %u frames",
                 stream.endpointName.c_str(), channels, rate, stream.bufferFrames, queueAhead);

    HRESULT hr = stream.client->Start();
    if (FAILED(hr))
    {
        m_failures.fetch_add(1, std::memory_order_relaxed);
        SDS_LOG_ERROR("submix sink: IAudioClient::Start failed hr=0x%08X",
                      static_cast<unsigned>(hr));
        return true;
    }
    m_streamOpen.store(true, std::memory_order_relaxed);
    m_coilResampler.Reset();
    m_speakerResampler.Reset();

    bool          coilsWereSilent = true;
    std::uint64_t lastAssertMs    = 0;

    while (m_running.load(std::memory_order_acquire))
    {
        UINT32 padding = 0;
        if (FAILED(stream.client->GetCurrentPadding(&padding)))
        {
            m_failures.fetch_add(1, std::memory_order_relaxed);
            SDS_LOG_ERROR("submix sink: GetCurrentPadding failed; reopening the stream");
            break;
        }
        if (padding >= queueAhead)
        {
            ::Sleep(kPollMs);
            continue;
        }
        const UINT32 want = std::min<UINT32>(queueAhead - padding, stream.bufferFrames - padding);
        if (want == 0)
        {
            ::Sleep(kPollMs);
            continue;
        }

        const double step = static_cast<double>(m_sourceRate.load(std::memory_order_relaxed)) /
                            static_cast<double>(rate);
        const std::size_t coilFrames =
            PullLane(m_coilRing, m_coilResampler, step, want, m_coilPull, m_coilOut);
        const std::size_t speakerFrames =
            PullLane(m_speakerRing, m_speakerResampler, step, want, m_speakerPull, m_speakerOut);

        BYTE* raw = nullptr;
        if (FAILED(stream.render->GetBuffer(want, &raw)) || raw == nullptr)
        {
            m_failures.fetch_add(1, std::memory_order_relaxed);
            SDS_LOG_ERROR("submix sink: GetBuffer(%u) failed; reopening the stream", want);
            break;
        }

        const LanePeaks peaks = InterleaveLanes(
            m_coilOut.data(), coilFrames, m_coilGain.load(std::memory_order_relaxed),
            m_speakerOut.data(), speakerFrames, m_speakerGain.load(std::memory_order_relaxed),
            want, channels, reinterpret_cast<float*>(raw));
        stream.render->ReleaseBuffer(want, 0);
        m_framesWritten.fetch_add(want, std::memory_order_relaxed);

        // Waveform mode, re-asserted on the coil lane's silence -> signal edge. HidMode's own
        // 2 s cadence covers the steady state; this covers the case where libScePad wrote its
        // own report in the last two seconds and the very first haptic of a scratch would be
        // swallowed. The speaker lane has no such mode byte, so it does not take part.
        const bool coilsSilent = peaks.coils < kSilenceFloor;
        if (coilsWereSilent && !coilsSilent && m_beforeCoils)
        {
            const std::uint64_t now = NowMs();
            if (now - lastAssertMs >= kAssertCooldownMs)
            {
                lastAssertMs = now;
                m_beforeCoils();
            }
        }
        coilsWereSilent = coilsSilent;
    }

    m_streamOpen.store(false, std::memory_order_relaxed);
    stream.client->Stop();
    return m_running.load(std::memory_order_acquire);
}

} // namespace submix
} // namespace sds
