// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <cmath>

namespace sedef
{

/// Attack/release envelope follower on |x| (peak) or x^2 (power).
class EnvelopeFollower
{
public:
    enum class Mode
    {
        Peak,
        Power
    };

    void prepare (double sampleRate) noexcept
    {
        sampleRate_ = sampleRate;
        setTimes (attackMs_, releaseMs_);
    }

    void setTimes (float attackMs, float releaseMs) noexcept
    {
        attackMs_ = attackMs;
        releaseMs_ = releaseMs;
        attack_ = coeff (attackMs);
        release_ = coeff (releaseMs);
    }

    void setMode (Mode m) noexcept { mode_ = m; }
    void reset (float value = 0.0f) noexcept { env_ = value; }

    float process (float x) noexcept
    {
        const float in = mode_ == Mode::Peak ? std::abs (x) : x * x;
        const float c = in > env_ ? attack_ : release_;
        env_ = in + c * (env_ - in);
        return env_;
    }

    float value() const noexcept { return env_; }

private:
    float coeff (float ms) const noexcept
    {
        return ms > 0.0f ? static_cast<float> (std::exp (-1.0 / (0.001 * ms * sampleRate_))) : 0.0f;
    }

    double sampleRate_ = 48000.0;
    Mode mode_ = Mode::Peak;
    float attackMs_ = 1.0f, releaseMs_ = 100.0f;
    float attack_ = 0.0f, release_ = 0.0f;
    float env_ = 0.0f;
};

} // namespace sedef
