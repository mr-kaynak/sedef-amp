// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cmath>
#include <numbers>

namespace sedef
{

inline constexpr float kPi = std::numbers::pi_v<float>;
inline constexpr float kTwoPi = 2.0f * std::numbers::pi_v<float>;
inline constexpr double kPiD = std::numbers::pi;
inline constexpr double kTwoPiD = 2.0 * std::numbers::pi;

/// Number of samples between control-rate updates (filter coefficient recalculation while a
/// parameter is smoothing).
inline constexpr int kControlBlock = 32;

inline float dbToGain (float db) noexcept
{
    return std::pow (10.0f, db * 0.05f);
}

inline float gainToDb (float gain, float floorDb = -120.0f) noexcept
{
    return gain > 0.0f ? std::max (20.0f * std::log10 (gain), floorDb) : floorDb;
}

/// Replaces tiny values with zero so feedback paths cannot decay into denormals.
inline float flushToZero (float x) noexcept
{
    return std::abs (x) < 1.0e-15f ? 0.0f : x;
}

template <typename T>
constexpr T lerp (T a, T b, T t) noexcept
{
    return a + (b - a) * t;
}

/// Maps a 0..1 control onto [lo, hi] exponentially (equal ratios per equal travel).
inline float mapExp (float norm, float lo, float hi) noexcept
{
    return lo * std::pow (hi / lo, std::clamp (norm, 0.0f, 1.0f));
}

/// Maps a 0..1 control onto [lo, hi] linearly.
inline float mapLin (float norm, float lo, float hi) noexcept
{
    return lo + (hi - lo) * std::clamp (norm, 0.0f, 1.0f);
}

/// Audio-taper style curve for a 0..1 knob: slow start, fast finish.
inline float audioTaper (float norm, float curve = 3.0f) noexcept
{
    norm = std::clamp (norm, 0.0f, 1.0f);
    return (std::exp (curve * norm) - 1.0f) / (std::exp (curve) - 1.0f);
}

/// Rational approximation of tanh; max abs error ~2e-3, monotonic, exact saturation at +-1.
inline float fastTanh (float x) noexcept
{
    if (x > 3.0f)
        return 1.0f;
    if (x < -3.0f)
        return -1.0f;
    const float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

/// Clamps a frequency to a safe range for filter design at a given sample rate.
inline double clampFrequency (double hz, double sampleRate) noexcept
{
    return std::clamp (hz, 1.0, sampleRate * 0.49);
}

} // namespace sedef
