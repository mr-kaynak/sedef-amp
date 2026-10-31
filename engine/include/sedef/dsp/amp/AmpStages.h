// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "sedef/core/AudioView.h"
#include "sedef/core/Biquad.h"
#include "sedef/core/Math.h"

#include <array>
#include <cmath>

/// Building blocks shared by the amp models (internal to the amp section). Signals are in
/// normalised units: a triode stage starts to bend audibly around |x| = 1.
namespace sedef::dsp::amp
{

/// One-pole smoothing increment for a time constant, for `y += k * (target - y)`.
inline float onePoleIncrement (double sampleRate, double ms) noexcept
{
    return ms > 0.0 ? static_cast<float> (1.0 - std::exp (-1.0 / (0.001 * ms * sampleRate))) : 1.0f;
}

/// First-order IIR section (TDF-II) with per-channel state. Takes a first-order design from
/// core::design (b2 = a2 = 0) at half the cost of a biquad.
class FirstOrder
{
public:
    void setCoeffs (const BiquadCoeffs& c) noexcept
    {
        b0_ = c.b0;
        b1_ = c.b1;
        a1_ = c.a1;
    }

    void reset() noexcept { s_ = {}; }

    float process (int ch, float x) noexcept
    {
        float& s = s_[static_cast<size_t> (ch)];
        const float y = b0_ * x + s;
        s = b1_ * x - a1_ * y;
        return y;
    }

    void snapToZero() noexcept
    {
        for (auto& s : s_)
            s = flushToZero (s);
    }

private:
    float b0_ = 1.0f, b1_ = 0.0f, a1_ = 0.0f;
    std::array<float, kMaxChannels> s_ {};
};

/// First-order shelf in parallel form: x + (g - 1) * onepole(x), with a fixed pole at `hz`.
/// A high shelf (one-pole high-pass inside) has its zero at hz / g, a low shelf (low-pass inside)
/// at g * hz. Only the mix gain moves when the shelf is adjusted, so it can be ramped per sample
/// without the transients of re-designing a recursive filter on the fly, which the high-gain
/// stages after it would amplify into audible ticks.
class MixShelf
{
public:
    void setHigh (double sampleRate, double hz) noexcept
    {
        f_.setCoeffs (design::highpass1 (sampleRate, hz));
    }
    void setLow (double sampleRate, double hz) noexcept
    {
        f_.setCoeffs (design::lowpass1 (sampleRate, hz));
    }
    void reset() noexcept { f_.reset(); }
    void snapToZero() noexcept { f_.snapToZero(); }

    /// g: linear gain of the shelf's far end (above hz for a high shelf, below for a low shelf).
    float process (int ch, float x, float g) noexcept
    {
        return x + (g - 1.0f) * f_.process (ch, x);
    }

private:
    FirstOrder f_;
};

/// Static triode characteristic with unity slope at zero. The positive (grid-conduction) side
/// bends softly towards `soft`; the negative (cutoff) side limits earlier and firmer at `hard`.
/// `bias` moves the operating point up the curve, which is where the small-signal second harmonic
/// of a real stage comes from.
struct TriodeCurve
{
    float soft = 1.6f;
    float hard = 1.0f;
    float bias = 0.3f;
};

/// A triode gain stage: static curve plus grid-conduction bias shift. Gain is applied by the
/// caller (the curve itself is unity-slope), filters around it model coupling and Miller effects.
class TriodeStage
{
public:
    /// shiftDepth: the most grid conduction can drag the bias (0 = static stage). Grid current
    /// flows once the input swings more than `conductAt` above rest. The attack is the coupling
    /// capacitor charging through the source and grid stopper; the release is it draining through
    /// the grid leak.
    void configure (const TriodeCurve& c, float shiftDepth, double sampleRate,
                    double attackMs = 1.5, double releaseMs = 60.0, float conductAt = 0.6f) noexcept
    {
        invSoft2_ = 1.0f / (c.soft * c.soft);
        hard_ = c.hard;
        invHard_ = 1.0f / c.hard;
        bias_ = c.bias;
        offset_ = shape (c.bias); // static operating point, removed so silence stays silent
        threshold_ = c.bias + conductAt;
        depth_ = shiftDepth;
        att_ = onePoleIncrement (sampleRate, attackMs);
        rel_ = onePoleIncrement (sampleRate, releaseMs);
    }

