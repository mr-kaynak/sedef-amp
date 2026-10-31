// SPDX-License-Identifier: AGPL-3.0-or-later
#include "TimeFxTestUtils.h"
#include "sedef/dsp/GraphicEq.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>

using namespace sedef;
using namespace sedef::test;
using Catch::Approx;
using sedef::dsp::GraphicEq;

namespace
{
GraphicEq::Settings randomSettings (Random& rng)
{
    GraphicEq::Settings s;
    for (auto& g : s.bandsDb)
        g = rng.unipolar() < 0.2f ? 0.0f : 12.0f * rng.bipolar();
    const float lc = rng.unipolar();
    s.lowCutHz = lc < 0.3f ? 20.0f : mapExp (lc, 20.0f, 500.0f);
    const float hc = rng.unipolar();
    s.highCutHz = hc < 0.3f ? 20000.0f : mapExp (hc, 1000.0f, 20000.0f);
    return s;
}

double processedGainDb (GraphicEq& eq, double hz, double fs)
{
    return measureGainDb ([&] (AudioView v)
                          { forEachBlock (v, 512, [&] (AudioView b, int) { eq.process (b); }); },
                          hz, fs);
}

double designDb (const BiquadCoeffs& c, double hz, double fs)
{
    return 20.0 * std::log10 (design::magnitude (c, hz, fs));
}
} // namespace

TEST_CASE ("GraphicEq output is finite for random settings and full-scale noise", "[timefx][eq]")
{
    const double fs = GENERATE (from_range (kTestRates));
    const int blockSize = GENERATE (from_range (kTestBlockSizes));
    GraphicEq eq;
    eq.prepare ({ fs, 512, 2 });
    Random rng (11);
    Buffer b (2, static_cast<int> (fs * 0.4));
    fillNoise (b[0], 1.0f, 1);
    fillNoise (b[1], 1.0f, 2);
    int sinceChange = 1 << 20;
    forEachBlock (b.view(), blockSize,
                  [&] (AudioView v, int)
                  {
                      if ((sinceChange += v.numFrames()) >= 1500)
                      {
                          eq.setSettings (randomSettings (rng));
                          sinceChange = 0;
                      }
                      eq.process (v);
                  });
    CHECK (allFinite (b[0]));
    CHECK (allFinite (b[1]));
}

TEST_CASE ("GraphicEq measured gain at each band centre matches responseDb", "[timefx][eq]")
{
    const double fs = GENERATE (from_range (kTestRates));
    Random rng (5);
    for (int trial = 0; trial < 3; ++trial)
    {
        GraphicEq::Settings s = randomSettings (rng);
        // Keep the cuts away from the band centres being measured: the Hann-windowed tone
        // estimate is accurate, but a steep skirt right at a band would make 0.1 dB a coin toss.
        s.lowCutHz = std::min (s.lowCutHz, 40.0f);
        s.highCutHz = 20000.0f;
        for (float hz : GraphicEq::kBandHz)
        {
            if (hz > 0.45 * fs)
                continue;
            GraphicEq eq;
            eq.prepare ({ fs, 512, 1 });
            eq.setSettings (s);
            INFO ("fs " << fs << " band " << hz);
            CHECK (processedGainDb (eq, hz, fs) ==
                   Approx (GraphicEq::responseDb (s, fs, hz)).margin (0.1));
        }
    }
}

TEST_CASE ("GraphicEq with flat settings is transparent", "[timefx][eq]")
{
    const double fs = GENERATE (from_range (kTestRates));
    GraphicEq eq;
    eq.prepare ({ fs, 512, 2 });
    eq.setSettings ({});

    Buffer b (2, 4096);
    fillNoise (b[0], 1.0f, 3);
    fillNoise (b[1], 1.0f, 4);
    const auto dry = b.data;
    forEachBlock (b.view(), 100, [&] (AudioView v, int) { eq.process (v); });
    CHECK (b.data == dry); // every filter is out of the path: bit-exact

    for (double hz : { 30.0, 100.0, 1000.0, 6000.0, 15000.0 })
        CHECK (processedGainDb (eq, hz, fs) == Approx (0.0).margin (0.01));
    for (float hz : { 10.0f, 20.0f, 1000.0f, 20000.0f })
        CHECK (GraphicEq::responseDb ({}, fs, hz) == 0.0f);
}

