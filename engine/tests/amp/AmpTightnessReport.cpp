// SPDX-License-Identifier: AGPL-3.0-or-later
// Hidden report for the high-gain voicing: how tight, articulate and dynamic Abyss (and, for
// comparison, Reef) is per gain setting. docs/dsp/amps.md, "Tight, not fuzzy", quotes it.
// Run with: sedef_engine_tests "[.tight]"
#include "AmpTestUtils.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include <functional>
#include <string>

using namespace sedef;
using dsp::AmpSection;

namespace
{
constexpr double kFs = 48000.0;
constexpr double kE2 = 82.41, kB2 = 123.47, kE3 = 164.81, kGs3 = 207.65, kB3 = 246.94;
constexpr double kG3 = 196.0;

struct Setting
{
    std::string label;
    AmpSection::Settings s;
    const char* name() const { return label.c_str(); }
};

AmpSection::Settings abyss (float gain)
{
    AmpSection::Settings s;
    s.model = AmpSection::Abyss;
    s.abyss.gain = gain;
    return s;
}

AmpSection::Settings reef (float gain)
{
    AmpSection::Settings s;
    s.model = AmpSection::Reef;
    s.reef.gain = gain;
    return s;
}

std::vector<float> run (const AmpSection::Settings& s, std::vector<float> v)
{
    AmpSection amp;
    test::prepareAmp (amp, s, kFs);
    test::runAmp (amp, v);
    return v;
}

std::function<void (std::vector<float>&)> processOf (const AmpSection::Settings& s)
{
    return [s] (std::vector<float>& v) {
        AmpSection amp;
        test::prepareAmp (amp, s, kFs);
        test::runAmp (amp, v);
    };
}

std::vector<float> scaled (std::vector<float> v, double db)
{
    const auto g = static_cast<float> (std::pow (10.0, db / 20.0));
    for (auto& x : v)
        x *= g;
    return v;
}

/// Odd minus even harmonic power (dB) of a 220 Hz sine at `db`, and the output's crest factor.
std::pair<double, double> oddEvenAndCrest (const AmpSection::Settings& s, double db)
{
    auto v = run (s, test::sineAt (220.0, kFs, 1.0, db));
    const int n = 32768;
    const float* d = v.data() + v.size() - n;
    double odd = 0.0, even = 0.0;
    for (int k = 2; k <= 15; ++k)
    {
        const double h = test::toneMagnitude (d, n, 220.0 * k, kFs);
        (k % 2 == 0 ? even : odd) += h * h;
    }
    const std::vector<float> tail (v.end() - n, v.end());
    return { 10.0 * std::log10 (odd / std::max (even, 1e-30)),
             test::dbfs (test::peak (tail)) - test::dbfs (test::rms (tail)) };
}

/// Fraction (dB re. total) of the strum output's power above `hz`, raw amp output.
double powerAbove (const std::vector<float>& v, double hz)
{
    const size_t n = 131072;
    const auto p = test::powerSpectrum (v, static_cast<size_t> (0.5 * kFs), n);
    double above = 0.0, total = 0.0;
    for (size_t k = 1; k < p.size(); ++k)
    {
        total += p[k];
        if (static_cast<double> (k) * kFs / static_cast<double> (n) >= hz)
            above += p[k];
    }
    return 10.0 * std::log10 (above / total);
}

/// How long a hard palm-muted chug (DI -12) at t = 1 s masks a quiet 1.5 kHz probe (-45 dBFS RMS):
/// seconds from the pick until the probe is back within 1 dB of its level before the chug. A
/// tight amp lets go of the chug quickly; a fuzz holds it, and grid blocking would hold the probe
/// down after the chug has gone.
double recoverySeconds (const AmpSection::Settings& s)
{
    const double probeHz = 1500.0;
    auto v = test::sineAt (probeHz, kFs, 3.0, -45.0);
    const auto chug = test::palmChug (kFs, 2.0, -12.0);
    const auto at = static_cast<size_t> (0.95 * kFs); // the chug's pick lands at 1.0 s
    for (size_t i = 0; i < chug.size() && at + i < v.size(); ++i)
        v[at + i] += chug[i];
    const auto out = run (s, v);
    const auto win = static_cast<int> (0.02 * kFs);
    const auto levelAt = [&] (double t) {
        return test::dbfs (
            test::toneMagnitude (out.data() + static_cast<size_t> (t * kFs), win, probeHz, kFs));
    };
    const double before = levelAt (0.8);
    double last = 1.0;
    for (double t = 1.0; t < 2.9; t += 0.002)
        if (std::abs (levelAt (t) - before) > 1.0)
            last = t;
    return last + 0.002 - 1.0;
}

/// Pick attack: the G3 note's envelope peak (10 ms windows) minus its level 0.5 s later, in dB.
double attackDb (const std::vector<float>& v)
{
    const auto env = test::envelopeDb (v, kFs, 10.0, 1.0);
    const auto peakIt = std::max_element (env.begin(), env.end());
    const auto later = static_cast<size_t> (peakIt - env.begin()) + 500;
    return *peakIt - env[std::min (later, env.size() - 1)];
}

double peakEnvDb (const std::vector<float>& v)
{
    const auto env = test::envelopeDb (v, kFs, 10.0, 2.5);
    return *std::max_element (env.begin(), env.end());
}
} // namespace

