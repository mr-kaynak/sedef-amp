// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "sedef/core/AudioView.h"

namespace sedef::dsp
{

/// "Tide" compressor pedal: fixed threshold, 3:1 ratio, soft knee, program-dependent release.
/// `comp` drives the signal into the threshold (more = more reduction), `tone` is a tilt EQ
/// (0.5 = flat, clockwise = brighter), `level` is makeup/output, `blend` mixes dry (0) to
/// compressed (1) in parallel.
class Tide
{
public:
    struct Settings
    {
        float comp = 0.5f;  ///< 0..1
        float tone = 0.5f;  ///< 0..1
        float level = 0.5f; ///< 0..1
        float blend = 0.7f; ///< 0..1
    };

    void prepare (const ProcessSpec& spec);
    void reset() noexcept;
    void setSettings (const Settings& s) noexcept;
    void process (const AudioView& io) noexcept;

    float gainReductionDb() const noexcept; ///< telemetry, <= 0
};

} // namespace sedef::dsp
