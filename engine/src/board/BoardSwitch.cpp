// SPDX-License-Identifier: AGPL-3.0-or-later
#include "sedef/board/BoardSwitch.h"

#include <algorithm>

namespace sedef
{

void BoardSwitch::prepare (double sampleRate, int maxBlockSize, int latency)
{
    duck_.prepare (sampleRate, kDuckSeconds, latency);
    inputGains_.assign (static_cast<size_t> (std::max (1, maxBlockSize)), 1.0f);
    outputGains_.assign (inputGains_.size(), 1.0f);
}

void BoardSwitch::reset (const PedalOrder& target, std::span<PedalRack* const> racks) noexcept
{
    target_ = target;
    for (auto* rack : racks)
        rack->rewire (target_);
    leaving_.fill (false);
    duck_.reset();
}

int BoardSwitch::step (int n, std::span<PedalRack* const> racks) noexcept
{
    // The racks play the same order; the first one speaks for all.
    const PedalRack& rack = *racks.front();
    const PedalOrder& applied = rack.order();
    leaving_.fill (false);
    if (applied == target_)
    {
        duck_.fadeIn(); // after a rewire, or a change of mind on the way down
        return n;
    }
    for (const auto r : applied)
        leaving_[slotIndex (r)] = !target_.contains (r);

    if (rack.needsDuck (target_))
    {
        if (duck_.silent())
        {
            for (auto* r : racks)
                r->rewire (target_);
            leaving_.fill (false);
            duck_.fadeIn();
            return n;
        }
        duck_.fadeOut();
        return std::min (n, duck_.samplesToSilence());
    }

    // Bypass fades are enough. A duck that was under way (for an order that needed one) comes
    // back up; the leaving pedals switch off, and once none of them sounds any more, they leave
    // and the new ones join.
    duck_.fadeIn();
    for (const auto r : applied)
        if (leaving_[slotIndex (r)] && !rack.fullyBypassed (r))
            return n;
    for (auto* r : racks)
        r->adopt (target_);
    leaving_.fill (false);
    return n;
}

BoardSwitch::Gains BoardSwitch::duckGains (int n) noexcept
{
    if (!duck_.render (inputGains_.data(), outputGains_.data(), n))
        return {};
    return { inputGains_.data(), outputGains_.data() };
}

} // namespace sedef
