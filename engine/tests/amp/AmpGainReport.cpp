// SPDX-License-Identifier: AGPL-3.0-or-later
// Hidden gain-staging report: where each amp starts to distort, how hard it compresses, how long a
// note sustains and how peaky the output is, per drive setting and DI level.
// Run with: sedef_engine_tests "[.gain]"
#include "AmpTestUtils.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include <string>

using namespace sedef;
using dsp::AmpSection;

namespace
{
constexpr double kFs = 48000.0;
const char* kNames[] = { "Lagoon", "Reef", "Abyss" };
const char* kDriveNames[] = { "volume", "gain", "gain" };
constexpr float kDrives[] = { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f };
constexpr double kDiLevels[] = { -30.0, -24.0, -18.0, -12.0 };
constexpr double kNoteHz = 196.0; // G3: a lead note in the middle of the neck

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

double peakEnvelopeDb (const std::vector<float>& v)
{
    const auto env = test::envelopeDb (v, kFs, 10.0, 2.5);
    return *std::max_element (env.begin(), env.end());
}

double crestDb (const std::vector<float>& v)
{
    const std::vector<float> body (v.begin() + static_cast<long> (0.5 * kFs), v.end());
    return test::dbfs (test::peak (body)) - test::dbfs (test::rms (body));
}

std::string fmt (double v, const char* format = "%6.1f")
{
    if (std::isnan (v))
        return "     -";
    char b[32];
    std::snprintf (b, sizeof b, format, v);
    return b;
}

std::vector<double> levelSweep()
{
    std::vector<double> l;
    for (int db = -80; db <= 0; db += 2)
        l.push_back (db);
    return l;
}

struct Knees
{
    double thd10 = 0.0, sine1 = 0.0, sine6 = 0.0, note1 = 0.0, note6 = 0.0;
};

/// Input levels (dBFS RMS; for the note, the matching strum level) where THD reaches 10 % and
/// where the output falls 1 dB and 6 dB behind the small-signal gain.
Knees knees (const AmpSection::Settings& s)
{
    const auto levels = levelSweep();
    const auto sineAt = [&] (double db) {
        return run (s, test::sineAt (220.0, kFs, 1.0, db));
    };
    const auto tail = [] (const std::vector<float>& v) {
        return test::dbfs (test::rms (v, v.size() - 32768));
    };
    const double sineRef = tail (sineAt (-100.0)) + 100.0;
    const auto note = [&] (double db) {
        const auto in = test::guitarNote (kFs, 1.0, kNoteHz, db);
        return peakEnvelopeDb (run (s, in)) - peakEnvelopeDb (in);
    };
    const double noteRef = note (-100.0);

    std::vector<double> thd, sineLoss, noteLoss;
    for (double db : levels)
    {
        const auto v = sineAt (db);
        thd.push_back (test::thd (v, 220.0, kFs));
        sineLoss.push_back (sineRef - (tail (v) - db));
        noteLoss.push_back (noteRef - note (db));
    }
    return { test::crossingLevel (levels, thd, 0.1), test::crossingLevel (levels, sineLoss, 1.0),
             test::crossingLevel (levels, sineLoss, 6.0),
             test::crossingLevel (levels, noteLoss, 1.0),
             test::crossingLevel (levels, noteLoss, 6.0) };
}

std::vector<float> scaled (std::vector<float> v, double db)
{
    const auto g = static_cast<float> (std::pow (10.0, db / 20.0));
    for (auto& x : v)
        x *= g;
    return v;
}
} // namespace

TEST_CASE ("Amp gain staging report", "[.gain]")
{
    const auto strum = test::guitarStrum (kFs, 6.0);
    std::printf ("Reference signals at -18 dBFS: strum peak %.1f dBFS, crest %.1f dB; G3 note peak "
                 "%.1f dBFS\n",
                 test::dbfs (test::peak (strum)), crestDb (strum),
                 test::dbfs (test::peak (test::guitarNote (kFs, 1.0, kNoteHz))));

    std::printf ("\n1. Knees: input level in dBFS RMS (220 Hz sine; G3 note: level of the matching "
                 "strum)\n");
    std::printf ("%-7s %-7s %6s | %7s %7s | %7s %7s\n", "model", "drive", "THD10", "sine1dB",
                 "sine6dB", "note1dB", "note6dB");
    for (int m = 0; m < 3; ++m)
        for (float g : kDrives)
        {
            const auto k = knees (at (m, g));
            std::printf ("%-7s %s %4.2f %s | %s  %s | %s  %s\n", kNames[m], kDriveNames[m], g,
                         fmt (k.thd10).c_str(), fmt (k.sine1).c_str(), fmt (k.sine6).c_str(),
                         fmt (k.note1).c_str(), fmt (k.note6).c_str());
        }

    std::printf ("\n2. Sustain: seconds a G3 note stays within 6 dB of its peak, per DI level\n");
    std::printf ("%-15s", "DI dBFS");
    for (double l : kDiLevels)
        std::printf ("%7.0f", l);
    std::printf ("\n%-15s", "dry note");
    for (double l : kDiLevels)
        std::printf ("%7.2f", test::sustainSeconds (test::guitarNote (kFs, 8.0, kNoteHz, l), kFs));
    std::printf ("\n");
    for (int m = 0; m < 3; ++m)
        for (float g : kDrives)
        {
            std::printf ("%-7s %4.2f   ", kNames[m], g);
            for (double l : kDiLevels)
                std::printf ("%7.2f",
                             test::sustainSeconds (
                                 run (at (m, g), test::guitarNote (kFs, 8.0, kNoteHz, l)), kFs));
            std::printf ("\n");
        }

    std::printf ("\n3. Strum output: RMS dBFS / crest factor dB, per DI level\n");
    std::printf ("%-15s", "DI dBFS");
    for (double l : kDiLevels)
        std::printf ("%13.0f", l);
    std::printf ("\n%-15s", "dry strum");
    for (double l : kDiLevels)
        std::printf ("  %5.1f / %4.1f", l, crestDb (scaled (strum, l + 18.0)));
    std::printf ("\n");
    for (int m = 0; m < 3; ++m)
        for (float g : kDrives)
        {
            std::printf ("%-7s %4.2f   ", kNames[m], g);
            for (double l : kDiLevels)
            {
                const auto v = run (at (m, g), scaled (strum, l + 18.0));
                std::printf ("  %5.1f / %4.1f",
                             test::dbfs (test::rms (v, static_cast<size_t> (0.5 * kFs))),
                             crestDb (v));
            }
            std::printf ("\n");
        }

    std::printf ("\n4. Palm-muted chugs at -18 dBFS, full drive: octave bands re. total (raw / "
                 "through the rough cab)\n%-14s",
                 "bands (Hz)");
    for (double fc = 63.0; fc < 17000.0; fc *= 2.0)
        std::printf ("%6.0f", fc);
    std::printf ("\n");
    const auto chugs = test::palmChugs (kFs, 4.0);
    for (int m = 0; m < 3; ++m)
    {
        auto v = run (at (m, 1.0f), chugs);
        std::printf ("%-14s", kNames[m]);
        for (double b : test::octaveBands (v, kFs))
            std::printf ("%6.1f", b);
        test::roughCab (v, kFs);
        std::printf ("\n%-14s", "  +cab");
        for (double b : test::octaveBands (v, kFs))
            std::printf ("%6.1f", b);
        std::printf ("\n");
    }
}
