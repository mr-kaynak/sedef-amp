// SPDX-License-Identifier: AGPL-3.0-or-later
#include "../pedals/OctaverDi.h"
#include "PitchTestSignals.h"
#include "sedef/analysis/MonoTracker.h"

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdio>

using namespace sedef;
using analysis::MonoTracker;
using namespace test::octaver;

namespace
{
constexpr double kFs = 48000.0;
constexpr double kNotes[] = { kE2, kA2, kE3, kA3, kE4, kE5 };

/// What the tracker said, every 0.5 ms.
struct Trace
{
    std::vector<double> time;
    std::vector<float> hz;
    std::vector<bool> voiced;
    std::vector<std::uint32_t> noteId;
    double turns = 0.0; ///< turns of the fundamental's phase over [turnsFrom, turnsTo)
};

Trace trace (const std::vector<float>& x, double fs, double turnsFrom = 1.0e9,
             double turnsTo = 1.0e9)
{
    MonoTracker m;
    m.prepare (fs);
    Trace t;
    const auto every = static_cast<size_t> (std::lround (0.0005 * fs));
    float lastRe = 0.0f, lastIm = 0.0f;
    for (size_t i = 0; i < x.size(); ++i)
    {
        m.push (x[i]);
        const double now = static_cast<double> (i + 1) / fs;
        const float re = m.fundamentalRe(), im = m.fundamentalIm();
        if (now >= turnsFrom && now < turnsTo)
            t.turns += std::atan2 (im * lastRe - re * lastIm, re * lastRe + im * lastIm) / kTwoPiD;
        lastRe = re;
        lastIm = im;
        if ((i + 1) % every == 0)
        {
            t.time.push_back (now);
            t.hz.push_back (m.hz());
            t.voiced.push_back (m.voiced());
            t.noteId.push_back (m.noteId());
        }
    }
    return t;
}

/// Milliseconds from `from` to the first report of a note within 20 cents of `hz`.
double lockMs (const Trace& t, double from, double hz)
{
    for (size_t k = 0; k < t.time.size(); ++k)
        if (t.time[k] > from && t.voiced[k] && std::abs (test::centsError (t.hz[k], hz)) < 20.0)
            return 1000.0 * (t.time[k] - from);
    return 1.0e9;
}

/// Share of reports in [from, to) naming the note within 50 cents, and the median error.
struct Accuracy
{
    double right = 0.0, medianCents = 0.0;
};

Accuracy accuracy (const Trace& t, double from, double to, double hz)
{
    std::vector<double> cents;
    int right = 0, total = 0;
    for (size_t k = 0; k < t.time.size(); ++k)
        if (t.time[k] >= from && t.time[k] < to)
        {
            ++total;
            const double c = t.voiced[k] ? test::centsError (t.hz[k], hz) : 1.0e4;
            right += std::abs (c) < 50.0 ? 1 : 0;
            cents.push_back (c);
        }
    std::nth_element (cents.begin(), cents.begin() + static_cast<std::ptrdiff_t> (cents.size() / 2),
                      cents.end());
    return { total > 0 ? static_cast<double> (right) / total : 0.0,
             cents.empty() ? 1.0e4 : cents[cents.size() / 2] };
}
} // namespace

TEST_CASE ("MonoTracker locks within about two periods", "[analysis][monotracker]")
{
    // The estimator judges a period on one period of signal, so a note can be read once two of
    // them have arrived, and nothing is judged inside the first 8 ms of a pick. Bounds: two
    // periods or 8 ms, plus 5 ms for the pick's detection and the hop.
    for (double fs : { 44100.0, 48000.0, 96000.0, 192000.0 })
        for (double f : kNotes)
        {
            INFO ("fs " << fs << ", note " << f << " Hz");
            const auto s = single ("bridge", bridgePluck (f), fs, 0.6);
            const double ms = lockMs (trace (s.signal, fs), 0.1, f);
            CHECK (ms < std::max (2000.0 / f, 8.0) + 5.0);
        }
}

TEST_CASE ("MonoTracker names bright notes in the right octave", "[analysis][monotracker]")
{
    // A bridge pickup (2nd and 3rd partials 5-8 dB over the fundamental), a piezo-like note
    // (fundamental 4 dB under the 2nd partial) and a loud pick: after the lock, more than 99 % of
    // the reports name the note, and their median is within 3 cents.
    for (double fs : { 44100.0, 96000.0 })
        for (double f : kNotes)
            for (int kind = 0; kind < 3; ++kind)
            {
                test::Pluck p = kind == 1 ? piezoPluck (f) : bridgePluck (f);
                if (kind == 2)
                    p.pickNoise = 0.3f;
                INFO ("fs " << fs << ", note " << f << " Hz, kind " << kind);
                const auto t = trace (single ("note", p, fs, 1.4).signal, fs);
                const double lock = 0.1 + lockMs (t, 0.1, f) / 1000.0;
                const auto a = accuracy (t, lock, 1.4, f);
                CHECK (a.right > 0.99);
                CHECK (std::abs (a.medianCents) < 3.0);
            }
}

