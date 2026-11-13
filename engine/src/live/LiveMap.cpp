// SPDX-License-Identifier: AGPL-3.0-or-later
#include "sedef/live/LiveMap.h"

#include <algorithm>
#include <cmath>

namespace sedef::live
{

float continuousValue (const ParamSpec& spec, float low, float high, int value) noexcept
{
    const float t = static_cast<float> (std::clamp (value, 0, 127)) / 127.0f;
    // std::lerp is exact at both ends: a pedal at heel or toe reaches the end of its range.
    const float n = std::clamp (std::lerp (low, high, t), 0.0f, 1.0f);
    return spec.fromNormalised (n);
}

std::optional<bool> switchValue (SwitchMode mode, bool current, int value) noexcept
{
    const bool pressed = value >= kSwitchThreshold;
    if (mode == SwitchMode::latch)
        return pressed;
    if (!pressed)
        return std::nullopt; // a momentary switch's release
    return !current;
}

std::optional<SwitchMode> SwitchClassifier::first (int value, std::int64_t time) noexcept
{
    started_ = true;
    start_ = time;
    // A pedal or a knob (anything but the two ends), or a press that sends 0: only a latching
    // switch does that.
    if (value != 127)
        decided_ = SwitchMode::latch;
    return decided_;
}

std::optional<SwitchMode> SwitchClassifier::next (int value, std::int64_t time) noexcept
{
    if (decided_ || !started_)
        return decided_;
    if (time - start_ > window_)
        decided_ = SwitchMode::latch; // the window was over before this message
    else if (value < kSwitchThreshold)
        decided_ = SwitchMode::toggle; // released: a momentary switch
    return decided_;
}

std::optional<SwitchMode> SwitchClassifier::elapsed (std::int64_t now) noexcept
{
    if (!decided_ && started_ && now - start_ > window_)
        decided_ = SwitchMode::latch;
    return decided_;
}

} // namespace sedef::live
