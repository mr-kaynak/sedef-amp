// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "sedef/core/AudioView.h"
#include "sedef/core/Biquad.h"
#include "sedef/core/Oversampler.h"
#include "sedef/core/Smoothed.h"
#include "sedef/dsp/detail/BjtStage.h"

#include <array>
#include <vector>

namespace sedef::dsp
{

/// "Breaker" treble booster: a single germanium transistor in common emitter with a small input
/// capacitor. The capacitor and the pickup's inductance keep the lows out of the boost and put
/// its peak in the upper mids; the Range switch swaps in a larger capacitor for a full-range
/// boost. Boost bypasses more of the emitter resistor: more gain, less feedback, and the
/// transistor's soft, lopsided clipping when pushed. See docs/dsp/breaker.md.
class Breaker
{
public:
    enum Range
    {
        Treble = 0,
        Full = 1,
    };

    struct Settings
    {
        float boost = 0.5f; ///< 0..1
        int range = Treble;
    };

    /// The input network's components and its analogue transfer function N(s) / D(s) (ascending
    /// powers of s), from the pickup's open-circuit voltage to the base. Exposed for tests and
    /// docs.
    static constexpr double kTrebleCapacitor = 4.7e-9;
    static constexpr double kFullCapacitor = 47.0e-9;
    static constexpr double kInputResistance = 7.5e3; ///< bias divider || the transistor's r_pi
    struct InputNetwork
    {
        std::array<double, 3> numerator {}, denominator {};
        double centreHz = 0.0;
    };
    static InputNetwork inputNetwork (int range) noexcept;

    /// Small-signal gain of the transistor stage (dB) at the given boost. Exposed for tests.
    static float stageGainDb (float boost) noexcept;

    void prepare (const ProcessSpec& spec);
    void reset() noexcept;
    void setSettings (const Settings& s) noexcept;
    void process (const AudioView& io) noexcept;
    int latencySamples() const noexcept;

private:
    void processBlock (const AudioView& io) noexcept;
    void applySettings (bool snap) noexcept;

    double sampleRate_ = 48000.0;
    int numChannels_ = 0;
    int maxBlockSize_ = 0;
    int factor_ = 1;
    Settings settings_;
    bool primed_ = false;

    Oversampler oversampler_;
    std::array<std::vector<float>, kMaxChannels> drive_;
    std::array<float*, kMaxChannels> drivePtrs_ {};
    std::vector<float> boostAt_; // per base-rate sample, for the oversampled stage
    std::array<detail::CurveAdaa, kMaxChannels> stage_;
    const detail::BjtCurveFamily* curves_ = nullptr; // shared, see stageCurves()
    float stageBoost_ = -1.0f;                       // the curve the stage is on

    LinearSmoothed rangeMix_; // 0 treble .. 1 full: both input networks run, crossfaded
    ExpSmoothed boostKnob_;

    Biquad trebleIn_, fullIn_; // pickup + input capacitor + input resistance
    Biquad dcBlock_, bandLp_;  // output capacitor into the amp, transistor bandwidth
};

} // namespace sedef::dsp
