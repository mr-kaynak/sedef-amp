// SPDX-License-Identifier: AGPL-3.0-or-later
#include "sedef/dsp/amp/AmpModels.h"

#include <cmath>

namespace sedef::dsp::amp
{
namespace
{
// Tightness comes from keeping low end out of the clipping stages: the coupling high-passes ahead
// of V2 and V3 sit at 70 and 160 Hz, and V1's and V3's cathodes are only partly bypassed. The tone
// stack and the resonance shelf restore weight after the distortion. Low-pass poles between
// stages (plate and Miller capacitance) keep the stacked distortion from turning to fizz.
constexpr float kV1Drive = 1.5f;
constexpr TriodeCurve kV1Curve { 1.6f, 1.0f, 0.3f };
constexpr float kV1Shift = 0.15f;

// Bright cap across the gain pot: a big treble lift at low gain that fades as gain goes up.
constexpr double kBrightHz = 3500.0;
constexpr float kBrightMaxDb = 9.0f, kBrightMinDb = 1.5f;

// V2: cold-biased clipper, the main source of grind.
constexpr TriodeCurve kV2Curve { 1.2f, 0.6f, 0.6f };
constexpr float kV2Shift = 0.3f;
constexpr float kGainMin = 0.6f, kGainMax = 30.0f;

constexpr float kV3Drive = 3.5f;
constexpr TriodeCurve kV3Curve { 1.5f, 0.9f, 0.3f };
constexpr float kV3Shift = 0.25f;

constexpr float kV4Drive = 2.5f;
constexpr TriodeCurve kV4Curve { 1.5f, 0.9f, 0.25f };
constexpr float kV4Shift = 0.2f;

constexpr float kCfKnee = 0.5f, kCfInvWidth2 = 1.0f / (0.5f * 0.5f);

// Tone stack on British-style values with larger bass/mid capacitors: the scoop sits near 480 Hz
// at noon. Shift shrinks both capacitors, which moves it up to about 800 Hz.
constexpr double kStackCapLow = 47e-9, kStackCapHigh = 18e-9;
constexpr float kStackMakeup = 1.6f;

// Power amp: resonance (fixed low shelf) and presence (variable high shelf) model the frequency
// dependent negative feedback, applied ahead of the output stage so they also shape its clipping.
constexpr double kResonanceHz = 100.0;
constexpr float kResonanceDb = 4.5f;
constexpr double kPresenceHz = 5000.0;
constexpr float kPresenceMaxDb = 10.0f;
constexpr float kMasterMin = 0.3f, kMasterMax = 3.5f;
constexpr PowerAmp::Config kPowerAmp { 0.18f, 0.05f, 5.0, 120.0 };

// Level follows only part of a drive increase (drive^(1 - comp) while the stages are still
// linear); the rest turns into saturation.
constexpr float kGainComp = 0.08f, kMasterComp = 0.35f;
constexpr float kMakeup = 0.402f;

float gainDrive (float g) noexcept
{
    return mapExp (g, kGainMin, kGainMax);
}

float masterDrive (float m) noexcept
{
    return mapExp (m, kMasterMin, kMasterMax);
}

float brightGain (float bright, float gain) noexcept
{
    return dbToGain (bright * lerp (kBrightMaxDb, kBrightMinDb, std::sqrt (gain)));
}

float presenceGain (float p) noexcept
{
    return dbToGain (kPresenceMaxDb * p);
}

float outputGain (float gain, float master, float output) noexcept
{
    return kMakeup /
           (std::pow (gainDrive (gain), kGainComp) * std::pow (masterDrive (master), kMasterComp)) *
           outputLevel (output);
}
} // namespace

void AbyssModel::prepare (double sampleRate)
{
    prepareBase (sampleRate);
    knobs_.prepare (sampleRate);
    knobs_.setRampSeconds (Bright, sampleRate, kSwitchRampSeconds);
    knobs_.setRampSeconds (Shift, sampleRate, kSwitchRampSeconds);

    inHpf_.setCoeffs (design::highpass1 (fs_, 25.0));
    v1Shelf_.setCoeffs (design::lowShelf1 (fs_, 150.0, -5.0));
    v1_.configure (kV1Curve, kV1Shift, fsOs_);
    v1Lpf_.setCoeffs (design::lowpass1 (fsOs_, 14000.0));
    gainHpf_.setCoeffs (design::highpass1 (fsOs_, 70.0));
    bright_.setHigh (fsOs_, kBrightHz);
    v2_.configure (kV2Curve, kV2Shift, fsOs_, 1.5, 80.0);
    v2Lpf_.setCoeffs (design::lowpass1 (fsOs_, 9000.0));
    v3Hpf_.setCoeffs (design::highpass1 (fsOs_, 160.0));
    v3Shelf_.setCoeffs (design::lowShelf1 (fsOs_, 300.0, -4.0));
    v3_.configure (kV3Curve, kV3Shift, fsOs_);
    v3Lpf_.setCoeffs (design::lowpass1 (fsOs_, 7000.0));
    v4Hpf_.setCoeffs (design::highpass1 (fsOs_, 50.0));
    v4_.configure (kV4Curve, kV4Shift, fsOs_);
    v4Lpf_.setCoeffs (design::lowpass1 (fsOs_, 7000.0));
    piHpf_.setCoeffs (design::highpass1 (fsOs_, 20.0));
    resonance_.setCoeffs (design::lowShelf1 (fsOs_, kResonanceHz, kResonanceDb));
    presence_.setHigh (fsOs_, kPresenceHz);
    pa_.configure (kPowerAmp, fsOs_);
    otHpf_.setCoeffs (design::highpass (fs_, 45.0, 0.7));
    otLpf_.setCoeffs (design::lowpass1 (fs_, 14000.0));

    reset();
}

void AbyssModel::reset() noexcept
{
    knobs_.snap();
    updateCoeffs();
    brightG_.snap (brightGain (knobs_[Bright], knobs_[Gain]));
    presenceG_.snap (presenceGain (knobs_[Presence]));
    v2Drive_.snap (gainDrive (knobs_[Gain]));
    paDrive_.snap (kStackMakeup * masterDrive (knobs_[Master]));
    out_.snap (outputGain (knobs_[Gain], knobs_[Master], knobs_[Output]));

    os_.reset();
    for (auto* f : { &inHpf_, &v1Shelf_, &v1Lpf_, &gainHpf_, &v2Lpf_, &v3Hpf_, &v3Shelf_, &v3Lpf_,
                     &v4Hpf_, &v4Lpf_, &piHpf_, &resonance_, &otLpf_ })
        f->reset();
    bright_.reset();
    presence_.reset();
    for (auto* v : { &v1_, &v2_, &v3_, &v4_ })
        v->reset();
    stack_.reset();
    pa_.reset();
    otHpf_.reset();
}

void AbyssModel::setSettings (const AbyssSettings& s) noexcept
{
    knobs_.set (Bright, s.bright ? 1.0f : 0.0f);
    knobs_.set (Gain, s.gain);
    knobs_.set (Bass, s.bass);
    knobs_.set (Mid, s.mid);
    knobs_.set (Treble, s.treble);
    knobs_.set (Shift, s.shift ? 1.0f : 0.0f);
    knobs_.set (Presence, s.presence);
    knobs_.set (Master, s.master);
    knobs_.set (Output, s.output);
}

void AbyssModel::updateCoeffs() noexcept
{
    auto parts = kBritishStack;
    parts.c2 = parts.c3 =
        kStackCapLow * std::pow (kStackCapHigh / kStackCapLow, static_cast<double> (knobs_[Shift]));
    const auto analog =
        toneStackAnalog (parts, knobs_[Treble], knobs_[Mid], logTaper (knobs_[Bass]));
    stack_.setCoeffs (toneStackDigital (analog, fsOs_));
}

void AbyssModel::snapState() noexcept
{
    for (auto* f : { &inHpf_, &v1Shelf_, &v1Lpf_, &gainHpf_, &v2Lpf_, &v3Hpf_, &v3Shelf_, &v3Lpf_,
                     &v4Hpf_, &v4Lpf_, &piHpf_, &resonance_, &otLpf_ })
        f->snapToZero();
    bright_.snapToZero();
    presence_.snapToZero();
    for (auto* v : { &v1_, &v2_, &v3_, &v4_ })
        v->snapToZero();
    stack_.snapToZero();
    pa_.snapToZero();
    otHpf_.snapToZero();
}

void AbyssModel::processChunk (const AudioView& io) noexcept
{
    const int n = io.numFrames();
    const int channels = io.numChannels();
    if (knobs_.advance (n))
        updateCoeffs();

    const int nOs = n * os_.factor();
    brightG_.begin (brightGain (knobs_[Bright], knobs_[Gain]), nOs);
    presenceG_.begin (presenceGain (knobs_[Presence]), nOs);
    v2Drive_.begin (gainDrive (knobs_[Gain]), nOs);
    paDrive_.begin (kStackMakeup * masterDrive (knobs_[Master]), nOs);
    out_.begin (outputGain (knobs_[Gain], knobs_[Master], knobs_[Output]), n);

    for (int ch = 0; ch < channels; ++ch)
    {
        float* d = io.channel (ch);
        for (int i = 0; i < n; ++i)
            d[i] = v1Shelf_.process (ch, inHpf_.process (ch, d[i]));
    }

    const AudioView up = os_.upsample (io);
    for (int ch = 0; ch < up.numChannels(); ++ch)
    {
        float* d = up.channel (ch);
        float gb = brightG_.current, gpr = presenceG_.current;
        float gv = v2Drive_.current, gp = paDrive_.current;
        for (int i = 0; i < nOs; ++i)
        {
            gb += brightG_.step;
            gpr += presenceG_.step;
            gv += v2Drive_.step;
            gp += paDrive_.step;
            float x = v1_.process (ch, kV1Drive * d[i]);
            x = v1Lpf_.process (ch, x);
            x = gainHpf_.process (ch, x);
            x = bright_.process (ch, x, gb);
            x = v2_.process (ch, gv * x);
            x = v2Lpf_.process (ch, x);
            x = v3Hpf_.process (ch, x);
            x = v3Shelf_.process (ch, x);
            x = v3_.process (ch, kV3Drive * x);
            x = v3Lpf_.process (ch, x);
            x = v4Hpf_.process (ch, x);
            x = v4_.process (ch, kV4Drive * x);
            x = v4Lpf_.process (ch, x);
            x = cathodeFollower (x, kCfKnee, kCfInvWidth2);
            x = stack_.processSample (ch, x);
            x = piHpf_.process (ch, x);
            x = resonance_.process (ch, x);
            x = presence_.process (ch, x, gpr);
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
            d[i] = g * otLpf_.process (ch, otHpf_.processSample (ch, d[i]));
        }
    }
    snapState();
}

} // namespace sedef::dsp::amp
