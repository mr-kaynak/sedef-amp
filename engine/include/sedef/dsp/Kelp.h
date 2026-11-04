// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "sedef/core/AudioView.h"
#include "sedef/core/Biquad.h"
#include "sedef/core/Oversampler.h"
#include "sedef/core/Smoothed.h"

#include <array>
#include <vector>

namespace sedef::dsp
{

/// "Kelp" overdrive: the classic mid-hump op-amp overdrive. A non-inverting op-amp stage with two
/// silicon diodes across its feedback resistor clips only what a 720 Hz high-pass lets into the
/// feedback network, so the lows pass clean under a pushed, honking midrange; a 723 Hz passive
/// low-pass and an active treble control follow. The diodes are solved from the diode law
/// (quasi-static), so the knee moves with the drive the way the circuit's does. See
/// docs/dsp/kelp.md.
class Kelp
{
public:
    struct Settings
    {
        float drive = 0.5f; ///< 0..1, feedback resistance (audio taper)
        float tone = 0.5f;  ///< 0..1, treble control, dark to bright
        float level = 0.5f; ///< 0..1, unity around 0.5
    };

    void prepare (const ProcessSpec& spec);
    void reset() noexcept;
    void setSettings (const Settings& s) noexcept;
    void process (const AudioView& io) noexcept;
    int latencySamples() const noexcept;

    /// The diode pair in the feedback loop, in thermal-voltage units: solves v + a sinh v = w
    /// (w: the voltage the feedback resistor alone would develop, v: what the diodes allow) and
    /// anti-aliases it to first order with the closed-form antiderivative. Exposed for tests.
    class FeedbackDiodes
    {
    public:
        static double solve (double w, double a) noexcept;
        static double antiderivative (double w, double a) noexcept;

        void reset() noexcept { x1_ = f1_ = 0.0; }
        /// Changes the curve; the stored antiderivative is re-evaluated so ADAA stays consistent.
        void setA (double a) noexcept;
        double a() const noexcept { return a_; }
        float process (float w) noexcept;

    private:
        double a_ = 0.01;
        double x1_ = 0.0, f1_ = 0.0; // previous input and its antiderivative
    };

    /// Small-signal gain of the clipping stage's feedback path, Rf / R4 (for tests and docs).
    static double feedbackResistance (float drive) noexcept;

private:
    void processBlock (const AudioView& io) noexcept;
    void applySettings (bool snap) noexcept;
    void updateDrive (float drive) noexcept;

    double sampleRate_ = 48000.0;
    int numChannels_ = 0;
    int maxBlockSize_ = 0;
    int latency_ = 0;
    Settings settings_;
    bool primed_ = false;

    Oversampler oversampler_;
    AlignedDryPath clean_; // lines the clean term up with the clipper
    std::array<std::vector<float>, kMaxChannels> drive_;
    std::array<float*, kMaxChannels> drivePtrs_ {};
    std::array<FeedbackDiodes, kMaxChannels> diodes_;
    std::vector<float> curveA_; // per control block: the diode curve parameter

    LinearSmoothed driveGain_;  // Rf / R4 / nVt: base-rate input to thermal-voltage units
    LinearSmoothed outputGain_; // trim x level
    LinearSmoothed treble_;     // the tone control's high-pass blend, G - 1
    ExpSmoothed driveKnob_;

    Biquad inputHp_;    // input coupling
    Biquad feedbackHp_; // R4 + C3 to ground: what the feedback network sees
    Biquad feedbackLp_; // Cf across Rf: the corner falls as the drive rises
    Biquad toneLp_, toneHp_, dcBlock_;
};

} // namespace sedef::dsp
