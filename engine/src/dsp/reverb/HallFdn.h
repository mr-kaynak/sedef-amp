// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "ReverbBlocks.h"
#include "sedef/core/Lfo.h"

#include <array>

namespace sedef::dsp::reverb
{

/// Hall reverberator: sparse early reflections plus an 8-line feedback delay network (Jot &
/// Chaigne 1991) with an orthogonal Hadamard feedback matrix, prime line lengths of 31-102 ms,
/// per-line first-order shelving absorption for frequency-dependent decay, input diffusion and
/// slow random delay modulation. Stereo in, decorrelated stereo out.
class HallFdn
{
public:
    static constexpr int kLines = 8;
    static constexpr int kReflections = 10;

    void prepare (double sampleRate);
    void clear() noexcept;

    /// rt60Seconds is the decay time at low and mid frequencies; above dampingHz the decay time
    /// falls to kHighRatio of it.
    void setDecay (float rt60Seconds, float dampingHz) noexcept;

    /// Advances the modulators by one control block. Call once every kControlBlock samples.
    void controlTick() noexcept;

    /// n <= kControlBlock.
    void process (const float* inL, const float* inR, float* outL, float* outR, int n) noexcept;

    /// Largest per-line DC gain (for tests; must stay below 1).
    float maxLineGain() const noexcept;

    static constexpr float kHighRatio = 0.5f;

private:
    struct Line
    {
        DelayLine delay;
        int length = 1;
        // First-order shelf with the line's absorption folded in: y = b0 x + s, s = b1 x - a1 y.
        float b0 = 0.0f, b1 = 0.0f, a1 = 0.0f, state = 0.0f;
        float dcGain = 0.0f;
        SmoothRandom wander;
        float mod = 0.0f, modStep = 0.0f;
    };

    double fs_ = 48000.0;
    std::array<Line, kLines> lines_;
    std::array<std::array<Allpass, 4>, 2> diffusers_;
    float modDepth_ = 0.0f;

    DelayLine reflectL_, reflectR_;
    std::array<int, kReflections> reflectTapsL_ {}, reflectTapsR_ {};
};

} // namespace sedef::dsp::reverb
