// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <cmath>

namespace sedef
{

/// Linear ramp towards a target over a fixed time. Use for gains and mix amounts.
class LinearSmoothed
{
public:
    void reset (double sampleRate, double rampSeconds) noexcept
    {
        steps_ = static_cast<int> (std::floor (rampSeconds * sampleRate));
        setCurrentAndTarget (target_);
    }

    void setCurrentAndTarget (float value) noexcept
    {
        current_ = target_ = value;
        countdown_ = 0;
    }

    void setTarget (float value) noexcept
    {
        if (value == target_)
            return;
        if (steps_ <= 0)
        {
            setCurrentAndTarget (value);
            return;
        }
        target_ = value;
        countdown_ = steps_;
        step_ = (target_ - current_) / static_cast<float> (countdown_);
    }

    float next() noexcept
    {
        if (countdown_ <= 0)
            return target_;
        --countdown_;
        current_ = countdown_ > 0 ? current_ + step_ : target_;
        return current_;
    }

    /// Advances by n samples and returns the value reached.
    float skip (int n) noexcept
    {
        if (n >= countdown_)
        {
            countdown_ = 0;
            current_ = target_;
            return target_;
        }
        current_ += step_ * static_cast<float> (n);
        countdown_ -= n;
        return current_;
    }

    bool isSmoothing() const noexcept { return countdown_ > 0; }
    float current() const noexcept { return current_; }
    float target() const noexcept { return target_; }

private:
    float current_ = 0.0f;
    float target_ = 0.0f;
    float step_ = 0.0f;
    int countdown_ = 0;
    int steps_ = 0;
};

/// One-pole exponential smoother. Use for frequencies and other values where a ratio-like glide
/// sounds natural; call next() per sample or skip() per control block.
class ExpSmoothed
{
public:
    void reset (double sampleRate, double timeConstantSeconds) noexcept
    {
        coeff_ = timeConstantSeconds > 0.0
                     ? static_cast<float> (std::exp (-1.0 / (timeConstantSeconds * sampleRate)))
                     : 0.0f;
        current_ = target_;
    }

    void setCurrentAndTarget (float value) noexcept { current_ = target_ = value; }
    void setTarget (float value) noexcept { target_ = value; }

    float next() noexcept
    {
        current_ = target_ + coeff_ * (current_ - target_);
        return current_;
    }

    float skip (int n) noexcept
    {
        current_ = target_ + std::pow (coeff_, static_cast<float> (n)) * (current_ - target_);
        return current_;
    }

    bool isSmoothing (float tolerance = 1.0e-4f) const noexcept
    {
        return std::abs (current_ - target_) > tolerance * (std::abs (target_) + 1.0e-6f);
    }

    float current() const noexcept { return current_; }
    float target() const noexcept { return target_; }

private:
    float current_ = 0.0f;
    float target_ = 0.0f;
    float coeff_ = 0.0f;
};

} // namespace sedef
