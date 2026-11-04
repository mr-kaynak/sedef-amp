// SPDX-License-Identifier: AGPL-3.0-or-later
#include "sedef/core/Oversampler.h"

#include "sedef/core/Math.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>

namespace sedef
{
namespace
{
double besselI0 (double x) noexcept
{
    double sum = 1.0, term = 1.0;
    const double halfX = 0.5 * x;
    for (int k = 1; k < 64; ++k)
    {
        term *= (halfX / k) * (halfX / k);
        sum += term;
        if (term < sum * 1.0e-17)
            break;
    }
    return sum;
}
} // namespace

void HalfBandStage::design (int numTaps, double beta)
{
    assert (numTaps % 2 == 1);
    numTaps_ = numTaps;
    centre_ = (numTaps - 1) / 2;
    taps_.assign (static_cast<size_t> (numTaps), 0.0);

    const double i0Beta = besselI0 (beta);
    for (int n = 0; n < numTaps; ++n)
    {
        const int k = n - centre_;
        double sinc = 0.5;
        if (k != 0)
        {
            if (k % 2 == 0)
                continue; // exact zero of the half-band response
            const double x = kPiD * k * 0.5;
            sinc = 0.5 * std::sin (x) / x;
        }
        const double r = 2.0 * n / (numTaps - 1) - 1.0;
        const double window = besselI0 (beta * std::sqrt (std::max (0.0, 1.0 - r * r))) / i0Beta;
        taps_[static_cast<size_t> (n)] = sinc * window;
    }

    // Normalise to exactly unity DC gain.
    double sum = 0.0;
    for (double t : taps_)
        sum += t;
    for (double& t : taps_)
        t /= sum;

    for (auto& p : phaseTaps_)
        p.clear();
    for (int n = 0; n < numTaps; ++n)
        if (taps_[static_cast<size_t> (n)] != 0.0)
            phaseTaps_[static_cast<size_t> (n & 1)].push_back (
                { n, static_cast<float> (taps_[static_cast<size_t> (n)]) });
}

void HalfBandStage::prepare (int maxInputFrames, int numChannels)
{
    // History buffers hold (taps + 2 * frames) samples so a whole block can be appended and read
    // linearly; they are compacted at the start of each call.
    const auto upSize = static_cast<size_t> (numTaps_ + maxInputFrames + 1);
    const auto downSize = static_cast<size_t> (numTaps_ + 2 * maxInputFrames + 1);
    for (int c = 0; c < kMaxChannels; ++c)
    {
        upHistory_[static_cast<size_t> (c)].data.assign (c < numChannels ? upSize : 0, 0.0f);
        downHistory_[static_cast<size_t> (c)].data.assign (c < numChannels ? downSize : 0, 0.0f);
    }
    reset();
}

void HalfBandStage::reset() noexcept
{
    for (auto* group : { &upHistory_, &downHistory_ })
        for (auto& h : *group)
        {
            std::fill (h.data.begin(), h.data.end(), 0.0f);
            h.pos = numTaps_;
        }
}

void HalfBandStage::up (int channel, const float* in, float* out, int n) noexcept
{
    // Zero-stuffing puts x[m] at high-rate index 2m, so output 2m + p only sees taps i with
    // (i - p) even: y[2m + p] = 2 * sum h[i] * x[m - (i - p) / 2].
    auto& h = upHistory_[static_cast<size_t> (channel)];
    float* hist = h.data.data();
    std::memmove (hist, hist + h.pos - numTaps_, sizeof (float) * static_cast<size_t> (numTaps_));
    h.pos = numTaps_;
    std::memcpy (hist + h.pos, in, sizeof (float) * static_cast<size_t> (n));

    for (int m = 0; m < n; ++m)
    {
        const float* x = hist + h.pos + m; // x[0] is the newest input sample for this frame
        for (int p = 0; p < 2; ++p)
        {
            float acc = 0.0f;
            for (const auto& t : phaseTaps_[static_cast<size_t> (p)])
                acc += t.coeff * x[-((t.index - p) >> 1)];
            out[2 * m + p] = 2.0f * acc;
        }
    }
    h.pos += n;
}

void HalfBandStage::down (int channel, const float* in, float* out, int n) noexcept
{
    // y[m] = sum_i h[i] * u[2m - i], evaluated on non-zero taps only. Keeping the even phase makes
    // the up+down round trip an integer number of low-rate samples.
    auto& h = downHistory_[static_cast<size_t> (channel)];
    float* hist = h.data.data();
    std::memmove (hist, hist + h.pos - numTaps_, sizeof (float) * static_cast<size_t> (numTaps_));
    h.pos = numTaps_;
    std::memcpy (hist + h.pos, in, sizeof (float) * static_cast<size_t> (2 * n));

    for (int m = 0; m < n; ++m)
    {
        const float* u = hist + h.pos + 2 * m;
        float acc = 0.0f;
        for (const auto& phase : phaseTaps_)
            for (const auto& t : phase)
                acc += t.coeff * u[-t.index];
        out[m] = acc;
    }
    h.pos += 2 * n;
}

// -------------------------------------------------------------------------------------------------
// Minimum-phase polyphase IIR half-band.
//
// Elliptic half-band design in closed form. The transition band fixes the elliptic modulus k
// (passband edge at (1/4 - transition) fs); the nome q follows from the series of Abramowitz and
// Stegun 17.3.21; the all-pass coefficients come from the theta-function series of the Jacobi
// elliptic sn at the pole positions (Valenzuela and Constantinides 1983, "Digital signal
// processing schemes for efficient interpolation and decimation", IEE Proc. G 130 (6)). The same
// closed form is used by Laurent de Soras' public-domain (WTFPL) HIIR library, which served as a
// cross-check. docs/dsp/oversampling.md derives the group delay and lists the measurements.

namespace
{
struct Elliptic
{
    double k, q;
};

Elliptic ellipticFor (double transition) noexcept
{
    const double t = std::tan ((1.0 - 2.0 * transition) * kPiD * 0.25);
    const double k = t * t;
    const double kRoot = std::pow (1.0 - k * k, 0.25); // sqrt (k')
    const double e = 0.5 * (1.0 - kRoot) / (1.0 + kRoot);
    const double e4 = e * e * e * e;
    return { k, e * (1.0 + e4 * (2.0 + e4 * (15.0 + 150.0 * e4))) };
}

double coefficient (int index, const Elliptic& el, int order) noexcept
{
    const int c = index + 1;
    double num = 0.0;
    for (int i = 0, sign = 1;; ++i, sign = -sign)
    {
        const double term = std::pow (el.q, static_cast<double> (i * (i + 1))) *
                            std::sin ((2 * i + 1) * c * kPiD / order) * sign;
        num += term;
        if (std::abs (term) < 1.0e-100 || i > 100)
            break;
    }
    num *= std::pow (el.q, 0.25);
    double den = 0.5;
    for (int i = 1, sign = -1;; ++i, sign = -sign)
    {
        const double term = std::pow (el.q, static_cast<double> (i * i)) *
                            std::cos (2 * i * c * kPiD / order) * sign;
        den += term;
        if (std::abs (term) < 1.0e-100 || i > 100)
            break;
    }
    const double w = num / den;
    const double w2 = w * w;
    const double x = std::sqrt ((1.0 - w2 * el.k) * (1.0 - w2 / el.k)) / (1.0 + w2);
    return (1.0 - x) / (1.0 + x);
}
} // namespace

std::vector<double> AllpassHalfBand::coefficientsFor (int numCoefs, double transition)
{
    assert (numCoefs >= 1 && numCoefs <= kMaxCoefs);
    assert (transition > 0.0 && transition < 0.25);
    const auto el = ellipticFor (transition);
    const int order = 2 * numCoefs + 1;
    std::vector<double> coefs (static_cast<size_t> (numCoefs));
    for (int i = 0; i < numCoefs; ++i)
        coefs[static_cast<size_t> (i)] = coefficient (i, el, order);
    return coefs;
}

double AllpassHalfBand::attenuationFor (int numCoefs, double transition) noexcept
{
    const auto el = ellipticFor (transition);
    const double a = 4.0 * std::pow (el.q, (2 * numCoefs + 1) * 0.5);
    return -10.0 * std::log10 (a / (1.0 + a));
}

double AllpassHalfBand::groupDelayAtDc (const std::vector<double>& coefs) noexcept
{
    // Each section (a + z^-2) / (1 + a z^-2) delays DC by 2 (1 - a) / (1 + a) high-rate samples;
    // the two branches, one of them a sample late, average: 1/2 (tau0 + 1 + tau1).
    double sum = 0.5;
    for (double a : coefs)
        sum += (1.0 - a) / (1.0 + a);
    return sum;
}

void AllpassHalfBand::design (int numCoefs, double transition)
{
    coefs_ = coefficientsFor (numCoefs, transition);
    attenuationDb_ = attenuationFor (numCoefs, transition);
    transition_ = transition;
    branchSize_ = { 0, 0 };
    for (int i = 0; i < numCoefs; ++i)
    {
        const int b = i & 1;
        branchCoefs_[static_cast<size_t> (b)]
                    [static_cast<size_t> (branchSize_[static_cast<size_t> (b)]++)] =
                        static_cast<float> (coefs_[static_cast<size_t> (i)]);
    }
    reset();
}

void AllpassHalfBand::reset() noexcept
{
    state_ = {};
}

float AllpassHalfBand::run (int branch, Branch& s, float x) const noexcept
{
    // y[n] = a (x[n] - y[n-1]) + x[n-1] for each section in turn.
    const auto& a = branchCoefs_[static_cast<size_t> (branch)];
    const int m = branchSize_[static_cast<size_t> (branch)];
    float in = x;
    float inPrev = s.x1;
    for (int k = 0; k < m; ++k)
    {
        const auto kk = static_cast<size_t> (k);
        const float out = a[kk] * (in - s.y[kk]) + inPrev;
        inPrev = s.y[kk];
        s.y[kk] = out;
        in = out;
    }
    s.x1 = x;
    return in;
}

void AllpassHalfBand::flush (Channel& c) const noexcept
{
    // The sections are feedback paths: keep a decaying tail out of the denormal range even when
    // the host's flush-to-zero mode is off.
    for (auto* group : { &c.up, &c.down })
        for (auto& b : *group)
        {
            b.x1 = flushToZero (b.x1);
            for (auto& y : b.y)
                y = flushToZero (y);
        }
    c.downOdd = flushToZero (c.downOdd);
}

void AllpassHalfBand::up (int channel, const float* in, float* out, int n) noexcept
{
    // Interpolation by 2 H(z): the even outputs are branch 0 and the odd ones branch 1, both fed
    // the low-rate input (the 1/2 of H and the 2 of interpolation cancel).
    // The state is worked on in local copies: the output can't alias them, so it stays in
    // registers instead of being reloaded after every store.
    auto& c = state_[static_cast<size_t> (channel)];
    Branch b0 = c.up[0], b1 = c.up[1];
    for (int m = 0; m < n; ++m)
    {
        const float x = in[m];
        out[2 * m] = run (0, b0, x);
        out[2 * m + 1] = run (1, b1, x);
    }
    c.up = { b0, b1 };
    flush (c);
}

void AllpassHalfBand::down (int channel, const float* in, float* out, int n) noexcept
{
    // Decimation of H(z) u: y[m] = 1/2 (A0 u[2m] + A1 u[2m - 1]), so the round trip is H(z)^2
    // at the high rate and its group delay a whole number of high-rate samples apart from the
    // design's.
    auto& c = state_[static_cast<size_t> (channel)];
    Branch b0 = c.down[0], b1 = c.down[1];
    float odd1 = c.downOdd;
    for (int m = 0; m < n; ++m)
    {
        const float even = run (0, b0, in[2 * m]);
        const float odd = run (1, b1, odd1);
        odd1 = in[2 * m + 1];
        out[m] = 0.5f * (even + odd);
    }
    c.down = { b0, b1 };
    c.downOdd = odd1;
    flush (c);
}

// -------------------------------------------------------------------------------------------------

namespace
{
/// Both families pass 20 kHz flat at 44.1 kHz and keep the band above it; at 48 kHz and up the
/// passband reaches further. Normalised to the base rate.
constexpr double kPassband = 20000.0 / 44100.0;

/// Minimum-phase designs (docs/dsp/oversampling.md, "Designs"): coefficient counts, and the
/// whole-sample group delay at DC the round trip is tuned to.
constexpr int kStage0Coefs4x = 9; // 2x stage of the 4x cascade
constexpr int kStage1Coefs4x = 5; // 4x stage
constexpr int kLatency4x = 5;     // base-rate samples
constexpr int kStage0Coefs2x = 5; // the only stage at 2x
constexpr int kLatency2x = 3;     // base-rate samples
constexpr int kFirLatency[] = { 0, 31, 37 };
constexpr int kIirLatency[] = { 0, kLatency2x, kLatency4x };

/// The transition (by bisection; the delay grows with it) that makes `delayOf (transition)`
/// equal `target`.
template <typename DelayOf>
double transitionForDelay (DelayOf&& delayOf, double target) noexcept
{
    double lo = 1.0e-4, hi = 0.2499;
    for (int i = 0; i < 100; ++i)
    {
        const double mid = 0.5 * (lo + hi);
        (delayOf (mid) < target ? lo : hi) = mid;
    }
    return 0.5 * (lo + hi);
}
} // namespace

int Oversampler::factorLog2For (double sampleRate) noexcept
{
    if (sampleRate <= 50000.0)
        return 2;
    if (sampleRate <= 100000.0)
        return 1;
    return 0;
}

int Oversampler::latencyFor (int factorLog2, OversamplingMode mode) noexcept
{
    const auto i = static_cast<size_t> (std::clamp (factorLog2, 0, 2));
    return mode == OversamplingMode::linearPhase ? kFirLatency[i] : kIirLatency[i];
}

void Oversampler::prepare (int maxBlockSize, int numChannels, int factorLog2, OversamplingMode mode)
{
    assert (factorLog2 >= 0 && factorLog2 <= 2);
    factorLog2_ = factorLog2;
    numChannels_ = numChannels;
    mode_ = mode;

    if (mode == OversamplingMode::linearPhase)
    {
        // Stage 1 does the heavy lifting (passband to ~0.41 fs). Stage 2 only has to reject the
        // images of an already band-limited signal, so it can be much shorter. Tap counts are
        // chosen so the round-trip latency is a whole number of base-rate samples.
        stages_[0].design (63, 8.5);
        stages_[1].design (25, 7.0);
        stages_[0].prepare (maxBlockSize, numChannels);
        stages_[1].prepare (maxBlockSize * 2, numChannels);
    }
    else if (factorLog2 == 2)
    {
        // The 4x stage rejects the images and aliases of the band the 2x stage passes, from
        // 2 fs - kPassband fs up; the 2x stage's transition is then tuned so the round trip's
        // group delay at DC is exactly kLatency4x.
        iir_[1].design (kStage1Coefs4x, 0.25 * (1.0 - kPassband));
        const double stage1 = iir_[1].groupDelayAtDc() / 2.0; // base-rate samples
        const double t = transitionForDelay (
            [stage1] (double tr) {
                return AllpassHalfBand::groupDelayAtDc (
                           AllpassHalfBand::coefficientsFor (kStage0Coefs4x, tr)) +
                       stage1;
            },
            static_cast<double> (kLatency4x));
        iir_[0].design (kStage0Coefs4x, t);
    }
    else if (factorLog2 == 1)
    {
        const double t = transitionForDelay (
            [] (double tr) {
                return AllpassHalfBand::groupDelayAtDc (
                    AllpassHalfBand::coefficientsFor (kStage0Coefs2x, tr));
            },
            static_cast<double> (kLatency2x));
        iir_[0].design (kStage0Coefs2x, t);
    }

    for (int k = 0; k < 2; ++k)
        for (int c = 0; c < kMaxChannels; ++c)
            buffers_[static_cast<size_t> (k)][static_cast<size_t> (c)].assign (
                c < numChannels ? static_cast<size_t> (maxBlockSize << (k + 1)) : 0, 0.0f);

    latency_ = latencyFor (factorLog2, mode);
    reset();
}

double Oversampler::groupDelayAtDc() const noexcept
{
    if (mode_ == OversamplingMode::linearPhase || factorLog2_ == 0)
        return static_cast<double> (latency_);
    // Up and down through a stage at 2^s times the base rate: 2 * delay / 2^s base samples.
    double d = iir_[0].groupDelayAtDc();
    if (factorLog2_ == 2)
        d += iir_[1].groupDelayAtDc() / 2.0;
    return d;
}

void Oversampler::reset() noexcept
{
    for (auto& s : stages_)
        s.reset();
    for (auto& s : iir_)
        s.reset();
}

AudioView Oversampler::upsample (const AudioView& in) noexcept
{
    lastFrames_ = in.numFrames();
    const int channels = std::min (in.numChannels(), numChannels_);

    if (factorLog2_ == 0)
        return in;

    const bool fir = mode_ == OversamplingMode::linearPhase;
    for (int c = 0; c < channels; ++c)
    {
        auto& b0 = buffers_[0][static_cast<size_t> (c)];
        if (fir)
            stages_[0].up (c, in.channel (c), b0.data(), lastFrames_);
        else
            iir_[0].up (c, in.channel (c), b0.data(), lastFrames_);
        float* result = b0.data();
        if (factorLog2_ == 2)
        {
            auto& b1 = buffers_[1][static_cast<size_t> (c)];
            if (fir)
                stages_[1].up (c, b0.data(), b1.data(), lastFrames_ * 2);
            else
                iir_[1].up (c, b0.data(), b1.data(), lastFrames_ * 2);
            result = b1.data();
        }
        viewPtrs_[static_cast<size_t> (c)] = result;
    }
    return AudioView (viewPtrs_.data(), channels, lastFrames_ << factorLog2_);
}

void Oversampler::downsample (const AudioView& out) noexcept
{
    assert (out.numFrames() == lastFrames_);
    if (factorLog2_ == 0)
        return;

    const bool fir = mode_ == OversamplingMode::linearPhase;
    const int channels = std::min (out.numChannels(), numChannels_);
    for (int c = 0; c < channels; ++c)
    {
        auto& b0 = buffers_[0][static_cast<size_t> (c)];
        if (factorLog2_ == 2)
        {
            auto& b1 = buffers_[1][static_cast<size_t> (c)];
            if (fir)
                stages_[1].down (c, b1.data(), b0.data(), lastFrames_ * 2);
            else
                iir_[1].down (c, b1.data(), b0.data(), lastFrames_ * 2);
        }
        if (fir)
            stages_[0].down (c, b0.data(), out.channel (c), lastFrames_);
        else
            iir_[0].down (c, b0.data(), out.channel (c), lastFrames_);
    }
}

// -------------------------------------------------------------------------------------------------

void AlignedDryPath::prepare (int maxBlockSize, int numChannels, int factorLog2,
                              OversamplingMode mode)
{
    latency_ = Oversampler::latencyFor (factorLog2, mode);
    roundTrip_ = mode == OversamplingMode::lowLatency && factorLog2 > 0;
    if (roundTrip_)
        os_.prepare (maxBlockSize, numChannels, factorLog2, mode);
    for (auto& d : delay_)
        d.prepare (std::max (latency_, 1));
    reset();
}

void AlignedDryPath::reset() noexcept
{
    os_.reset();
    for (auto& d : delay_)
        d.reset();
}

void AlignedDryPath::process (const AudioView& io) noexcept
{
    if (roundTrip_)
    {
        os_.upsample (io);
        os_.downsample (io);
        return;
    }
    if (latency_ == 0)
        return;
    for (int c = 0; c < io.numChannels(); ++c)
    {
        auto& d = delay_[static_cast<size_t> (c)];
        float* x = io.channel (c);
        for (int i = 0; i < io.numFrames(); ++i)
        {
            d.push (x[i]);
            x[i] = d.read (latency_);
        }
    }
}

} // namespace sedef
