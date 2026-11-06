// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "sedef/core/AudioView.h"
#include "sedef/core/Smoothed.h"
#include "sedef/core/Svf.h"

#include <array>

namespace sedef::dsp
{

/// "Anemone" envelope filter: a resonant state-variable filter whose cutoff follows the picking
/// envelope (the classic auto-wah). Low-, band- or high-pass; sweeps up or down. Linear and
/// latency-free. See docs/dsp/anemone.md.
class Anemone
{
public:
    enum Mode
    {
        LowPass = 0,
        BandPass = 1,
        HighPass = 2
    };

    enum Direction
    {
        Up = 0,
        Down = 1
    };

    struct Settings
    {
        float sensitivity = 0.5f; ///< 0..1, how hard the envelope pushes the cutoff
        float range = 0.6f;       ///< 0..1, sweep width: 1 to 5 octaves
        float resonance = 0.5f;   ///< 0..1, Q 0.7 to 16
        int mode = LowPass;
        int direction = Up;
    };

    static constexpr float kBaseHz = 200.0f;    ///< the bottom of every sweep
    static constexpr float kAttackMs = 1.0f;    ///< peak detector attack (one-pole time constant)
    static constexpr float kReleaseMs = 120.0f; ///< peak detector release
    static constexpr float kSmoothMs = 3.0f;    ///< then a one-pole that irons out the ripple

    /// Sweep travel 0..1 for an envelope value (linear peak) at a sensitivity.
    static float travel (float envelope, float sensitivity) noexcept;
    /// Cutoff for a travel value and the range and direction settings.
    static float cutoffHz (float travel, float range, int direction) noexcept;
    static float resonanceToQ (float resonance) noexcept;

    void prepare (const ProcessSpec& spec);
    void reset() noexcept;
    void setSettings (const Settings& s) noexcept;
    void process (const AudioView& io) noexcept;
    int latencySamples() const noexcept { return 0; }

    float currentCutoffHz() const noexcept { return cutoff_; }
    float currentEnvelope() const noexcept { return smoothed_; }

private:
    static constexpr int kNumModes = 3;

    void applySettings (bool snap) noexcept;

    double sampleRate_ = 48000.0;
    int numChannels_ = 0;
    Settings settings_;
    bool primed_ = false;

    Svf filter_;
    PeakEnvelope envelope_;
    float smoothed_ = 0.0f; // the envelope after the ripple filter
    float smoothCoeff_ = 0.0f;
    ExpSmoothed sensitivity_, range_, resonance_;
    LinearSmoothed down_; // 0 up .. 1 down, crossfades direction switches
    std::array<LinearSmoothed, kNumModes> modeGain_;
    float cutoff_ = kBaseHz;
};

} // namespace sedef::dsp
