// SPDX-License-Identifier: AGPL-3.0-or-later
#include "PedalTestUtils.h"
#include "sedef/dsp/Ripple.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace sedef;
using Catch::Approx;
using dsp::Ripple;

namespace
{
constexpr double kFs = 48000.0;

void run (Ripple& r, AudioView v)
{
    r.process (v);
}

test::Buffer render (const Ripple::Settings& s, const test::Buffer& in)
{
    test::Buffer b (in.channels(), in.frames()); // Buffer copies would alias the source's pointers
    for (int c = 0; c < b.channels(); ++c)
        b[c] = in.data[static_cast<size_t> (c)];
    Ripple r;
    r.prepare ({ kFs, 512, b.channels() });
    r.setSettings (s);
    test::processInBlocks (b, 256, [&] (AudioView v) { r.process (v); });
    return b;
}

/// (max - min) / mean of the zero-crossing periods after the first half second.
double periodSpread (const std::vector<float>& v)
{
    const auto t = test::risingZeroCrossings (v, static_cast<size_t> (0.5 * kFs));
    std::vector<double> periods;
    for (size_t i = 1; i < t.size(); ++i)
        periods.push_back (t[i] - t[i - 1]);
    const auto [lo, hi] = std::minmax_element (periods.begin(), periods.end());
    double mean = 0.0;
    for (double p : periods)
        mean += p;
    mean /= static_cast<double> (periods.size());
    return (*hi - *lo) / mean;
}
} // namespace

TEST_CASE ("Ripple output stays finite for random settings and full-scale noise",
           "[pedals][ripple]")
{
    test::checkFiniteWithRandomSettings<Ripple> (
        [] (Random& r) {
            return Ripple::Settings { static_cast<int> (r.nextU32() % 3u),
                                      0.69f + 6.5f * r.unipolar(), r.unipolar() };
        },
        run);
}

TEST_CASE ("Ripple passes silence as silence", "[pedals][ripple]")
{
    for (int mode : { 0, 1, 2 })
        CHECK (test::silenceOutputPeak<Ripple> (Ripple::Settings { mode, 7.19f, 1.0f }, run) ==
               0.0f);
}

TEST_CASE ("Ripple tremolo period follows the speed", "[pedals][ripple]")
{
    for (float speed : { 0.69f, 3.92f, 7.19f })
    {
        test::Buffer dc (1, static_cast<int> (6.0 * kFs));
        std::fill (dc[0].begin(), dc[0].end(), 1.0f);
        const auto out = render ({ Ripple::Tremolo, speed, 1.0f }, dc)[0];

        // Bottoms of the dips (the gain curve itself, since the input is DC): the minimum of each
        // run below 0.5.
        std::vector<size_t> troughs;
        for (size_t i = 0; i < out.size(); ++i)
        {
            if (out[i] >= 0.5f)
                continue;
            size_t lowest = i;
            for (; i < out.size() && out[i] < 0.5f; ++i)
                if (out[i] < out[lowest])
                    lowest = i;
            if (i < out.size() && lowest > 0)
                troughs.push_back (lowest);
        }
        REQUIRE (troughs.size() >= 3);
        const double period = static_cast<double> (troughs.back() - troughs.front()) /
                              static_cast<double> (troughs.size() - 1);
        CHECK (kFs / period == Approx (speed).epsilon (0.01));
        CHECK (test::peak (out) == Approx (1.0f).margin (1e-4));      // peaks at unity...
        CHECK (*std::min_element (out.begin(), out.end()) < 1.0e-3f); // ...full depth is 100 % AM
    }
}

TEST_CASE ("Ripple tremolo depth 0 is transparent", "[pedals][ripple]")
{
    test::Buffer b (1, 48000);
    test::fillNoise (b[0], 0.5f, 3);
    const auto in = b[0];
    const auto out = render ({ Ripple::Tremolo, 5.0f, 0.0f }, b)[0];
    for (size_t i = 0; i < in.size(); ++i)
        REQUIRE (out[i] == in[i]);
}

