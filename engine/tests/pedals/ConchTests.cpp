// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ModsTestUtils.h"
#include "sedef/dsp/Conch.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace sedef;
using Catch::Approx;
using dsp::Conch;

namespace
{
void run (Conch& c, AudioView v)
{
    c.process (v);
}

Conch::Settings manual (float position, float q = 5.0f)
{
    return { Conch::Manual, position, q, 0.5f };
}

std::vector<double> responseAt (float position, float q, double fs, const std::vector<double>& f)
{
    return test::responseDb (test::impulseResponse<Conch> (manual (position, q), fs, 1 << 15), f,
                             fs);
}
} // namespace

TEST_CASE ("Conch output stays finite for random settings and full-scale noise", "[pedals][conch]")
{
    test::checkFiniteWithRandomSettings<Conch> (
        [] (Random& r) {
            return Conch::Settings { static_cast<int> (r.nextU32() % 2u), r.unipolar(),
                                     2.0f + 8.0f * r.unipolar(), r.unipolar() };
        },
        run);
}

TEST_CASE ("Conch passes silence as silence", "[pedals][conch]")
{
    for (int mode : { 0, 1 })
        CHECK (test::silenceOutputPeak<Conch> (Conch::Settings { mode, 0.3f, 10.0f, 1.0f }, run) ==
               0.0f);
}

TEST_CASE ("Conch centre frequency and Q follow the rocker", "[pedals][conch]")
{
    // Heel 350 Hz to toe 2.2 kHz, exponential travel; Q 1.5x the setting at the heel, 1/1.5x at
    // the toe. Measured from the impulse response at every supported rate.
    const auto f = test::logFrequencies (150.0, 6000.0, 192);
    for (double fs : test::kAllSampleRates)
        for (float q : { 4.0f, 8.0f })
            for (float p : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f })
            {
                INFO ("fs " << fs << " Q setting " << q << " position " << p);
                const auto r = responseAt (p, q, fs, f);
                const auto peak = test::findPeak (r, f);
                CHECK (peak.hz == Approx (Conch::positionToHz (p)).epsilon (0.01));
                CHECK (test::peakQ (r, f) == Approx (Conch::qAt (q, p)).epsilon (0.06));
            }
    CHECK (Conch::positionToHz (0.0f) == Approx (350.0f));
    CHECK (Conch::positionToHz (0.5f) == Approx (877.5f).epsilon (0.001));
    CHECK (Conch::positionToHz (1.0f) == Approx (2200.0f));
    CHECK (Conch::qAt (5.0f, 0.0f) == Approx (7.5f));
    CHECK (Conch::qAt (5.0f, 1.0f) == Approx (3.333f).epsilon (0.001));
}

TEST_CASE ("Conch keeps the inductor wah's fixed low skirt", "[pedals][conch]")
{
    // Below the resonance every position shares one +6 dB/oct asymptote: the bass doesn't swell
    // or vanish as the pedal moves, only the peak does.
    const double fs = 48000.0;
    const std::vector<double> f { 40.0, 80.0 };
    const auto heel = responseAt (0.0f, 5.0f, fs, f);
    const auto toe = responseAt (1.0f, 5.0f, fs, f);
    CHECK (heel[0] == Approx (toe[0]).margin (0.6));
    CHECK (heel[1] - heel[0] == Approx (6.0).margin (0.5));
    CHECK (toe[1] - toe[0] == Approx (6.0).margin (0.2));

    // Peak gain at named positions (Q setting 5): +9 dB at the heel to +18 dB at the toe.
    const auto band = test::logFrequencies (150.0, 6000.0, 96);
    CHECK (test::findPeak (responseAt (0.0f, 5.0f, fs, band), band).db ==
           Approx (9.3).margin (0.3));
    CHECK (test::findPeak (responseAt (0.5f, 5.0f, fs, band), band).db ==
           Approx (13.8).margin (0.3));
    CHECK (test::findPeak (responseAt (1.0f, 5.0f, fs, band), band).db ==
           Approx (18.3).margin (0.3));
}

TEST_CASE ("Conch keeps the guitar near unity at mid travel", "[pedals][conch]")
{
    CHECK (test::guitarGainDb<Conch> (manual (0.5f)) == Approx (-0.8).margin (1.0));
    for (float p : { 0.0f, 0.25f, 0.75f, 1.0f })
    {
        const double g = test::guitarGainDb<Conch> (manual (p));
        CHECK (g < 0.0);
        CHECK (g > -9.0);
    }
}

TEST_CASE ("Conch rocker sweeps are free of zipper noise", "[pedals][conch]")
{
    // Host automation arrives in 64-sample steps; the rocker glides. The click indicator stays
    // within the static settings' own, and nothing appears above 6 kHz.
    for (double fs : { 44100.0, 96000.0 })
    {
        const auto r =
            test::measureSweep<Conch> ([] (float t) { return manual (t); }, run, fs, 220.0);
        CHECK (r.finite);
        CHECK (r.sweep <= 1.5f * r.reference);
    }

    // A fast heel-to-toe-and-back rock: 4 sweeps a second. The output's content above 6 kHz,
    // where a 220 Hz tone has none, stays 80 dB down.
    const double fs = 48000.0;
    Conch c;
    c.prepare ({ fs, 64, 1 });
    c.setSettings (manual (0.0f));
    test::Buffer b (1, static_cast<int> (2.0 * fs));
    test::fillSine (b[0], 220.0, fs, 0.25f);
    test::processInBlocks (
        b, 64, [&] (AudioView v) { c.process (v); },
        [&] (int start) {
            const double t = static_cast<double> (start) / fs;
            c.setSettings (manual (static_cast<float> (0.5 - 0.5 * std::cos (kTwoPiD * 2.0 * t))));
        });
    const auto start = static_cast<size_t> (0.25 * fs);
    CHECK (test::settledRmsAbove (b[0], 6000.0, fs, start) < 1.0e-4f * test::rms (b[0], start));
}

