// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "sedef/core/AudioView.h"

namespace sedef::dsp
{

/// "Coral" distortion: high-gain hard clipping (diodes to ground) with a post low-pass "cut"
/// control (turning it up removes highs).
class Coral
{
public:
    struct Settings
    {
        float gain = 0.5f;  ///< 0..1
        float cut = 0.5f;   ///< 0..1, more = darker
        float level = 0.5f; ///< 0..1, unity around 0.5
    };

    void prepare (const ProcessSpec& spec);
    void reset() noexcept;
    void setSettings (const Settings& s) noexcept;
    void process (const AudioView& io) noexcept;
    int latencySamples() const noexcept;
};

} // namespace sedef::dsp
