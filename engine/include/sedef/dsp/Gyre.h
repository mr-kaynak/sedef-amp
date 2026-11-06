// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "sedef/core/AudioView.h"
#include "sedef/core/Lfo.h"
#include "sedef/core/PrecisePhase.h"
#include "sedef/core/Smoothed.h"
#include "sedef/core/Svf.h"

#include <array>

namespace sedef::dsp
{

/// "Gyre" phaser: four (or six) matched first-order all-pass stages swept by an LFO and mixed
/// with the dry signal, which cuts two (or three) notches that glide up and down the spectrum.
/// Feedback around the stages adds resonant peaks between the notches. Linear and latency-free.
/// See docs/dsp/gyre.md.
class Gyre
{
public:
    enum Stages
    {
        Four = 0,
        Six = 1
    };

    struct Settings
    {
        float speedHz = 0.5f;  ///< 0.1..8 Hz
        float depth = 0.8f;    ///< 0..1, sweep width
        float feedback = 0.3f; ///< 0..1
        int stages = Four;
    };

    static constexpr float kMinSpeedHz = 0.1f;
    static constexpr float kMaxSpeedHz = 8.0f;
    static constexpr float kCentreHz = 600.0f;      ///< the stages' break frequency at mid sweep
    static constexpr float kMaxSweepOctaves = 2.0f; ///< either side of the centre at full depth
    static constexpr float kMaxFeedback = 0.85f;

    void prepare (const ProcessSpec& spec);
    void reset() noexcept;
    void setSettings (const Settings& s) noexcept;
    void process (const AudioView& io) noexcept;
    int latencySamples() const noexcept { return 0; }

    /// The stages' common break frequency now.
    float breakHz() const noexcept { return breakHz_; }

private:
    static constexpr int kMaxStages = 6;

    void applySettings (bool snap) noexcept;

    double sampleRate_ = 48000.0;
    int numChannels_ = 0;
    Settings settings_;
    bool primed_ = false;

    PrecisePhase lfo_;
    std::array<TptOnePole, kMaxStages> stages_;
    std::array<float, kMaxChannels> feedbackState_ {};
    ExpSmoothed speed_, depth_, feedback_;
    LinearSmoothed six_; // 0: four stages .. 1: six stages
    float breakHz_ = kCentreHz;
};

} // namespace sedef::dsp
