// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ModsTestUtils.h"
#include "sedef/dsp/Gyre.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace sedef;
using Catch::Approx;
using dsp::Gyre;

namespace
{
constexpr double kFs = 48000.0;

void run (Gyre& g, AudioView v)
{
    g.process (v);
}

/// Where N matched first-order all-passes at fc, mixed with the dry signal, put their notches:
/// the total phase -2N atan(tan(pi f / fs) / tan(pi fc / fs)) crosses an odd multiple of pi.
std::vector<double> expectedNotches (int stages, double fc, double fs)
{
    std::vector<double> f;
    for (int m = 0; 2 * m + 1 < stages; ++m)
        f.push_back (fs / kPiD *
                     std::atan (std::tan (kPiD * fc / fs) *
                                std::tan ((2.0 * m + 1.0) * kPiD / (2.0 * stages))));
    return f;
}

/// The break frequency over `seconds` of running at the given settings.
std::vector<float> sweepTrajectory (const Gyre::Settings& s, double fs, double seconds)
{
    Gyre g;
    g.prepare ({ fs, 512, 1 });
    g.setSettings (s);
    std::vector<float> hz;
    test::Buffer b (1, static_cast<int> (seconds * fs));
    for (int i = 0; i < b.frames(); ++i)
    {
        g.process (b.view().subView (i, 1));
        hz.push_back (g.breakHz());
    }
    return hz;
}

/// Mean period (samples) between the trajectory's maxima.
double meanPeriod (const std::vector<float>& v)
{
    std::vector<size_t> peaks;
    for (size_t i = 1; i + 1 < v.size(); ++i)
        if (v[i] > v[i - 1] && v[i] >= v[i + 1])
            peaks.push_back (i);
    REQUIRE (peaks.size() >= 3);
    return static_cast<double> (peaks.back() - peaks.front()) /
           static_cast<double> (peaks.size() - 1);
}
} // namespace

TEST_CASE ("Gyre output stays finite for random settings and full-scale noise", "[pedals][gyre]")
{
    test::checkFiniteWithRandomSettings<Gyre> (
        [] (Random& r) {
            return Gyre::Settings { 0.1f + 7.9f * r.unipolar(), r.unipolar(), r.unipolar(),
                                    static_cast<int> (r.nextU32() % 2u) };
        },
        run);
}

TEST_CASE ("Gyre passes silence as silence", "[pedals][gyre]")
{
    for (int stages : { 0, 1 })
        CHECK (test::silenceOutputPeak<Gyre> (Gyre::Settings { 8.0f, 1.0f, 1.0f, stages }, run) ==
               0.0f);
}

TEST_CASE ("Gyre cuts two notches with four stages and three with six", "[pedals][gyre]")
{
    // Depth 0 holds the stages at the centre (600 Hz). Four stages: notches at fc tan(pi/8) and
    // fc tan(3pi/8) (249 Hz and 1.45 kHz); six: fc tan(pi/12), fc, fc tan(5pi/12). Positions
    // within 1 %, every notch deeper than 40 dB, at every supported rate.
    const auto f = test::logFrequencies (40.0, 15000.0, 192);
    for (double fs : test::kAllSampleRates)
        for (int stages : { Gyre::Four, Gyre::Six })
        {
            const int n = stages == Gyre::Four ? 4 : 6;
            INFO ("fs " << fs << " stages " << n);
            const auto h = test::impulseResponse<Gyre> ({ 1.0f, 0.0f, 0.0f, stages }, fs, 1 << 14);
            const auto r = test::responseDb (h, f, fs);
            const auto notches = test::findNotches (r, f, 20.0);
            const auto expected = expectedNotches (n, Gyre::kCentreHz, fs);
            REQUIRE (notches.size() == expected.size());
            for (size_t i = 0; i < notches.size(); ++i)
            {
                CHECK (notches[i].hz == Approx (expected[i]).epsilon (0.01));
                CHECK (notches[i].db < -40.0);
            }
        }
}

TEST_CASE ("Gyre feedback adds peaks between the notches but leaves the notches in place",
           "[pedals][gyre]")
{
    const auto f = test::logFrequencies (40.0, 15000.0, 192);
    const auto dry = test::responseDb (
        test::impulseResponse<Gyre> ({ 1.0f, 0.0f, 0.0f, Gyre::Four }, kFs, 1 << 15), f, kFs);
    const auto wet = test::responseDb (
        test::impulseResponse<Gyre> ({ 1.0f, 0.0f, 1.0f, Gyre::Four }, kFs, 1 << 15), f, kFs);
    const auto a = test::findNotches (dry, f, 20.0);
    const auto b = test::findNotches (wet, f, 20.0);
    REQUIRE (a.size() == 2);
    REQUIRE (b.size() == 2);
    for (size_t i = 0; i < 2; ++i)
        CHECK (b[i].hz == Approx (a[i].hz).epsilon (0.01));
    // The peak between the two notches rises by about 1 / (1 - 0.85): +16.5 dB, less the trim,
    // to about +15 dB.
    const auto between = [&] (const std::vector<double>& r) {
        double best = -200.0;
        for (size_t i = 0; i < f.size(); ++i)
            if (f[i] > a[0].hz && f[i] < a[1].hz)
                best = std::max (best, r[i]);
        return best;
    };
    CHECK (between (wet) - between (dry) > 8.0);
    CHECK (between (wet) < 16.0);
}

