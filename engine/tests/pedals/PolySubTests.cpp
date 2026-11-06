// SPDX-License-Identifier: AGPL-3.0-or-later
#include "OctaverChords.h"
#include "sedef/dsp/detail/PolySub.h"

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdio>

using namespace sedef;
using dsp::detail::PolySub;
using namespace test::octaver;

namespace
{
constexpr double kStrings[] = { 82.4069, 110.0, 146.832, 195.998, 246.942, 329.628 };

struct Subs
{
    std::vector<float> one, two;
};

Subs runPoly (const std::vector<float>& in, double fs)
{
    PolySub p;
    p.prepare (fs);
    Subs s;
    s.one.resize (in.size());
    s.two.resize (in.size());
    for (size_t i = 0; i < in.size(); ++i)
        p.process (in[i], s.one[i], s.two[i]);
    return s;
}

/// The strongest component within 30 cents of `hz`, in cents from it.
double peakCents (const std::vector<float>& x, double fs, double from, double seconds, double hz)
{
    double best = -1.0e9, bestCents = 0.0;
    for (double c = -30.0; c <= 30.0; c += 0.25)
    {
        const double v =
            dftDb (x, static_cast<size_t> (from * fs), static_cast<size_t> (seconds * fs),
                   hz * std::exp2 (c / 1200.0), fs);
        if (v > best)
        {
            best = v;
            bestCents = c;
        }
    }
    return bestCents;
}

double power (const std::vector<float>& x, size_t from, size_t to)
{
    double acc = 0.0;
    for (size_t i = from; i < to; ++i)
        acc += static_cast<double> (x[i]) * x[i];
    return acc / static_cast<double> (to - from);
}

struct StringResult
{
    double levelDb = 0.0; ///< sub against an ideal octave down, 0.4-0.73 s after the pick
    double lagMs = 0.0;   ///< reaching half the settled level, behind the ideal octave down
    double cents1 = 0.0, cents2 = 0.0; ///< the subs' fundamentals against f/2 and f/4
};

StringResult divideString (double f, double fs)
{
    std::vector<float> in (static_cast<size_t> (1.0 * fs), 0.0f), ideal (in.size(), 0.0f);
    const auto p = chordString (f, 0);
    test::addPluck (in, fs, static_cast<size_t> (0.1 * fs), p);
    test::addPluck (ideal, fs, static_cast<size_t> (0.1 * fs), octaveDown (p));
    const auto s = runPoly (in, fs);
    const auto a = static_cast<size_t> (0.5 * fs), b = static_cast<size_t> (0.83 * fs);
    return { 10.0 * std::log10 (power (s.one, a, b) / power (ideal, a, b)),
             riseLagMs (s.one, ideal, fs, 0.1, 2.0 / f), peakCents (s.one, fs, 0.4, 0.5, f / 2.0),
             peakCents (s.two, fs, 0.4, 0.5, f / 4.0) };
}

struct ChordResult
{
    ChordSub sub;
    double worstCents = 0.0, warbleDb = 0.0;
};

ChordResult divideChord (const Chord& c)
{
    std::vector<float> in, ideal;
    renderChord (c, 48000.0, 1.8, in, ideal);
    const auto s = runPoly (in, 48000.0);
    ChordResult r { judgeChordSub (c, s.one, 48000.0), 0.0, warbleDb (s.one, 48000.0, 0.4, 1.7) };
    for (double x : r.sub.noteCents)
        r.worstCents = std::max (r.worstCents, std::abs (x));
    return r;
}

const std::vector<std::vector<double>> kMuted { { 82.4069 },
                                                { 110.0 },
                                                { 82.4069, 123.471, 164.814 } };

/// Milliseconds from a mute (the strings damped 0.5 s after the pick) until the sub's 10 ms
/// level is 60 dB under its level over the 350 ms before.
double muteMs (const std::vector<double>& notes)
{
    constexpr double fs = 48000.0;
    std::vector<float> in (static_cast<size_t> (1.2 * fs), 0.0f);
    for (size_t k = 0; k < notes.size(); ++k)
    {
        auto p = chordString (notes[k], static_cast<int> (k));
        p.muteAt = 0.5;
        test::addPluck (in, fs, static_cast<size_t> (0.1 * fs), p);
    }
    const auto s = runPoly (in, fs);
    const double held =
        power (s.one, static_cast<size_t> (0.25 * fs), static_cast<size_t> (0.6 * fs));
    for (double t = 0.6; t < 1.1; t += 0.001)
        if (power (s.one, static_cast<size_t> (t * fs), static_cast<size_t> ((t + 0.01) * fs)) <
            1.0e-6 * held)
            return 1000.0 * (t + 0.01 - 0.6);
    return 1.0e9;
}
} // namespace

TEST_CASE ("PolySub divides every string an octave and two octaves down",
           "[pedals][trench][polysub]")
{
    // One string at a time: the sub is the note's own spectrum at half its frequency, as loud as
    // the note (within 1.5 dB of an ideal octave down), and reaches half its level at most 13 ms
    // after the ideal one does. The two-octave sub sits at a quarter of the note.
    for (double fs : { 44100.0, 48000.0, 96000.0 })
        for (double f : kStrings)
        {
            INFO ("fs " << fs << ", string " << f << " Hz");
            const auto r = divideString (f, fs);
            CHECK (std::abs (r.levelDb) < 1.5);
            CHECK (r.lagMs < 13.0);
            CHECK (std::abs (r.cents1) < 5.0);
            CHECK (std::abs (r.cents2) < 5.0);
        }
}

