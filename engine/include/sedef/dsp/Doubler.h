// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "sedef/core/AudioView.h"
#include "sedef/core/Biquad.h"
#include "sedef/core/DelayLine.h"
#include "sedef/core/Lfo.h"

#include <array>

namespace sedef::dsp
{

/// Double-tracking stereo widener: two short, slowly drifting delays with a few cents of pitch
/// movement, one per side, emulating two takes of the same part.
class Doubler
{
public:
    struct Settings
    {
        float spreadMs = 6.0f; ///< 3..20 ms
    };

    void prepare (const ProcessSpec& spec);
    void reset() noexcept;
    void setSettings (const Settings& s) noexcept;
    /// Stereo in place (2 channels).
    void process (const AudioView& io) noexcept;

private:
    void controlTick() noexcept;
    void processSamples (float* left, float* right, int length) noexcept;

    double sampleRate_ = 48000.0;
    float spreadMs_ = 6.0f;
    bool snapNext_ = true;

    // Left take: base delay plus drift. Right take ("the double"): base + spread plus its own,
    // independent drift, slightly darker.
    std::array<DelayLine, 2> takes_;
    Biquad soften_;
    float baseSamples_ = 0.0f;

    // Spread glides through two one-poles, so a change bends pitch smoothly instead of zipping.
    double targetSpread_ = 0.0, spread1_ = 0.0, spread2_ = 0.0, glideCoeff_ = 0.0;

    // Drift per side: a slow wander in timing plus a gentle wobble in pitch. Evaluated per control
    // tick and interpolated in between.
    std::array<SmoothRandom, 2> wander_, jitter_;
    std::array<Lfo, 2> wobble_;
    std::array<float, 2> mod_ {}, modStep_ {};
    float wobbleSamples_ = 0.0f;
    int tickCountdown_ = 0;
};

} // namespace sedef::dsp