TEST_CASE ("GraphicEq cuts are bypassed exactly at their extremes", "[timefx][eq]")
{
    const double fs = 48000.0;
    GraphicEq::Settings s;
    s.lowCutHz = 20.0f;
    s.highCutHz = 20000.0f;
    CHECK (GraphicEq::responseDb (s, fs, 20.0f) == 0.0f);
    CHECK (GraphicEq::responseDb (s, fs, 20000.0f) == 0.0f);

    // Just inside the range the Butterworth sections engage: -3 dB at the corner.
    s.lowCutHz = 21.0f;
    s.highCutHz = 19000.0f;
    CHECK (GraphicEq::responseDb (s, fs, 21.0f) == Approx (-3.01).margin (0.02));
    CHECK (GraphicEq::responseDb (s, fs, 19000.0f) == Approx (-3.01).margin (0.05));

    // Bands on, cuts at the extremes: the response is the bands alone.
    GraphicEq::Settings bands;
    bands.bandsDb[4] = 6.0f;
    for (double hz : { 20.0, 100.0, 1000.0, 19000.0 })
        CHECK (GraphicEq::responseDb (bands, fs, static_cast<float> (hz)) ==
               Approx (designDb (design::peaking (fs, 1000.0, GraphicEq::kBandQ, 6.0), hz, fs))
                   .margin (1e-4));
}

TEST_CASE ("GraphicEq responseDb is the sum of the RBJ section magnitudes", "[timefx][eq]")
{
    const double fs = GENERATE (from_range (kTestRates));
    GraphicEq::Settings s;
    s.bandsDb = { 3.0f, -6.0f, 0.0f, 12.0f, -12.0f, 0.0f, 4.5f, -2.0f, 7.0f };
    s.lowCutHz = 90.0f;
    s.highCutHz = 9000.0f;
    for (double hz : { 25.0, 65.0, 90.0, 333.0, 1000.0, 4000.0, 9000.0, 15000.0 })
    {
        double expected = designDb (design::highpass (fs, 90.0), hz, fs) +
                          designDb (design::lowpass (fs, 9000.0), hz, fs);
        for (size_t b = 0; b < s.bandsDb.size(); ++b)
            if (s.bandsDb[b] != 0.0f)
                expected += designDb (
                    design::peaking (
                        fs, std::min (static_cast<double> (GraphicEq::kBandHz[b]), 0.45 * fs),
                        GraphicEq::kBandQ, s.bandsDb[b]),
                    hz, fs);
        CHECK (GraphicEq::responseDb (s, fs, static_cast<float> (hz)) ==
               Approx (expected).margin (1e-3));
    }
    // A lone cut is -3 dB at its corner (Butterworth).
    GraphicEq::Settings cut;
    cut.lowCutHz = 100.0f;
    CHECK (GraphicEq::responseDb (cut, fs, 100.0f) == Approx (-3.01).margin (0.01));
}

TEST_CASE ("GraphicEq keeps the 16 kHz band and the cuts stable at 32 kHz", "[timefx][eq]")
{
    const double fs = 32000.0;
    GraphicEq::Settings s;
    s.bandsDb[8] = 12.0f;
    s.highCutHz = 19000.0f;
    GraphicEq eq;
    eq.prepare ({ fs, 512, 2 });
    eq.setSettings (s);

    Buffer b (2, 32000);
    fillNoise (b[0], 1.0f, 8);
    fillNoise (b[1], 1.0f, 9);
    forEachBlock (b.view(), 512, [&] (AudioView v, int) { eq.process (v); });
    CHECK (allFinite (b[0]));
    CHECK (peak (b[0]) < 20.0f);

    // The band centre is clamped to 0.45 fs = 14.4 kHz, in the processor and in responseDb alike.
    const float r = GraphicEq::responseDb (s, fs, 14400.0f);
    CHECK (std::isfinite (r));
    GraphicEq eq2;
    eq2.prepare ({ fs, 512, 1 });
    eq2.setSettings (s);
    CHECK (processedGainDb (eq2, 10000.0, fs) ==
           Approx (GraphicEq::responseDb (s, fs, 10000.0f)).margin (0.1));
}