    void reset() noexcept { shift_ = {}; }

    float process (int ch, float x) noexcept
    {
        float& sh = shift_[static_cast<size_t> (ch)];
        const float u = x + bias_ - sh;
        const float y = shape (u) - offset_;
        // Positive grid swings charge the coupling capacitor and push the bias colder for a
        // while: the stage compresses and clips more asymmetrically (bloom). The shift saturates
        // at depth_, so a hard pick never chokes the decay that follows (blocking).
        const float excess = u > threshold_ ? u - threshold_ : 0.0f;
        const float target = depth_ * excess / (1.0f + excess);
        sh += (target > sh ? att_ : rel_) * (target - sh);
        return y;
    }

    void snapToZero() noexcept
    {
        for (auto& s : shift_)
            s = flushToZero (s);
    }

private:
    float shape (float u) const noexcept
    {
        if (u >= 0.0f)
            return u / std::sqrt (1.0f + u * u * invSoft2_);
        return hard_ * fastTanh (u * invHard_);
    }

    float invSoft2_ = 1.0f, hard_ = 1.0f, invHard_ = 1.0f, bias_ = 0.0f, offset_ = 0.0f;
    float threshold_ = 1.0f, depth_ = 0.0f, att_ = 1.0f, rel_ = 1.0f;
    std::array<float, kMaxChannels> shift_ {};
};

/// Cathode follower driving a tone stack: unity gain until the grid swings above the cathode, then
/// grid current softly limits the positive side. One-sided, so it adds even harmonics; the tone
/// stack after it has no DC path, so the offset it creates goes nowhere.
inline float cathodeFollower (float x, float knee, float invWidth2) noexcept
{
    if (x <= knee)
        return x;
    const float d = x - knee;
    return knee + d / std::sqrt (1.0f + d * d * invWidth2);
}

/// Push-pull power amp: symmetric clip with a firm but rounded knee, and supply sag. The supply
/// envelope follows the output; a sagging rail lowers the clip level (headroom) and slightly the
/// gain, which gives the compression and "bloom" of a tube rectifier.
class PowerAmp
{
public:
    struct Config
    {
        float sagDepth = 0.15f; ///< headroom lost at full output
        float sagGain = 0.05f;  ///< gain lost at full output
        double attackMs = 8.0, releaseMs = 150.0;
    };

    void configure (const Config& c, double sampleRate) noexcept
    {
        depth_ = c.sagDepth;
        gainDepth_ = c.sagGain;
        att_ = onePoleIncrement (sampleRate, c.attackMs);
        rel_ = onePoleIncrement (sampleRate, c.releaseMs);
    }

    void reset() noexcept { env_ = {}; }

    float process (int ch, float x) noexcept
    {
        float& e = env_[static_cast<size_t> (ch)];
        const float headroom = 1.0f - depth_ * e;
        const float u = x / headroom;
        const float u2 = u * u;
        // u / (1 + u^4)^(1/4): within 0.2 dB of linear up to half swing, then a quick, smooth knee.
        const float y = headroom * u / std::sqrt (std::sqrt (1.0f + u2 * u2));
        const float a = std::abs (y);
        e += (a > e ? att_ : rel_) * (a - e);
        return y * (1.0f - gainDepth_ * e);
    }

    void snapToZero() noexcept
    {
        for (auto& e : env_)
            e = flushToZero (e);
    }

private:
    float depth_ = 0.0f, gainDepth_ = 0.0f, att_ = 1.0f, rel_ = 1.0f;
    std::array<float, kMaxChannels> env_ {};
};

/// A per-chunk linear gain ramp: set the end value, then step once per sample.
struct GainRamp
{
    float current = 1.0f;
    float step = 0.0f;
    float target = 1.0f;

    void snap (float v) noexcept
    {
        current = target = v;
        step = 0.0f;
    }

    /// Starts a ramp from the previous end value to `v` over n samples.
    void begin (float v, int n) noexcept
    {
        current = target;
        target = v;
        step = n > 0 ? (target - current) / static_cast<float> (n) : 0.0f;
    }
};

} // namespace sedef::dsp::amp
