// SPDX-License-Identifier: AGPL-3.0-or-later
#include "sedef/dsp/Tide.h"

#include "sedef/core/Math.h"

#include <algorithm>
#include <cmath>

namespace sedef::dsp
{
namespace
{
// Static curve, on the mean-square level of the driven signal (dB re full-scale power).
constexpr float kThresholdDb = -12.0f;
constexpr float kRatio = 3.0f;
constexpr float kKneeDb = 6.0f;

// comp: drive into the detector, 6..40 dB.
constexpr float kMinDriveDb = 6.0f;
constexpr float kDriveRangeDb = 34.0f;
// Automatic makeup cancels the reduction a typical playing level (-24 dB mean square, about
// -21 dBFS sine peak) would get, so turning `comp` up adds sustain rather than volume.
constexpr float kReferenceDb = -24.0f;
// level: +-12 dB around the auto-made-up level.
constexpr float kLevelRangeDb = 24.0f;

// tone: first-order tilt around 1 kHz, +-6 dB at the spectrum extremes.
constexpr double kTiltPivotHz = 1000.0;
constexpr float kTiltRangeDb = 12.0f;

constexpr double kDetectorHpHz = 60.0; // keeps low notes from pumping the whole chord
// Short mean-square window: follows the energy, not the waveform, but falls fast enough that the
// release is set by the gain-reduction stages below, not by the detector.
constexpr double kMsAttackMs = 2.0;
constexpr double kMsReleaseMs = 6.0;
constexpr double kAttackMs = 3.0;
// Program-dependent release: a fast stage recovers from transients; a slow stage charges only
// while the signal stays above threshold, so after long, dense passages it holds the gain back
// and lets it recover gently.
constexpr double kFastReleaseMs = 50.0;
constexpr double kSlowChargeMs = 800.0;
constexpr double kSlowReleaseMs = 400.0;

constexpr double kGainRampSeconds = 0.03;
constexpr double kToneSmoothingSeconds = 0.02;

constexpr float kDbToNeper = 0.11512925f; // ln(10) / 20

float onePole (double ms, double sampleRate) noexcept
{
    return static_cast<float> (std::exp (-1.0 / (0.001 * ms * sampleRate)));
}

/// Soft-knee gain reduction (dB, >= 0) for a detector level (Giannoulis, Massberg & Reiss 2012).
float reductionDb (float levelDb) noexcept
{
    constexpr float slope = 1.0f - 1.0f / kRatio;
    const float over = levelDb - kThresholdDb;
    if (2.0f * over <= -kKneeDb)
        return 0.0f;
    if (2.0f * over >= kKneeDb)
        return slope * over;
    const float t = over + 0.5f * kKneeDb;
    return slope * t * t / (2.0f * kKneeDb);
}
} // namespace

void Tide::prepare (const ProcessSpec& spec)
{
    sampleRate_ = spec.sampleRate;
    detectorDrive_.reset (sampleRate_, kGainRampSeconds);
    outputGain_.reset (sampleRate_, kGainRampSeconds);
    blend_.reset (sampleRate_, kGainRampSeconds);
    tone_.reset (sampleRate_, kToneSmoothingSeconds);
    detectorHp_.setCoeffs (design::highpass1 (sampleRate_, kDetectorHpHz));
    tiltK_ = static_cast<float> (std::tan (kPiD * kTiltPivotHz / sampleRate_));

    msAttack_ = onePole (kMsAttackMs, sampleRate_);
    msRelease_ = onePole (kMsReleaseMs, sampleRate_);
    attack_ = onePole (kAttackMs, sampleRate_);
    fastRelease_ = onePole (kFastReleaseMs, sampleRate_);
    slowCharge_ = onePole (kSlowChargeMs, sampleRate_);
    slowRelease_ = onePole (kSlowReleaseMs, sampleRate_);

    applySettings (true);
    reset();
}

void Tide::reset() noexcept
{
    detectorHp_.reset();
    tilt_.reset();
    meanSquare_ = fastReduction_ = slowReduction_ = 0.0f;
    gainReductionDb_.store (0.0f, std::memory_order_relaxed);
    primed_ = false;
}

void Tide::setSettings (const Settings& s) noexcept
{
    settings_ = s;
    applySettings (!primed_);
    primed_ = true;
}

void Tide::applySettings (bool snap) noexcept
{
    const float comp = std::clamp (settings_.comp, 0.0f, 1.0f);
    const float tone = std::clamp (settings_.tone, 0.0f, 1.0f);
    const float level = std::clamp (settings_.level, 0.0f, 1.0f);
    const float blend = std::clamp (settings_.blend, 0.0f, 1.0f);

    const float driveDb = kMinDriveDb + kDriveRangeDb * comp;
    const float makeupDb = reductionDb (kReferenceDb + driveDb);
    const float levelDb = kLevelRangeDb * (level - 0.5f);
    const float drive = dbToGain (driveDb);
    const float output = dbToGain (makeupDb + levelDb);

    if (snap)
    {
        detectorDrive_.setCurrentAndTarget (drive);
        outputGain_.setCurrentAndTarget (output);
        blend_.setCurrentAndTarget (blend);
        tone_.setCurrentAndTarget (tone);
        updateTilt (tone);
    }
    else
    {
        detectorDrive_.setTarget (drive);
        outputGain_.setTarget (output);
        blend_.setTarget (blend);
        tone_.setTarget (tone);
    }
}

void Tide::updateTilt (float tone) noexcept
{
    // design::highShelf1 of 2t dB centred on the pivot, scaled down by t dB: -t at DC, 0 at the
    // pivot, +t at the top. With the pivot fixed only the shelf gain moves, so this is cheap
    // enough to run every sample while the knob is turning (no zipper from stepped coefficients).
    const float h = std::exp (-kDbToNeper * kTiltRangeDb * (tone - 0.5f)); // 1 / shelf gain
    const float inv = 1.0f / (h + tiltK_);
    tilt_.setCoeffs (
        { (1.0f + tiltK_ * h) * inv, (tiltK_ * h - 1.0f) * inv, 0.0f, (tiltK_ - h) * inv, 0.0f });
}

void Tide::process (const AudioView& io) noexcept
{
    const int n = io.numFrames();
    const int channels = std::min (io.numChannels(), kMaxChannels);
    float maxReduction = 0.0f;

    for (int start = 0; start < n; start += kControlBlock)
    {
        const int end = std::min (n, start + kControlBlock);
        const bool toneMoving = tone_.isSmoothing();

        for (int i = start; i < end; ++i)
        {
            if (toneMoving)
                updateTilt (tone_.next());
            const float drive = detectorDrive_.next();
            const float output = outputGain_.next();
            const float mix = blend_.next();

            // Stereo-linked: the louder channel sets the gain for both.
            float power = 0.0f;
            for (int ch = 0; ch < channels; ++ch)
            {
                const float d = detectorHp_.processSample (ch, io.channel (ch)[i]) * drive;
                power = std::max (power, d * d);
            }
            const float msCoeff = power > meanSquare_ ? msAttack_ : msRelease_;
            meanSquare_ = power + msCoeff * (meanSquare_ - power);

            const float target = reductionDb (10.0f * std::log10 (meanSquare_ + 1.0e-12f));
            const float fastCoeff = target > fastReduction_ ? attack_ : fastRelease_;
            fastReduction_ = target + fastCoeff * (fastReduction_ - target);
            const float slowCoeff = target > slowReduction_ ? slowCharge_ : slowRelease_;
            slowReduction_ = target + slowCoeff * (slowReduction_ - target);

            const float reduction = std::max (fastReduction_, slowReduction_);
            maxReduction = std::max (maxReduction, reduction);
            const float gain = std::exp (-kDbToNeper * reduction) * output;

            for (int ch = 0; ch < channels; ++ch)
            {
                float& x = io.channel (ch)[i];
                const float wet = tilt_.processSample (ch, x * gain);
                x += mix * (wet - x);
            }
        }
    }

    meanSquare_ = flushToZero (meanSquare_);
    fastReduction_ = flushToZero (fastReduction_);
    slowReduction_ = flushToZero (slowReduction_);
    gainReductionDb_.store (-maxReduction, std::memory_order_relaxed);
}

float Tide::gainReductionDb() const noexcept
{
    return gainReductionDb_.load (std::memory_order_relaxed);
}

} // namespace sedef::dsp
