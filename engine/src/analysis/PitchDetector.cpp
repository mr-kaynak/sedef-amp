// SPDX-License-Identifier: AGPL-3.0-or-later
#include "sedef/analysis/PitchDetector.h"

#include "sedef/core/Math.h"

#include <algorithm>
#include <cmath>
#include <pffft/pffft.h>

namespace sedef::analysis
{
namespace
{
/// Longest analysis window, in periods of minHz.
constexpr double kWindowPeriods = 8.0;
/// Lags are evaluated up to this fraction of the window, so a period is only found when the
/// window holds 2.5 of them (an overlap of at least 1.5 periods at that lag).
constexpr double kMaxLagFraction = 0.4;
/// MPM: the first key maximum reaching this fraction of the highest one is the period. Lower
/// values favour the shortest period (octave-high errors), higher ones its multiples.
constexpr double kKeyMaxThreshold = 0.9;
/// Below this clarity the window is treated as unpitched. It is this low so a note still reads
/// while the previous string rings on, when its clarity is about its share of the power.
constexpr double kMinClarity = 0.5;
/// Key maxima of a noise NSDF scale as 1 / sqrt (window); short windows must clear this many
/// times that (~0.64 at 10 ms of 12 kHz, where low-passed noise peaks near 0.48).
constexpr double kNoiseClarityScale = 7.0;
/// Tolerance on the [minHz, maxHz] range so notes right at the limits are not lost.
constexpr double kRangeTolerance = 1.03;

/// Precision stage: each sub-window spans at least this many periods. The Blackman-Harris main
/// lobe is +-4 bins wide, so the second partial (one bin per period away) lands in the sidelobes,
/// which are below -92 dB.
constexpr double kSubWindowPeriods = 5.5;
/// Longer windows use sub-windows of half their length: about as precise in white noise as a
/// third (longer offset, shorter windows) but with half the main-lobe width, so interference
/// close to the fundamental (mains hum, a neighbouring string) leaks far less.
constexpr double kSubWindowFraction = 0.5;
/// Minimum offset between the two sub-windows; the phase advance over it carries the frequency.
constexpr double kMinSpacingPeriods = 2.0;
/// The phase advance is only unambiguous while (frequency error) x (offset) < half a cycle. The
/// periodicity estimate can be several cents off, so an intermediate sub-window this many cycles
/// of the partial in unwraps first (tolerates 40 cents), and the long offset then refines.
constexpr double kUnwrapPeriods = 20.0;
/// A partial must hold this fraction of the sub-window's power (-25 dB) to be measured.
constexpr double kMinPartialPower = 0.003;
/// Length of the harmonic-support check, in periods: short enough that a periodicity estimate 2%
/// off still falls in the main lobe, long enough that a partial of another note 15% away does not.
constexpr double kSupportPeriods = 20.0;
/// The result may sit this far from the periodicity estimate (stiffness, interpolation).
constexpr double kMaxRefineCents = 25.0;

constexpr double kCentsPerOctave = 1200.0;
} // namespace

struct PitchDetector::Workspace
{
    struct Plan
    {
        int size = 0;
        PFFFT_Setup* setup = nullptr;
    };

    Workspace() = default;
    Workspace (const Workspace&) = delete;
    Workspace& operator= (const Workspace&) = delete;

    ~Workspace()
    {
        for (auto& p : plans)
            pffft_destroy_setup (p.setup);
        pffft_aligned_free (input);
        pffft_aligned_free (spectrum);
        pffft_aligned_free (work);
    }

    const Plan* planFor (int minSize) const noexcept
    {
        for (const auto& p : plans)
            if (p.size >= minSize)
                return &p;
        return nullptr;
    }

