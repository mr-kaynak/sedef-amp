// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "sedef/core/AudioView.h"
#include "sedef/core/Transport.h"

namespace sedef::dsp
{

/// "Drift" multi-head tape echo with wow/flutter, tape saturation, tone controls in the feedback
/// loop, ping-pong width and host/tap/free sync. Disabling stops new input but lets the repeats
/// ring out (spillover).
class Drift
{
public:
    enum Sync
    {
        Free = 0,
        Host = 1,
        Tap = 2
    };

    struct Settings
    {
        bool enabled = false;
        int heads = 0;          ///< 0..3 -> 1..4 playback heads
        float timeMs = 500.0f;  ///< 16..2500 ms (Free)
        int sync = Host;
        int note = 13;          ///< index into kNoteBeats (1/4 by default)
        float tapBpm = 120.0f;  ///< used when sync == Tap
        float feedback = 0.35f, tapeMod = 0.3f, tapeDrive = 0.0f;
        float bass = 0.5f, treble = 0.5f, width = 1.0f, mix = 0.35f;
    };

    /// Note length in quarter-note beats for each driftNote choice
    /// (1/64T, 1/64, 1/32T, 1/64D, 1/32, 1/16T, 1/32D, 1/16, 1/8T, 1/16D, 1/8, 1/4T, 1/8D, 1/4,
    ///  1/2T, 1/4D, 1/2, 1/1T, 1/2D, 1/1, 1/1D).
    static float noteBeats (int noteIndex) noexcept;

    void prepare (const ProcessSpec& spec);
    void reset() noexcept;
    void setSettings (const Settings& s) noexcept;
    /// Stereo in place (2 channels).
    void process (const AudioView& io, const Transport& transport) noexcept;

    /// The delay time currently in effect (after sync), for the UI.
    float effectiveTimeMs() const noexcept;
};

} // namespace sedef::dsp
