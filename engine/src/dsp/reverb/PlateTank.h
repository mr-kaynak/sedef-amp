// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "ReverbBlocks.h"
#include "sedef/core/Lfo.h"

#include <array>

namespace sedef::dsp::reverb
{

/// Plate reverberator after J. Dattorro, "Effect Design, Part 1: Reverberator and Other Filters",
/// JAES 45(9), 1997: input band-limiting, four input diffusers and the figure-eight tank with
/// modulated decay-diffusion all-passes, damping, decay gains and the published output taps. All
/// lengths are the paper's (given at 29761 Hz) scaled to the session rate.
class PlateTank
{
public:
    void prepare (double sampleRate);
    void clear() noexcept;

    /// RT60 in seconds; the decay gain follows from the tank's loop length.
    void setDecay (float rt60Seconds) noexcept;
    /// Corner of the in-tank damping low-pass (high frequencies die faster above it).
    void setDamping (float hz) noexcept;
    /// Corner of the input band-limiting low-pass.
    void setBandwidth (float hz) noexcept;

    /// Advances the modulators by one control block. Call once every kControlBlock samples.
    void controlTick() noexcept;

    /// n <= kControlBlock. Stereo in (summed to mono, as in the paper), stereo out.
    void process (const float* inL, const float* inR, float* outL, float* outR, int n) noexcept;

    /// The decay gain currently in use (for tests).
    float decayGain() const noexcept { return decay_; }

private:
    double fs_ = 48000.0;

    OnePoleLowpass bandwidth_, dampL_, dampR_;
    std::array<Allpass, 4> diffusers_;
    Allpass apL1_, apL2_, apR1_, apR2_;
    DelayLine dL1_, dL2_, dR1_, dR2_;
    int lenL1_ = 1, lenL2_ = 1, lenR1_ = 1, lenR2_ = 1;
    double loopSeconds_ = 0.7;

    float decay_ = 0.5f;
    float decayDiffusion2_ = 0.5f;

    // Output taps (session-rate offsets), see Dattorro Table 2.
    std::array<int, 14> taps_ {};

    // Modulation of the decay-diffusion 1 delays: quadrature sines plus a slow random wander,
    // computed per control block and ramped per sample.
    Lfo lfo_;
    SmoothRandom wanderL_, wanderR_;
    float excursion_ = 0.0f;
    float modL_ = 0.0f, modR_ = 0.0f, modStepL_ = 0.0f, modStepR_ = 0.0f;
};

} // namespace sedef::dsp::reverb
