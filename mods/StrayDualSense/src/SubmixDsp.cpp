#include "SubmixDsp.hpp"

#include <cmath>
#include <cstring>

namespace sds {
namespace submix {

void DownmixToStereo(const float* interleaved, std::size_t frames, int numChannels,
                     float* out)
{
    if (out == nullptr)
        return;
    if (interleaved == nullptr || numChannels <= 0 || frames == 0)
    {
        std::memset(out, 0, frames * 2 * sizeof(float));
        return;
    }
    if (numChannels == 1)
    {
        for (std::size_t i = 0; i < frames; ++i)
        {
            const float v = interleaved[i];
            out[i * 2]     = v;
            out[i * 2 + 1] = v;
        }
        return;
    }
    const std::size_t stride = static_cast<std::size_t>(numChannels);
    for (std::size_t i = 0; i < frames; ++i)
    {
        out[i * 2]     = interleaved[i * stride];
        out[i * 2 + 1] = interleaved[i * stride + 1];
    }
}

float SoftClip(float x)
{
    if (!(x == x))          // NaN: a coil is not the place to find out
        return 0.0f;
    const float mag = x < 0.0f ? -x : x;
    if (mag <= kSoftClipKnee)
        return x;
    const float shoulder = 1.0f - kSoftClipKnee;
    const float y = kSoftClipKnee + shoulder * std::tanh((mag - kSoftClipKnee) / shoulder);
    return x < 0.0f ? -y : y;
}

void LinearResampler::Reset()
{
    m_phase  = 0.0;
    m_lastL  = 0.0f;
    m_lastR  = 0.0f;
    m_primed = false;
}

std::size_t LinearResampler::InputFramesFor(std::size_t outFrames, double step) const
{
    if (outFrames == 0)
        return 0;
    if (!(step > 0.0) || !(step < 1e6))
        step = 1.0;
    // The last sample read sits at `phase + (outFrames - 1) * step`, and interpolating it
    // needs the frame after that, so the buffer must reach floor(last) + 1 — i.e. hold
    // floor(last) + 2 frames. Before the first call the phase is not yet primed and is 0.
    const double phase = m_primed ? m_phase : 0.0;
    const double last  = phase + static_cast<double>(outFrames - 1) * step;
    const double need  = std::floor(last) + 2.0;
    if (!(need > 1.0))
        return 1;
    if (need > 1e9)
        return static_cast<std::size_t>(1e9);
    return static_cast<std::size_t>(need);
}

std::size_t LinearResampler::Process(const float* in, std::size_t inFrames, double step,
                                     float* out, std::size_t maxOutFrames)
{
    if (out == nullptr || maxOutFrames == 0)
        return 0;
    if (in == nullptr || inFrames == 0)
        return 0;
    if (!(step > 0.0) || !(step < 1e6))     // also rejects NaN
        step = 1.0;

    // `m_phase` is measured from the sample BEFORE the incoming buffer: phase in [-1, 0)
    // interpolates across the seam using the previous buffer's last frame, phase >= 0 reads
    // inside `in`. Priming with the first frame makes the very first buffer start exactly on
    // sample 0 rather than fading up from silence.
    if (!m_primed)
    {
        m_lastL  = in[0];
        m_lastR  = in[1];
        m_primed = true;
        m_phase  = 0.0;
    }

    std::size_t produced = 0;
    while (produced < maxOutFrames)
    {
        const double floorPos = std::floor(m_phase);
        const float  frac = static_cast<float>(m_phase - floorPos);
        const long long i0 = static_cast<long long>(floorPos);
        const long long i1 = i0 + 1;
        // The pair (i0, i1) must both be available. i0 == -1 is the seam and reads the
        // previous buffer's last frame; i1 beyond the buffer means the next output sample
        // needs data we do not have yet, so it waits for the next callback.
        if (i0 < -1 || i1 > static_cast<long long>(inFrames) - 1)
            break;

        float l0, r0;
        if (i0 < 0)
        {
            l0 = m_lastL;
            r0 = m_lastR;
        }
        else
        {
            l0 = in[static_cast<std::size_t>(i0) * 2];
            r0 = in[static_cast<std::size_t>(i0) * 2 + 1];
        }
        const float l1 = in[static_cast<std::size_t>(i1) * 2];
        const float r1 = in[static_cast<std::size_t>(i1) * 2 + 1];

        out[produced * 2]     = l0 + (l1 - l0) * frac;
        out[produced * 2 + 1] = r0 + (r1 - r0) * frac;
        ++produced;
        m_phase += step;
    }

    // Carry the seam: remember the last input frame and rebase the phase relative to it.
    m_lastL = in[(inFrames - 1) * 2];
    m_lastR = in[(inFrames - 1) * 2 + 1];
    m_phase -= static_cast<double>(inFrames);
    if (m_phase < -1.0)
        m_phase = -1.0;
    return produced;
}

namespace {

std::uint64_t DoubleBits(double v)
{
    std::uint64_t bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    return bits;
}

double BitsDouble(std::uint64_t bits)
{
    double v = 0.0;
    std::memcpy(&v, &bits, sizeof(v));
    return v;
}

std::uint32_t FloatBits(float v)
{
    std::uint32_t bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    return bits;
}

float BitsFloat(std::uint32_t bits)
{
    float v = 0.0f;
    std::memcpy(&v, &bits, sizeof(v));
    return v;
}

std::size_t RoundUpPow2(std::size_t v)
{
    std::size_t p = 1;
    while (p < v)
        p <<= 1;
    return p;
}

} // namespace

void LevelMeter::Push(const float* in, std::size_t frames)
{
    if (in == nullptr || frames == 0)
        return;
    double peak = 0.0;
    double sum  = 0.0;
    for (std::size_t i = 0; i < frames * 2; ++i)
    {
        const float v = in[i];
        if (!(v == v))
            continue;
        const double a = v < 0.0f ? -static_cast<double>(v) : static_cast<double>(v);
        if (a > peak) peak = a;
        sum += a * a;
    }

    m_frames.fetch_add(frames, std::memory_order_relaxed);

    std::uint64_t oldSum = m_sumSquaresBits.load(std::memory_order_relaxed);
    while (!m_sumSquaresBits.compare_exchange_weak(oldSum, DoubleBits(BitsDouble(oldSum) + sum),
                                                   std::memory_order_relaxed))
    {
    }

    const std::uint32_t want = FloatBits(static_cast<float>(peak));
    std::uint32_t oldPeak = m_peakBits.load(std::memory_order_relaxed);
    while (want > oldPeak &&
           !m_peakBits.compare_exchange_weak(oldPeak, want, std::memory_order_relaxed))
    {
    }
}

LevelReading LevelMeter::Take()
{
    LevelReading r;
    r.frames = m_frames.exchange(0, std::memory_order_relaxed);
    const double sum = BitsDouble(m_sumSquaresBits.exchange(0, std::memory_order_relaxed));
    r.peak = BitsFloat(m_peakBits.exchange(0, std::memory_order_relaxed));
    if (r.frames > 0)
        r.rms = static_cast<float>(std::sqrt(sum / static_cast<double>(r.frames * 2)));
    return r;
}

void SubmixRing::Init(std::size_t capacityFrames)
{
    if (capacityFrames < 64)
        capacityFrames = 64;
    m_capacity = RoundUpPow2(capacityFrames);
    m_mask     = m_capacity - 1;
    m_buffer.assign(m_capacity * 2, 0.0f);
    m_writePos.store(0, std::memory_order_relaxed);
    m_readPos.store(0, std::memory_order_relaxed);
    ResetCounters();
}

void SubmixRing::ResetCounters()
{
    m_written.store(0, std::memory_order_relaxed);
    m_dropped.store(0, std::memory_order_relaxed);
    m_underruns.store(0, std::memory_order_relaxed);
}

std::size_t SubmixRing::Available() const
{
    if (m_capacity == 0)
        return 0;
    const std::uint64_t w = m_writePos.load(std::memory_order_acquire);
    const std::uint64_t r = m_readPos.load(std::memory_order_acquire);
    return static_cast<std::size_t>(w - r);
}

void SubmixRing::Write(const float* in, std::size_t frames)
{
    if (m_capacity == 0 || in == nullptr || frames == 0)
        return;

    // A single buffer larger than the whole ring can only be represented by its tail.
    if (frames > m_capacity)
    {
        m_dropped.fetch_add(frames - m_capacity, std::memory_order_relaxed);
        in += (frames - m_capacity) * 2;
        frames = m_capacity;
    }

    const std::uint64_t w = m_writePos.load(std::memory_order_relaxed);
    for (std::size_t i = 0; i < frames; ++i)
    {
        const std::size_t slot = static_cast<std::size_t>((w + i) & m_mask);
        m_buffer[slot * 2]     = in[i * 2];
        m_buffer[slot * 2 + 1] = in[i * 2 + 1];
    }
    m_writePos.store(w + frames, std::memory_order_release);
    m_written.fetch_add(frames, std::memory_order_relaxed);

    // Overwrite-oldest: if the writer has lapped the reader, drag the read cursor forward so
    // the consumer sees the newest `capacity` frames rather than a scrambled mixture.
    const std::uint64_t r = m_readPos.load(std::memory_order_acquire);
    const std::uint64_t filled = (w + frames) - r;
    if (filled > m_capacity)
    {
        const std::uint64_t lost = filled - m_capacity;
        m_readPos.store(r + lost, std::memory_order_release);
        m_dropped.fetch_add(lost, std::memory_order_relaxed);
    }
}

std::size_t SubmixRing::Read(float* out, std::size_t frames)
{
    if (out == nullptr || frames == 0)
        return 0;
    if (m_capacity == 0)
    {
        std::memset(out, 0, frames * 2 * sizeof(float));
        return 0;
    }
    const std::uint64_t w = m_writePos.load(std::memory_order_acquire);
    const std::uint64_t r = m_readPos.load(std::memory_order_relaxed);
    std::size_t have = static_cast<std::size_t>(w - r);
    if (have > frames)
        have = frames;

    for (std::size_t i = 0; i < have; ++i)
    {
        const std::size_t slot = static_cast<std::size_t>((r + i) & m_mask);
        out[i * 2]     = m_buffer[slot * 2];
        out[i * 2 + 1] = m_buffer[slot * 2 + 1];
    }
    if (have < frames)
    {
        std::memset(out + have * 2, 0, (frames - have) * 2 * sizeof(float));
        m_underruns.fetch_add(frames - have, std::memory_order_relaxed);
    }
    m_readPos.store(r + have, std::memory_order_release);
    return have;
}

} // namespace submix
} // namespace sds
