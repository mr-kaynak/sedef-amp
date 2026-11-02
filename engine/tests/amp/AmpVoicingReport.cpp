// SPDX-License-Identifier: AGPL-3.0-or-later
// Hidden report used while voicing the amps: levels, THD curves and small-signal responses.
// Run with: sedef_engine_tests "[.voicing]"
#include "AmpTestUtils.h"
#include "sedef/core/Biquad.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include <string>

using namespace sedef;
using dsp::AmpSection;

namespace
{
const char* kNames[] = { "Lagoon", "Reef", "Abyss" };

AmpSection::Settings defaults (int model)
{
    AmpSection::Settings s;
    s.model = model;
    return s;
}

void setDrive (AmpSection::Settings& s, float v)
{
    s.lagoon.volume = v;
    s.reef.gain = v;
    s.abyss.gain = v;
}

double sineThd (const AmpSection::Settings& s, double hz, double dbfsRms, double fs = 48000.0)
{
    AmpSection amp;
    test::prepareAmp (amp, s, fs);
    std::vector<float> v (static_cast<size_t> (fs * 1.5));
    test::fillSine (v, hz, fs,
                    static_cast<float> (std::sqrt (2.0) * std::pow (10.0, dbfsRms / 20.0)));
    test::runAmp (amp, v);
    return test::thd (v, hz, fs);
}

double strumDb (const AmpSection::Settings& s, double fs = 48000.0)
{
    AmpSection amp;
    test::prepareAmp (amp, s, fs);
    auto v = test::guitarStrum (fs, 6.0);
    test::runAmp (amp, v);
    return test::dbfs (test::rms (v, static_cast<size_t> (fs * 0.5)));
}
} // namespace

TEST_CASE ("Amp voicing report", "[.voicing]")
{
    const double fs = 48000.0;
    {
        const auto in = test::guitarStrum (fs, 6.0);
        std::printf ("strum input: rms %.1f dBFS, peak %.1f dBFS\n", test::dbfs (test::rms (in)),
                     test::dbfs (test::peak (in)));
    }

    for (int m = 0; m < 3; ++m)
    {
        std::printf ("\n== %s ==\n", kNames[m]);
        AmpSection amp;
        test::prepareAmp (amp, defaults (m), fs);
        auto v = test::guitarStrum (fs, 6.0);
        test::runAmp (amp, v);
        std::printf ("defaults strum: rms %.1f dBFS, peak %.1f dBFS\n",
                     test::dbfs (test::rms (v, static_cast<size_t> (fs * 0.5))),
                     test::dbfs (test::peak (v)));

        std::printf ("drive : ");
        for (int g = 0; g <= 10; ++g)
            std::printf ("%6.1f", g * 0.1);
        std::printf ("\nTHD%% : ");
        for (int g = 0; g <= 10; ++g)
        {
            auto s = defaults (m);
            setDrive (s, static_cast<float> (g) * 0.1f);
            std::printf ("%6.1f", 100.0 * sineThd (s, 220.0, -18.0));
        }
        std::printf ("\nlevel : ");
        for (int g = 0; g <= 10; ++g)
        {
            auto s = defaults (m);
            setDrive (s, static_cast<float> (g) * 0.1f);
            std::printf ("%6.1f", strumDb (s));
        }
        std::printf ("\nmaster: ");
        for (int g = 0; g <= 10; g += 2)
        {
            auto s = defaults (m);
            s.lagoon.master = s.reef.master = s.abyss.master = static_cast<float> (g) * 0.1f;
            std::printf ("%6.1f", strumDb (s));
        }

        // Small-signal response relative to 1 kHz (at -50 dBFS; distorting models still compress
        // a little, so this is indicative only).
        std::printf ("\nresp  : ");
        const double freqs[] = { 50, 100, 200, 400, 800, 1600, 3200, 6400, 12800 };
        double ref = 0.0;
        {
            auto s = defaults (m);
            ref = test::measureGainDb (
                [&] (AudioView view) {
                    AmpSection a;
                    test::prepareAmp (a, s, fs);
                    a.process (view);
                },
                1000.0, fs, 48000, 0.003f);
        }
        for (double f : freqs)
        {
            auto s = defaults (m);
            const double g = test::measureGainDb (
                [&] (AudioView view) {
                    AmpSection a;
                    test::prepareAmp (a, s, fs);
                    a.process (view);
                },
                f, fs, 48000, 0.003f);
            std::printf ("%5.0f:%+5.1f ", f, g - ref);
        }
        std::printf ("\n");
    }
}

namespace
{
void printBands (const char* label, const std::vector<double>& b)
{
    std::printf ("%-14s", label);
    for (double x : b)
        std::printf ("%6.1f", x);
    std::printf ("\n");
}
} // namespace

