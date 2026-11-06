// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "sedef/core/AudioView.h"
#include "sedef/core/Math.h"

#include <array>
#include <cmath>

namespace sedef
{

/// Prewarped integrator gain of a trapezoidal (TPT) filter: tan(pi f / fs), with f clamped to a
/// safe range. Cheap enough to call once per sample for swept filters.
inline float tptGain (double hz, double sampleRate) noexcept
{
    return static_cast<float> (std::tan (kPiD * clampFrequency (hz, sampleRate) / sampleRate));
}

/// Shapes a 0..1 linear crossfade ramp into a smoothstep (3w^2 - 2w^3): the same end points and
/// the same sum for a two-way fade (s(w) + s(1 - w) = 1), but no kink where the ramp starts and
/// stops, so a switch between two differently filtered signals leaves no corner in the output.
inline float crossfadeCurve (float w) noexcept
{
    return w * w * (3.0f - 2.0f * w);
}

/// Outputs of one state-variable filter step.
struct SvfOutputs
{
    float low = 0.0f;
    float band = 0.0f; ///< peak gain 1/k = Q; multiply by k for a 0 dB peak
    float high = 0.0f;
};

/// Trapezoidal-integrated state-variable filter (topology-preserving transform, after
/// V. Zavalishin, "The Art of VA Filter Design", and A. Simper's linear SVF derivation).
/// Unlike direct-form biquads it stays well-behaved when its cutoff and damping change every
/// sample, which is what swept filters (wahs, envelope filters) need: no zipper noise, no
/// transients from coefficient jumps, stable for any k > 0.
class Svf
{
public:
    /// g = tptGain(cutoff), k = 1/Q.
    void setCoeffs (float g, float k) noexcept
    {
        k_ = k;
        a1_ = 1.0f / (1.0f + g * (g + k));
        a2_ = g * a1_;
        a3_ = g * a2_;
    }

    void reset() noexcept { state_ = {}; }

    SvfOutputs process (int channel, float x) noexcept
    {
        auto& s = state_[static_cast<size_t> (channel)];
        const float v3 = x - s[1];
        const float v1 = a1_ * s[0] + a2_ * v3;
        const float v2 = s[1] + a2_ * s[0] + a3_ * v3;
        s[0] = 2.0f * v1 - s[0];
        s[1] = 2.0f * v2 - s[1];
        return { v2, v1, x - k_ * v1 - v2 };
    }

    float damping() const noexcept { return k_; }

    /// Flushes denormal-range state (long decays after the input stops).
    void snapToZero() noexcept
    {
        for (auto& s : state_)
            for (auto& v : s)
                v = flushToZero (v);
    }

private:
    float k_ = 1.414f, a1_ = 1.0f, a2_ = 0.0f, a3_ = 0.0f;
    std::array<std::array<float, 2>, kMaxChannels> state_ {};
};

/// First-order trapezoidal low-pass, per channel. The all-pass is 2 * low - x: unity magnitude,
/// phase -2 atan(f / fc) through the bilinear map, so its 90 degree point lands exactly on fc.
class TptOnePole
{
public:
    void setGain (float g) noexcept { G_ = g / (1.0f + g); }
    void reset() noexcept { state_ = {}; }

    float lowpass (int channel, float x) noexcept
    {
        float& s = state_[static_cast<size_t> (channel)];
        const float v = G_ * (x - s);
        const float y = v + s;
        s = y + v;
        return y;
    }

    float allpass (int channel, float x) noexcept { return 2.0f * lowpass (channel, x) - x; }

    void snapToZero() noexcept
    {
        for (auto& s : state_)
            s = flushToZero (s);
    }

private:
    float G_ = 0.0f;
    std::array<float, kMaxChannels> state_ {};
};

/// Peak envelope with separate attack and release, for envelope-driven filters. The time
/// constants are the one-pole time constants (63 % of a step).
class PeakEnvelope
{
public:
    void prepare (double sampleRate, double attackSeconds, double releaseSeconds) noexcept
    {
        attack_ = coeff (sampleRate, attackSeconds);
        release_ = coeff (sampleRate, releaseSeconds);
    }

    void reset() noexcept { env_ = 0.0f; }

    float process (float magnitude) noexcept
    {
        const float c = magnitude > env_ ? attack_ : release_;
        env_ = magnitude + c * (env_ - magnitude);
        return env_;
    }

    float value() const noexcept { return env_; }

private:
    static float coeff (double fs, double seconds) noexcept
    {
        return seconds > 0.0 ? static_cast<float> (std::exp (-1.0 / (seconds * fs))) : 0.0f;
    }

    float attack_ = 0.0f, release_ = 0.0f, env_ = 0.0f;
};

} // namespace sedef
