// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

namespace sedef::dsp::detail
{

/// Quasi-static common-emitter transistor stage with negative feedback, after the Ebers-Moll
/// junction law (Ebers & Moll, Proc. IRE 42(12), 1954), used by the fuzz and booster pedals.
///
/// The state is x, the change of collector current relative to its quiescent value Iq: x = -1 is
/// cutoff (no current left), x = xs is saturation (the collector has run out of voltage). The
/// drive w is the input in thermal-voltage units (v / (N * Vt)). The stage satisfies
///
///     w = U(x) = ln(1 + x) + g * x + W(x)
///
/// - ln(1 + x): the base-emitter junction, i = Iq * e^(vbe / (N Vt)); the current dies away
///   exponentially towards cutoff and grows exponentially the other way.
/// - g * x: the feedback (emitter degeneration, or the shunt feedback of a two-transistor pair).
///   g is the loop gain; a large g keeps the stage linear until it hits a limit, a small one leaves
///   the junction's exponential curve exposed.
/// - W(x) = kappa * xs * (atanh(x / xs) - x / xs) for x > 0: the saturation wall. It is flat near
///   zero (no effect on small signals) and makes x approach xs as w grows; a larger kappa is a
///   softer saturation knee.
///
/// U is strictly increasing, so the stage is a monotonic curve x(w) with slope 1 / (1 + g) at rest
/// and bounded output. In volts the stage gives Hc * x, where Hc = Rc * Iq is the cutoff headroom
/// and Hc * xs the saturation headroom.
///
/// x(w) has no closed form. solve() finds it with Newton's method in a variable that straightens
/// each side of the curve: q = ln(1 + x) towards cutoff, where U = q + g (e^q - 1) is nearly linear
/// in q for any drive, and r = atanh(x / xs) towards saturation, where U grows like kappa * xs * r.
/// Neither side then has a wall for Newton to overshoot into, and states arbitrarily close to
/// cutoff or saturation stay representable. The audio thread does not solve: BjtCurve tabulates
/// the curve once per shape, and BjtCurveFamily blends tabulated shapes along a knob.
///
/// The stage runs with first-order antiderivative anti-aliasing (Parker, Zavalishin & Le Bivic,
/// DAFx-16; Bilbao, Esqueda, Parker & Valimaki, IEEE SPL 24(7), 2017). An implicit curve still has
/// a closed-form antiderivative, from its explicit inverse (integration by parts):
///     integral_0^w x(s) ds = w * x - integral_0^x U(t) dt,
/// insensitive to small errors in x (its derivative with respect to x is w - U(x), the residual).
/// antiderivative() evaluates it; the tables integrate their own interpolant instead, and the
/// tests check the two against each other.
struct BjtShape
{
    double g = 1.0;     ///< loop gain, > 0
    double xs = 1.0;    ///< saturation headroom in units of the cutoff headroom, > 0
    double kappa = 1.0; ///< saturation wall strength, > 0
};

/// A solved point of the curve: the state, the straightened variable of its side, and values the
/// antiderivative reuses.
struct BjtPoint
{
    double x = 0.0;     ///< normalised collector current change, -1..xs
    double v = 0.0;     ///< q = ln(1 + x) if x <= 0, r = atanh(x / xs) if x > 0
    double ln1x = 0.0;  ///< ln(1 + x)
    double e = 1.0;     ///< e^(-2r) on the saturation side
    double slope = 1.0; ///< dU/dv there, for the next sample's tangent predictor
};

namespace bjt
{
inline constexpr double kLn2 = 0.6931471805599453;
// Newton converges quadratically: once a step is this small, the error left after it is of the
// order of its square, far below what the output (a float) can show. The last step is applied to
// the already evaluated functions to first order, which leaves an error of the same order.
inline constexpr double kTolerance = 1.0e-7;
inline constexpr int kMaxIterations = 40;

/// Solves U(x) = w for x <= 0 (w <= 0) in q = ln(1 + x): h(q) = q + g expm1(q) - w is convex and
/// increasing, so Newton from the right of the root converges monotonically, and a step from the
/// left lands on the right.
inline BjtPoint solveCutoffSide (const BjtShape& s, double w, double guess) noexcept
{
    // w / (1 + g) is always right of the root (expm1(q) >= q); a warm start may be on either side.
    double q = std::min (0.0, guess < 0.0 ? guess : w / (1.0 + s.g));
    double x = std::expm1 (q);
    double slope = 1.0 + s.g * (1.0 + x);
    for (int it = 0; it < kMaxIterations; ++it)
    {
        const double step = (q + s.g * x - w) / slope;
        if (std::abs (step) <= kTolerance * (1.0 + std::abs (q)))
        {
            q -= step;
            x -= (1.0 + x) * step;
            break;
        }
        q = std::min (0.0, q - step);
        x = std::expm1 (q);
        slope = 1.0 + s.g * (1.0 + x);
    }
    x = std::min (x, 0.0);
    q = std::min (q, 0.0);
    return { x, q, q, 1.0, slope };
}

/// Solves U(x) = w for x > 0 (w > 0) in r = atanh(x / xs), x = xs tanh(r), with the root bracketed
/// in [0, 1 + w / (kappa xs)] (there U >= kappa xs (r - 1) >= w) and bisection as the safeguard.
/// tanh comes from e = e^(-2r), which also gives 1 - tanh(r) = 2e / (1 + e) without cancellation.
inline BjtPoint solveSaturationSide (const BjtShape& s, double w, double guess) noexcept
{
    double lo = 0.0;
    double hi = 1.0 + w / (s.kappa * s.xs);
    double r = guess > 0.0 && guess < hi
                   ? guess
                   : std::min (hi, std::atanh (std::min (0.5, w / ((1.0 + s.g) * s.xs))));
    BjtPoint p;
    for (int it = 0; it < kMaxIterations; ++it)
    {
        const double e = std::exp (-2.0 * r);
        const double t = (1.0 - e) / (1.0 + e);
        const double sech2 = 4.0 * e / ((1.0 + e) * (1.0 + e));
        const double x = s.xs * t;
        const double ln1x = std::log1p (x);
        const double h = ln1x + s.g * x + s.kappa * s.xs * (r - t) - w;
        const double slope = s.xs * (sech2 * (1.0 / (1.0 + x) + s.g - s.kappa) + s.kappa);
        const double step = h / slope;
        p = { x, r, ln1x, e, slope };
        if (std::abs (step) <= kTolerance * (1.0 + r) || it + 1 == kMaxIterations)
        {
            const double dx = -s.xs * sech2 * step;
            p.v = std::max (r - step, 0.0);
            p.x = std::clamp (x + dx, 0.0, s.xs);
            p.ln1x = ln1x + dx / (1.0 + x);
            p.e = e * (1.0 + 2.0 * step);
            break;
        }
        if (h < 0.0)
            lo = r;
        else
            hi = r;
        r -= step;
        if (!(r > lo && r < hi))
            r = 0.5 * (lo + hi);
    }
    return p;
}

/// x(w), warm-started from the previous point (at drive wPrevious): Newton starts on the tangent
/// there, which for a smooth, oversampled signal is usually within the tolerance after one step.
inline BjtPoint solve (const BjtShape& s, double w, const BjtPoint& previous,
                       double wPrevious) noexcept
{
    const double predicted = previous.v + (w - wPrevious) / previous.slope;
    if (w <= 0.0)
        return solveCutoffSide (s, w, previous.x <= 0.0 ? std::min (predicted, -1.0e-300) : 1.0);
    return solveSaturationSide (s, w, previous.x > 0.0 ? std::max (predicted, 1.0e-300) : -1.0);
}

/// integral_0^w x(s) ds at a solved point.
inline double antiderivative (const BjtShape& s, double w, const BjtPoint& p) noexcept
{
    const double x = p.x;
    // integral of ln(1 + x) + g x = (1 + x) ln(1 + x) - x + g x^2 / 2
    double integral = (1.0 + x) * p.ln1x - x + 0.5 * s.g * x * x;
    if (x > 0.0)
    {
        // integral of kappa xs (atanh(t) - t), t = x / xs = tanh(r):
        //   kappa xs^2 (((1 + t) ln(1 + t) + (1 - t) ln(1 - t)) / 2 - t^2 / 2)
        // with 1 - t = 2e / (1 + e) and ln(1 - t) = ln 2 - 2r - ln(1 + e), no cancellation.
        const double e = p.e;
        const double t = (1.0 - e) / (1.0 + e);
        const double oneMinusT = 2.0 * e / (1.0 + e);
        const double lnOneMinusT = kLn2 - 2.0 * p.v - std::log1p (e);
        const double wall =
            0.5 * ((1.0 + t) * std::log1p (t) + oneMinusT * lnOneMinusT) - 0.5 * t * t;
        integral += s.kappa * s.xs * s.xs * wall;
    }
    return w * x - integral;
}
} // namespace bjt

/// One curve x(w), tabulated for the audio thread: solving U(x) = w costs a few transcendental
/// functions per Newton step, too much at four times the sample rate. Nodes are spaced uniformly in
/// u = w / (c + |w|), dense around the knees and sparse where the curve has gone flat; c is where
/// the stage's linear range would reach cutoff, (1 + g), or saturation, (1 + g) xs, on each side.
/// Between nodes x is a cubic Hermite through the exact
/// values and slopes (dx/dw = 1 / U'(x)). The running integral is that of the interpolant itself,
/// exactly, so antiderivative anti-aliasing on the table is consistent to rounding. Beyond the
/// range the curve is flat to within 1e-9 and continues as a constant.
class BjtCurve
{
public:
    static constexpr int kNodes = 768;

