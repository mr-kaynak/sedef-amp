// SPDX-License-Identifier: AGPL-3.0-or-later
#include "sedef/dsp/capture/ReferenceStrum.h"

#include "sedef/core/Math.h"
#include "sedef/core/Random.h"

#include <cmath>
#include <map>
#include <mutex>
#include <tuple>
#include <utility>

namespace sedef::dsp::capture
{
namespace
{
std::vector<float> generate (double fs, double seconds, std::uint32_t seed)
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
                // amp e^(-t/tau) sin(w n + phase) for t = n / fs <= 6 tau, by rotating a phasor
                // (one complex multiply per sample instead of exp and sin).
                const double decay = std::exp (-1.0 / (tau * fs));
                const double cr = decay * std::cos (w), ci = decay * std::sin (w);
                double re = amp * std::cos (phase), im = amp * std::sin (phase);
                const auto count = static_cast<size_t> (std::floor (6.0 * tau * fs)) + 1;
                const size_t last = std::min (frames, first + count);
                for (size_t i = first; i < last; ++i)
                {
                    acc[i] += im;
                    const double r = re * cr - im * ci;
                    im = re * ci + im * cr;
                    re = r;
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
} // namespace

std::vector<float> referenceStrum (double fs, double seconds, std::uint32_t seed)
{
    // Captures are measured with it on every load, mostly at 48 kHz: keep what was generated.
    static std::mutex lock;
    static std::map<std::tuple<double, double, std::uint32_t>, std::vector<float>> cache;
    const auto key = std::make_tuple (fs, seconds, seed);
    {
        const std::scoped_lock guard (lock);
        if (const auto it = cache.find (key); it != cache.end())
            return it->second;
    }
    auto strum = generate (fs, seconds, seed);
    const std::scoped_lock guard (lock);
    if (cache.size() >= 8)
        cache.clear();
    cache.emplace (key, strum);
    return strum;
}

} // namespace sedef::dsp::capture
