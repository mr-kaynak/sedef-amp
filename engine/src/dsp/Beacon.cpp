// SPDX-License-Identifier: AGPL-3.0-or-later
#include "sedef/dsp/Beacon.h"

#include "sedef/core/Math.h"

#include <algorithm>
#include <cmath>

namespace sedef::dsp
{
namespace
{
// Each stage is a transistor phase splitter whose two legs are not quite balanced: the
// non-inverting leg's gain alpha and the inverting leg's beta (measured per stage by Darabundit,
// Wedelich and Bischoff, DAFx-19). H = (alpha + beta) * LP - beta: alpha at DC, -beta at high
// frequencies, so each stage is a near all-pass with a ~1 dB high shelf that moves with the sweep.
constexpr std::array<float, Beacon::kNumStages> kAlpha { 1.01f, 0.98f, 0.97f, 0.95f };
constexpr std::array<float, Beacon::kNumStages> kBeta { 1.11f, 1.09f, 1.10f, 1.09f };

// Photocell: resistance falls as a power of the light (R ~ L^-gamma), from its lit value up to a
// dark ceiling of about 300 times that (the cells measure ~7-13 kOhm lit, megohms dark).
constexpr double kGamma = 0.8;
constexpr double kMaxRatio = 300.0;

// Lamp and cells. The filament heats faster than it cools; the cells follow light quickly and
// recover slowly. Together they bend the sine LFO into a quick brightening and a long fade.
constexpr double kLampHeatSeconds = 0.012;
constexpr double kLampCoolSeconds = 0.030;
constexpr double kCellOnSeconds = 0.005;
constexpr double kCellOffSeconds = 0.045;

// Chorus mixes the stages 50/50 with the dry signal; the notches take about 3 dB out of a
// guitar, which this makes up so the two modes play at the same level (docs/dsp/beacon.md).
constexpr float kChorusTrim = 1.58f;
constexpr float kVibratoTrim = 0.95f; // the stages' shelf lifts the treble ~3 dB

// The lamp's lag shrinks the swing as the speed rises; the LFO drive grows with speed to make up
// for it (the circuit does the same, if less on purpose), clipping at the lamp's limits.
constexpr float kDriveSpeedHz = 3.0f; // drive amplitude x (1 + speed / this)

constexpr double kSpeedSeconds = 0.05;
constexpr double kDepthSeconds = 0.03;
constexpr double kModeSeconds = 0.03;

float onePole (double fs, double seconds) noexcept
{
    return static_cast<float> (std::exp (-1.0 / (seconds * fs)));
}
} // namespace

double Beacon::resistanceRatio (double light) noexcept
{
    return std::min (kMaxRatio, std::pow (std::max (light, 1.0e-6), -kGamma));
}

double Beacon::stageHz (int stage, double light) noexcept
{
    const auto s = static_cast<size_t> (std::clamp (stage, 0, kNumStages - 1));
    return 1.0 / (kTwoPiD * kLitOhms[s] * resistanceRatio (light) * kCapacitance[s]);
}

void Beacon::prepare (const ProcessSpec& spec)
{
    sampleRate_ = spec.sampleRate;
    numChannels_ = std::clamp (spec.numChannels, 1, kMaxChannels);
    lfo_.prepare (sampleRate_);
    lampHeat_ = onePole (sampleRate_, kLampHeatSeconds);
    lampCool_ = onePole (sampleRate_, kLampCoolSeconds);
    cellOn_ = onePole (sampleRate_, kCellOnSeconds);
    cellOff_ = onePole (sampleRate_, kCellOffSeconds);
    speed_.reset (sampleRate_, kSpeedSeconds);
    depth_.reset (sampleRate_, kDepthSeconds);
    vibrato_.reset (sampleRate_, kModeSeconds);
    applySettings (true);
    reset();
}

void Beacon::reset() noexcept
{
    for (auto& s : stages_)
        s.reset();
    lfo_.setPhase (0.0f);
    // Start where the lamp idles (its drive at the LFO's midpoint), not from a cold filament.
    const float idle = 0.5f * 0.5f;
    lamp_ = idle;
    cell_ = idle;
    primed_ = false;
}

void Beacon::setSettings (const Settings& s) noexcept
{
    settings_ = s;
    applySettings (!primed_);
    primed_ = true;
}

void Beacon::applySettings (bool snap) noexcept
{
    const float speed = std::clamp (settings_.speedHz, kMinSpeedHz, kMaxSpeedHz);
    const float depth = std::clamp (settings_.depth, 0.0f, 1.0f);
    const float vibrato = settings_.mode == Vibrato ? 1.0f : 0.0f;
    if (snap)
    {
        speed_.setCurrentAndTarget (speed);
        depth_.setCurrentAndTarget (depth);
        vibrato_.setCurrentAndTarget (vibrato);
        lfo_.setFrequency (speed);
    }
    else
    {
        speed_.setTarget (speed);
        depth_.setTarget (depth);
        vibrato_.setTarget (vibrato);
    }
}

void Beacon::process (const AudioView& io) noexcept
{
    const int n = io.numFrames();
    const int channels = std::min (io.numChannels(), numChannels_);

    for (int i = 0; i < n; ++i)
    {
        if (speed_.isSmoothing())
            lfo_.setFrequency (speed_.next());

        // Lamp drive: the LFO swings the lamp current about its idle point; Depth sets how
        // far. The filament's heat follows the power (current squared), the cells follow its
        // light.
        const float swing = depth_.next() * (1.0f + speed_.current() / kDriveSpeedHz);
        const float drive =
            std::clamp (0.5f * (1.0f + swing * Lfo::sine (lfo_.advance())), 0.0f, 1.0f);
        const float power = drive * drive;
        const float heat = power > lamp_ ? lampHeat_ : lampCool_;
        lamp_ = power + heat * (lamp_ - power);
        const float cell = lamp_ > cell_ ? cellOn_ : cellOff_;
        cell_ = lamp_ + cell * (cell_ - lamp_);

        // Bilinear stages without prewarping (g = w / 2fs): the phase below each break matches
        // the analog stage's at any rate, which is where these stages' notches fall. Prewarping
        // to the break instead would squeeze the 470 pF stage (14 kHz at idle, 40 kHz lit) into
        // the top octave and move the upper notch with the sample rate.
        const double ratio = resistanceRatio (cell_);
        for (size_t s = 0; s < stages_.size(); ++s)
            stages_[s].setGain (static_cast<float> (
                1.0 / (2.0 * sampleRate_ * kLitOhms[s] * ratio * kCapacitance[s])));

        const float vibrato = crossfadeCurve (vibrato_.next());
        const float dryGain = (1.0f - vibrato) * 0.5f * kChorusTrim;
        const float wetGain = dryGain + vibrato * kVibratoTrim;

        for (int ch = 0; ch < channels; ++ch)
        {
            float& x = io.channel (ch)[i];
            float a = x;
            for (size_t s = 0; s < stages_.size(); ++s)
                a = (kAlpha[s] + kBeta[s]) * stages_[s].lowpass (ch, a) - kBeta[s] * a;
            x = dryGain * x + wetGain * a;
        }
    }
    for (auto& s : stages_)
        s.snapToZero();
}

} // namespace sedef::dsp
