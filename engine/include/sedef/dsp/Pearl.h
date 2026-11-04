// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "sedef/core/AudioView.h"
#include "sedef/core/Biquad.h"
#include "sedef/core/Oversampler.h"
#include "sedef/core/Smoothed.h"
#include "sedef/dsp/detail/Adaa.h"

#include <array>
#include <vector>

namespace sedef::dsp
{

/// "Pearl" overdrive: mid-focused soft clipping in the feedback path (op-amp + diodes style) with
/// a switchable-in mid boost before the clipper. Low gain stays touch-sensitive and transparent.
class Pearl
{
public:
    struct Settings
    {
        float gain = 0.5f;  ///< 0..1
        float tone = 0.5f;  ///< 0..1, high-frequency shaping
        float level = 0.5f; ///< 0..1, unity around 0.5
        float mid = 0.0f;   ///< 0..1, mid boost amount
    };

    void prepare (const ProcessSpec& spec);
    void reset() noexcept;
    void setSettings (const Settings& s) noexcept;
    void process (const AudioView& io) noexcept;
    int latencySamples() const noexcept;

private:
    void processBlock (const AudioView& io) noexcept;
    void applySettings (bool snap) noexcept;
    void updateMid (float mid) noexcept;
    void updateDriveBandwidth (float gain) noexcept;
    void updateTone (float tone) noexcept;

    double sampleRate_ = 48000.0;
    int numChannels_ = 0;
    int maxBlockSize_ = 0;
    int latency_ = 0;
    Settings settings_;
    bool primed_ = false;

    Oversampler oversampler_;
    AlignedDryPath clean_; // lines the clean term up with the clipper
    std::array<std::vector<float>, kMaxChannels> drive_;
    std::array<float*, kMaxChannels> drivePtrs_ {};
    std::array<detail::Adaa1, kMaxChannels> clipper_;

    LinearSmoothed driveGain_;  // op-amp gain of the clipping path
    LinearSmoothed outputGain_; // trim x level
    ExpSmoothed gainKnob_, tone_, mid_;

    Biquad inputHp_, midBoost_; // shared by both paths
    Biquad driveHp_, driveLp_;  // band-limits the clipping path
    Biquad dcBlock_, toneLp_, fizzLp_;
};

} // namespace sedef::dsp