TEST_CASE ("Conch Auto rocks the pedal from the Position with the picking envelope",
           "[pedals][conch]")
{
    const double fs = 48000.0;
    // A steady level A holds the envelope at A: the rocker sits at p + (1 - p) * travel(A).
    for (float sensitivity : { 0.2f, 0.5f, 0.9f })
        for (float level : { 0.05f, 0.2f, 0.5f })
            for (float position : { 0.0f, 0.4f })
            {
                Conch c;
                c.prepare ({ fs, 512, 1 });
                c.setSettings ({ Conch::Auto, position, 5.0f, sensitivity });
                test::Buffer b (1, static_cast<int> (fs));
                std::fill (b[0].begin(), b[0].end(), level);
                test::processInBlocks (b, 512, [&] (AudioView v) { c.process (v); });
                const float expected =
                    position + (1.0f - position) * Conch::autoTravel (level, sensitivity);
                INFO ("sens " << sensitivity << " level " << level << " position " << position);
                CHECK (c.currentPosition() == Approx (expected).margin (1e-3));
                CHECK (c.centreHz() == Approx (Conch::positionToHz (expected)).epsilon (0.003));
            }

    // Harder picking opens it further; silence lets it fall back to the Position.
    CHECK (Conch::autoTravel (0.3f, 0.5f) > Conch::autoTravel (0.1f, 0.5f));
    CHECK (Conch::autoTravel (0.1f, 0.9f) > Conch::autoTravel (0.1f, 0.3f));
    Conch c;
    c.prepare ({ fs, 512, 1 });
    c.setSettings ({ Conch::Auto, 0.2f, 5.0f, 0.7f });
    test::Buffer b (1, static_cast<int> (1.5 * fs));
    test::fillSine (b[0], 196.0, fs, 0.4f);
    std::fill (b[0].begin() + static_cast<long> (0.5 * fs), b[0].end(), 0.0f);
    float opened = 0.0f;
    test::processInBlocks (b, 64, [&] (AudioView v) {
        c.process (v);
        opened = std::max (opened, c.currentPosition());
    });
    CHECK (opened > 0.8f);
    CHECK (c.currentPosition() == Approx (0.2f).margin (0.01));
}

TEST_CASE ("Conch Manual ignores the envelope", "[pedals][conch]")
{
    const double fs = 48000.0;
    Conch c;
    c.prepare ({ fs, 512, 1 });
    c.setSettings ({ Conch::Manual, 0.3f, 5.0f, 1.0f });
    test::Buffer b (1, static_cast<int> (0.5 * fs));
    test::fillSine (b[0], 196.0, fs, 0.8f);
    test::processInBlocks (b, 512, [&] (AudioView v) { c.process (v); });
    CHECK (c.currentPosition() == Approx (0.3f));
}

TEST_CASE ("Conch automation and mode switches are click-free", "[pedals][conch]")
{
    const auto check = [] (auto settingsAt) {
        const auto r = test::measureSweep<Conch> (settingsAt, run, 48000.0, 220.0);
        CHECK (r.finite);
        CHECK (r.sweep <= 1.5f * r.reference);
    };
    check ([] (float t) { return manual (0.4f, 2.0f + 8.0f * t); });
    check ([] (float t) { return Conch::Settings { Conch::Auto, 0.3f, 5.0f, t }; });
    check ([] (float t) {
        return Conch::Settings { static_cast<int> (t * 3.99f) % 2, 0.3f, 5.0f, 0.6f };
    });
}

TEST_CASE ("Conch is linear and block-size independent", "[pedals][conch]")
{
    const double fs = 48000.0;
    // No harmonics (and so no aliasing) from a hot note anywhere on the rocker.
    for (float p : { 0.0f, 0.5f, 1.0f })
        for (double hz : { 220.0, 1318.5 })
        {
            const int frames = 24000;
            test::Buffer b (1, frames);
            test::fillSine (b[0], hz, fs, 0.9f);
            Conch c;
            c.prepare ({ fs, 512, 1 });
            c.setSettings (manual (p));
            test::processInBlocks (b, 512, [&] (AudioView v) { c.process (v); });
            const float* tail = b[0].data() + frames / 2;
            CHECK (test::thd (tail, frames / 2, hz, fs) < 1.0e-4);
            CHECK (test::worstInharmonicDb (tail, frames / 2, hz, fs) < -75.0);
        }

    for (int mode : { 0, 1 })
        CHECK (test::blockSizeSpread<Conch> ({ mode, 0.3f, 6.0f, 0.7f }, fs) < 1.0e-6f);
}
