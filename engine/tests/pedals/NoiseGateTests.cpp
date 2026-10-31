// SPDX-License-Identifier: AGPL-3.0-or-later
#include "PedalTestUtils.h"
#include "sedef/dsp/NoiseGate.h"

#include <catch2/catch_test_macros.hpp>

using namespace sedef;
using dsp::NoiseGate;

namespace
{
constexpr double kFs = 48000.0;

int ms (double milliseconds)
{
    return static_cast<int> (milliseconds * 0.001 * kFs);
}

/// io is all ones, so the output is the gate's gain curve. Returns the gain and records isOpen()
/// after every block.
struct GainProbe
{
    std::vector<float> gain;
    std::vector<bool> open;
};

GainProbe probe (NoiseGate& gate, const std::vector<float>& sidechain, int block = 16)
{
    test::Buffer io (1, static_cast<int> (sidechain.size()));
    std::fill (io[0].begin(), io[0].end(), 1.0f);
    GainProbe p;
    int offset = 0;
    test::processInBlocks (
        io, block,
        [&] (AudioView v)
        {
            gate.process (v, sidechain.data() + offset);
            p.open.push_back (gate.isOpen());
        },
        [&] (int start) { offset = start; });
    p.gain = io[0];
    return p;
}

int transitions (const std::vector<bool>& v)
{
    int count = 0;
    for (size_t i = 1; i < v.size(); ++i)
        count += v[i] != v[i - 1] ? 1 : 0;
    return count;
}
} // namespace

TEST_CASE ("Gate output stays finite for random settings and full-scale noise", "[pedals][gate]")
{
    test::Buffer key (1, static_cast<int> (0.25 * 96000.0));
    test::fillNoise (key[0], 1.0f, 99);
    int offset = 0;
    test::checkFiniteWithRandomSettings<NoiseGate> (
        [] (Random& r) { return NoiseGate::Settings{-96.0f + 96.0f * r.unipolar()}; },
        [&] (NoiseGate& g, AudioView v)
        {
            // Key from the noise buffer; the offset only needs to stay in range.
            g.process (v, key[0].data() + (offset % 1000));
            offset += v.numFrames();
        });
}

TEST_CASE ("Gate passes silence as silence", "[pedals][gate]")
{
    for (float threshold : {-96.0f, -72.0f, -20.0f})
        CHECK (test::silenceOutputPeak<NoiseGate> (NoiseGate::Settings{threshold},
                                                   [] (NoiseGate& g, AudioView v)
                                                   { g.process (v, v.channel (0)); }) == 0.0f);
}

TEST_CASE ("Gate opens within about a millisecond of a burst", "[pedals][gate]")
{
    NoiseGate gate;
    gate.prepare ({kFs, 512, 1});
    gate.setSettings ({-50.0f});

    const int onset = ms (200.0);
    std::vector<float> key (static_cast<size_t> (ms (400.0)), 0.0f);
    for (size_t i = static_cast<size_t> (onset); i < key.size(); ++i)
        key[i] = 0.1f * static_cast<float> (
                            std::sin (kTwoPiD * 1000.0 *
                                      static_cast<double> (i - static_cast<size_t> (onset)) / kFs));

    const auto p = probe (gate, key);
    const auto at = [&] (int i)
    {
        return p.gain[static_cast<size_t> (i)];
    };
    CHECK (at (onset - 1) < 2.0e-4f); // closed: -80 dB floor
    CHECK (at (onset + ms (1.0)) > 0.8f);
    CHECK (at (onset + ms (3.0)) > 0.99f);
    CHECK (gate.isOpen());
    CHECK (gate.gainReductionDb() > -0.1f);
}

TEST_CASE ("Gate closes after hold and release", "[pedals][gate]")
{
    NoiseGate gate;
    gate.prepare ({kFs, 512, 1});
    gate.setSettings ({-50.0f});

    const int stop = ms (200.0);
    std::vector<float> key (static_cast<size_t> (ms (800.0)), 0.0f);
    test::fillSine (key, 220.0, kFs, 0.1f);
    std::fill (key.begin() + stop, key.end(), 0.0f);

    const auto p = probe (gate, key);
    const auto at = [&] (int i)
    {
        return p.gain[static_cast<size_t> (i)];
    };

    // Still fully open through the hold time...
    CHECK (at (stop + ms (35.0)) > 0.999f);
    // ...closes some 40-80 ms after the signal stops...
    const auto closedBlock = std::find (p.open.begin() + stop / 16, p.open.end(), false);
    REQUIRE (closedBlock != p.open.end());
    const double closeMs =
        16.0 * static_cast<double> (closedBlock - p.open.begin()) / kFs * 1000.0 - 200.0;
    CHECK (closeMs > 40.0);
    CHECK (closeMs < 80.0);
    // ...and fades out over the release (-40 dB within ~150 ms of closing).
    CHECK (at (stop + ms (90.0)) > 0.05f); // a fade, not a cut
    CHECK (at (stop + ms (250.0)) < 0.01f);
    CHECK (at (stop + ms (500.0)) < 2.0e-4f);
    CHECK_FALSE (gate.isOpen());
    CHECK (gate.gainReductionDb() < -75.0f);
}

