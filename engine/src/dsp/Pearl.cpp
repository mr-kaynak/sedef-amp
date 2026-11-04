// SPDX-License-Identifier: AGPL-3.0-or-later
#include "sedef/dsp/Pearl.h"

#include "sedef/core/Math.h"

#include <algorithm>
#include <cmath>

namespace sedef::dsp
{
namespace
{
// Op-amp stage with diodes across its feedback resistor: out = in + D(G * HP(in)). The clean
// "in" term carries the lows untouched; only what the feedback network's high-pass lets through
// is amplified and clipped, which is where the mid hump comes from.
constexpr double kInputHpHz = 20.0;
constexpr double kMidHz = 850.0;
constexpr double kMidQ = 0.8;
constexpr float kMaxMidDb = 10.0f;
constexpr double kDriveHpHz = 650.0;
constexpr float kMinDriveDb = 3.0f;    // gain 0
constexpr float kDriveRangeDb = 37.0f; // gain 1: +40 dB
constexpr float kDriveLpLowGainHz = 14000.0f;
constexpr float kDriveLpHighGainHz = 5000.0f;

// Diode knee in full-scale units, and the earlier knee of the negative half (mismatched diodes:
// a little even-order content).
constexpr float kKnee = 0.35f;
constexpr double kNegativeKnee = 0.82;
constexpr double kTail = 0.15; // log slope above the knee

constexpr double kDcBlockHz = 12.0;
constexpr float kToneLowHz = 1500.0f;
constexpr float kToneHighHz = 8000.0f;
constexpr double kFizzHz = 11000.0;
constexpr double kFizzQ = 0.6;
constexpr float kLevelRangeDb = 40.0f; // level: -20..+20 dB

constexpr double kGainRampSeconds = 0.03;
constexpr double kFilterSmoothingSeconds = 0.02;

// Feedback-diode limiter, normalised to a knee of 1, for a >= 0: linear at first, a firm knee,
// then a slow logarithmic rise like the diode current law (harder picking still gets louder).
// Slope 1 at zero. The negative half uses an earlier knee.
inline double diodeCurve (double a) noexcept
{
    const double s = std::sqrt (1.0 + a * a);
    return (1.0 - kTail) * a / s + kTail * std::log (a + s); // log(a + s) = asinh(a)
}

inline double diodeCurveIntegral (double a) noexcept
{
    const double s = std::sqrt (1.0 + a * a);
    return (1.0 - kTail) * (s - 1.0) + kTail * (a * std::log (a + s) - s + 1.0);
}

inline double clip (double r) noexcept
{
    return r >= 0.0 ? diodeCurve (r) : -kNegativeKnee * diodeCurve (-r / kNegativeKnee);
}

inline double clipIntegral (double r) noexcept
{
    return r >= 0.0 ? diodeCurveIntegral (r)
                    : kNegativeKnee * kNegativeKnee * diodeCurveIntegral (-r / kNegativeKnee);
}

/// Output trim (dB): measured makeup for a typical single-coil level (see docs/dsp/pearl.md), so
/// that at level 0.5 the output is -0.5 dB at gain 0, unity at gain 0.5 and +1 dB at gain 1.
float trimDb (float gain) noexcept
{
    return -2.26f + gain * (-7.7f + 3.44f * gain);
}
} // namespace

void Pearl::prepare (const ProcessSpec& spec)
{
    sampleRate_ = spec.sampleRate;
    numChannels_ = std::clamp (spec.numChannels, 1, kMaxChannels);
    maxBlockSize_ = std::max (spec.maxBlockSize, 1);

    oversampler_.prepare (spec.maxBlockSize, numChannels_, Oversampler::factorLog2For (sampleRate_),
                          spec.oversampling);
    latency_ = oversampler_.latencySamples();

    for (int ch = 0; ch < kMaxChannels; ++ch)
    {
        const auto c = static_cast<size_t> (ch);
        cleanDelay_[c].prepare (std::max (latency_, 1));
        drive_[c].assign (ch < numChannels_ ? static_cast<size_t> (spec.maxBlockSize) : 0, 0.0f);
        drivePtrs_[c] = drive_[c].data();
    }

    driveGain_.reset (sampleRate_, kGainRampSeconds);
    outputGain_.reset (sampleRate_, kGainRampSeconds);
    gainKnob_.reset (sampleRate_, kFilterSmoothingSeconds);
    tone_.reset (sampleRate_, kFilterSmoothingSeconds);
    mid_.reset (sampleRate_, kFilterSmoothingSeconds);

    inputHp_.setCoeffs (design::highpass1 (sampleRate_, kInputHpHz));
    driveHp_.setCoeffs (design::highpass1 (sampleRate_, kDriveHpHz));
    dcBlock_.setCoeffs (design::highpass1 (sampleRate_, kDcBlockHz));
    fizzLp_.setCoeffs (design::lowpass (sampleRate_, kFizzHz, kFizzQ));

    applySettings (true);
    reset();
}

void Pearl::reset() noexcept
{
    oversampler_.reset();
    for (auto& c : clipper_)
        c.reset();
    for (auto& d : cleanDelay_)
        d.reset();
    for (auto* f : { &inputHp_, &midBoost_, &driveHp_, &driveLp_, &dcBlock_, &toneLp_, &fizzLp_ })
        f->reset();
    primed_ = false;
}

void Pearl::setSettings (const Settings& s) noexcept
{
    settings_ = s;
    applySettings (!primed_);
    primed_ = true;
}

int Pearl::latencySamples() const noexcept
{
    return latency_;
}

void Pearl::applySettings (bool snap) noexcept
{
    const float gain = std::clamp (settings_.gain, 0.0f, 1.0f);
    const float tone = std::clamp (settings_.tone, 0.0f, 1.0f);
    const float level = std::clamp (settings_.level, 0.0f, 1.0f);
    const float mid = std::clamp (settings_.mid, 0.0f, 1.0f);

    // The clipping path is scaled to knee units here and back to full scale after the clipper.
    const float drive = dbToGain (kMinDriveDb + kDriveRangeDb * gain) / kKnee;
    const float output = dbToGain (trimDb (gain) + kLevelRangeDb * (level - 0.5f));

    if (snap)
    {
        driveGain_.setCurrentAndTarget (drive);
        outputGain_.setCurrentAndTarget (output);
        gainKnob_.setCurrentAndTarget (gain);
        tone_.setCurrentAndTarget (tone);
        mid_.setCurrentAndTarget (mid);
        updateDriveBandwidth (gain);
        updateTone (tone);
        updateMid (mid);
    }
    else
    {
        driveGain_.setTarget (drive);
        outputGain_.setTarget (output);
        gainKnob_.setTarget (gain);
        tone_.setTarget (tone);
        mid_.setTarget (mid);
    }
}

void Pearl::updateMid (float mid) noexcept
{
    midBoost_.setCoeffs (design::peaking (sampleRate_, kMidHz, kMidQ, kMaxMidDb * mid));
}

void Pearl::updateDriveBandwidth (float gain) noexcept
{
    // The feedback capacitor's corner falls as the feedback resistance (gain) rises.
    driveLp_.setCoeffs (
        design::lowpass1 (sampleRate_, mapExp (gain, kDriveLpLowGainHz, kDriveLpHighGainHz)));
}

void Pearl::updateTone (float tone) noexcept
{
    toneLp_.setCoeffs (design::lowpass1 (sampleRate_, mapExp (tone, kToneLowHz, kToneHighHz)));
}

void Pearl::process (const AudioView& io) noexcept
{
    // The scratch buffers hold maxBlockSize frames; split anything larger.
    if (maxBlockSize_ <= 0)
        return; // not prepared
    for (int start = 0; start < io.numFrames(); start += maxBlockSize_)
        processBlock (io.subView (start, std::min (maxBlockSize_, io.numFrames() - start)));
}

void Pearl::processBlock (const AudioView& io) noexcept
{
    const int n = io.numFrames();
    const int channels = std::min (io.numChannels(), numChannels_);
    if (n == 0 || channels == 0)
        return;

    // 1. Base rate: shared input filtering, then split into the (delayed) clean path, left in io,
    //    and the band-limited, amplified clipping path.
    for (int start = 0; start < n; start += kControlBlock)
    {
        const int end = std::min (n, start + kControlBlock);
        if (mid_.isSmoothing())
            updateMid (mid_.skip (end - start));
        if (gainKnob_.isSmoothing())
            updateDriveBandwidth (gainKnob_.skip (end - start));

        for (int i = start; i < end; ++i)
        {
            const float g = driveGain_.next();
            for (int ch = 0; ch < channels; ++ch)
            {
                const auto c = static_cast<size_t> (ch);
                float x = inputHp_.processSample (ch, io.channel (ch)[i]);
                x = midBoost_.processSample (ch, x);
                cleanDelay_[c].push (x);
                io.channel (ch)[i] = cleanDelay_[c].read (latency_);
                drive_[c][static_cast<size_t> (i)] =
                    driveLp_.processSample (ch, g * driveHp_.processSample (ch, x));
            }
        }
    }

    // 2. Oversampled and anti-aliased: the diode limiter.
    const AudioView drive (drivePtrs_.data(), channels, n);
    const AudioView up = oversampler_.upsample (drive);
    for (int ch = 0; ch < channels; ++ch)
    {
        auto& clipper = clipper_[static_cast<size_t> (ch)];
        float* d = up.channel (ch);
        for (int i = 0; i < up.numFrames(); ++i)
            d[i] = clipper.process (d[i], clip, clipIntegral);
    }
    oversampler_.downsample (drive);

    // 3. Base rate: sum, then tone and output.
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
                float& x = io.channel (ch)[i];
                float y = x + kKnee * drive_[static_cast<size_t> (ch)][static_cast<size_t> (i)];
                y = dcBlock_.processSample (ch, y);
                y = toneLp_.processSample (ch, y);
                y = fizzLp_.processSample (ch, y);
                x = out * y;
            }
        }
    }
}

} // namespace sedef::dsp