TEST_CASE ("GraphicEq processes mono and stereo identically per channel", "[timefx][eq]")
{
    Random rng (21);
    const auto s = randomSettings (rng);
    GraphicEq mono, stereo;
    mono.prepare ({ 48000.0, 512, 1 });
    stereo.prepare ({ 48000.0, 512, 2 });
    mono.setSettings (s);
    stereo.setSettings (s);

    Buffer m (1, 3000), st (2, 3000);
    fillNoise (m[0], 0.5f, 5);
    st[0] = m[0];
    st[1] = m[0];
    forEachBlock (m.view(), 256, [&] (AudioView v, int) { mono.process (v); });
    forEachBlock (st.view(), 256, [&] (AudioView v, int) { stereo.process (v); });
    CHECK (st[0] == m[0]);
    CHECK (st[1] == m[0]);
}

TEST_CASE ("GraphicEq automation is smoothed and click-free", "[timefx][eq]")
{
    const double fs = 48000.0;
    const int blockSize = GENERATE (1, 17, 64, 512);
    GraphicEq eq;
    eq.prepare ({ fs, 512, 2 });
    eq.setSettings ({});

    Buffer b (2, static_cast<int> (fs * 2.0));
    fillSine (b[0], 110.0, fs, 0.25f);
    b[1] = b[0];
    Random rng (17);
    int sinceChange = 0;
    forEachBlock (b.view(), blockSize,
                  [&] (AudioView v, int)
                  {
                      if ((sinceChange += v.numFrames()) >= 4800)
                      {
                          // Worst-case jumps: every band to a random extreme, cuts flipping between
                          // bypass and their far end.
                          GraphicEq::Settings s;
                          for (auto& g : s.bandsDb)
                              g = rng.unipolar() < 0.5f ? -12.0f : 12.0f;
                          s.lowCutHz = rng.unipolar() < 0.5f ? 20.0f : 500.0f;
                          s.highCutHz = rng.unipolar() < 0.5f ? 20000.0f : 1000.0f;
                          eq.setSettings (s);
                          sinceChange = 0;
                      }
                      eq.process (v);
                  });
    CHECK (allFinite (b[0]));
    // A 110 Hz tone at up to ~7x the input level has a second difference near 4e-4; stepping the
    // coefficients once per control block instead of ramping them measured 0.02-0.05 here.
    CHECK (maxSecondDifference (b[0]) < 2.0e-3f);
    CHECK (b[0] == b[1]);
}

TEST_CASE ("GraphicEq reaches its target after the glide", "[timefx][eq]")
{
    const double fs = 48000.0;
    GraphicEq eq;
    eq.prepare ({ fs, 512, 1 });
    eq.setSettings ({});
    GraphicEq::Settings s;
    s.bandsDb[4] = 9.0f;
    s.lowCutHz = 200.0f;
    eq.setSettings (s); // glides, not a snap
    Buffer warm (1, 9600);
    forEachBlock (warm.view(), 64, [&] (AudioView v, int) { eq.process (v); });
    CHECK (processedGainDb (eq, 1000.0, fs) ==
           Approx (GraphicEq::responseDb (s, fs, 1000.0f)).margin (0.05));
    CHECK (processedGainDb (eq, 200.0, fs) ==
           Approx (GraphicEq::responseDb (s, fs, 200.0f)).margin (0.1));
}

TEST_CASE ("GraphicEq silence in is silence out", "[timefx][eq]")
{
    Random rng (3);
    GraphicEq eq;
    eq.prepare ({ 44100.0, 512, 2 });
    eq.setSettings (randomSettings (rng));
    Buffer b (2, 4000);
    forEachBlock (b.view(), 17,
                  [&] (AudioView v, int)
                  {
                      eq.setSettings (randomSettings (rng));
                      eq.process (v);
                  });
    CHECK (peak (b[0]) == 0.0f);
    CHECK (peak (b[1]) == 0.0f);
}

TEST_CASE ("GraphicEq benchmark", "[.bench][timefx-bench]")
{
    GraphicEq eq;
    eq.prepare ({ 48000.0, 64, 2 });
    GraphicEq::Settings s;
    s.bandsDb = { 3.0f, -2.0f, 1.0f, 4.0f, -3.0f, 2.0f, 5.0f, -1.0f, 2.0f };
    s.lowCutHz = 80.0f;
    s.highCutHz = 9000.0f;
    eq.setSettings (s);
    benchmarkNsPerFrame ("GraphicEq (all 11 sections)", 10.0,
                         [&] (AudioView v) { eq.process (v); });
}
