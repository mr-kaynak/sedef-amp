// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "sedef/core/AudioView.h"

namespace sedef::dsp
{

/// "Fathom" reverb. Plate: Dattorro's figure-eight tank topology. Hall: 8-line feedback delay
/// network with frequency-dependent decay. Disabling stops new input; the tail rings out.
class Fathom
{
public:
    enum Mode
    {
        Plate = 0,
        Hall = 1
    };

    struct Settings
    {
        bool enabled = false;
        int mode = Plate;
        float mix = 0.3f;            ///< 0..1
        float decaySeconds = 2.5f;   ///< 1..60 s (RT60)
        float lowCutHz = 20.0f;      ///< 20..500 Hz, applied to the wet signal
        float highCutHz = 5000.0f;   ///< 800..5000 Hz, applied to the wet signal
        float preDelayMs = 20.0f;    ///< 1..200 ms
    };

    Fathom();
    ~Fathom();
    Fathom (const Fathom&) = delete;
    Fathom& operator= (const Fathom&) = delete;

    void prepare (const ProcessSpec& spec);
    void reset() noexcept;
    void setSettings (const Settings& s) noexcept;
    /// Stereo in place (2 channels).
    void process (const AudioView& io) noexcept;

private:
    struct Impl;
    Impl* impl_ = nullptr;
};

} // namespace sedef::dsp
