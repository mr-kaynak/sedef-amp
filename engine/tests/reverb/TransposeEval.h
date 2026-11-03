// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

// Objective evaluation of a pitch shifter against an ideal reference (docs/dsp/transpose.md,
// "Quality bars"). Used by the transpose tests and by tools/transpose-bench.
//
// Every test signal is a sum of known partials, so the ideal shifted output is synthesised
// directly: same partials, same envelopes in time, every frequency times the pitch ratio.

#include "sedef/analysis/PitchDetector.h"
#include "sedef/core/Math.h"
#include "sedef/core/Random.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <pffft/pffft.h>
#include <string>
#include <utility>
#include <vector>

namespace sedef::test::transpose
{

// ---------------------------------------------------------------------------------------------
// Signals

/// One plucked string as a sum of partials. Partial n sits at n f sqrt ((1 + B n^2) / (1 + B)),
/// with amplitude sin (n pi p) / n (pluck position p) and its own exponential decay.
struct Note
{
    double start = 0.0; ///< seconds
    double hz = 110.0;
    double level = 0.2;     ///< sum of partial amplitudes (an upper bound on the peak)
    double decay = 2.0;     ///< amplitude time constant of the fundamental, seconds
    double damping = 1.0;   ///< scales how much faster the upper partials die (palm mute > 1)
    double stopAt = 1.0e9;  ///< seconds; the string is damped from here on
    double stopTau = 0.008; ///< seconds
    double pluckPosition = 0.18;
    double inharmonicity = 1.0e-4;
    double maxPartialHz = 8000.0;
    double attackSeconds = 5.0e-4;
    /// Pitch bend: (seconds after start, cents) breakpoints, linear in between.
    std::vector<std::pair<double, double>> bend;
    std::uint32_t seed = 1;

    double bendCentsAt (double t) const
    {
        if (bend.empty())
            return 0.0;
        if (t <= bend.front().first)
            return bend.front().second;
        for (size_t i = 1; i < bend.size(); ++i)
            if (t < bend[i].first)
            {
                const auto [t0, c0] = bend[i - 1];
                const auto [t1, c1] = bend[i];
                return c0 + (c1 - c0) * (t - t0) / (t1 - t0);
            }
        return bend.back().second;
    }

