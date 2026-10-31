// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

namespace sedef
{

/// Single-channel circular delay line with power-of-two storage.
///
/// Convention: after push(x), read(0) == x. A delay of D samples is push() followed by read(D).
class DelayLine
{
public:
    /// Allocates storage for delays up to maxDelaySamples (plus interpolation headroom).
    void prepare (int maxDelaySamples)
    {
        int size = 1;
        while (size < maxDelaySamples + 4)
            size <<= 1;
        buffer_.assign (static_cast<size_t> (size), 0.0f);
        mask_ = size - 1;
        write_ = 0;
        maxDelay_ = maxDelaySamples;
    }

    void reset() noexcept { std::fill (buffer_.begin(), buffer_.end(), 0.0f); }

    void push (float x) noexcept
    {
        write_ = (write_ + 1) & mask_;
        buffer_[static_cast<size_t> (write_)] = x;
    }

    float read (int delay) const noexcept
    {
        return buffer_[static_cast<size_t> ((write_ - delay) & mask_)];
    }

    float readLinear (float delay) const noexcept
    {
        delay = std::clamp (delay, 0.0f, static_cast<float> (maxDelay_));
        const int i = static_cast<int> (delay);
        const float frac = delay - static_cast<float> (i);
        const float a = read (i);
        const float b = read (i + 1);
        return a + frac * (b - a);
    }

    /// 4-point, 3rd-order Hermite interpolation. Delay is clamped to [1, maxDelay].
    float readCubic (float delay) const noexcept
    {
        delay = std::clamp (delay, 1.0f, static_cast<float> (maxDelay_));
        const int i = static_cast<int> (delay);
        const float t = delay - static_cast<float> (i);
        const float xm1 = read (i - 1);
        const float x0 = read (i);
        const float x1 = read (i + 1);
        const float x2 = read (i + 2);
        const float c1 = 0.5f * (x1 - xm1);
        const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
        const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
        return ((c3 * t + c2) * t + c1) * t + x0;
    }

    int maxDelay() const noexcept { return maxDelay_; }

private:
    std::vector<float> buffer_ { 0.0f, 0.0f, 0.0f, 0.0f };
    int mask_ = 3;
    int write_ = 0;
    int maxDelay_ = 0;
};

} // namespace sedef
