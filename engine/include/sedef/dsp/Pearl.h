// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "sedef/core/AudioView.h"

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
};

} // namespace sedef::dsp