TEST_CASE ("Gate hysteresis prevents chatter near the threshold", "[pedals][gate]")
{
    NoiseGate gate;
    gate.prepare ({kFs, 512, 1});
    const float threshold = -40.0f;
    gate.setSettings ({threshold});

    // A tone whose level wobbles +-1.5 dB around the threshold, three times a second.
    std::vector<float> key (static_cast<size_t> (3.0 * kFs));
    for (size_t i = 0; i < key.size(); ++i)
    {
        const double t = static_cast<double> (i) / kFs;
        const double levelDb = threshold + 1.5 * std::sin (kTwoPiD * 3.0 * t);
        key[i] =
            static_cast<float> (std::pow (10.0, levelDb / 20.0) * std::sin (kTwoPiD * 200.0 * t));
    }

    const auto p = probe (gate, key);
    CHECK (transitions (p.open) <= 1);
    // Once open, the gain does not move.
    const auto firstOpen = std::find (p.open.begin(), p.open.end(), true);
    REQUIRE (firstOpen != p.open.end());
    const size_t openedAt =
        static_cast<size_t> (firstOpen - p.open.begin()) * 16 + static_cast<size_t> (ms (5.0));
    CHECK (*std::min_element (p.gain.begin() + static_cast<std::ptrdiff_t> (openedAt),
                              p.gain.end()) > 0.999f);
}

TEST_CASE ("Gate is driven by the sidechain, not by its input", "[pedals][gate]")
{
    NoiseGate gate;
    gate.prepare ({kFs, 512, 1});
    gate.setSettings ({-60.0f});

    // Loud input, silent sidechain: gated to the floor.
    test::Buffer io (1, ms (300.0));
    test::fillNoise (io[0], 1.0f, 3);
    const std::vector<float> silent (io[0].size(), 0.0f);
    gate.process (io.view(), silent.data());
    CHECK (test::rms (io[0], static_cast<size_t> (ms (50.0))) < 1.0e-4f);

    // Quiet input, loud sidechain: passed untouched.
    gate.reset();
    test::Buffer quiet (1, ms (300.0));
    test::fillSine (quiet[0], 440.0, kFs, 1.0e-4f);
    const auto original = quiet[0];
    std::vector<float> key (quiet[0].size());
    test::fillSine (key, 110.0, kFs, 0.3f);
    gate.process (quiet.view(), key.data());
    float maxError = 0.0f;
    for (size_t i = static_cast<size_t> (ms (10.0)); i < original.size(); ++i)
        maxError = std::max (maxError, std::abs (quiet[0][i] - original[i]));
    CHECK (maxError < 1.0e-9f);
    CHECK (gate.isOpen());
}

TEST_CASE ("Gate at -96 dB is off", "[pedals][gate]")
{
    NoiseGate gate;
    gate.prepare ({kFs, 512, 2});
    gate.setSettings ({-96.0f});
    test::Buffer io (2, ms (200.0));
    test::fillNoise (io[0], 0.5f, 5);
    test::fillNoise (io[1], 0.5f, 6);
    const auto left = io[0];
    const std::vector<float> silent (left.size(), 0.0f);
    gate.process (io.view(), silent.data());
    float maxError = 0.0f;
    for (size_t i = static_cast<size_t> (ms (10.0)); i < left.size(); ++i)
        maxError = std::max (maxError, std::abs (io[0][i] - left[i]));
    CHECK (maxError == 0.0f);
    CHECK (gate.isOpen());
    CHECK (gate.gainReductionDb() == 0.0f);
}

TEST_CASE ("Gate threshold automation never snaps the gain", "[pedals][gate]")
{
    // Sweep the threshold through the level of a steady tone in both directions. The gain must
    // fade, never jump: the per-sample change is bounded by the tone's own slope plus the attack.
    NoiseGate gate;
    gate.prepare ({kFs, 64, 1});
    const float amp = 0.25f;
    test::Buffer io (1, static_cast<int> (3.0 * kFs));
    test::fillSine (io[0], 110.0, kFs, amp);
    const auto key = io[0];
    int offset = 0;
    test::processInBlocks (
        io, 64, [&] (AudioView v) { gate.process (v, key.data() + offset); },
        [&] (int start)
        {
            offset = start;
            const double t = static_cast<double> (start) / io.frames();
            const double tri = 1.0 - std::abs (2.0 * t - 1.0); // 0 -> 1 -> 0
            gate.setSettings ({static_cast<float> (-96.0 + 90.0 * tri)});
        });
    REQUIRE (test::allFinite (io[0]));
    CHECK (test::maxFirstDifference (io[0]) < test::maxFirstDifference (key) + 0.1f * amp);
}
