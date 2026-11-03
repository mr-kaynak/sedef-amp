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

/// "Remora" fuzz: the classic two-transistor shunt-feedback fuzz, germanium or silicon. Its low
/// input impedance loads the guitar, so the guitar's volume knob (modelled here, since Sedef sees
/// a DI) cleans it up; the bias of the output transistor sets how symmetric, or how starved and
/// gated, the clipping is. See docs/dsp/remora.md.
class Remora
{
public:
    enum Transistor
    {
        Germanium = 0,
        Silicon = 1,
    };

    struct Settings
    {
        float fuzz = 1.0f;         ///< 0..1
        float volume = 0.5f;       ///< 0..1, unity around 0.5
        float bias = 0.5f;         ///< 0..1: starved .. noon (symmetric) .. hot
        float guitarVolume = 1.0f; ///< 0..1, the guitar's volume knob (1 = full up)
        int transistor = Germanium;
    };

    void prepare (const ProcessSpec& spec);
    void reset() noexcept;
    void setSettings (const Settings& s) noexcept;
    void process (const AudioView& io) noexcept;
    int latencySamples() const noexcept;

    /// Small-signal voltage gain of the transistor pair (dB) at the given settings, from the base
    /// of the input transistor to the output. Exposed for tests and docs.
    static float pairGainDb (float fuzz, float material) noexcept;

private:
    void processBlock (const AudioView& io) noexcept;
    void applySettings (bool snap) noexcept;
    void updateInput (float guitarVolume, float material) noexcept;
    void updateBandwidth (float material) noexcept;
    void updatePair (float bias, float material) noexcept;
    float inputGain (float fuzz, float guitarVolume, float material) const noexcept;

    double sampleRate_ = 48000.0;
    int numChannels_ = 0;
    int maxBlockSize_ = 0;
    int factor_ = 1;
    Settings settings_;
    bool primed_ = false;

    Oversampler oversampler_;
    std::array<std::vector<float>, kMaxChannels> drive_;
    std::array<float*, kMaxChannels> drivePtrs_ {};
    std::vector<float> biasAt_, materialAt_; // per base-rate sample, for the oversampled stage
    std::array<detail::CurveAdaa, kMaxChannels> stage_;
    const detail::BjtCurveFamily* germanium_ = nullptr; // shared, see pairCurves()
    const detail::BjtCurveFamily* silicon_ = nullptr;
    float pairBias_ = -1.0f, pairMaterial_ = -1.0f; // the curve the stage is on
    double cutoffHeadroom_ = 0.0, servoGain_ = 0.0;
    std::array<double, kMaxChannels> servo_ {}; // slow average of the output current
    double servoCoeff_ = 0.0;

    LinearSmoothed inputGain_;  // guitar loading x the pair's drive
    LinearSmoothed outputGain_; // trim x volume
    ExpSmoothed guitarKnob_, biasKnob_, materialKnob_;

    Biquad guitarLp_;         // pickup inductance into the fuzz's input impedance
    Biquad dcBlock_, bandLp_; // output coupling, transistor bandwidth
    Biquad fizzLp_;
};

} // namespace sedef::dsp
