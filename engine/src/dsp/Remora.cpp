// SPDX-License-Identifier: AGPL-3.0-or-later
#include "sedef/dsp/Remora.h"

#include "sedef/core/Math.h"
#include "sedef/dsp/detail/Pickup.h"

#include <algorithm>
#include <cmath>

namespace sedef::dsp
{
namespace
{
using detail::Pickup;

// The transistor pair, normalised (detail/BjtStage.h). The output transistor's collector swings
// over kSwing volts between saturation and cutoff; the bias sets where it rests in that range.
constexpr double kSwing = 0.5; // output swing at the volume pot, V (DAFx-17 waveforms)
// Where the collector rests, as the fraction of the swing already dropped across its load (which
// is also the quiescent current relative to the noon bias, halved): bias 0 starves the transistor
// at 1.5 % (little current, little gain, resting next to cutoff), noon rests mid-swing, bias 1 runs
// it hot at 94 %, next to saturation. Below noon the travel is logarithmic, so the starved end
// spreads over the knob.
constexpr double kStarvedRest = 0.015;
constexpr double kHotRest = 0.94;

// Germanium against silicon: loop gain of the pair at the noon bias (silicon's current gain is
// several times higher), saturation knee, input impedance, bandwidth and gain range.
struct Voicing
{
    double loopGain;
    double kappa;
    double inputImpedance; // Ohm, at the input transistor's base
    float bandwidthHz;
    float minGainDb, maxGainDb; // small-signal gain of the pair, fuzz 0 .. 1
};
constexpr Voicing kGermanium { 3.0, 1.0, 7.5e3, 4500.0f, 14.0f, 34.0f };
constexpr Voicing kSilicon { 24.0, 0.25, 15.0e3, 12000.0f, 22.0f, 44.0f };

constexpr double kDcBlockHz = 20.0;
constexpr double kFizzHz = 11000.0;
constexpr double kFizzQ = 0.6;
constexpr float kLevelRangeDb = 40.0f; // volume: -20..+20 dB

// Bias servo: the pair's DC feedback (through the emitter bypass capacitor) holds the average
// output current near its resting value, so a loud, lopsided note pushes the operating point
// towards cutoff and the decay that follows comes out gated until it recovers.
constexpr double kServoSeconds = 0.03;
constexpr double kServoStrength = 1.0;

constexpr double kGainRampSeconds = 0.03;
constexpr double kKnobSmoothingSeconds = 0.02;
// The germanium/silicon switch morphs between the two over this time: gain, bias servo and
// curves change together, slowly enough not to thump.
constexpr double kMaterialMorphSeconds = 0.05;

double logLerp (double a, double b, double t) noexcept
{
    return a * std::pow (b / a, t);
}

double restingPoint (double bias) noexcept
{
    return bias <= 0.5 ? kStarvedRest * std::pow (0.5 / kStarvedRest, 2.0 * bias)
                       : 0.5 + (kHotRest - 0.5) * (2.0 * bias - 1.0);
}

double loopGainAt (float material) noexcept
{
    return logLerp (kGermanium.loopGain, kSilicon.loopGain, material);
}

double inputImpedanceAt (float material) noexcept
{
    return logLerp (kGermanium.inputImpedance, kSilicon.inputImpedance, material);
}

detail::BjtShape pairShape (double bias, int material) noexcept
{
    const Voicing& v = material == 0 ? kGermanium : kSilicon;
    const double rest = restingPoint (bias);
    detail::BjtShape shape;
    shape.g = v.loopGain * rest / 0.5; // gm follows the resting current
    shape.xs = (1.0 - rest) / rest;
    shape.kappa = v.kappa;
    return shape;
}

/// The pair's curves over the bias knob, one family per transistor type. They depend on nothing
/// else (not even the sample rate), so every instance shares them; built on first use.
constexpr int kBiasCurves = 33;

const detail::BjtCurveFamily& pairCurves (int material)
{
    static const std::array<detail::BjtCurveFamily, 2> families = [] {
        std::array<detail::BjtCurveFamily, 2> f;
        for (int m = 0; m < 2; ++m)
            f[static_cast<size_t> (m)].build (kBiasCurves,
                                              [m] (double bias) { return pairShape (bias, m); });
        return f;
    }();
    return families[static_cast<size_t> (material)];
}

/// The guitar's volume pot and pickup into the fuzz's input impedance: a first-order low-pass with
/// DC gain dcGain and corner hz (pickup inductance against the resistances around it).
struct GuitarLoad
{
    double dcGain;
    double hz;
};

GuitarLoad guitarLoad (float guitarVolume, float material) noexcept
{
    const double bottom = Pickup::kVolumePot * audioTaper (guitarVolume, Pickup::kPotTaper);
    const double top = Pickup::kVolumePot - bottom;
    const double zin = inputImpedanceAt (material);
    const double shunt = bottom * zin / (bottom + zin + 1.0e-9);
    const double series = Pickup::kResistance + top + shunt;
    return { shunt / series, series / (kTwoPiD * Pickup::kInductance) };
}

/// Output trim (dB), fitted so that at volume 0.5, noon bias and the guitar on full the reference
/// tone comes out within +-0.2 dB of its input level at every fuzz setting (see
/// docs/dsp/remora.md). Rolling the guitar back or starving the bias is quieter, as it should be.
float trimDb (float fuzz, float material) noexcept
{
    const float ge = 3.80f - 1.26f * std::exp (-fuzz / 0.28f);
    const float si = 3.57f + 0.34f * fuzz;
    return -lerp (ge, si, material);
}
} // namespace

float Remora::pairGainDb (float fuzz, float material) noexcept
{
    fuzz = std::clamp (fuzz, 0.0f, 1.0f);
    const float ge = mapLin (fuzz, kGermanium.minGainDb, kGermanium.maxGainDb);
    const float si = mapLin (fuzz, kSilicon.minGainDb, kSilicon.maxGainDb);
    return lerp (ge, si, std::clamp (material, 0.0f, 1.0f));
}

void Remora::prepare (const ProcessSpec& spec)
{
    sampleRate_ = spec.sampleRate;
    numChannels_ = std::clamp (spec.numChannels, 1, kMaxChannels);
    maxBlockSize_ = std::max (spec.maxBlockSize, 1);

    oversampler_.prepare (maxBlockSize_, numChannels_, Oversampler::factorLog2For (sampleRate_));
    factor_ = oversampler_.factor();
    for (int ch = 0; ch < kMaxChannels; ++ch)
    {
        const auto c = static_cast<size_t> (ch);
        drive_[c].assign (ch < numChannels_ ? static_cast<size_t> (maxBlockSize_) : 0, 0.0f);
        drivePtrs_[c] = drive_[c].data();
    }
    biasAt_.assign (static_cast<size_t> (maxBlockSize_), 0.5f);
    materialAt_.assign (static_cast<size_t> (maxBlockSize_), 0.0f);
    servoCoeff_ = 1.0 - std::exp (-1.0 / (kServoSeconds * sampleRate_ * factor_));
    germanium_ = &pairCurves (Germanium);
    silicon_ = &pairCurves (Silicon);

    inputGain_.reset (sampleRate_, kGainRampSeconds);
    outputGain_.reset (sampleRate_, kGainRampSeconds);
    guitarKnob_.reset (sampleRate_, kKnobSmoothingSeconds);
    biasKnob_.reset (sampleRate_, kKnobSmoothingSeconds);
    materialKnob_.reset (sampleRate_, kMaterialMorphSeconds);

    dcBlock_.setCoeffs (design::highpass1 (sampleRate_, kDcBlockHz));
    fizzLp_.setCoeffs (design::lowpass (sampleRate_, kFizzHz, kFizzQ));

    applySettings (true);
    reset();
}

void Remora::reset() noexcept
{
    oversampler_.reset();
    for (auto& s : stage_)
        s.reset();
    pairBias_ = pairMaterial_ = -1.0f;
    servo_.fill (0.0);
    for (auto* f : { &guitarLp_, &dcBlock_, &bandLp_, &fizzLp_ })
        f->reset();
    primed_ = false;
}

void Remora::setSettings (const Settings& s) noexcept
{
    settings_ = s;
    applySettings (!primed_);
    primed_ = true;
}

int Remora::latencySamples() const noexcept
{
    return oversampler_.latencySamples();
}

float Remora::inputGain (float fuzz, float guitarVolume, float material) const noexcept
{
    // Drive in thermal-voltage units per volt at the base, so that the pair's small-signal gain at
    // the noon bias is pairGainDb (gain = drive * Hc / (1 + g), Hc = kSwing / 2).
    const double drive =
        dbToGain (pairGainDb (fuzz, material)) * (1.0 + loopGainAt (material)) / (0.5 * kSwing);
    return static_cast<float> (drive * guitarLoad (guitarVolume, material).dcGain);
}

void Remora::applySettings (bool snap) noexcept
{
    const float fuzz = std::clamp (settings_.fuzz, 0.0f, 1.0f);
    const float volume = std::clamp (settings_.volume, 0.0f, 1.0f);
    const float bias = std::clamp (settings_.bias, 0.0f, 1.0f);
    const float guitar = std::clamp (settings_.guitarVolume, 0.0f, 1.0f);
    const float material = settings_.transistor == Silicon ? 1.0f : 0.0f;

    const float in = inputGain (fuzz, guitar, material);
    const float out = dbToGain (trimDb (fuzz, material) + kLevelRangeDb * (volume - 0.5f));

    if (snap)
    {
        inputGain_.setCurrentAndTarget (in);
        outputGain_.setCurrentAndTarget (out);
        guitarKnob_.setCurrentAndTarget (guitar);
        biasKnob_.setCurrentAndTarget (bias);
        materialKnob_.setCurrentAndTarget (material);
        updateInput (guitar, material);
        updateBandwidth (material);
    }
    else
    {
        inputGain_.setTarget (in);
        outputGain_.setTarget (out);
        guitarKnob_.setTarget (guitar);
        biasKnob_.setTarget (bias);
        materialKnob_.setTarget (material);
    }
}

void Remora::updateInput (float guitarVolume, float material) noexcept
{
    guitarLp_.setCoeffs (design::lowpass1 (sampleRate_, guitarLoad (guitarVolume, material).hz));
}

void Remora::updatePair (float bias, float material) noexcept
{
    pairBias_ = bias;
    pairMaterial_ = material;
    const double rest = restingPoint (bias);
    cutoffHeadroom_ = kSwing * rest;
    servoGain_ = kServoStrength * (1.0 + loopGainAt (material) * rest / 0.5);
}

void Remora::updateBandwidth (float material) noexcept
{
    bandLp_.setCoeffs (design::lowpass1 (
        sampleRate_, logLerp (kGermanium.bandwidthHz, kSilicon.bandwidthHz, material)));
}

void Remora::process (const AudioView& io) noexcept
{
    // The scratch buffers hold maxBlockSize frames; split anything larger.
    if (maxBlockSize_ <= 0)
        return; // not prepared
    for (int start = 0; start < io.numFrames(); start += maxBlockSize_)
        processBlock (io.subView (start, std::min (maxBlockSize_, io.numFrames() - start)));
}

void Remora::processBlock (const AudioView& io) noexcept
{
    const int n = io.numFrames();
    const int channels = std::min (io.numChannels(), numChannels_);
    if (n == 0 || channels == 0)
        return;
    const AudioView view = io.withChannels (channels);

    // 1. Base rate: the guitar's pickup and volume pot loaded by the fuzz, then the pair's drive.
    for (int start = 0; start < n; start += kControlBlock)
    {
        const int end = std::min (n, start + kControlBlock);
        const bool guitarMoving = guitarKnob_.isSmoothing();
        const bool materialMoving = materialKnob_.isSmoothing();
        if (guitarMoving || materialMoving)
            updateInput (guitarKnob_.skip (end - start), materialKnob_.current());

        for (int i = start; i < end; ++i)
        {
            const float g = inputGain_.next();
            biasAt_[static_cast<size_t> (i)] = biasKnob_.next();
            materialAt_[static_cast<size_t> (i)] = materialKnob_.next();
            for (int ch = 0; ch < channels; ++ch)
            {
                float& x = view.channel (ch)[i];
                drive_[static_cast<size_t> (ch)][static_cast<size_t> (i)] =
                    g * guitarLp_.processSample (ch, x);
            }
        }
        if (materialMoving)
            updateBandwidth (materialKnob_.current());
    }

    // 2. Oversampled and anti-aliased: the transistor pair.
    const AudioView drive (drivePtrs_.data(), channels, n);
    const AudioView up = oversampler_.upsample (drive);
    for (int i = 0; i < n; ++i)
    {
        const auto k = static_cast<size_t> (i);
        const bool moved = biasAt_[k] != pairBias_ || materialAt_[k] != pairMaterial_;
        if (moved)
            updatePair (biasAt_[k], materialAt_[k]);
        const double bias = pairBias_;
        const double material = pairMaterial_;
        const auto curve = [this, bias, material] (double w) {
            // Germanium or silicon, blended only while the switch fades between them.
            const auto ge = germanium_->at (w, bias);
            if (material <= 0.0)
                return ge;
            const auto si = silicon_->at (w, bias);
            if (material >= 1.0)
                return si;
            return detail::BjtCurve::Value { ge.x + material * (si.x - ge.x),
                                             ge.integral + material * (si.integral - ge.integral) };
        };

        for (int ch = 0; ch < channels; ++ch)
        {
            const auto c = static_cast<size_t> (ch);
            auto& stage = stage_[c];
            double& servo = servo_[c];
            float* d = up.channel (ch) + i * factor_;
            for (int j = 0; j < factor_; ++j)
            {
                const double w = static_cast<double> (d[j]) - servoGain_ * servo;
                const double x = stage.process (w, curve, moved && j == 0);
                servo += servoCoeff_ * (x - servo);
                d[j] = static_cast<float> (cutoffHeadroom_ * x);
            }
        }
    }
    for (auto& s : servo_)
        s = flushToZero (static_cast<float> (s));
    oversampler_.downsample (drive);

    // 3. Base rate: output coupling, bandwidth and volume.
    for (int i = 0; i < n; ++i)
    {
        const float out = outputGain_.next();
        for (int ch = 0; ch < channels; ++ch)
        {
            float y = drive_[static_cast<size_t> (ch)][static_cast<size_t> (i)];
            y = dcBlock_.processSample (ch, y);
            y = bandLp_.processSample (ch, y);
            y = fizzLp_.processSample (ch, y);
            view.channel (ch)[i] = out * y;
        }
    }
}

} // namespace sedef::dsp
