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
    // THE ENGINE'S FOLD, coefficient for coefficient (UE 4.27 AudioMixerChannelMaps.cpp:86-91,
    // ToStereoMatrix; docs/RESEARCH-UE-PAD-AUDIO-ENDPOINT.md §3.3). This is what the real PS5
    // endpoint receives, so it is what the coils must receive from us.
    {
        // 6 channels in the engine's order: FL FR C LFE SL SR.
        //   L = FL + 0.707 C + 0.707 SL      R = FR + 0.707 C + 0.707 SR      LFE dropped
        const float surround[12] = { 1, 2, 10, 1000, 20, 30,
                                     4, 5, 40, 1000, 50, 60 };
        float out[4] = {};
        DownmixToStereo(surround, 2, 6, out);
        Check(Near(out[0], 1 + 0.707f * 10 + 0.707f * 20), "6ch: L = FL + 0.707 C + 0.707 SL");
        Check(Near(out[1], 2 + 0.707f * 10 + 0.707f * 30), "6ch: R = FR + 0.707 C + 0.707 SR");
        Check(Near(out[2], 4 + 0.707f * 40 + 0.707f * 50) && Near(out[3], 5 + 0.707f * 40 + 0.707f * 60),
              "6ch: frame 1 folds the same way");
        Check(out[0] < 1000 && out[1] < 1000, "6ch: LFE is dropped (coefficient 0.0)");
    }
    {
        // 8 channels — the measured `ch=8` — in the engine's column order: FL FR C LFE SL SR
        // BL BR. Every coefficient of the cited table, exercised one column at a time by a
        // unit impulse in each channel.
        const float expectL[8] = { 1.0f, 0.0f, 0.707f, 0.0f, 0.707f, 0.0f, 0.707f, 0.0f };
        const float expectR[8] = { 0.0f, 1.0f, 0.707f, 0.0f, 0.0f, 0.707f, 0.0f, 0.707f };
        bool allOk = true;
        for (int c = 0; c < 8; ++c)
        {
            float impulse[8] = {};
            impulse[c] = 1.0f;
            float out[2] = { -1, -1 };
            DownmixToStereo(impulse, 1, 8, out);
            if (!Near(out[0], expectL[c]) || !Near(out[1], expectR[c]))
            {
                allOk = false;
                std::printf("      8ch column %d -> L %.3f R %.3f (want %.3f %.3f)\n", c,
                            static_cast<double>(out[0]), static_cast<double>(out[1]),
                            static_cast<double>(expectL[c]), static_cast<double>(expectR[c]));
            }
        }
        Check(allOk, "8ch: every column of ToStereoMatrix (AudioMixerChannelMaps.cpp:86-91) is exact");
        Check(Near(kFoldCentreAndSurround, 0.707f), "the fold coefficient is the engine's literal 0.707, not 1/sqrt(2)");
    }
    {
        // A REAR-ONLY input is no longer silent: a spatialised haptic send behind the cat
        // lands in BL/BR (8ch) or SL/SR (6ch), and the engine's endpoint shakes the coils at
        // -3 dB for it. The old channels-0/1 fold dropped it to exactly zero.
        const float rearOnly8[8] = { 0, 0, 0, 0, 0, 0, 0.5f, 0.25f };   // BL, BR
        float out[2] = {};
        DownmixToStereo(rearOnly8, 1, 8, out);
        Check(Near(out[0], 0.707f * 0.5f) && Near(out[1], 0.707f * 0.25f),
              "8ch rear-only: BL reaches the LEFT grip and BR the RIGHT, at 0.707");
        const float rearOnly6[6] = { 0, 0, 0, 0, 0.5f, 0.25f };        // SL, SR
        DownmixToStereo(rearOnly6, 1, 6, out);
        Check(Near(out[0], 0.707f * 0.5f) && Near(out[1], 0.707f * 0.25f),
              "6ch rear-only: SL/SR reach their grips at 0.707 - not silence");
        const float centreOnly[6] = { 0, 0, 1.0f, 0, 0, 0 };
        DownmixToStereo(centreOnly, 1, 6, out);
        Check(Near(out[0], 0.707f) && Near(out[1], 0.707f), "centre goes to BOTH grips at 0.707");
    }
    {
        // QUAD is the engine's special case: channels 0 1 2 3 are FL FR SL SR and index
        // columns 0 1 4 5 (Get2DChannelMapInternal :379-404), NOT columns 2/3 (C/LFE).
        const float quad[4] = { 1, 2, 10, 20 };
        float out[2] = {};
        DownmixToStereo(quad, 1, 4, out);
        Check(Near(out[0], 1 + 0.707f * 10) && Near(out[1], 2 + 0.707f * 20),
              "quad: SL/SR fold into their grips at 0.707 (columns 4/5, the engine's special case)");
    }
    {
        // Wider than the engine's own maximum (8): folded on the first eight, never read past.
        const float wide[10] = { 1, 2, 0, 0, 0, 0, 0, 0, 999, 999 };
        float out[2] = {};
        DownmixToStereo(wide, 1, 10, out);
        Check(Near(out[0], 1) && Near(out[1], 2), "10ch: channels beyond the engine's 8 are ignored");
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
    // THE REGRESSION THAT MATTERS FOR THE SINK, and the bug it was written for.
    //
    // Process() consumes the WHOLE buffer and drops whatever it did not need, so the sink must
    // hand it exactly InputFramesFor(). The first version of the sink asked for
    // `want * step + 2` "for the interpolator's seam" — two frames per iteration more than the
    // resampler could use. That drains the producer faster than it fills, which shows up not
    // as an obvious glitch but as a slow leak into permanent underruns plus a discontinuity
    // every buffer. This test is a whole pipeline run: a continuous ramp through a ring, in
    // sink-sized chunks, checked for both gaps and repeats.
    // -------------------------------------------------------------------------------
    {
        for (double step : { 1.0, 0.5, 2.0, 48000.0 / 44100.0 })
        {
            LinearResampler r;
            SubmixRing ring;
            ring.Init(8192);

            double  produced   = 0.0;      // total output frames
            double  consumed   = 0.0;      // total input frames requested
            float   fed        = 0.0f;     // the ramp value written next
            bool    ok         = true;
            bool    continuous = true;
            float   lastOut    = -1.0f;

            std::vector<float> pull, out(2 * 512);
            for (int iter = 0; iter < 200; ++iter)
            {
                const std::size_t want = 256;
                const std::size_t need = r.InputFramesFor(want, step);
                // The producer supplies exactly what was asked for — as the tap does, at its
                // own rate. If the consumer asks for more than it can use, this diverges.
                if (pull.size() < need * 2) pull.assign(need * 2, 0.0f);
                std::vector<float> chunk(need * 2);
                for (std::size_t i = 0; i < need; ++i)
                {
                    chunk[i * 2]     = fed;
                    chunk[i * 2 + 1] = fed;
                    fed += 1.0f;
                }
                ring.Write(chunk.data(), need);
                if (ring.Read(pull.data(), need) != need) ok = false;
                const std::size_t got = r.Process(pull.data(), need, step, out.data(), want);
                if (got != want) ok = false;
                consumed += static_cast<double>(need);
                produced += static_cast<double>(got);
                // The output must be a monotone ramp with no jump back and no stall: a
                // dropped input frame shows as a step larger than `step`, a repeated one as a
                // step of zero.
                for (std::size_t i = 0; i < got; ++i)
                {
                    const float v = out[i * 2];
                    if (iter > 0 || i > 0)
                    {
                        const float d = v - lastOut;
                        if (d < static_cast<float>(step) * 0.5f || d > static_cast<float>(step) * 1.5f)
                            continuous = false;
                    }
                    lastOut = v;
                }
            }
            Check(ok, "sink loop: every iteration produced exactly the frames it asked for");
            Check(continuous, "sink loop: the ramp survives 200 buffers with no gap and no repeat");
            // The ratio of input to output frames must be the step, to within a frame or two
            // of start-up transient. A +2 per iteration overdraw would show as ~1.008 at
            // step 1.0 — small, and permanent.
            const double ratio = consumed / produced;
            Check(ratio > step * 0.999 && ratio < step * 1.001 + 0.001,
                  "sink loop: input/output frame ratio equals the resampling step (no drain leak)");
            Check(ring.Underruns() == 0, "sink loop: NO underruns — the ring is not being over-drained");
        }
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

    // -------------------------------------------------------------------------------
    // InterleaveLanes: two tapped submixes -> the pad's one 4-channel stream. The channel
    // map is MEASURED (FL FR RL RR; speaker on the front pair, coils on the rear pair) and a
    // swapped pair is a haptic waveform coming out of the speaker, so it is pinned here.
    // -------------------------------------------------------------------------------
    {
        const float coils[4]   = { 0.10f, -0.20f, 0.30f, -0.40f };   // 2 frames, L/R per frame
        const float speaker[4] = { 0.50f,  0.50f, 0.60f,  0.60f };
        float out[8] = { 9, 9, 9, 9, 9, 9, 9, 9 };
        const LanePeaks p = InterleaveLanes(coils, 2, 1.0f, speaker, 2, 1.0f, 2, 4, out);
        Check(Near(out[kEndpointChannelFL], 0.5f) && Near(out[kEndpointChannelFR], 0.5f),
              "frame 0: the SPEAKER lane lands on FL/FR");
        Check(Near(out[kEndpointChannelRL], 0.1f) && Near(out[kEndpointChannelRR], -0.2f),
              "frame 0: the COIL lane lands on RL/RR, left grip then right grip");
        Check(Near(out[4 + kEndpointChannelFL], 0.6f) && Near(out[4 + kEndpointChannelRL], 0.3f) &&
                  Near(out[4 + kEndpointChannelRR], -0.4f),
              "frame 1 follows at a stride of `channels`");
        Check(Near(p.coils, 0.4f) && Near(p.speaker, 0.6f),
              "the peaks are reported PER LANE, as magnitudes");
    }
    {
        // An 8-channel endpoint (the engine's own mixer runs at 7.1; the pad measured 4):
        // the four extra channels are written as silence, never left as whatever the WASAPI
        // buffer held.
        const float coils[2]   = { 0.1f, 0.2f };
        const float speaker[2] = { 0.3f, 0.4f };
        float out[8] = { 9, 9, 9, 9, 9, 9, 9, 9 };
        InterleaveLanes(coils, 1, 1.0f, speaker, 1, 1.0f, 1, 8, out);
        Check(out[4] == 0.0f && out[5] == 0.0f && out[6] == 0.0f && out[7] == 0.0f,
              "channels beyond RR are silence on a wider endpoint");
        Check(Near(out[0], 0.3f) && Near(out[1], 0.4f) && Near(out[2], 0.1f) && Near(out[3], 0.2f),
              "and the four we use are unchanged by the width");
    }
    {
        // Lanes are independent: a short (or empty) speaker lane leaves the coils alone, and
        // vice versa. This is the steady state - the purr is the only event that drives both.
        const float coils[4] = { 0.1f, 0.1f, 0.2f, 0.2f };
        float out[8] = {};
        const LanePeaks p = InterleaveLanes(coils, 2, 1.0f, nullptr, 0, 1.0f, 2, 4, out);
        Check(out[0] == 0.0f && out[1] == 0.0f && out[4] == 0.0f && out[5] == 0.0f,
              "an empty speaker lane writes silence to FL/FR");
        Check(Near(out[2], 0.1f) && Near(out[6], 0.2f), "while the coils still reach RL/RR");
        Check(p.speaker == 0.0f && Near(p.coils, 0.2f), "and only the coil peak is non-zero");

        float out2[8] = {};
        const float speaker[2] = { 0.7f, 0.7f };
        InterleaveLanes(coils, 2, 1.0f, speaker, 1, 1.0f, 2, 4, out2);
        Check(Near(out2[0], 0.7f) && out2[4] == 0.0f,
              "a lane shorter than the frame count is zero-padded, not repeated");
    }
    {
        // Gain is per lane and the soft clip runs AFTER it: PadVibrationEnabled=off is a coil
        // gain of 0 and must silence the coils without touching the speaker.
        const float coils[2]   = { 0.5f, 0.5f };
        const float speaker[2] = { 0.5f, 0.5f };
        float out[4] = {};
        const LanePeaks p = InterleaveLanes(coils, 1, 0.0f, speaker, 1, 1.0f, 1, 4, out);
        Check(out[2] == 0.0f && out[3] == 0.0f && Near(out[0], 0.5f),
              "coil gain 0 silences RL/RR only");
        Check(p.coils == 0.0f && Near(p.speaker, 0.5f), "and the coil peak reads 0");

        float out2[4] = {};
        const LanePeaks p2 = InterleaveLanes(coils, 1, 4.0f, speaker, 1, 1.0f, 1, 4, out2);
        Check(out2[2] > kSoftClipKnee && out2[2] <= 1.0f && Near(out2[2], SoftClip(2.0f)),
              "a hot coil lane is soft-clipped after the gain, never hard-clamped");
        Check(Near(p2.coils, SoftClip(2.0f)), "the reported peak is the clipped value that was written");
    }
    {
        // A 2-channel endpoint has no coil pair. The sink refuses it before ever calling this,
        // but the function itself must not write past the frame either.
        const float coils[2]   = { 0.1f, 0.2f };
        const float speaker[2] = { 0.3f, 0.4f };
        float out[4] = { 9, 9, 9, 9 };
        InterleaveLanes(coils, 1, 1.0f, speaker, 1, 1.0f, 1, 2, out);
        Check(Near(out[0], 0.3f) && Near(out[1], 0.4f) && out[2] == 9.0f && out[3] == 9.0f,
              "a 2-channel endpoint gets the speaker pair and nothing is written beyond it");
    }

    std::printf(g_failures == 0 ? "\nall SubmixDsp cases passed\n" : "\n%d FAILURE(S)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
