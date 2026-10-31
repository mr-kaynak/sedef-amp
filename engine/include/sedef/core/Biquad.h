// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "sedef/core/AudioView.h"

#include <array>

namespace sedef
{

/// Normalised biquad coefficients (a0 == 1).
struct BiquadCoeffs
{
    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
};

/// Filter designers. Second-order designs follow Robert Bristow-Johnson's "Audio EQ Cookbook";
/// first-order designs use the bilinear transform with frequency pre-warping. All frequencies are
/// clamped to (0, 0.49 * fs).
namespace design
{
BiquadCoeffs lowpass (double fs, double hz, double q = 0.7071067811865476) noexcept;
BiquadCoeffs highpass (double fs, double hz, double q = 0.7071067811865476) noexcept;
BiquadCoeffs bandpass (double fs, double hz, double q) noexcept; ///< 0 dB peak gain
BiquadCoeffs notch (double fs, double hz, double q) noexcept;
BiquadCoeffs allpass (double fs, double hz, double q) noexcept;
BiquadCoeffs peaking (double fs, double hz, double q, double gainDb) noexcept;
BiquadCoeffs lowShelf (double fs, double hz, double q, double gainDb) noexcept;
BiquadCoeffs highShelf (double fs, double hz, double q, double gainDb) noexcept;

BiquadCoeffs lowpass1 (double fs, double hz) noexcept;
BiquadCoeffs highpass1 (double fs, double hz) noexcept;
BiquadCoeffs allpass1 (double fs, double hz) noexcept;
/// First-order shelves; the transition is centred (geometric mean) on hz.
BiquadCoeffs lowShelf1 (double fs, double hz, double gainDb) noexcept;
BiquadCoeffs highShelf1 (double fs, double hz, double gainDb) noexcept;

/// |H(e^jw)| at the given frequency. For tests, plots and UI curves.
double magnitude (const BiquadCoeffs& c, double hz, double fs) noexcept;
} // namespace design

/// Transposed direct form II biquad with independent state per channel.
class Biquad
{
public:
    void setCoeffs (const BiquadCoeffs& c) noexcept { c_ = c; }
    const BiquadCoeffs& coeffs() const noexcept { return c_; }

    void reset() noexcept { state_ = {}; }

    float processSample (int channel, float x) noexcept
    {
        auto& s = state_[static_cast<size_t> (channel)];
        const float y = c_.b0 * x + s[0];
        s[0] = c_.b1 * x - c_.a1 * y + s[1];
        s[1] = c_.b2 * x - c_.a2 * y;
        return y;
    }

    void process (const AudioView& io) noexcept
    {
        for (int ch = 0; ch < io.numChannels(); ++ch)
        {
            float* d = io.channel (ch);
            for (int i = 0; i < io.numFrames(); ++i)
                d[i] = processSample (ch, d[i]);
        }
    }

    /// Flushes denormal-range state. Call once per block in long-tail feedback structures.
    void snapToZero() noexcept;

private:
    BiquadCoeffs c_;
    std::array<std::array<float, 2>, kMaxChannels> state_ {};
};

} // namespace sedef