TEST_CASE ("Ripple vibrato and chorus modulate pitch; tremolo does not", "[pedals][ripple]")
{
    test::Buffer sine (1, static_cast<int> (2.5 * kFs));
    test::fillSine (sine[0], 1000.0, kFs, 0.5f);
    const double tremolo = periodSpread (render ({ Ripple::Tremolo, 3.92f, 0.8f }, sine)[0]);
    const double chorus = periodSpread (render ({ Ripple::Chorus, 3.92f, 1.0f }, sine)[0]);
    const double vibrato = periodSpread (render ({ Ripple::Vibrato, 3.92f, 1.0f }, sine)[0]);
    CHECK (tremolo < 1.0e-3);
    CHECK (chorus > 5.0e-3);
    // Full-depth vibrato swings about +-45 cents (+-2.6 %), so the period spread is about 5 %.
    CHECK (vibrato > 0.04);
    CHECK (vibrato < 0.07);
}

TEST_CASE ("Ripple vibrato depth sets the pitch swing independent of speed", "[pedals][ripple]")
{
    test::Buffer sine (1, static_cast<int> (4.0 * kFs));
    test::fillSine (sine[0], 1000.0, kFs, 0.5f);
    const double slow = periodSpread (render ({ Ripple::Vibrato, 0.69f, 0.5f }, sine)[0]);
    const double fast = periodSpread (render ({ Ripple::Vibrato, 7.19f, 0.5f }, sine)[0]);
    CHECK (slow == Approx (fast).epsilon (0.15));
    CHECK (slow == Approx (2.0 * (std::exp2 (22.5 / 1200.0) - 1.0)).epsilon (0.2));
}

TEST_CASE ("Ripple chorus keeps the dry signal", "[pedals][ripple]")
{
    test::Buffer impulse (1, 4800);
    impulse[0][100] = 1.0f;
    const auto out = render ({ Ripple::Chorus, 2.0f, 1.0f }, impulse)[0];
    CHECK (out[100] == Approx (0.5f)); // dry, undelayed, half of the 50/50 mix
    const int earliestWet = 100 + static_cast<int> (0.0055 * kFs);
    float before = 0.0f, wet = 0.0f;
    for (int i = 101; i < earliestWet; ++i)
        before = std::max (before, std::abs (out[static_cast<size_t> (i)]));
    for (int i = earliestWet; i < 100 + static_cast<int> (0.013 * kFs); ++i)
        wet += std::abs (out[static_cast<size_t> (i)]);
    CHECK (before < 1.0e-6f);
    CHECK (wet > 0.3f);
}

TEST_CASE ("Ripple stereo chorus offsets the two channels", "[pedals][ripple]")
{
    test::Buffer b (2, static_cast<int> (kFs));
    test::fillSine (b[0], 440.0, kFs, 0.5f);
    b[1] = b[0];
    const auto out = render ({ Ripple::Chorus, 1.0f, 1.0f }, b);
    std::vector<float> diff (out.data[0].size());
    for (size_t i = 0; i < diff.size(); ++i)
        diff[i] = out.data[0][i] - out.data[1][i];
    CHECK (test::rms (diff, 4800) > 0.05f * test::rms (out.data[0], 4800));
}

TEST_CASE ("Ripple automation and mode switches are click-free", "[pedals][ripple]")
{
    const auto check = [] (auto settingsAt, float ratio = 1.5f) {
        const auto r = test::measureSweep<Ripple> (settingsAt, run, kFs, 220.0);
        CHECK (r.finite);
        CHECK (r.sweep <= ratio * r.reference);
    };
    for (int mode : { 0, 1, 2 })
    {
        check ([mode] (float t) { return Ripple::Settings { mode, 0.69f + 6.5f * t, 0.7f }; });
        check ([mode] (float t) { return Ripple::Settings { mode, 3.0f, t }; });
    }
    // Mode steps through tremolo -> chorus -> vibrato -> tremolo.
    check (
        [] (float t) { return Ripple::Settings { static_cast<int> (t * 3.99f) % 3, 2.0f, 1.0f }; });
    check (
        [] (float t) { return Ripple::Settings { 2 - static_cast<int> (t * 2.99f), 6.0f, 1.0f }; });
}
