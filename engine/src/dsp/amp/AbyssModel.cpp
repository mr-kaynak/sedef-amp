// SPDX-License-Identifier: AGPL-3.0-or-later
#include "sedef/dsp/amp/AmpModels.h"

#include <cmath>

namespace sedef::dsp::amp
{
namespace
{
// Tightness comes from keeping low end out of the clipping stages: the coupling high-passes ahead
// of V2 and V3 sit at 100 and 200 Hz, and V1's, V2's and V3's cathodes are only partly bypassed,
// so the low strings reach the cold clipper about 20 dB less driven than the upper mids. The tone
// stack and the resonance shelf restore weight after the distortion. Low-pass poles between
// stages (anode bypass and Miller capacitance) keep the stacked distortion from turning to fizz.
//
// Gain staging (docs/dsp/amps.md, sections 4 and 5): the gain pot ahead of V2 does the work. V3
// and V4 add only a few dB of saturation of their own, so at moderate gain a pick still sounds
// through the cascade instead of being squared off by two fixed stages after it (v0.3.0 drove
// them x7 and x3.5, which held V4 on the flat of its curve for 84 % of a strum at gain 0.5 and
// ran palm mutes and chords together). The knob tapers like an audio pot: the lower half is
// crunch to a tight rhythm sound, the top quarter the lead wall (10 % THD at -73 dBFS RMS).
constexpr float kV1Drive = 1.5f;
constexpr TriodeCurve kV1Curve { 1.6f, 1.0f, 0.3f };
constexpr float kV1Shift = 0.15f;

// Bright cap across the gain pot: a big treble lift at low gain that fades as gain goes up.
constexpr double kBrightHz = 3500.0;
constexpr float kBrightMaxDb = 9.0f, kBrightMinDb = 1.5f;

// V2: cold-biased clipper, the main source of grind. Its cathode resistor is bypassed by a small
// capacitor, so it has about 10 dB more gain above ~900 Hz than below ~280 Hz.
constexpr TriodeCurve kV2Curve { 1.2f, 0.6f, 0.6f };
constexpr float kV2Shift = 0.3f;
constexpr double kV2CathodeHz = 500.0;
constexpr double kV2CathodeDb = -10.0;
// Gain pot: drive = kGainMin * (kGainMax / kGainMin)^t with t = g (a + (1 - a) g), an audio-style
// taper that spends less of the travel low down and more at the top (64 dB in all).
constexpr float kGainMin = 1.6f, kGainMax = 2600.0f, kGainTaperA = 0.6f;

// A fully clipped V2 drives V3 about 8 dB and V3 drives V4 about 3 dB past their knees.
constexpr float kV3Drive = 2.5f;
constexpr TriodeCurve kV3Curve { 1.5f, 0.9f, 0.3f };
constexpr float kV3Shift = 0.25f;

constexpr float kV4Drive = 1.4f;
constexpr TriodeCurve kV4Curve { 1.5f, 0.9f, 0.25f };
constexpr float kV4Shift = 0.2f;

constexpr float kCfKnee = 0.5f, kCfInvWidth2 = 1.0f / (0.5f * 0.5f);

// Tone stack on British-style values with larger bass/mid capacitors: the scoop sits near 480 Hz
// at noon. Shift shrinks both capacitors, which moves it up to about 800 Hz.
constexpr double kStackCapLow = 47e-9, kStackCapHigh = 18e-9;
constexpr float kStackMakeup = 1.6f;

// Fixed voicing after the stack. Cutting the low end ahead of the clippers changes what they
// square, and with it the spectrum of what comes out: less below 250 Hz, more at 1-2 kHz. These
// two filters put back the v0.3.0 balance (octave bands through a cab within 0.8 dB on average),
// so the change is in how the amp moves, not in its colour.
constexpr double kWeightHz = 120.0, kWeightDb = 4.0;
constexpr double kVoiceMidHz = 1300.0, kVoiceMidQ = 0.6, kVoiceMidDb = -3.5;

// Power amp: resonance (fixed low shelf) and presence (variable high shelf) model the frequency
// dependent negative feedback, applied ahead of the output stage so they also shape its clipping.
constexpr double kResonanceHz = 100.0;
constexpr float kResonanceDb = 6.0f;
constexpr double kPresenceHz = 5000.0;
constexpr float kPresenceMaxDb = 10.0f;
constexpr float kMasterMin = 0.3f, kMasterMax = 3.5f;
constexpr PowerAmp::Config kPowerAmp { 0.18f, 0.05f, 5.0, 120.0 };

// Level follows only part of a drive increase (drive^(1 - comp) while the stages are still
// linear); the rest turns into saturation.
constexpr float kGainComp = 0.02f, kMasterComp = 0.35f;
constexpr float kMakeup = 0.497f;

float gainDrive (float g) noexcept
{
    g = std::clamp (g, 0.0f, 1.0f);
    return mapExp (g * (kGainTaperA + (1.0f - kGainTaperA) * g), kGainMin, kGainMax);
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

void AbyssModel::prepare (double sampleRate, OversamplingMode mode)
{
    prepareBase (sampleRate, mode);
    knobs_.prepare (sampleRate);
    knobs_.setRampSeconds (Bright, sampleRate, kSwitchRampSeconds);
    knobs_.setRampSeconds (Shift, sampleRate, kSwitchRampSeconds);

    inHpf_.setCoeffs (design::highpass1 (fs_, 25.0));
    v1Shelf_.setCoeffs (design::lowShelf1 (fs_, 150.0, -5.0));
    v1_.configure (kV1Curve, kV1Shift, fsOs_);
    v1Lpf_.setCoeffs (design::lowpass1 (fsOs_, 14000.0));
    gainHpf_.setCoeffs (design::highpass1 (fsOs_, 100.0));
    bright_.setHigh (fsOs_, kBrightHz);
    v2Shelf_.setCoeffs (design::lowShelf1 (fsOs_, kV2CathodeHz, kV2CathodeDb));
    v2_.configure (kV2Curve, kV2Shift, fsOs_, 1.5, 80.0);
    v2Lpf_.setCoeffs (design::lowpass1 (fsOs_, 4500.0)); // anode bypass and Miller: fizz control
    v3Hpf_.setCoeffs (design::highpass1 (fsOs_, 200.0));
    v3Shelf_.setCoeffs (design::lowShelf1 (fsOs_, 300.0, -4.0));
    v3_.configure (kV3Curve, kV3Shift, fsOs_);
    v3Lpf_.setCoeffs (design::lowpass1 (fsOs_, 4000.0));
    v4Hpf_.setCoeffs (design::highpass1 (fsOs_, 50.0));
    v4_.configure (kV4Curve, kV4Shift, fsOs_);
    v4Lpf_.setCoeffs (design::lowpass1 (fsOs_, 3000.0));
    weight_.setCoeffs (design::lowShelf1 (fsOs_, kWeightHz, kWeightDb));
    voiceMid_.setCoeffs (design::peaking (fsOs_, kVoiceMidHz, kVoiceMidQ, kVoiceMidDb));
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
    for (auto* f : { &inHpf_, &v1Shelf_, &v1Lpf_, &gainHpf_, &v2Shelf_, &v2Lpf_, &v3Hpf_, &v3Shelf_,
                     &v3Lpf_, &v4Hpf_, &v4Lpf_, &weight_, &piHpf_, &resonance_, &otLpf_ })
        f->reset();
    bright_.reset();
    presence_.reset();
    v1_.reset();
    for (auto* v : { &v2_, &v3_, &v4_ })
        v->reset();
    stack_.reset();
    voiceMid_.reset();
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
    for (auto* f : { &inHpf_, &v1Shelf_, &v1Lpf_, &gainHpf_, &v2Shelf_, &v2Lpf_, &v3Hpf_, &v3Shelf_,
                     &v3Lpf_, &v4Hpf_, &v4Lpf_, &weight_, &piHpf_, &resonance_, &otLpf_ })
        f->snapToZero();
    bright_.snapToZero();
    presence_.snapToZero();
    v1_.snapToZero();
    for (auto* v : { &v2_, &v3_, &v4_ })
        v->snapToZero();
    stack_.snapToZero();
    voiceMid_.snapToZero();
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
            x = v2Shelf_.process (ch, x);
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
            x = voiceMid_.processSample (ch, weight_.process (ch, x));
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
