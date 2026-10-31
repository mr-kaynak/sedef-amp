// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "sedef/core/AudioView.h"
#include "sedef/core/Biquad.h"

#include <atomic>

namespace sedef::dsp
{

/// Downward expander with hysteresis and hold. The detector listens to a separate sidechain (the
/// raw instrument input) so gating follows the DI level, not the input-gain-boosted level.
class NoiseGate
{
public:
    struct Settings
    {
        float thresholdDb = -72.0f; ///< -96..0 dB
    };

    void prepare (const ProcessSpec& spec);
    void reset() noexcept;
    void setSettings (const Settings& s) noexcept;
    /// Gates io in place. sidechain has io.numFrames() samples (mono).
    void process (const AudioView& io, const float* sidechain) noexcept;

    float gainReductionDb() const noexcept; ///< telemetry, <= 0
    bool isOpen() const noexcept;           ///< telemetry

private:
    float targetGain (float envelope) noexcept;

    double sampleRate_ = 48000.0;
    bool bypassed_ = false;            // threshold at the bottom of its range: always open
    float openLevel_ = 2.5118864e-4f;  // -72 dB (the default threshold)
    float closeLevel_ = 1.5848932e-4f; // -76 dB

    // Detector: DC-blocked peak follower with a short peak hold, so the envelope does not ripple
    // between the cycles of low notes.
    Biquad dcBlock_;
    float envelope_ = 0.0f;
    float envelopeRelease_ = 0.0f;
    int envelopeHoldLeft_ = 0;
    int envelopeHoldSamples_ = 0;

    // Hysteresis state machine.
    bool open_ = false;
    int holdLeft_ = 0;
    int holdSamples_ = 0;

    // Gain smoothing: attack/release one-pole followed by a short rounding pole (no slope steps).
    float gain_ = 0.0f;
    float smoothGain_ = 0.0f;
    float attack_ = 0.0f;
    float release_ = 0.0f;
    float rounding_ = 0.0f;

    std::atomic<float> gainReductionDb_ { 0.0f };
    std::atomic<bool> isOpen_ { false };
};

} // namespace sedef::dsp
