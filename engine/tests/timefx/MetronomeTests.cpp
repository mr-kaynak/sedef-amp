// SPDX-License-Identifier: AGPL-3.0-or-later
#include "TimeFxTestUtils.h"
#include "sedef/dsp/Metronome.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>
#include <vector>

using namespace sedef;
using namespace sedef::test;
using sedef::dsp::Metronome;

namespace
{
Metronome::Settings allSteps (std::uint8_t state, int signature = 2, float bpm = 120.0f,
                              int sound = 0)
{
    Metronome::Settings s;
    s.enabled = true;
    s.signature = signature;
    s.bpm = bpm;
    s.sound = sound;
    s.steps.fill (state);
    return s;
}

/// Indices where a click starts: non-zero after an exact zero (voices end in exact silence).
std::vector<long long> onsets (const std::vector<float>& v)
{
    std::vector<long long> out;
    for (size_t i = 0; i < v.size(); ++i)
        if (v[i] != 0.0f && (i == 0 || v[i - 1] == 0.0f))
            out.push_back (static_cast<long long> (i));
    return out;
}

/// Processes with a deterministic mix of odd block sizes.
void runOddBlocks (Metronome& m, Buffer& b, std::uint32_t seed)
{
    Random rng (seed);
    for (int start = 0; start < b.frames();)
    {
        const int len =
            std::min (b.frames() - start, 1 + static_cast<int> (rng.unipolar() * 700.0f));
        m.process (b.view().subView (start, len));
        start += len;
    }
}
} // namespace

TEST_CASE ("Metronome steps per bar follow the signature", "[timefx][metronome]")
{
    const int expected[] = { 2, 3, 4, 5, 6, 7, 9, 12 };
    for (int sig = 0; sig < 8; ++sig)
        CHECK (Metronome::stepsFor (sig) == expected[sig]);
    CHECK (Metronome::stepsFor (-1) == 2);
    CHECK (Metronome::stepsFor (42) == 12);
}

TEST_CASE ("Metronome clicks land on exact sample indices across odd block sizes",
           "[timefx][metronome]")
{
    const double fs = GENERATE (44100.0, 48000.0, 96000.0);
    const int signature = GENERATE (2, 5); // 4/4 counts quarters, 7/8 counts eighths
    const float bpm = 97.0f;
    Metronome m;
    m.prepare ({ fs, 512, 2 });
    m.setSettings (allSteps (Metronome::Normal, signature, bpm));

    Buffer b (2, static_cast<int> (fs * 6.0));
    runOddBlocks (m, b, 3);

    const double interval = fs * (signature >= 4 ? 30.0 : 60.0) / bpm;
    std::vector<long long> expected;
    for (long long k = 0;; ++k)
    {
        const auto at = static_cast<long long> (std::ceil (static_cast<double> (k) * interval));
        if (at >= b.frames())
            break;
        expected.push_back (at);
    }
    INFO ("fs " << fs << " signature " << signature);
    CHECK (onsets (b[0]) == expected);
    CHECK (b[0] == b[1]);
}

TEST_CASE ("Metronome Off steps are silent", "[timefx][metronome]")
{
    const double fs = 48000.0;
    Metronome m;
    m.prepare ({ fs, 512, 2 });
    auto s = allSteps (Metronome::Off, 2, 150.0f);
    s.steps[0] = Metronome::Accent;
    s.steps[2] = Metronome::Normal;
    m.setSettings (s);
    Buffer b (2, static_cast<int> (fs * 4.0));
    runOddBlocks (m, b, 9);

    const double interval = fs * 60.0 / 150.0;
    std::vector<long long> expected;
    for (long long k = 0; k * interval < b.frames(); ++k)
        if (k % 4 == 0 || k % 4 == 2)
            expected.push_back (
                static_cast<long long> (std::ceil (static_cast<double> (k) * interval)));
    CHECK (onsets (b[0]) == expected);
}

TEST_CASE ("Metronome accents are louder and soft steps quieter, for every sound",
           "[timefx][metronome]")
{
    const double fs = 48000.0;
    for (int sound = 0; sound < 6; ++sound)
    {
        Metronome m;
        m.prepare ({ fs, 512, 2 });
        auto s = allSteps (Metronome::Off, 2, 60.0f, sound);
        s.steps[0] = Metronome::Accent;
        s.steps[1] = Metronome::Normal;
        s.steps[2] = Metronome::Soft;
        m.setSettings (s);
        Buffer b (2, static_cast<int> (fs * 3.0));
        runOddBlocks (m, b, 1);
        const size_t step = static_cast<size_t> (fs);
        const float accent = peakIn (b[0], 0, step);
        const float normal = peakIn (b[0], step, 2 * step);
        const float soft = peakIn (b[0], 2 * step, 3 * step);
        INFO ("sound " << sound << " accent " << accent << " normal " << normal << " soft "
                       << soft);
        CHECK (accent > 0.2f);
        CHECK (accent < 0.8f);
        CHECK (accent > 1.3f * normal);
        CHECK (normal > 1.5f * soft);
        CHECK (soft > 0.02f);
        CHECK (allFinite (b[0]));
    }
}

