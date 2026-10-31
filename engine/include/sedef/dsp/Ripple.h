// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "sedef/core/AudioView.h"
#include "sedef/core/Biquad.h"
#include "sedef/core/DelayLine.h"
#include "sedef/core/Lfo.h"
#include "sedef/core/Smoothed.h"

#include <array>

namespace sedef::dsp
{

/// "Ripple" modulation pedal: tremolo, chorus or vibrato.
class Ripple
{
public:
    enum Mode
    {
        Tremolo = 0,
        Chorus = 1,
        Vibrato = 2
    };

    struct Settings
    {
        int mode = Tremolo;
        float speedHz = 3.92f; ///< 0.69..7.19 Hz
        float depth = 0.5f;    ///< 0..1
    };

    void prepare (const ProcessSpec& spec);
    void reset() noexcept;
    void setSettings (const Settings& s) noexcept;
    void process (const AudioView& io) noexcept;

private:
    static constexpr int kNumModes = 3;

    void applySettings (bool snap) noexcept;

    double sampleRate_ = 48000.0;
    int numChannels_ = 0;
    Settings settings_;
    bool primed_ = false;

    Lfo lfo_;
    std::array<SmoothRandom, kMaxChannels> drift_; // slow analog-style wander of the chorus delay
    std::array<DelayLine, kMaxChannels> delay_;
    Biquad chorusLp_; // bucket-brigade style darkening of the chorus wet signal

    ExpSmoothed speed_;
    ExpSmoothed tremoloDepth_; // modulation index, 0..1
    ExpSmoothed chorusSwing_;  // samples
    ExpSmoothed vibratoSwing_; // samples
    std::array<LinearSmoothed, kNumModes> modeGain_;
    float chorusCentre_ = 0.0f;  // samples
    float vibratoMargin_ = 0.0f; // samples
};

} // namespace sedef::dsp
