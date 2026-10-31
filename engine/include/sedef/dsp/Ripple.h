// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "sedef/core/AudioView.h"

namespace sedef::dsp
{

/// "Ripple" modulation pedal: tremolo, chorus or vibrato.
class Ripple
{
public:
    enum Mode
    {
        Tremolo = 0,
        Chorus = 1,
        Vibrato = 2
    };

    struct Settings
    {
        int mode = Tremolo;
        float speedHz = 3.92f; ///< 0.69..7.19 Hz
        float depth = 0.5f;    ///< 0..1
    };

    void prepare (const ProcessSpec& spec);
    void reset() noexcept;
    void setSettings (const Settings& s) noexcept;
    void process (const AudioView& io) noexcept;
};

} // namespace sedef::dsp
