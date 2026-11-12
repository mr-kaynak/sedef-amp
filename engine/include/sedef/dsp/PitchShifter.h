// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "sedef/core/AudioView.h"

#include <cstdint>
#include <memory>

namespace sedef::dsp
{

/// Transpose: polyphonic -12..+12 semitone pitch shifter for alternate tunings and octaves
/// (docs/dsp/transpose.md). Attack-synchronous time-domain grains shift what a long-window partial
/// model cannot explain; the model's partials are resynthesised shifted, without splices. Mono.
///
/// At 0 semitones it is off: bit-exact, no latency. Otherwise it delays attacks by exactly
/// latencySamples(): 3 ms shifting down, 10 ms shifting up.
class PitchShifter
{
public:
    struct Settings
    {
        int semitones = 0; ///< -12..+12
    };

    /// What the tests and the bench can read back.
    struct Stats
    {
        std::int64_t onsets = 0;  ///< attacks the grains were re-anchored to
        std::int64_t splices = 0; ///< sustain splices
        int tracks = 0;           ///< partials in the model now
    };

    PitchShifter();
    ~PitchShifter();
    PitchShifter (const PitchShifter&) = delete;
    PitchShifter& operator= (const PitchShifter&) = delete;

    void prepare (const ProcessSpec& spec);
    void reset() noexcept;
    void setSettings (const Settings& s) noexcept;
    /// Mono in place (channel 0 of `io`; other channels are left untouched).
    void process (const AudioView& io) noexcept;
    void process (float* x, int numFrames) noexcept;

    /// The latency the current setting adds, in samples: what the host is told.
    int latencySamples() const noexcept;
    /// The latency `semitones` adds at `sampleRate` (0 at 0 semitones).
    static int latencyFor (int semitones, double sampleRate) noexcept;
    /// True while the shifter touches the audio (a shift, or a fade back to 0).
    bool isActive() const noexcept;

    Stats stats() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    Settings settings_;
};

} // namespace sedef::dsp
