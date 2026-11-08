// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

// Chords for Trench's Poly mode, and the measurements its bars are written in: each chord
// rendered together with its ideal octave down (every partial at half its frequency, same
// amplitude, decay and phase), so the sub can be judged against what it should be.

#include "../analysis/PitchTestSignals.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace sedef::test::octaver
{

struct Chord
{
    std::string name;
    std::vector<double> notes;   ///< Hz, low string first
    double strumSeconds = 0.010; ///< between strings
};

/// Open E and A majors, E5 and A5 power chords (struck at once), an F barre.
inline std::vector<Chord> chords()
{
    return { { "E major open", { 82.4069, 123.471, 164.814, 207.652, 246.942, 329.628 } },
             { "A major open", { 110.0, 164.814, 220.0, 277.183, 329.628 } },
             { "E5 power", { 82.4069, 123.471, 164.814 }, 0.0 },
             { "A5 power", { 110.0, 164.814, 220.0 }, 0.0 },
             { "F major barre", { 87.3071, 130.813, 174.614, 220.0, 261.626, 349.228 } } };
}

/// One string of a chord: a 3 s decay, a little pick noise, its own noise seed.
inline Pluck chordString (double f1, int string)
{
    Pluck p;
    p.f1 = f1;
    p.peak = 0.12f;
    p.pickNoise = 0.02f;
    p.decaySeconds = 3.0;
    p.seed = static_cast<std::uint32_t> (string + 3);
    return p;
}

/// The same string an octave down, as an ideal octaver would play it.
inline Pluck octaveDown (const Pluck& p)
{
    Pluck q = p;
    q.f1 = p.f1 / 2.0;
    q.maxPartialHz = p.maxPartialHz / 2.0;
    q.pickNoise = 0.0f;
    return q;
}

/// Renders the chord, picked at `pick` seconds, and its ideal octave down.
inline void renderChord (const Chord& c, double fs, double seconds, std::vector<float>& in,
                         std::vector<float>& ideal, double pick = 0.1)
{
    const auto n = static_cast<size_t> (seconds * fs);
    in.assign (n, 0.0f);
    ideal.assign (n, 0.0f);
    for (size_t s = 0; s < c.notes.size(); ++s)
    {
        const auto p = chordString (c.notes[s], static_cast<int> (s));
        const auto at =
            static_cast<size_t> ((pick + c.strumSeconds * static_cast<double> (s)) * fs);
        addPluck (in, fs, at, p);
        addPluck (ideal, fs, at, octaveDown (p));
    }
}

/// Hann-windowed DFT magnitude (dB re a full-scale sine) at `hz` over [start, start + n).
inline double dftDb (const std::vector<float>& x, size_t start, size_t n, double hz, double fs)
{
    const double w = 2.0 * kPiD * hz / fs;
    const double cw = std::cos (w), sw = std::sin (w);
    double re = 0.0, im = 0.0, c = 1.0, s = 0.0;
    for (size_t i = 0; i < n; ++i)
    {
        const double win = 0.5 - 0.5 * std::cos (2.0 * kPiD * static_cast<double> (i) /
                                                 static_cast<double> (n - 1));
        const double v = x[start + i] * win;
        re += v * c;
        im -= v * s;
        const double next = c * cw - s * sw;
        s = s * cw + c * sw;
        c = next;
    }
    return 20.0 *
           std::log10 (std::max (1.0e-12, 4.0 * std::hypot (re, im) / static_cast<double> (n)));
}

/// The partials an ideal octave down of the chord has (with the strings' inharmonicity), Hz.
inline std::vector<double> idealPartials (const Chord& c, double maxHz)
{
    std::vector<double> f;
    const double b = Pluck {}.inharmonicity;
    for (double f1 : c.notes)
        for (int n = 1; n * f1 / 2.0 < maxHz + 50.0; ++n)
        {
            const double nn = static_cast<double> (n);
            f.push_back (nn * f1 * std::sqrt ((1.0 + b * nn * nn) / (1.0 + b)) / 2.0);
        }
    return f;
}

struct ChordSub
{
    std::vector<double> noteCents, noteDb; ///< each note's sub (f/2): its error and level
    double offGridDb = 0.0;      ///< power away from the ideal partials, re the power on them
    double worstPeakDb = -200.0; ///< strongest peak away from them, re the strongest note's sub
    double worstPeakHz = 0.0;
    double worstPeakVsWeakestDb = -200.0; ///< the same peak, re the weakest note's sub
};

/// Judges a sub over `seconds` from `from`: the notes an octave down (searched within 30 cents),
/// and what is neither a note nor one of its partials (intermodulation), up to `maxHz`.
inline ChordSub judgeChordSub (const Chord& c, const std::vector<float>& sub, double fs,
                               double from = 0.45, double seconds = 1.0, double maxHz = 700.0)
{
    ChordSub r;
    const auto start = static_cast<size_t> (from * fs), n = static_cast<size_t> (seconds * fs);
    double weakest = 1.0e9, strongest = -1.0e9;
    for (double f1 : c.notes)
    {
        const double target = f1 / 2.0;
        double best = -1.0e9, bestHz = target;
        for (double cents = -30.0; cents <= 30.0; cents += 0.5)
        {
            const double hz = target * std::exp2 (cents / 1200.0);
            const double d = dftDb (sub, start, n, hz, fs);
            if (d > best)
            {
                best = d;
                bestHz = hz;
            }
        }
        const double below = dftDb (sub, start, n, bestHz - 0.05, fs);
        const double above = dftDb (sub, start, n, bestHz + 0.05, fs);
        const double den = below - 2.0 * best + above;
        const double shift = den < 0.0 ? 0.5 * (below - above) / den : 0.0;
        r.noteCents.push_back (1200.0 * std::log2 ((bestHz + 0.05 * shift) / target));
        r.noteDb.push_back (best);
        weakest = std::min (weakest, best);
        strongest = std::max (strongest, best);
    }

    const auto ideal = idealPartials (c, maxHz);
    const auto onGrid = [&] (double hz) {
        return std::any_of (ideal.begin(), ideal.end(), [hz] (double f) {
            return std::abs (hz - f) <= std::max (2.0, f * (std::exp2 (10.0 / 1200.0) - 1.0));
        });
    };
    std::vector<double> grid, mag;
    for (double hz = 25.0; hz <= maxHz; hz += 0.25)
    {
        grid.push_back (hz);
        mag.push_back (dftDb (sub, start, n, hz, fs));
    }
    double on = 0.0, off = 0.0;
    for (size_t i = 0; i < grid.size(); ++i)
        (onGrid (grid[i]) ? on : off) += std::pow (10.0, mag[i] / 10.0);
    r.offGridDb = 10.0 * std::log10 (off / on);
    for (size_t i = 1; i + 1 < grid.size(); ++i)
        if (mag[i] > mag[i - 1] && mag[i] >= mag[i + 1] && !onGrid (grid[i]) &&
            mag[i] - strongest > r.worstPeakDb)
        {
            r.worstPeakDb = mag[i] - strongest;
            r.worstPeakVsWeakestDb = mag[i] - weakest;
            r.worstPeakHz = grid[i];
        }
    return r;
}

/// Peak-to-trough of the level (50 ms RMS, 5 ms hop) around its 300 ms running mean, dB.
inline double warbleDb (const std::vector<float>& x, double fs, double from, double to)
{
    std::vector<double> level;
    const auto w = static_cast<size_t> (0.05 * fs), hop = static_cast<size_t> (0.005 * fs);
    for (auto i = static_cast<size_t> (from * fs); i + w < static_cast<size_t> (to * fs); i += hop)
    {
        double acc = 0.0;
        for (size_t k = 0; k < w; ++k)
            acc += static_cast<double> (x[i + k]) * x[i + k];
        level.push_back (10.0 * std::log10 (acc / static_cast<double> (w) + 1.0e-20));
    }
    double lo = 0.0, hi = 0.0;
    const size_t half = 30;
    for (size_t k = half; k + half < level.size(); ++k)
    {
        double mean = 0.0;
        for (size_t j = k - half; j <= k + half; ++j)
            mean += level[j];
        mean /= static_cast<double> (2 * half + 1);
        lo = std::min (lo, level[k] - mean);
        hi = std::max (hi, level[k] - mean);
    }
    return hi - lo;
}

/// Milliseconds by which `out` reaches half its settled amplitude (trailing RMS over `window`
/// seconds) later than `ideal` does, after a pick at `pick` seconds. Settled: the mean power
/// 100-150 ms after the pick.
inline double riseLagMs (const std::vector<float>& out, const std::vector<float>& ideal, double fs,
                         double pick, double window)
{
    const auto w = std::max<size_t> (1, static_cast<size_t> (window * fs));
    const auto rise = [&] (const std::vector<float>& x) {
        const auto a = static_cast<size_t> ((pick + 0.10) * fs);
        const auto b = static_cast<size_t> ((pick + 0.15) * fs);
        double settled = 0.0;
        for (size_t i = a; i < b; ++i)
            settled += static_cast<double> (x[i]) * x[i];
        settled /= static_cast<double> (b - a);
        double acc = 0.0;
        const auto start = static_cast<size_t> (pick * fs);
        for (size_t i = start; i < b; ++i)
        {
            acc += static_cast<double> (x[i]) * x[i];
            if (i >= start + w)
                acc -= static_cast<double> (x[i - w]) * x[i - w];
            if (acc / static_cast<double> (w) >= 0.25 * settled)
                return static_cast<double> (i - start) / fs;
        }
        return 1.0;
    };
    return 1000.0 * (rise (out) - rise (ideal));
}

} // namespace sedef::test::octaver
