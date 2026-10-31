// SPDX-License-Identifier: AGPL-3.0-or-later
#include "PedalTestUtils.h"
#include "sedef/dsp/Tide.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace sedef;
using Catch::Approx;
using dsp::Tide;

namespace
{
constexpr double kFs = 48000.0;

void run (Tide& t, AudioView v)
{
    t.process (v);
}

/// Steady-state output level (dB RMS) of a 1 kHz sine at the given peak level.
double outputDb (const Tide::Settings& s, double inputPeakDb, float* telemetry = nullptr)
{
    Tide t;
    t.prepare ({kFs, 512, 1});
    t.setSettings (s);
    test::Buffer b (1, static_cast<int> (0.6 * kFs));
    test::fillSine (b[0], 1000.0, kFs, static_cast<float> (std::pow (10.0, inputPeakDb / 20.0)));
    test::processInBlocks (b, 256, [&] (AudioView v) { t.process (v); });
    if (telemetry != nullptr)
        *telemetry = t.gainReductionDb();
    return test::db (test::rms (b[0], static_cast<size_t> (0.4 * kFs)));
}

/// Number of 64-sample blocks after a burst of `burstSeconds` until gain reduction is back
/// within 1 dB.
int recoveryBlocks (double burstSeconds)
{
    Tide t;
    t.prepare ({kFs, 64, 1});
    t.setSettings ({0.4f, 0.5f, 0.5f, 1.0f}); // about 11 dB of reduction
    const int burst = static_cast<int> (burstSeconds * kFs);
    test::Buffer b (1, burst + static_cast<int> (3.0 * kFs));
    test::fillSine (b[0], 300.0, kFs, 0.25f);
    std::fill (b[0].begin() + burst, b[0].end(), 0.0f);
    int blocks = 0;
    bool released = false;
    test::processInBlocks (b, 64,
                           [&] (AudioView v)
                           {
                               t.process (v);
                               if (released)
                                   return;
                               ++blocks;
                               if (blocks * 64 > burst && t.gainReductionDb() > -1.0f)
                                   released = true;
                           });
    return blocks - burst / 64;
}
} // namespace

TEST_CASE ("Tide output stays finite for random settings and full-scale noise", "[pedals][tide]")
{
    test::checkFiniteWithRandomSettings<Tide> (
        [] (Random& r)
        { return Tide::Settings{r.unipolar(), r.unipolar(), r.unipolar(), r.unipolar()}; }, run);
}

TEST_CASE ("Tide passes silence as silence", "[pedals][tide]")
{
    CHECK (test::silenceOutputPeak<Tide> (Tide::Settings{}, run) == 0.0f);
    CHECK (test::silenceOutputPeak<Tide> (Tide::Settings{1.0f, 1.0f, 1.0f, 1.0f}, run) == 0.0f);
}

TEST_CASE ("Tide static curve has a 3:1 slope above the knee", "[pedals][tide]")
{
    const Tide::Settings s{0.5f, 0.5f, 0.5f, 1.0f};
    // comp 0.5 puts the knee (6 dB wide) around a -32 dBFS sine peak.
    const double below = outputDb (s, -40.0) - outputDb (s, -50.0);
    const double above = outputDb (s, -4.0) - outputDb (s, -24.0);
    CHECK (below / 10.0 == Approx (1.0).margin (0.03));
    CHECK (above / 20.0 == Approx (1.0 / 3.0).margin (0.03));
    // More comp, more reduction at the same input.
    float lightGr = 0.0f, heavyGr = 0.0f;
    outputDb ({0.2f, 0.5f, 0.5f, 1.0f}, -12.0, &lightGr);
    outputDb ({0.9f, 0.5f, 0.5f, 1.0f}, -12.0, &heavyGr);
    CHECK (heavyGr < lightGr - 10.0f);
}

TEST_CASE ("Tide gain-reduction telemetry matches the applied reduction", "[pedals][tide]")
{
    const Tide::Settings s{0.5f, 0.5f, 0.5f, 1.0f};
    float quiet = 0.0f, loud = 0.0f;
    const double gainQuiet = outputDb (s, -60.0, &quiet) + 60.0;
    const double gainLoud = outputDb (s, -10.0, &loud) + 10.0;
    CHECK (quiet <= 0.0f);
    CHECK (quiet > -0.1f);
    CHECK (loud < -10.0f);
    CHECK (gainLoud - gainQuiet == Approx (loud).margin (0.5));
}

