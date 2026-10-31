// SPDX-License-Identifier: AGPL-3.0-or-later
#include "sedef/dsp/NoiseGate.h"

#include "sedef/core/Math.h"

#include <algorithm>
#include <cmath>

namespace sedef::dsp
{
namespace
{
constexpr float kOffThresholdDb = -96.0f; // bottom of the parameter range: gate disabled
constexpr float kHysteresisDb = 4.0f;     // close this far below the open threshold
constexpr float kFloor = 1.0e-4f;         // -80 dB: maximum attenuation
constexpr double kPeakHoldMs = 15.0;      // >= one period of the lowest guitar notes
constexpr double kDetectorReleaseMs = 5.0;
constexpr double kHoldMs = 25.0; // after the detector's own peak hold: ~40 ms in total
constexpr double kAttackMs = 0.4;
constexpr double kReleaseMs = 25.0; // ~0.35 dB/ms: -40 dB about 115 ms after closing
constexpr double kRoundingMs = 0.15;
constexpr double kDcBlockHz = 10.0;

float onePole (double ms, double sampleRate) noexcept
{
    return static_cast<float> (std::exp (-1.0 / (0.001 * ms * sampleRate)));
}

int samplesFor (double ms, double sampleRate) noexcept
{
    return static_cast<int> (std::lround (0.001 * ms * sampleRate));
}
} // namespace

void NoiseGate::prepare (const ProcessSpec& spec)
{
    sampleRate_ = spec.sampleRate;
    dcBlock_.setCoeffs (design::highpass1 (sampleRate_, kDcBlockHz));
    envelopeRelease_ = onePole (kDetectorReleaseMs, sampleRate_);
    envelopeHoldSamples_ = samplesFor (kPeakHoldMs, sampleRate_);
    holdSamples_ = samplesFor (kHoldMs, sampleRate_);
    attack_ = onePole (kAttackMs, sampleRate_);
    release_ = onePole (kReleaseMs, sampleRate_);
    rounding_ = onePole (kRoundingMs, sampleRate_);
    reset();
}

void NoiseGate::reset() noexcept
{
    dcBlock_.reset();
    envelope_ = 0.0f;
    envelopeHoldLeft_ = 0;
    open_ = false;
    holdLeft_ = 0;
    gain_ = smoothGain_ = bypassed_ ? 1.0f : kFloor;
    gainReductionDb_.store (gainToDb (smoothGain_), std::memory_order_relaxed);
    isOpen_.store (bypassed_, std::memory_order_relaxed);
}

void NoiseGate::setSettings (const Settings& s) noexcept
{
    const float threshold = std::clamp (s.thresholdDb, kOffThresholdDb, 0.0f);
    bypassed_ = threshold <= kOffThresholdDb + 0.05f;
    openLevel_ = dbToGain (threshold);
    closeLevel_ = dbToGain (threshold - kHysteresisDb);
}

float NoiseGate::targetGain (float envelope) noexcept
{
    if (bypassed_)
    {
        open_ = false;
        return 1.0f;
    }

    if (envelope >= openLevel_)
    {
        open_ = true;
        holdLeft_ = holdSamples_;
    }
    else if (open_)
    {
        // Inside the hysteresis band the hold is refreshed; below it, it runs down.
        if (envelope >= closeLevel_)
            holdLeft_ = holdSamples_;
        else if (--holdLeft_ <= 0)
            open_ = false;
    }

    if (open_)
        return 1.0f;

    // Closed: 1:7 downward expansion below the open threshold (-24 dB at the close threshold),
    // so the gain follows the envelope continuously instead of switching.
    const float r = envelope / openLevel_;
    const float r2 = r * r;
    return std::max (kFloor, std::min (1.0f, r2 * r2 * r2));
}

void NoiseGate::process (const AudioView& io, const float* sidechain) noexcept
{
    const int n = io.numFrames();
    const int channels = io.numChannels();
    if (sidechain == nullptr && channels == 0)
        return;
    // Without a sidechain the gate keys off its own input (read before it is overwritten).
    const float* key = sidechain != nullptr ? sidechain : io.channel (0);

    for (int i = 0; i < n; ++i)
    {
        const float level = std::abs (dcBlock_.processSample (0, key[i]));
        if (level >= envelope_)
        {
            envelope_ = level;
            envelopeHoldLeft_ = envelopeHoldSamples_;
        }
        else if (envelopeHoldLeft_ > 0)
        {
            --envelopeHoldLeft_;
        }
        else
        {
            envelope_ = envelope_ > 1.0e-10f ? envelope_ * envelopeRelease_ : 0.0f;
        }

        const float target = targetGain (envelope_);
        const float c = target > gain_ ? attack_ : release_;
        gain_ = target + c * (gain_ - target);
        smoothGain_ = gain_ + rounding_ * (smoothGain_ - gain_);
        // Land exactly on the target, so an open gate is bit-transparent.
        if (std::abs (smoothGain_ - target) < 1.0e-6f)
            gain_ = smoothGain_ = target;

        for (int ch = 0; ch < channels; ++ch)
            io.channel (ch)[i] *= smoothGain_;
    }

    gainReductionDb_.store (std::min (0.0f, gainToDb (smoothGain_)), std::memory_order_relaxed);
    isOpen_.store (bypassed_ || open_, std::memory_order_relaxed);
}

float NoiseGate::gainReductionDb() const noexcept
{
    return gainReductionDb_.load (std::memory_order_relaxed);
}

bool NoiseGate::isOpen() const noexcept
{
    return isOpen_.load (std::memory_order_relaxed);
}

} // namespace sedef::dsp
