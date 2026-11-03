// SPDX-License-Identifier: AGPL-3.0-or-later
#include "sedef/dsp/Breaker.h"

#include "sedef/core/Math.h"
#include "sedef/dsp/detail/Bilinear.h"
#include "sedef/dsp/detail/Pickup.h"

#include <algorithm>
#include <cmath>

namespace sedef::dsp
{
namespace
{
using detail::Pickup;

// The transistor, normalised (detail/BjtStage.h). With a 10 kOhm collector load and about
// 0.24 mA of quiescent current the collector rests 2.4 V from cutoff and about 5.5 V from
// saturation (the clipped waveforms in Holmes, Holters & van Walstijn, DAFx-17, swing +5.5/-2.5 V).
constexpr double kCutoffHeadroom = 2.4;           // V
constexpr double kSaturationHeadroom = 5.5;       // V
constexpr double kThermalVoltage = 1.17 * 0.0255; // ideality x kT/q of a germanium transistor
constexpr double kOpenLoopGain = kCutoffHeadroom / kThermalVoltage; // gm * Rc, about 38 dB
constexpr double kSaturationKnee = 1.0;                             // germanium saturates softly

// Boost: the small-signal gain of the stage, linear in dB.
constexpr float kMinGainDb = 8.0f;
constexpr float kGainRangeDb = 28.0f; // boost 1: +36 dB, the emitter nearly fully bypassed

constexpr double kDcBlockHz = 16.0;      // 10 nF into an amp's 1 MOhm input
constexpr double kBandwidthHz = 12000.0; // the transistor's own roll-off

constexpr double kRangeFadeSeconds = 0.02;
constexpr double kBoostSmoothingSeconds = 0.02;

detail::BjtShape stageShape (double boost) noexcept
{
    // Small-signal gain = gm Rc / (1 + g): the emitter degeneration's loop gain g follows from the
    // gain Boost asks for.
    detail::BjtShape shape;
    shape.g = std::max (
        kOpenLoopGain / dbToGain (Breaker::stageGainDb (static_cast<float> (boost))) - 1.0, 0.0);
    shape.xs = kSaturationHeadroom / kCutoffHeadroom;
    shape.kappa = kSaturationKnee;
    return shape;
}

/// The stage's curves over the boost knob, shared by every instance and built on first use.
const detail::BjtCurveFamily& stageCurves()
{
    static const detail::BjtCurveFamily family = [] {
        detail::BjtCurveFamily f;
        f.build (33, stageShape);
        return f;
    }();
    return family;
}
} // namespace

Breaker::InputNetwork Breaker::inputNetwork (int range) noexcept
{
    // The pickup (R + sL), the input capacitor and the stage's input resistance in series: the
    // voltage across the resistance is a band-pass,
    //   H(s) = s C Rin / (s^2 L C + s C (R + Rin) + 1),
    // centred where the capacitor resonates with the pickup's inductance.
    const double c = range == Full ? kFullCapacitor : kTrebleCapacitor;
    const double l = Pickup::kInductance;
    InputNetwork net;
    net.numerator = { 0.0, c * kInputResistance, 0.0 };
    net.denominator = { 1.0, c * (Pickup::kResistance + kInputResistance), l * c };
    net.centreHz = 1.0 / (kTwoPiD * std::sqrt (l * c));
    return net;
}

float Breaker::stageGainDb (float boost) noexcept
{
    return kMinGainDb + kGainRangeDb * std::clamp (boost, 0.0f, 1.0f);
}

void Breaker::prepare (const ProcessSpec& spec)
{
    sampleRate_ = spec.sampleRate;
    numChannels_ = std::clamp (spec.numChannels, 1, kMaxChannels);
    maxBlockSize_ = std::max (spec.maxBlockSize, 1);

    oversampler_.prepare (maxBlockSize_, numChannels_, Oversampler::factorLog2For (sampleRate_));
    factor_ = oversampler_.factor();
    for (int ch = 0; ch < kMaxChannels; ++ch)
    {
        const auto c = static_cast<size_t> (ch);
        drive_[c].assign (ch < numChannels_ ? static_cast<size_t> (maxBlockSize_) : 0, 0.0f);
        drivePtrs_[c] = drive_[c].data();
    }
    boostAt_.assign (static_cast<size_t> (maxBlockSize_), 0.5f);
    curves_ = &stageCurves();

    rangeMix_.reset (sampleRate_, kRangeFadeSeconds);
    boostKnob_.reset (sampleRate_, kBoostSmoothingSeconds);

    for (int range : { Treble, Full })
    {
        const auto net = inputNetwork (range);
        (range == Treble ? trebleIn_ : fullIn_)
            .setCoeffs (
                detail::bilinear (net.numerator, net.denominator, sampleRate_, net.centreHz));
    }
    dcBlock_.setCoeffs (design::highpass1 (sampleRate_, kDcBlockHz));
    bandLp_.setCoeffs (design::lowpass1 (sampleRate_, kBandwidthHz));

    applySettings (true);
    reset();
}

void Breaker::reset() noexcept
{
    oversampler_.reset();
    for (auto& s : stage_)
        s.reset();
    stageBoost_ = -1.0f;
    for (auto* f : { &trebleIn_, &fullIn_, &dcBlock_, &bandLp_ })
        f->reset();
    primed_ = false;
}

void Breaker::setSettings (const Settings& s) noexcept
{
    settings_ = s;
    applySettings (!primed_);
    primed_ = true;
}

int Breaker::latencySamples() const noexcept
{
    return oversampler_.latencySamples();
}

void Breaker::applySettings (bool snap) noexcept
{
    const float boost = std::clamp (settings_.boost, 0.0f, 1.0f);
    const float mix = settings_.range == Full ? 1.0f : 0.0f;
    if (snap)
    {
        boostKnob_.setCurrentAndTarget (boost);
        rangeMix_.setCurrentAndTarget (mix);
    }
    else
    {
        boostKnob_.setTarget (boost);
        rangeMix_.setTarget (mix);
    }
}

void Breaker::process (const AudioView& io) noexcept
{
    // The scratch buffers hold maxBlockSize frames; split anything larger.
    if (maxBlockSize_ <= 0)
        return; // not prepared
    for (int start = 0; start < io.numFrames(); start += maxBlockSize_)
        processBlock (io.subView (start, std::min (maxBlockSize_, io.numFrames() - start)));
}

void Breaker::processBlock (const AudioView& io) noexcept
{
    const int n = io.numFrames();
    const int channels = std::min (io.numChannels(), numChannels_);
    if (n == 0 || channels == 0)
        return;
    const AudioView view = io.withChannels (channels);

    // 1. Base rate: the input network (both ranges, crossfaded), in thermal-voltage units.
    constexpr auto toDrive = static_cast<float> (1.0 / kThermalVoltage);
    for (int i = 0; i < n; ++i)
    {
        const float mix = rangeMix_.next();
        boostAt_[static_cast<size_t> (i)] = boostKnob_.next();
        for (int ch = 0; ch < channels; ++ch)
        {
            const float x = view.channel (ch)[i];
            const float treble = trebleIn_.processSample (ch, x);
            const float full = fullIn_.processSample (ch, x);
            drive_[static_cast<size_t> (ch)][static_cast<size_t> (i)] =
                toDrive * (treble + mix * (full - treble));
        }
    }

    // 2. Oversampled and anti-aliased: the transistor.
    const AudioView drive (drivePtrs_.data(), channels, n);
    const AudioView up = oversampler_.upsample (drive);
    for (int i = 0; i < n; ++i)
    {
        const float boost = boostAt_[static_cast<size_t> (i)];
        const bool moved = boost != stageBoost_;
        stageBoost_ = boost;
        const auto curve = [this, boost] (double w) {
            return curves_->at (w, boost);
        };
        for (int ch = 0; ch < channels; ++ch)
        {
            auto& stage = stage_[static_cast<size_t> (ch)];
            float* d = up.channel (ch) + i * factor_;
            for (int j = 0; j < factor_; ++j)
                d[j] = static_cast<float> (kCutoffHeadroom *
                                           stage.process (d[j], curve, moved && j == 0));
        }
    }
    oversampler_.downsample (drive);

    // 3. Base rate: output coupling and bandwidth. The booster's output is its point: no trim.
    for (int i = 0; i < n; ++i)
        for (int ch = 0; ch < channels; ++ch)
        {
            float y = drive_[static_cast<size_t> (ch)][static_cast<size_t> (i)];
            y = dcBlock_.processSample (ch, y);
            view.channel (ch)[i] = bandLp_.processSample (ch, y);
        }
}

} // namespace sedef::dsp