TEST_CASE ("Metronome currentStep follows the playhead", "[timefx][metronome]")
{
    const double fs = 48000.0;
    Metronome m;
    m.prepare ({ fs, 512, 2 });
    CHECK (m.currentStep() == -1);

    auto s = allSteps (Metronome::Normal, 1, 200.0f); // 3/4
    s.steps[1] = Metronome::Off;                      // silent steps still move the playhead
    m.setSettings (s);
    const double interval = fs * 60.0 / 200.0;
    Buffer b (2, 64);
    long long processed = 0;
    for (int block = 0; block < 2000; ++block)
    {
        m.process (b.view());
        processed += 64;
        // Last step whose trigger sample has been processed.
        long long k = 0;
        while (std::ceil (static_cast<double> (k + 1) * interval) < static_cast<double> (processed))
            ++k;
        REQUIRE (m.currentStep() == static_cast<int> (k % 3));
    }

    s.enabled = false;
    m.setSettings (s);
    m.process (b.view());
    CHECK (m.currentStep() == -1);
}

TEST_CASE ("Metronome starts on the downbeat when enabled", "[timefx][metronome]")
{
    const double fs = 44100.0;
    Metronome m;
    m.prepare ({ fs, 512, 2 });
    auto s = allSteps (Metronome::Normal);
    s.steps[0] = Metronome::Accent;
    s.enabled = false;
    m.setSettings (s);

    Buffer b (2, 20000);
    m.process (b.view().subView (0, 1234)); // disabled: adds nothing
    s.enabled = true;
    m.setSettings (s);
    m.process (b.view().subView (1234, 20000 - 1234));
    const auto found = onsets (b[0]);
    REQUIRE (!found.empty());
    CHECK (found.front() == 1234);
    CHECK (m.currentStep() == 0);
}

TEST_CASE ("Metronome adds nothing while disabled and scales with volume", "[timefx][metronome]")
{
    const double fs = 48000.0;
    Metronome off;
    off.prepare ({ fs, 512, 2 });
    auto s = allSteps (Metronome::Accent);
    s.enabled = false;
    off.setSettings (s);
    Buffer noise (2, 48000);
    fillNoise (noise[0], 0.3f, 1);
    fillNoise (noise[1], 0.3f, 2);
    const auto dry = noise.data;
    runOddBlocks (off, noise, 4);
    CHECK (noise.data == dry);

    float peaks[2] {};
    for (int i = 0; i < 2; ++i)
    {
        Metronome m;
        m.prepare ({ fs, 512, 2 });
        s.enabled = true;
        s.volumeDb = i == 0 ? 0.0f : 6.0f;
        m.setSettings (s);
        Buffer b (2, 12000);
        runOddBlocks (m, b, 5);
        peaks[i] = peak (b[0]);
    }
    CHECK (peaks[1] / peaks[0] == Catch::Approx (dbToGain (6.0f)).epsilon (0.01));
}

TEST_CASE ("Metronome output is finite for random settings, rates and block sizes",
           "[timefx][metronome]")
{
    const double fs = GENERATE (from_range (kTestRates));
    const int blockSize = GENERATE (from_range (kTestBlockSizes));
    Metronome m;
    m.prepare ({ fs, 512, 2 });
    Random rng (66);
    Buffer b (2, static_cast<int> (fs * 2.0));
    fillNoise (b[0], 1.0f, 7);
    fillNoise (b[1], 1.0f, 8);
    int sinceChange = 1 << 20;
    forEachBlock (b.view(), blockSize,
                  [&] (AudioView v, int)
                  {
                      if ((sinceChange += v.numFrames()) >= 5000)
                      {
                          Metronome::Settings s;
                          s.enabled = rng.unipolar() < 0.8f;
                          s.volumeDb = mapLin (rng.unipolar(), -24.0f, 12.0f);
                          s.signature = static_cast<int> (rng.unipolar() * 8.0f);
                          s.sound = static_cast<int> (rng.unipolar() * 6.0f);
                          s.bpm = mapLin (rng.unipolar(), 40.0f, 400.0f);
                          for (auto& st : s.steps)
                              st = static_cast<std::uint8_t> (rng.unipolar() * 4.0f);
                          m.setSettings (s);
                          sinceChange = 0;
                      }
                      m.process (v);
                  });
    CHECK (allFinite (b[0]));
    CHECK (allFinite (b[1]));
    CHECK (peak (b[0]) < 4.0f);
}

TEST_CASE ("Metronome tempo changes keep a steady grid from the last click", "[timefx][metronome]")
{
    const double fs = 48000.0;
    Metronome m;
    m.prepare ({ fs, 512, 2 });
    auto s = allSteps (Metronome::Normal, 2, 60.0f);
    m.setSettings (s);
    Buffer b (2, static_cast<int> (fs * 5.0));
    m.process (b.view().subView (0, 60000)); // clicks at 0 and 48000
    s.bpm = 120.0f;
    m.setSettings (s);
    m.process (b.view().subView (60000, b.frames() - 60000));
    // Next click one new interval after the last one: 48000 + 24000, then every 24000.
    const auto found = onsets (b[0]);
    const std::vector<long long> expected { 0,      48000,  72000,  96000, 120000,
                                            144000, 168000, 192000, 216000 };
    CHECK (found == expected);
}

TEST_CASE ("Metronome benchmark", "[.bench][timefx-bench]")
{
    Metronome m;
    m.prepare ({ 48000.0, 64, 2 });
    auto s = allSteps (Metronome::Normal, 7, 240.0f, 3); // 12/8 cowbell: busiest case
    m.setSettings (s);
    benchmarkNsPerFrame ("Metronome (12/8 cowbell)", 10.0, [&] (AudioView v) { m.process (v); });
}