TEST_CASE ("High-gain tightness report", "[.tight]")
{
    std::vector<Setting> settings;
    for (float g : { 0.3f, 0.5f, 0.75f, 1.0f })
    {
        char name[32];
        std::snprintf (name, sizeof name, "Abyss %.2f", static_cast<double> (g));
        settings.push_back ({ name, abyss (g) });
    }
    settings.push_back ({ "Reef 0.50", reef (0.5f) });
    settings.push_back ({ "Reef 1.00", reef (1.0f) });

    std::printf ("1. Compression points (dBFS RMS sine in): 1 / 6 / 20 dB below the small-signal "
                 "gain at 82 / 220 / 660 Hz; LF margin = 82 Hz minus 660 Hz 6 dB point\n");
    std::printf ("%-11s | %7s %7s %7s | %7s %7s %7s | %7s %7s %7s | %6s\n", "setting", "82:1",
                 "82:6", "82:20", "220:1", "220:6", "220:20", "660:1", "660:6", "660:20", "LF");
    for (const auto& st : settings)
    {
        const auto p = processOf (st.s);
        double c[3][3];
        const double hz[3] = { kE2, 220.0, 660.0 };
        for (int f = 0; f < 3; ++f)
            for (int d = 0; d < 3; ++d)
                c[f][d] = test::compressionPoint (p, hz[f], kFs,
                                                  d == 0   ? 1.0
                                                  : d == 1 ? 6.0
                                                           : 20.0);
        std::printf ("%-11s | %7.1f %7.1f %7.1f | %7.1f %7.1f %7.1f | %7.1f %7.1f %7.1f | %6.1f\n",
                     st.name(), c[0][0], c[0][1], c[0][2], c[1][0], c[1][1], c[1][2], c[2][0],
                     c[2][1], c[2][2], c[0][1] - c[2][1]);
    }

    std::printf (
        "\n2. Compression (dB below small-signal gain) of a sine at DI level, 82 / 660 Hz\n");
    std::printf ("%-11s | %13s %13s %13s\n", "setting", "-36", "-24", "-18");
    for (const auto& st : settings)
    {
        const auto p = processOf (st.s);
        std::printf ("%-11s |", st.name());
        const double small82 = test::sineGainDb (p, kE2, kFs, -100.0);
        const double small660 = test::sineGainDb (p, 660.0, kFs, -100.0);
        for (double db : { -36.0, -24.0, -18.0 })
            std::printf ("  %5.1f / %5.1f", small82 - test::sineGainDb (p, kE2, kFs, db),
                         small660 - test::sineGainDb (p, 660.0, kFs, db));
        std::printf ("\n");
    }

    std::printf (
        "\n3. Palm mutes (DI -18): seconds one chug rings above its peak -10 / -20 / -30 dB; "
        "dip between chugs at 150 / 240 bpm eighths (dB)\n");
    {
        const auto chug = test::palmChug (kFs, 2.0);
        const auto train150 = test::palmChugs (kFs, 4.0, 150.0);
        const auto train240 = test::palmChugs (kFs, 4.0, 240.0);
        std::printf ("%-11s | %5.3f %5.3f %5.3f | %5.1f %5.1f\n", "dry",
                     test::ringSeconds (chug, kFs, 10.0), test::ringSeconds (chug, kFs, 20.0),
                     test::ringSeconds (chug, kFs, 30.0), test::chugDipDb (train150, kFs, 150.0),
                     test::chugDipDb (train240, kFs, 240.0));
        for (const auto& st : settings)
        {
            const auto o = run (st.s, chug);
            std::printf ("%-11s | %5.3f %5.3f %5.3f | %5.1f %5.1f\n", st.name(),
                         test::ringSeconds (o, kFs, 10.0), test::ringSeconds (o, kFs, 20.0),
                         test::ringSeconds (o, kFs, 30.0),
                         test::chugDipDb (run (st.s, train150), kFs, 150.0),
                         test::chugDipDb (run (st.s, train240), kFs, 240.0));
        }
    }

    std::printf (
        "\n4. Intermodulation (dB, power between the notes' harmonics re. on them): E2+B2 "
        "sines at -30 / -24 each; ringing E5 (E2 B2 E3) and E triad (E3 G#3 B3) at DI -18\n");
    {
        const std::vector<double> e5 { kE2, kB2, kE3 }, triad { kE3, kGs3, kB3 };
        const auto chordE5 = test::ringingChord (kFs, 3.0, e5);
        const auto chordTriad = test::ringingChord (kFs, 3.0, triad);
        const auto start = static_cast<size_t> (0.1 * kFs);
        std::printf ("%-11s | %6.1f %6.1f | %6.1f %6.1f\n", "dry", -300.0, -300.0,
                     test::inharmonicDb (chordE5, kFs, e5, start),
                     test::inharmonicDb (chordTriad, kFs, triad, start));
        for (const auto& st : settings)
        {
            const auto p = processOf (st.s);
            std::printf ("%-11s | %6.1f %6.1f | %6.1f %6.1f\n", st.name(),
                         test::twoToneImdDb (p, kE2, kB2, kFs, -30.0),
                         test::twoToneImdDb (p, kE2, kB2, kFs, -24.0),
                         test::inharmonicDb (run (st.s, chordE5), kFs, e5, start),
                         test::inharmonicDb (run (st.s, chordTriad), kFs, triad, start));
        }
    }

    std::printf (
        "\n5. Clipping shape, 220 Hz sine at DI -36 / -24: odd minus even harmonic power "
        "(dB) and output crest factor (dB; sine 3.0, square 0.0); THD %% at DI -36 / -24\n");
    for (const auto& st : settings)
    {
        const auto a = oddEvenAndCrest (st.s, -36.0);
        const auto b = oddEvenAndCrest (st.s, -24.0);
        std::printf (
            "%-11s | %6.1f %5.2f | %6.1f %5.2f | %5.1f %5.1f\n", st.name(), a.first, a.second,
            b.first, b.second,
            100.0 * test::thd (run (st.s, test::sineAt (220.0, kFs, 1.0, -36.0)), 220.0, kFs),
            100.0 * test::thd (run (st.s, test::sineAt (220.0, kFs, 1.0, -24.0)), 220.0, kFs));
    }

    std::printf ("\n6. Fizz: strum (DI -18) power above 4 / 6 / 8 kHz re. total, raw amp output "
                 "(dB); octave bands 4k / 8k through the rough cab\n");
    for (const auto& st : settings)
    {
        auto v = run (st.s, test::guitarStrum (kFs, 6.0));
        const double a4 = powerAbove (v, 4000.0), a6 = powerAbove (v, 6000.0),
                     a8 = powerAbove (v, 8000.0);
        test::roughCab (v, kFs);
        const auto bands = test::octaveBands (v, kFs);
        std::printf ("%-11s | %6.1f %6.1f %6.1f | %6.1f %6.1f\n", st.name(), a4, a6, a8, bands[6],
                     bands[7]);
    }

    std::printf (
        "\n7. Picking dynamics, G3 note: output envelope peak (dBFS) at DI -36 / -30 / -24 "
        "/ -18 / -12, range, pick attack at DI -24 / -18 (dB), seconds a hard chug masks a quiet "
        "probe\n");
    {
        std::printf ("%-11s |", "dry");
        std::vector<double> dry;
        for (double di : { -36.0, -30.0, -24.0, -18.0, -12.0 })
            dry.push_back (peakEnvDb (test::guitarNote (kFs, 1.5, kG3, di)));
        for (double d : dry)
            std::printf (" %6.1f", d);
        std::printf (" | %5.1f | %5.1f %5.1f |\n", dry.back() - dry.front(),
                     attackDb (test::guitarNote (kFs, 1.5, kG3, -24.0)),
                     attackDb (test::guitarNote (kFs, 1.5, kG3, -18.0)));
    }
    for (const auto& st : settings)
    {
        std::vector<double> lv;
        for (double di : { -36.0, -30.0, -24.0, -18.0, -12.0 })
            lv.push_back (peakEnvDb (run (st.s, test::guitarNote (kFs, 1.5, kG3, di))));
        std::printf ("%-11s |", st.name());
        for (double d : lv)
            std::printf (" %6.1f", d);
        std::printf (" | %5.1f | %5.1f %5.1f | %5.3f\n", lv.back() - lv.front(),
                     attackDb (run (st.s, test::guitarNote (kFs, 1.5, kG3, -24.0))),
                     attackDb (run (st.s, test::guitarNote (kFs, 1.5, kG3, -18.0))),
                     recoverySeconds (st.s));
    }

    std::printf ("\n8. Abyss 0.5, other knobs: chug ring to -20 dB (s), dip at 150 bpm (dB), E2+B2 "
                 "IMD at -24 (dB), strum level (dBFS)\n");
    {
        struct K
        {
            const char* name;
            std::function<void (AmpSection::Settings&)> set;
        };
        const std::vector<K> knobs {
            { "defaults", [] (auto&) {} },
            { "master 0.1", [] (auto& s) { s.abyss.master = 0.1f; } },
            { "master 0.8", [] (auto& s) { s.abyss.master = 0.8f; } },
            { "master 1.0", [] (auto& s) { s.abyss.master = 1.0f; } },
            { "presence 0", [] (auto& s) { s.abyss.presence = 0.0f; } },
            { "presence 1", [] (auto& s) { s.abyss.presence = 1.0f; } },
            { "bass 0", [] (auto& s) { s.abyss.bass = 0.0f; } },
            { "bass 1", [] (auto& s) { s.abyss.bass = 1.0f; } },
            { "bright off", [] (auto& s) { s.abyss.bright = false; } },
            { "shift on", [] (auto& s) { s.abyss.shift = true; } },
            { "Mariana",
              [] (auto& s) {
                  s.abyss.gain = 0.65f;
                  s.abyss.shift = true;
                  s.abyss.mid = 0.4f;
                  s.abyss.bass = 0.55f;
              } },
            { "Riptide",
              [] (auto& s) {
                  s.abyss.gain = 0.55f;
                  s.abyss.mid = 0.55f;
              } },
        };
        const auto chug = test::palmChug (kFs, 2.0);
        const auto train = test::palmChugs (kFs, 4.0, 150.0);
        const auto strum = test::guitarStrum (kFs, 6.0);
        for (const auto& k : knobs)
        {
            auto s = abyss (0.5f);
            k.set (s);
            const auto st = run (s, strum);
            std::printf ("%-11s | %5.3f %5.1f %6.1f %6.1f\n", k.name,
                         test::ringSeconds (run (s, chug), kFs, 20.0),
                         test::chugDipDb (run (s, train), kFs, 150.0),
                         test::twoToneImdDb (processOf (s), kE2, kB2, kFs, -24.0),
                         test::dbfs (test::rms (st, static_cast<size_t> (0.5 * kFs))));
        }
    }

    std::printf ("\n9. Strum level (dBFS RMS) per DI level -30 / -24 / -18 / -12\n");
    for (const auto& st : settings)
    {
        std::printf ("%-11s |", st.name());
        for (double di : { -30.0, -24.0, -18.0, -12.0 })
        {
            const auto v = run (st.s, scaled (test::guitarStrum (kFs, 6.0), di + 18.0));
            std::printf (" %6.1f", test::dbfs (test::rms (v, static_cast<size_t> (0.5 * kFs))));
        }
        std::printf ("\n");
    }
}