TEST_CASE ("MonoTracker's fundamental turns once per period", "[analysis][monotracker]")
{
    // The analytic fundamental is what an octave divider halves: over a second of a bright note
    // its phase must turn exactly f times (a slip would be a wrong sub period).
    for (double f : kNotes)
    {
        INFO ("note " << f << " Hz");
        test::Pluck p = bridgePluck (f);
        p.decaySeconds = 3.0;
        p.glideCents = 0.0;
        const auto t = trace (single ("note", p, kFs, 1.5).signal, kFs, 0.4, 1.4);
        CHECK (std::abs (t.turns - f) < 0.5);
    }
}

TEST_CASE ("MonoTracker follows the new note over a ringing neighbour", "[analysis][monotracker]")
{
    // The previous note, a fourth below or a fifth above, rings 6 dB under the new one. Together
    // they repeat at a period no note has (a "virtual fundamental"), which must not be reported.
    // The new note is named within two of its periods plus 10 ms, and then held.
    for (double f : { kE2, kE3, kA3 })
        for (double cents : { -500.0, 700.0 })
        {
            INFO ("note " << f << " Hz, neighbour at " << cents << " cents");
            const auto s = ringingNeighbour (f, cents, kFs);
            const auto t = trace (s.signal, kFs);
            const double lock = 0.5 + lockMs (t, 0.5, f) / 1000.0;
            CHECK (lock < 0.5 + std::max (2.0 / f, 0.008) + 0.01);
            CHECK (accuracy (t, lock, 1.6, f).right > 0.99);
        }
}

TEST_CASE ("MonoTracker follows vibrato, bends and legato runs", "[analysis][monotracker]")
{
    for (double f : { kE2, kA3 })
    {
        for (const auto& s : { vibrato (f, kFs), bend (f, kFs) })
        {
            INFO (s.name << " on " << f << " Hz");
            const auto t = trace (s.signal, kFs);
            const auto& n = s.notes[0];
            int right = 0, total = 0;
            for (size_t k = 0; k < t.time.size(); ++k)
                if (t.time[k] > n.start + 0.05)
                {
                    ++total;
                    right += t.voiced[k] && std::abs (test::centsError (t.hz[k],
                                                                        n.hzAt (t.time[k]))) < 50.0
                                 ? 1
                                 : 0;
                }
            CHECK (right > 0.99 * total);
        }
    }

    // Hammer-ons a whole step apart, 150 ms each: each named within 15 ms of the step.
    for (double f : { kA2, kE3 })
    {
        INFO ("legato run from " << f << " Hz");
        const auto s = legatoRun (f, kFs);
        const auto t = trace (s.signal, kFs);
        for (size_t k = 1; k < s.notes.size(); ++k)
        {
            const auto& n = s.notes[k];
            CHECK (lockMs (t, n.start, n.hzAt (n.start + 0.05)) < 15.0);
        }
    }
}

TEST_CASE ("MonoTracker stays quiet without a note", "[analysis][monotracker]")
{
    // Silence, hiss at -40 dBFS and an open fifth (two notes at equal level, no fundamental at
    // their common period): nothing is reported as a note an octave or more below them.
    {
        std::vector<float> silence (static_cast<size_t> (kFs), 0.0f);
        const auto t = trace (silence, kFs);
        CHECK (std::none_of (t.voiced.begin(), t.voiced.end(), [] (bool v) { return v; }));
    }
    {
        std::vector<float> hiss (static_cast<size_t> (kFs), 0.0f);
        test::addNoise (hiss, -40.0f);
        const auto t = trace (hiss, kFs);
        CHECK (std::none_of (t.voiced.begin(), t.voiced.end(), [] (bool v) { return v; }));
    }
    {
        std::vector<float> fifth (static_cast<size_t> (1.2 * kFs), 0.0f);
        test::Pluck a, e;
        a.f1 = kA2;
        e.f1 = kA2 * 1.5;
        e.seed = 2;
        test::addPluck (fifth, kFs, 4800, a);
        test::addPluck (fifth, kFs, 4800, e);
        const auto t = trace (fifth, kFs);
        for (size_t k = 0; k < t.time.size(); ++k)
            if (t.voiced[k])
                CHECK (t.hz[k] > 0.9 * kA2);
    }
}

TEST_CASE ("MonoTracker resets to its initial state", "[analysis][monotracker]")
{
    const auto s = single ("note", bridgePluck (kE3), kFs, 0.5);
    MonoTracker m;
    m.prepare (kFs);
    for (float x : s.signal)
        m.push (x);
    CHECK (m.voiced());
    m.reset();
    CHECK_FALSE (m.voiced());
    CHECK (m.hz() == 0.0f);
    MonoTracker fresh;
    fresh.prepare (kFs);
    for (float x : s.signal)
    {
        m.push (x);
        fresh.push (x);
        REQUIRE (m.fundamentalRe() == fresh.fundamentalRe());
        REQUIRE (m.hz() == fresh.hz());
    }
}
