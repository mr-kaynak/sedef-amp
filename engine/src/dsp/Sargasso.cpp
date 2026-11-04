// SPDX-License-Identifier: AGPL-3.0-or-later
#include "sedef/dsp/Sargasso.h"

#include "sedef/core/Math.h"
#include "sedef/dsp/detail/Bilinear.h"

#include <algorithm>
#include <cmath>

namespace sedef::dsp
{
namespace
{
// Input stage: coupling, then the gain Sustain sets into the first clipping stage, band-limited by
// the stage's feedback capacitor.
constexpr double kInputHpHz = 40.0;
constexpr float kMinDriveDb = 6.0f;    // sustain 0
constexpr float kDriveRangeDb = 40.0f; // sustain 1: +46 dB
constexpr double kStageBandwidthHz = 3500.0;

// Clipping stages: a common-emitter stage with antiparallel silicon diodes across its feedback
// resistor. Normalised to the diodes' knee (kKnee volts at the stage output).
constexpr float kKnee = 0.45f;
constexpr double kNegativeKnee = 0.92; // the transistor's bias sits slightly off centre
constexpr double kTail = 0.15;         // the diode law's slow rise above the knee ...
constexpr double kTailLimit = 4.0;     // ... until the transistor runs out of swing
constexpr double kCouplingHz = 90.0;   // between the two clipping stages
constexpr float kSecondStageGain = 10.0f;

// Recovery stage after the tone network.
constexpr float kRecoveryGain = 4.0f;
constexpr double kRecoveryHpHz = 30.0;
constexpr double kOutputLpHz = 8000.0;
constexpr double kOutputLpQ = 0.6;
constexpr float kLevelRangeDb = 40.0f; // volume: -20..+20 dB

constexpr double kGainRampSeconds = 0.03;
constexpr double kFilterSmoothingSeconds = 0.02;

// Diodes in the feedback path: a soft algebraic knee, and the diode law's slow rise above it until
// the transistor's swing bounds it. Slope 1 at zero, odd; the negative half has a slightly earlier
// knee. Square roots only, so the antiderivative is cheap.
inline double diodeCurve (double a) noexcept
{
    const double b = a / kTailLimit;
    return (1.0 - kTail) * a / std::sqrt (1.0 + a * a) + kTail * a / std::sqrt (1.0 + b * b);
}

inline double diodeCurveIntegral (double a) noexcept
{
    const double b = a / kTailLimit;
    return (1.0 - kTail) * (std::sqrt (1.0 + a * a) - 1.0) +
           kTail * kTailLimit * kTailLimit * (std::sqrt (1.0 + b * b) - 1.0);
}

inline double clip (double r) noexcept
{
    return r >= 0.0 ? diodeCurve (r) : -kNegativeKnee * diodeCurve (-r / kNegativeKnee);
}

inline double clipIntegral (double r) noexcept
{
    return r >= 0.0 ? diodeCurveIntegral (r)
                    : kNegativeKnee * kNegativeKnee * diodeCurveIntegral (-r / kNegativeKnee);
}

/// Output trim (dB), fitted so that at volume 0.5 the reference tone comes out within +-0.2 dB of
/// its input level at every sustain setting, tone at noon (see docs/dsp/sargasso.md).
float trimDb (float sustain) noexcept
{
    return -(14.05f - 1.66f * std::exp (-sustain / 0.25f));
}
} // namespace

Sargasso::ToneNetwork Sargasso::toneNetwork (float tone) noexcept
{
    // Nodal analysis. The source drives the low-pass branch (series R1, shunt C1, node L) and the
    // high-pass branch (series C2, shunt R2, node H); the pot runs from L to H, and its wiper W
    // feeds the recovery stage's input resistance. With the wiper a fraction t from L:
    //   Ga = 1 / (t Rp), Gb = 1 / ((1 - t) Rp), S = Ga + Gb + GL
    //   P = (G1 + Ga + s C1) S - Ga^2, Q = (G2 + Gb + s C2) S - Gb^2, K = Ga Gb
    //   V_W / V_in = [Ga (G1 Q + K s C2) + Gb (s C2 P + K G1)] / (P Q - K^2)
    ToneNetwork net;
    const double t = std::clamp (static_cast<double> (tone), 1.0e-4, 1.0 - 1.0e-4);
    const double g1 = 1.0 / ToneNetwork::kLowPassR;
    const double g2 = 1.0 / ToneNetwork::kHighPassR;
    const double c1 = ToneNetwork::kLowPassC;
    const double c2 = ToneNetwork::kHighPassC;
    const double ga = 1.0 / (t * ToneNetwork::kPot);
    const double gb = 1.0 / ((1.0 - t) * ToneNetwork::kPot);
    const double sum = ga + gb + 1.0 / ToneNetwork::kLoad;
    const double k = ga * gb;
    const double p0 = (g1 + ga) * sum - ga * ga, p1 = c1 * sum;
    const double q0 = (g2 + gb) * sum - gb * gb, q1 = c2 * sum;
    net.denominator = { p0 * q0 - k * k, p0 * q1 + p1 * q0, p1 * q1 };
    net.numerator = { ga * g1 * q0 + gb * k * g1, ga * g1 * q1 + ga * k * c2 + gb * c2 * p0,
                      gb * c2 * p1 };
    return net;
}

void Sargasso::prepare (const ProcessSpec& spec)
{
    sampleRate_ = spec.sampleRate;
    numChannels_ = std::clamp (spec.numChannels, 1, kMaxChannels);
    maxBlockSize_ = std::max (spec.maxBlockSize, 1);
    oversampler_.prepare (maxBlockSize_, numChannels_, Oversampler::factorLog2For (sampleRate_),
                          spec.oversampling);
    const double osRate = sampleRate_ * oversampler_.factor();

    driveGain_.reset (sampleRate_, kGainRampSeconds);
    outputGain_.reset (sampleRate_, kGainRampSeconds);
    tone_.reset (sampleRate_, kFilterSmoothingSeconds);

    inputHp_.setCoeffs (design::highpass1 (sampleRate_, kInputHpHz));
    bandA_.setCoeffs (design::lowpass1 (sampleRate_, kStageBandwidthHz));
    couplingAB_.setCoeffs (design::highpass1 (osRate, kCouplingHz));
    bandB_.setCoeffs (design::lowpass1 (osRate, kStageBandwidthHz));
    recoveryHp_.setCoeffs (design::highpass1 (sampleRate_, kRecoveryHpHz));
    outLp_.setCoeffs (design::lowpass (sampleRate_, kOutputLpHz, kOutputLpQ));

    applySettings (true);
    reset();
}

void Sargasso::reset() noexcept
{
    oversampler_.reset();
    for (auto* group : { &clipA_, &clipB_ })
        for (auto& c : *group)
            c.reset();
    for (auto* f : { &inputHp_, &bandA_, &couplingAB_, &bandB_, &toneNet_, &recoveryHp_, &outLp_ })
        f->reset();
    primed_ = false;
}

void Sargasso::setSettings (const Settings& s) noexcept
{
    settings_ = s;
    applySettings (!primed_);
    primed_ = true;
}

int Sargasso::latencySamples() const noexcept
{
    return oversampler_.latencySamples();
}

void Sargasso::applySettings (bool snap) noexcept
{
    const float sustain = std::clamp (settings_.sustain, 0.0f, 1.0f);
    const float tone = std::clamp (settings_.tone, 0.0f, 1.0f);
    const float volume = std::clamp (settings_.volume, 0.0f, 1.0f);

    const float drive = dbToGain (kMinDriveDb + kDriveRangeDb * sustain) / kKnee;
    const float output =
        kKnee * kRecoveryGain * dbToGain (trimDb (sustain) + kLevelRangeDb * (volume - 0.5f));

    if (snap)
    {
        driveGain_.setCurrentAndTarget (drive);
        outputGain_.setCurrentAndTarget (output);
        tone_.setCurrentAndTarget (tone);
        updateTone (tone);
    }
    else
    {
        driveGain_.setTarget (drive);
        outputGain_.setTarget (output);
        tone_.setTarget (tone);
    }
}

void Sargasso::updateTone (float tone) noexcept
{
    const auto net = toneNetwork (tone);
    toneNet_.setCoeffs (detail::bilinear (net.numerator, net.denominator, sampleRate_, 1000.0));
}

void Sargasso::process (const AudioView& io) noexcept
{
    // The oversampler holds maxBlockSize frames; split anything larger.
    if (maxBlockSize_ <= 0)
        return; // not prepared
    for (int start = 0; start < io.numFrames(); start += maxBlockSize_)
        processBlock (io.subView (start, std::min (maxBlockSize_, io.numFrames() - start)));
}

void Sargasso::processBlock (const AudioView& io) noexcept
{
    const int n = io.numFrames();
    const int channels = std::min (io.numChannels(), numChannels_);
    if (n == 0 || channels == 0)
        return;
    const AudioView view = io.withChannels (channels);

    // 1. Base rate: input stage and the sustain gain, in place.
    for (int i = 0; i < n; ++i)
    {
        const float g = driveGain_.next();
        for (int ch = 0; ch < channels; ++ch)
        {
            float& x = view.channel (ch)[i];
            x = bandA_.processSample (ch, g * inputHp_.processSample (ch, x));
        }
    }

    // 2. Oversampled and anti-aliased: both clipping stages and the coupling between them.
    const AudioView up = oversampler_.upsample (view);
    for (int ch = 0; ch < channels; ++ch)
    {
        auto& a = clipA_[static_cast<size_t> (ch)];
        auto& b = clipB_[static_cast<size_t> (ch)];
        float* d = up.channel (ch);
        for (int i = 0; i < up.numFrames(); ++i)
        {
            const float first = a.process (d[i], clip, clipIntegral);
            const float between =
                bandB_.processSample (ch, kSecondStageGain * couplingAB_.processSample (ch, first));
            d[i] = b.process (between, clip, clipIntegral);
        }
    }
    oversampler_.downsample (view);

    // 3. Base rate: tone network, recovery stage and volume.
    for (int start = 0; start < n; start += kControlBlock)
    {
        const int end = std::min (n, start + kControlBlock);
        if (tone_.isSmoothing())
            updateTone (tone_.skip (end - start));

        for (int i = start; i < end; ++i)
        {
            const float out = outputGain_.next();
            for (int ch = 0; ch < channels; ++ch)
            {
                float& x = view.channel (ch)[i];
                float y = toneNet_.processSample (ch, x);
                y = recoveryHp_.processSample (ch, y);
                y = outLp_.processSample (ch, y);
                x = out * y;
            }
        }
    }
}

} // namespace sedef::dsp
