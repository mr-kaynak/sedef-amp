// SPDX-License-Identifier: AGPL-3.0-or-later
#include "sedef/dsp/Anemone.h"

#include "sedef/core/Math.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace sedef::dsp
{
namespace
{
constexpr float kMinOctaves = 1.0f;
constexpr float kOctaveRange = 4.0f; // range 1: 5 octaves, 200 Hz to 6.4 kHz
constexpr float kMinQ = 0.7071f;
constexpr float kMaxQ = 16.0f;

constexpr float kMinSensitivityDb = -6.0f;
constexpr float kSensitivityRangeDb = 42.0f;

// Level per mode: a makeup gain and a power of Q / Qmin. Resonance makes the low- and high-pass
// peaks Q times louder and narrows the band-pass; these keep plucked notes within about 1 dB
// across the Resonance travel (low- and band-pass near unity, high-pass, which takes the
// fundamental away, about 2 dB down). Fitted in docs/dsp/anemone.md, "Level".
struct ModeLevel
{
    float makeupDb;
    float qPower;
};
constexpr std::array<ModeLevel, 3> kModeLevel {
    { { 0.0f, -0.2f }, { 7.0f, 0.45f }, { 9.7f, -0.6f } }
};

constexpr double kSmoothingSeconds = 0.03;
constexpr double kSwitchSeconds = 0.03; // mode and direction crossfades
} // namespace

float Anemone::travel (float envelope, float sensitivity) noexcept
{
    const float gain =
        dbToGain (kMinSensitivityDb + kSensitivityRangeDb * std::clamp (sensitivity, 0.0f, 1.0f));
    return 1.0f - std::exp (-std::max (envelope, 0.0f) * gain);
}

float Anemone::cutoffHz (float travel, float range, int direction) noexcept
{
    const float octaves = kMinOctaves + kOctaveRange * std::clamp (range, 0.0f, 1.0f);
    const float t = std::clamp (travel, 0.0f, 1.0f);
    return kBaseHz * std::exp2 (octaves * (direction == Down ? 1.0f - t : t));
}

float Anemone::resonanceToQ (float resonance) noexcept
{
    return mapExp (resonance, kMinQ, kMaxQ);
}

void Anemone::prepare (const ProcessSpec& spec)
{
    sampleRate_ = spec.sampleRate;
    numChannels_ = std::clamp (spec.numChannels, 1, kMaxChannels);
    envelope_.prepare (sampleRate_, 0.001 * kAttackMs, 0.001 * kReleaseMs);
    smoothCoeff_ = static_cast<float> (std::exp (-1.0 / (0.001 * kSmoothMs * sampleRate_)));
    sensitivity_.reset (sampleRate_, kSmoothingSeconds);
    range_.reset (sampleRate_, kSmoothingSeconds);
    resonance_.reset (sampleRate_, kSmoothingSeconds);
    down_.reset (sampleRate_, kSwitchSeconds);
    for (auto& g : modeGain_)
        g.reset (sampleRate_, kSwitchSeconds);
    applySettings (true);
    reset();
}

void Anemone::reset() noexcept
{
    filter_.reset();
    envelope_.reset();
    smoothed_ = 0.0f;
    primed_ = false;
}

void Anemone::setSettings (const Settings& s) noexcept
{
    settings_ = s;
    applySettings (!primed_);
    primed_ = true;
}

void Anemone::applySettings (bool snap) noexcept
{
    const float sensitivity = std::clamp (settings_.sensitivity, 0.0f, 1.0f);
    const float range = std::clamp (settings_.range, 0.0f, 1.0f);
    const float resonance = std::clamp (settings_.resonance, 0.0f, 1.0f);
    const int mode = std::clamp (settings_.mode, 0, kNumModes - 1);
    const float down = settings_.direction == Down ? 1.0f : 0.0f;
    if (snap)
    {
        sensitivity_.setCurrentAndTarget (sensitivity);
        range_.setCurrentAndTarget (range);
        resonance_.setCurrentAndTarget (resonance);
        down_.setCurrentAndTarget (down);
        for (int m = 0; m < kNumModes; ++m)
            modeGain_[static_cast<size_t> (m)].setCurrentAndTarget (m == mode ? 1.0f : 0.0f);
    }
    else
    {
        sensitivity_.setTarget (sensitivity);
        range_.setTarget (range);
        resonance_.setTarget (resonance);
        down_.setTarget (down);
        for (int m = 0; m < kNumModes; ++m)
            modeGain_[static_cast<size_t> (m)].setTarget (m == mode ? 1.0f : 0.0f);
    }
}

void Anemone::process (const AudioView& io) noexcept
{
    const int n = io.numFrames();
    const int channels = std::min (io.numChannels(), numChannels_);

    for (int i = 0; i < n; ++i)
    {
        float magnitude = 0.0f;
        for (int ch = 0; ch < channels; ++ch)
            magnitude = std::max (magnitude, std::abs (io.channel (ch)[i]));
        // Peak detector, then a short smoother: a held note's ripple at twice its frequency would
        // otherwise wobble the cutoff and add harmonics.
        const float peak = envelope_.process (magnitude);
        smoothed_ = peak + smoothCoeff_ * (smoothed_ - peak);
        const float env = smoothed_;

        const float up = travel (env, sensitivity_.next());
        const float down = crossfadeCurve (down_.next());
        // Up and Down are mirror images; a direction switch glides between them.
        const float t = up + down * (1.0f - 2.0f * up);
        cutoff_ = cutoffHz (t, range_.next(), Up);

        const float q = resonanceToQ (resonance_.next());
        filter_.setCoeffs (tptGain (cutoff_, sampleRate_), 1.0f / q);
        const float qRatio = q / kMinQ;
        const auto level = [qRatio] (int m) {
            const auto& l = kModeLevel[static_cast<size_t> (m)];
            return dbToGain (l.makeupDb) * std::pow (qRatio, l.qPower);
        };
        const float wLow = crossfadeCurve (modeGain_[LowPass].next()) * level (LowPass);
        const float wBand = crossfadeCurve (modeGain_[BandPass].next()) * level (BandPass) / q;
        const float wHigh = crossfadeCurve (modeGain_[HighPass].next()) * level (HighPass);

        for (int ch = 0; ch < channels; ++ch)
        {
            float& x = io.channel (ch)[i];
            const auto y = filter_.process (ch, x);
            x = wLow * y.low + wBand * y.band + wHigh * y.high;
        }
    }
    filter_.snapToZero();
}

} // namespace sedef::dsp
