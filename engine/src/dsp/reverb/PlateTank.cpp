// SPDX-License-Identifier: AGPL-3.0-or-later
#include "PlateTank.h"

namespace sedef::dsp::reverb
{
namespace
{
// Dattorro (1997), Table 1 and Fig. 1. Lengths in samples at 29761 Hz.
constexpr double kRefRate = 29761.0;
constexpr std::array<int, 4> kDiffuserLengths { 142, 107, 379, 277 };
constexpr float kInputDiffusion1 = 0.75f;
constexpr float kInputDiffusion2 = 0.625f;
// The tank's first all-passes have the opposite sign in the figure.
constexpr float kDecayDiffusion1 = -0.70f;

constexpr int kLeftAp1 = 672, kLeftDelay1 = 4453, kLeftAp2 = 1800, kLeftDelay2 = 3720;
constexpr int kRightAp1 = 908, kRightDelay1 = 4217, kRightAp2 = 2656, kRightDelay2 = 3163;

// Peak excursion of the modulated all-passes, in reference samples (the paper allows up to 16).
constexpr int kExcursion = 10;
constexpr float kLfoHz = 0.9f;
constexpr float kWanderHz = 0.35f;

// Output tap offsets, Table 2: left output then right output.
constexpr std::array<int, 14> kTaps { 266, 2974, 1913, 1996, 1990, 187, 1066,
                                      353, 3627, 1228, 2673, 2111, 335, 121 };
constexpr float kTapGain = 0.6f;
} // namespace

void PlateTank::prepare (double sampleRate)
{
    fs_ = sampleRate;
    auto scale = [&] (int n) {
        return scaleLength (n, kRefRate, fs_);
    };

    for (size_t i = 0; i < diffusers_.size(); ++i)
        diffusers_[i].prepare (scale (kDiffuserLengths[i]));

    const int excursion = scale (kExcursion);
    excursion_ = static_cast<float> (excursion);
    apL1_.prepare (scale (kLeftAp1), excursion + 2);
    apR1_.prepare (scale (kRightAp1), excursion + 2);
    apL2_.prepare (scale (kLeftAp2));
    apR2_.prepare (scale (kRightAp2));

    lenL1_ = scale (kLeftDelay1);
    lenL2_ = scale (kLeftDelay2);
    lenR1_ = scale (kRightDelay1);
    lenR2_ = scale (kRightDelay2);
    dL1_.prepare (lenL1_);
    dL2_.prepare (lenL2_);
    dR1_.prepare (lenR1_);
    dR2_.prepare (lenR2_);

    for (size_t i = 0; i < taps_.size(); ++i)
        taps_[i] = scale (kTaps[i]);

    // One trip around the figure eight passes all eight delays (an all-pass delays a flat
    // spectrum by D on average) and four decay gains.
    const int loop = apL1_.delay() + lenL1_ + apL2_.delay() + lenL2_ + apR1_.delay() + lenR1_ +
                     apR2_.delay() + lenR2_;
    loopSeconds_ = loop / fs_;

    const double controlRate = fs_ / kControlBlock;
    lfo_.prepare (controlRate);
    lfo_.setFrequency (kLfoHz);
    wanderL_.prepare (controlRate, 0x5EDEF001u);
    wanderR_.prepare (controlRate, 0x5EDEF002u);
    wanderL_.setRate (kWanderHz);
    wanderR_.setRate (kWanderHz * 1.37f);

    setDecay (2.5f);
    setDamping (12000.0f);
    setBandwidth (18000.0f);
    clear();
}

void PlateTank::clear() noexcept
{
    bandwidth_.clear();
    dampL_.clear();
    dampR_.clear();
    for (auto& d : diffusers_)
        d.clear();
    apL1_.clear();
    apL2_.clear();
    apR1_.clear();
    apR2_.clear();
    dL1_.reset();
    dL2_.reset();
    dR1_.reset();
    dR2_.reset();
    lfo_.setPhase (0.0f);
    modL_ = modR_ = modStepL_ = modStepR_ = 0.0f;
}

void PlateTank::setDecay (float rt60Seconds) noexcept
{
    // Four decay multiplications per loop: decay^4 = loop gain.
    const double loopGain = loopGainForRt60 (loopSeconds_, std::max (rt60Seconds, 0.05f));
    decay_ = static_cast<float> (std::pow (loopGain, 0.25));
    // Dattorro: decay diffusion 2 = decay + 0.15, clamped to [0.25, 0.5].
    decayDiffusion2_ = std::clamp (decay_ + 0.15f, 0.25f, 0.5f);
}

void PlateTank::setDamping (float hz) noexcept
{
    dampL_.setCutoff (fs_, hz);
    dampR_.setCutoff (fs_, hz);
}

void PlateTank::setBandwidth (float hz) noexcept
{
    bandwidth_.setCutoff (fs_, hz);
}

void PlateTank::controlTick() noexcept
{
    const float phase = lfo_.advance();
    const float s = Lfo::sine (phase);
    const float c = Lfo::sine (phase + 0.25f);
    const float targetL = excursion_ * (0.7f * s + 0.3f * wanderL_.next());
    const float targetR = excursion_ * (0.7f * c + 0.3f * wanderR_.next());
    constexpr float inv = 1.0f / static_cast<float> (kControlBlock);
    modStepL_ = (targetL - modL_) * inv;
    modStepR_ = (targetR - modR_) * inv;

    dampL_.snapToZero();
    dampR_.snapToZero();
    bandwidth_.snapToZero();
}

void PlateTank::process (const float* inL, const float* inR, float* outL, float* outR,
                         int n) noexcept
{
    const float decay = decay_;
    const float dd2 = decayDiffusion2_;
    const auto& t = taps_;

    for (int i = 0; i < n; ++i)
    {
        float x = bandwidth_.process (0.5f * (inL[i] + inR[i]));
        x = diffusers_[0].process (x, kInputDiffusion1);
        x = diffusers_[1].process (x, kInputDiffusion1);
        x = diffusers_[2].process (x, kInputDiffusion2);
        x = diffusers_[3].process (x, kInputDiffusion2);

        modL_ += modStepL_;
        modR_ += modStepR_;

        // Each half of the figure eight is fed by the other half's last delay.
        const float tailL = dL2_.read (lenL2_ - 1);
        const float tailR = dR2_.read (lenR2_ - 1);

        float l = apL1_.processModulated (x + decay * tailR, kDecayDiffusion1, modL_);
        dL1_.push (l);
        l = dampL_.process (dL1_.read (lenL1_)) * decay;
        l = apL2_.process (l, dd2);
        dL2_.push (flushToZero (l));

        float r = apR1_.processModulated (x + decay * tailL, kDecayDiffusion1, modR_);
        dR1_.push (r);
        r = dampR_.process (dR1_.read (lenR1_)) * decay;
        r = apR2_.process (r, dd2);
        dR2_.push (flushToZero (r));

        // Table 2 taps (node names in the paper: 24_30 = dL1, 31_33 = apL2, 33_39 = dL2,
        // 48_54 = dR1, 55_59 = apR2, 59_63 = dR2).
        outL[i] =
            kTapGain * (dR1_.read (t[0]) + dR1_.read (t[1]) - apR2_.tap (t[2]) + dR2_.read (t[3]) -
                        dL1_.read (t[4]) - apL2_.tap (t[5]) - dL2_.read (t[6]));
        outR[i] =
            kTapGain * (dL1_.read (t[7]) + dL1_.read (t[8]) - apL2_.tap (t[9]) + dL2_.read (t[10]) -
                        dR1_.read (t[11]) - apR2_.tap (t[12]) - dR2_.read (t[13]));
    }
}

} // namespace sedef::dsp::reverb
