// SPDX-License-Identifier: AGPL-3.0-or-later
#include "sedef/dsp/amp/AmpModels.h"

#include <cmath>

namespace sedef::dsp::amp
{
namespace
{
// Voicing. Signal levels are normalised so a triode bends audibly around |x| = 1; a guitar DI
// peaking at -6 dBFS reaches about a third of that at V1.
constexpr float kV1Drive = 0.8f;
constexpr TriodeCurve kV1Curve { 1.8f, 1.1f, 0.22f };
constexpr float kV1Shift = 0.12f;

// Tone stack: treble and mid are fixed at a bright, scooped setting; the stack loses ~12 dB in
// the mids, made up before V2.
constexpr double kTreble = 0.68, kMid = 0.32;
constexpr float kStackMakeup = 3.2f;

// Volume pot with a bright cap across it: the cap bypasses the pot's top section, so the treble
// lift is large at low volume and vanishes when the pot is fully up.
constexpr double kBrightHz = 3500.0;
constexpr float kBrightDb = 7.0f;

constexpr TriodeCurve kV2Curve { 1.5f, 0.95f, 0.2f };
constexpr float kV2Shift = 0.22f;
constexpr float kVolumeMin = 0.12f, kVolumeMax = 7.0f;

// Power amp: master sets the drive into the output stage.
constexpr float kMasterMin = 0.3f, kMasterMax = 3.5f;
constexpr PowerAmp::Config kPowerAmp { 0.12f, 0.04f, 10.0, 180.0 };

// Turning volume or master up raises the level only partly (square root of the drive); the rest
// becomes saturation, so the knobs feel like an amp without jumping by tens of decibels.
constexpr float kMakeup = 1.64f;

float volumeDrive (float v) noexcept
{
    return mapExp (v, kVolumeMin, kVolumeMax);
}

float masterDrive (float v) noexcept
{
    return mapExp (v, kMasterMin, kMasterMax);
}

float brightGain (float volume) noexcept
{
    return dbToGain (kBrightDb * std::pow (1.0f - volume, 1.5f));
}

float outputGain (float volume, float master, float output) noexcept
{
    return kMakeup / std::sqrt (volumeDrive (volume) * masterDrive (master)) * outputLevel (output);
}
} // namespace

void LagoonModel::prepare (double sampleRate)
{
    prepareBase (sampleRate);
    knobs_.prepare (sampleRate);

    inHpf_.setCoeffs (design::highpass1 (fs_, 10.0));
    v1_.configure (kV1Curve, kV1Shift, fsOs_);
    v1Lpf_.setCoeffs (design::lowpass1 (fsOs_, 20000.0)); // Miller capacitance
    bright_.setHigh (fsOs_, kBrightHz);
    v2Hpf_.setCoeffs (design::highpass1 (fsOs_, 18.0));
    v2Shelf_.setCoeffs (design::lowShelf1 (fsOs_, 90.0, -3.0)); // partially bypassed cathode
    v2_.configure (kV2Curve, kV2Shift, fsOs_);
    v2Lpf_.setCoeffs (design::lowpass1 (fsOs_, 16000.0));
    piHpf_.setCoeffs (design::highpass1 (fsOs_, 15.0));
    pa_.configure (kPowerAmp, fsOs_);
    otHpf_.setCoeffs (design::highpass (fs_, 35.0, 0.6)); // output transformer bandwidth
    otLpf_.setCoeffs (design::lowpass1 (fs_, 16000.0));

    reset();
}

void LagoonModel::reset() noexcept
{
    knobs_.snap();
    updateCoeffs();
    brightG_.snap (brightGain (knobs_[Volume]));
    v2Drive_.snap (kStackMakeup * volumeDrive (knobs_[Volume]));
    paDrive_.snap (masterDrive (knobs_[Master]));
    out_.snap (outputGain (knobs_[Volume], knobs_[Master], knobs_[Output]));

    os_.reset();
    for (auto* f : { &inHpf_, &v1Lpf_, &v2Hpf_, &v2Shelf_, &v2Lpf_, &piHpf_, &otLpf_, &cut_ })
        f->reset();
    bright_.reset();
    v1_.reset();
    v2_.reset();
    stack_.reset();
    pa_.reset();
    otHpf_.reset();
}

void LagoonModel::setSettings (const LagoonSettings& s) noexcept
{
    knobs_.set (Volume, s.volume);
    knobs_.set (Bass, s.bass);
    knobs_.set (Cut, s.cut);
    knobs_.set (Master, s.master);
    knobs_.set (Output, s.output);
}

void LagoonModel::updateCoeffs() noexcept
{
    const auto analog = toneStackAnalog (kTweedStack, kTreble, kMid, logTaper (knobs_[Bass]));
    stack_.setCoeffs (toneStackDigital (analog, fsOs_));

    // Cut: a gentle first-order high cut from 12 kHz (barely there) down to 3 kHz.
    cut_.setCoeffs (design::lowpass1 (fs_, mapExp (knobs_[Cut], 12000.0f, 3000.0f)));
}

void LagoonModel::snapState() noexcept
{
    for (auto* f : { &inHpf_, &v1Lpf_, &v2Hpf_, &v2Shelf_, &v2Lpf_, &piHpf_, &otLpf_, &cut_ })
        f->snapToZero();
    bright_.snapToZero();
    v1_.snapToZero();
    v2_.snapToZero();
    stack_.snapToZero();
    pa_.snapToZero();
    otHpf_.snapToZero();
}

void LagoonModel::processChunk (const AudioView& io) noexcept
{
    const int n = io.numFrames();
    const int channels = io.numChannels();
    if (knobs_.advance (n))
        updateCoeffs();

    const int nOs = n * os_.factor();
    brightG_.begin (brightGain (knobs_[Volume]), nOs);
    v2Drive_.begin (kStackMakeup * volumeDrive (knobs_[Volume]), nOs);
    paDrive_.begin (masterDrive (knobs_[Master]), nOs);
    out_.begin (outputGain (knobs_[Volume], knobs_[Master], knobs_[Output]), n);

    for (int ch = 0; ch < channels; ++ch)
    {
        float* d = io.channel (ch);
        for (int i = 0; i < n; ++i)
            d[i] = inHpf_.process (ch, d[i]);
    }

    const AudioView up = os_.upsample (io);
    for (int ch = 0; ch < up.numChannels(); ++ch)
    {
        float* d = up.channel (ch);
        float gb = brightG_.current, g2 = v2Drive_.current, gp = paDrive_.current;
        for (int i = 0; i < nOs; ++i)
        {
            gb += brightG_.step;
            g2 += v2Drive_.step;
            gp += paDrive_.step;
            float x = v1_.process (ch, kV1Drive * d[i]);
            x = v1Lpf_.process (ch, x);
            x = stack_.processSample (ch, x);
            x = bright_.process (ch, x, gb);
            x = v2Hpf_.process (ch, x);
            x = v2Shelf_.process (ch, x);
            x = v2_.process (ch, g2 * x);
            x = v2Lpf_.process (ch, x);
            x = piHpf_.process (ch, x);
            d[i] = pa_.process (ch, gp * x);
        }
    }
    os_.downsample (io);

    for (int ch = 0; ch < channels; ++ch)
    {
        float* d = io.channel (ch);
        float g = out_.current;
        for (int i = 0; i < n; ++i)
        {
            g += out_.step;
            const float x = otLpf_.process (ch, otHpf_.processSample (ch, d[i]));
            d[i] = g * cut_.process (ch, x);
        }
    }
    snapState();
}

} // namespace sedef::dsp::amp
