// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "sedef/core/AudioView.h"
#include "sedef/dsp/amp/AmpSection.h"
#include "sedef/dsp/cab/CabSim.h"
#include "sedef/dsp/capture/CaptureFile.h"

#include <memory>

namespace sedef::dsp
{

/// The amp stage when the amp can be a capture: plays the built-in AmpSection or a CaptureModel
/// and crossfades between them, crossfades from one capture to the next, and for full-rig
/// captures can take Sedef's cab out of the path. Both paths share one latency, fixed per
/// prepare(), so switching never moves the timing.
///
/// Threading (docs/dsp/nam.md):
///
///   setModel()        any non-real-time thread (null clears); the newest model always plays
///                     next, once the fade in progress (at most 30 ms) is over
///   process*()        audio thread: picks up a new model once per block, never frees
///   collectGarbage()  any non-real-time thread, periodically: frees retired models (all but
///                     the one fading out; setModel() collects too)
class CaptureStage
{
public:
    /// Amp/capture and capture/capture crossfades.
    static constexpr double kFadeSeconds = 0.03;
    /// Level of the straight path that replaces the cab: what the cab does to the built-in
    /// amps' level at the default rig (docs/dsp/nam.md), so full-rig captures sit with them.
    static constexpr float kDirectGainDb = 4.0f;

    struct Settings
    {
        bool useCapture = false;    ///< amp source is Capture (falls back while none is loaded)
        float inputDb = 0.0f;       ///< input trim
        float outputDb = 0.0f;      ///< output trim
        float interfaceDbu = 12.0f; ///< level at which the player's interface reads 0 dBFS
        bool throughCab = true;     ///< false: the capture already contains a cab
    };

    CaptureStage();
    ~CaptureStage();
    CaptureStage (const CaptureStage&) = delete;
    CaptureStage& operator= (const CaptureStage&) = delete;

    /// The stage's latency: the built-in amp's, or what the rate conversion needs for captures
    /// made at 44.1, 48, 88.2 or 96 kHz if that is longer (minimum phase: its group delay at low
    /// frequencies; linear phase: the full filter's look-ahead). Also what captures must be
    /// built for.
    static int latencyFor (double sampleRate, int ampLatency,
                           OversamplingMode mode = OversamplingMode::lowLatency) noexcept;

    /// Mono. `ampLatency` is the built-in amp's latency at this rate.
    void prepare (const ProcessSpec& spec, int ampLatency);
    void reset() noexcept;
    void setSettings (const Settings& s) noexcept;
    int latencySamples() const noexcept;
    /// What models published to this stage must be built for. Any thread.
    CaptureSpec modelSpec() const noexcept;

    /// Audio thread. Mono, in place: the built-in amp and/or the capture. The amp gets its
    /// settings here, and is skipped (then reset before it plays again) while the capture plays.
    void process (AmpSection& amp, const AmpSection::Settings& ampSettings,
                  const AudioView& io) noexcept;
    /// Audio thread. The cab, or a straight path while a full-rig capture plays without it; the
    /// cab gets its settings here. `out` must have two channels and in's frame count; `in` may
    /// alias out's first channel.
    void processCab (CabSim& cab, const CabSim::Settings& cabSettings, const float* in,
                     const AudioView& out) noexcept;

    /// True while the capture is what you hear (not fading, not falling back). Audio thread.
    bool captureAudible() const noexcept;
    /// The file of the model the audio thread plays (null for none). Audio thread, or tests.
    const CaptureFile* playingFile() const noexcept;

    void setModel (std::unique_ptr<CaptureModel> model);
    void collectGarbage();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace sedef::dsp
