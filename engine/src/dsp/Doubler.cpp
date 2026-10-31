// SPDX-License-Identifier: AGPL-3.0-or-later
#include "sedef/dsp/Doubler.h"

#include "sedef/core/Math.h"

#include <algorithm>
#include <cmath>

namespace sedef::dsp
{
namespace
{
constexpr float kMinSpreadMs = 3.0f;
constexpr float kMaxSpreadMs = 20.0f;

// Every read sits at least this far behind the write head, which leaves room for the drift to
// swing both ways.
constexpr double kBaseSeconds = 1.5e-3;

// Timing wander: a slow random walk, at most 0.8 ms and never more than 12 % of the spread so the
// two takes cannot drift into each other at small spreads.
constexpr double kWanderMaxSeconds = 0.8e-3;
constexpr float kWanderSpreadRatio = 0.12f;
constexpr float kWanderHz = 0.3f;

// Pitch wobble: ~2-3 cents from the sine, ~1 cent from the jitter, different rates per side.
constexpr double kWobbleSeconds = 0.45e-3;
constexpr std::array<float, 2> kWobbleHz { 0.9f, 1.13f };
constexpr std::array<float, 2> kJitterHz { 1.7f, 2.1f };

constexpr double kGlideSeconds = 0.12;
constexpr double kSoftenHz = 7500.0;
// The later take is lifted slightly, partly offsetting the precedence effect pulling the image
// towards the earlier side.
constexpr float kDoubleGainDb = 1.0f;
} // namespace

void Doubler::prepare (const ProcessSpec& spec)
{
    sampleRate_ = spec.sampleRate;
    baseSamples_ = static_cast<float> (kBaseSeconds * sampleRate_);
    const double maxSeconds =
        kBaseSeconds + kMaxSpreadMs * 1.0e-3 + kWanderMaxSeconds + kWobbleSeconds + 1.0e-3;
    for (auto& t : takes_)
        t.prepare (static_cast<int> (std::ceil (maxSeconds * sampleRate_)));
    glideCoeff_ = 1.0 - std::exp (-1.0 / (kGlideSeconds * sampleRate_));
    wobbleSamples_ = static_cast<float> (kWobbleSeconds * sampleRate_);

    auto c = design::lowpass1 (sampleRate_, std::min (kSoftenHz, 0.45 * sampleRate_));
    const float g = dbToGain (kDoubleGainDb);
    c.b0 *= g;
    c.b1 *= g;
    soften_.setCoeffs (c);

    const double tickRate = sampleRate_ / kControlBlock;
    for (size_t s = 0; s < 2; ++s)
    {
        wander_[s].prepare (tickRate, 0xD0B1E000u + static_cast<std::uint32_t> (s));
        wander_[s].setRate (kWanderHz);
        jitter_[s].prepare (tickRate, 0xD0B1E100u + static_cast<std::uint32_t> (s));
        jitter_[s].setRate (kJitterHz[s]);
        wobble_[s].prepare (sampleRate_);
        wobble_[s].setFrequency (kWobbleHz[s]);
        wobble_[s].setPhase (0.37f * static_cast<float> (s));
    }
    targetSpread_ = static_cast<double> (spreadMs_) * 1.0e-3 * sampleRate_;
    reset();
}

void Doubler::reset() noexcept
{
    for (auto& t : takes_)
        t.reset();
    soften_.reset();
    spread1_ = spread2_ = targetSpread_;
    mod_ = {};
    modStep_ = {};
    tickCountdown_ = 0;
    snapNext_ = true;
}

void Doubler::setSettings (const Settings& s) noexcept
{
    spreadMs_ =
        std::isfinite (s.spreadMs) ? std::clamp (s.spreadMs, kMinSpreadMs, kMaxSpreadMs) : 6.0f;
    targetSpread_ = static_cast<double> (spreadMs_) * 1.0e-3 * sampleRate_;
    if (snapNext_)
    {
        spread1_ = spread2_ = targetSpread_;
        snapNext_ = false;
    }
}

void Doubler::controlTick() noexcept
{
    const float wanderDepth = std::min (static_cast<float> (kWanderMaxSeconds * sampleRate_),
                                        kWanderSpreadRatio * static_cast<float> (spread2_));
    for (size_t s = 0; s < 2; ++s)
    {
        wobble_[s].advance (kControlBlock);
        const float wobble = 0.6f * Lfo::sine (wobble_[s].phase()) + 0.4f * jitter_[s].next();
        const float next = wanderDepth * wander_[s].next() + wobbleSamples_ * wobble;
        modStep_[s] = (next - mod_[s]) / static_cast<float> (kControlBlock);
    }
}

void Doubler::processSamples (float* left, float* right, int length) noexcept
{
    for (int i = 0; i < length; ++i)
    {
        spread1_ += glideCoeff_ * (targetSpread_ - spread1_);
        spread2_ += glideCoeff_ * (spread1_ - spread2_);
        mod_[0] += modStep_[0];
        mod_[1] += modStep_[1];

        takes_[0].push (left[i]);
        takes_[1].push (right[i]);
        left[i] = takes_[0].readCubic (baseSamples_ + mod_[0]);
        const float later = static_cast<float> (baseSamples_ + spread2_) + mod_[1];
        right[i] = soften_.processSample (0, takes_[1].readCubic (later));
    }
}

void Doubler::process (const AudioView& io) noexcept
{
    // Doubling is a stereo effect; a mono stream passes untouched (the engine folds to mono after
    // the post chain anyway, where the double would only comb-filter).
    if (io.numChannels() < 2)
        return;
    float* left = io.channel (0);
    float* right = io.channel (1);
    const int n = io.numFrames();
    for (int pos = 0; pos < n;)
    {
        if (tickCountdown_ == 0)
        {
            controlTick();
            tickCountdown_ = kControlBlock;
        }
        const int length = std::min (n - pos, tickCountdown_);
        processSamples (left + pos, right + pos, length);
        pos += length;
        tickCountdown_ -= length;
    }
    soften_.snapToZero();
}

} // namespace sedef::dsp
