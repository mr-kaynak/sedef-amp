// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "sedef/core/Biquad.h"
#include "sedef/core/DelayLine.h"

#include <array>

namespace sedef::dsp::cab
{

/// Small-room ambience heard by a room microphone pair a couple of metres from the cabinet:
/// band-limited input, sparse early reflections, then an 8-line feedback delay network with a
/// Hadamard mix and per-line damping (RT60 ~0.4 s at low frequencies, shorter in the treble).
/// Stereo in (each mic's room send is panned), stereo out (added to the outputs).
class RoomAmbience
{
public:
    static constexpr float kRt60 = 0.42f;

    void prepare (double sampleRate);
    void reset() noexcept;

    /// Adds the room response of inL/inR to outL/outR.
    void process (const float* inL, const float* inR, float* outL, float* outR, int n) noexcept;

    /// Samples after the input goes silent until the tail is below -120 dB.
    int tailSamples() const noexcept { return tailSamples_; }

private:
    static constexpr int kLines = 8;
    static constexpr int kTaps = 6;

    struct Tap
    {
        int delay = 0;
        float gain = 0.0f;
        bool fromLeft = true;
    };

    Biquad inputHp_, inputLp_; // two channels each
    DelayLine earlyL_, earlyR_;
    std::array<Tap, kTaps> tapsL_ {}, tapsR_ {};
    int preDelay_ = 0;

    std::array<DelayLine, kLines> lines_;
    std::array<int, kLines> lengths_ {};
    std::array<float, kLines> feedback_ {};
    std::array<float, kLines> damping_ {};
    float dampCoeff_ = 0.5f;

    float earlyGain_ = 1.0f;
    float lateGain_ = 1.0f;
    int tailSamples_ = 0;
};

} // namespace sedef::dsp::cab
