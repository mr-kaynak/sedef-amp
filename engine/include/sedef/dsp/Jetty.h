// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "sedef/core/AudioView.h"
#include "sedef/core/DelayLine.h"
#include "sedef/core/Lfo.h"
#include "sedef/core/PrecisePhase.h"
#include "sedef/core/Smoothed.h"
#include "sedef/core/Svf.h"

#include <array>

namespace sedef::dsp
{

/// "Jetty" flanger: a short modulated delay mixed 50/50 with the dry signal, in the manner of a
/// bucket-brigade flanger: the comb's notches sweep up and down, and Regen feeds the delay's
/// output back to its input (either polarity) for the metallic, resonant jet sweep. The delayed
/// path is band-limited like a bucket-brigade line clocked for that delay. Latency-free.
/// See docs/dsp/jetty.md.
class Jetty
{
public:
    struct Settings
    {
        float rateHz = 0.3f;   ///< 0.05..5 Hz
        float depth = 0.6f;    ///< 0..1, sweep width
        float manualMs = 2.0f; ///< 0.25..10 ms, the delay at the centre of the sweep
        float regen = 0.4f;    ///< -1..1, feedback (negative: inverted)
    };

    static constexpr float kMinRateHz = 0.05f;
    static constexpr float kMaxRateHz = 5.0f;
    static constexpr float kMinManualMs = 0.25f;
    static constexpr float kMaxManualMs = 10.0f;
    static constexpr float kMaxSweepOctaves = 1.5f; ///< either side of Manual at full depth
    static constexpr float kMinDelayMs = 0.1f;
    static constexpr float kMaxDelayMs = 16.0f;
    static constexpr float kMaxRegen = 0.95f;

    /// The delay for a Manual setting, depth and LFO value (-1..1), clamped to the line.
    static float delayMs (float manualMs, float depth, float lfo) noexcept;

    void prepare (const ProcessSpec& spec);
    void reset() noexcept;
    void setSettings (const Settings& s) noexcept;
    void process (const AudioView& io) noexcept;
    int latencySamples() const noexcept { return 0; }

    float currentDelayMs() const noexcept { return delayMs_; }

private:
    void applySettings (bool snap) noexcept;

    double sampleRate_ = 48000.0;
    int numChannels_ = 0;
    Settings settings_;
    bool primed_ = false;

    PrecisePhase lfo_;
    std::array<DelayLine, kMaxChannels> line_;
    Svf bandLimit_;      // the line's anti-imaging filter, which follows its clock (the delay)
    TptOnePole regenHp_; // coupling capacitor in the regen path (its low-pass; HP = x - LP)
    ExpSmoothed rate_, depth_, manual_, regen_;
    float delayMs_ = 2.0f;
    int filled_ = 0;       // samples written since reset (capped), see process()
    float fadeInv_ = 0.0f; // 1 / wet fade-in length in samples
};

} // namespace sedef::dsp
