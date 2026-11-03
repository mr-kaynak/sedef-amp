// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

// Helpers shared by the Remora, Gyre and Breaker tests: the full sample-rate and block-size matrix
// the pedals must handle, and a few measurements.

#include "PedalTestUtils.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace sedef::test
{

inline constexpr double kDriveRates[] = { 44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0 };
inline constexpr int kDriveBlocks[] = { 1, 17, 64, 512, 4096 };
inline constexpr int kDriveMaxBlock = 4096;

/// Full-scale noise with settings randomised before every block, at every rate in kDriveRates and
/// block size in kDriveBlocks, mono and stereo: the output must stay finite.
template <typename Module, typename RandomSettings>
void checkFiniteEverywhere (RandomSettings&& randomSettings)
{
    for (double fs : kDriveRates)
        for (int block : kDriveBlocks)
            for (int channels : { 1, 2 })
            {
                Module m;
                m.prepare ({ fs, kDriveMaxBlock, channels });
                Buffer b (channels, static_cast<int> (0.1 * fs));
                for (int c = 0; c < channels; ++c)
                    fillNoise (b[c], 1.0f, static_cast<std::uint32_t> (5 + c));
                Random r (static_cast<std::uint32_t> (block * 13 + channels));
                m.process (b.view().subView (0, 0)); // empty blocks are legal
                processInBlocks (
                    b, block, [&] (AudioView v) { m.process (v); },
                    [&] (int) { m.setSettings (randomSettings (r)); });
                for (int c = 0; c < channels; ++c)
                    REQUIRE (allFinite (b[c]));
            }
}

/// Renders a guitar-like tone through a fresh module in blocks of `block` frames.
template <typename Module>
std::vector<float> renderInBlocks (const typename Module::Settings& s, double fs, int block,
                                   int frames)
{
    Module m;
    m.prepare ({ fs, kDriveMaxBlock, 1 });
    m.setSettings (s);
    Buffer b (1, frames);
    fillGuitarTone (b[0], fs);
    processInBlocks (
        b, block, [&] (AudioView v) { m.process (v); }, [&] (int) { m.setSettings (s); });
    return b[0];
}

/// Largest difference between renders at every block size in kDriveBlocks and one 4096-frame
/// block at a time, relative to the output's peak. Static settings: the result must not depend on
/// how the host slices the audio.
template <typename Module>
double blockSizeDeviation (const typename Module::Settings& s, double fs)
{
    const int frames = static_cast<int> (0.2 * fs);
    const auto reference = renderInBlocks<Module> (s, fs, kDriveMaxBlock, frames);
    double worst = 0.0;
    for (int block : kDriveBlocks)
    {
        const auto out = renderInBlocks<Module> (s, fs, block, frames);
        for (size_t i = 0; i < out.size(); ++i)
            worst = std::max (worst, static_cast<double> (std::abs (out[i] - reference[i])));
    }
    return worst / std::max (static_cast<double> (peak (reference)), 1.0e-9);
}

/// Harmonic magnitudes 1..5 of a steady tone (index 0 unused), relative to the fundamental for
/// k >= 2; h[1] is the fundamental's absolute magnitude.
struct Harmonics
{
    double h[6] {};
    double evenOverOdd() const { return std::hypot (h[2], h[4]) / std::hypot (h[3], h[5]); }
};

inline Harmonics harmonicsOf (const float* d, int n, double f0, double fs)
{
    Harmonics r;
    r.h[1] = toneMagnitude (d, n, f0, fs);
    for (int k = 2; k <= 5; ++k)
        r.h[k] = toneMagnitude (d, n, k * f0, fs) / std::max (r.h[1], 1.0e-12);
    return r;
}

/// RMS envelope in windows of `window` frames.
inline std::vector<float> envelope (const std::vector<float>& v, int window)
{
    std::vector<float> env;
    for (size_t s = 0; s + static_cast<size_t> (window) <= v.size();
         s += static_cast<size_t> (window))
        env.push_back (rms (v.data() + s, window));
    return env;
}

/// Seconds until the RMS envelope last exceeds its peak minus dropDb (20 ms windows).
inline double decaySeconds (const std::vector<float>& v, double fs, double dropDb)
{
    const int window = static_cast<int> (0.02 * fs);
    const auto env = envelope (v, window);
    const float top = *std::max_element (env.begin(), env.end());
    const auto threshold = static_cast<float> (top * std::pow (10.0, -dropDb / 20.0));
    double t = 0.0;
    for (size_t k = 0; k < env.size(); ++k)
        if (env[k] > threshold)
            t = static_cast<double> (k + 1) * window / fs;
    return t;
}

/// A plucked-string-like test note: a sine with an exponential decay.
inline void fillDecayingTone (std::vector<float>& v, double hz, double fs, float amp, double tau)
{
    for (size_t i = 0; i < v.size(); ++i)
    {
        const double t = static_cast<double> (i) / fs;
        v[i] = amp * static_cast<float> (std::exp (-t / tau) * std::sin (kTwoPiD * hz * t));
    }
}

} // namespace sedef::test
