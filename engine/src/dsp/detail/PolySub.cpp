// SPDX-License-Identifier: AGPL-3.0-or-later
#include "sedef/dsp/detail/PolySub.h"

#include "sedef/core/Math.h"

#include <algorithm>
#include <cmath>

namespace sedef::dsp::detail
{
namespace
{
// Resampling: the bank runs at about 4 kHz.
constexpr double kBankHz = 4000.0;
constexpr double kAntiAliasHz = 1600.0; // 6th-order Butterworth
constexpr double kAntiAliasQ[] = { 0.5176381, 0.7071068, 1.9318517 };
constexpr double kSmoothHz = 1500.0; // 4th-order Butterworth after the linear interpolation
constexpr double kSmoothQ[] = { 0.5411961, 1.3065630 };

// Bands: 14 Hz wide (-3 dB, all three poles) from 50 Hz, a 60th of the centre above 840 Hz.
constexpr double kLowHz = 50.0;
constexpr double kHighHz = 1200.0;
constexpr double kMinWidthHz = 14.0;
constexpr double kWidthQ = 60.0;

// Widening on a pick or a drop: 4x, back to 1x with a 30 ms time constant.
constexpr float kWide = 4.0f;
constexpr double kNarrowSeconds = 0.03;
constexpr double kFastSeconds = 0.002;
constexpr double kSlowSeconds = 0.040;
constexpr float kPickRatio = 4.0f; // 2 ms power over 40 ms power
constexpr float kRearmRatio = 2.0f;
constexpr float kDropRatio = 1.0f / 16.0f;
constexpr float kDropRearmRatio = 0.5f;
constexpr float kMinDetectPower = 1.0e-8f; // -80 dBFS
constexpr double kRefractorySeconds = 0.05;
constexpr int kWidthUpdateSteps = 4; // 1 ms
constexpr float kWidthTolerance = 0.02f;

// Owners.
constexpr float kKeep = 0.5f;              // an owner hands over when a neighbour is 3 dB stronger
constexpr float kRadiusScale = 0.7f;       // owners closer than this many (widened) bands merge
constexpr float kRelativeFloor = 1.0e-5f;  // -50 dB under the strongest band
constexpr float kAbsoluteFloor = 1.0e-10f; // band amplitude 1e-5: a -94 dBFS sine
constexpr double kFadeSeconds = 0.003;
constexpr float kMinResponse = 0.8f; // level correction is at most +1.9 dB

constexpr int kFlushSteps = 8; // a multiple of kWidthUpdateSteps
static_assert (kFlushSteps % kWidthUpdateSteps == 0);

/// The principal square root of a unit complex number.
inline void unitSqrt (float re, float im, float& outRe, float& outIm) noexcept
{
    outRe = std::sqrt (std::max (0.0f, 0.5f * (1.0f + re)));
    outIm = std::copysign (std::sqrt (std::max (0.0f, 0.5f * (1.0f - re))), im);
}

/// Rotates (re, im) by the unit step (sr, si) and renormalises.
inline void rotate (float& re, float& im, float sr, float si) noexcept
{
    const float nr = re * sr - im * si;
    const float ni = re * si + im * sr;
    const float norm = 1.0f / std::sqrt (nr * nr + ni * ni);
    re = nr * norm;
    im = ni * norm;
}
} // namespace

void PolySub::prepare (double sampleRate)
{
    sampleRate_ = sampleRate;
    factor_ = std::max (1, static_cast<int> (std::lround (sampleRate / kBankHz)));
    bankRate_ = sampleRate / factor_;

    for (size_t s = 0; s < antiAlias_.size(); ++s)
        antiAlias_[s].c = design::lowpass (sampleRate_, kAntiAliasHz, kAntiAliasQ[s]);
    for (size_t s = 0; s < smooth1_.size(); ++s)
        smooth1_[s].c = smooth2_[s].c = design::lowpass (sampleRate_, kSmoothHz, kSmoothQ[s]);

    // Three cascaded poles of equal width w have an overall -3 dB width of w * sqrt (2^(1/3) - 1).
    const double perPole = 1.0 / std::sqrt (std::exp2 (1.0 / kOrder) - 1.0);
    centre_.clear();
    damping_.clear();
    const double top = std::min (kHighHz, 0.4 * bankRate_);
    for (double f = kLowHz; f <= top;)
    {
        const double width = std::max (kMinWidthHz, f / kWidthQ);
        centre_.push_back (static_cast<float> (f));
        damping_.push_back (static_cast<float> (kPiD * width * perPole / bankRate_));
        f += width;
    }
    const size_t n = centre_.size();
    for (auto* v :
         { &cosW_, &sinW_, &zeroRe_, &zeroIm_, &poleRe_, &poleIm_, &gain_, &radius_, &lastRe_,
           &lastIm_, &power_, &halfRe_, &halfIm_, &quarterRe_, &quarterIm_, &fade_ })
        v->assign (n, 0.0f);
    for (auto& v : re_)
        v.assign (n, 0.0f);
    for (auto& v : im_)
        v.assign (n, 0.0f);
    owner_.assign (n, 0);
    for (size_t k = 0; k < n; ++k)
    {
        const double w = kTwoPiD * centre_[k] / bankRate_;
        cosW_[k] = static_cast<float> (std::cos (w));
        sinW_[k] = static_cast<float> (std::sin (w));
        // The first pole gets a zero at -f_c (the negative-frequency image of the partial), as
        // (1 - e^{-i w_c} z^-1); its gain at +f_c is 1 - e^{-2 i w_c}, divided out here.
        const double zr = 1.0 - std::cos (2.0 * w), zi = std::sin (2.0 * w);
        const double m2 = zr * zr + zi * zi;
        zeroRe_[k] = static_cast<float> (zr / m2);
        zeroIm_[k] = static_cast<float> (-zi / m2);
    }

    fastCoeff_ = static_cast<float> (1.0 - std::exp (-1.0 / (kFastSeconds * bankRate_)));
    slowCoeff_ = static_cast<float> (1.0 - std::exp (-1.0 / (kSlowSeconds * bankRate_)));
    narrowCoeff_ = static_cast<float> (std::exp (-1.0 / (kNarrowSeconds * bankRate_)));
    fadeStep_ = static_cast<float> (1.0 / (kFadeSeconds * bankRate_));
    refractorySteps_ = static_cast<int> (std::lround (kRefractorySeconds * bankRate_));
    reset();
}

void PolySub::reset() noexcept
{
    for (auto& section : antiAlias_)
        section.clear();
    for (auto* s : { &smooth1_, &smooth2_ })
        for (auto& section : *s)
            section.clear();
    previous1_ = previous2_ = current1_ = current2_ = 0.0f;
    phase_ = 0;
    for (auto& v : re_)
        std::fill (v.begin(), v.end(), 0.0f);
    for (auto& v : im_)
        std::fill (v.begin(), v.end(), 0.0f);
    for (auto* v : { &lastRe_, &lastIm_, &power_, &halfIm_, &quarterIm_, &fade_ })
        std::fill (v->begin(), v->end(), 0.0f);
    std::fill (halfRe_.begin(), halfRe_.end(), 1.0f);
    std::fill (quarterRe_.begin(), quarterRe_.end(), 1.0f);
    std::fill (owner_.begin(), owner_.end(), std::uint8_t { 0 });
    input_ = 0.0f;
    fast_ = slow_ = 0.0f;
    armed_ = true;
    dropArmed_ = true;
    widen_ = 1.0f;
    steps_ = 0;
    refractory_ = 0;
    setWidth (1.0f);
}

int PolySub::partials() const noexcept
{
    return static_cast<int> (std::count (owner_.begin(), owner_.end(), std::uint8_t { 1 }));
}

void PolySub::setWidth (float factor) noexcept
{
    for (size_t k = 0; k < centre_.size(); ++k)
    {
        const float r = std::exp (-damping_[k] * factor);
        radius_[k] = r;
        gain_[k] = 1.0f - r;
        poleRe_[k] = r * cosW_[k];
        poleIm_[k] = r * sinW_[k];
    }
    width_ = factor;
}

void PolySub::process (float x, float& sub1, float& sub2) noexcept
{
    float a = x;
    for (auto& s : antiAlias_)
        a = s.process (a);
    if (++phase_ >= factor_)
    {
        phase_ = 0;
        previous1_ = current1_;
        previous2_ = current2_;
        step (a);
    }
    // Linear interpolation from the previous bank output to the latest (one bank step late),
    // then a low-pass for the images.
    const float t = static_cast<float> (phase_) / static_cast<float> (factor_);
    float y1 = previous1_ + t * (current1_ - previous1_);
    float y2 = previous2_ + t * (current2_ - previous2_);
    for (auto& s : smooth1_)
        y1 = s.process (y1);
    for (auto& s : smooth2_)
        y2 = s.process (y2);
    sub1 = y1;
    sub2 = y2;
}

void PolySub::step (float x) noexcept
{
    // 1. Picks and drops widen the bank; it narrows back by itself.
    const float x2 = x * x;
    fast_ += fastCoeff_ * (x2 - fast_);
    slow_ += slowCoeff_ * (x2 - slow_);
    // The 2 ms power ripples with the waveform of a low note, and the 40 ms power lags behind a
    // pick, so after one detection the detector rests for 50 ms.
    if (refractory_ > 0)
        --refractory_;
    if (armed_ && refractory_ == 0 && fast_ > kPickRatio * slow_ && fast_ > kMinDetectPower)
    {
        widen_ = kWide;
        armed_ = false;
        refractory_ = refractorySteps_;
    }
    else if (!armed_ && fast_ < kRearmRatio * slow_)
        armed_ = true;
    if (dropArmed_ && refractory_ == 0 && fast_ < kDropRatio * slow_ && slow_ > kMinDetectPower)
    {
        widen_ = kWide;
        dropArmed_ = false;
        refractory_ = refractorySteps_;
    }
    else if (!dropArmed_ && fast_ > kDropRearmRatio * slow_)
        dropArmed_ = true;
    widen_ = 1.0f + (widen_ - 1.0f) * narrowCoeff_;
    steps_ = (steps_ + 1) % kFlushSteps;
    if (steps_ % kWidthUpdateSteps == 0)
    {
        const float target = widen_ < 1.0f + kWidthTolerance ? 1.0f : widen_;
        if (std::abs (target - width_) > kWidthTolerance * width_)
            setWidth (target);
    }
    if (steps_ % kFlushSteps == 0)
        flushDenormals();

    // 2. The bank.
    const size_t n = centre_.size();
    const float xp = input_;
    input_ = x;
    float strongest = 0.0f;
    for (size_t k = 0; k < n; ++k)
    {
        // (x - e^{-i w_c} x[n-1]) times the zero's normalising gain, then three poles.
        const float dr = x - cosW_[k] * xp;
        const float di = sinW_[k] * xp;
        const float g = gain_[k], pr = poleRe_[k], pi = poleIm_[k];
        float inR = g * (zeroRe_[k] * dr - zeroIm_[k] * di);
        float inI = g * (zeroRe_[k] * di + zeroIm_[k] * dr);
        for (int o = 0; o < kOrder; ++o)
        {
            float& zr = re_[static_cast<size_t> (o)][k];
            float& zi = im_[static_cast<size_t> (o)][k];
            const float nr = pr * zr - pi * zi + inR;
            const float ni = pr * zi + pi * zr + inI;
            zr = nr;
            zi = ni;
            inR = g * nr;
            inI = g * ni;
        }
        const float zr = re_[kOrder - 1][k], zi = im_[kOrder - 1][k];
        power_[k] = zr * zr + zi * zi;
        strongest = std::max (strongest, power_[k]);
    }

    // 3. Which bands voice a partial.
    floor_ = std::max (kAbsoluteFloor, kRelativeFloor * strongest);
    assignOwners();

    // 4. Voices: the owners (and the bands fading out) follow their band's phase, halved and
    //    quartered.
    float y1 = 0.0f, y2 = 0.0f;
    for (size_t k = 0; k < n; ++k)
    {
        const float zr = re_[kOrder - 1][k], zi = im_[kOrder - 1][k];
        if (owner_[k] != 0 || fade_[k] > 0.0f)
        {
            const float cr = zr * lastRe_[k] + zi * lastIm_[k];
            const float ci = zi * lastRe_[k] - zr * lastIm_[k];
            const float m2 = cr * cr + ci * ci;
            float response = 1.0f;
            if (m2 > 1.0e-30f)
            {
                const float inv = 1.0f / std::sqrt (m2);
                const float ur = cr * inv, ui = ci * inv;
                float hr, hi, qr, qi;
                unitSqrt (ur, ui, hr, hi);
                unitSqrt (hr, hi, qr, qi);
                rotate (halfRe_[k], halfIm_[k], hr, hi);
                rotate (quarterRe_[k], quarterIm_[k], qr, qi);
                // The band's response at the partial's frequency (the step's angle).
                const float cosD = ur * cosW_[k] + ui * sinW_[k];
                const float r = radius_[k];
                const float pole2 = (1.0f - r) * (1.0f - r) / (1.0f - 2.0f * r * cosD + r * r);
                response = pole2 * std::sqrt (pole2);
            }
            fade_[k] =
                std::clamp (fade_[k] + (owner_[k] != 0 ? fadeStep_ : -fadeStep_), 0.0f, 1.0f);
            const float level =
                fade_[k] * std::sqrt (power_[k]) / std::max (kMinResponse, response);
            y1 += level * halfRe_[k];
            y2 += level * quarterRe_[k];
        }
        lastRe_[k] = zr;
        lastIm_[k] = zi;
    }

    // 5. Silent bands take the phase of their stronger neighbour (sweeping towards the peaks),
    //    so a band that becomes an owner starts in step with the partial's previous owner.
    for (size_t k = 1; k < n; ++k)
        if (owner_[k] == 0 && fade_[k] <= 0.0f &&
            power_[k - 1] >= (k + 1 < n ? power_[k + 1] : 0.0f))
        {
            halfRe_[k] = halfRe_[k - 1];
            halfIm_[k] = halfIm_[k - 1];
            quarterRe_[k] = quarterRe_[k - 1];
            quarterIm_[k] = quarterIm_[k - 1];
        }
    for (size_t k = n - 1; k-- > 0;)
        if (owner_[k] == 0 && fade_[k] <= 0.0f && power_[k + 1] > (k > 0 ? power_[k - 1] : 0.0f))
        {
            halfRe_[k] = halfRe_[k + 1];
            halfIm_[k] = halfIm_[k + 1];
            quarterRe_[k] = quarterRe_[k + 1];
            quarterIm_[k] = quarterIm_[k + 1];
        }

    // An analytic band carries half the amplitude of a real partial.
    current1_ = 2.0f * y1;
    current2_ = 2.0f * y2;
}

void PolySub::assignOwners() noexcept
{
    const int n = static_cast<int> (centre_.size());
    const int reach = std::max (1, static_cast<int> (std::lround (kRadiusScale * width_)));
    const auto ownerNear = [&] (int k, bool stronger) {
        for (int j = std::max (0, k - reach); j <= std::min (n - 1, k + reach); ++j)
        {
            if (j == k || owner_[static_cast<size_t> (j)] == 0)
                continue;
            if (!stronger)
                return true;
            const float pj = power_[static_cast<size_t> (j)], pk = power_[static_cast<size_t> (k)];
            if (pj > pk || (pj == pk && j < k))
                return true;
        }
        return false;
    };

    // Owners keep their partial while they stay near its peak: an owner next to a stronger one
    // lets go (the same partial, seen twice while the bank was wide), and an owner whose
    // neighbour is clearly stronger hands phase and level over to it.
    for (int k = 0; k < n; ++k)
    {
        const auto i = static_cast<size_t> (k);
        if (owner_[i] == 0)
            continue;
        if (power_[i] <= floor_ || ownerNear (k, true))
        {
            owner_[i] = 0;
            continue;
        }
        int to = -1;
        for (int j : { k - 1, k + 1 })
            if (j >= 0 && j < n && kKeep * power_[static_cast<size_t> (j)] > power_[i] &&
                (to < 0 || power_[static_cast<size_t> (j)] > power_[static_cast<size_t> (to)]))
                to = j;
        if (to >= 0 && owner_[static_cast<size_t> (to)] == 0)
        {
            const auto t = static_cast<size_t> (to);
            halfRe_[t] = halfRe_[i];
            halfIm_[t] = halfIm_[i];
            quarterRe_[t] = quarterRe_[i];
            quarterIm_[t] = quarterIm_[i];
            fade_[t] = fade_[i];
            fade_[i] = 0.0f;
            owner_[i] = 0;
            owner_[t] = 1;
            if (to > k)
                ++k; // the new owner was judged already
        }
    }

    // A new partial: a strict peak, away from the band edges and from other owners.
    for (int k = 1; k + 1 < n; ++k)
    {
        const auto i = static_cast<size_t> (k);
        if (owner_[i] != 0)
            continue;
        if (power_[i] > power_[i - 1] && power_[i] >= power_[i + 1] && power_[i] > floor_ &&
            !ownerNear (k, false))
            owner_[i] = 1;
    }
}

void PolySub::flushDenormals() noexcept
{
    const auto flush = [] (float& v) {
        v = std::abs (v) < 1.0e-20f ? 0.0f : v;
    };
    for (auto* group : { &re_, &im_ })
        for (auto& v : *group)
            for (auto& s : v)
                flush (s);
    const auto flushSection = [&] (Section& section) {
        flush (section.s0);
        flush (section.s1);
    };
    for (auto& section : antiAlias_)
        flushSection (section);
    for (auto* s : { &smooth1_, &smooth2_ })
        for (auto& section : *s)
            flushSection (section);
    flush (fast_);
    flush (slow_);
    flush (input_);
}

} // namespace sedef::dsp::detail
