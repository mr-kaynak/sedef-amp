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
constexpr double kRegenHpHz = 50.0; // coupling capacitor: no bass build-up in the loop

// The line's input headroom: exactly linear up to the knee, then a soft limit, so guitar
// levels stay clean while a runaway regen loop is still bounded.
constexpr float kKnee = 1.0f;
constexpr float kCeiling = 2.0f;

// The 50/50 comb takes about 4.5 dB out of a guitar, and strong regen gives some of it back
// (the comb's peaks grow by 1 / (1 - |g|)). Makeup and trim keep the guitar reference tone within
// about 1 dB across the Regen travel (docs/dsp/jetty.md, "Level").
constexpr float kMakeup = 1.68f;
constexpr float kRegenTrim = 0.41f;         // times g squared, positive regen
constexpr float kNegativeRegenTrim = 0.59f; // negative regen rings where the guitar is louder

constexpr double kRateSeconds = 0.05;
constexpr double kManualSeconds = 0.06; // a delay glide; faster would bend the pitch audibly
constexpr double kSmoothingSeconds = 0.03;
constexpr double kMaxLineMs = 20.0;

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

float Jetty::delayMs (float manualMs, float depth, float lfo) noexcept
{
    const float d = std::clamp (manualMs, kMinManualMs, kMaxManualMs) *
                    std::exp2 (kMaxSweepOctaves * std::clamp (depth, 0.0f, 1.0f) * lfo);
    return std::clamp (d, kMinDelayMs, kMaxDelayMs);
}

void Jetty::prepare (const ProcessSpec& spec)
{
    sampleRate_ = spec.sampleRate;
    numChannels_ = std::clamp (spec.numChannels, 1, kMaxChannels);
    const int maxDelay = static_cast<int> (std::ceil (0.001 * kMaxLineMs * sampleRate_)) + 4;
    for (auto& l : line_)
        l.prepare (maxDelay);
    lfo_.prepare (sampleRate_);
    regenHp_.setGain (tptGain (kRegenHpHz, sampleRate_));
    rate_.reset (sampleRate_, kRateSeconds);
    depth_.reset (sampleRate_, kSmoothingSeconds);
    manual_.reset (sampleRate_, kManualSeconds);
    regen_.reset (sampleRate_, kSmoothingSeconds);
    applySettings (true);
    reset();
}

void Jetty::reset() noexcept
{
    for (auto& l : line_)
        l.reset();
    bandLimit_.reset();
    regenHp_.reset();
    lfo_.setPhase (0.0f);
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
    const float rate = std::clamp (settings_.rateHz, kMinRateHz, kMaxRateHz);
    const float depth = std::clamp (settings_.depth, 0.0f, 1.0f);
    const float manual = std::clamp (settings_.manualMs, kMinManualMs, kMaxManualMs);
    const float regen = kMaxRegen * std::clamp (settings_.regen, -1.0f, 1.0f);
    if (snap)
    {
        rate_.setCurrentAndTarget (rate);
        depth_.setCurrentAndTarget (depth);
        manual_.setCurrentAndTarget (manual);
        regen_.setCurrentAndTarget (regen);
        lfo_.setFrequency (rate);
        delayMs_ = delayMs (manual, depth, 0.0f);
    }
    else
    {
        rate_.setTarget (rate);
        depth_.setTarget (depth);
        manual_.setTarget (manual);
        regen_.setTarget (regen);
    }
}

void Jetty::process (const AudioView& io) noexcept
{
    const int n = io.numFrames();
    const int channels = std::min (io.numChannels(), numChannels_);
    const auto samplesPerMs = static_cast<float> (0.001 * sampleRate_);

    for (int i = 0; i < n; ++i)
    {
        if (rate_.isSmoothing())
            lfo_.setFrequency (rate_.next());
        // A triangle in octaves of delay: the notches sweep at a constant musical rate.
        delayMs_ = delayMs (manual_.next(), depth_.next(), Lfo::triangle (lfo_.advance()));
        const float clockHz = kBbdStages / (0.002f * delayMs_);
        const float bandLimitG = tptGain (std::min (kMaxBandLimitHz, clockHz / 3.0f), sampleRate_);
        bandLimit_.setCoeffs (bandLimitG, 1.0f / kBandLimitQ);
        // read() before push(): a delay of d samples is read (d - 1). The band-limit filter adds
        // its own low-frequency group delay, 1 / (Q w) = 1 / (2 Q g) samples after the bilinear
        // map; reading that much sooner keeps the comb's notches where Manual puts them.
        const float delay =
            delayMs_ * samplesPerMs - 1.0f - 1.0f / (2.0f * kBandLimitQ * bandLimitG);

        const float g = regen_.next();
        const float trim = kMakeup * (1.0f - (g < 0.0f ? kNegativeRegenTrim : kRegenTrim) * g * g);

        for (int ch = 0; ch < channels; ++ch)
        {
            float& x = io.channel (ch)[i];
            auto& line = line_[static_cast<size_t> (ch)];
            const float wet = bandLimit_.process (ch, line.readCubic (delay)).low;
            const float loop = wet - regenHp_.lowpass (ch, wet);
            line.push (limit (x + g * loop));
            x = trim * 0.5f * (x + wet);
        }
    }
    bandLimit_.snapToZero();
    regenHp_.snapToZero();
}

} // namespace sedef::dsp
