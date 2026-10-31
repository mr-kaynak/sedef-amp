// SPDX-License-Identifier: AGPL-3.0-or-later
#include "TimeFxTestUtils.h"
#include "sedef/dsp/Doubler.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>

using namespace sedef;
using namespace sedef::test;
using Catch::Approx;
using sedef::dsp::Doubler;

namespace
{
constexpr double kFs = 48000.0;

void run (Doubler& d, Buffer& b, int blockSize)
{
    forEachBlock (b.view(), blockSize, [&] (AudioView v, int) { d.process (v); });
}

double correlation (const std::vector<float>& a, const std::vector<float>& b, size_t from,
                    size_t to, long lag)
{
    double ab = 0.0, aa = 0.0, bb = 0.0;
    for (size_t i = from; i < to; ++i)
    {
        const double x = a[i];
        const double y = b[static_cast<size_t> (static_cast<long> (i) + lag)];
        ab += x * y;
        aa += x * x;
        bb += y * y;
    }
    return ab / std::sqrt (aa * bb + 1e-30);
}

/// Mono white noise in both channels.
Buffer monoNoise (int frames, float amp = 0.5f)
{
    Buffer b (2, frames);
    fillNoise (b[0], amp, 13);
    b[1] = b[0];
    return b;
}
} // namespace

TEST_CASE ("Doubler output is finite for random settings and full-scale noise", "[timefx][doubler]")
{
    const double fs = GENERATE (from_range (kTestRates));
    const int blockSize = GENERATE (from_range (kTestBlockSizes));
    Doubler d;
    d.prepare ({ fs, 512, 2 });
    Random rng (4);
    Buffer b (2, static_cast<int> (fs * 0.5));
    fillNoise (b[0], 1.0f, 1);
    fillNoise (b[1], 1.0f, 2);
    int sinceChange = 1 << 20;
    forEachBlock (b.view(), blockSize, [&] (AudioView v, int) {
        if ((sinceChange += v.numFrames()) >= 1000)
        {
            d.setSettings ({ mapLin (rng.unipolar(), 0.0f, 25.0f) }); // includes out-of-range
            sinceChange = 0;
        }
        d.process (v);
    });
    CHECK (allFinite (b[0]));
    CHECK (allFinite (b[1]));
    CHECK (peak (b[0]) < 1.5f);
    CHECK (peak (b[1]) < 1.5f);
}

TEST_CASE ("Doubler decorrelates a mono source", "[timefx][doubler]")
{
    const float spread = GENERATE (3.0f, 6.0f, 20.0f);
    Doubler d;
    d.prepare ({ kFs, 512, 2 });
    d.setSettings ({ spread });
    Buffer b = monoNoise (static_cast<int> (kFs * 1.5));
    run (d, b, 64);
    INFO ("spread " << spread);
    CHECK (std::abs (correlation (b[0], b[1], 24000, 72000, 0)) < 0.1);
}

TEST_CASE ("Doubler spread sets the offset between the sides", "[timefx][doubler]")
{
    for (float spread : { 4.0f, 10.0f, 18.0f })
    {
        Doubler d;
        d.prepare ({ kFs, 512, 2 });
        d.setSettings ({ spread });
        Buffer b = monoNoise (static_cast<int> (kFs * 2.0));
        run (d, b, 128);

        // The right side is the same signal as the left, arriving `spread` later (give or take the
        // drift of each side).
        long bestLag = 0;
        double best = -1.0, floor = 0.0;
        const long maxLag = static_cast<long> (0.025 * kFs);
        for (long lag = 0; lag < maxLag; ++lag)
        {
            const double c = correlation (b[0], b[1], 24000, 48000, lag);
            floor += std::abs (c) / static_cast<double> (maxLag);
            if (c > best)
            {
                best = c;
                bestLag = lag;
            }
        }
        INFO ("spread " << spread << " measured lag " << bestLag << " peak " << best);
        CHECK (std::abs (static_cast<double> (bestLag) - spread * 1.0e-3 * kFs) < 1.6e-3 * kFs);
        // A clear peak. It is well below 1 because the drift keeps moving the relative delay by
        // fractions of a sample, which smears the correlation of white noise.
        CHECK (best > 10.0 * floor);
    }
}

