// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cmath>
#include <utility>

namespace sedef
{

/// Folds a stereo input to the mono source of the single instrument chain ("Sum to mono").
///
/// A guitar plugged into one input of a stereo track arrives with the other side silent, or
/// carrying only the interface's noise. Averaging (L + R) / 2 would hand the amp that guitar 6 dB
/// down, and the amps are gain-staged for the instrument's real level (docs/dsp/amps.md).
///
/// The fold therefore watches how much signal each side carries:
/// - one side 30 dB or more below the other: the live side passes at unity;
/// - otherwise (a mono source on both sides, or two real signals): the average, which leaves a
///   mono source copied to both sides at unity.
///
/// It decides only while the louder side carries signal (power envelope above -60 dBFS), so the
/// gaps between notes and the interface's noise floor never flip it. Leaving the one-sided state
/// needs the quiet side back within 20 dB (hysteresis). The gains glide linearly over 50 ms, so a
/// change never clicks. The very first decision after prepare() snaps, so the first note of a
/// session already arrives at full level. reset() keeps the decision: it describes the routing,
/// not the audio.
class InputFold
{
public:
    void prepare (double sampleRate) noexcept
    {
        const double fs = std::max (1.0, sampleRate);
        attack_ = static_cast<float> (std::exp (-1.0 / (0.005 * fs)));
        release_ = static_cast<float> (std::exp (-1.0 / (0.3 * fs)));
        step_ = static_cast<float> (1.0 / (0.05 * fs));
        state_ = State::Both;
        decided_ = false;
        gainL_ = gainR_ = 0.5f;
        reset();
    }

    void reset() noexcept
    {
        envL_ = envR_ = 0.0f;
        const auto [tl, tr] = targets();
        gainL_ = tl;
        gainR_ = tr;
    }

    void process (const float* l, const float* r, float* out, int n) noexcept
    {
        for (int i = 0; i < n; ++i)
        {
            envL_ = follow (envL_, l[i] * l[i]);
            envR_ = follow (envR_, r[i] * r[i]);
            decide();

            const auto [tl, tr] = targets();
            gainL_ = approach (gainL_, tl);
            gainR_ = approach (gainR_, tr);
            out[i] = gainL_ * l[i] + gainR_ * r[i];
        }
        if (envL_ < kTiny)
            envL_ = 0.0f;
        if (envR_ < kTiny)
            envR_ = 0.0f;
    }

    float leftGain() const noexcept { return gainL_; }
    float rightGain() const noexcept { return gainR_; }

private:
    enum class State
    {
        Both,
        LeftOnly,
        RightOnly
    };

    static constexpr float kSignalFloor = 1.0e-6f; // -60 dBFS power
    static constexpr float kEnterRatio = 1.0e-3f;  // the quiet side 30 dB down
    static constexpr float kLeaveRatio = 1.0e-2f;  // back within 20 dB
    static constexpr float kTiny = 1.0e-20f;

    float follow (float env, float power) const noexcept
    {
        const float c = power > env ? attack_ : release_;
        return power + c * (env - power);
    }

    float approach (float g, float target) const noexcept
    {
        if (g < target)
            return std::min (target, g + step_);
        return std::max (target, g - step_);
    }

    std::pair<float, float> targets() const noexcept
    {
        switch (state_)
        {
        case State::LeftOnly:
            return { 1.0f, 0.0f };
        case State::RightOnly:
            return { 0.0f, 1.0f };
        case State::Both:
            break;
        }
        return { 0.5f, 0.5f };
    }

    void decide() noexcept
    {
        const float loud = std::max (envL_, envR_);
        if (loud < kSignalFloor)
            return;

        State next = state_;
        if (envR_ < envL_ * kEnterRatio)
            next = State::LeftOnly;
        else if (envL_ < envR_ * kEnterRatio)
            next = State::RightOnly;
        else if (state_ == State::LeftOnly    ? envR_ > envL_ * kLeaveRatio
                 : state_ == State::RightOnly ? envL_ > envR_ * kLeaveRatio
                                              : false)
            next = State::Both;

        if (next == state_ && decided_)
            return;
        state_ = next;
        if (!decided_)
        {
            decided_ = true;
            const auto [tl, tr] = targets();
            gainL_ = tl;
            gainR_ = tr;
        }
    }

    float attack_ = 0.0f, release_ = 0.0f, step_ = 1.0f;
    float envL_ = 0.0f, envR_ = 0.0f;
    float gainL_ = 0.5f, gainR_ = 0.5f;
    State state_ = State::Both;
    bool decided_ = false;
};

} // namespace sedef
