// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "TestUtils.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cmath>
#include <vector>

namespace sedef::test
{

inline constexpr double kSampleRates[] = {44100.0, 48000.0, 96000.0};
inline constexpr int kBlockSizes[] = {1, 17, 64, 512};
inline constexpr int kMaxBlock = 512;

/// Runs `process` over a buffer in blocks of `block` frames; `perBlock(startFrame)` is called
/// before each block (for automation).
template <typename Process, typename PerBlock>
void processInBlocks (Buffer& b, int block, Process&& process, PerBlock&& perBlock)
{
    for (int start = 0; start < b.frames(); start += block)
    {
        const int n = std::min (block, b.frames() - start);
        perBlock (start);
        process (b.view().subView (start, n));
    }
}

template <typename Process>
void processInBlocks (Buffer& b, int block, Process&& process)
{
    processInBlocks (b, block, process, [] (int) {});
}

/// Calls process on consecutive sub-views of at most `block` frames.
template <typename Process>
void processChunked (const AudioView& v, Process&& process, int block = 512)
{
    for (int start = 0; start < v.numFrames(); start += block)
        process (v.subView (start, std::min (block, v.numFrames() - start)));
}

/// Guitar-like steady test tone: eight harmonics of 147 Hz with 1/k amplitudes, peaking at about
/// -10 dBFS. Used as the "typical single-coil level" reference for level calibration.
inline void fillGuitarTone (std::vector<float>& v, double fs, float peakAmp = 0.3f)
{
    constexpr double f0 = 147.0;
    float maxAbs = 0.0f;
    for (size_t i = 0; i < v.size(); ++i)
    {
        double s = 0.0;
        for (int k = 1; k <= 8; ++k)
            s += std::sin (kTwoPiD * f0 * k * static_cast<double> (i) / fs) / k;
        v[i] = static_cast<float> (s);
        maxAbs = std::max (maxAbs, std::abs (v[i]));
    }
    for (auto& s : v)
        s *= peakAmp / maxAbs;
}

/// Largest absolute second difference: a crude high-pass that makes clicks stand out.
inline float maxSecondDifference (const std::vector<float>& v, size_t start = 0)
{
    float m = 0.0f;
    for (size_t i = std::max<size_t> (start, 2); i < v.size(); ++i)
        m = std::max (m, std::abs (v[i] - 2.0f * v[i - 1] + v[i - 2]));
    return m;
}

inline float maxFirstDifference (const std::vector<float>& v, size_t start = 0)
{
    float m = 0.0f;
    for (size_t i = std::max<size_t> (start, 1); i < v.size(); ++i)
        m = std::max (m, std::abs (v[i] - v[i - 1]));
    return m;
}

/// Total harmonic distortion (amplitude ratio) of a steady tone, harmonics 2..10.
inline double thd (const float* d, int n, double f0, double fs)
{
    const double fundamental = toneMagnitude (d, n, f0, fs);
    double harmonics = 0.0;
    for (int k = 2; k <= 10 && k * f0 < 0.45 * fs; ++k)
    {
        const double h = toneMagnitude (d, n, k * f0, fs);
        harmonics += h * h;
    }
    return std::sqrt (harmonics) / std::max (fundamental, 1e-12);
}

/// Strongest component (relative to the fundamental) that is not within `guardHz` of a harmonic
/// of f0: aliasing and other inharmonic products.
inline double worstInharmonicDb (const float* d, int n, double f0, double fs, double guardHz = 60.0)
{
    const double fundamental = toneMagnitude (d, n, f0, fs);
    double worst = 0.0;
    for (double hz = 40.0; hz < std::min (20000.0, 0.45 * fs); hz += 5.0)
    {
        const double k = std::round (hz / f0);
        if (std::abs (hz - k * f0) < guardHz)
            continue;
        worst = std::max (worst, toneMagnitude (d, n, hz, fs));
    }
    return 20.0 * std::log10 (std::max (worst, 1e-12) / fundamental);
}

/// Positive-going zero-crossing times (fractional samples, linear interpolation).
inline std::vector<double> risingZeroCrossings (const std::vector<float>& v, size_t start = 0)
{
    std::vector<double> t;
    for (size_t i = std::max<size_t> (start, 1); i < v.size(); ++i)
        if (v[i - 1] < 0.0f && v[i] >= 0.0f)
            t.push_back (static_cast<double> (i - 1) + v[i - 1] / (v[i - 1] - v[i]));
    return t;
}

/// Energy above `hz` (RMS after a 4th-order high-pass), for spectral-balance checks.
inline float rmsAbove (const std::vector<float>& v, double hz, double fs, size_t start)
{
    std::vector<float> x (v.begin() + static_cast<std::ptrdiff_t> (start), v.end());
    for (int pass = 0; pass < 2; ++pass)
    {
        // RBJ high-pass, Q = 0.7071, evaluated inline to keep the helper self-contained.
        const double cw = std::cos (kTwoPiD * hz / fs), sw = std::sin (kTwoPiD * hz / fs);
        const double alpha = sw / (2.0 * 0.7071067811865476);
        const double a0 = 1.0 + alpha;
        const double b0 = (1.0 + cw) * 0.5 / a0, b1 = -(1.0 + cw) / a0, b2 = b0;
        const double a1 = -2.0 * cw / a0, a2 = (1.0 - alpha) / a0;
        double s1 = 0.0, s2 = 0.0;
        for (auto& s : x)
        {
            const double in = s;
            const double out = b0 * in + s1;
            s1 = b1 * in - a1 * out + s2;
            s2 = b2 * in - a2 * out;
            s = static_cast<float> (out);
        }
    }
    return rms (x);
}

inline double db (double g)
{
    return 20.0 * std::log10 (std::max (g, 1e-12));
}

/// Wall-clock nanoseconds per sample of `process` over `seconds` of mono noise at 48 kHz.
template <typename Process>
double nsPerSample (Process&& process, double seconds = 10.0, int block = 64)
{
    const int frames = static_cast<int> (48000.0 * seconds);
    Buffer b (1, frames);
    fillNoise (b[0], 0.3f, 7);
    const auto t0 = std::chrono::steady_clock::now();
    processInBlocks (b, block, process);
    const auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::nano> (t1 - t0).count() / frames;
}

/// Feeds full-scale noise through a module at every test sample rate, block size and channel
/// count while `randomSettings(rng)` changes the settings before every block. Output must stay
/// finite. `process(module, view)` adapts the module's process call.
template <typename Module, typename RandomSettings, typename Process>
void checkFiniteWithRandomSettings (RandomSettings&& randomSettings, Process&& process)
{
    for (double fs : kSampleRates)
        for (int block : kBlockSizes)
            for (int channels : {1, 2})
            {
                Module m;
                m.prepare ({fs, kMaxBlock, channels});
                Buffer b (channels, static_cast<int> (0.25 * fs));
                for (int c = 0; c < channels; ++c)
                    fillNoise (b[c], 1.0f, static_cast<std::uint32_t> (11 + c));
                Random r (static_cast<std::uint32_t> (block * 31 + channels));
                process (m, b.view().subView (0, 0)); // empty blocks are legal
                processInBlocks (
                    b, block, [&] (AudioView v) { process (m, v); },
                    [&] (int) { m.setSettings (randomSettings (r)); });
                for (int c = 0; c < channels; ++c)
                    REQUIRE (allFinite (b[c]));
            }
}

/// Digital silence in (from reset, with the given settings) must give digital silence out.
template <typename Module, typename Settings, typename Process>
float silenceOutputPeak (const Settings& settings, Process&& process, int channels = 2)
{
    Module m;
    m.prepare ({48000.0, kMaxBlock, channels});
    m.setSettings (settings);
    Buffer b (channels, 48000);
    processInBlocks (b, 64, [&] (AudioView v) { process (m, v); });
    float p = 0.0f;
    for (int c = 0; c < channels; ++c)
        p = std::max (p, peak (b[c]));
    return p;
}

struct SweepResult
{
    float sweep = 0.0f;     ///< max second difference while automating
    float reference = 0.0f; ///< max second difference over static settings along the sweep
    bool finite = true;
};

/// Automates settingsAt(t), t: 0 -> 1 over one second while a steady sine plays, and compares
/// its click indicator (max second difference) with static runs at nine points of the sweep.
template <typename Module, typename SettingsAt, typename Process>
SweepResult measureSweep (SettingsAt&& settingsAt, Process&& process, double fs = 48000.0,
                          double toneHz = 110.0, float amp = 0.25f)
{
    const int settle = static_cast<int> (0.25 * fs);
    const int sweep = static_cast<int> (1.0 * fs);
    constexpr int block = 64;
    SweepResult r;

    for (int k = 0; k <= 8; ++k)
    {
        const float t = static_cast<float> (k) / 8.0f;
        Module m;
        m.prepare ({fs, block, 1});
        m.setSettings (settingsAt (t));
        Buffer b (1, 2 * settle);
        fillSine (b[0], toneHz, fs, amp);
        processInBlocks (
            b, block, [&] (AudioView v) { process (m, v); },
            [&] (int) { m.setSettings (settingsAt (t)); });
        r.reference =
            std::max (r.reference, maxSecondDifference (b[0], static_cast<size_t> (settle)));
    }

    Module m;
    m.prepare ({fs, block, 1});
    m.setSettings (settingsAt (0.0f));
    Buffer b (1, 2 * settle + sweep);
    fillSine (b[0], toneHz, fs, amp);
    processInBlocks (
        b, block, [&] (AudioView v) { process (m, v); },
        [&] (int start)
        {
            const float t = std::clamp (static_cast<float> (start - settle) / sweep, 0.0f, 1.0f);
            m.setSettings (settingsAt (t));
        });
    r.sweep = maxSecondDifference (b[0], static_cast<size_t> (settle));
    r.finite = allFinite (b[0]);
    return r;
}

} // namespace sedef::test
