// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "sedef/core/Math.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace sedef
{

/// The board's duck: raised-cosine fades to silence, a hold there while the owner rewires the
/// pedals, and the same fades back. There are two gains: one for the board's input and one for
/// its output, which is the input's gain `lag` samples ago, the gain the signal leaving the
/// pedals had when it went in. On the way down the output is silent once the signal has left
/// every pedal's pipeline; on the way up it stays silent until the new signal arrives, so
/// whatever the rewired pedals still held (filter tails, rerouted at the rewire) never gets
/// through. Either way the duck can turn round half way; it continues from the gains it
/// reached, so a change of mind never jumps.
///
/// prepare() allocates; everything else is real-time safe.
class Duck
{
public:
    void prepare (double sampleRate, double fadeSeconds, int lagSamples)
    {
        length_ = std::max (1, static_cast<int> (std::lround (fadeSeconds * sampleRate)));
        lag_ = std::max (0, lagSamples);
        history_.assign (static_cast<std::size_t> (lag_) + 1, 0);
        reset();
    }

    /// Unity gain, nothing pending.
    void reset() noexcept
    {
        depth_ = 0;
        direction_ = 0;
        std::fill (history_.begin(), history_.end(), 0);
        write_ = 0;
        steady_ = lag_;
    }

    void fadeOut() noexcept { direction_ = 1; }
    void fadeIn() noexcept { direction_ = -1; }

    /// Both gains at unity, nothing pending.
    bool idle() const noexcept { return depth_ == 0 && direction_ <= 0 && steady_ >= lag_; }
    bool fadingOut() const noexcept { return direction_ > 0; }
    /// Both gains at zero, waiting for fadeIn().
    bool silent() const noexcept { return depth_ == length_ && steady_ >= lag_; }
    /// Samples from unity to silence.
    int downSamples() const noexcept { return length_ + lag_; }
    /// Samples until silence while fading out, else 0.
    int samplesToSilence() const noexcept
    {
        if (direction_ <= 0)
            return 0;
        return depth_ < length_ ? length_ - depth_ + lag_ : std::max (0, lag_ - steady_);
    }

    /// Writes the input and output gains of the next n samples and advances. False, writing
    /// nothing, at unity.
    bool render (float* input, float* output, int n) noexcept
    {
        if (idle())
            return false;
        const int size = lag_ + 1;
        for (int i = 0; i < n; ++i)
        {
            const int next = std::clamp (depth_ + direction_, 0, length_);
            steady_ = next == depth_ ? steady_ + 1 : 0;
            depth_ = next;
            history_[static_cast<std::size_t> (write_)] = depth_;
            const int read = write_ >= lag_ ? write_ - lag_ : write_ + size - lag_;
            write_ = write_ + 1 == size ? 0 : write_ + 1;
            input[i] = curve (depth_);
            output[i] = curve (history_[static_cast<std::size_t> (read)]);
        }
        if (depth_ == 0 && direction_ < 0)
            direction_ = 0;
        return true;
    }

    float inputGain() const noexcept { return curve (depth_); }

private:
    float curve (int depth) const noexcept
    {
        const double x = static_cast<double> (depth) / static_cast<double> (length_);
        return static_cast<float> (0.5 + 0.5 * std::cos (kPiD * x));
    }

    int length_ = 1;
    int lag_ = 0;
    int depth_ = 0;            // the input's: 0 = unity, length_ = silence
    int direction_ = 0;        // +1 fading out, -1 fading in, 0 holding
    std::vector<int> history_; // the input's depth over the last lag_ + 1 samples
    int write_ = 0;
    int steady_ = 0; // samples the input's depth has stayed where it is
};

} // namespace sedef
