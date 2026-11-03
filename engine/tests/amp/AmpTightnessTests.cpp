// SPDX-License-Identifier: AGPL-3.0-or-later
// Abyss stays a tight, articulate high-gain amp rather than a fuzz (docs/dsp/amps.md, section 5):
// palm mutes stop, power chords keep their notes apart, the low strings reach the clippers later
// than the mids, and the default gain still answers the pick. The v0.3.0 numbers in the comments
// are the ones this test was written against (hidden report "[.tight]").
#include "AmpTestUtils.h"

#include <catch2/catch_test_macros.hpp>
#include <functional>

using namespace sedef;
using dsp::AmpSection;

namespace
{
constexpr double kFs = 48000.0;
constexpr double kE2 = 82.41, kB2 = 123.47, kE3 = 164.81, kGs3 = 207.65, kB3 = 246.94;

AmpSection::Settings abyss (float gain)
{
    AmpSection::Settings s;
    s.model = AmpSection::Abyss;
    s.abyss.gain = gain;
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
        v = run (s, std::move (v));
    };
}

double peakEnvelopeDb (const std::vector<float>& v)
{
    const auto env = test::envelopeDb (v, kFs, 10.0, 2.5);
    return *std::max_element (env.begin(), env.end());
}
} // namespace

TEST_CASE ("Abyss palm mutes stop instead of droning", "[amp]")
{
    // One palm-muted E5 chug at DI -18: seconds it rings above its peak -20 dB (dry: 0.18 s), and
    // how far the output falls between eighth-note chugs at 150 bpm (dry: 30 dB). v0.3.0 held a
    // chug for 0.39 s at gain 0.5 and the chugs ran together (10 dB between them).
    const auto chug = test::palmChug (kFs, 2.0);
    const auto train = test::palmChugs (kFs, 4.0, 150.0);
    struct Target
    {
        float gain;
        double maxRing, minDip;
    };
    for (const auto& t :
         { Target { 0.3f, 0.23, 25.0 }, Target { 0.5f, 0.31, 17.0 }, Target { 0.75f, 0.46, 5.5 } })
    {
        const double ring = test::ringSeconds (run (abyss (t.gain), chug), kFs, 20.0);
        const double dip = test::chugDipDb (run (abyss (t.gain), train), kFs, 150.0);
        CAPTURE (t.gain, ring, dip); // measured 0.20 / 28.0, 0.27 / 20.5, 0.42 / 6.1
        CHECK (ring <= t.maxRing);   // v0.3.0: 0.28, 0.39, 0.49
        CHECK (dip >= t.minDip);     // v0.3.0: 20.0, 10.3, 4.9
    }
}

TEST_CASE ("Abyss power chords and triads keep their notes apart", "[amp]")
{
    // Intermodulation: power between the notes' harmonics re. the power on them (the DI has
    // exactly harmonic partials, so everything in between is sum and difference tones).
    const std::vector<double> e5 { kE2, kB2, kE3 }, triad { kE3, kGs3, kB3 };
    const auto start = static_cast<size_t> (0.1 * kFs);
    const auto s = abyss (0.5f);

    // E2 + B2 sines at -24 dBFS RMS each (a power chord as two pure tones).
    const double twoTone = test::twoToneImdDb (processOf (s), kE2, kB2, kFs, -24.0);
    // A ringing E5 (E2 B2 E3) and an E major triad (E3 G#3 B3), picked as hard as a -18 dBFS strum.
    const double chord =
        test::inharmonicDb (run (s, test::ringingChord (kFs, 3.0, e5)), kFs, e5, start);
    const double third =
        test::inharmonicDb (run (s, test::ringingChord (kFs, 3.0, triad)), kFs, triad, start);
    CAPTURE (twoTone, chord, third); // measured -18.7, -17.5, -8.9
    CHECK (twoTone < -18.0);         // v0.3.0: -16.9
    CHECK (chord < -16.0);           // v0.3.0: -11.3
    CHECK (third < -7.5);            // v0.3.0: -4.9

    // Full gain is the lead wall: chords blur there on any high-gain amp, but no more than before.
    const double chordFull =
        test::inharmonicDb (run (abyss (1.0f), test::ringingChord (kFs, 3.0, e5)), kFs, e5, start);
    CAPTURE (chordFull); // measured -10.3, v0.3.0 -10.2
    CHECK (chordFull < -9.5);
}

