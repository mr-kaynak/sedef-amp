// SPDX-License-Identifier: AGPL-3.0-or-later
#include "sedef/dsp/detail/BjtStage.h"

#include <catch2/catch_test_macros.hpp>
#include <cmath>

using namespace sedef::dsp::detail;

namespace
{
// Shapes the pedals use: germanium and silicon pairs at noon, a starved and a hot germanium pair,
// and the booster at minimum and full boost.
const BjtShape kShapes[] = {
    { 3.0, 1.0, 1.0 },   { 24.0, 1.0, 0.25 }, { 0.09, 65.7, 1.0 },
    { 5.6, 0.064, 1.0 }, { 32.0, 2.29, 1.0 }, { 0.27, 2.29, 1.0 },
};

double drive (const BjtShape& s, double x)
{
    double u = std::log1p (x) + s.g * x;
    if (x > 0.0)
    {
        const double z = x / s.xs;
        u += s.kappa * s.xs * (std::atanh (z) - z);
    }
    return u;
}

/// Reference solution by bisection on the defining equation.
double bisect (const BjtShape& s, double w)
{
    double lo = -1.0, hi = s.xs;
    for (int i = 0; i < 200; ++i)
    {
        const double mid = 0.5 * (lo + hi);
        (drive (s, mid) < w ? lo : hi) = mid;
    }
    return 0.5 * (lo + hi);
}
} // namespace

TEST_CASE ("Transistor stage: Newton matches bisection", "[pedals][bjt]")
{
    for (const auto& s : kShapes)
    {
        BjtPoint p;
        double wPrev = 0.0;
        double worst = 0.0;
        for (double w = -300.0; w <= 900.0; w += 0.37)
        {
            p = bjt::solve (s, w, p, wPrev);
            wPrev = w;
            worst = std::max (worst, std::abs (p.x - bisect (s, w)));
        }
        CHECK (worst < 1.0e-9);
    }
}

TEST_CASE ("Transistor stage: the curve is monotonic, bounded and linear at rest", "[pedals][bjt]")
{
    for (const auto& s : kShapes)
    {
        BjtCurve c;
        c.build (s);
        double previous = -2.0;
        for (double w = -500.0; w <= 2000.0; w += 0.05)
        {
            const double x = c.at (w).x;
            CHECK (x >= previous - 1.0e-12);
            CHECK (x > -1.0 - 1.0e-12);
            CHECK (x < s.xs + 1.0e-12);
            previous = x;
        }
        CHECK (c.at (0.0).x == 0.0);
        // Slope 1 / (1 + g) at rest.
        const double slope = (c.at (1.0e-4).x - c.at (-1.0e-4).x) / 2.0e-4;
        CHECK (std::abs (slope * (1.0 + s.g) - 1.0) < 1.0e-3);
        // Saturates at xs and cuts off at -1.
        CHECK (std::abs (c.at (1.0e4).x - s.xs) < 1.0e-6 * s.xs);
        CHECK (std::abs (c.at (-1.0e4).x + 1.0) < 1.0e-6);
    }
}

TEST_CASE ("Transistor stage: the table follows the exact curve", "[pedals][bjt]")
{
    for (const auto& s : kShapes)
    {
        BjtCurve c;
        c.build (s);
        double worst = 0.0;
        for (double w = -200.0; w <= 800.0; w += 0.0173)
            worst = std::max (worst, std::abs (c.at (w).x - bisect (s, w)));
        CHECK (worst < 1.0e-5);
    }
}

TEST_CASE ("Transistor stage: the table's integral matches the closed-form antiderivative",
           "[pedals][bjt]")
{
    // The tables integrate their interpolant; the curve's antiderivative also has a closed form
    // through its inverse. Differences between points must agree.
    for (const auto& s : kShapes)
    {
        BjtCurve c;
        c.build (s);
        BjtPoint p0;
        p0 = bjt::solve (s, -3.0, p0, 0.0);
        const double f0 = bjt::antiderivative (s, -3.0, p0);
        const double t0 = c.at (-3.0).integral;
        BjtPoint p;
        double wPrev = -3.0;
        p = p0;
        for (double w = -2.5; w <= 60.0; w += 2.5)
        {
            p = bjt::solve (s, w, p, wPrev);
            wPrev = w;
            const double exact = bjt::antiderivative (s, w, p) - f0;
            const double table = c.at (w).integral - t0;
            CHECK (std::abs (exact - table) < 1.0e-6 * (1.0 + std::abs (exact)));
        }
    }
}

TEST_CASE ("Transistor stage: blending a family keeps values and integrals consistent",
           "[pedals][bjt]")
{
    BjtCurveFamily f;
    f.build (5, [] (double k) { return BjtShape { 1.0 + 10.0 * k, 1.0 + k, 1.0 }; });
    // Between grid points the value is the derivative of the integral.
    for (double knob : { 0.0, 0.1, 0.37, 0.5, 0.99, 1.0 })
        for (double w : { -7.0, -0.5, 0.3, 4.0, 25.0 })
        {
            const double h = 1.0e-4;
            const double derivative =
                (f.at (w + h, knob).integral - f.at (w - h, knob).integral) / (2.0 * h);
            CHECK (std::abs (derivative - f.at (w, knob).x) < 1.0e-6);
        }
}

TEST_CASE ("Transistor stage: anti-aliasing averages the curve over each step", "[pedals][bjt]")
{
    BjtCurve c;
    c.build (kShapes[0]);
    CurveAdaa adaa;
    const auto curve = [&c] (double w) {
        return c.at (w);
    };
    // A slow ramp: the output is the curve at the segment's midpoint (to second order).
    double w = -20.0;
    adaa.process (w, curve, true);
    for (int i = 0; i < 4000; ++i)
    {
        const double next = w + 0.01;
        const double y = adaa.process (next, curve, false);
        CHECK (std::abs (y - c.at (0.5 * (w + next)).x) < 1.0e-5);
        w = next;
    }
    // Silence stays exact.
    adaa.reset();
    for (int i = 0; i < 10; ++i)
        CHECK (adaa.process (0.0, curve, false) == 0.0f);
}
