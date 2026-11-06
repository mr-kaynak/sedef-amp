// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ModsTestUtils.h"
#include "sedef/dsp/Beacon.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <complex>

using namespace sedef;
using Catch::Approx;
using dsp::Beacon;

namespace
{
constexpr double kFs = 48000.0;
constexpr double kIdleLight = 0.25; // the lamp's idle drive (half current) squared

void run (Beacon& b, AudioView v)
{
    b.process (v);
}

/// The photocells' light over `seconds`.
std::vector<float> lightTrajectory (const Beacon::Settings& s, double fs, double seconds)
{
    Beacon b;
    b.prepare ({ fs, 512, 1 });
    b.setSettings (s);
    std::vector<float> light;
    test::Buffer buf (1, static_cast<int> (seconds * fs));
    for (int i = 0; i < buf.frames(); ++i)
    {
        b.process (buf.view().subView (i, 1));
        light.push_back (b.light());
    }
    return light;
}

struct Cycle
{
    double period = 0.0;       ///< samples between maxima
    double riseFraction = 0.0; ///< share of the cycle spent getting brighter (minimum to maximum)
};

Cycle measureCycle (const std::vector<float>& v, size_t start)
{
    std::vector<size_t> maxima, minima;
    for (size_t i = std::max<size_t> (start, 1); i + 1 < v.size(); ++i)
    {
        if (v[i] > v[i - 1] && v[i] >= v[i + 1])
            maxima.push_back (i);
        if (v[i] < v[i - 1] && v[i] <= v[i + 1])
            minima.push_back (i);
    }
    REQUIRE (maxima.size() >= 3);
    Cycle c;
    c.period = static_cast<double> (maxima.back() - maxima.front()) /
               static_cast<double> (maxima.size() - 1);
    double rise = 0.0;
    int count = 0;
    for (size_t m : minima)
    {
        const auto next = std::upper_bound (maxima.begin(), maxima.end(), m);
        if (next == maxima.end())
            break;
        rise += static_cast<double> (*next - m);
        ++count;
    }
    REQUIRE (count >= 2);
    c.riseFraction = rise / count / c.period;
    return c;
}

/// Chorus-mode response of the four analog stages at a light level: each stage is
/// (alpha + beta) w / (s + w) - beta, the measured phase splitter's legs; the chorus mix is dry
/// plus stages in equal parts.
double analogChorusDb (double hz, double light)
{
    constexpr double alpha[] = { 1.01, 0.98, 0.97, 0.95 };
    constexpr double beta[] = { 1.11, 1.09, 1.10, 1.09 };
    const std::complex<double> s (0.0, kTwoPiD * hz);
    std::complex<double> h (1.0, 0.0);
    for (int k = 0; k < Beacon::kNumStages; ++k)
    {
        const double w = kTwoPiD * Beacon::stageHz (k, light);
        h *= (alpha[k] + beta[k]) * w / (s + w) - beta[k];
    }
    return 20.0 * std::log10 (std::abs (1.0 + h));
}
} // namespace

TEST_CASE ("Beacon output stays finite for random settings and full-scale noise",
           "[pedals][beacon]")
{
    test::checkFiniteWithRandomSettings<Beacon> (
        [] (Random& r) {
            return Beacon::Settings { 0.3f + 7.7f * r.unipolar(), r.unipolar(),
                                      static_cast<int> (r.nextU32() % 2u) };
        },
        run);
}

TEST_CASE ("Beacon passes silence as silence", "[pedals][beacon]")
{
    for (int mode : { 0, 1 })
        CHECK (test::silenceOutputPeak<Beacon> (Beacon::Settings { 8.0f, 1.0f, mode }, run) ==
               0.0f);
}

