// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <cmath>

namespace sedef::dsp::detail
{

/// First-order antiderivative anti-aliasing for a memoryless nonlinearity f with antiderivative F
/// (F(0) == 0): y[n] = (F(x[n]) - F(x[n-1])) / (x[n] - x[n-1]), i.e. f averaged over the segment
/// between consecutive samples. It suppresses most of the aliasing that folds down to low
/// frequencies, on top of oversampling, at the cost of half a sample of delay at the rate it runs
/// at. Evaluated in double: F grows with |x| and the difference quotient would lose precision in
/// float. Parker, Zavalishin & Le Bivic, DAFx-16; Bilbao, Esqueda, Parker & Valimaki, IEEE SPL
/// 2017.
class Adaa1
{
public:
    void reset() noexcept { x1_ = f1_ = 0.0; }

    template <typename Fn, typename AntiFn>
    float process (float in, Fn&& f, AntiFn&& antiderivative) noexcept
    {
        const double x = in;
        const double fx = antiderivative (x);
        const double dx = x - x1_;
        // Near-equal samples: the quotient is ill-conditioned; use f at the midpoint instead.
        const double y = std::abs (dx) > 1.0e-6 ? (fx - f1_) / dx : f (0.5 * (x + x1_));
        x1_ = x;
        f1_ = fx;
        return static_cast<float> (y);
    }

private:
    double x1_ = 0.0;
    double f1_ = 0.0;
};

/// log(cosh(x)) without overflow; the antiderivative of tanh.
inline double logCosh (double x) noexcept
{
    constexpr double ln2 = 0.6931471805599453;
    const double a = std::abs (x);
    if (a > 18.0)
        return a - ln2; // the correction is below 1e-15
    return a + std::log1p (std::exp (-2.0 * a)) - ln2;
}

} // namespace sedef::dsp::detail
