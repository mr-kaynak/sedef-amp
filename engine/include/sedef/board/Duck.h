// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "sedef/core/Math.h"

#include <algorithm>
#include <cmath>

namespace sedef
{

/// The board's duck: raised-cosine fades to silence, a hold there while the owner rewires the
/// pedals, and the same fades back. There are two gains: one for the board's input and one for
/// its output, which trails the input by `lag` samples on the way down, so by the time it is
/// silent the signal has left every pedal's pipeline (whatever the rewired pedals hold is then
/// silence). Either way the duck can turn round half way; it continues from the gains it
/// reached, so a change of mind never jumps.
class Duck
{
public:
    void prepare (double sampleRate, double fadeSeconds, int lagSamples) noexcept
    {
        length_ = std::max (1, static_cast<int> (std::lround (fadeSeconds * sampleRate)));
        lag_ = std::max (0, lagSamples);
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
    /// Both gains at zero, waiting for fadeIn().
    bool silent() const noexcept { return depth_ == bottom(); }
    /// Samples from unity to silence.
    int downSamples() const noexcept { return bottom(); }
    /// Samples until silence while fading out, else 0.
    int samplesToSilence() const noexcept { return direction_ > 0 ? bottom() - depth_ : 0; }

    /// Writes the input and output gains of the next n samples and advances. False, writing
    /// nothing, at unity.
    bool render (float* input, float* output, int n) noexcept
    {
        if (idle())
            return false;
        for (int i = 0; i < n; ++i)
        {
            depth_ = std::clamp (depth_ + direction_, 0, bottom());
            input[i] = curve (std::min (depth_, length_));
            output[i] = curve (std::max (0, depth_ - lag_));
        }
        if (depth_ == 0)
            direction_ = 0;
        return true;
    }

    float inputGain() const noexcept { return curve (std::min (depth_, length_)); }
    float outputGain() const noexcept { return curve (std::max (0, depth_ - lag_)); }

private:
    int bottom() const noexcept { return length_ + lag_; }

    float curve (int depth) const noexcept
    {
        const double x = static_cast<double> (depth) / static_cast<double> (length_);
        return static_cast<float> (0.5 + 0.5 * std::cos (kPiD * x));
    }

    int length_ = 1;
    int lag_ = 0;
    int depth_ = 0;     // 0 = unity, bottom() = silence
    int direction_ = 0; // +1 fading out, -1 fading in, 0 holding
};

} // namespace sedef
