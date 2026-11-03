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

/// "Urchin" octave fuzz: the transformer-and-diodes octave-up circuit. A gain stage drives a
/// centre-tapped transformer whose two halves feed a pair of germanium diodes, so the signal is
/// full-wave rectified (every period folds into two: an octave up), then a transistor stage fuzzes
/// it. The octave comes from the rectifier alone, so it is strongest where a real one is: clean,
/// sine-like single notes high on the neck. Octave blends the rectified signal against the plain
/// fuzz. See docs/dsp/urchin.md.
class Urchin
{
public:
    struct Settings
    {
        float fuzz = 0.5f;   ///< 0..1, gain into the output stage
        float octave = 1.0f; ///< 0..1, plain fuzz to full rectified octave
        float tone = 0.5f;   ///< 0..1, dark to bright
        float level = 0.5f;  ///< 0..1, unity around 0.5
    };

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
    std::array<detail::Adaa1, kMaxChannels> rectifier_, clipper_;
    // Coupling after the rectifier (two first-order high-passes, in double at the oversampled
    // rate): the rectified signal's DC follows its envelope and must not reach the fuzz stage.
    struct Coupling
    {
        double x1 = 0.0, y1 = 0.0, x2 = 0.0, y2 = 0.0;
    };
    std::array<Coupling, kMaxChannels> coupling_ {};
    double couplingPole_ = 0.0;

    LinearSmoothed fuzzGain_;   // oversampled rate
    LinearSmoothed octaveMix_;  // oversampled rate
    LinearSmoothed outputGain_; // base rate
    ExpSmoothed tone_;

    Biquad inputHp_, transformerHp_, transformerLp_; // before the rectifier
    Biquad dcBlock_, toneLp_, fizzLp_;               // after the fuzz stage
};

} // namespace sedef::dsp
