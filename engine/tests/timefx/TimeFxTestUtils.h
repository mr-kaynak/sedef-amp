// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "TestUtils.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>

namespace sedef::test
{

inline constexpr std::array<double, 3> kTestRates { 44100.0, 48000.0, 96000.0 };
inline constexpr std::array<int, 4> kTestBlockSizes { 1, 17, 64, 512 };

/// Calls fn (subView, startFrame) over consecutive chunks of at most blockSize frames.
template <typename Fn>
void forEachBlock (const AudioView& io, int blockSize, Fn&& fn)
{
    for (int start = 0; start < io.numFrames(); start += blockSize)
        fn (io.subView (start, std::min (blockSize, io.numFrames() - start)), start);
}

/// Largest |x[n] - 2x[n-1] + x[n-2]|. For a smooth signal this stays near A * (2 pi f / fs)^2;
/// a step discontinuity of size s shows up as roughly s.
inline float maxSecondDifference (const std::vector<float>& v, size_t start = 2)
{
    float worst = 0.0f;
    for (size_t i = std::max<size_t> (start, 2); i < v.size(); ++i)
        worst = std::max (worst, std::abs (v[i] - 2.0f * v[i - 1] + v[i - 2]));
    return worst;
}

/// Index of the largest |v| in [from, to).
inline size_t argMaxAbs (const std::vector<float>& v, size_t from, size_t to)
{
    to = std::min (to, v.size());
    size_t best = from;
    for (size_t i = from; i < to; ++i)
        if (std::abs (v[i]) > std::abs (v[best]))
            best = i;
    return best;
}

inline float peakIn (const std::vector<float>& v, size_t from, size_t to)
{
    return std::abs (v[argMaxAbs (v, from, to)]);
}

/// Runs a stereo 48 kHz benchmark: fn (view) per 64-frame block over `seconds` of noise.
/// Prints and returns nanoseconds per stereo frame.
template <typename Fn>
double benchmarkNsPerFrame (const char* name, double seconds, Fn&& fn)
{
    constexpr double fs = 48000.0;
    constexpr int block = 64;
    Buffer b (2, block);
    Random rng (99);
    const int blocks = static_cast<int> (seconds * fs / block);
    double totalNs = 0.0;
    for (int k = 0; k < blocks; ++k)
    {
        for (int c = 0; c < 2; ++c)
            for (auto& s : b[c])
                s = 0.25f * rng.bipolar();
        const auto t0 = std::chrono::steady_clock::now();
        fn (b.view());
        const auto t1 = std::chrono::steady_clock::now();
        totalNs += std::chrono::duration<double, std::nano> (t1 - t0).count();
    }
    const double ns = totalNs / (static_cast<double> (blocks) * block);
    std::printf ("[bench] %-28s %7.1f ns/frame  (%.3f %% of one core at 48 kHz stereo)\n", name, ns,
                 ns * fs * 1.0e-9 * 100.0);
    return ns;
}

} // namespace sedef::test
