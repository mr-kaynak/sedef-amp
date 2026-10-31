// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "sedef/core/Math.h"
#include "sedef/core/Random.h"

#include <cmath>

namespace sedef
{

/// Phase accumulator LFO. Phase runs in [0, 1).
class Lfo
{
public:
    void prepare (double sampleRate) noexcept { sampleRate_ = sampleRate; }
    void setFrequency (float hz) noexcept { increment_ = static_cast<float> (hz / sampleRate_); }
    void setPhase (float phase) noexcept { phase_ = phase - std::floor (phase); }
    float phase() const noexcept { return phase_; }

    /// Advances one sample and returns the new phase.
    float advance() noexcept
    {
        phase_ += increment_;
        if (phase_ >= 1.0f)
            phase_ -= 1.0f;
        return phase_;
    }

    void advance (int n) noexcept
    {
        phase_ += increment_ * static_cast<float> (n);
        phase_ -= std::floor (phase_);
    }

    static float sine (float phase) noexcept { return std::sin (kTwoPi * phase); }

    /// Triangle in [-1, 1], peak at phase 0.25.
    static float triangle (float phase) noexcept
    {
        const float t = phase + 0.25f;
        const float f = t - std::floor (t);
        return 4.0f * std::abs (f - 0.5f) - 1.0f;
    }

private:
    double sampleRate_ = 48000.0;
    float phase_ = 0.0f;
    float increment_ = 0.0f;
};

/// Band-limited random walk: new random target every period, cosine-interpolated in between.
/// Good for tape flutter and chorus drift.
class SmoothRandom
{
public:
    void prepare (double sampleRate, std::uint32_t seed) noexcept
    {
        sampleRate_ = sampleRate;
        rng_.seed (seed);
        from_ = 0.0f;
        to_ = rng_.bipolar();
    }

    void setRate (float hz) noexcept { increment_ = static_cast<float> (hz / sampleRate_); }

    float next() noexcept
    {
        phase_ += increment_;
        if (phase_ >= 1.0f)
        {
            phase_ -= 1.0f;
            from_ = to_;
            to_ = rng_.bipolar();
        }
        const float w = 0.5f - 0.5f * std::cos (kPi * phase_);
        return from_ + (to_ - from_) * w;
    }

private:
    Random rng_;
    double sampleRate_ = 48000.0;
    float phase_ = 0.0f;
    float increment_ = 0.0f;
    float from_ = 0.0f;
    float to_ = 0.0f;
};

} // namespace sedef
