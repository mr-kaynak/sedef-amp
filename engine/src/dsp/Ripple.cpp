// SPDX-License-Identifier: AGPL-3.0-or-later
#include "sedef/dsp/Ripple.h"

#include "sedef/core/Math.h"

#include <algorithm>
#include <cmath>

namespace sedef::dsp
{
namespace
{
constexpr float kMinSpeedHz = 0.69f;
constexpr float kMaxSpeedHz = 7.19f;
constexpr double kCrossfadeSeconds = 0.025; // mode switches
constexpr double kSpeedSmoothingSeconds = 0.05;
constexpr double kDepthSmoothingSeconds = 0.03;
constexpr double kSwingSmoothingSeconds = 0.06;

// Tremolo: lamp-and-photocell attenuators darken quickly and recover slowly, so the dip takes
// 40 % of the cycle and the recovery 60 %; the dip is narrowed so more time is spent near full
// volume. Full depth reaches silence (100 % amplitude modulation).
constexpr float kTremoloFall = 0.4f;

// Chorus: ~9 ms centre, swing up to 3 ms but limited so the pitch deviation stays under
// 30 cents at fast speeds; 50/50 mix; the wet path is darkened like a bucket-brigade line.
constexpr double kChorusCentreMs = 9.0;
constexpr float kChorusMaxSwingSeconds = 0.003f;
constexpr float kChorusMaxCents = 30.0f;
constexpr double kChorusLpHz = 6000.0;
constexpr double kChorusLpQ = 0.6;
constexpr float kStereoPhaseOffset = 0.25f; // second channel's LFO, in cycles
constexpr float kDriftAmount = 0.15f;       // slow random wander, relative to the swing
constexpr float kDriftRateHz = 0.4f;

// Vibrato: 100 % wet; depth sets the peak pitch deviation directly, independent of speed.
constexpr float kVibratoMaxCents = 45.0f;
constexpr double kVibratoMarginMs = 0.3; // minimum delay, keeps the cubic read in range

constexpr double kMaxDelayMs = 20.0;

/// Peak delay swing (seconds) of a sine-modulated delay that bends pitch by at most `cents`.
float swingForCents (float cents, float speedHz) noexcept
{
    const float ratio = std::exp2 (cents / 1200.0f) - 1.0f;
    return ratio / (kTwoPi * speedHz);
}

/// 0 at full volume, 1 at the bottom of the dip.
float tremoloDip (float phase) noexcept
{
    const float warped = phase < kTremoloFall
                             ? 0.5f * phase / kTremoloFall
                             : 0.5f + 0.5f * (phase - kTremoloFall) / (1.0f - kTremoloFall);
    const float dip = 0.5f - 0.5f * std::cos (kTwoPi * warped);
    return dip * std::sqrt (dip);
}
} // namespace

void Ripple::prepare (const ProcessSpec& spec)
{
    sampleRate_ = spec.sampleRate;
    numChannels_ = std::clamp (spec.numChannels, 1, kMaxChannels);

    const int maxDelay = static_cast<int> (std::ceil (0.001 * kMaxDelayMs * sampleRate_)) + 4;
    for (auto& d : delay_)
        d.prepare (maxDelay);
    for (int ch = 0; ch < kMaxChannels; ++ch)
    {
        drift_[static_cast<size_t> (ch)].prepare (sampleRate_,
                                                  0x5EDEFu + static_cast<unsigned> (ch));
        drift_[static_cast<size_t> (ch)].setRate (kDriftRateHz);
    }

    lfo_.prepare (sampleRate_);
    chorusLp_.setCoeffs (design::lowpass (sampleRate_, kChorusLpHz, kChorusLpQ));
    chorusCentre_ = static_cast<float> (0.001 * kChorusCentreMs * sampleRate_);
    vibratoMargin_ = static_cast<float> (0.001 * kVibratoMarginMs * sampleRate_);

    speed_.reset (sampleRate_, kSpeedSmoothingSeconds);
    tremoloDepth_.reset (sampleRate_, kDepthSmoothingSeconds);
    chorusSwing_.reset (sampleRate_, kSwingSmoothingSeconds);
    vibratoSwing_.reset (sampleRate_, kSwingSmoothingSeconds);
    for (auto& g : modeGain_)
        g.reset (sampleRate_, kCrossfadeSeconds);

    applySettings (true);
    reset();
}

void Ripple::reset() noexcept
{
    for (auto& d : delay_)
        d.reset();
    chorusLp_.reset();
    lfo_.setPhase (0.0f);
    primed_ = false;
}

void Ripple::setSettings (const Settings& s) noexcept
{
    settings_ = s;
    applySettings (!primed_);
    primed_ = true;
}

void Ripple::applySettings (bool snap) noexcept
{
    const int mode = std::clamp (settings_.mode, 0, kNumModes - 1);
    const float speed = std::clamp (settings_.speedHz, kMinSpeedHz, kMaxSpeedHz);
    const float depth = std::clamp (settings_.depth, 0.0f, 1.0f);

    const float tremolo = 1.0f - (1.0f - depth) * (1.0f - depth); // ease-out: -12 dB dip at noon
    const auto fs = static_cast<float> (sampleRate_);
    const float chorusSwing =
        depth * std::min (kChorusMaxSwingSeconds, swingForCents (kChorusMaxCents, speed)) * fs;
    const float vibratoSwing = swingForCents (kVibratoMaxCents * depth, speed) * fs;

    if (snap)
    {
        speed_.setCurrentAndTarget (speed);
        lfo_.setFrequency (speed);
        tremoloDepth_.setCurrentAndTarget (tremolo);
        chorusSwing_.setCurrentAndTarget (chorusSwing);
        vibratoSwing_.setCurrentAndTarget (vibratoSwing);
        for (int m = 0; m < kNumModes; ++m)
            modeGain_[static_cast<size_t> (m)].setCurrentAndTarget (m == mode ? 1.0f : 0.0f);
    }
    else
    {
        speed_.setTarget (speed);
        tremoloDepth_.setTarget (tremolo);
        chorusSwing_.setTarget (chorusSwing);
        vibratoSwing_.setTarget (vibratoSwing);
        for (int m = 0; m < kNumModes; ++m)
            modeGain_[static_cast<size_t> (m)].setTarget (m == mode ? 1.0f : 0.0f);
    }
}

void Ripple::process (const AudioView& io) noexcept
{
    const int n = io.numFrames();
    const int channels = std::min (io.numChannels(), numChannels_);
    auto& tremoloGain = modeGain_[Tremolo];
    auto& chorusGain = modeGain_[Chorus];
    auto& vibratoGain = modeGain_[Vibrato];

    for (int start = 0; start < n; start += kControlBlock)
    {
        const int end = std::min (n, start + kControlBlock);
        if (speed_.isSmoothing())
            lfo_.setFrequency (speed_.skip (end - start));
        // The chorus filter only runs while chorus is audible; start it clean when it fades in.
        if (chorusGain.current() == 0.0f && chorusGain.target() > 0.0f)
            chorusLp_.reset();

        for (int i = start; i < end; ++i)
        {
            const float phase = lfo_.advance();
            const float wTremolo = tremoloGain.next();
            const float wChorus = chorusGain.next();
            const float wVibrato = vibratoGain.next();
            const float tremoloIndex = tremoloDepth_.next();
            const float chorusSwing = chorusSwing_.next();
            const float vibratoSwing = vibratoSwing_.next();

            const float tremolo = wTremolo > 0.0f ? 1.0f - tremoloIndex * tremoloDip (phase) : 0.0f;
            const float vibratoDelay =
                wVibrato > 0.0f ? vibratoMargin_ + vibratoSwing * (1.0f + std::sin (kTwoPi * phase))
                                : 0.0f;

            for (int ch = 0; ch < channels; ++ch)
            {
                auto& line = delay_[static_cast<size_t> (ch)];
                float& sample = io.channel (ch)[i];
                const float x = sample;
                line.push (x);

                float y = 0.0f;
                if (wTremolo > 0.0f)
                    y += wTremolo * tremolo * x;
                if (wChorus > 0.0f)
                {
                    const float lfo =
                        std::sin (kTwoPi * (phase + kStereoPhaseOffset * static_cast<float> (ch)));
                    const float wander = kDriftAmount * drift_[static_cast<size_t> (ch)].next();
                    const float d = chorusCentre_ + chorusSwing * (lfo + wander);
                    const float wet = chorusLp_.processSample (ch, line.readCubic (d));
                    y += wChorus * 0.5f * (x + wet);
                }
                if (wVibrato > 0.0f)
                    y += wVibrato * line.readCubic (vibratoDelay);
                sample = y;
            }
        }
    }
}

} // namespace sedef::dsp
