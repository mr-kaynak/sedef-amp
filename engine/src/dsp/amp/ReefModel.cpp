// SPDX-License-Identifier: AGPL-3.0-or-later
#include "sedef/dsp/amp/AmpModels.h"

#include <cmath>

namespace sedef::dsp::amp
{
namespace
{
// Voicings morph continuously, so switching is click-free. Each sets how much low end reaches the
// clipping stages (V1 cathode bypass), the bright cap on the gain pot, a treble roll-off after
// the second stage, and some low-end weight after the tone stack. All four are parallel-form
// shelves with fixed poles; only their gains move.
struct Voicing
{
    float cathodeDb; ///< below ~kCathodeHz, ahead of V1a
    float brightDb;  ///< at gain 0; fades out as gain goes up
    float topDb;     ///< above ~kTopHz, after V1b
    float weightDb;  ///< below ~kWeightHz, after the tone stack
};

constexpr std::array<Voicing, 3> kVoicings { {
    { -7.0f, 8.0f, 0.0f, 0.0f },  // Normal: tight and bright
    { -4.0f, 4.5f, -3.0f, 1.5f }, // Thick
    { -1.5f, 1.5f, -6.0f, 3.0f }, // Thicker: full lows, soft top
} };

constexpr double kCathodeHz = 350.0;
constexpr double kBrightHz = 3500.0;
constexpr double kTopHz = 2500.0;
constexpr double kWeightHz = 110.0;

constexpr float kV1aDrive = 1.3f;
constexpr TriodeCurve kV1aCurve { 1.6f, 1.0f, 0.3f };
constexpr float kV1aShift = 0.15f;

// V1b is biased colder: strongly asymmetric clipping, the root of the crunch. With V2a it is hit
// hard at full gain (10 % THD at -47 dBFS RMS for a 220 Hz sine, about 5 mV RMS at the guitar), so
// both use the anti-aliased stage.
constexpr TriodeCurve kV1bCurve { 1.3f, 0.75f, 0.45f };
constexpr float kV1bShift = 0.3f;
constexpr float kGainMin = 0.35f, kGainMax = 60.0f;

constexpr float kV2aDrive = 2.6f;
constexpr TriodeCurve kV2aCurve { 1.5f, 0.9f, 0.3f };
constexpr float kV2aShift = 0.25f;

// Presence acts in the preamp: a high shelf before the last stage, -12 dB at 0 up to flat at 1.
constexpr double kPresenceHz = 1500.0;
constexpr float kPresenceCutDb = -12.0f;

constexpr float kCfKnee = 0.6f, kCfInvWidth2 = 1.0f / (0.7f * 0.7f);

constexpr float kStackMakeup = 2.5f;
constexpr float kMasterMin = 0.3f, kMasterMax = 3.5f;
constexpr PowerAmp::Config kPowerAmp { 0.25f, 0.08f, 6.0, 200.0 };

// Level follows only part of a drive increase (drive^(1 - comp) while the stages are still
// linear); the rest turns into saturation.
constexpr float kGainComp = 0.11f, kMasterComp = 0.35f;
constexpr float kMakeup = 0.3904f;

float gainDrive (float g) noexcept
{
    return mapExp (g, kGainMin, kGainMax);
}

float masterDrive (float m) noexcept
{
    return mapExp (m, kMasterMin, kMasterMax);
}

float presenceGain (float p) noexcept
{
    return dbToGain (kPresenceCutDb * std::pow (1.0f - p, 1.3f));
}

float outputGain (float gain, float master, float output) noexcept
{
    // Level follows a fraction of the drive increase; most of it turns into saturation.
    return kMakeup /
           (std::pow (gainDrive (gain), kGainComp) * std::pow (masterDrive (master), kMasterComp)) *
           outputLevel (output);
}

Voicing voicingAt (float v) noexcept
{
    v = std::clamp (v, 0.0f, 2.0f);
    const auto i = static_cast<size_t> (std::min (v, 1.0f));
    const float f = v - static_cast<float> (i);
    const auto& a = kVoicings[i];
    const auto& b = kVoicings[i + 1];
    return { lerp (a.cathodeDb, b.cathodeDb, f), lerp (a.brightDb, b.brightDb, f),
             lerp (a.topDb, b.topDb, f), lerp (a.weightDb, b.weightDb, f) };
}
} // namespace

void ReefModel::prepare (double sampleRate)
{
    prepareBase (sampleRate);
    knobs_.prepare (sampleRate);
    knobs_.setRampSeconds (Voice, sampleRate, kSwitchRampSeconds);

    inHpf_.setCoeffs (design::highpass1 (fs_, 15.0));
    cathode_.setLow (fs_, kCathodeHz);
    v1a_.configure (kV1aCurve, kV1aShift, fsOs_);
    v1aLpf_.setCoeffs (design::lowpass1 (fsOs_, 18000.0));
    gainHpf_.setCoeffs (design::highpass1 (fsOs_, 25.0));
    bright_.setHigh (fsOs_, kBrightHz);
    v1b_.configure (kV1bCurve, kV1bShift, fsOs_, 1.5, 80.0);
    // Anode bypass after the two hard-driven stages: keeps the top end of the extra saturation, and
    // the edges the cathode follower would clip, out of the alias range.
    v1bLpf_.setCoeffs (design::lowpass1 (fsOs_, 6000.0));
    voiceTop_.setHigh (fsOs_, kTopHz);
    v2aHpf_.setCoeffs (design::highpass1 (fsOs_, 40.0));
    presence_.setHigh (fsOs_, kPresenceHz);
    v2a_.configure (kV2aCurve, kV2aShift, fsOs_);
    v2aLpf_.setCoeffs (design::lowpass1 (fsOs_, 6000.0));
    weight_.setLow (fsOs_, kWeightHz);
    piHpf_.setCoeffs (design::highpass1 (fsOs_, 20.0));
    pa_.configure (kPowerAmp, fsOs_);
    otHpf_.setCoeffs (design::highpass (fs_, 40.0, 0.65));
    otLpf_.setCoeffs (design::lowpass1 (fs_, 15000.0));

    reset();
}

void ReefModel::reset() noexcept
{
    knobs_.snap();
    updateCoeffs();
    const Voicing v = voicingAt (knobs_[Voice]);
    cathodeG_.snap (dbToGain (v.cathodeDb));
    brightG_.snap (dbToGain (v.brightDb * std::pow (1.0f - knobs_[Gain], 1.3f)));
    voiceTopG_.snap (dbToGain (v.topDb));
    weightG_.snap (dbToGain (v.weightDb));
    presenceG_.snap (presenceGain (knobs_[Presence]));
    v1bDrive_.snap (gainDrive (knobs_[Gain]));
    paDrive_.snap (kStackMakeup * masterDrive (knobs_[Master]));
    out_.snap (outputGain (knobs_[Gain], knobs_[Master], knobs_[Output]));

    os_.reset();
    for (auto* f : { &inHpf_, &v1aLpf_, &gainHpf_, &v1bLpf_, &v2aHpf_, &v2aLpf_, &piHpf_, &otLpf_ })
        f->reset();
    for (auto* f : { &cathode_, &bright_, &voiceTop_, &presence_, &weight_ })
        f->reset();
    v1a_.reset();
    v1b_.reset();
    v2a_.reset();
    stack_.reset();
    pa_.reset();
    otHpf_.reset();
}

void ReefModel::setSettings (const ReefSettings& s) noexcept
{
    knobs_.set (Voice, static_cast<float> (std::clamp (s.voice, 0, 2)));
    knobs_.set (Gain, s.gain);
    knobs_.set (Bass, s.bass);
    knobs_.set (Mid, s.mid);
    knobs_.set (Treble, s.treble);
    knobs_.set (Presence, s.presence);
    knobs_.set (Master, s.master);
    knobs_.set (Output, s.output);
}

void ReefModel::updateCoeffs() noexcept
{
    const auto analog =
        toneStackAnalog (kBritishStack, knobs_[Treble], knobs_[Mid], logTaper (knobs_[Bass]));
    stack_.setCoeffs (toneStackDigital (analog, fsOs_));
}

void ReefModel::snapState() noexcept
{
    for (auto* f : { &inHpf_, &v1aLpf_, &gainHpf_, &v1bLpf_, &v2aHpf_, &v2aLpf_, &piHpf_, &otLpf_ })
        f->snapToZero();
    for (auto* f : { &cathode_, &bright_, &voiceTop_, &presence_, &weight_ })
        f->snapToZero();
    v1a_.snapToZero();
    v1b_.snapToZero();
    v2a_.snapToZero();
    stack_.snapToZero();
    pa_.snapToZero();
    otHpf_.snapToZero();
}

void ReefModel::processChunk (const AudioView& io) noexcept
{
    const int n = io.numFrames();
    const int channels = io.numChannels();
    if (knobs_.advance (n))
        updateCoeffs();

    const int nOs = n * os_.factor();
    const Voicing v = voicingAt (knobs_[Voice]);
    cathodeG_.begin (dbToGain (v.cathodeDb), n);
    brightG_.begin (dbToGain (v.brightDb * std::pow (1.0f - knobs_[Gain], 1.3f)), nOs);
    voiceTopG_.begin (dbToGain (v.topDb), nOs);
    weightG_.begin (dbToGain (v.weightDb), nOs);
    presenceG_.begin (presenceGain (knobs_[Presence]), nOs);
    v1bDrive_.begin (gainDrive (knobs_[Gain]), nOs);
    paDrive_.begin (kStackMakeup * masterDrive (knobs_[Master]), nOs);
    out_.begin (outputGain (knobs_[Gain], knobs_[Master], knobs_[Output]), n);

    for (int ch = 0; ch < channels; ++ch)
    {
        float* d = io.channel (ch);
        float gc = cathodeG_.current;
        for (int i = 0; i < n; ++i)
        {
            gc += cathodeG_.step;
            d[i] = cathode_.process (ch, inHpf_.process (ch, d[i]), gc);
        }
    }

    const AudioView up = os_.upsample (io);
    for (int ch = 0; ch < up.numChannels(); ++ch)
    {
        float* d = up.channel (ch);
        float gb = brightG_.current, gt = voiceTopG_.current, gw = weightG_.current;
        float gpr = presenceG_.current, gv = v1bDrive_.current, gp = paDrive_.current;
        for (int i = 0; i < nOs; ++i)
        {
            gb += brightG_.step;
            gt += voiceTopG_.step;
            gw += weightG_.step;
            gpr += presenceG_.step;
            gv += v1bDrive_.step;
            gp += paDrive_.step;
            float x = v1a_.process (ch, kV1aDrive * d[i]);
            x = v1aLpf_.process (ch, x);
            x = gainHpf_.process (ch, x);
            x = bright_.process (ch, x, gb);
            x = v1b_.process (ch, gv * x);
            x = v1bLpf_.process (ch, x);
            x = voiceTop_.process (ch, x, gt);
            x = v2aHpf_.process (ch, x);
            x = presence_.process (ch, x, gpr);
            x = v2a_.process (ch, kV2aDrive * x);
            x = v2aLpf_.process (ch, x);
            x = cathodeFollower (x, kCfKnee, kCfInvWidth2);
            x = stack_.processSample (ch, x);
            x = weight_.process (ch, x, gw);
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
            d[i] = g * otLpf_.process (ch, otHpf_.processSample (ch, d[i]));
        }
    }
    snapState();
}

} // namespace sedef::dsp::amp