    struct Value
    {
        double x = 0.0;        ///< x(w)
        double integral = 0.0; ///< integral of x from the first node to w (any constant works)
    };

    /// Not real-time safe (allocates and solves kNodes points).
    void build (const BjtShape& s)
    {
        negScale_ = 1.0 + s.g;
        posScale_ = (1.0 + s.g) * s.xs;
        // Where the curve is within 1e-9 of cutoff and of saturation.
        constexpr double edge = 1.0e-9;
        const double wLo = std::log (edge) + s.g * (edge - 1.0);
        const double rHi = std::atanh (1.0 - edge);
        const double tHi = 1.0 - edge;
        const double wHi =
            std::log1p (s.xs * tHi) + s.g * s.xs * tHi + s.kappa * s.xs * (rHi - tHi);
        // Half the nodes on each side, with one exactly at w = 0 so silence stays silence.
        invDuNeg_ = kZeroNode / -warp (wLo);
        invDuPos_ = (kNodes - 1 - kZeroNode) / warp (wHi);

        w_.assign (kNodes, 0.0);
        x_.assign (kNodes, 0.0);
        slope_.assign (kNodes, 0.0);
        sum_.assign (kNodes, 0.0);
        BjtPoint p;
        double wPrev = 0.0;
        for (int k = 0; k < kNodes; ++k)
        {
            const auto i = static_cast<size_t> (k);
            const double u =
                k < kZeroNode ? (k - kZeroNode) / invDuNeg_ : (k - kZeroNode) / invDuPos_;
            w_[i] = (u < 0.0 ? negScale_ : posScale_) * u / (1.0 - std::abs (u));
            p = k == kZeroNode ? BjtPoint { 0.0, 0.0, 0.0, 1.0, 1.0 + s.g }
                               : bjt::solve (s, w_[i], p, wPrev);
            wPrev = w_[i];
            x_[i] = p.x;
            slope_[i] = 1.0 / driveSlope (s, p.x);
            if (k > 0)
                sum_[i] = sum_[i - 1] + segmentIntegral (i - 1, 1.0);
        }
    }

