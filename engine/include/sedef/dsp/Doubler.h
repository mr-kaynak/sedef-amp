// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "sedef/core/AudioView.h"

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
};

} // namespace sedef::dsp
