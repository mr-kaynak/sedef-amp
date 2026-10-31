// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "sedef/core/AudioView.h"

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
};

} // namespace sedef::dsp
