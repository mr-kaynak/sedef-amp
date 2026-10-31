// SPDX-License-Identifier: AGPL-3.0-or-later
#include "sedef/core/Biquad.h"

#include "sedef/core/Math.h"

#include <cmath>
#include <complex>

namespace sedef::design
{
namespace
{
BiquadCoeffs normalise (double b0, double b1, double b2, double a0, double a1, double a2) noexcept
{
    const double inv = 1.0 / a0;
    return { static_cast<float> (b0 * inv), static_cast<float> (b1 * inv),
             static_cast<float> (b2 * inv), static_cast<float> (a1 * inv),
             static_cast<float> (a2 * inv) };
}

struct Omega
{
    double cosw, sinw;
};

Omega omega (double fs, double hz) noexcept
{
    const double w = kTwoPiD * clampFrequency (hz, fs) / fs;
    return { std::cos (w), std::sin (w) };
}

double prewarp (double fs, double hz) noexcept
{
    return std::tan (kPiD * clampFrequency (hz, fs) / fs);
}
} // namespace

BiquadCoeffs lowpass (double fs, double hz, double q) noexcept
{
    const auto [cw, sw] = omega (fs, hz);
    const double alpha = sw / (2.0 * q);
    return normalise ((1.0 - cw) * 0.5, 1.0 - cw, (1.0 - cw) * 0.5, 1.0 + alpha, -2.0 * cw,
                      1.0 - alpha);
}

BiquadCoeffs highpass (double fs, double hz, double q) noexcept
{
    const auto [cw, sw] = omega (fs, hz);
    const double alpha = sw / (2.0 * q);
    return normalise ((1.0 + cw) * 0.5, -(1.0 + cw), (1.0 + cw) * 0.5, 1.0 + alpha, -2.0 * cw,
                      1.0 - alpha);
}

BiquadCoeffs bandpass (double fs, double hz, double q) noexcept
{
    const auto [cw, sw] = omega (fs, hz);
    const double alpha = sw / (2.0 * q);
    return normalise (alpha, 0.0, -alpha, 1.0 + alpha, -2.0 * cw, 1.0 - alpha);
}

BiquadCoeffs notch (double fs, double hz, double q) noexcept
{
    const auto [cw, sw] = omega (fs, hz);
    const double alpha = sw / (2.0 * q);
    return normalise (1.0, -2.0 * cw, 1.0, 1.0 + alpha, -2.0 * cw, 1.0 - alpha);
}

BiquadCoeffs allpass (double fs, double hz, double q) noexcept
{
    const auto [cw, sw] = omega (fs, hz);
    const double alpha = sw / (2.0 * q);
    return normalise (1.0 - alpha, -2.0 * cw, 1.0 + alpha, 1.0 + alpha, -2.0 * cw, 1.0 - alpha);
}

BiquadCoeffs peaking (double fs, double hz, double q, double gainDb) noexcept
{
    const auto [cw, sw] = omega (fs, hz);
    const double a = std::pow (10.0, gainDb / 40.0);
    const double alpha = sw / (2.0 * q);
    return normalise (1.0 + alpha * a, -2.0 * cw, 1.0 - alpha * a, 1.0 + alpha / a, -2.0 * cw,
                      1.0 - alpha / a);
}

BiquadCoeffs lowShelf (double fs, double hz, double q, double gainDb) noexcept
{
    const auto [cw, sw] = omega (fs, hz);
    const double a = std::pow (10.0, gainDb / 40.0);
    const double alpha = sw / (2.0 * q);
    const double k = 2.0 * std::sqrt (a) * alpha;
    return normalise (a * ((a + 1.0) - (a - 1.0) * cw + k), 2.0 * a * ((a - 1.0) - (a + 1.0) * cw),
                      a * ((a + 1.0) - (a - 1.0) * cw - k), (a + 1.0) + (a - 1.0) * cw + k,
                      -2.0 * ((a - 1.0) + (a + 1.0) * cw), (a + 1.0) + (a - 1.0) * cw - k);
}

BiquadCoeffs highShelf (double fs, double hz, double q, double gainDb) noexcept
{
    const auto [cw, sw] = omega (fs, hz);
    const double a = std::pow (10.0, gainDb / 40.0);
    const double alpha = sw / (2.0 * q);
    const double k = 2.0 * std::sqrt (a) * alpha;
    return normalise (a * ((a + 1.0) + (a - 1.0) * cw + k), -2.0 * a * ((a - 1.0) + (a + 1.0) * cw),
                      a * ((a + 1.0) + (a - 1.0) * cw - k), (a + 1.0) - (a - 1.0) * cw + k,
                      2.0 * ((a - 1.0) - (a + 1.0) * cw), (a + 1.0) - (a - 1.0) * cw - k);
}

BiquadCoeffs lowpass1 (double fs, double hz) noexcept
{
    const double k = prewarp (fs, hz);
    return normalise (k, k, 0.0, k + 1.0, k - 1.0, 0.0);
}

BiquadCoeffs highpass1 (double fs, double hz) noexcept
{
    const double k = prewarp (fs, hz);
    return normalise (1.0, -1.0, 0.0, k + 1.0, k - 1.0, 0.0);
}

BiquadCoeffs allpass1 (double fs, double hz) noexcept
{
    const double k = prewarp (fs, hz);
    return normalise (k - 1.0, k + 1.0, 0.0, k + 1.0, k - 1.0, 0.0);
}

BiquadCoeffs lowShelf1 (double fs, double hz, double gainDb) noexcept
{
    // H(s) = (s + g) / (s + 1/g), g = sqrt(G), s normalised to the corner frequency.
    const double k = prewarp (fs, hz);
    const double g = std::pow (10.0, gainDb / 40.0);
    return normalise (1.0 + g * k, g * k - 1.0, 0.0, 1.0 + k / g, k / g - 1.0, 0.0);
}

BiquadCoeffs highShelf1 (double fs, double hz, double gainDb) noexcept
{
    // H(s) = (g s + 1) / (s / g + 1).
    const double k = prewarp (fs, hz);
    const double g = std::pow (10.0, gainDb / 40.0);
    return normalise (g + k, k - g, 0.0, 1.0 / g + k, k - 1.0 / g, 0.0);
}

double magnitude (const BiquadCoeffs& c, double hz, double fs) noexcept
{
    const double w = kTwoPiD * hz / fs;
    const std::complex<double> z1 = std::polar (1.0, -w);
    const std::complex<double> z2 = z1 * z1;
    const auto num = static_cast<double> (c.b0) + static_cast<double> (c.b1) * z1 +
                     static_cast<double> (c.b2) * z2;
    const auto den = 1.0 + static_cast<double> (c.a1) * z1 + static_cast<double> (c.a2) * z2;
    return std::abs (num / den);
}

} // namespace sedef::design

namespace sedef
{
void Biquad::snapToZero() noexcept
{
    for (auto& s : state_)
        for (auto& v : s)
            v = flushToZero (v);
}
} // namespace sedef