    /// x(w) and its running integral. Real-time safe.
    Value at (double w) const noexcept
    {
        const double u = warp (w);
        const double pos = kZeroNode + u * (u < 0.0 ? invDuNeg_ : invDuPos_);
        if (!(pos > 0.0))
            return { x_.front(), x_.front() * (w - w_.front()) };
        if (pos >= kNodes - 1)
            return { x_.back(), sum_.back() + x_.back() * (w - w_.back()) };
        auto i = static_cast<size_t> (pos);
        // Rounding in the warp can put w just outside its segment; step to the right one.
        if (w < w_[i] && i > 0)
            --i;
        else if (w > w_[i + 1] && i + 2 < static_cast<size_t> (kNodes))
            ++i;
        const double h = w_[i + 1] - w_[i];
        const double t = std::clamp ((w - w_[i]) / h, 0.0, 1.0);
        const double t2 = t * t, t3 = t2 * t;
        const double x = (2.0 * t3 - 3.0 * t2 + 1.0) * x_[i] + (t3 - 2.0 * t2 + t) * h * slope_[i] +
                         (3.0 * t2 - 2.0 * t3) * x_[i + 1] + (t3 - t2) * h * slope_[i + 1];
        return { x, sum_[i] + segmentIntegral (i, t) };
    }

private:
    double warp (double w) const noexcept
    {
        return w / ((w < 0.0 ? negScale_ : posScale_) + std::abs (w));
    }

