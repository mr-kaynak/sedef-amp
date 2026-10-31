// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "sedef/core/AudioView.h"

namespace sedef::dsp
{

/// Polyphonic +-12 semitone pitch shifter for alternate tunings (e.g. play in Eb with standard
/// tuning). Time-domain overlap-add with correlation-based splice points (WSOLA-style), so chords
/// survive. At 0 semitones it is a true bypass.
class PitchShifter
{
public:
    struct Settings
    {
        int semitones = 0; ///< -12..+12
    };

    void prepare (const ProcessSpec& spec);
    void reset() noexcept;
    void setSettings (const Settings& s) noexcept;
    /// Mono or stereo in place.
    void process (const AudioView& io) noexcept;
};

} // namespace sedef::dsp