    double partialRatio (int n) const
    {
        const double b = inharmonicity;
        return n * std::sqrt ((1.0 + b * n * n) / (1.0 + b));
    }
};

struct Score
{
    std::string name;
    double seconds = 3.0;
    std::vector<Note> notes;
    std::vector<double> onsets; ///< attack times (s) used by the transient metrics
    double sustainStart = 0.0;  ///< steady span for the spectral and warble metrics (s)
    double sustainEnd = 0.0;
    bool isolatedHits = false; ///< silence before each onset: pre-echo is measurable
};

/// Renders the score with every frequency scaled by `ratio`. Partials of the input are those
/// below min (maxPartialHz, 0.45 fs); scaled partials that would land above 0.47 fs are dropped,
/// as an ideal (alias-free) shifter would.
inline std::vector<float> render (const Score& score, double fs, double ratio = 1.0)
{
    std::vector<double> acc (static_cast<size_t> (score.seconds * fs), 0.0);
    for (const auto& note : score.notes)
    {
        struct Partial
        {
            double ratio, amp, decayPerSample, phase;
        };
        Random rng (note.seed * 2654435761u + 17u);
        std::vector<Partial> partials;
        const double inputLimit = std::min (note.maxPartialHz, 0.45 * fs);
        double maxBend = 1.0;
        for (const auto& b : note.bend)
            maxBend = std::max (maxBend, std::exp2 (b.second / 1200.0));
        double sumAmp = 0.0;
        for (int n = 1;; ++n)
        {
            const double r = note.partialRatio (n);
            if (r * note.hz * maxBend > inputLimit)
                break;
            const double amp = std::sin (n * kPiD * note.pluckPosition) / n;
            sumAmp += std::abs (amp);
            const double hz = r * note.hz;
            const double tau =
                note.decay / (1.0 + 0.1 * (n - 1) + note.damping * std::pow (hz / 2000.0, 2.0) +
                              (note.damping - 1.0) * 0.15 * (n - 1));
            const double phase = kTwoPiD * rng.unipolar();
            if (r * note.hz * maxBend * ratio < 0.47 * fs)
                partials.push_back ({ r, amp, std::exp (-1.0 / (tau * fs)), phase });
        }
        if (sumAmp <= 0.0)
            continue;
        const double gain = note.level / sumAmp;

        const auto first = static_cast<size_t> (std::max (0.0, note.start * fs));
        const double stopDecay = std::exp (-1.0 / (note.stopTau * fs));
        std::vector<double> env (partials.size(), 1.0);
        double stop = 1.0;
        for (size_t i = first; i < acc.size(); ++i)
        {
            const double t = static_cast<double> (i) / fs - note.start;
            const double attack =
                t < note.attackSeconds ? 0.5 - 0.5 * std::cos (kPiD * t / note.attackSeconds) : 1.0;
            if (t + note.start >= note.stopAt)
                stop *= stopDecay;
            if (stop < 1.0e-6)
                break;
            const double inc =
                kTwoPiD * note.hz * ratio * std::exp2 (note.bendCentsAt (t) / 1200.0) / fs;
            double sum = 0.0;
            for (size_t k = 0; k < partials.size(); ++k)
            {
                auto& p = partials[k];
                sum += p.amp * env[k] * std::sin (p.phase);
                p.phase += inc * p.ratio;
                if (p.phase > kTwoPiD)
                    p.phase -= kTwoPiD;
                env[k] *= p.decayPerSample;
            }
            acc[i] += gain * attack * stop * sum;
        }
    }
    std::vector<float> out (acc.size());
    for (size_t i = 0; i < acc.size(); ++i)
        out[i] = static_cast<float> (acc[i]);
    return out;
}

/// Frequency (Hz, unscaled) and amplitude at t0 of every partial of the notes sounding at t0
/// (no bend).
struct PartialInfo
{
    double hz;
    double amp;
};

inline std::vector<PartialInfo> sustainedPartials (const Score& score, double fs, double t0)
{
    std::vector<PartialInfo> out;
    for (const auto& note : score.notes)
    {
        if (note.start > t0 || note.stopAt < t0)
            continue;
        double sumAmp = 0.0;
        const double inputLimit = std::min (note.maxPartialHz, 0.45 * fs);
        std::vector<PartialInfo> mine;
        for (int n = 1;; ++n)
        {
            const double hz = note.partialRatio (n) * note.hz;
            if (hz > inputLimit)
                break;
            const double amp = std::sin (n * kPiD * note.pluckPosition) / n;
            sumAmp += std::abs (amp);
            const double tau =
                note.decay / (1.0 + 0.1 * (n - 1) + note.damping * std::pow (hz / 2000.0, 2.0) +
                              (note.damping - 1.0) * 0.15 * (n - 1));
            mine.push_back ({ hz, std::abs (amp) * std::exp (-(t0 - note.start) / tau) });
        }
        for (auto& p : mine)
            out.push_back ({ p.hz, p.amp * note.level / sumAmp });
    }
    return out;
}

namespace scores
{
inline constexpr double kE2 = 82.4069;

/// Open E major strum, low to high, 10 ms between strings, with string stiffness: the partials
/// are not on a common grid, so the chord is not periodic (a periodic chord would let a
/// time-domain shifter splice seamlessly, which a real guitar never does).
inline Score chord()
{
    Score s;
    s.name = "chord";
    s.seconds = 3.0;
    const double hz[6] = { 82.4069, 123.4708, 164.8138, 207.6523, 246.9417, 329.6276 };
    for (int i = 0; i < 6; ++i)
    {
        Note n;
        n.start = 0.1 + 0.010 * i;
        n.hz = hz[i];
        n.inharmonicity = i < 3 ? 1.5e-4 : 0.6e-4;
        n.level = 0.10;
        n.decay = 2.5 - 0.2 * i;
        n.pluckPosition = 0.16 + 0.01 * i;
        n.seed = static_cast<std::uint32_t> (11 + i);
        s.notes.push_back (n);
    }
    s.onsets = { 0.1 };
    s.sustainStart = 0.45;
    s.sustainEnd = 2.45;
    return s;
}

/// A single sustained low E, for the timbre metric: one note has no beating partials, so its
/// band levels depend only on how the shifter treats each frequency.
inline Score lowNote()
{
    Score s;
    s.name = "low-e";
    s.seconds = 2.6;
    Note n;
    n.start = 0.1;
    n.hz = kE2;
    n.inharmonicity = 1.5e-4;
    n.level = 0.3;
    n.decay = 3.0;
    n.seed = 41;
    s.notes.push_back (n);
    s.onsets = { 0.1 };
    s.sustainStart = 0.4;
    s.sustainEnd = 2.4;
    return s;
}

/// Open D major (D3 A3 D4 F#4), for the per-partial warble: its partials sit on a coarser grid
/// than the E chord's, so more of them can be demodulated one by one.
inline Score chordD()
{
    Score s;
    s.name = "chord-d";
    s.seconds = 3.0;
    const double hz[4] = { 146.8324, 220.0, 293.6648, 369.9944 };
    for (int i = 0; i < 4; ++i)
    {
        Note n;
        n.start = 0.1 + 0.010 * i;
        n.hz = hz[i];
        n.inharmonicity = 0.6e-4;
        n.level = 0.12;
        n.decay = 2.2 - 0.2 * i;
        n.pluckPosition = 0.17 + 0.01 * i;
        n.seed = static_cast<std::uint32_t> (31 + i);
        s.notes.push_back (n);
    }
    s.onsets = { 0.1 };
    s.sustainStart = 0.45;
    s.sustainEnd = 2.45;
    return s;
}

/// An E5 power chord, palm-muted: dies within about 40 ms. The three strings start together, so
/// the reference has exactly one attack.
inline void addMutedHit (Score& s, double t, double level, double stopAt, std::uint32_t seed)
{
    const double hz[3] = { 82.4069, 123.4708, 164.8138 };
    for (int i = 0; i < 3; ++i)
    {
        Note n;
        n.start = t;
        n.hz = hz[i];
        n.level = level;
        n.decay = 0.045;
        n.damping = 4.0;
        n.stopAt = stopAt;
        n.stopTau = 0.004;
        n.inharmonicity = 1.5e-4;
        n.pluckPosition = 0.12;
        n.seed = seed * 3u + static_cast<std::uint32_t> (i);
        s.notes.push_back (n);
    }
    s.onsets.push_back (t);
}

/// Isolated palm-muted hits, 400 ms apart, silent in between.
inline Score hits()
{
    Score s;
    s.name = "hits";
    s.seconds = 3.4;
    s.isolatedHits = true;
    for (int k = 0; k < 8; ++k)
        addMutedHit (s, 0.2 + 0.4 * k, 0.25, 0.2 + 0.4 * k + 0.2, static_cast<std::uint32_t> (k));
    return s;
}

/// Sixteenth notes at 150 bpm (100 ms apart), accents on the beat; each pick damps the last.
inline Score riff()
{
    Score s;
    s.name = "riff";
    s.seconds = 3.6;
    for (int k = 0; k < 32; ++k)
    {
        const double t = 0.1 + 0.1 * k;
        addMutedHit (s, t, k % 4 == 0 ? 0.3 : 0.18, t + 0.1, static_cast<std::uint32_t> (100 + k));
    }
    return s;
}

/// An E-minor-pentatonic line (300 ms notes, each pick damping the last), then a whole-step
/// bend up and back on B3.
inline Score line()
{
    Score s;
    s.name = "line";
    const double notes[] = { 164.81, 196.0, 220.0,  246.94, 293.66, 329.63,
                             392.0,  440.0, 493.88, 587.33, 659.26 };
    double t = 0.1;
    for (size_t i = 0; i < std::size (notes); ++i)
    {
        Note n;
        n.start = t;
        n.hz = notes[i];
        n.level = 0.3;
        n.decay = 1.2;
        n.stopAt = t + 0.3;
        n.stopTau = 0.006;
        n.seed = static_cast<std::uint32_t> (200 + i);
        s.notes.push_back (n);
        s.onsets.push_back (t);
        t += 0.3;
    }
    Note b;
    b.start = t;
    b.hz = 246.94;
    b.level = 0.3;
    b.decay = 2.0;
    b.stopAt = t + 1.5;
    b.bend = { { 0.15, 0.0 }, { 0.3, 200.0 }, { 0.8, 200.0 }, { 0.95, 0.0 } };
    b.seed = 300;
    s.notes.push_back (b);
    s.onsets.push_back (t);
    s.seconds = t + 1.7;
    return s;
}
} // namespace scores

// ---------------------------------------------------------------------------------------------
// Signal helpers

inline double dB (double power) // power ratio -> dB
{
    return 10.0 * std::log10 (std::max (power, 1.0e-30));
}

/// Centred moving RMS over `window` samples.
inline std::vector<double> rmsEnvelope (const std::vector<float>& x, int window)
{
    std::vector<double> prefix (x.size() + 1, 0.0);
    for (size_t i = 0; i < x.size(); ++i)
        prefix[i + 1] = prefix[i] + static_cast<double> (x[i]) * x[i];
    std::vector<double> env (x.size());
    const auto half = static_cast<std::ptrdiff_t> (window / 2);
    const auto n = static_cast<std::ptrdiff_t> (x.size());
    for (std::ptrdiff_t i = 0; i < n; ++i)
    {
        const auto a = std::max<std::ptrdiff_t> (0, i - half);
        const auto b = std::min<std::ptrdiff_t> (n, i + half + 1);
        env[static_cast<size_t> (i)] =
            std::sqrt ((prefix[static_cast<size_t> (b)] - prefix[static_cast<size_t> (a)]) /
                       static_cast<double> (b - a));
    }
    return env;
}

/// x delayed back by `delay` samples (fractional, linear interpolation): y[i] = x[i + delay].
inline std::vector<float> advance (const std::vector<float>& x, double delay)
{
    std::vector<float> y (x.size(), 0.0f);
    for (size_t i = 0; i < y.size(); ++i)
    {
        const double p = static_cast<double> (i) + delay;
        const auto k = static_cast<std::ptrdiff_t> (std::floor (p));
        const double f = p - static_cast<double> (k);
        if (k >= 0 && k + 1 < static_cast<std::ptrdiff_t> (x.size()))
            y[i] = static_cast<float> ((1.0 - f) * x[static_cast<size_t> (k)] +
                                       f * x[static_cast<size_t> (k + 1)]);
    }
    return y;
}

/// Delay of `out` relative to `ref` in samples, from the cross-correlation of their 1 ms RMS
/// envelopes, searched over [minDelay, maxDelay] seconds.
inline double envelopeDelay (const std::vector<float>& out, const std::vector<float>& ref,
                             double fs, double minDelay, double maxDelay)
{
    const int step = std::max (1, static_cast<int> (fs / 4000.0)); // 0.25 ms resolution
    const auto eo = rmsEnvelope (out, static_cast<int> (0.001 * fs));
    const auto er = rmsEnvelope (ref, static_cast<int> (0.001 * fs));
    std::vector<double> o, r;
    for (size_t i = 0; i < eo.size(); i += static_cast<size_t> (step))
        o.push_back (eo[i]);
    for (size_t i = 0; i < er.size(); i += static_cast<size_t> (step))
        r.push_back (er[i]);
    const auto lo = static_cast<int> (std::floor (minDelay * fs / step));
    const auto hi = static_cast<int> (std::ceil (maxDelay * fs / step));
    std::vector<double> score;
    int best = lo;
    double bestScore = -1.0;
    for (int d = lo; d <= hi; ++d)
    {
        double s = 0.0;
        for (size_t i = 0; i < r.size(); ++i)
        {
            const auto j = static_cast<std::ptrdiff_t> (i) + d;
            if (j >= 0 && j < static_cast<std::ptrdiff_t> (o.size()))
                s += r[i] * o[static_cast<size_t> (j)];
        }
        score.push_back (s);
        if (s > bestScore)
        {
            bestScore = s;
            best = d;
        }
    }
    double frac = 0.0;
    const auto bi = static_cast<size_t> (best - lo);
    if (bi > 0 && bi + 1 < score.size())
    {
        const double a = score[bi - 1], b = score[bi], c = score[bi + 1];
        const double curv = a - 2.0 * b + c;
        if (curv < 0.0)
            frac = std::clamp (0.5 * (a - c) / curv, -0.5, 0.5);
    }
    return (best + frac) * step;
}

/// Welch power spectrum (Hann, 75 % overlap) of x[start, start + length). Bin k is k fs / nfft.
inline std::vector<double> welch (const std::vector<float>& x, size_t start, size_t length,
                                  int nfft)
{
    PFFFT_Setup* setup = pffft_new_setup (nfft, PFFFT_REAL);
    const size_t bytes = sizeof (float) * static_cast<size_t> (nfft);
    auto* in = static_cast<float*> (pffft_aligned_malloc (bytes));
    auto* out = static_cast<float*> (pffft_aligned_malloc (bytes));
    auto* work = static_cast<float*> (pffft_aligned_malloc (bytes));
    std::vector<double> power (static_cast<size_t> (nfft / 2 + 1), 0.0);
    std::vector<float> win (static_cast<size_t> (nfft));
    double winPower = 0.0;
    for (int i = 0; i < nfft; ++i)
    {
        win[static_cast<size_t> (i)] =
            static_cast<float> (0.5 - 0.5 * std::cos (kTwoPiD * i / nfft));
        winPower +=
            static_cast<double> (win[static_cast<size_t> (i)]) * win[static_cast<size_t> (i)];
    }
    int frames = 0;
    for (size_t pos = start; pos + static_cast<size_t> (nfft) <= start + length &&
                             pos + static_cast<size_t> (nfft) <= x.size();
         pos += static_cast<size_t> (nfft / 4))
    {
        for (int i = 0; i < nfft; ++i)
            in[i] = x[pos + static_cast<size_t> (i)] * win[static_cast<size_t> (i)];
        pffft_transform_ordered (setup, in, out, work, PFFFT_FORWARD);
        power[0] += static_cast<double> (out[0]) * out[0];
        power.back() += static_cast<double> (out[1]) * out[1];
        for (int k = 1; k < nfft / 2; ++k)
            power[static_cast<size_t> (k)] += static_cast<double> (out[2 * k]) * out[2 * k] +
                                              static_cast<double> (out[2 * k + 1]) * out[2 * k + 1];
        ++frames;
    }
    for (auto& p : power)
        p /= std::max (1, frames) * winPower;
    pffft_aligned_free (in);
    pffft_aligned_free (out);
    pffft_aligned_free (work);
    pffft_destroy_setup (setup);
    return power;
}

/// Removes a least-squares cubic from v (in place) and returns the RMS of what is left.
inline double detrendedRms (std::vector<double> v)
{
    const size_t n = v.size();
    if (n < 8)
        return 0.0;
    // Normal equations for a cubic in t in [-1, 1].
    double m[4][5] = {};
    for (size_t i = 0; i < n; ++i)
    {
        const double t = 2.0 * static_cast<double> (i) / static_cast<double> (n - 1) - 1.0;
        const double p[4] = { 1.0, t, t * t, t * t * t };
        for (int r = 0; r < 4; ++r)
        {
            for (int c = 0; c < 4; ++c)
                m[r][c] += p[r] * p[c];
            m[r][4] += p[r] * v[i];
        }
    }
    for (int c = 0; c < 4; ++c)
    {
        int piv = c;
        for (int r = c + 1; r < 4; ++r)
            if (std::abs (m[r][c]) > std::abs (m[piv][c]))
                piv = r;
        for (int k = 0; k < 5; ++k)
            std::swap (m[c][k], m[piv][k]);
        for (int r = 0; r < 4; ++r)
            if (r != c)
            {
                const double f = m[r][c] / m[c][c];
                for (int k = 0; k < 5; ++k)
                    m[r][k] -= f * m[c][k];
            }
    }
    double coef[4];
    for (int r = 0; r < 4; ++r)
        coef[r] = m[r][4] / m[r][r];
    double acc = 0.0;
    for (size_t i = 0; i < n; ++i)
    {
        const double t = 2.0 * static_cast<double> (i) / static_cast<double> (n - 1) - 1.0;
        const double e = v[i] - (coef[0] + coef[1] * t + coef[2] * t * t + coef[3] * t * t * t);
        acc += e * e;
    }
    return std::sqrt (acc / static_cast<double> (n));
}

/// Zero-phase 4th-order Butterworth low-pass (two biquads, run forwards then backwards), double.
inline void zeroPhaseLowpass (std::vector<double>& x, double fs, double hz)
{
    const double qs[2] = { 0.5411961, 1.3065630 };
    for (double q : qs)
    {
        const double w = kTwoPiD * hz / fs;
        const double alpha = std::sin (w) / (2.0 * q);
        const double cw = std::cos (w);
        const double a0 = 1.0 + alpha;
        const double b0 = (1.0 - cw) / 2.0 / a0, b1 = (1.0 - cw) / a0, b2 = b0;
        const double a1 = -2.0 * cw / a0, a2 = (1.0 - alpha) / a0;
        for (int pass = 0; pass < 2; ++pass)
        {
            double s1 = 0.0, s2 = 0.0;
            const auto run = [&] (double& v) {
                const double y = b0 * v + s1;
                s1 = b1 * v - a1 * y + s2;
                s2 = b2 * v - a2 * y;
                v = y;
            };
            if (pass == 0)
                for (auto& v : x)
                    run (v);
            else
                for (auto it = x.rbegin(); it != x.rend(); ++it)
                    run (*it);
        }
    }
}

/// |x| demodulated at hz with a zero-phase low-pass of `bandwidth`, over [start, end).
inline std::vector<double> demodulate (const std::vector<float>& x, double fs, double hz,
                                       double bandwidth, size_t start, size_t end)
{
    // Filter a margin on both sides so the zero-phase filter settles inside the span.
    const auto margin = static_cast<size_t> (fs / bandwidth * 2.0);
    const size_t a = start > margin ? start - margin : 0;
    const size_t b = std::min (x.size(), end + margin);
    std::vector<double> re (b - a), im (b - a);
    const double w = kTwoPiD * hz / fs;
    for (size_t i = a; i < b; ++i)
    {
        re[i - a] = x[i] * std::cos (w * static_cast<double> (i));
        im[i - a] = -x[i] * std::sin (w * static_cast<double> (i));
    }
    zeroPhaseLowpass (re, fs, bandwidth);
    zeroPhaseLowpass (im, fs, bandwidth);
    std::vector<double> mag;
    for (size_t i = start; i < end && i < b; ++i)
        mag.push_back (2.0 * std::hypot (re[i - a], im[i - a]));
    return mag;
}

// ---------------------------------------------------------------------------------------------
// Metrics

struct SustainMetrics
{
    double purityDb = 0.0;    ///< off-partial energy share of the output
    double purityRefDb = 0.0; ///< the same for the reference (the measurement floor)
    double timbreDb = 0.0;    ///< energy-weighted RMS level error of the partials
};

/// Purity and timbre over the score's sustain span. `out` must already be aligned to `ref`.
inline SustainMetrics sustainMetrics (const Score& score, const std::vector<float>& out,
                                      const std::vector<float>& ref, double fs, double ratio)
{
    int nfft = 1;
    while (nfft < static_cast<int> (0.68 * fs))
        nfft <<= 1;
    const auto start = static_cast<size_t> (score.sustainStart * fs);
    const auto length = static_cast<size_t> ((score.sustainEnd - score.sustainStart) * fs);
    const auto po = welch (out, start, length, nfft);
    const auto pr = welch (ref, start, length, nfft);
    const double binHz = fs / nfft;

    // Partial clusters: target frequencies closer than 8 Hz are merged.
    auto partials = sustainedPartials (score, fs, score.sustainStart);
    std::vector<double> targets;
    for (const auto& p : partials)
        if (p.hz * ratio < 0.47 * fs)
            targets.push_back (p.hz * ratio);
    std::sort (targets.begin(), targets.end());
    std::vector<std::pair<double, double>> clusters; // [lo, hi] Hz
    for (double f : targets)
    {
        if (!clusters.empty() && f - 4.0 <= clusters.back().second)
            clusters.back().second = f + 4.0;
        else
            clusters.push_back ({ f - 4.0, f + 4.0 });
    }

    const double bandLo = 40.0, bandHi = std::min (10000.0, 0.45 * fs);
    std::vector<bool> inCluster (po.size(), false);
    for (const auto& [lo, hi] : clusters)
        for (auto k = static_cast<size_t> (std::max (0.0, std::ceil (lo / binHz)));
             k <= static_cast<size_t> (std::floor (hi / binHz)) && k < po.size(); ++k)
            inCluster[k] = true;
    double totO = 0.0, offO = 0.0, totR = 0.0, offR = 0.0;
    for (size_t k = 0; k < po.size(); ++k)
    {
        const double hz = static_cast<double> (k) * binHz;
        if (hz < bandLo || hz > bandHi)
            continue;
        totO += po[k];
        totR += pr[k];
        if (!inCluster[k])
        {
            offO += po[k];
            offR += pr[k];
        }
    }
    SustainMetrics m;
    m.purityDb = dB (offO / std::max (totO, 1.0e-30));
    m.purityRefDb = dB (offR / std::max (totR, 1.0e-30));

    // Timbre: third-octave band levels (63 Hz - 5 kHz), weighted by the reference energy. Bands
    // rather than single partials: near-coincident partials of different strings beat slowly,
    // so their joint level over 2 s depends on the beat phase, not on the shifter.
    double wsum = 0.0, acc = 0.0, maxRef = 0.0;
    std::vector<std::pair<double, double>> bands; // (ref energy, out energy)
    for (int b = -14; b <= 7; ++b)
    {
        const double centre = 1000.0 * std::exp2 (b / 3.0);
        const double lo = centre * std::exp2 (-1.0 / 6.0), hi = centre * std::exp2 (1.0 / 6.0);
        double eo = 0.0, er = 0.0;
        for (auto k = static_cast<size_t> (std::ceil (lo / binHz));
             k <= static_cast<size_t> (std::floor (hi / binHz)) && k < po.size(); ++k)
        {
            eo += po[k];
            er += pr[k];
        }
        bands.push_back ({ er, eo });
        maxRef = std::max (maxRef, er);
    }
    for (const auto& [er, eo] : bands)
    {
        if (er < maxRef * 1.0e-4) // bands 40 dB below the strongest carry no timbre
            continue;
        const double e = dB (eo / er);
        acc += er * e * e;
        wsum += er;
    }
    m.timbreDb = wsum > 0.0 ? std::sqrt (acc / wsum) : 0.0;
    return m;
}

struct WarbleMetrics
{
    double partialDb = 0.0; ///< energy-weighted RMS of the per-partial level ratio, 1..band Hz
    double partialBandHz = 12.0;
    int partials = 0;        ///< isolated partials measured
    double envelopeDb = 0.0; ///< RMS of the measured partials' total-power ratio, 1..band Hz
};

/// Warble over the sustain span of an aperiodic (stiff-string, equal-temperament) chord whose
/// partials lie roughly on a grid of `gridHz` (half its lowest note). `out` must already be
/// aligned to `ref`.
inline WarbleMetrics warbleMetrics (const Score& score, const std::vector<float>& out,
                                    const std::vector<float>& ref, double fs, double ratio,
                                    double gridHz)
{
    WarbleMetrics m;
    const auto start = static_cast<size_t> (score.sustainStart * fs);
    const auto end = static_cast<size_t> (score.sustainEnd * fs);

    // Partials are measured one by one where no other partial lies within 3.5 demodulation
    // bandwidths (-43 dB leakage through the zero-phase 4th-order low-pass). The bandwidth is
    // 12 Hz, narrower when the shift packs the partials closer (grid / 3.5).
    m.partialBandHz = std::min (12.0, gridHz * ratio / 3.5);
    auto partials = sustainedPartials (score, fs, score.sustainStart);
    double maxAmp = 0.0;
    for (const auto& p : partials)
        maxAmp = std::max (maxAmp, p.amp);
    const int decim = std::max (1, static_cast<int> (fs / 1000.0));
    double wsum = 0.0, acc = 0.0;
    std::vector<double> sumOut, sumRef; // total power of the measured partials
    for (const auto& p : partials)
    {
        const double hz = p.hz * ratio;
        if (p.amp < maxAmp * 0.03 || hz > std::min (6000.0, 0.45 * fs)) // -30 dB
            continue;
        bool isolated = true;
        for (const auto& q : partials)
            if (&q != &p && q.amp > maxAmp * 3.0e-3 &&
                std::abs (q.hz * ratio - hz) < 3.5 * m.partialBandHz)
                isolated = false;
        if (!isolated)
            continue;
        const auto zo = demodulate (out, fs, hz, m.partialBandHz, start, end);
        const auto zr = demodulate (ref, fs, hz, m.partialBandHz, start, end);
        const double floor = *std::max_element (zr.begin(), zr.end()) * 1.0e-2;
        std::vector<double> ratioDb;
        const size_t n = std::min (zr.size(), zo.size());
        sumOut.resize (std::max (sumOut.size(), n / static_cast<size_t> (decim) + 1), 0.0);
        sumRef.resize (sumOut.size(), 0.0);
        for (size_t i = 0; i < n; i += static_cast<size_t> (decim))
        {
            if (zr[i] > floor)
                ratioDb.push_back (20.0 * std::log10 (std::max (zo[i], 1.0e-12) / zr[i]));
            sumOut[i / static_cast<size_t> (decim)] += zo[i] * zo[i];
            sumRef[i / static_cast<size_t> (decim)] += zr[i] * zr[i];
        }
        const double w = p.amp * p.amp;
        const double r = detrendedRms (ratioDb);
        acc += w * r * r;
        wsum += w;
        ++m.partials;
    }
    m.partialDb = wsum > 0.0 ? std::sqrt (acc / wsum) : 0.0;

    // Envelope warble: the same partials' total power, output over reference. A broadband
    // short-time RMS would mostly measure where near-coincident partials of different strings
    // happen to be in their beat cycle (0.3-0.6 dB RMS between two ideal outputs that differ only
    // in partial phases), not the shifter.
    std::vector<double> env;
    for (size_t i = 0; i < sumRef.size(); ++i)
        if (sumRef[i] > 0.0)
            env.push_back (dB (sumOut[i] / sumRef[i]));
    m.envelopeDb = detrendedRms (env);
    return m;
}

struct TransientMetrics
{
    std::vector<double> latencyMs; ///< per onset
    double latencyMeanMs = 0.0;
    double latencyRangeMs = 0.0;
    double preEchoDb = -120.0; ///< worst onset (isolated hits only)
    double preEchoRefDb = -120.0;
    double flamDb = 0.0;   ///< worst onset, output minus reference
    double attackDb = 0.0; ///< worst |peak level error|
    double attackMeanDb = 0.0;
    double riseMs = 0.0; ///< worst rise-time excess over the reference
};

struct AttackShape
{
    bool found = false;
    double crossing = 0.0; ///< samples, interpolated
    double peak = 0.0;
    size_t peakIndex = 0;
    double riseSamples = 0.0;
    double preEchoDb = -120.0;
    double flamDb = 0.0;
};

inline AttackShape attackShape (const std::vector<double>& env, size_t from, size_t to, double fs)
{
    AttackShape a;
    to = std::min (to, env.size());
    if (from + 2 >= to)
        return a;
    double peakAll = 0.0;
    for (size_t i = from; i < to; ++i)
        peakAll = std::max (peakAll, env[i]);
    if (peakAll <= 1.0e-9)
        return a;
    // The attack: the first upward crossing of half the window's peak, after the envelope has
    // been below 30 % of it (so a previous note's tail at the window start is not taken).
    bool armed = false;
    size_t i = from;
    for (; i + 1 < to; ++i)
    {
        armed = armed || env[i] < 0.3 * peakAll;
        if (armed && env[i] < 0.5 * peakAll && env[i + 1] >= 0.5 * peakAll)
            break;
    }
    if (i + 1 >= to)
        return a;
    a.found = true;
    a.crossing = static_cast<double> (i) + (0.5 * peakAll - env[i]) / (env[i + 1] - env[i]);
    // Attack peak: the maximum within 5 ms of the crossing.
    const auto peakEnd = std::min (to, i + static_cast<size_t> (0.005 * fs));
    size_t pk = i + 1;
    for (size_t k = i + 1; k < peakEnd; ++k)
        if (env[k] > env[pk])
            pk = k;
    a.peak = env[pk];
    a.peakIndex = pk;
    // A later rise of the envelope (dB) above its running minimum, within 40 ms: a re-attack.
    // Levels more than 30 dB below the attack peak count as the floor.
    double runMin = 1.0e9, flam = 0.0;
    const auto flamEnd = std::min (env.size(), pk + static_cast<size_t> (0.040 * fs));
    const double floorDb = 20.0 * std::log10 (a.peak) - 30.0;
    for (size_t k = pk; k < flamEnd; ++k)
    {
        const double v = std::max (floorDb, 20.0 * std::log10 (std::max (env[k], 1.0e-12)));
        runMin = std::min (runMin, v);
        flam = std::max (flam, v - runMin);
    }
    a.flamDb = flam;
    // 10-90 % rise of the attack peak.
    size_t lo = i;
    while (lo > from && env[lo] >= 0.1 * a.peak)
        --lo;
    size_t hi = lo;
    while (hi < pk && env[hi] < 0.9 * a.peak)
        ++hi;
    a.riseSamples = static_cast<double> (hi) - static_cast<double> (lo);
    // Pre-echo: mean power 3-20 ms before the crossing, relative to the attack peak power.
    const auto c = i;
    const auto p0 = static_cast<size_t> (0.020 * fs), p1 = static_cast<size_t> (0.003 * fs);
    if (c > p0)
    {
        double acc = 0.0;
        for (size_t k = c - p0; k < c - p1; ++k)
            acc += env[k] * env[k];
        a.preEchoDb = dB (acc / static_cast<double> (p0 - p1) / (a.peak * a.peak));
    }
    return a;
}

/// x high-passed at hz (two 2nd-order Butterworth sections, causal).
inline std::vector<float> highpass (const std::vector<float>& x, double fs, double hz)
{
    std::vector<double> y (x.begin(), x.end());
    const double w = kTwoPiD * hz / fs, alpha = std::sin (w) / (2.0 * 0.7071067811865476);
    const double cw = std::cos (w), a0 = 1.0 + alpha;
    const double b0 = (1.0 + cw) / 2.0 / a0, b1 = -(1.0 + cw) / a0, b2 = b0;
    const double a1 = -2.0 * cw / a0, a2 = (1.0 - alpha) / a0;
    for (int pass = 0; pass < 2; ++pass)
    {
        double s1 = 0.0, s2 = 0.0;
        for (auto& v : y)
        {
            const double o = b0 * v + s1;
            s1 = b1 * v - a1 * o + s2;
            s2 = b2 * v - a2 * o;
            v = o;
        }
    }
    std::vector<float> out (y.size());
    for (size_t i = 0; i < y.size(); ++i)
        out[i] = static_cast<float> (y[i]);
    return out;
}

/// Per-onset attack analysis. `out` is the raw output (not aligned); onsets are searched in
/// [onset + expectedDelay - 10 ms, onset + expectedDelay + 70 ms]. Latency, level, rise and
/// pre-echo use the broadband 1 ms RMS envelope. Flam uses the envelope above 2 kHz: there the
/// palm-muted strings die within a few ms, so a second rise is a second attack, not the beating
/// of the chord's partials.
inline TransientMetrics transientMetrics (const Score& score, const std::vector<float>& out,
                                          const std::vector<float>& ref, double fs,
                                          double expectedDelaySamples)
{
    TransientMetrics m;
    const int win = static_cast<int> (0.001 * fs);
    const auto eo = rmsEnvelope (out, win);
    const auto er = rmsEnvelope (ref, win);
    const auto ho = rmsEnvelope (highpass (out, fs, 2000.0), win);
    const auto hr = rmsEnvelope (highpass (ref, fs, 2000.0), win);
    double minL = 1.0e9, maxL = -1.0e9, sumL = 0.0, sumA = 0.0;
    int count = 0;
    for (double onset : score.onsets)
    {
        const auto t = static_cast<size_t> (onset * fs);
        const auto r0 =
            t > static_cast<size_t> (0.002 * fs) ? t - static_cast<size_t> (0.002 * fs) : 0;
        const auto r1 = t + static_cast<size_t> (0.020 * fs);
        const auto ar = attackShape (er, r0, r1, fs);
        const auto fr = attackShape (hr, r0, r1, fs);
        const double s0 = static_cast<double> (t) + expectedDelaySamples - 0.010 * fs;
        const auto o0 = static_cast<size_t> (std::max (0.0, s0));
        const auto o1 = o0 + static_cast<size_t> (0.080 * fs);
        const auto ao = attackShape (eo, o0, o1, fs);
        const auto fo = attackShape (ho, o0, o1, fs);
        if (!ar.found || !ao.found)
            continue;
        const double lat = (ao.crossing - ar.crossing) / fs * 1000.0;
        m.latencyMs.push_back (lat);
        minL = std::min (minL, lat);
        maxL = std::max (maxL, lat);
        sumL += lat;
        const double att = 20.0 * std::log10 (ao.peak / ar.peak);
        sumA += att;
        m.attackDb = std::max (m.attackDb, std::abs (att));
        if (fr.found && fo.found)
            m.flamDb = std::max (m.flamDb, fo.flamDb - fr.flamDb);
        m.riseMs = std::max (m.riseMs, (ao.riseSamples - ar.riseSamples) / fs * 1000.0);
        if (score.isolatedHits)
        {
            m.preEchoDb = std::max (m.preEchoDb, ao.preEchoDb);
            m.preEchoRefDb = std::max (m.preEchoRefDb, ar.preEchoDb);
        }
        ++count;
    }
    if (count > 0)
    {
        m.latencyMeanMs = sumL / count;
        m.latencyRangeMs = maxL - minL;
        m.attackMeanDb = sumA / count;
    }
    return m;
}

struct PitchMetrics
{
    double medianCents = 0.0;
    double p95Cents = 0.0;
    int frames = 0;
};

/// MPM pitch of output and reference, frame by frame, outside the first 30 ms of each note.
/// `out` must already be aligned to `ref`.
inline PitchMetrics pitchMetrics (const Score& score, const std::vector<float>& out,
                                  const std::vector<float>& ref, double fs)
{
    analysis::PitchDetector det;
    det.prepare (fs, 70.0f, 1400.0f);
    const int frame = static_cast<int> (std::lround (2048.0 * fs / 48000.0));
    const auto hop = static_cast<size_t> (frame / 4);
    std::vector<double> errs;
    for (size_t pos = 0; pos + static_cast<size_t> (frame) <= ref.size(); pos += hop)
    {
        const double t0 = static_cast<double> (pos) / fs;
        const double t1 = t0 + frame / fs;
        bool clean = false;
        for (size_t k = 0; k < score.onsets.size(); ++k)
        {
            const double next = k + 1 < score.onsets.size() ? score.onsets[k + 1] : score.seconds;
            if (t0 >= score.onsets[k] + 0.03 && t1 <= next - 0.01)
                clean = true;
        }
        if (!clean)
            continue;
        const auto r = det.detect (ref.data() + pos, frame);
        if (!r.valid || r.clarity < 0.9f)
            continue;
        const auto o = det.detect (out.data() + pos, frame);
        const double e =
            o.valid ? std::abs (1200.0 * std::log2 (o.frequencyHz / r.frequencyHz)) : 1200.0;
        errs.push_back (e);
    }
    PitchMetrics m;
    m.frames = static_cast<int> (errs.size());
    if (errs.empty())
        return m;
    std::sort (errs.begin(), errs.end());
    m.medianCents = errs[errs.size() / 2];
    m.p95Cents = errs[std::min (errs.size() - 1,
                                static_cast<size_t> (0.95 * static_cast<double> (errs.size())))];
    return m;
}

// ---------------------------------------------------------------------------------------------
// Running a shifter

/// What the harness needs from a shifter: mono, in place.
struct Shifter
{
    std::function<void (double fs, int maxBlock)> prepare;
    std::function<void (int semitones)> setSemitones;
    std::function<void (float* x, int n)> process;
    std::function<int()> latency; ///< what it reports to the host, in samples
};

inline std::vector<float> runShifter (Shifter& s, const std::vector<float>& in, double fs,
                                      int semitones, int block)
{
    s.setSemitones (semitones);
    s.prepare (fs, block);
    s.setSemitones (semitones);
    std::vector<float> x = in;
    for (size_t pos = 0; pos < x.size(); pos += static_cast<size_t> (block))
        s.process (x.data() + pos,
                   static_cast<int> (std::min (static_cast<size_t> (block), x.size() - pos)));
    return x;
}

struct Evaluation
{
    int semitones = 0;
    double reportedMs = 0.0;
    double delayMs = 0.0; ///< alignment used for the sustain metrics (chord)
    SustainMetrics chord;
    WarbleMetrics warble;
    TransientMetrics hits, riff;
    PitchMetrics pitch;
    double aliasDb = -200.0;
    double cpuPercent = 0.0;
};

inline double aliasing (Shifter& s, double fs, int semitones)
{
    std::vector<float> in (static_cast<size_t> (fs));
    for (size_t i = 0; i < in.size(); ++i)
    {
        const double t = static_cast<double> (i) / fs;
        in[i] = static_cast<float> (0.25 * std::sin (kTwoPiD * 12000.0 * t) +
                                    0.25 * std::sin (kTwoPiD * 15000.0 * t));
    }
    const auto out = runShifter (s, in, fs, semitones, 64);
    double eo = 0.0, ei = 0.0;
    for (size_t i = static_cast<size_t> (0.3 * fs); i < in.size(); ++i)
    {
        eo += static_cast<double> (out[i]) * out[i];
        ei += static_cast<double> (in[i]) * in[i];
    }
    return dB (eo / ei);
}

inline double cpuPercent (Shifter& s, double fs, int semitones, const std::vector<float>& in)
{
    using Clock = std::chrono::steady_clock;
    double best = 1.0e9;
    for (int rep = 0; rep < 3; ++rep)
    {
        s.setSemitones (semitones);
        s.prepare (fs, 64);
        s.setSemitones (semitones);
        std::vector<float> x = in;
        const auto t0 = Clock::now();
        for (size_t pos = 0; pos + 64 <= x.size(); pos += 64)
            s.process (x.data() + pos, 64);
        const double secs = std::chrono::duration<double> (Clock::now() - t0).count();
        best = std::min (best, secs);
    }
    return 100.0 * best / (static_cast<double> (in.size()) / fs);
}

/// Runs every metric for one shift.
inline Evaluation evaluate (Shifter& s, double fs, int semitones, bool withCpu = true,
                            int block = 64)
{
    Evaluation e;
    e.semitones = semitones;
    const double ratio = std::exp2 (semitones / 12.0);
    s.setSemitones (semitones);
    s.prepare (fs, block);
    s.setSemitones (semitones);
    const double reported = s.latency();
    e.reportedMs = reported / fs * 1000.0;

    {
        const auto score = scores::chord();
        const auto in = render (score, fs);
        const auto ref = render (score, fs, ratio);
        const auto out = runShifter (s, in, fs, semitones, block);
        const double d = envelopeDelay (out, ref, fs, reported / fs - 0.005, reported / fs + 0.060);
        e.delayMs = d / fs * 1000.0;
        const auto aligned = advance (out, d);
        e.chord = sustainMetrics (score, aligned, ref, fs, ratio);
        e.chord.timbreDb = 0.0; // from the single note below
        if (withCpu)
            e.cpuPercent = cpuPercent (s, fs, semitones, in);
    }
    {
        const auto score = scores::lowNote();
        const auto in = render (score, fs);
        const auto ref = render (score, fs, ratio);
        const auto out = runShifter (s, in, fs, semitones, block);
        const double d = envelopeDelay (out, ref, fs, reported / fs - 0.005, reported / fs + 0.060);
        e.chord.timbreDb = sustainMetrics (score, advance (out, d), ref, fs, ratio).timbreDb;
    }
    {
        const auto score = scores::chordD();
        const auto in = render (score, fs);
        const auto ref = render (score, fs, ratio);
        const auto out = runShifter (s, in, fs, semitones, block);
        const double d = envelopeDelay (out, ref, fs, reported / fs - 0.005, reported / fs + 0.060);
        const auto w = warbleMetrics (score, advance (out, d), ref, fs, ratio, 0.5 * 146.8324);
        e.warble = w;
    }
    for (int which = 0; which < 2; ++which)
    {
        const auto score = which == 0 ? scores::hits() : scores::riff();
        const auto in = render (score, fs);
        const auto ref = render (score, fs, ratio);
        const auto out = runShifter (s, in, fs, semitones, block);
        (which == 0 ? e.hits : e.riff) = transientMetrics (score, out, ref, fs, reported);
    }
    {
        const auto score = scores::line();
        const auto in = render (score, fs);
        const auto ref = render (score, fs, ratio);
        const auto out = runShifter (s, in, fs, semitones, block);
        const double d = envelopeDelay (out, ref, fs, reported / fs - 0.005, reported / fs + 0.060);
        e.pitch = pitchMetrics (score, advance (out, d), ref, fs);
    }
    if (semitones > 0)
        e.aliasDb = aliasing (s, fs, semitones);
    return e;
}

// ---------------------------------------------------------------------------------------------
// Bars (docs/dsp/transpose.md)

struct Bars
{
    double latencyMs, latencyRangeMs, reportErrorMs;
    double purityDb, timbreDb, partialWarbleDb, envelopeWarbleDb;
    double preEchoDb, flamDb, attackDb, riseMs;
    double pitchMedianCents, pitchP95Cents;
    double aliasDb, cpuPercent;
};

inline Bars barsFor (int semitones)
{
    const bool octave = std::abs (semitones) == 12;
    return { 10.0,
             0.5,
             0.25,
             octave ? -30.0 : -35.0,
             octave ? 1.5 : 1.0,
             octave ? 0.75 : 0.5,
             octave ? 0.5 : 0.25,
             -30.0,
             1.0,
             octave ? 2.0 : 1.5,
             1.0,
             2.0,
             10.0,
             -60.0,
             2.0 };
}

/// Names of the bars an evaluation misses (empty = clears every bar).
inline std::vector<std::string> missedBars (const Evaluation& e)
{
    const auto b = barsFor (e.semitones);
    std::vector<std::string> missed;
    const auto check = [&] (bool ok, const char* name) {
        if (!ok)
            missed.emplace_back (name);
    };
    const double latMean = 0.5 * (e.hits.latencyMeanMs + e.riff.latencyMeanMs);
    double lo = 1.0e9, hi = -1.0e9;
    for (const auto* t : { &e.hits, &e.riff })
        for (double l : t->latencyMs)
        {
            lo = std::min (lo, l);
            hi = std::max (hi, l);
        }
    check (latMean <= b.latencyMs, "B1 latency");
    check (hi - lo <= b.latencyRangeMs, "B2 latency range");
    check (std::abs (latMean - e.reportedMs) <= b.reportErrorMs, "B3 reported latency");
    check (e.chord.purityDb <= b.purityDb, "B4 purity");
    check (e.chord.timbreDb <= b.timbreDb, "B5 timbre");
    check (e.warble.partialDb <= b.partialWarbleDb, "B6 partial warble");
    check (e.warble.envelopeDb <= b.envelopeWarbleDb, "B7 envelope warble");
    check (e.hits.preEchoDb <= b.preEchoDb, "B8 pre-echo");
    check (std::max (e.hits.flamDb, e.riff.flamDb) <= b.flamDb, "B9 flam");
    check (std::max (e.hits.attackDb, e.riff.attackDb) <= b.attackDb &&
               std::max (e.hits.riseMs, e.riff.riseMs) <= b.riseMs,
           "B10 attack");
    check (e.pitch.medianCents <= b.pitchMedianCents && e.pitch.p95Cents <= b.pitchP95Cents,
           "B11 pitch");
    if (e.semitones > 0 && std::abs (e.semitones) == 12)
        check (e.aliasDb <= b.aliasDb, "B12 aliasing");
    if (e.cpuPercent > 0.0)
        check (e.cpuPercent <= b.cpuPercent, "B13 CPU");
    return missed;
}

} // namespace sedef::test::transpose