    /// dU/dx (the slope of the drive against the state).
    static double driveSlope (const BjtShape& s, double x) noexcept
    {
        double d = 1.0 / (1.0 + x) + s.g;
        if (x > 0.0)
        {
            const double z = x / s.xs;
            d += s.kappa * (1.0 / ((1.0 - z) * (1.0 + z)) - 1.0);
        }
        return d;
    }

    /// Integral of the Hermite interpolant over the first fraction t of segment i.
    double segmentIntegral (size_t i, double t) const noexcept
    {
        const double h = w_[i + 1] - w_[i];
        const double t2 = t * t, t3 = t2 * t, t4 = t3 * t;
        return h * ((t - t3 + 0.5 * t4) * x_[i] +
                    (0.5 * t2 - 2.0 / 3.0 * t3 + 0.25 * t4) * h * slope_[i] +
                    (t3 - 0.5 * t4) * x_[i + 1] + (0.25 * t4 - t3 / 3.0) * h * slope_[i + 1]);
    }

    static constexpr int kZeroNode = kNodes / 2;
    double negScale_ = 1.0, posScale_ = 1.0, invDuNeg_ = 1.0, invDuPos_ = 1.0;
    std::vector<double> w_, x_, slope_, sum_;
};

/// Curves for one knob on a uniform grid of it. Between grid points the two neighbouring curves
/// are blended, values and integrals alike, which keeps each blended pair consistent for
/// antiderivative anti-aliasing.
class BjtCurveFamily
{
public:
    /// Not real-time safe. shapeAt(knob) gives the shape for a knob value in 0..1.
    template <typename ShapeAt>
    void build (int count, ShapeAt&& shapeAt)
    {
        curves_.assign (static_cast<size_t> (std::max (count, 2)), BjtCurve {});
        for (size_t k = 0; k < curves_.size(); ++k)
            curves_[k].build (shapeAt (static_cast<double> (k) / (curves_.size() - 1)));
    }

    BjtCurve::Value at (double w, double knob) const noexcept
    {
        const double pos = std::clamp (knob, 0.0, 1.0) * static_cast<double> (curves_.size() - 1);
        const auto i = std::min (static_cast<size_t> (pos), curves_.size() - 2);
        const double a = pos - static_cast<double> (i);
        const auto lo = curves_[i].at (w);
        if (a <= 0.0)
            return lo;
        const auto hi = curves_[i + 1].at (w);
        return { lo.x + a * (hi.x - lo.x), lo.integral + a * (hi.integral - lo.integral) };
    }

private:
    std::vector<BjtCurve> curves_;
};

/// First-order antiderivative anti-aliasing over a tabulated curve: each output is x averaged over
/// the segment between the previous drive and this one (half a sample of delay at the rate it runs
/// at). `curve(w)` returns a BjtCurve::Value; pass curveChanged when it is not the curve of the
/// previous call, so both ends of the segment come from the same curve.
class CurveAdaa
{
public:
    void reset() noexcept
    {
        w1_ = 0.0;
        v1_ = {};
        primed_ = false;
    }

    template <typename Curve>
    float process (double w, Curve&& curve, bool curveChanged) noexcept
    {
        if (curveChanged || !primed_)
        {
            v1_ = curve (w1_);
            primed_ = true;
        }
        const BjtCurve::Value v = curve (w);
        const double dw = w - w1_;
        // A segment too short for the quotient to be accurate: the average is as good.
        const double y =
            std::abs (dw) > 1.0e-6 ? (v.integral - v1_.integral) / dw : 0.5 * (v.x + v1_.x);
        w1_ = w;
        v1_ = v;
        return static_cast<float> (y);
    }

private:
    double w1_ = 0.0;
    BjtCurve::Value v1_ {};
    bool primed_ = false;
};

} // namespace sedef::dsp::detail