TEST_CASE ("Amp tone and dynamics report", "[.voicing]")
{
    const double fs = 48000.0;
    const auto in = test::guitarStrum (fs, 6.0);
    std::printf ("\nbands (Hz)    ");
    for (double fc = 63.0; fc < 17000.0; fc *= 2.0)
        std::printf ("%6.0f", fc);
    std::printf ("\n");
    printBands ("DI", test::octaveBands (in, fs));
    for (int m = 0; m < 3; ++m)
    {
        AmpSection amp;
        test::prepareAmp (amp, defaults (m), fs);
        auto v = in;
        test::runAmp (amp, v);
        printBands (kNames[m], test::octaveBands (v, fs));
        test::roughCab (v, fs);
        printBands ((std::string (kNames[m]) + "+cab").c_str(), test::octaveBands (v, fs));
    }

    std::printf (
        "\ninput level (dB re. -18 dBFS) -> output level (dBFS), defaults and drive sweep\n");
    for (int row = 0; row < 9; ++row)
    {
        const int m = row < 3 ? row : (row < 6 ? 1 : 2);
        auto st = defaults (m);
        if (row >= 3)
            st.reef.gain = st.abyss.gain = row % 3 == 0 ? 0.0f : (row % 3 == 1 ? 0.75f : 1.0f);
        std::printf ("%-6s g%.2f", kNames[m], row < 3 ? 0.5 : static_cast<double> (st.reef.gain));
        double ref = 0.0;
        for (int db = -24; db <= 12; db += 6)
        {
            AmpSection amp;
            test::prepareAmp (amp, st, fs);
            auto v = in;
            for (auto& s : v)
                s *= static_cast<float> (std::pow (10.0, db / 20.0));
            test::runAmp (amp, v);
            const double out = test::dbfs (test::rms (v, static_cast<size_t> (fs * 0.5)));
            if (db == 0)
                ref = out;
            std::printf (" %+3d:%6.1f", db, out);
        }
        std::printf ("   (0 dB -> %.1f dBFS)\n", ref);
    }
}

TEST_CASE ("Amp switch and knob report", "[.voicing]")
{
    const double fs = 48000.0;
    const auto in = test::guitarStrum (fs, 6.0);
    const auto bandsFor = [&] (const AmpSection::Settings& s) {
        AmpSection amp;
        test::prepareAmp (amp, s, fs);
        auto v = in;
        test::runAmp (amp, v);
        std::printf ("[%5.1f dBFS] ", test::dbfs (test::rms (v, static_cast<size_t> (fs * 0.5))));
        return test::octaveBands (v, fs);
    };
    std::printf ("\nbands (Hz)                 ");
    for (double fc = 63.0; fc < 17000.0; fc *= 2.0)
        std::printf ("%6.0f", fc);
    std::printf ("\n");
    for (int voice = 0; voice < 3; ++voice)
    {
        auto s = defaults (1);
        s.reef.voice = voice;
        const auto b = bandsFor (s);
        printBands ((std::string ("Reef voice ") + std::to_string (voice)).c_str(), b);
    }
    for (float p : { 0.0f, 1.0f })
    {
        auto s = defaults (1);
        s.reef.presence = p;
        const auto b = bandsFor (s);
        printBands ((std::string ("Reef pres ") + std::to_string (static_cast<int> (p))).c_str(),
                    b);
    }
    for (int shift = 0; shift < 2; ++shift)
    {
        auto s = defaults (2);
        s.abyss.shift = shift == 1;
        const auto b = bandsFor (s);
        printBands ((std::string ("Abyss shift ") + std::to_string (shift)).c_str(), b);
    }
    for (int bright = 0; bright < 2; ++bright)
    {
        auto s = defaults (2);
        s.abyss.bright = bright == 1;
        s.abyss.gain = 0.2f;
        const auto b = bandsFor (s);
        printBands ((std::string ("Abyss g.2 br ") + std::to_string (bright)).c_str(), b);
    }
    for (float b0 : { 0.0f, 1.0f })
    {
        auto s = defaults (0);
        s.lagoon.bass = b0;
        const auto b = bandsFor (s);
        printBands ((std::string ("Lagoon bass ") + std::to_string (static_cast<int> (b0))).c_str(),
                    b);
    }
}

TEST_CASE ("Amp aliasing report", "[.voicing]")
{
    // Largest image of a harmonic (dB re. fundamental) for a high sine at full gain, at full and
    // at the default master (test::worstAliasDb; the models are noise-free).
    const double fs = 48000.0;
    for (int m = 0; m < 3; ++m)
        for (float master : { 1.0f, 0.5f })
            for (double f0 : { 1234.5, 2717.0, 4321.0 })
            {
                auto s = defaults (m);
                s.lagoon.volume = 1.0f;
                s.reef.gain = s.abyss.gain = 1.0f;
                s.lagoon.master = s.reef.master = s.abyss.master = master;
                AmpSection amp;
                test::prepareAmp (amp, s, fs);
                std::vector<float> v (static_cast<size_t> (fs * 2.0));
                test::fillSine (v, f0, fs, 0.25f);
                test::runAmp (amp, v);
                std::printf ("%-7s master %.1f f0 %6.1f Hz: worst alias %6.1f dB\n", kNames[m],
                             static_cast<double> (master), f0, test::worstAliasDb (v, f0, fs));
            }
}