TEST_CASE ("PolySub plays chords an octave down", "[pedals][trench][polysub]")
{
    // Open chords, power chords and a barre, after the strum has settled: every note has its sub
    // within 10 cents, the power away from the ideal octave down's partials is 20 dB under the
    // power on them, its strongest peak is 20 dB under the strongest note's sub, and the level
    // does not warble by more than 3 dB.
    for (const auto& c : chords())
    {
        INFO (c.name);
        const auto r = divideChord (c);
        CHECK (r.worstCents < 10.0);
        CHECK (r.sub.offGridDb < -20.0);
        CHECK (r.sub.worstPeakDb < -20.0);
        CHECK (r.warbleDb < 3.0);
    }
}

TEST_CASE ("PolySub is silent without a note and stops after a mute", "[pedals][trench][polysub]")
{
    constexpr double fs = 48000.0;
    {
        std::vector<float> silence (static_cast<size_t> (fs), 0.0f);
        const auto s = runPoly (silence, fs);
        CHECK (std::all_of (s.one.begin(), s.one.end(), [] (float v) { return v == 0.0f; }));
        CHECK (std::all_of (s.two.begin(), s.two.end(), [] (float v) { return v == 0.0f; }));
    }
    // A note (or a chord) damped by the hand: the sub is 60 dB down within 150 ms.
    for (const auto& notes : kMuted)
    {
        INFO ("lowest note " << notes[0] << " Hz, " << notes.size() << " strings");
        CHECK (muteMs (notes) < 150.0);
    }
}

TEST_CASE ("PolySub stays bounded on hot input", "[pedals][trench][polysub]")
{
    // Full-scale noise and a full-scale square: finite, and never far above the input.
    constexpr double fs = 48000.0;
    std::vector<float> in (static_cast<size_t> (fs), 0.0f);
    test::addNoise (in, 0.0f);
    for (size_t i = in.size() / 2; i < in.size(); ++i)
        in[i] = (i / 120) % 2 == 0 ? 1.0f : -1.0f;
    const auto s = runPoly (in, fs);
    float peak = 0.0f;
    for (size_t i = 0; i < in.size(); ++i)
    {
        REQUIRE (std::isfinite (s.one[i]));
        REQUIRE (std::isfinite (s.two[i]));
        peak = std::max ({ peak, std::abs (s.one[i]), std::abs (s.two[i]) });
    }
    CHECK (peak < 4.0f);
}

TEST_CASE ("PolySub resets to its initial state", "[pedals][trench][polysub]")
{
    constexpr double fs = 48000.0;
    std::vector<float> in, ideal;
    renderChord (chords()[0], fs, 0.6, in, ideal);
    PolySub p, fresh;
    p.prepare (fs);
    fresh.prepare (fs);
    float a = 0.0f, b = 0.0f, c = 0.0f, d = 0.0f;
    for (float x : in)
        p.process (x, a, b);
    CHECK (p.partials() > 0);
    p.reset();
    CHECK (p.partials() == 0);
    for (float x : in)
    {
        p.process (x, a, b);
        fresh.process (x, c, d);
        REQUIRE (a == c);
        REQUIRE (b == d);
    }
}

TEST_CASE ("PolySub measurements", "[.report]")
{
    std::printf ("PolySub (Trench Poly): each string alone, against an ideal octave down\n");
    std::printf ("rate     string    level     lag      sub -1    sub -2\n");
    for (double fs : { 44100.0, 48000.0, 96000.0 })
        for (double f : kStrings)
        {
            const auto r = divideString (f, fs);
            std::printf ("%5.1fk  %6.1f Hz  %+5.2f dB  %5.2f ms  %+5.2f c  %+5.2f c\n", fs / 1000.0,
                         f, r.levelDb, r.lagMs, r.cents1, r.cents2);
        }
    std::printf ("\nchords, 48 kHz, 0.45-1.45 s\n");
    std::printf ("chord           notes    off-grid  worst peak (re strongest / weakest note)  "
                 "warble\n");
    for (const auto& c : chords())
    {
        const auto r = divideChord (c);
        std::printf ("%-14s  %4.2f c  %+5.1f dB  %+5.1f / %+5.1f dB at %5.1f Hz  %4.2f dB\n",
                     c.name.c_str(), r.worstCents, r.sub.offGridDb, r.sub.worstPeakDb,
                     r.sub.worstPeakVsWeakestDb, r.sub.worstPeakHz, r.warbleDb);
    }
    std::printf ("\nmutes: 60 dB down after");
    for (const auto& notes : kMuted)
        std::printf ("  %.0f ms (%zu string%s from %.1f Hz)", muteMs (notes), notes.size(),
                     notes.size() > 1 ? "s" : "", notes[0]);
    std::printf ("\n");
}
