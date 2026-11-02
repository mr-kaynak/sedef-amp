// SPDX-License-Identifier: AGPL-3.0-or-later
// Gain-staging targets (docs/dsp/amps.md, "Gain targets"): how much gain each amp has, how it is
// spread over the knob, and that the extra gain stays tight, fizz-free and alias-free.
#include "AmpTestUtils.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

using namespace sedef;
using dsp::AmpSection;

namespace
{
constexpr double kFs = 48000.0;
constexpr double kNoteHz = 196.0; // G3

AmpSection::Settings at (int model, float drive)
{
    AmpSection::Settings s;
    s.model = model;
    s.lagoon.volume = drive;
    s.reef.gain = drive;
    s.abyss.gain = drive;
    return s;
}

std::vector<float> run (const AmpSection::Settings& s, std::vector<float> v)
{
    AmpSection amp;
    test::prepareAmp (amp, s, kFs);
    test::runAmp (amp, v);
    return v;
}

double noteSustain (const AmpSection::Settings& s, double diDbfs)
{
    return test::sustainSeconds (run (s, test::guitarNote (kFs, 8.0, kNoteHz, diDbfs)), kFs);
}

double strumLevel (const AmpSection::Settings& s)
{
    const auto v = run (s, test::guitarStrum (kFs, 6.0));
    return test::dbfs (test::rms (v, static_cast<size_t> (0.5 * kFs)));
}
} // namespace

TEST_CASE ("Full gain reaches high-gain saturation on Abyss and hot crunch on Reef", "[amp]")
{
    // Input level (dBFS RMS, 220 Hz sine) where THD reaches 10 %, at every oversampling factor.
    const double fs = GENERATE (44100.0, 48000.0, 96000.0, 192000.0);
    const double abyss = test::thdCrossing (at (AmpSection::Abyss, 1.0f), fs);
    const double reef = test::thdCrossing (at (AmpSection::Reef, 1.0f), fs);
    CAPTURE (fs, abyss, reef);
    CHECK (abyss <= -64.0); // v0.2.0: -44
    CHECK (reef <= -43.0);  // v0.2.0: -34
    CHECK (reef >= -51.0);  // still a crunch amp, not a second lead channel
    CHECK (abyss <= reef - 15.0);
}

TEST_CASE ("Gain knobs are useful over their whole travel", "[amp]")
{
    // Reef and Abyss: every quarter turn moves the distortion threshold by a similar step, so no
    // part of the travel is dead.
    for (int model : { AmpSection::Reef, AmpSection::Abyss })
    {
        double previous = 0.0;
        for (int q = 0; q <= 4; ++q)
        {
            const double t = test::thdCrossing (at (model, 0.25f * static_cast<float> (q)), kFs);
            CAPTURE (model, q, t, previous);
            REQUIRE (!std::isnan (t));
            if (q > 0)
            {
                CHECK (previous - t >= 8.0);
                CHECK (previous - t <= 16.0);
            }
            previous = t;
        }
    }
    // Lagoon stays clean up to noon at every DI level up to -12 dBFS and breaks up near the top.
    const double lagoonNoon = test::thdCrossing (at (AmpSection::Lagoon, 0.5f), kFs);
    const double lagoonTop = test::thdCrossing (at (AmpSection::Lagoon, 1.0f), kFs);
    CAPTURE (lagoonNoon, lagoonTop);
    CHECK ((std::isnan (lagoonNoon) || lagoonNoon > -12.0));
    CHECK (lagoonTop < -15.0);
}

TEST_CASE ("Single notes sustain at full gain, even from a quiet DI", "[amp]")
{
    // A G3 picked as hard as one string of a strum at the given DI level; the dry note stays
    // within 6 dB of its peak for 0.68 s.
    const double dry = test::sustainSeconds (test::guitarNote (kFs, 8.0, kNoteHz, -24.0), kFs);
    const auto abyss = at (AmpSection::Abyss, 1.0f);
    const auto reef = at (AmpSection::Reef, 1.0f);
    const double abyss30 = noteSustain (abyss, -30.0), abyss24 = noteSustain (abyss, -24.0);
    const double reef30 = noteSustain (reef, -30.0), reef24 = noteSustain (reef, -24.0);
    CAPTURE (dry, abyss30, abyss24, reef30, reef24);
    CHECK (dry < 0.8);
    CHECK (abyss30 >= 4.2); // v0.2.0: 1.6
    CHECK (abyss24 >= 5.0); // v0.2.0: 2.3
    CHECK (reef30 >= 1.6);  // v0.2.0: 0.9
    CHECK (reef24 >= 2.2);  // v0.2.0: 1.3
}

