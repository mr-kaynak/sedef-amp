// SPDX-License-Identifier: AGPL-3.0-or-later
#include "sedef/dsp/Coral.h"

#include "sedef/core/Math.h"

#include <algorithm>
#include <cmath>

namespace sedef::dsp
{
namespace
{
// Tight pre-emphasis: low end removed and upper mids lifted before the gain, so palm mutes stay
// defined and the pick attack drives the clipper hardest.
constexpr double kInputHpHz = 100.0;
constexpr double kInputHpQ = 0.55;
constexpr double kEmphasisHz = 1000.0;
constexpr double kEmphasisDb = 6.0;
constexpr float kMinDriveDb = 3.0f; // + 3 dB of emphasis at 1 kHz: 6..50 dB
constexpr float kDriveRangeDb = 44.0f;
// Gain-bandwidth limit of the op-amp: the stage gets darker as its gain rises.
constexpr float kDriveLpLowGainHz = 18000.0f;
constexpr float kDriveLpHighGainHz = 7000.0f;

// Clipper, in units of the diode knee (full-scale units). The op-amp's output swing (slightly
// off-centre) bounds the diodes' slow rise above the knee.
constexpr float kKnee = 0.25f;
constexpr double kPositiveRail = 6.0;
constexpr double kNegativeRail = 5.4;
constexpr double kTail = 0.06;

constexpr double kDcBlockHz = 15.0;
constexpr float kCutBrightHz = 12000.0f; // cut 0
constexpr float kCutDarkHz = 1200.0f;    // cut 1
constexpr double kCutQ = 0.6;            // 12 dB/oct, no resonance: tames fizz without honk
constexpr double kFizzHz = 10000.0;
constexpr double kFizzQ = 0.6;
constexpr float kLevelRangeDb = 40.0f; // level: -20..+20 dB

constexpr double kGainRampSeconds = 0.03;
constexpr double kFilterSmoothingSeconds = 0.02;

// Diodes to ground behind the op-amp: a tanh knee at 1 plus a small tail slope (the diode law
// keeps rising slowly) that saturates at the rail. Slope 1 at zero.
inline double clipCurve (double r, double rail) noexcept
{
    return (1.0 - kTail) * std::tanh (r) + kTail * rail * std::tanh (r / rail);
}

inline double clipCurveIntegral (double r, double rail) noexcept
{
    return (1.0 - kTail) * detail::logCosh (r) + kTail * rail * rail * detail::logCosh (r / rail);
}

inline double clip (double r) noexcept
{
    return clipCurve (r, r >= 0.0 ? kPositiveRail : kNegativeRail);
}

inline double clipIntegral (double r) noexcept
{
    return clipCurveIntegral (r, r >= 0.0 ? kPositiveRail : kNegativeRail);
}

/// Output trim (dB): measured makeup for a typical single-coil level (see docs/dsp/coral.md), so
/// that level 0.5 is close to unity loudness across the whole gain range.
float trimDb (float gain) noexcept
{
    return -4.62f + 7.03f * std::exp (-gain / 0.22f);
}
} // namespace

void Coral::prepare (const ProcessSpec& spec)
{
    sampleRate_ = spec.sampleRate;
    numChannels_ = std::clamp (spec.numChannels, 1, kMaxChannels);
    maxBlockSize_ = std::max (spec.maxBlockSize, 1);
    oversampler_.prepare (spec.maxBlockSize, numChannels_,
                          Oversampler::factorLog2For (sampleRate_));

    driveGain_.reset (sampleRate_, kGainRampSeconds);
    outputGain_.reset (sampleRate_, kGainRampSeconds);
    gainKnob_.reset (sampleRate_, kFilterSmoothingSeconds);
    cut_.reset (sampleRate_, kFilterSmoothingSeconds);

    inputHp_.setCoeffs (design::highpass (sampleRate_, kInputHpHz, kInputHpQ));
    emphasis_.setCoeffs (design::highShelf1 (sampleRate_, kEmphasisHz, kEmphasisDb));
    dcBlock_.setCoeffs (design::highpass1 (sampleRate_, kDcBlockHz));
    fizzLp_.setCoeffs (design::lowpass (sampleRate_, kFizzHz, kFizzQ));

    applySettings (true);
    reset();
}

void Coral::reset() noexcept
{
    oversampler_.reset();
    for (auto& c : clipper_)
        c.reset();
    for (auto* f : {&inputHp_, &emphasis_, &driveLp_, &dcBlock_, &cutLp_, &fizzLp_})
        f->reset();
    primed_ = false;
}

void Coral::setSettings (const Settings& s) noexcept
{
    settings_ = s;
    applySettings (!primed_);
    primed_ = true;
}

int Coral::latencySamples() const noexcept
{
    return oversampler_.latencySamples();
}

void Coral::applySettings (bool snap) noexcept
{
    const float gain = std::clamp (settings_.gain, 0.0f, 1.0f);
    const float cut = std::clamp (settings_.cut, 0.0f, 1.0f);
    const float level = std::clamp (settings_.level, 0.0f, 1.0f);

    const float drive = dbToGain (kMinDriveDb + kDriveRangeDb * gain) / kKnee;
    const float output = kKnee * dbToGain (trimDb (gain) + kLevelRangeDb * (level - 0.5f));

    if (snap)
    {
        driveGain_.setCurrentAndTarget (drive);
        outputGain_.setCurrentAndTarget (output);
        gainKnob_.setCurrentAndTarget (gain);
        cut_.setCurrentAndTarget (cut);
        updateDriveBandwidth (gain);
        updateCut (cut);
    }
    else
    {
        driveGain_.setTarget (drive);
        outputGain_.setTarget (output);
        gainKnob_.setTarget (gain);
        cut_.setTarget (cut);
    }
}

void Coral::updateDriveBandwidth (float gain) noexcept
{
    driveLp_.setCoeffs (
        design::lowpass1 (sampleRate_, mapExp (gain, kDriveLpLowGainHz, kDriveLpHighGainHz)));
}

void Coral::updateCut (float cut) noexcept
{
    cutLp_.setCoeffs (design::lowpass (sampleRate_, mapExp (cut, kCutBrightHz, kCutDarkHz), kCutQ));
}

void Coral::process (const AudioView& io) noexcept
{
    // The scratch buffers hold maxBlockSize frames; split anything larger.
    if (maxBlockSize_ <= 0)
        return; // not prepared
    for (int start = 0; start < io.numFrames(); start += maxBlockSize_)
        processBlock (io.subView (start, std::min (maxBlockSize_, io.numFrames() - start)));
}

void Coral::processBlock (const AudioView& io) noexcept
{
    const int n = io.numFrames();
    const int channels = std::min (io.numChannels(), numChannels_);
    if (n == 0 || channels == 0)
        return;
    const AudioView view = io.withChannels (channels);

    // 1. Base rate: pre-emphasis and gain, in place.
    for (int start = 0; start < n; start += kControlBlock)
    {
        const int end = std::min (n, start + kControlBlock);
        if (gainKnob_.isSmoothing())
            updateDriveBandwidth (gainKnob_.skip (end - start));

        for (int i = start; i < end; ++i)
        {
            const float g = driveGain_.next();
            for (int ch = 0; ch < channels; ++ch)
            {
                float& x = view.channel (ch)[i];
                const float pre = emphasis_.processSample (ch, inputHp_.processSample (ch, x));
                x = driveLp_.processSample (ch, g * pre);
            }
        }
    }

    // 2. Oversampled and anti-aliased: the clipper.
    const AudioView up = oversampler_.upsample (view);
    for (int ch = 0; ch < channels; ++ch)
    {
        auto& clipper = clipper_[static_cast<size_t> (ch)];
        float* d = up.channel (ch);
        for (int i = 0; i < up.numFrames(); ++i)
            d[i] = clipper.process (d[i], clip, clipIntegral);
    }
    oversampler_.downsample (view);

    // 3. Base rate: cut and level.
    for (int start = 0; start < n; start += kControlBlock)
    {
        const int end = std::min (n, start + kControlBlock);
        if (cut_.isSmoothing())
            updateCut (cut_.skip (end - start));

        for (int i = start; i < end; ++i)
        {
            const float out = outputGain_.next();
            for (int ch = 0; ch < channels; ++ch)
            {
                float& x = view.channel (ch)[i];
                float y = dcBlock_.processSample (ch, x);
                y = cutLp_.processSample (ch, y);
                y = fizzLp_.processSample (ch, y);
                x = out * y;
            }
        }
    }
}

} // namespace sedef::dsp
