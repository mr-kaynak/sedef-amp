// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "sedef/core/AudioView.h"
#include "sedef/core/Biquad.h"
#include "sedef/core/Oversampler.h"
#include "sedef/core/Smoothed.h"
#include "sedef/dsp/detail/Adaa.h"

#include <array>

namespace sedef::dsp
{

/// "Coral" distortion: high-gain hard clipping (diodes to ground) with a post low-pass "cut"
/// control (turning it up removes highs).
class Coral
{
public:
    struct Settings
    {
        float gain = 0.5f;  ///< 0..1
        float cut = 0.5f;   ///< 0..1, more = darker
        float level = 0.5f; ///< 0..1, unity around 0.5
    };

    void prepare (const ProcessSpec& spec);
    void reset() noexcept;
    void setSettings (const Settings& s) noexcept;
    void process (const AudioView& io) noexcept;
    int latencySamples() const noexcept;

private:
    void processBlock (const AudioView& io) noexcept;
    void applySettings (bool snap) noexcept;
    void updateDriveBandwidth (float gain) noexcept;
    void updateCut (float cut) noexcept;

    double sampleRate_ = 48000.0;
    int numChannels_ = 0;
    int maxBlockSize_ = 0;
    Settings settings_;
    bool primed_ = false;

    Oversampler oversampler_;
    std::array<detail::Adaa1, kMaxChannels> clipper_;

    LinearSmoothed driveGain_;
    LinearSmoothed outputGain_;
    ExpSmoothed gainKnob_, cut_;

    Biquad inputHp_, emphasis_, driveLp_; // before the clipper
    Biquad dcBlock_, cutLp_, fizzLp_;     // after the clipper
};

} // namespace sedef::dsp
