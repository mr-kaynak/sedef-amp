// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "sedef/core/Biquad.h"
#include "sedef/core/Math.h"

#include <array>
#include <cmath>

namespace sedef::dsp::detail
{

/// Bilinear transform of a second-order analogue transfer function N(s) / D(s) (coefficients in
/// ascending powers of s), pre-warped so the response is exact at warpHz.
inline BiquadCoeffs bilinear (const std::array<double, 3>& n, const std::array<double, 3>& d,
                              double fs, double warpHz) noexcept
{
    const double w = kTwoPiD * clampFrequency (warpHz, fs);
    const double k = w / std::tan (w / (2.0 * fs));
    const double k2 = k * k;
    const double a0 = d[0] + d[1] * k + d[2] * k2;
    BiquadCoeffs c;
    c.b0 = static_cast<float> ((n[0] + n[1] * k + n[2] * k2) / a0);
    c.b1 = static_cast<float> ((2.0 * n[0] - 2.0 * n[2] * k2) / a0);
    c.b2 = static_cast<float> ((n[0] - n[1] * k + n[2] * k2) / a0);
    c.a1 = static_cast<float> ((2.0 * d[0] - 2.0 * d[2] * k2) / a0);
    c.a2 = static_cast<float> ((d[0] - d[1] * k + d[2] * k2) / a0);
    return c;
}

} // namespace sedef::dsp::detail
