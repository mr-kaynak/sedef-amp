// SPDX-License-Identifier: AGPL-3.0-or-later
#include "sedef/dsp/amp/ToneStack.h"

#include <algorithm>
#include <cmath>

namespace sedef::dsp::amp
{

ToneStackAnalog toneStackAnalog (const ToneStackComponents& c, double t, double m,
                                 double l) noexcept
{
    // Coefficients as published by Yeh & Smith (DAFx-06, eq. 3); re-derived from the nodal
    // equations and checked against an independent nodal solve in the tests.
    const double C1 = c.c1, C2 = c.c2, C3 = c.c3;
    const double R1 = c.r1, R2 = c.r2, R3 = c.r3, R4 = c.r4;
    t = std::clamp (t, 0.0, 1.0);
    m = std::clamp (m, 0.0, 1.0);
    l = std::clamp (l, 0.0, 1.0);
    const double m2 = m * m, lm = l * m;
    const double C123 = C1 * C2 * C3;

    ToneStackAnalog h;
    h.b[0] = 0.0;
    h.b[1] = t * C1 * R1 + m * C3 * R3 + l * (C1 * R2 + C2 * R2) + (C1 * R3 + C2 * R3);
    h.b[2] = t * (C1 * C2 * R1 * R4 + C1 * C3 * R1 * R4) -
             m2 * (C1 * C3 * R3 * R3 + C2 * C3 * R3 * R3) +
             m * (C1 * C3 * R1 * R3 + C1 * C3 * R3 * R3 + C2 * C3 * R3 * R3) +
             l * (C1 * C2 * R1 * R2 + C1 * C2 * R2 * R4 + C1 * C3 * R2 * R4) +
             lm * (C1 * C3 * R2 * R3 + C2 * C3 * R2 * R3) +
             (C1 * C2 * R1 * R3 + C1 * C2 * R3 * R4 + C1 * C3 * R3 * R4);
    h.b[3] = lm * C123 * (R1 * R2 * R3 + R2 * R3 * R4) - m2 * C123 * (R1 * R3 * R3 + R3 * R3 * R4) +
             m * C123 * (R1 * R3 * R3 + R3 * R3 * R4) + t * C123 * R1 * R3 * R4 -
             t * m * C123 * R1 * R3 * R4 + t * l * C123 * R1 * R2 * R4;

    h.a[0] = 1.0;
    h.a[1] =
        (C1 * R1 + C1 * R3 + C2 * R3 + C2 * R4 + C3 * R4) + m * C3 * R3 + l * (C1 * R2 + C2 * R2);
    h.a[2] = m * (C1 * C3 * R1 * R3 - C2 * C3 * R3 * R4 + C1 * C3 * R3 * R3 + C2 * C3 * R3 * R3) +
             lm * (C1 * C3 * R2 * R3 + C2 * C3 * R2 * R3) -
             m2 * (C1 * C3 * R3 * R3 + C2 * C3 * R3 * R3) +
             l * (C1 * C2 * R2 * R4 + C1 * C2 * R1 * R2 + C1 * C3 * R2 * R4 + C2 * C3 * R2 * R4) +
             (C1 * C2 * R1 * R4 + C1 * C3 * R1 * R4 + C1 * C2 * R3 * R4 + C1 * C2 * R1 * R3 +
              C1 * C3 * R3 * R4 + C2 * C3 * R3 * R4);
    h.a[3] = lm * C123 * (R1 * R2 * R3 + R2 * R3 * R4) - m2 * C123 * (R1 * R3 * R3 + R3 * R3 * R4) +
             m * C123 * (R3 * R3 * R4 + R1 * R3 * R3 - R1 * R3 * R4) + l * C123 * R1 * R2 * R4 +
             C123 * R1 * R3 * R4;
    return h;
}

ToneStackCoeffs toneStackDigital (const ToneStackAnalog& h, double sampleRate) noexcept
{
    // s = K (1 - z^-1) / (1 + z^-1). Multiplying through by (1 + z^-1)^3 maps s^k to
    // K^k (1 - z^-1)^k (1 + z^-1)^(3-k), whose z^-n coefficients are the rows below.
    static constexpr double kExpand[4][4] = {
        { 1.0, 3.0, 3.0, 1.0 },   // (1+z)^3
        { 1.0, 1.0, -1.0, -1.0 }, // (1-z)(1+z)^2
        { 1.0, -1.0, -1.0, 1.0 }, // (1-z)^2(1+z)
        { 1.0, -3.0, 3.0, -1.0 }, // (1-z)^3
    };
    const double k = 2.0 * sampleRate;
    std::array<double, 4> b {}, a {};
    double kPow = 1.0;
    for (int p = 0; p < 4; ++p)
    {
        const auto ps = static_cast<size_t> (p);
        for (int n = 0; n < 4; ++n)
        {
            const auto ns = static_cast<size_t> (n);
            b[ns] += h.b[ps] * kPow * kExpand[p][n];
            a[ns] += h.a[ps] * kPow * kExpand[p][n];
        }
        kPow *= k;
    }

    ToneStackCoeffs c;
    const double inv = 1.0 / a[0];
    for (size_t n = 0; n < 4; ++n)
    {
        c.b[n] = b[n] * inv;
        c.a[n] = a[n] * inv;
    }
    return c;
}

double logTaper (double knob) noexcept
{
    // (81^x - 1) / 80: 0 -> 0, 0.5 -> 0.1, 1 -> 1.
    knob = std::clamp (knob, 0.0, 1.0);
    return (std::exp (knob * 4.394449154672439) - 1.0) / 80.0;
}

void ToneStackFilter::snapToZero() noexcept
{
    for (auto& s : state_)
        for (auto& v : s)
            v = std::abs (v) < 1.0e-15 ? 0.0 : v;
}

} // namespace sedef::dsp::amp