    std::vector<Plan> plans; ///< ascending sizes, ratio <= 1.5 between neighbours
    float* input = nullptr;
    float* spectrum = nullptr;
    float* work = nullptr;
};

void PitchDetector::WorkspaceDeleter::operator() (Workspace* w) const noexcept
{
    delete w;
}

void PitchDetector::prepare (double sampleRate, float minHz, float maxHz)
{
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 12000.0;
    maxHz_ = std::min (maxHz, static_cast<float> (0.45 * sampleRate_));
    minHz_ = std::clamp (minHz, 1.0f, 0.5f * maxHz_);

    maxLag_ = static_cast<int> (std::ceil (kRangeTolerance * sampleRate_ / minHz_)) + 1;
    window_ = static_cast<int> (std::ceil (kWindowPeriods * sampleRate_ / minHz_));
    nsdf_.assign (static_cast<size_t> (maxLag_) + 1, 0.0f);
    peaks_.clear();
    peaks_.reserve (static_cast<size_t> (maxLag_) / 2 + 2);

    // Linear (not circular) autocorrelation needs window + lags of zero-padded FFT length.
    const int maxSize = window_ + maxLag_ + 1;
    ws_.reset (new Workspace);
    for (int base = 64;; base *= 2)
    {
        for (int size : { base, base + base / 2 })
        {
            if (pffft_is_valid_size (size, PFFFT_REAL) == 0)
                continue;
            ws_->plans.push_back ({ size, pffft_new_setup (size, PFFFT_REAL) });
        }
        if (ws_->plans.back().size >= maxSize)
            break;
    }
    const auto bytes = sizeof (float) * static_cast<size_t> (ws_->plans.back().size);
    ws_->input = static_cast<float*> (pffft_aligned_malloc (bytes));
    ws_->spectrum = static_cast<float*> (pffft_aligned_malloc (bytes));
    ws_->work = static_cast<float*> (pffft_aligned_malloc (bytes));
}

PitchDetector::Result PitchDetector::detect (const float* frames, int numFrames)
{
    Result result;
    if (ws_ == nullptr)
        prepare (sampleRate_, minHz_, maxHz_);
    if (frames == nullptr || numFrames <= 0)
        return result;
    if (numFrames > window_)
    {
        frames += numFrames - window_;
        numFrames = window_;
    }
    const int n = numFrames;
    const double shortestPeriod = sampleRate_ / (kRangeTolerance * maxHz_);
    const double longestPeriod = kRangeTolerance * sampleRate_ / minHz_;
    const int lagEnd = std::min (maxLag_, static_cast<int> (kMaxLagFraction * n));
    if (lagEnd < static_cast<int> (shortestPeriod) + 2 || lagEnd < 3)
        return result;

    double energy = 0.0;
    for (int i = 0; i < n; ++i)
        energy += static_cast<double> (frames[i]) * frames[i];
    if (energy <= 1.0e-14 * n) // ~ -140 dBFS
        return result;

    // r'(tau) = sum_j x_j x_(j+tau) via the power spectrum of the zero-padded window.
    const auto* plan = ws_->planFor (n + lagEnd + 1);
    if (plan == nullptr)
        return result;
    const int fftSize = plan->size;
    std::copy (frames, frames + n, ws_->input);
    std::fill (ws_->input + n, ws_->input + fftSize, 0.0f);
    pffft_transform_ordered (plan->setup, ws_->input, ws_->spectrum, ws_->work, PFFFT_FORWARD);
    float* s = ws_->spectrum;
    s[0] *= s[0]; // DC and Nyquist share the first complex slot
    s[1] *= s[1];
    for (int k = 2; k < fftSize; k += 2)
    {
        s[k] = s[k] * s[k] + s[k + 1] * s[k + 1];
        s[k + 1] = 0.0f;
    }
    pffft_transform_ordered (plan->setup, s, ws_->input, ws_->work, PFFFT_BACKWARD);

    // NSDF n'(tau) = 2 r'(tau) / m'(tau), with m'(tau) = sum_j (x_j^2 + x_(j+tau)^2) updated
    // incrementally as the overlap shrinks.
    const double rScale = 2.0 / fftSize;
    double m = 2.0 * energy;
    for (int tau = 0; tau <= lagEnd; ++tau)
    {
        nsdf_[static_cast<size_t> (tau)] =
            m > 1.0e-20 ? static_cast<float> (rScale * ws_->input[tau] / m) : 0.0f;
        const double a = frames[tau], b = frames[n - 1 - tau];
        m -= a * a + b * b;
    }

    // Key maxima: the highest point of each positive lobe after the zero-lag lobe.
    const float* d = nsdf_.data();
    peaks_.clear();
    int tau = 1;
    while (tau <= lagEnd && d[tau] > 0.0f)
        ++tau;
    while (tau <= lagEnd)
    {
        while (tau <= lagEnd && d[tau] <= 0.0f)
            ++tau;
        if (tau > lagEnd)
            break;
        int best = tau;
        while (tau <= lagEnd && d[tau] > 0.0f)
        {
            if (d[tau] > d[best])
                best = tau;
            ++tau;
        }
        if (best >= lagEnd) // still rising at the end of the evaluated lags
            continue;

        const double y0 = d[best - 1], y1 = d[best], y2 = d[best + 1];
        const double den = y0 - 2.0 * y1 + y2;
        const double delta = den < 0.0 ? 0.5 * (y0 - y2) / den : 0.0;
        const Peak p { best + delta, y1 - 0.25 * (y0 - y2) * delta };
        if (p.lag <= longestPeriod)
            peaks_.push_back (p);
    }
    if (peaks_.empty())
        return result;

    // Periods shorter than the range still compete, so a tone above maxHz is rejected instead of
    // being reported at a subharmonic. Two strings ringing together (the previous one still
    // sounding while the next is plucked) are periodic at a common subharmonic with no energy at
    // its first two harmonics; such a period is skipped and the choice repeats among shorter ones.
    Peak chosen {};
    for (auto end = peaks_.size(); end > 0;)
    {
        double highest = 0.0;
        for (size_t i = 0; i < end; ++i)
            highest = std::max (highest, peaks_[i].value);
        size_t pick = 0;
        while (peaks_[pick].value < kKeyMaxThreshold * highest)
            ++pick;
        chosen = peaks_[pick];
        if (chosen.lag < shortestPeriod || hasHarmonicSupport (frames, n, chosen.lag))
            break;
        chosen = {};
        end = pick;
    }
    if (chosen.lag < shortestPeriod)
        return result;

    // Only trust a period once the window could also have shown a key maximum at twice it.
    if (2.0 * chosen.lag + 1.0 > lagEnd && 2.0 * chosen.lag <= longestPeriod)
        return result;

    result.clarity = static_cast<float> (std::min (chosen.value, 1.0));
    if (result.clarity < std::max (kMinClarity, kNoiseClarityScale / std::sqrt (n)))
        return result;

    double hz = sampleRate_ / chosen.lag;
    refineFundamental (frames, n, chosen.lag, hz);
    result.frequencyHz = static_cast<float> (hz);
    result.valid = true;
    return result;
}

bool PitchDetector::hasHarmonicSupport (const float* x, int n, double period) const noexcept
{
    // Hann-windowed demodulation at f and 2f over the last kSupportPeriods periods (the main lobe
    // is +-2 bins). A short window leaks more and so errs towards accepting.
    const int len = std::min (n, static_cast<int> (std::lround (kSupportPeriods * period)));
    x += n - len;
    n = len;
    const double w = kTwoPiD / period;
    const double dTheta = kTwoPiD / n;
    const double stepC = std::cos (dTheta), stepS = std::sin (dTheta);
    const double rotC = std::cos (w), rotS = std::sin (w);
    double thC = std::cos (0.5 * dTheta), thS = std::sin (0.5 * dTheta);
    double zRe = 1.0, zIm = 0.0;
    double re1 = 0.0, im1 = 0.0, re2 = 0.0, im2 = 0.0, energy = 0.0, sumW = 0.0, sumW2 = 0.0;
    for (int t = 0; t < n; ++t)
    {
        const double win = 0.5 - 0.5 * thC;
        const double v = win * x[t];
        re1 += v * zRe;
        im1 += v * zIm;
        re2 += v * (zRe * zRe - zIm * zIm);
        im2 += v * 2.0 * zRe * zIm;
        energy += v * v;
        sumW += win;
        sumW2 += win * win;

        const double nextC = thC * stepC - thS * stepS;
        thS = thS * stepC + thC * stepS;
        thC = nextC;
        const double nextRe = zRe * rotC + zIm * rotS;
        zIm = zIm * rotC - zRe * rotS;
        zRe = nextRe;
    }
    const double limit = kMinPartialPower * energy * sumW * sumW / (2.0 * sumW2);
    return re1 * re1 + im1 * im1 >= limit || re2 * re2 + im2 * im2 >= limit;
}

bool PitchDetector::refineFundamental (const float* x, int n, double period,
                                       double& hz) const noexcept
{
    const double periodicHz = sampleRate_ / period;
    const auto plausible = [&] (double f) {
        return f > 0.0 &&
               std::abs (kCentsPerOctave * std::log2 (f / periodicHz)) <= kMaxRefineCents;
    };

    const auto p1 = measurePartial (x, n, period, 1);
    if (p1.power >= kMinPartialPower)
    {
        if (!plausible (p1.hz))
            return false;
        hz = p1.hz;
        return true;
    }

    // Weak fundamental (bridge pickup, bass): a stiff string has f_n = n f0 sqrt (1 + B n^2), so
    // (f_n / n)^2 is linear in n^2 and partials 2 and 3 give the first one without knowing B:
    // f1^2 = 1.6 (f2 / 2)^2 - 0.6 (f3 / 3)^2.
    const auto p2 = measurePartial (x, n, period, 2);
    if (p2.power < kMinPartialPower)
        return false;
    const double u2 = (p2.hz / 2.0) * (p2.hz / 2.0);
    const auto p3 = measurePartial (x, n, period, 3);
    double f1 = std::sqrt (u2); // half the second partial: sharp by only ~1.5 B
    if (p3.power >= kMinPartialPower)
    {
        const double u1 = 1.6 * u2 - 0.6 * (p3.hz / 3.0) * (p3.hz / 3.0);
        f1 = u1 > 0.0 ? std::sqrt (u1) : 0.0;
    }
    if (!plausible (f1))
        return false;
    hz = f1;
    return true;
}

PitchDetector::Partial PitchDetector::measurePartial (const float* x, int n, double period,
                                                      int k) const noexcept
{
    // Demodulate partial k in equal sub-windows A = [0, len) and B = [spacing, n). For a partial
    // of frequency wk, c_B / c_A = exp (j wk spacing) whatever the demodulation frequency, window
    // shape or exponential decay, so the phase of c_B conj (c_A) is the partial's advance over
    // `spacing` samples. M (at `near`) resolves the cycle count first.
    Partial result;
    const int len = static_cast<int> (
        std::lround (std::max (kSubWindowPeriods * period, kSubWindowFraction * n)));
    const int spacing = n - len;
    if (len < 16 || spacing < kMinSpacingPeriods * period)
        return result;
    const int near =
        std::min (spacing, static_cast<int> (std::lround (kUnwrapPeriods * period / k)));

    const double w = k * kTwoPiD / period;
    const double dTheta = kTwoPiD / len;
    const double stepC = std::cos (dTheta), stepS = std::sin (dTheta);
    const double rotC = std::cos (w), rotS = std::sin (w);
    double thC = std::cos (0.5 * dTheta), thS = std::sin (0.5 * dTheta); // window phase
    double zRe = 1.0, zIm = 0.0;                                         // exp (-j w t)

    double aRe = 0.0, aIm = 0.0, bRe = 0.0, bIm = 0.0, mRe = 0.0, mIm = 0.0, aE = 0.0, bE = 0.0;
    double sumW = 0.0, sumW2 = 0.0;
    const float* xb = x + spacing;
    const float* xm = x + near;
    for (int t = 0; t < len; ++t)
    {
        // 4-term Blackman-Harris; cos 2th and cos 3th from cos th (Chebyshev).
        const double c = thC;
        const double win = 0.35875 - 0.48829 * c + 0.14128 * (2.0 * c * c - 1.0) -
                           0.01168 * c * (4.0 * c * c - 3.0);
        const double kRe = win * zRe, kIm = win * zIm;
        const double xa = x[t], xbv = xb[t];
        aRe += kRe * xa;
        aIm += kIm * xa;
        bRe += kRe * xbv;
        bIm += kIm * xbv;
        mRe += kRe * xm[t];
        mIm += kIm * xm[t];
        aE += win * win * xa * xa;
        bE += win * win * xbv * xbv;
        sumW += win;
        sumW2 += win * win;

        const double nextC = thC * stepC - thS * stepS;
        thS = thS * stepC + thC * stepS;
        thC = nextC;
        const double nextRe = zRe * rotC + zIm * rotS;
        zIm = zIm * rotC - zRe * rotS;
        zRe = nextRe;
    }
    if (aE <= 0.0 || bE <= 0.0)
        return result;

    // Fraction of each sub-window's power carried by the partial: (A^2 / 2) / mean square.
    const double toPower = 2.0 * sumW2 / (sumW * sumW);
    result.power =
        std::min (toPower * (aRe * aRe + aIm * aIm) / aE, toPower * (bRe * bRe + bIm * bIm) / bE);

    // Unwrap: the advance over `offset` is the wrapped phase plus the whole cycles that the
    // current estimate predicts.
    const auto unwrap = [] (double estimate, double re, double im, int offset) {
        const double phase = std::atan2 (im, re);
        return (phase + kTwoPiD * std::round ((estimate * offset - phase) / kTwoPiD)) / offset;
    };
    double wk = unwrap (w, mRe * aRe + mIm * aIm, mIm * aRe - mRe * aIm, near);
    wk = unwrap (wk, bRe * aRe + bIm * aIm, bIm * aRe - bRe * aIm, spacing);
    result.hz = sampleRate_ * wk / kTwoPiD;
    return result;
}

} // namespace sedef::analysis