TEST_CASE ("Doubler keeps the level of each side", "[timefx][doubler]")
{
    Doubler d;
    d.prepare ({ kFs, 512, 2 });
    d.setSettings ({ 8.0f });
    Buffer b (2, static_cast<int> (kFs));
    fillSine (b[0], 440.0, kFs, 0.5f);
    b[1] = b[0];
    const float in = rms (b[0], 4800);
    run (d, b, 64);
    CHECK (20.0f * std::log10 (rms (b[0], 4800) / in) == Approx (0.0f).margin (0.2f));
    // The double is lifted 1 dB and softened above ~7.5 kHz.
    CHECK (20.0f * std::log10 (rms (b[1], 4800) / in) == Approx (1.0f).margin (0.3f));
}

TEST_CASE ("Doubler adds only a few cents of pitch movement", "[timefx][doubler]")
{
    // Count zero crossings of a 1 kHz tone over 50 ms windows: the frequency each window implies
    // must stay within ~10 cents of 1 kHz on both sides.
    Doubler d;
    d.prepare ({ kFs, 512, 2 });
    d.setSettings ({ 12.0f });
    Buffer b (2, static_cast<int> (kFs * 8.0));
    fillSine (b[0], 1000.0, kFs, 0.5f);
    b[1] = b[0];
    run (d, b, 64);
    const int window = 4800;
    double worstCents = 0.0;
    for (int c = 0; c < 2; ++c)
    {
        for (int start = 4800; start + window < b.frames(); start += window)
        {
            // Interpolated crossing times give sub-sample precision.
            double first = -1.0, last = -1.0;
            int crossings = 0;
            for (int i = start + 1; i < start + window; ++i)
            {
                const float p = b[c][static_cast<size_t> (i - 1)],
                            q = b[c][static_cast<size_t> (i)];
                if (p < 0.0f && q >= 0.0f)
                {
                    const double t = (i - 1) + static_cast<double> (p / (p - q));
                    if (first < 0.0)
                        first = t;
                    last = t;
                    ++crossings;
                }
            }
            const double hz = (crossings - 1) * kFs / (last - first);
            worstCents = std::max (worstCents, std::abs (1200.0 * std::log2 (hz / 1000.0)));
        }
    }
    CHECK (worstCents > 0.3); // it does move
    CHECK (worstCents < 10.0);
}

TEST_CASE ("Doubler spread automation glides without clicks", "[timefx][doubler]")
{
    const int blockSize = GENERATE (1, 64, 512);
    Doubler d;
    d.prepare ({ kFs, 512, 2 });
    d.setSettings ({ 3.0f });
    Buffer b (2, static_cast<int> (kFs * 2.0));
    fillSine (b[0], 220.0, kFs, 0.5f);
    b[1] = b[0];
    int sinceChange = 0;
    bool wide = false;
    forEachBlock (b.view(), blockSize, [&] (AudioView v, int) {
        if ((sinceChange += v.numFrames()) >= 4800)
        {
            wide = !wide;
            d.setSettings ({ wide ? 20.0f : 3.0f });
            sinceChange = 0;
        }
        d.process (v);
    });
    // 220 Hz at 0.5 has a second difference of ~4e-3; the glide bends pitch by under a semitone.
    // Skip the onset, where the delayed sine starts with a kink of its own.
    CHECK (maxSecondDifference (b[0], 2400) < 6.0e-3f);
    CHECK (maxSecondDifference (b[1], 2400) < 6.0e-3f);
}

TEST_CASE ("Doubler silence in is silence out; mono passes untouched", "[timefx][doubler]")
{
    Doubler d;
    d.prepare ({ 44100.0, 512, 2 });
    d.setSettings ({ 7.0f });
    Buffer b (2, 6000);
    run (d, b, 17);
    CHECK (peak (b[0]) == 0.0f);
    CHECK (peak (b[1]) == 0.0f);

    Buffer m (1, 3000);
    fillNoise (m[0], 0.5f, 6);
    const auto dry = m.data;
    run (d, m, 64);
    CHECK (m.data == dry);
}

TEST_CASE ("Doubler benchmark", "[.bench][timefx-bench]")
{
    Doubler d;
    d.prepare ({ 48000.0, 64, 2 });
    d.setSettings ({ 9.0f });
    benchmarkNsPerFrame ("Doubler", 10.0, [&] (AudioView v) { d.process (v); });
}