TEST_CASE ("Gyre's LFO runs at the Speed and sweeps two octaves either way at full Depth",
           "[pedals][gyre]")
{
    for (double fs : { 44100.0, 96000.0, 192000.0 })
        for (float speed : { 0.1f, 1.0f, 8.0f })
        {
            INFO ("fs " << fs << " speed " << speed);
            const double seconds = std::max (4.0, 3.5 / speed);
            const auto hz = sweepTrajectory ({ speed, 1.0f, 0.0f, Gyre::Four }, fs, seconds);
            CHECK (fs / meanPeriod (hz) == Approx (speed).epsilon (0.01));
            const auto [lo, hi] = std::minmax_element (hz.begin(), hz.end());
            CHECK (*lo == Approx (Gyre::kCentreHz / 4.0f).epsilon (0.01));
            CHECK (*hi == Approx (Gyre::kCentreHz * 4.0f).epsilon (0.01));
        }
    // Depth scales the sweep in octaves: half depth, one octave either way.
    const auto half = sweepTrajectory ({ 2.0f, 0.5f, 0.0f, Gyre::Four }, kFs, 2.0);
    const auto [lo, hi] = std::minmax_element (half.begin(), half.end());
    CHECK (*hi / *lo == Approx (4.0f).epsilon (0.01));

    // In the audio: a 150 Hz tone dips as the low notch passes it, once on the way up and once
    // on the way down, and the pattern repeats at the LFO period.
    const float speed = 0.5f;
    Gyre g;
    g.prepare ({ kFs, 512, 1 });
    g.setSettings ({ speed, 1.0f, 0.0f, Gyre::Four });
    test::Buffer b (1, static_cast<int> (8.0 * kFs));
    test::fillSine (b[0], 150.0, kFs, 0.25f);
    test::processInBlocks (b, 512, [&] (AudioView v) { g.process (v); });
    const auto env = test::rmsEnvelope (b[0], 480); // 10 ms windows
    std::vector<size_t> dips;
    for (size_t i = 1; i + 1 < env.size(); ++i)
        if (env[i] < env[i - 1] && env[i] <= env[i + 1] && env[i] < 0.1f * test::peak (env))
            dips.push_back (i);
    REQUIRE (dips.size() >= 6);
    CHECK (dips.size() <= 8);
    // Every second dip is one LFO period later.
    for (size_t i = 2; i < dips.size(); ++i)
        CHECK (static_cast<double> (dips[i] - dips[i - 2]) * 0.01 ==
               Approx (1.0 / speed).margin (0.02));
}

TEST_CASE ("Gyre is stable at full feedback", "[pedals][gyre]")
{
    for (double fs : test::kAllSampleRates)
        for (int stages : { 0, 1 })
        {
            INFO ("fs " << fs << " stages " << stages);
            Gyre g;
            g.prepare ({ fs, 512, 1 });
            g.setSettings ({ 8.0f, 1.0f, 1.0f, stages });
            test::Buffer b (1, static_cast<int> (1.0 * fs));
            std::vector<float> noise (static_cast<size_t> (0.5 * fs));
            test::fillNoise (noise, 1.0f, 9);
            std::copy (noise.begin(), noise.end(), b[0].begin());
            test::processInBlocks (b, 512, [&] (AudioView v) { g.process (v); });
            REQUIRE (test::allFinite (b[0]));
            CHECK (test::peak (b[0]) < 6.0f);
            const auto tail = static_cast<int> (0.8 * fs);
            CHECK (test::rms (b[0].data() + tail, b.frames() - tail) < 1.0e-6f);
        }
}

TEST_CASE ("Gyre keeps a guitar near unity across Feedback", "[pedals][gyre]")
{
    for (int stages : { 0, 1 })
        for (float fb : { 0.0f, 0.5f, 1.0f })
        {
            INFO ("stages " << stages << " feedback " << fb);
            CHECK (test::guitarGainDb<Gyre> ({ 0.5f, 0.8f, fb, stages }, kFs, 4.0) ==
                   Approx (0.0).margin (2.5));
        }
}

TEST_CASE ("Gyre automation and stage switches are click-free", "[pedals][gyre]")
{
    const auto check = [] (auto settingsAt) {
        const auto r = test::measureSweep<Gyre> (settingsAt, run, kFs, 220.0);
        CHECK (r.finite);
        CHECK (r.sweep <= 1.5f * r.reference);
    };
    for (int stages : { 0, 1 })
    {
        check (
            [stages] (float t) { return Gyre::Settings { 0.1f + 7.9f * t, 0.8f, 0.5f, stages }; });
        check ([stages] (float t) { return Gyre::Settings { 1.0f, t, 0.5f, stages }; });
        check ([stages] (float t) { return Gyre::Settings { 1.0f, 0.8f, t, stages }; });
    }
    check ([] (float t) {
        return Gyre::Settings { 1.0f, 0.8f, 0.5f, static_cast<int> (t * 3.99f) % 2 };
    });
}

TEST_CASE ("Gyre is linear and block-size independent", "[pedals][gyre]")
{
    for (int stages : { 0, 1 })
    {
        const int frames = 24000;
        test::Buffer b (1, frames);
        test::fillSine (b[0], 1318.5, kFs, 0.9f);
        Gyre g;
        g.prepare ({ kFs, 512, 1 });
        g.setSettings ({ 1.0f, 0.0f, 0.8f, stages }); // static, so the spectrum is the tone's
        test::processInBlocks (b, 512, [&] (AudioView v) { g.process (v); });
        const float* tail = b[0].data() + frames / 2;
        CHECK (test::thd (tail, frames / 2, 1318.5, kFs) < 1.0e-4);
        CHECK (test::worstInharmonicDb (tail, frames / 2, 1318.5, kFs) < -75.0);
        CHECK (test::blockSizeSpread<Gyre> ({ 3.0f, 0.9f, 0.7f, stages }, kFs) < 1.0e-6f);
    }
}
