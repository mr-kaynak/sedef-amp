// SPDX-License-Identifier: AGPL-3.0-or-later
#include "sedef/dsp/Urchin.h"

#include "sedef/core/Math.h"

#include <algorithm>
#include <cmath>

namespace sedef::dsp
{
namespace
{
// Input stage and transformer. A small interstage transformer passes neither deep lows nor
// much above the upper mids; the band limit also keeps the rectifier's input close to a sine,
// which is what makes a clean octave. 1.0 full scale = 1 V at the jack (docs/dsp/amps.md, sec. 4).
constexpr double kInputHpHz = 40.0;
constexpr double kTransformerHpHz = 90.0;
constexpr double kTransformerLpHz = 3500.0;
constexpr double kTransformerLpQ = 0.6;
constexpr float kPreGain = 4.0f; // +12 dB into the transformer

// Germanium diodes: a soft knee around 0.15 V. Below it they conduct on a square law (still an
// octave, but a quieter one: soft picking gives less octave), above it the output is |x| - knee.
constexpr double kRectifierKnee = 0.15;
constexpr float kOctaveGain = 2.0f; // the rectified octave about as loud as the plain signal
constexpr double kCouplingHz = 70.0;

// Output stage: -6..+34 dB into a transistor clipper that gives up earlier on one side.
constexpr float kFuzzMinDb = -6.0f;
constexpr float kFuzzRangeDb = 40.0f;
constexpr float kFuzzKnee = 0.5f;
constexpr double kNegativeKnee = 0.55;

constexpr double kDcBlockHz = 12.0;
constexpr float kToneDarkHz = 1200.0f;
constexpr float kToneBrightHz = 9000.0f;
constexpr double kToneQ = 0.6;
constexpr double kFizzHz = 11000.0;
constexpr double kFizzQ = 0.6;
constexpr float kLevelRangeDb = 40.0f; // level: -20..+20 dB

constexpr double kGainRampSeconds = 0.03;
constexpr double kFilterSmoothingSeconds = 0.02;

/// Full-wave rectifier in units of the diode knee: sqrt(z^2 + 1) - 1, written to stay exact for
/// small z, and its antiderivative.
inline double rectify (double z) noexcept
{
    return z * z / (std::sqrt (z * z + 1.0) + 1.0);
}

inline double rectifyIntegral (double z) noexcept
{
    // (z sqrt(z^2 + 1) + asinh z) / 2 - z, with asinh |z| = log1p(|z| + z^2 / (sqrt(z^2 + 1) + 1))
    // sharing the square root (and exact for small z).
    const double a = std::abs (z);
    const double root = std::sqrt (a * a + 1.0);
    const double asinhA = std::log1p (a + a * a / (root + 1.0));
    const double integral = 0.5 * (a * root + asinhA) - a;
    return z < 0.0 ? -integral : integral; // odd: the rectifier itself is even
}

/// The output transistor: tanh above zero, an earlier, softer limit below (even harmonics).
inline double fuzzCurve (double z) noexcept
{
    return z >= 0.0 ? std::tanh (z) : kNegativeKnee * std::tanh (z / kNegativeKnee);
}

inline double fuzzCurveIntegral (double z) noexcept
{
    return z >= 0.0 ? detail::logCosh (z)
                    : kNegativeKnee * kNegativeKnee * detail::logCosh (z / kNegativeKnee);
}

/// Output trim (dB): measured makeup for the reference tone (see docs/dsp/urchin.md), fitted
/// over Octave 0, 0.5 and 1 so that level 0.5 is within 1.5 dB of unity loudness for any fuzz.
float trimDb (float fuzz) noexcept
{
    return -(1.243f + fuzz * (17.867f + fuzz * (-20.255f + 7.816f * fuzz)));
}
} // namespace

void Urchin::prepare (const ProcessSpec& spec)
{
    sampleRate_ = spec.sampleRate;
    numChannels_ = std::clamp (spec.numChannels, 1, kMaxChannels);
    maxBlockSize_ = std::max (spec.maxBlockSize, 1);
    oversampler_.prepare (maxBlockSize_, numChannels_, Oversampler::factorLog2For (sampleRate_));

    const double osRate = sampleRate_ * oversampler_.factor();
    couplingPole_ = std::exp (-kTwoPiD * kCouplingHz / osRate);
    fuzzGain_.reset (osRate, kGainRampSeconds);
    octaveMix_.reset (osRate, kGainRampSeconds);
    outputGain_.reset (sampleRate_, kGainRampSeconds);
    tone_.reset (sampleRate_, kFilterSmoothingSeconds);

    inputHp_.setCoeffs (design::highpass1 (sampleRate_, kInputHpHz));
    transformerHp_.setCoeffs (design::highpass1 (sampleRate_, kTransformerHpHz));
    transformerLp_.setCoeffs (design::lowpass (sampleRate_, kTransformerLpHz, kTransformerLpQ));
    dcBlock_.setCoeffs (design::highpass1 (sampleRate_, kDcBlockHz));
    fizzLp_.setCoeffs (design::lowpass (sampleRate_, kFizzHz, kFizzQ));

    applySettings (true);
    reset();
}

void Urchin::reset() noexcept
{
    oversampler_.reset();
    for (auto* group : { &rectifier_, &clipper_ })
        for (auto& a : *group)
            a.reset();
    coupling_ = {};
    for (auto* f : { &inputHp_, &transformerHp_, &transformerLp_, &dcBlock_, &toneLp_, &fizzLp_ })
        f->reset();
    primed_ = false;
}

void Urchin::setSettings (const Settings& s) noexcept
{
    settings_ = s;
    applySettings (!primed_);
    primed_ = true;
}

int Urchin::latencySamples() const noexcept
{
    return oversampler_.latencySamples();
}

void Urchin::applySettings (bool snap) noexcept
{
    const float fuzz = std::clamp (settings_.fuzz, 0.0f, 1.0f);
    const float octave = std::clamp (settings_.octave, 0.0f, 1.0f);
    const float tone = std::clamp (settings_.tone, 0.0f, 1.0f);
    const float level = std::clamp (settings_.level, 0.0f, 1.0f);

    const float gain = dbToGain (kFuzzMinDb + kFuzzRangeDb * fuzz) / kFuzzKnee;
    const float output = kFuzzKnee * dbToGain (trimDb (fuzz) + kLevelRangeDb * (level - 0.5f));

    if (snap)
    {
        fuzzGain_.setCurrentAndTarget (gain);
        octaveMix_.setCurrentAndTarget (octave);
        outputGain_.setCurrentAndTarget (output);
        tone_.setCurrentAndTarget (tone);
        updateTone (tone);
    }
    else
    {
        fuzzGain_.setTarget (gain);
        octaveMix_.setTarget (octave);
        outputGain_.setTarget (output);
        tone_.setTarget (tone);
    }
}

void Urchin::updateTone (float tone) noexcept
{
    toneLp_.setCoeffs (
        design::lowpass (sampleRate_, mapExp (tone, kToneDarkHz, kToneBrightHz), kToneQ));
}

void Urchin::process (const AudioView& io) noexcept
{
    // The oversampler's buffers hold maxBlockSize frames; split anything larger.
    if (maxBlockSize_ <= 0)
        return; // not prepared
    for (int start = 0; start < io.numFrames(); start += maxBlockSize_)
        processBlock (io.subView (start, std::min (maxBlockSize_, io.numFrames() - start)));
}

void Urchin::processBlock (const AudioView& io) noexcept
{
    const int n = io.numFrames();
    const int channels = std::min (io.numChannels(), numChannels_);
    if (n == 0 || channels == 0)
        return;
    const AudioView view = io.withChannels (channels);

    // 1. Base rate: input stage and transformer, in place.
    for (int ch = 0; ch < channels; ++ch)
    {
        float* d = view.channel (ch);
        for (int i = 0; i < n; ++i)
        {
            float x = inputHp_.processSample (ch, d[i]);
            x = transformerHp_.processSample (ch, x);
            d[i] = kPreGain * transformerLp_.processSample (ch, x);
        }
    }

    // 2. Oversampled and anti-aliased: rectifier, coupling, octave blend and output stage.
    const AudioView up = oversampler_.upsample (view);
    const double pole = couplingPole_;
    for (int i = 0; i < up.numFrames(); ++i)
    {
        const float g = fuzzGain_.next();
        const float mix = octaveMix_.next();
        for (int ch = 0; ch < channels; ++ch)
        {
            const auto c = static_cast<size_t> (ch);
            float& s = up.channel (ch)[i];
            const float p = s;

            const double rectified =
                kRectifierKnee * rectifier_[c].process (static_cast<float> (p / kRectifierKnee),
                                                        rectify, rectifyIntegral);
            auto& k = coupling_[c];
            const double h1 = rectified - k.x1 + pole * k.y1;
            k.x1 = rectified;
            k.y1 = h1;
            const double h2 = h1 - k.x2 + pole * k.y2;
            k.x2 = h1;
            k.y2 = std::abs (h2) < 1.0e-20 ? 0.0 : h2;
            const auto octave = static_cast<float> (h2) * kOctaveGain;

            const float blend = p + mix * (octave - p);
            s = clipper_[c].process (g * blend, fuzzCurve, fuzzCurveIntegral);
        }
    }
    oversampler_.downsample (view);

    // 3. Base rate: coupling, tone and level.
    for (int start = 0; start < n; start += kControlBlock)
    {
        const int end = std::min (n, start + kControlBlock);
        if (tone_.isSmoothing())
            updateTone (tone_.skip (end - start));

        for (int i = start; i < end; ++i)
        {
            const float out = outputGain_.next();
            for (int ch = 0; ch < channels; ++ch)
            {
                float& x = view.channel (ch)[i];
                float y = dcBlock_.processSample (ch, x);
                y = toneLp_.processSample (ch, y);
                y = fizzLp_.processSample (ch, y);
                x = out * y;
            }
        }
    }
}

} // namespace sedef::dsp
