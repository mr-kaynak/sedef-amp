// SPDX-License-Identifier: AGPL-3.0-or-later
#include "sedef/dsp/Jetty.h"

#include "sedef/core/Math.h"

#include <algorithm>
#include <cmath>

namespace sedef::dsp
{
namespace
{
// Bucket-brigade line: N stages clocked at f = N / (2 delay); its anti-imaging filters sit near a
// third of the clock. Long delays get darker, short ones stay open (capped at 16 kHz).
constexpr float kBbdStages = 512.0f;
constexpr float kMaxBandLimitHz = 16000.0f;
constexpr float kBandLimitQ = 0.6f;
constexpr double kLoopHpHz = 50.0; // coupling capacitor: no bass build-up in the loop

// The line's input headroom: exactly linear up to the knee, then a soft limit, so guitar
// levels stay clean while a runaway regen loop is still bounded.
constexpr float kKnee = 1.0f;
constexpr float kCeiling = 2.0f;

// The 50/50 comb takes about 4.5 dB out of a guitar, and strong regen gives some of it back
// (the comb's peaks grow by 1 / (1 - |g|)). Makeup and trim keep the guitar reference tone within
// about 1 dB across the Feedback travel (docs/dsp/jetty.md, "Level").
constexpr float kMakeup = 1.68f;
constexpr float kFeedbackTrim = 0.41f;         // times g squared, positive regen
constexpr float kNegativeFeedbackTrim = 0.59f; // negative regen rings where the guitar is louder

constexpr double kSpeedSeconds = 0.05;
constexpr double kDelaySeconds = 0.06; // a delay glide; faster would bend the pitch audibly
constexpr double kSmoothingSeconds = 0.03;
constexpr double kMaxLineMs = 20.0;
constexpr double kWetFadeSeconds = 0.005; // after a reset, see process()

inline float limit (float v) noexcept
{
    const float a = std::abs (v);
    if (a <= kKnee)
        return v;
    const float over = a - kKnee;
    const float y = kKnee + over / (1.0f + over / (kCeiling - kKnee));
    return v < 0.0f ? -y : y;
}
} // namespace

float Jetty::sweepDelayMs (float delaySettingMs, float depth, float lfo) noexcept
{
    const float d = std::clamp (delaySettingMs, kMinDelaySettingMs, kMaxDelaySettingMs) *
                    std::exp2 (kMaxSweepOctaves * std::clamp (depth, 0.0f, 1.0f) * lfo);
    return std::clamp (d, kLineMinMs, kLineMaxMs);
}

void Jetty::prepare (const ProcessSpec& spec)
{
    sampleRate_ = spec.sampleRate;
    numChannels_ = std::clamp (spec.numChannels, 1, kMaxChannels);
    const int maxDelay = static_cast<int> (std::ceil (0.001 * kMaxLineMs * sampleRate_)) + 4;
    for (auto& l : line_)
        l.prepare (maxDelay);
    lfo_.prepare (sampleRate_);
    loopHp_.setGain (tptGain (kLoopHpHz, sampleRate_));
    speed_.reset (sampleRate_, kSpeedSeconds);
    depth_.reset (sampleRate_, kSmoothingSeconds);
    delaySetting_.reset (sampleRate_, kDelaySeconds);
    feedback_.reset (sampleRate_, kSmoothingSeconds);
    fadeInv_ = static_cast<float> (1.0 / (kWetFadeSeconds * sampleRate_));
    applySettings (true);
    reset();
}

void Jetty::reset() noexcept
{
    for (auto& l : line_)
        l.reset();
    bandLimit_.reset();
    loopHp_.reset();
    lfo_.setPhase (0.0f);
    filled_ = 0;
    primed_ = false;
}

void Jetty::setSettings (const Settings& s) noexcept
{
    settings_ = s;
    applySettings (!primed_);
    primed_ = true;
}

void Jetty::applySettings (bool snap) noexcept
{
    const float speed = std::clamp (settings_.speedHz, kMinSpeedHz, kMaxSpeedHz);
    const float depth = std::clamp (settings_.depth, 0.0f, 1.0f);
    const float delay = std::clamp (settings_.delayMs, kMinDelaySettingMs, kMaxDelaySettingMs);
    const float feedback = kMaxFeedback * std::clamp (settings_.feedback, -1.0f, 1.0f);
    if (snap)
    {
        speed_.setCurrentAndTarget (speed);
        depth_.setCurrentAndTarget (depth);
        delaySetting_.setCurrentAndTarget (delay);
        feedback_.setCurrentAndTarget (feedback);
        lfo_.setFrequency (speed);
        delayMs_ = sweepDelayMs (delay, depth, 0.0f);
    }
    else
    {
        speed_.setTarget (speed);
        depth_.setTarget (depth);
        delaySetting_.setTarget (delay);
        feedback_.setTarget (feedback);
    }
}

void Jetty::process (const AudioView& io) noexcept
{
    const int n = io.numFrames();
    const int channels = std::min (io.numChannels(), numChannels_);
    const auto samplesPerMs = static_cast<float> (0.001 * sampleRate_);

    for (int i = 0; i < n; ++i)
    {
        if (speed_.isSmoothing())
            lfo_.setFrequency (speed_.next());
        // A triangle in octaves of delay: the notches sweep at a constant musical rate.
        delayMs_ =
            sweepDelayMs (delaySetting_.next(), depth_.next(), Lfo::triangle (lfo_.advance()));
        const float clockHz = kBbdStages / (0.002f * delayMs_);
        const float bandLimitG = tptGain (std::min (kMaxBandLimitHz, clockHz / 3.0f), sampleRate_);
        bandLimit_.setCoeffs (bandLimitG, 1.0f / kBandLimitQ);
        // read() before push(): a delay of d samples is read (d - 1). The band-limit filter adds
        // its own low-frequency group delay, 1 / (Q w) = 1 / (2 Q g) samples after the bilinear
        // map; reading that much sooner keeps the comb's notches where Delay puts them.
        const float delay =
            delayMs_ * samplesPerMs - 1.0f - 1.0f / (2.0f * kBandLimitQ * bandLimitG);

        // A reset line is empty: the first signal reaches its output one delay later, and would
        // arrive as a step. Fade the delayed path in over 5 ms from that moment instead.
        const float wetGain = crossfadeCurve (
            std::clamp ((static_cast<float> (filled_) - delay - 4.0f) * fadeInv_, 0.0f, 1.0f));
        if (filled_ < (1 << 30))
            ++filled_;

        const float g = feedback_.next();
        const float trim =
            kMakeup * (1.0f - (g < 0.0f ? kNegativeFeedbackTrim : kFeedbackTrim) * g * g);

        for (int ch = 0; ch < channels; ++ch)
        {
            float& x = io.channel (ch)[i];
            auto& line = line_[static_cast<size_t> (ch)];
            const float wet = wetGain * bandLimit_.process (ch, line.readCubic (delay)).low;
            const float loop = wet - loopHp_.lowpass (ch, wet);
            line.push (limit (x + g * loop));
            x = trim * 0.5f * (x + wet);
        }
    }
    bandLimit_.snapToZero();
    loopHp_.snapToZero();
}

} // namespace sedef::dsp
