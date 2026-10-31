// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "sedef/analysis/PitchTracker.h"
#include "sedef/core/Math.h"
#include "sedef/core/Random.h"

#include <cmath>
#include <cstdint>
#include <vector>

namespace sedef::test
{

inline double centsError (double hz, double refHz)
{
    return 1200.0 * std::log2 (hz / refHz);
}

/// Additive model of a plucked, slightly stiff string; just enough physics for the tuner tests.
/// - Partial n sits at n f1 sqrt ((1 + B n^2) / (1 + B)): the fundamental partial is exactly f1
///   (the "true pitch") and the upper partials are stretched sharp by the stiffness B.
/// - Amplitudes sin (n pi p) / n for a pluck at fraction p of the length (velocity pickup).
/// - Exponential decay, faster for higher partials.
/// - Attack glide: every partial starts `glideCents` sharp, decaying with `glideTau` (the tension
///   of a large displacement), plus an optional slow drift during the sustain.
/// - A short pick-noise burst. Background noise is added separately (addNoise).
struct Pluck
{
    double f1 = 110.0;
    double inharmonicity = 1.0e-4;
    double pluckPosition = 0.18;
    double decaySeconds = 2.0; ///< amplitude time constant of the fundamental
    double glideCents = 8.0;
    double glideTau = 0.05; ///< the glide is gone (< 0.5 cent) about 150 ms after the pick
    double driftCentsPerSecond = 0.0;
    float peak = 0.3f;
    float pickNoise = 0.05f;
    double maxPartialHz = 7000.0;
    double fundamentalGain = 1.0; ///< e.g. a bridge pickup that barely hears the fundamental
    std::uint32_t seed = 1;