TEST_CASE ("Beacon's stages are staggered over decades", "[pedals][beacon]")
{
    // Each stage has its own capacitor (15 nF, 220 nF, 470 pF, 4.7 nF) and its own cell, so at
    // any light their break frequencies spread over about 8.6 octaves: at idle 276 Hz, 35 Hz,
    // 14.6 kHz and 1.8 kHz. All four move together (one lamp), keeping their ratios.
    CHECK (Beacon::stageHz (0, kIdleLight) == Approx (276.3).epsilon (0.01));
    CHECK (Beacon::stageHz (1, kIdleLight) == Approx (34.8).epsilon (0.01));
    CHECK (Beacon::stageHz (2, kIdleLight) == Approx (14540.0).epsilon (0.01));
    CHECK (Beacon::stageHz (3, kIdleLight) == Approx (1798.0).epsilon (0.01));
    for (int k = 0; k < 4; ++k)
        CHECK (Beacon::stageHz (k, 1.0) / Beacon::stageHz (k, 0.05) ==
               Approx (std::pow (20.0, 0.8)).epsilon (1e-6));

    // The chorus mix's measured notches are the ones the analog staggered stages put there:
    // two notches (four stages), at idle 85 Hz and 5.2 kHz: almost six octaves apart, where four
    // matched stages' are 2.5. The low one is deep; the high one is shallower, because the
    // unbalanced phase splitters are not quite all-pass at the top. Within 1 % of the analog
    // circuit at every rate for the low notch; the high one, near 5 kHz, carries the bilinear map's
    // frequency warping (6 % at 44.1 kHz, 1.5 % at 96 kHz).
    const auto f = test::logFrequencies (20.0, 20000.0, 192);
    std::vector<double> analog;
    for (double hz : f)
        analog.push_back (analogChorusDb (hz, kIdleLight));
    const auto expected = test::findNotches (analog, f, 10.0);
    REQUIRE (expected.size() == 2);
    CHECK (expected[0].hz == Approx (85.0).epsilon (0.01));
    CHECK (expected[1].hz == Approx (5195.0).epsilon (0.01));
    for (double fs : test::kAllSampleRates)
    {
        INFO ("fs " << fs);
        const auto h = test::impulseResponse<Beacon> ({ 2.0f, 0.0f, Beacon::Chorus }, fs, 1 << 15);
        const auto measured = test::findNotches (test::responseDb (h, f, fs), f, 10.0);
        REQUIRE (measured.size() == 2);
        INFO ("notches at " << measured[0].hz << " Hz (" << measured[0].db << " dB) and "
                            << measured[1].hz << " Hz (" << measured[1].db << " dB)");
        CHECK (measured[0].hz == Approx (expected[0].hz).epsilon (0.01));
        CHECK (measured[1].hz == Approx (expected[1].hz).epsilon (fs < 80000.0 ? 0.07 : 0.02));
        CHECK (measured[0].db < -25.0);
        CHECK (measured[1].db < -10.0);
        CHECK (std::log2 (measured[1].hz / measured[0].hz) > 5.5);
    }
}

TEST_CASE ("Beacon's lamp makes a lopsided throb, not a sine", "[pedals][beacon]")
{
    // The LFO is a sine; the lamp heats faster than it cools and the cells light faster than
    // they darken, so the light rises in well under half the cycle and fades over the rest.
    for (float speed : { 0.5f, 2.0f, 6.0f })
    {
        const auto light =
            lightTrajectory ({ speed, 1.0f, Beacon::Chorus }, kFs, 3.0 + 6.0 / speed);
        const auto c = measureCycle (light, static_cast<size_t> (2.0 * kFs));
        INFO ("speed " << speed << ": rises in " << c.riseFraction << " of the cycle");
        CHECK (kFs / c.period == Approx (speed).epsilon (0.01));
        CHECK (c.riseFraction < 0.43);
        CHECK (c.riseFraction > 0.25);
    }

    // Measured as a waveform: the light against its own fundamental. The sine LFO that drives
    // it has no second harmonic; the throb's is within 26 dB of the fundamental.
    const float speed = 1.0f;
    const auto light = lightTrajectory ({ speed, 1.0f, Beacon::Chorus }, kFs, 6.0);
    const auto from = static_cast<int> (2.0 * kFs);
    const auto n = static_cast<int> (4.0 * kFs);
    std::vector<float> cycle (light.begin() + from, light.begin() + from + n);
    double mean = 0.0;
    for (float x : cycle)
        mean += x;
    mean /= n;
    for (auto& x : cycle)
        x -= static_cast<float> (mean);
    const double h1 = test::toneMagnitude (cycle.data(), n, speed, kFs);
    const double h2 = test::toneMagnitude (cycle.data(), n, 2.0 * speed, kFs);
    CHECK (20.0 * std::log10 (h2 / h1) > -26.0);

    // Heard as pitch, in Vibrato: the fast brightening bends a tone up harder than the slow fade
    // bends it down.
    Beacon b;
    b.prepare ({ kFs, 512, 1 });
    b.setSettings ({ speed, 1.0f, Beacon::Vibrato });
    test::Buffer tone (1, static_cast<int> (6.0 * kFs));
    test::fillSine (tone[0], 500.0, kFs, 0.3f);
    test::processInBlocks (tone, 512, [&] (AudioView v) { b.process (v); });
    const auto t = test::risingZeroCrossings (tone[0], static_cast<size_t> (2.0 * kFs));
    double up = 0.0, down = 0.0;
    for (size_t i = 1; i < t.size(); ++i)
    {
        const double hz = kFs / (t[i] - t[i - 1]);
        up = std::max (up, hz - 500.0);
        down = std::max (down, 500.0 - hz);
    }
    INFO ("bends up " << up << " Hz, down " << down << " Hz");
    CHECK (up > 1.3 * down);
    CHECK (down > 0.5);
}

