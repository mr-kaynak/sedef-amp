// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <cmath>

namespace sedef
{

/// LFO phase accumulator in double precision. A float phase drifts at slow rates and high
/// sample rates (0.1 Hz at 96 kHz adds 1e-6 per sample to a value near 1, whose float spacing is
/// 6e-8, so each step rounds by a few per cent); this one keeps the rate exact to 1e-9.
class PrecisePhase
{
public:
    void prepare (double sampleRate) noexcept { sampleRate_ = sampleRate; }
    void setFrequency (double hz) noexcept { increment_ = hz / sampleRate_; }
    void setPhase (double phase) noexcept { phase_ = phase - std::floor (phase); }

    /// Advances one sample and returns the new phase in [0, 1).
    float advance() noexcept
    {
        phase_ += increment_;
        if (phase_ >= 1.0)
            phase_ -= 1.0;
        return static_cast<float> (phase_);
    }

private:
    double sampleRate_ = 48000.0;
    double phase_ = 0.0;
    double increment_ = 0.0;
};

} // namespace sedef
