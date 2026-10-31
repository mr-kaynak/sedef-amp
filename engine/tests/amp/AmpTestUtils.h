// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "TestUtils.h"
#include "sedef/dsp/amp/AmpSection.h"

#include <vector>

namespace sedef::test
{

/// A deterministic stand-in for a guitar DI: an open E-major chord strummed once per second
/// (alternating down/up strokes), each string a sum of decaying partials shaped by the pluck
/// position. Normalised to -18 dBFS RMS; strum peaks land around -6 dBFS.
inline std::vector<float> guitarStrum (double fs, double seconds, std::uint32_t seed = 7)
{
    const auto frames = static_cast<size_t> (seconds * fs);
    std::vector<double> acc (frames, 0.0);
    const double strings[] = { 82.41, 123.47, 164.81, 207.65, 246.94, 329.63 };
    Random rng (seed);
    int stroke = 0;
    for (double t0 = 0.05; t0 < seconds; t0 += 1.0, ++stroke)
    {
        const double strength = 0.8 + 0.4 * rng.unipolar();
        for (int s = 0; s < 6; ++s)
        {
            const int order = stroke % 2 == 0 ? s : 5 - s;
            const double start = t0 + 0.009 * order;
            const double f0 = strings[s];
            const auto first = static_cast<size_t> (start * fs);
            for (int k = 1; k * f0 < 8000.0; ++k)
            {
                const double pluck = std::abs (std::sin (kPiD * k * 0.18));
                const double amp = strength * pluck / std::pow (k, 1.1);
                const double tau = 1.6 / (1.0 + 0.35 * k);
                const double phase = kTwoPiD * rng.unipolar();
                const double w = kTwoPiD * k * f0 / fs;
                for (size_t i = first; i < frames; ++i)
                {
                    const double t = static_cast<double> (i - first) / fs;
                    if (t > 6.0 * tau)
                        break;
                    acc[i] += amp * std::exp (-t / tau) *
                              std::sin (w * static_cast<double> (i - first) + phase);
                }
            }
        }
    }
    double power = 0.0;
    for (double v : acc)
        power += v * v;
    const double scale =
        std::pow (10.0, -18.0 / 20.0) / std::sqrt (power / static_cast<double> (frames));
    std::vector<float> out (frames);
    for (size_t i = 0; i < frames; ++i)
        out[i] = static_cast<float> (acc[i] * scale);
    return out;
}

inline double dbfs (double rmsValue)
{
    return 20.0 * std::log10 (std::max (rmsValue, 1.0e-12));
}

/// Runs mono audio through an amp in fixed-size blocks, in place.
inline void runAmp (dsp::AmpSection& amp, std::vector<float>& audio, int block = 64)
{
    for (size_t start = 0; start < audio.size(); start += static_cast<size_t> (block))
    {
        const int n =
            static_cast<int> (std::min (static_cast<size_t> (block), audio.size() - start));
        float* p = audio.data() + start;
        amp.process (AudioView (&p, 1, n));
    }
}

/// Total harmonic distortion (harmonics 2..15) of a steady sine at hz in the second half of v.
inline double thd (const std::vector<float>& v, double hz, double fs)
{
    const int n = 32768;
    const float* d = v.data() + v.size() - n;
    const double fundamental = toneMagnitude (d, n, hz, fs);
    double harmonics = 0.0;
    for (int k = 2; k <= 15 && k * hz < 0.45 * fs; ++k)
    {
        const double h = toneMagnitude (d, n, k * hz, fs);
        harmonics += h * h;
    }
    return std::sqrt (harmonics) / std::max (fundamental, 1.0e-12);
}

/// Prepares an amp for mono processing with the given settings applied and settled.
inline void prepareAmp (dsp::AmpSection& amp, const dsp::AmpSection::Settings& s,
                        double fs = 48000.0, int block = 512)
{
    amp.setSettings (s);
    amp.prepare ({ fs, block, 1 });
    amp.setSettings (s);
}

} // namespace sedef::test