TEST_CASE ("Abyss keeps the low strings out of the clippers", "[amp]")
{
    // Input level where an 82 Hz sine (low E) is compressed by 6 dB, minus the level for 660 Hz:
    // how much later the low end saturates than the upper mids (v0.3.0: 12.6 dB; Reef: 2 dB).
    for (float g : { 0.5f, 1.0f })
    {
        const auto p = processOf (abyss (g));
        const double lowE = test::compressionPoint (p, kE2, kFs, 6.0);
        const double mids = test::compressionPoint (p, 660.0, kFs, 6.0);
        CAPTURE (g, lowE, mids, lowE - mids); // measured 19.2 and 19.0 dB
        CHECK (lowE - mids >= 17.0);
    }
    // At the default gain a low E at DI -24 passes almost uncompressed while 660 Hz saturates.
    const auto p = processOf (abyss (0.5f));
    const double low =
        test::sineGainDb (p, kE2, kFs, -100.0) - test::sineGainDb (p, kE2, kFs, -24.0);
    const double mid =
        test::sineGainDb (p, 660.0, kFs, -100.0) - test::sineGainDb (p, 660.0, kFs, -24.0);
    CAPTURE (low, mid); // measured 0.3 / 12.8 dB; v0.3.0 9.0 / 20.6
    CHECK (low < 2.0);
    CHECK (mid > 8.0);
}

TEST_CASE ("Abyss compression points: rhythm at noon, lead wall at full gain", "[amp]")
{
    // 1 dB compression point of a 220 Hz sine (dBFS RMS in).
    const double noon = test::compressionPoint (processOf (abyss (0.5f)), 220.0, kFs, 1.0);
    const double full = test::compressionPoint (processOf (abyss (1.0f)), 220.0, kFs, 1.0);
    CAPTURE (noon, full); // measured -31.2 / -69.7; v0.3.0 -44.9 / -73.5
    CHECK (noon > -35.0); // a strum at DI -18 is about 15 dB into saturation, not 27
    CHECK (noon < -27.0); // still a high-gain rhythm sound (Reef at noon: -27.8)
    CHECK (full < -66.0); // light single notes 30+ dB into saturation: leads sustain
}

TEST_CASE ("Abyss at the default gain answers the pick", "[amp]")
{
    // A G3 picked from DI -36 to -12 (24 dB): how much the output follows (dry 24 dB, v0.3.0
    // 3.0 dB), and a soft pick still cleans up below 10 % THD (v0.3.0: 14.6 %).
    std::vector<double> level;
    for (double di : { -36.0, -12.0 })
        level.push_back (
            peakEnvelopeDb (run (abyss (0.5f), test::guitarNote (kFs, 1.5, 196.0, di))));
    const double range = level[1] - level[0];
    const double soft =
        test::thd (run (abyss (0.5f), test::sineAt (220.0, kFs, 1.0, -36.0)), 220.0, kFs);
    CAPTURE (range, soft); // measured 6.9 dB, 8.7 %
    CHECK (range > 5.5);
    CHECK (soft < 0.1);

    // Not a square-wave buzz: at DI -24 the even harmonics outweigh the odd ones (v0.3.0: odd
    // 12 dB above even, crest factor 1.5 dB, nearly a square's 0 dB).
    auto v = run (abyss (0.5f), test::sineAt (220.0, kFs, 1.0, -24.0));
    const int n = 32768;
    const float* d = v.data() + v.size() - n;
    double odd = 0.0, even = 0.0;
    for (int k = 2; k <= 15; ++k)
    {
        const double h = test::toneMagnitude (d, n, 220.0 * k, kFs);
        (k % 2 == 0 ? even : odd) += h * h;
    }
    const std::vector<float> tail (v.end() - n, v.end());
    const double crest = test::dbfs (test::peak (tail)) - test::dbfs (test::rms (tail));
    CAPTURE (10.0 * std::log10 (odd / even), crest); // measured -5.4 dB, 2.9 dB
    CHECK (odd < even);
    CHECK (crest > 2.3);
}

TEST_CASE ("Abyss at the default gain is less fizzy than v0.3.0", "[amp]")
{
    // Strum at DI -18: power above 6 kHz re. the total, raw amp output (v0.3.0: -19.9 dB).
    const auto v = run (abyss (0.5f), test::guitarStrum (kFs, 6.0));
    const size_t n = 131072;
    const auto p = test::powerSpectrum (v, static_cast<size_t> (0.5 * kFs), n);
    double above = 0.0, total = 0.0;
    for (size_t k = 1; k < p.size(); ++k)
    {
        total += p[k];
        if (static_cast<double> (k) * kFs / static_cast<double> (n) >= 6000.0)
            above += p[k];
    }
    const double db = 10.0 * std::log10 (above / total);
    CAPTURE (db); // measured -23.8
    CHECK (db < -22.0);
}