TEST_CASE ("Tide blend 0 is a dry passthrough", "[pedals][tide]")
{
    Tide t;
    t.prepare ({kFs, 512, 2});
    t.setSettings ({1.0f, 0.0f, 1.0f, 0.0f});
    test::Buffer b (2, 24000);
    test::fillNoise (b[0], 0.8f, 1);
    test::fillNoise (b[1], 0.8f, 2);
    const auto dry = b.data;
    test::processInBlocks (b, 100, [&] (AudioView v) { t.process (v); });
    for (int c = 0; c < 2; ++c)
        for (size_t i = 0; i < dry[0].size(); ++i)
            REQUIRE (b[c][i] == dry[static_cast<size_t> (c)][i]);
}

TEST_CASE ("Tide tone at noon is flat", "[pedals][tide]")
{
    const auto gainAt = [] (double hz, float tone, float blend)
    {
        Tide t;
        t.prepare ({kFs, 512, 1});
        t.setSettings ({0.5f, tone, 0.5f, blend});
        return test::measureGainDb (
            [&] (AudioView v) { test::processChunked (v, [&] (AudioView c) { t.process (c); }); },
            hz, kFs, 32768, 0.003f);
    };
    const double ref = gainAt (1000.0, 0.5f, 0.7f);
    for (double hz : {50.0, 100.0, 200.0, 500.0, 2000.0, 5000.0, 10000.0, 15000.0})
        CHECK (gainAt (hz, 0.5f, 0.7f) == Approx (ref).margin (0.2));

    // The extremes tilt about +-6 dB around 1 kHz.
    CHECK (gainAt (15000.0, 1.0f, 1.0f) - gainAt (1000.0, 1.0f, 1.0f) > 4.5);
    CHECK (gainAt (50.0, 1.0f, 1.0f) - gainAt (1000.0, 1.0f, 1.0f) < -4.5);
    CHECK (gainAt (15000.0, 0.0f, 1.0f) - gainAt (50.0, 0.0f, 1.0f) < -10.0);
}

TEST_CASE ("Tide attacks within a few milliseconds", "[pedals][tide]")
{
    Tide t;
    t.prepare ({kFs, 16, 1});
    t.setSettings ({0.6f, 0.5f, 0.5f, 1.0f});
    test::Buffer b (1, static_cast<int> (0.3 * kFs));
    test::fillSine (b[0], 1000.0, kFs, 0.5f);
    std::vector<float> reduction;
    test::processInBlocks (b, 16,
                           [&] (AudioView v)
                           {
                               t.process (v);
                               reduction.push_back (t.gainReductionDb());
                           });
    const float settled = reduction.back();
    REQUIRE (settled < -10.0f);
    const auto reached = std::find_if (reduction.begin(), reduction.end(),
                                       [&] (float r) { return r < 0.63f * settled; });
    const double attackMs =
        16.0 * static_cast<double> (reached - reduction.begin() + 1) / kFs * 1000.0;
    CHECK (attackMs > 1.0);
    CHECK (attackMs < 8.0);
}

TEST_CASE ("Tide release is program dependent", "[pedals][tide]")
{
    // A short stab recovers fast; the same level held for two seconds recovers slowly.
    const int afterStab = recoveryBlocks (0.03);
    const int afterHeld = recoveryBlocks (2.0);
    CHECK (afterStab * 64 < static_cast<int> (0.25 * kFs));
    CHECK (afterHeld > 3 * afterStab);
}

TEST_CASE ("Tide automation is click-free", "[pedals][tide]")
{
    const auto check = [] (auto settingsAt)
    {
        const auto r = test::measureSweep<Tide> (settingsAt, run);
        CHECK (r.finite);
        CHECK (r.sweep <= 1.5f * r.reference);
    };
    check ([] (float t) { return Tide::Settings{t, 0.5f, 0.5f, 0.7f}; });
    check ([] (float t) { return Tide::Settings{0.5f, t, 0.5f, 0.7f}; });
    check ([] (float t) { return Tide::Settings{0.5f, 0.5f, t, 0.7f}; });
    check ([] (float t) { return Tide::Settings{0.5f, 0.3f, 0.5f, t}; });
    check ([] (float t) { return Tide::Settings{t, 1.0f - t, t, 1.0f - t}; });
}
