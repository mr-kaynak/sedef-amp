// SPDX-License-Identifier: AGPL-3.0-or-later
#include "sedef/dsp/Conch.h"

#include "sedef/core/Math.h"

#include <algorithm>
#include <cmath>

namespace sedef::dsp
{
namespace
{
// The inductor wah's small-signal response (Holters and Zoelzer, DAFx-11; docs/dsp/conch.md): a
// resonance on a fixed +6 dB/oct skirt. Below the peak every rocker position shares the same
// asymptote, |H| = f / fx, and the peak, Q * f0 / fx, stays about as high at the toe as at the
// heel: the resonance is sharp at the heel (Q ~10) and broad at the toe (Q ~1.5). So
// H = (f0 / fx) * band-pass (peak Q) with Q falling as f0 rises, Q * f0 constant; and a trim
// that keeps the guitar's level near unity at mid travel.
constexpr float kAsymptoteHz = 300.0f;
constexpr float kTrimDb = -8.5f;
constexpr float kQTilt = 2.5f; // Q ratio heel : mid and mid : toe; 2.5^2 ~ toe / heel frequency

constexpr double kRockerSeconds = 0.012; // a rocker has mass; host automation steps glide
constexpr double kQSeconds = 0.03;
constexpr double kModeSeconds = 0.04;
constexpr double kAttackSeconds = 0.004;  // Auto: quick to open on the pick...
constexpr double kReleaseSeconds = 0.150; // ...and a slower close, like a foot easing back

constexpr float kMinSensitivityDb = -6.0f;
constexpr float kSensitivityRangeDb = 42.0f;
} // namespace

float Conch::positionToHz (float position) noexcept
{
    return mapExp (position, kHeelHz, kToeHz);
}

float Conch::resonanceToQ (float resonance) noexcept
{
    return mapExp (resonance, kMinQ, kMaxQ);
}

float Conch::qAt (float resonance, float position) noexcept
{
    const float p = std::clamp (position, 0.0f, 1.0f);
    return resonanceToQ (resonance) * std::pow (kQTilt, 1.0f - 2.0f * p);
}

float Conch::autoTravel (float envelope, float sensitivity) noexcept
{
    const float gain =
        dbToGain (kMinSensitivityDb + kSensitivityRangeDb * std::clamp (sensitivity, 0.0f, 1.0f));
    return 1.0f - std::exp (-std::max (envelope, 0.0f) * gain);
}

void Conch::prepare (const ProcessSpec& spec)
{
    sampleRate_ = spec.sampleRate;
    numChannels_ = std::clamp (spec.numChannels, 1, kMaxChannels);
    envelope_.prepare (sampleRate_, kAttackSeconds, kReleaseSeconds);
    rocker_.reset (sampleRate_, kRockerSeconds);
    resonance_.reset (sampleRate_, kQSeconds);
    sensitivity_.reset (sampleRate_, kQSeconds);
    autoMix_.reset (sampleRate_, kModeSeconds);
    applySettings (true);
    reset();
}

void Conch::reset() noexcept
{
    filter_.reset();
    envelope_.reset();
    primed_ = false;
}

void Conch::setSettings (const Settings& s) noexcept
{
    settings_ = s;
    applySettings (!primed_);
    primed_ = true;
}

void Conch::applySettings (bool snap) noexcept
{
    const float position = std::clamp (settings_.position, 0.0f, 1.0f);
    const float resonance = std::clamp (settings_.resonance, 0.0f, 1.0f);
    const float sensitivity = std::clamp (settings_.sensitivity, 0.0f, 1.0f);
    const float autoMix = settings_.mode == Auto ? 1.0f : 0.0f;
    if (snap)
    {
        rocker_.setCurrentAndTarget (position);
        resonance_.setCurrentAndTarget (resonance);
        sensitivity_.setCurrentAndTarget (sensitivity);
        autoMix_.setCurrentAndTarget (autoMix);
        position_ = position;
    }
    else
    {
        rocker_.setTarget (position);
        resonance_.setTarget (resonance);
        sensitivity_.setTarget (sensitivity);
        autoMix_.setTarget (autoMix);
    }
}

void Conch::process (const AudioView& io) noexcept
{
    const int n = io.numFrames();
    const int channels = std::min (io.numChannels(), numChannels_);
    const float trim = dbToGain (kTrimDb);

    for (int i = 0; i < n; ++i)
    {
        // The envelope always runs, so switching to Auto mid-note picks up where the note is.
        float magnitude = 0.0f;
        for (int ch = 0; ch < channels; ++ch)
            magnitude = std::max (magnitude, std::abs (io.channel (ch)[i]));
        const float env = envelope_.process (magnitude);

        const float rocker = rocker_.next();
        const float resonance = resonance_.next();
        const float sensitivity = sensitivity_.next();
        const float autoMix = crossfadeCurve (autoMix_.next());
        const float travel = autoMix > 0.0f ? autoMix * autoTravel (env, sensitivity) : 0.0f;
        position_ = rocker + (1.0f - rocker) * travel;

        const float hz = positionToHz (position_);
        filter_.setCoeffs (tptGain (hz, sampleRate_), 1.0f / qAt (resonance, position_));
        const float gain = trim * hz / kAsymptoteHz;
        for (int ch = 0; ch < channels; ++ch)
        {
            float& x = io.channel (ch)[i];
            x = gain * filter_.process (ch, x).band;
        }
    }
    filter_.snapToZero();
}

} // namespace sedef::dsp
