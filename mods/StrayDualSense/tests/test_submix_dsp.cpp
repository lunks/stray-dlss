// The only part of the submix spike that CAN be proven without the game: the arithmetic
// between an engine submix buffer and the coils. The tap itself (finding FAudioDevice,
// registering the listener) is unverifiable from here, so this must not be.
#include "SubmixDsp.hpp"

#include <cmath>
#include <cstdio>
#include <vector>

namespace {

int g_failures = 0;

void Check(bool ok, const char* what)
{
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failures;
}

bool Near(float a, float b, float eps = 1e-5f) { return std::fabs(a - b) < eps; }

} // namespace

int main()
{
    using namespace sds::submix;

    // -------------------------------------------------------------------------------
    // DownmixToStereo. NumSamples from UE is frames*channels; the caller divides, and
    // getting that wrong is a factor-of-N speed error, so the shapes are pinned here.
    // -------------------------------------------------------------------------------
    {
        const float mono[3] = { 0.25f, -0.5f, 1.0f };
        float out[6] = {};
        DownmixToStereo(mono, 3, 1, out);
        Check(Near(out[0], 0.25f) && Near(out[1], 0.25f), "mono goes to BOTH grips");
        Check(Near(out[4], 1.0f) && Near(out[5], 1.0f),   "mono, third frame");
    }
    {
        const float st[6] = { 1, 2, 3, 4, 5, 6 };
        float out[6] = {};
        DownmixToStereo(st, 3, 2, out);
        Check(out[0] == 1 && out[1] == 2 && out[4] == 5 && out[5] == 6, "stereo passes straight through");
    }
    {
        // 6 channels: FL FR C LFE SL SR. Only FL/FR reach the grips — a surround FOLD would
        // put the centre channel in both hands and smear anything directional.
        const float surround[12] = { 1, 2, 90, 91, 92, 93,
                                     4, 5, 94, 95, 96, 97 };
        float out[4] = {};
        DownmixToStereo(surround, 2, 6, out);
        Check(out[0] == 1 && out[1] == 2, "6ch: frame 0 takes channels 0/1 only");
        Check(out[2] == 4 && out[3] == 5, "6ch: frame 1 takes channels 0/1 only");
    }
    {
        float out[4] = { 9, 9, 9, 9 };
        DownmixToStereo(nullptr, 2, 2, out);
        Check(out[0] == 0 && out[3] == 0, "a null buffer zeroes rather than reading it");
    }

    // -------------------------------------------------------------------------------
    // SoftClip. Unity below the knee, bounded above it, odd, and C1 AT the knee — the
    // reason it exists instead of a clamp is that square corners on a voice coil buzz.
    // -------------------------------------------------------------------------------
    {
        Check(Near(SoftClip(0.0f), 0.0f),   "soft clip: 0 -> 0");
        Check(Near(SoftClip(0.5f), 0.5f),   "soft clip is EXACTLY unity below the knee");
        Check(Near(SoftClip(kSoftClipKnee), kSoftClipKnee), "soft clip is unity AT the knee");
        Check(Near(SoftClip(-0.5f), -0.5f), "soft clip is odd below the knee");
        Check(Near(SoftClip(1.0f), 0.9404f, 1e-3f), "soft clip: 1.0 lands on the shoulder, not at 1.0");
        Check(SoftClip(4.0f) > kSoftClipKnee && SoftClip(4.0f) <= 1.0f, "soft clip bounds a loud sample");
        Check(SoftClip(1e9f) <= 1.0f,       "soft clip bounds even an absurd sample");
        Check(Near(SoftClip(-3.0f), -SoftClip(3.0f)), "soft clip is odd above the knee");
        Check(Near(SoftClip(std::nanf("")), 0.0f), "NaN becomes silence, not a coil full of noise");
        // C1: the slope either side of the knee must agree.
        const float h = 1e-3f;
        const float below = (SoftClip(kSoftClipKnee) - SoftClip(kSoftClipKnee - h)) / h;
        const float above = (SoftClip(kSoftClipKnee + h) - SoftClip(kSoftClipKnee)) / h;
        Check(Near(below, above, 2e-2f), "soft clip is C1 at the knee (no slope discontinuity)");
        // Monotone.
        bool monotone = true;
        float prev = SoftClip(-8.0f);
        for (float x = -8.0f; x <= 8.0f; x += 0.01f)
        {
            const float v = SoftClip(x);
            if (v < prev - 1e-6f) monotone = false;
            prev = v;
        }
        Check(monotone, "soft clip is monotone over [-8, 8]");
    }

    // -------------------------------------------------------------------------------
    // LinearResampler. step == 1 must be BIT-EXACT: the measured setup has the engine
    // and the pad endpoint both at 48 kHz, and a resampler that quietly alters a
    // passthrough would be indistinguishable from a bad tap.
    // -------------------------------------------------------------------------------
    {
        LinearResampler r;
        std::vector<float> in;
        for (int i = 0; i < 64; ++i) { in.push_back(static_cast<float>(i)); in.push_back(static_cast<float>(-i)); }
        std::vector<float> out(256, 0.0f);
        const std::size_t n = r.Process(in.data(), 64, 1.0, out.data(), 128);
        Check(n == 63 || n == 64, "step 1: one buffer in, ~one buffer out");
        bool exact = true;
        for (std::size_t i = 0; i < n; ++i)
            if (!Near(out[i * 2], static_cast<float>(i)) || !Near(out[i * 2 + 1], -static_cast<float>(i)))
                exact = false;
        Check(exact, "step 1 is bit-exact passthrough");

        // The seam: a second buffer must continue without a gap or a repeat.
        std::vector<float> in2;
        for (int i = 64; i < 128; ++i) { in2.push_back(static_cast<float>(i)); in2.push_back(static_cast<float>(-i)); }
        const std::size_t n2 = r.Process(in2.data(), 64, 1.0, out.data(), 128);
        Check(n2 > 0, "the second buffer produces output");
        Check(Near(out[0], static_cast<float>(n)), "the seam continues the ramp with no gap or repeat");
    }
    {
        // step 0.5 = upsampling by 2 (endpoint faster than the engine).
        LinearResampler r;
        const float in[8] = { 0, 0, 1, -1, 2, -2, 3, -3 };
        float out[64] = {};
        const std::size_t n = r.Process(in, 4, 0.5, out, 32);
        Check(n >= 6, "step 0.5 produces about twice as many frames");
        Check(Near(out[0], 0.0f) && Near(out[2], 0.5f) && Near(out[4], 1.0f),
              "step 0.5 interpolates the midpoints");
    }
    {
        // A DC input must come out as the same DC at any rate — the cheapest test that
        // catches an off-by-one in the interpolation.
        LinearResampler r;
        std::vector<float> in(2 * 100, 0.7f);
        std::vector<float> out(2 * 400, -1.0f);
        const std::size_t n = r.Process(in.data(), 100, 0.37, out.data(), 200);
        bool dc = n > 0;
        for (std::size_t i = 0; i < n * 2; ++i)
            if (!Near(out[i], 0.7f)) dc = false;
        Check(dc, "DC survives an arbitrary resampling ratio");
    }
    {
        LinearResampler r;
        const float in[4] = { 1, 1, 1, 1 };
        float out[8] = {};
        Check(r.Process(in, 2, 0.0, out, 4) > 0,  "a zero step falls back to 1.0 rather than looping forever");
        r.Reset();
        Check(r.Process(in, 2, std::nan(""), out, 4) > 0, "a NaN step falls back to 1.0");
        Check(r.Process(nullptr, 2, 1.0, out, 4) == 0,    "a null input produces nothing");
    }

    // -------------------------------------------------------------------------------
    // LevelMeter — the proof instrument. Its whole job is to make "the engine mixed two
    // haptics for us" visible as a bigger number, so the additive case is pinned.
    // -------------------------------------------------------------------------------
    {
        LevelMeter m;
        const float quiet[4] = { 0.1f, -0.1f, 0.1f, -0.1f };
        m.Push(quiet, 2);
        LevelReading a = m.Take();
        Check(a.frames == 2, "meter reports the window it measured");
        Check(Near(a.peak, 0.1f), "meter peak");
        Check(Near(a.rms, 0.1f, 1e-4f), "meter rms of a square-ish signal");

        LevelReading empty = m.Take();
        Check(empty.frames == 0 && empty.peak == 0.0f, "Take() resets, so a silent second reads silent");

        const float loud[4] = { 0.5f, -0.5f, 0.5f, -0.5f };
        m.Push(loud, 2);
        LevelReading b = m.Take();
        Check(b.peak > a.peak && b.rms > a.rms,
              "TWO OVERLAPPING HAPTICS READ LOUDER THAN ONE - the whole point of the tap");

        m.Push(quiet, 2);
        const float withNan[2] = { std::nanf(""), std::nanf("") };
        m.Push(withNan, 1);
        LevelReading c = m.Take();
        Check(c.peak == c.peak && c.rms == c.rms, "a NaN sample does not poison the meter");
    }

    // -------------------------------------------------------------------------------
    // SubmixRing. The producer is the engine's audio render thread: it must never block,
    // so it drops — and a drop that is not COUNTED looks exactly like a tap that never
    // fired, which is the failure this project keeps re-learning.
    // -------------------------------------------------------------------------------
    {
        SubmixRing ring;
        ring.Init(100);
        Check(ring.CapacityFrames() == 128, "capacity rounds up to a power of two");
        Check(ring.Available() == 0, "a fresh ring is empty");

        std::vector<float> in(2 * 32);
        for (std::size_t i = 0; i < 32; ++i) { in[i * 2] = static_cast<float>(i); in[i * 2 + 1] = -static_cast<float>(i); }
        ring.Write(in.data(), 32);
        Check(ring.Available() == 32, "write advances the fill");
        Check(ring.Written() == 32 && ring.Dropped() == 0, "no drops when there is room");

        std::vector<float> out(2 * 32, -99.0f);
        Check(ring.Read(out.data(), 32) == 32, "read returns what was written");
        Check(out[0] == 0.0f && out[62] == 31.0f && out[63] == -31.0f, "the samples come back in order");
        Check(ring.Available() == 0, "the ring is empty again");

        // Underrun: silence, counted.
        std::vector<float> out2(2 * 16, -99.0f);
        Check(ring.Read(out2.data(), 16) == 0, "reading an empty ring returns 0 real frames");
        Check(out2[0] == 0.0f && out2[31] == 0.0f, "an underrun is SILENCE, not stale audio");
        Check(ring.Underruns() == 16, "the underrun is counted");

        // Overflow: the consumer sees the NEWEST audio, and the loss is counted.
        ring.ResetCounters();
        std::vector<float> big(2 * 200);
        for (std::size_t i = 0; i < 200; ++i) { big[i * 2] = static_cast<float>(i); big[i * 2 + 1] = static_cast<float>(i); }
        ring.Write(big.data(), 200);
        Check(ring.Available() == 128, "a burst larger than the ring leaves it exactly full");
        Check(ring.Dropped() == 200 - 128, "the overflow is counted, every frame of it");
        std::vector<float> out3(2 * 128, 0.0f);
        ring.Read(out3.data(), 128);
        Check(out3[0] == 72.0f, "on overflow the consumer keeps the NEWEST frames (a haptic wants to be current)");

        // Lapping the reader mid-stream.
        ring.Init(64);
        std::vector<float> chunk(2 * 40, 1.0f);
        ring.Write(chunk.data(), 40);
        ring.Write(chunk.data(), 40);
        Check(ring.Available() == 64, "lapping the reader leaves the ring full, not corrupt");
        Check(ring.Dropped() == 16, "and the lapped frames are counted");
    }

    std::printf(g_failures == 0 ? "\nall SubmixDsp cases passed\n" : "\n%d FAILURE(S)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