TEST_CASE ("Beacon's Depth sets how far the lamp swings", "[pedals][beacon]")
{
    const auto swing = [] (float depth) {
        const auto l = lightTrajectory ({ 2.0f, depth, Beacon::Chorus }, kFs, 4.0);
        const auto [lo, hi] =
            std::minmax_element (l.begin() + static_cast<long> (2.0 * kFs), l.end());
        return std::pair { *lo, *hi };
    };
    const auto off = swing (0.0f);
    CHECK (off.first == Approx (kIdleLight).margin (1e-4));
    CHECK (off.second == Approx (kIdleLight).margin (1e-4));
    const auto half = swing (0.5f);
    const auto full = swing (1.0f);
    CHECK (half.second - half.first > 0.3f);
    CHECK (full.second - full.first > half.second - half.first);
    // At full depth the first stage sweeps from below 100 Hz to above 700 Hz.
    CHECK (Beacon::stageHz (0, full.first) < 100.0);
    CHECK (Beacon::stageHz (0, full.second) > 700.0);
}

TEST_CASE ("Beacon's Chorus mode cuts moving notches; Vibrato only wobbles the pitch",
           "[pedals][beacon]")
{
    const auto render = [] (int mode, double hz) {
        Beacon b;
        b.prepare ({ kFs, 512, 1 });
        b.setSettings ({ 1.0f, 1.0f, mode });
        test::Buffer t (1, static_cast<int> (5.0 * kFs));
        test::fillSine (t[0], hz, kFs, 0.3f);
        test::processInBlocks (t, 512, [&] (AudioView v) { b.process (v); });
        return t[0];
    };
    for (double hz : { 150.0, 400.0, 1000.0 })
    {
        const auto range = [&] (int mode) {
            const auto env = test::rmsEnvelope (render (mode, hz), 960, static_cast<size_t> (kFs));
            const auto [lo, hi] = std::minmax_element (env.begin(), env.end());
            return test::db (*hi / std::max (*lo, 1e-9f));
        };
        INFO (hz << " Hz");
        CHECK (range (Beacon::Chorus) > 12.0); // a notch passes through the tone
        CHECK (range (Beacon::Vibrato) < 3.0); // near all-pass: only the stages' small shelf
    }

    // Both modes play at the same level, and vibrato bends the pitch.
    CHECK (test::guitarGainDb<Beacon> ({ 2.0f, 0.7f, Beacon::Chorus }, kFs, 4.0) ==
           Approx (0.0).margin (1.0));
    CHECK (test::guitarGainDb<Beacon> ({ 2.0f, 0.7f, Beacon::Vibrato }, kFs, 4.0) ==
           Approx (0.0).margin (1.0));
    const auto vib = render (Beacon::Vibrato, 500.0);
    const auto t = test::risingZeroCrossings (vib, static_cast<size_t> (kFs));
    double lo = 1e9, hi = 0.0;
    for (size_t i = 1; i < t.size(); ++i)
    {
        lo = std::min (lo, t[i] - t[i - 1]);
        hi = std::max (hi, t[i] - t[i - 1]);
    }
    CHECK ((hi - lo) / (kFs / 500.0) > 0.003);
}

TEST_CASE ("Beacon automation and mode switches are click-free", "[pedals][beacon]")
{
    const auto check = [] (auto settingsAt) {
        const auto r = test::measureSweep<Beacon> (settingsAt, run, kFs, 220.0);
        CHECK (r.finite);
        CHECK (r.sweep <= 1.5f * r.reference);
    };
    for (int mode : { 0, 1 })
    {
        check ([mode] (float t) { return Beacon::Settings { 0.3f + 7.7f * t, 0.7f, mode }; });
        check ([mode] (float t) { return Beacon::Settings { 2.0f, t, mode }; });
    }
    check (
        [] (float t) { return Beacon::Settings { 2.0f, 0.7f, static_cast<int> (t * 3.99f) % 2 }; });
}

TEST_CASE ("Beacon is linear and block-size independent", "[pedals][beacon]")
{
    for (int mode : { 0, 1 })
    {
        const int frames = 24000;
        test::Buffer b (1, frames);
        test::fillSine (b[0], 1318.5, kFs, 0.9f);
        Beacon v;
        v.prepare ({ kFs, 512, 1 });
        v.setSettings ({ 2.0f, 0.0f, mode }); // lamp at idle: a static filter
        test::processInBlocks (b, 512, [&] (AudioView view) { v.process (view); });
        const float* tail = b[0].data() + frames / 2;
        CHECK (test::thd (tail, frames / 2, 1318.5, kFs) < 1.0e-4);
        CHECK (test::worstInharmonicDb (tail, frames / 2, 1318.5, kFs) < -75.0);
        CHECK (test::blockSizeSpread<Beacon> ({ 3.0f, 0.8f, mode }, kFs) < 1.0e-6f);
    }
}