    /// Instantaneous frequency of the fundamental partial t seconds after the pick.
    double hzAt (double t) const
    {
        return f1 * std::exp2 ((glideCents * std::exp (-t / glideTau) + driftCentsPerSecond * t) /
                               1200.0);
    }
};

/// Adds the pluck to `out` from `start` (in samples) to the end of the buffer.
inline void addPluck (std::vector<float>& out, double fs, size_t start, const Pluck& p)
{
    struct Partial
    {
        double ratio, amp, decayPerSample, phase;
    };
    Random rng (p.seed);
    std::vector<Partial> partials;
    const double limit = std::min (0.45 * fs, p.maxPartialHz);
    const double b = p.inharmonicity;
    double sumAmp = 0.0;
    for (int n = 1;; ++n)
    {
        const double ratio = n * std::sqrt ((1.0 + b * n * n) / (1.0 + b));
        if (ratio * p.f1 > limit)
            break;
        const double amp =
            std::sin (n * kPiD * p.pluckPosition) / n * (n == 1 ? p.fundamentalGain : 1.0);
        const double tau =
            p.decaySeconds / (1.0 + 0.1 * (n - 1) + std::pow (ratio * p.f1 / 2000.0, 2.0));
        partials.push_back ({ ratio, amp, std::exp (-1.0 / (tau * fs)), kTwoPiD * rng.unipolar() });
        sumAmp += std::abs (amp);
    }
    const double gain = p.peak / sumAmp;

    std::vector<double> env (partials.size(), 1.0);
    for (size_t i = start; i < out.size(); ++i)
    {
        const double t = static_cast<double> (i - start) / fs;
        const double inc = kTwoPiD * p.hzAt (t) / fs; // fundamental's phase step
        const double attack = t < 5.0e-4 ? 0.5 - 0.5 * std::cos (kPiD * t / 5.0e-4) : 1.0;
        double acc = 0.0;
        for (size_t k = 0; k < partials.size(); ++k)
        {
            auto& q = partials[k];
            acc += q.amp * env[k] * std::sin (q.phase);
            q.phase += inc * q.ratio;
            env[k] *= q.decayPerSample;
        }
        out[i] += static_cast<float> (gain * attack * acc);
        if (t < 0.01)
            out[i] += p.pickNoise * static_cast<float> (std::exp (-t / 0.0015)) * rng.bipolar();
    }
}

/// White noise with the given RMS level in dBFS.
inline void addNoise (std::vector<float>& out, float rmsDb, std::uint32_t seed = 7)
{
    Random rng (seed);
    const float amp = dbToGain (rmsDb) * std::sqrt (3.0f); // uniform: rms = amp / sqrt (3)
    for (auto& s : out)
        s += amp * rng.bipolar();
}

/// Steady tone with harmonics 1..maxHarmonics at 1/h amplitude (below 0.4 fs), random phases.
inline std::vector<float> harmonicTone (double hz, double fs, size_t frames, int maxHarmonics = 8,
                                        float amp = 0.3f, std::uint32_t seed = 3)
{
    Random rng (seed);
    std::vector<double> phases;
    for (int h = 1; h <= maxHarmonics && h * hz < 0.4 * fs; ++h)
        phases.push_back (kTwoPiD * rng.unipolar());
    double norm = 0.0;
    for (size_t h = 1; h <= phases.size(); ++h)
        norm += 1.0 / static_cast<double> (h);
    std::vector<float> out (frames);
    for (size_t i = 0; i < frames; ++i)
    {
        double acc = 0.0;
        for (size_t h = 1; h <= phases.size(); ++h)
            acc += std::sin (kTwoPiD * hz * static_cast<double> (h * i) / fs + phases[h - 1]) /
                   static_cast<double> (h);
        out[i] = static_cast<float> (amp * acc / norm);
    }
    return out;
}

/// What a tracker run over one pluck looked like.
struct PluckRun
{
    double firstValid = -1.0;  ///< seconds after the pick
    double firstStable = -1.0; ///< seconds after the pick
    double firstStableCents = 0.0;
    double maxStableCents = 0.0; ///< largest |stableHz error| while stable
    double lastStableCents = 0.0;
    int stableReadings = 0;
    int validAfterSkip = 0;
    int rightNoteAfterSkip = 0; ///< within +-50 cents of the true note
    std::uint32_t onsetsAfterPick = 0;
    float lastStableStd = 0.0f;
};

/// Feeds `signal` in `block`-sample pushes and scores readings after `pickAt` seconds against the
/// sustained pitch `trueHz`.
inline PluckRun runTracker (analysis::PitchTracker& tracker, const std::vector<float>& signal,
                            double fs, double pickAt, double trueHz, int block = 64,
                            double attackSkip = 0.15)
{
    PluckRun run;
    std::uint32_t onsetAtPick = 0;
    bool havePickOnset = false;
    for (size_t start = 0; start < signal.size(); start += static_cast<size_t> (block))
    {
        const int n = static_cast<int> (
            std::min<size_t> (static_cast<size_t> (block), signal.size() - start));
        tracker.push (signal.data() + start, n);
        const double t = static_cast<double> (start + static_cast<size_t> (n)) / fs - pickAt;
        if (t < 0.0)
            continue;
        const auto r = tracker.latest();
        if (!havePickOnset && t > 0.03)
        {
            onsetAtPick = r.onsetId;
            havePickOnset = true;
        }
        if (havePickOnset)
            run.onsetsAfterPick = r.onsetId - onsetAtPick;
        if (r.valid && run.firstValid < 0.0)
            run.firstValid = t;
        if (r.valid && t >= attackSkip)
        {
            ++run.validAfterSkip;
            if (std::abs (centsError (r.frequencyHz, trueHz)) < 50.0)
                ++run.rightNoteAfterSkip;
        }
        if (r.stable)
        {
            const double err = centsError (r.stableHz, trueHz);
            if (run.firstStable < 0.0)
            {
                run.firstStable = t;
                run.firstStableCents = err;
            }
            run.maxStableCents = std::max (run.maxStableCents, std::abs (err));
            run.lastStableCents = err;
            run.lastStableStd = r.stableStdCents;
            ++run.stableReadings;
        }
    }
    return run;
}

} // namespace sedef::test
