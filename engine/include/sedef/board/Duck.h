// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "sedef/core/Math.h"

#include <algorithm>
#include <cmath>

namespace sedef
{

/// The board's duck: a raised-cosine fade to silence, a hold there while the owner rewires the
/// pedals, and a raised-cosine fade back. Either fade can turn round half way; it continues from
/// the gain it reached, so a change of mind never jumps.
class Duck
{
public:
    void prepare (double sampleRate, double fadeSeconds) noexcept
    {
        length_ = std::max (1, static_cast<int> (std::lround (fadeSeconds * sampleRate)));
        reset();
    }

    /// Unity gain, nothing pending.
    void reset() noexcept
    {
        depth_ = 0;
        direction_ = 0;
    }

    void fadeOut() noexcept { direction_ = 1; }
    void fadeIn() noexcept { direction_ = depth_ > 0 ? -1 : 0; }

    bool idle() const noexcept { return depth_ == 0 && direction_ <= 0; }
    bool fadingOut() const noexcept { return direction_ > 0; }
    /// At silence, waiting for fadeIn().
    bool silent() const noexcept { return depth_ == length_; }
    int fadeSamples() const noexcept { return length_; }
    /// Samples until silence while fading out, else 0.
    int samplesToSilence() const noexcept { return direction_ > 0 ? length_ - depth_ : 0; }

    /// Writes the gains of the next n samples and advances. False, writing nothing, at unity.
    bool render (float* gain, int n) noexcept
    {
        if (idle())
            return false;
        for (int i = 0; i < n; ++i)
        {
            depth_ = std::clamp (depth_ + direction_, 0, length_);
            gain[i] = gainAt (depth_);
        }
        if (depth_ == 0)
            direction_ = 0;
        return true;
    }

    float gain() const noexcept { return gainAt (depth_); }

private:
    float gainAt (int depth) const noexcept
    {
        const double x = static_cast<double> (depth) / static_cast<double> (length_);
        return static_cast<float> (0.5 + 0.5 * std::cos (kPiD * x));
    }

    int length_ = 1;
    int depth_ = 0;     // 0 = unity, length_ = silence
    int direction_ = 0; // +1 fading out, -1 fading in, 0 holding
};

} // namespace sedef
