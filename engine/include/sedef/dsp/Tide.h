// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "sedef/core/AudioView.h"
#include "sedef/core/Biquad.h"
#include "sedef/core/Smoothed.h"

#include <atomic>

namespace sedef::dsp
{

/// "Tide" compressor pedal: fixed threshold, 3:1 ratio, soft knee, program-dependent release.
/// `comp` drives the signal into the threshold (more = more reduction), `tone` is a tilt EQ
/// (0.5 = flat, clockwise = brighter), `level` is makeup/output, `blend` mixes dry (0) to
/// compressed (1) in parallel.
class Tide
{
public:
    struct Settings
    {
        float comp = 0.5f;  ///< 0..1
        float tone = 0.5f;  ///< 0..1
        float level = 0.5f; ///< 0..1
        float blend = 0.7f; ///< 0..1
    };

    void prepare (const ProcessSpec& spec);
    void reset() noexcept;
    void setSettings (const Settings& s) noexcept;
    void process (const AudioView& io) noexcept;

    float gainReductionDb() const noexcept; ///< telemetry, <= 0

private:
    void applySettings (bool snap) noexcept;
    void updateTilt (float tone) noexcept;

    double sampleRate_ = 48000.0;
    Settings settings_;
    bool primed_ = false;

    LinearSmoothed detectorDrive_; // linear gain into the detector
    LinearSmoothed outputGain_;    // automatic makeup x level
    LinearSmoothed blend_;
    ExpSmoothed tone_;

    Biquad detectorHp_;
    Biquad tilt_;
    float tiltK_ = 0.0f; // prewarped pivot, tan(pi f / fs)

    // Detector and gain-reduction state (gain reduction in dB, positive = reduction).
    float meanSquare_ = 0.0f;
    float fastReduction_ = 0.0f;
    float slowReduction_ = 0.0f;
    float msAttack_ = 0.0f, msRelease_ = 0.0f;
    float attack_ = 0.0f, fastRelease_ = 0.0f;
    float slowCharge_ = 0.0f, slowRelease_ = 0.0f;

    std::atomic<float> gainReductionDb_{0.0f};
};

} // namespace sedef::dsp
