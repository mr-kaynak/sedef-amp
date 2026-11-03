// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "sedef/core/AudioView.h"
#include "sedef/core/Smoothed.h"
#include "sedef/core/Svf.h"

namespace sedef::dsp
{

/// "Conch" wah: the inductor wah's resonant band-pass, swept by a rocker (Position) from about
/// 350 Hz at the heel to 2.2 kHz at the toe. In Auto mode the picking envelope rocks the pedal
/// for you, from the Position upwards. Linear and latency-free. See docs/dsp/conch.md.
class Conch
{
public:
    enum Mode
    {
        Manual = 0,
        Auto = 1
    };

    struct Settings
    {
        int mode = Manual;
        float position = 0.5f;    ///< 0 heel .. 1 toe
        float resonance = 0.5f;   ///< 0..1: Q 2.5 to 10 at mid travel (5 at noon)
        float sensitivity = 0.5f; ///< 0..1, Auto mode only
    };

    static constexpr float kHeelHz = 350.0f;
    static constexpr float kToeHz = 2200.0f;
    static constexpr float kMinQ = 2.5f;
    static constexpr float kMaxQ = 10.0f;

    /// Resonance frequency for a rocker position (exponential travel, like an audio-taper pot).
    static float positionToHz (float position) noexcept;
    /// Q at mid travel for a Resonance setting (exponential).
    static float resonanceToQ (float resonance) noexcept;
    /// Q at a rocker position: the mid-travel Q, 1.5 times higher at the heel and 1.5 times lower
    /// at the toe, as the inductor wah's measured responses show.
    static float qAt (float resonance, float position) noexcept;
    /// Auto mode: how far up from the Position (0..1 of the remaining travel) an envelope of
    /// `envelope` (linear peak) rocks the pedal at a given sensitivity.
    static float autoTravel (float envelope, float sensitivity) noexcept;

    void prepare (const ProcessSpec& spec);
    void reset() noexcept;
    void setSettings (const Settings& s) noexcept;
    void process (const AudioView& io) noexcept;
    int latencySamples() const noexcept { return 0; }

    /// The rocker position the filter is at now (after smoothing and the Auto envelope).
    float currentPosition() const noexcept { return position_; }
    float centreHz() const noexcept { return positionToHz (position_); }

private:
    void applySettings (bool snap) noexcept;

    double sampleRate_ = 48000.0;
    int numChannels_ = 0;
    Settings settings_;
    bool primed_ = false;

    Svf filter_;
    PeakEnvelope envelope_;
    ExpSmoothed rocker_;      // manual position
    ExpSmoothed resonance_;   //
    ExpSmoothed sensitivity_; //
    LinearSmoothed autoMix_;  // 0 manual .. 1 auto, crossfades mode switches
    float position_ = 0.5f;
};

} // namespace sedef::dsp
