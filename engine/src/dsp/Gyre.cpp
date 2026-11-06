// SPDX-License-Identifier: AGPL-3.0-or-later
#include "sedef/dsp/Gyre.h"

#include "sedef/core/Math.h"

#include <algorithm>
#include <cmath>

namespace sedef::dsp
{
namespace
{
constexpr double kSpeedSeconds = 0.05;
constexpr double kSmoothingSeconds = 0.03;
constexpr double kStagesSeconds = 0.03;

// The 50/50 mix loses about 3.5 dB of a guitar to the notches; feedback lifts the resonant peaks
// between them. Makeup and trim keep the guitar reference tone within about 2 dB of unity
// across the Feedback travel (docs/dsp/gyre.md, "Level").
constexpr float kMakeup = 1.5f;
constexpr float kFeedbackTrim = 0.5f;
} // namespace

void Gyre::prepare (const ProcessSpec& spec)
{
    sampleRate_ = spec.sampleRate;
    numChannels_ = std::clamp (spec.numChannels, 1, kMaxChannels);
    lfo_.prepare (sampleRate_);
    speed_.reset (sampleRate_, kSpeedSeconds);
    depth_.reset (sampleRate_, kSmoothingSeconds);
    feedback_.reset (sampleRate_, kSmoothingSeconds);
    six_.reset (sampleRate_, kStagesSeconds);
    applySettings (true);
    reset();
}

void Gyre::reset() noexcept
{
    for (auto& s : stages_)
        s.reset();
    feedbackState_ = {};
    lfo_.setPhase (0.0f);
    breakHz_ = kCentreHz;
    primed_ = false;
}

void Gyre::setSettings (const Settings& s) noexcept
{
    settings_ = s;
    applySettings (!primed_);
    primed_ = true;
}

void Gyre::applySettings (bool snap) noexcept
{
    const float speed = std::clamp (settings_.speedHz, kMinSpeedHz, kMaxSpeedHz);
    const float depth = std::clamp (settings_.depth, 0.0f, 1.0f);
    const float feedback = kMaxFeedback * std::clamp (settings_.feedback, 0.0f, 1.0f);
    const float six = settings_.stages == Six ? 1.0f : 0.0f;
    if (snap)
    {
        speed_.setCurrentAndTarget (speed);
        depth_.setCurrentAndTarget (depth);
        feedback_.setCurrentAndTarget (feedback);
        six_.setCurrentAndTarget (six);
        lfo_.setFrequency (speed);
    }
    else
    {
        speed_.setTarget (speed);
        depth_.setTarget (depth);
        feedback_.setTarget (feedback);
        six_.setTarget (six);
    }
}

void Gyre::process (const AudioView& io) noexcept
{
    const int n = io.numFrames();
    const int channels = std::min (io.numChannels(), numChannels_);

    for (int i = 0; i < n; ++i)
    {
        if (speed_.isSmoothing())
            lfo_.setFrequency (speed_.next());
        // A triangle in octaves: the notches move at a constant musical rate, and turn round at
        // the ends of the sweep without a step in the coefficients.
        const float sweep = kMaxSweepOctaves * depth_.next() * Lfo::triangle (lfo_.advance());
        breakHz_ = kCentreHz * std::exp2 (sweep);
        const float g = tptGain (breakHz_, sampleRate_);
        for (auto& s : stages_)
            s.setGain (g);

        const float fb = feedback_.next();
        const float six = crossfadeCurve (six_.next());
        const float trim = kMakeup * (1.0f - kFeedbackTrim * fb);

        for (int ch = 0; ch < channels; ++ch)
        {
            float& x = io.channel (ch)[i];
            auto& state = feedbackState_[static_cast<size_t> (ch)];
            // The dry half of the mix is taken after the feedback sum, so the notches stay
            // exactly where the stages put them and feedback only adds the peaks in between.
            const float v = x + fb * state;
            float a = v;
            for (int s = 0; s < 4; ++s)
                a = stages_[static_cast<size_t> (s)].allpass (ch, a);
            const float four = a;
            for (int s = 4; s < kMaxStages; ++s)
                a = stages_[static_cast<size_t> (s)].allpass (ch, a);
            const float wet = four + six * (a - four);
            state = wet;
            x = trim * 0.5f * (v + wet);
        }
    }
    for (auto& s : stages_)
        s.snapToZero();
    for (auto& s : feedbackState_)
        s = flushToZero (s);
}

} // namespace sedef::dsp
