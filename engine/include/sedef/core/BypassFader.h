// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "sedef/core/AudioView.h"
#include "sedef/core/Smoothed.h"

namespace sedef
{

/// Click-free on/off for a module: crossfades between the dry input and the processed output.
class BypassFader
{
public:
    void prepare (double sampleRate, double fadeSeconds = 0.010)
    {
        mix_.reset (sampleRate, fadeSeconds);
        mix_.setCurrentAndTarget (enabled_ ? 1.0f : 0.0f);
    }

    void setEnabled (bool enabled) noexcept
    {
        enabled_ = enabled;
        mix_.setTarget (enabled ? 1.0f : 0.0f);
    }

    /// True when the module can be skipped entirely for this block.
    bool isFullyBypassed() const noexcept { return !enabled_ && !mix_.isSmoothing(); }
    /// True when the processed output can be used as-is (no crossfade needed).
    bool isFullyActive() const noexcept { return enabled_ && !mix_.isSmoothing(); }

    /// wet (in place) <- dry + mix * (wet - dry). Call only when neither fully on nor fully off.
    void mix (const AudioView& dry, const AudioView& wet) noexcept
    {
        const int n = wet.numFrames();
        for (int i = 0; i < n; ++i)
        {
            const float g = mix_.next();
            for (int c = 0; c < wet.numChannels(); ++c)
            {
                const float d = dry.channel (c < dry.numChannels() ? c : 0)[i];
                float& w = wet.channel (c)[i];
                w = d + g * (w - d);
            }
        }
    }

    void skip (int n) noexcept { mix_.skip (n); }

private:
    LinearSmoothed mix_;
    bool enabled_ = false;
};

} // namespace sedef
