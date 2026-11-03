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

/// "Beacon" vibe: the photocell four-stage phase shifter. Each stage has its own capacitor, so
/// their break frequencies are staggered over several decades; one lamp lights all four
/// photocells, and the lamp's thermal lag and the cells' fast-on, slow-off response make the
/// sweep a lopsided throb rather than a sine. Chorus mixes the stages with the dry signal (moving
/// notches); Vibrato plays the stages alone (pitch wobble). Linear and latency-free.
/// See docs/dsp/beacon.md.
class Beacon
{
public:
    enum Mode
    {
        Chorus = 0,
        Vibrato = 1
    };

    struct Settings
    {
        float speedHz = 2.0f; ///< 0.3..8 Hz
        float depth = 0.7f;   ///< 0..1, how far the lamp swings
        int mode = Chorus;
    };

    static constexpr int kNumStages = 4;
    static constexpr float kMinSpeedHz = 0.3f;
    static constexpr float kMaxSpeedHz = 8.0f;

    /// Phasing capacitor of each stage (farads).
    static constexpr std::array<double, kNumStages> kCapacitance { 15.0e-9, 220.0e-9, 470.0e-12,
                                                                   4.7e-9 };
    /// Each stage's photocell when fully lit (ohms).
    static constexpr std::array<double, kNumStages> kLitOhms { 12.7e3, 6.86e3, 7.69e3, 6.22e3 };

    /// The photocells' light-dependent resistance for a light level 0..1 (shared by all stages,
    /// scaled by each stage's lit resistance).
    static double resistanceRatio (double light) noexcept;
    /// Break frequency of a stage at a light level.
    static double stageHz (int stage, double light) noexcept;

    void prepare (const ProcessSpec& spec);
    void reset() noexcept;
    void setSettings (const Settings& s) noexcept;
    void process (const AudioView& io) noexcept;
    int latencySamples() const noexcept { return 0; }

    /// The photocells' light level now, 0..1 (what sets the sweep).
    float light() const noexcept { return cell_; }
    float lampLevel() const noexcept { return lamp_; }

private:
    void applySettings (bool snap) noexcept;

    double sampleRate_ = 48000.0;
    int numChannels_ = 0;
    Settings settings_;
    bool primed_ = false;

    PrecisePhase lfo_;
    std::array<TptOnePole, kNumStages> stages_;
    ExpSmoothed speed_, depth_;
    LinearSmoothed vibrato_; // 0 chorus .. 1 vibrato
    float lamp_ = 0.0f;      // filament heat, 0..1
    float cell_ = 0.0f;      // photocell state, 0..1
    float lampHeat_ = 0.0f, lampCool_ = 0.0f, cellOn_ = 0.0f, cellOff_ = 0.0f; // one-pole coeffs
};

} // namespace sedef::dsp
