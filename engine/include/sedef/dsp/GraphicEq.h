// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "sedef/core/AudioView.h"

#include <array>

namespace sedef::dsp
{

/// Nine-band graphic EQ with 12 dB/oct low and high cuts. Bands are RBJ peaking filters with a
/// constant Q of 1.41 (one octave); cuts are 2nd-order Butterworth and are bypassed at their
/// extremes (low cut <= 20 Hz, high cut >= 20 kHz). The UI draws the same curve in TypeScript, so
/// keep these definitions in sync with ui/src/features/eq/response.ts.
class GraphicEq
{
public:
    static constexpr std::array<float, 9> kBandHz { 65.0f,   125.0f,  250.0f,  500.0f, 1000.0f,
                                                    2000.0f, 4000.0f, 8000.0f, 16000.0f };

    struct Settings
    {
        std::array<float, 9> bandsDb {}; ///< -12..+12 dB
        float lowCutHz = 20.0f;          ///< 20..500 Hz (20 = off)
        float highCutHz = 20000.0f;      ///< 1k..20k Hz (20k = off)
    };

    void prepare (const ProcessSpec& spec);
    void reset() noexcept;
    void setSettings (const Settings& s) noexcept;
    void process (const AudioView& io) noexcept;

    static constexpr float kBandQ = 1.41f;

    /// Magnitude response in dB for the given settings. Thread-safe (pure function).
    static float responseDb (const Settings& s, double sampleRate, float hz) noexcept;
};

} // namespace sedef::dsp
