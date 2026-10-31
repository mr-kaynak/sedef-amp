// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "sedef/core/DelayLine.h"
#include "sedef/core/Math.h"

#include <algorithm>
#include <cmath>

namespace sedef::dsp::reverb
{

/// One-pole low-pass, y += (1 - c) (x - y). Unity gain at DC and |H| <= 1 everywhere, so it can
/// sit inside a feedback loop without adding gain.
class OnePoleLowpass
{
public:
    void setCutoff (double sampleRate, double hz) noexcept
    {
        coeff_ =
            static_cast<float> (std::exp (-kTwoPiD * clampFrequency (hz, sampleRate) / sampleRate));
    }

    float process (float x) noexcept
    {
        y_ = x + coeff_ * (y_ - x);
        return y_;
    }

    void clear() noexcept { y_ = 0.0f; }
    void snapToZero() noexcept { y_ = flushToZero (y_); }

private:
    float coeff_ = 0.0f;
    float y_ = 0.0f;
};

/// Schroeder all-pass in the lattice form of Dattorro (1997), Fig. 1:
///   v[n] = x[n] - g v[n-D],   y[n] = v[n-D] + g v[n],   H(z) = (g + z^-D) / (1 + g z^-D).
class Allpass
{
public:
    /// headroom: extra samples for a modulated (longer) delay.
    void prepare (int delaySamples, int headroom = 0)
    {
        delay_ = std::max (delaySamples, 2);
        line_.prepare (delay_ + headroom + 4);
    }

    void clear() noexcept { line_.reset(); }

    float process (float x, float g) noexcept
    {
        // Before the push, read (D - 1) is v[n - D].
        const float z = line_.read (delay_ - 1);
        const float v = flushToZero (x - g * z);
        line_.push (v);
        return z + g * v;
    }

    /// Delay D + offset (fractional, Hermite-interpolated). Interpolation keeps |H| <= 1, so the
    /// section stays passive while its delay moves.
    float processModulated (float x, float g, float offset) noexcept
    {
        const float z = line_.readCubic (static_cast<float> (delay_ - 1) + offset);
        const float v = flushToZero (x - g * z);
        line_.push (v);
        return z + g * v;
    }

    /// Internal node v[n - k] (for output taps).
    float tap (int k) const noexcept { return line_.read (k); }
    int delay() const noexcept { return delay_; }

private:
    DelayLine line_;
    int delay_ = 2;
};

/// Rounds a length given at a reference rate to the session rate.
inline int scaleLength (int samplesAtRef, double refRate, double sampleRate) noexcept
{
    return std::max (1, static_cast<int> (std::lround (samplesAtRef * sampleRate / refRate)));
}

inline bool isPrime (int n) noexcept
{
    if (n < 2)
        return false;
    for (int d = 2; d * d <= n; ++d)
        if (n % d == 0)
            return false;
    return true;
}

/// Smallest prime >= n. Prime line lengths are pairwise coprime, so their echoes never coincide.
inline int nextPrime (int n) noexcept
{
    while (!isPrime (n))
        ++n;
    return n;
}

/// Gain per pass of a loop of the given length for an RT60 (-60 dB after rt60 seconds).
inline double loopGainForRt60 (double loopSeconds, double rt60Seconds) noexcept
{
    return std::pow (10.0, -3.0 * loopSeconds / rt60Seconds);
}

} // namespace sedef::dsp::reverb
