// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "sedef/core/AudioView.h"
#include "sedef/core/Biquad.h"
#include "sedef/core/Oversampler.h"
#include "sedef/core/Smoothed.h"
#include "sedef/dsp/detail/Adaa.h"

#include <array>

namespace sedef::dsp
{

/// "Gyre" fuzz: the classic four-stage sustain fuzz. An input stage, two cascaded clipping
/// transistor stages (diodes in their feedback), the passive tone network (a low-pass and a
/// high-pass blended by one pot, scooping the mids at noon) and a recovery stage. Sustain sets the
/// gain into the clippers, so a decaying note stays clipped, and loud, for longer. See
/// docs/dsp/gyre.md.
class Gyre
{
public:
    struct Settings
    {
        float sustain = 0.5f; ///< 0..1
        float tone = 0.5f;    ///< 0..1: bass (low-pass end) .. treble (high-pass end)
        float volume = 0.5f;  ///< 0..1, unity around 0.5
    };

    /// The tone network's components (our values) and the analogue transfer function they give,
    /// N(s) / D(s) with coefficients in ascending powers of s. Exposed for tests and docs.
    struct ToneNetwork
    {
        static constexpr double kLowPassR = 18.0e3;  ///< series R of the low-pass branch
        static constexpr double kLowPassC = 22.0e-9; ///< shunt C of the low-pass branch
        static constexpr double kHighPassR = 22.0e3; ///< shunt R of the high-pass branch
        static constexpr double kHighPassC = 3.9e-9; ///< series C of the high-pass branch
        static constexpr double kPot = 100.0e3;      ///< blend pot, linear
        static constexpr double kLoad = 100.0e3;     ///< input of the recovery stage
        std::array<double, 3> numerator {}, denominator {};
    };
    static ToneNetwork toneNetwork (float tone) noexcept;

    void prepare (const ProcessSpec& spec);
    void reset() noexcept;
    void setSettings (const Settings& s) noexcept;
    void process (const AudioView& io) noexcept;
    int latencySamples() const noexcept;

private:
    void processBlock (const AudioView& io) noexcept;
    void applySettings (bool snap) noexcept;
    void updateTone (float tone) noexcept;

    double sampleRate_ = 48000.0;
    int numChannels_ = 0;
    int maxBlockSize_ = 0;
    Settings settings_;
    bool primed_ = false;

    Oversampler oversampler_;
    std::array<detail::Adaa1, kMaxChannels> clipA_, clipB_;

    LinearSmoothed driveGain_;  // into the first clipping stage
    LinearSmoothed outputGain_; // trim x volume
    ExpSmoothed tone_;

    Biquad inputHp_, bandA_;              // base rate, before the first clipper
    Biquad couplingAB_, bandB_;           // oversampled, between the clippers
    Biquad toneNet_, recoveryHp_, outLp_; // base rate, after them
};

} // namespace sedef::dsp