TEST_CASE ("Full-gain Abyss output is dense at every DI level", "[amp]")
{
    // Crest factor of the strum: 14 dB dry; a saturated preamp flattens it to about 6-7 dB
    // whatever the interface level.
    const auto strum = test::guitarStrum (kFs, 6.0);
    for (double di : { -30.0, -24.0, -18.0, -12.0 })
    {
        auto in = strum;
        for (auto& x : in)
            x *= static_cast<float> (std::pow (10.0, (di + 18.0) / 20.0));
        const auto v = run (at (AmpSection::Abyss, 1.0f), in);
        const std::vector<float> body (v.begin() + static_cast<long> (0.5 * kFs), v.end());
        const double crest = test::dbfs (test::peak (body)) - test::dbfs (test::rms (body));
        CAPTURE (di, crest);
        CHECK (crest < 7.5);
    }
}

TEST_CASE ("Turning gain up never jumps the level", "[amp]")
{
    const int model = GENERATE (AmpSection::Reef, AmpSection::Abyss);
    std::vector<double> level;
    for (int g = 0; g <= 10; ++g)
        level.push_back (strumLevel (at (model, 0.1f * static_cast<float> (g))));
    for (size_t i = 1; i < level.size(); ++i)
    {
        CAPTURE (model, i, level[i - 1], level[i]);
        CHECK (std::abs (level[i] - level[i - 1]) < 4.0);
    }
    // In the upper half, where the preamp is saturated, the level stays put.
    const auto [lo, hi] = std::minmax_element (level.begin() + 5, level.end());
    CAPTURE (model, *lo, *hi);
    CHECK (*hi - *lo < 1.5);
}

TEST_CASE ("Full gain stays tight and does not fizz", "[amp]")
{
    // Palm-muted chugs: the octave bands at 63 and 125 Hz (re. the total) stay where the coupling
    // high-passes put them in v0.2.0 (-13.5 / -4.5 dB for Abyss), well below a clean amp's.
    const auto chugs = test::palmChugs (kFs, 4.0);
    const auto abyss = test::octaveBands (run (at (AmpSection::Abyss, 1.0f), chugs), kFs);
    const auto lagoon = test::octaveBands (run (at (AmpSection::Lagoon, 0.5f), chugs), kFs);
    CAPTURE (abyss[0], abyss[1], lagoon[0], lagoon[1]);
    CHECK (abyss[0] < -12.5);
    CHECK (abyss[1] < -3.5);
    CHECK (abyss[0] < lagoon[0] - 2.0);

    // Strum through a rough cab: the 8 kHz octave stays at or below v0.2.0's full-gain Abyss
    // (-29.4 dB re. the total) plus 1 dB, for both drive amps.
    for (int model : { AmpSection::Reef, AmpSection::Abyss })
    {
        auto v = run (at (model, 1.0f), test::guitarStrum (kFs, 6.0));
        test::roughCab (v, kFs);
        const auto bands = test::octaveBands (v, kFs);
        CAPTURE (model, bands[7], bands[8]);
        CHECK (bands[7] < -28.4);
        CHECK (bands[8] < -48.0);
    }
}

TEST_CASE ("Aliasing stays low at full gain", "[amp]")
{
    // Images of the harmonics of a high sine (-15 dBFS RMS) at full drive (test::worstAliasDb).
    // v0.2.0 worst: Lagoon -61.6 / -105.8 dB (full / default master), Reef -62.3 / -93.7, Abyss
    // -55.7 / -56.6.
    const int model = GENERATE (0, 1, 2);
    const float master = GENERATE (0.5f, 1.0f);
    double worst = -200.0;
    for (double f0 : { 1234.5, 2717.0, 4321.0 })
    {
        auto s = at (model, 1.0f);
        s.lagoon.master = s.reef.master = s.abyss.master = master;
        std::vector<float> v (static_cast<size_t> (2.0 * kFs));
        test::fillSine (v, f0, kFs, 0.25f);
        worst = std::max (worst, test::worstAliasDb (run (s, v), f0, kFs));
    }
    CAPTURE (model, master, worst);
    CHECK (worst < (master > 0.75f ? -58.0 : -62.0));
}
